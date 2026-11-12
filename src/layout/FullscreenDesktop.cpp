#include "FullscreenDesktop.hpp"
#include "MotionCurves.hpp"
#include "MotionClock.hpp"
#include "DesktopOrder.hpp"
#include "Rows.hpp"
#include "FullscreenBook.hpp"
#include "ConfigureHold.hpp"
#include "TripSizes.hpp"
#include "DesktopStrip.hpp"
#include "Config.hpp"
#include "DebugPaths.hpp"
#include "IOverview.hpp"
#include "FullscreenGuard.hpp"

#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/state/WorkspaceState.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/config/shared/actions/ConfigActions.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/managers/EventManager.hpp>
#include <hyprland/src/managers/input/UnifiedWorkspaceSwipeGesture.hpp>
#include <hyprland/src/Compositor.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <format>
#include <optional>
#include <tuple>
#include <utility>
#include <string>
#include <vector>

namespace Hyprdrive::FullscreenDesktop {
    namespace {
        // ONE MECHANIC (user, 2026-11-05: "a coherent strong mechanic that's
        // basically unbreakable and flexible"). Two things are kept, and one
        // pass (reconcile) squares them with what Hyprland actually shows,
        // after every event has fully happened:
        //   TRIP  a fullscreen app on the desktop it was given, and its home.
        //   DEBT  a place the desktops after it moved along for an app (`next`)
        //         that is owed back: once nothing of anyone else is in that
        //         place, the desktops after it come back a place.
        // And one word: a FREE PLACE is a desktop number with no desktop
        // (Hyprland drops an empty desktop: what the strip shows as an empty
        // desktop between two others is a missing number) or with an empty
        // one -- not a trip's.
        //
        // Fullscreen with others on its desktop -> the free place right after
        // it if there is one (taking over any debt there), else room is made
        // there (a new debt). Out of fullscreen on its desktop -> home, and
        // the debt is paid. Sent anywhere else -> it stays where it was sent
        // (out of fullscreen among other windows) and the debt is paid.
        // Closed -> the debt is paid. Spamming is the same rules in a row.
        struct STrip {
            PHLWINDOWREF    win;
            PHLWORKSPACEREF home;  // where it came from
            PHLWORKSPACEREF space; // the desktop it is fullscreen on
            PHLMONITORREF   mon;
            WORKSPACEID     at   = WORKSPACE_INVALID; // its place (kept up as numbers move: its desktop can go before it is done)
            WORKSPACEID     homeAt = WORKSPACE_INVALID; // its home's place, the same way (every other window there can close)
            bool            owes = false; // its place was made for it: owed back when it is done
            // Unmapped but not gone (a Wine game's window does that and maps
            // again): since when. Closed only once that outlasts REMAP_GRACE.
            std::optional<std::chrono::steady_clock::time_point> unmappedAt;
        };
        constexpr std::chrono::milliseconds REMAP_GRACE{800};
        struct SDebt {
            PHLWORKSPACEREF space; // the desktop in the owed place, while it lives
            WORKSPACEID     at = WORKSPACE_INVALID; // the owed place (kept up as numbers move)
            PHLMONITORREF   mon;
            PHLWORKSPACEREF home;  // where the app's own leftovers go
            std::string     cls;
            pid_t           pid    = 0;
            bool            beside = false; // a window opened over a fullscreen app was given this place: paid once it is empty
            // A beside debt is what an ended link owes, and the link's
            // windows -- ALL of them, the ones that joined it too -- are its
            // own (a remap's, a late one's), not a desktop of their own:
            std::vector<PHLWINDOWREF> wins;
            std::vector<pid_t>        pids;
            std::vector<std::string>  classes;
            PHLWINDOWREF              app; // ...and while that app is fullscreen, a window opened there takes the link up again (openedOn)
        };
        std::vector<STrip>   g_trips;
        std::vector<SDebt>   g_debts;
        // Places given to a window opened over a fullscreen app while they were
        // free (nothing owed): a compaction counts them into the row, and then
        // they are owed like any other (compactIfDue).
        std::vector<SDebt>   g_given;
        // BESIDE LINKS (FullscreenBook::link): the desktop windows opened over
        // a fullscreen app were given, while it is theirs. Ended, what it owes
        // is a beside debt (or, given free, a g_given place).
        struct SLink {
            PHLWINDOWREF              app;      // the fullscreen app
            PHLWORKSPACEREF           appSpace; // its desktop
            PHLWORKSPACEREF           space;    // the desktop given
            PHLMONITORREF             mon;
            WORKSPACEID               at     = WORKSPACE_INVALID; // its place (kept up as numbers move)
            bool                      owes   = false; // room was made for it
            bool                      wanted = false; // prompted, or the user went to it since
            std::vector<PHLWINDOWREF> wins;           // the windows put there (and theirs)
            std::vector<pid_t>        pids;
            std::vector<std::string>  classes;
            std::optional<std::chrono::steady_clock::time_point> emptySince;
        };
        std::vector<SLink>   g_links;
        constexpr std::chrono::milliseconds LINK_SETTLE{250}; // a Wine remap: a close, then a map
        std::vector<PHLWINDOWREF> g_settled; // fullscreen, deliberately left where it is
        std::vector<PHLWINDOWREF> g_besides; // opened over a fullscreen app and moved on: shown once mapped
        // Moved to another desktop, not on a trip, not by us (movedOntoOthers);
        // `arrived`: from an ordinary desktop -- not out of the minimize stash
        // or a scratchpad, which are not a move onto it (g_lastSpace says).
        struct SMoved {
            PHLWINDOWREF win;
            bool         arrived = true;
            // The fullscreen (`true`) or maximized windows on the desktop it
            // arrived on, as the move happened: Hyprland may take them out
            // itself before the pass (focusing the arrival under them).
            std::vector<std::pair<PHLWINDOWREF, bool>> hosts;
        };
        std::vector<SMoved> g_moved;
        // The desktop each window was last moved to (the move event names only
        // the new one).
        std::vector<std::pair<PHLWINDOWREF, PHLWORKSPACEREF>> g_lastSpace;
        std::vector<PHLWINDOWREF> g_unasked; // ...without being asked for: marked urgent once mapped
        std::vector<PHLWINDOWREF> g_asked;   // ...asked for: the view glides to it and it gets the focus once mapped
        PHLWORKSPACEREF          g_leftSpace; // the desktop the last trip ended on (justLeft)
        std::chrono::steady_clock::time_point g_leftAt;
        PHLWORKSPACEREF          g_enteredSpace; // the desktop the last trip went to (justEntered)
        std::chrono::steady_clock::time_point g_enteredAt;
        int                g_busy  = 0; // our own moves: their fullscreen changes are not the user's
        // The trip's own move of its window, while it happens (carrying()).
        struct SCarrying {
            PHLWINDOWREF win;
            bool         home = false;
        } g_carrying;
        wl_event_source*   g_timer = nullptr;
        CHyprSignalListener g_fsHook, g_closeHook, g_moveHook, g_shownHook, g_monAddHook, g_monGoneHook, g_monLayoutHook;
        const char*         g_healDue = nullptr; // why desktops sharing a number are pulled apart at the next pass
        // A DISPLAY CHANGE (compactIfDue): why, and when the last monitor event of the burst came.
        const char*                           g_compactDue = nullptr;
        std::chrono::steady_clock::time_point g_compactAt;
        constexpr std::chrono::milliseconds   COMPACT_SETTLE{700};
        void                arm(int ms); // the reconcile timer, below

        void trace(const std::string& s) {
            if (!DebugPaths::flagOn("hyprdrive-mosaic.on"))
                return;
            if (FILE* const F = DebugPaths::openLog("hyprdrive-mosaic.log")) {
                std::fprintf(F, "%lld fsdesk %s\n",
                             (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(), s.c_str());
                std::fclose(F);
            }
        }

        std::string mode() {
            return Config::getValue<std::string>("plugin:hyprdrive:general:fullscreen_desktop");
        }

        PHLWORKSPACE byId(WORKSPACEID id) {
            for (const auto& REF : State::workspaceState()->workspaces())
                if (const auto WS = REF.lock(); DesktopOrder::ordinary(WS) && WS->m_id == id)
                    return WS;
            return nullptr;
        }

        int othersOn(const PHLWORKSPACE& ws, const PHLWINDOW& except) {
            int n = 0;
            for (const auto& W : Desktop::windowState()->windows())
                if (W && W != except && W->m_isMapped && !W->isHidden() && W->m_workspace == ws)
                    ++n;
            return n;
        }

        bool fullscreen(const PHLWINDOW& w) {
            return w && Fullscreen::controller()->isFullscreen(w, Fullscreen::FSMODE_FULLSCREEN);
        }

        STrip* tripOf(const PHLWINDOW& w) {
            for (auto& T : g_trips)
                if (T.win.lock() == w)
                    return &T;
            return nullptr;
        }

        // ONE SIZE EACH WAY (TripSizes.hpp): between a fullscreen change and
        // the trip's move it calls for, the window's sizes wait; the pass that
        // moves it (reconcile) releases them, once.
        bool sizesWait(const PHLWINDOW& w) {
            if (!w)
                return false;
            const STrip* T  = tripOf(w);
            const auto   WS = w->m_workspace;
            const bool   WAIT = TripSizes::wait({
                  .mapped      = w->m_isMapped,
                  .carrying    = g_carrying.win.lock() == w,
                  .onTrip      = T != nullptr,
                  .onTripSpace = T && WS && T->space.lock() == WS,
                  .fullscreen  = fullscreen(w),
                  .ordinary    = WS && DesktopOrder::ordinary(WS) && WS->m_monitor.lock(),
                  .others      = WS ? othersOn(WS, w) : 0,
                  .settled     = std::ranges::any_of(g_settled, [&](const PHLWINDOWREF& r) { return r.lock() == w; }),
            });
            // The pass that ends the wait is due (it is, after every fullscreen
            // change; this keeps it so whatever held the event back).
            if (WAIT && !g_carrying.win)
                arm(1);
            return WAIT;
        }

        // Show `ws` on `mon`: the strip's slide (reversible where it is), else Hyprland's.
        void show(const PHLMONITOR& mon, const PHLWORKSPACE& ws) {
            if (!mon || !ws || mon->m_activeWorkspace == ws)
                return;
            // The overview draws the desktops as cards and animates the change
            // itself (its insert transition): the strip would only slide what
            // is hidden under it.
            if (openScrollOverviewForMonitor(mon)) {
                (void)::Config::Actions::changeWorkspace(std::to_string(ws->m_id));
                return;
            }
            if (!DesktopStrip::glideTo(mon, sc<int>(ws->m_id)))
                (void)::Config::Actions::changeWorkspace(std::to_string(ws->m_id));
        }

        // Move `w` to `ws` without following it there, fullscreen kept.
        void carry(const PHLWINDOW& w, const PHLWORKSPACE& ws, bool keepFullscreen) {
            ++g_busy;
            (void)::Config::Actions::moveToWorkspace(ws, true, w);
            // Hyprland fades a window out of a desktop it leaves on screen, and
            // into one that is on screen; this one travels instead (travel()),
            // seen all the way.
            w->alpha(Desktop::View::WINDOW_ALPHA_MOVE_TO_WORKSPACE)->setValueAndWarp(1.F);
            w->alpha(Desktop::View::WINDOW_ALPHA_MOVE_FROM_WORKSPACE)->setValueAndWarp(1.F);
            w->m_monitorMovedFrom = -1;
            if (keepFullscreen && !fullscreen(w))
                Fullscreen::controller()->setFullscreenMode(w, Fullscreen::FSMODE_FULLSCREEN, std::nullopt);
            --g_busy;
        }

        // ...and it travels IN STEP WITH THE SLIDE: every frame (preRender) it
        // is where the slide has got to between where it was seen and its new
        // place -- arriving as the desktops land, turning back with them when
        // the slide does. (Its own animation alone outran the slide by half a
        // screen in two frames; the layout also settles a moved window straight
        // onto its place a moment after the move.)
        // The windows left behind travel too: the ones on the desktop the app
        // leaves widen into its room as that desktop slides out, and shrink
        // back as it slides in -- in step, so a desktop is never seen without
        // them (their own animation lagged the slide by a dozen frames).
        // `gauge` is the desktop being slid TO: how far it has come is how far
        // every one of them has.
        struct STravel {
            PHLWINDOWREF                          win;
            PHLWORKSPACEREF                       gauge;
            bool                                  screen = true; // crossing desktops: travels on screen; else within its desktop
            Vector2D                              seen, seenSize;
            double                                span = 0; // how far the gauge had to slide when it set off
            std::chrono::steady_clock::time_point at;
            // Where it was last DRAWN, on screen. A trip that turns back
            // mid-way sets off from here: the window's animation values are
            // not it -- Hyprland's own tick moves them on between frames, and
            // spammed, every turn-back started a few hundred px off and the
            // window jumped (user, 2026-11-06: "spamming super+f ... snaps").
            std::optional<std::pair<Vector2D, Vector2D>> drawn;
            // The goal it is heading for, and how far along it was when that
            // goal was taken: the layout can re-place a window mid-trip (a
            // fullscreen settling a frame late), and lerping the rest of the way
            // to a moved goal at the same progress jumped it by the difference.
            std::optional<std::pair<Vector2D, Vector2D>> goal;
            double                                       t0 = 0, lastT = 0;
        };
        std::vector<STravel> g_travels;
        constexpr double     TRAVEL_MIN_MS = 220.0;

        // THE WINDOW STAYS WHERE IT IS SEEN. Moved to another desktop, a window
        // would vanish from its place and turn up on a desktop still sliding in
        // -- an empty desktop leaving, a hole where it was. Instead it sets off
        // from exactly where it was on screen and travels to its new place
        // while the desktops slide under it: into fullscreen as its desktop
        // comes in, back into its slot as its old desktop does.
        // `seen` / `seenSize`: where it was drawn (its place plus its desktop's
        // slide) just before the move.
        Vector2D offsetOf(const PHLWORKSPACE& ws) {
            return ws && ws->m_renderOffset ? ws->m_renderOffset->value() : Vector2D{};
        }
        void travel(const PHLWINDOW& w, const Vector2D& seen, const Vector2D& seenSize, const PHLWORKSPACE& gauge, bool screen = true) {
            if (!w || !w->m_workspace)
                return;
            // Not under the overview: it draws its tiles from where the windows
            // are, and animates the change itself.
            if (openScrollOverviewForMonitor(w->m_workspace->m_monitor.lock()))
                return;
            const Vector2D OFF  = offsetOf(w->m_workspace);
            const Vector2D GO   = offsetOf(gauge);
            const Vector2D GOAL = w->positionAnimation()->goal(), GSIZE = w->sizeAnimation()->goal();
            w->positionAnimation()->setValue(screen ? seen - OFF : seen);
            w->sizeAnimation()->setValue(seenSize);
            (void)GOAL;
            (void)GSIZE;
            std::erase_if(g_travels, [&](const STravel& t) { return t.win.lock() == w; });
            g_travels.push_back({w, gauge, screen, seen, seenSize, std::hypot(GO.x, GO.y), std::chrono::steady_clock::now()});
        }
        // Where the other windows of `ws` are drawn within it, before the move.
        const STravel* travelOf(const PHLWINDOW& w) {
            for (const auto& T : g_travels)
                if (T.win.lock() == w && T.drawn)
                    return &T;
            return nullptr;
        }
        std::vector<std::tuple<PHLWINDOWREF, Vector2D, Vector2D>> others(const PHLWORKSPACE& ws, const PHLWINDOW& except) {
            std::vector<std::tuple<PHLWINDOWREF, Vector2D, Vector2D>> out;
            for (const auto& W : Desktop::windowState()->windows())
                if (W && W != except && W->m_isMapped && !W->isHidden() && W->m_workspace == ws) {
                    if (const auto* T = travelOf(W))
                        out.emplace_back(W, T->drawn->first - offsetOf(ws), T->drawn->second);
                    else
                        out.emplace_back(W, W->positionAnimation()->value(), W->sizeAnimation()->value());
                }
            return out;
        }
        void travelOthers(const std::vector<std::tuple<PHLWINDOWREF, Vector2D, Vector2D>>& was, const PHLWORKSPACE& gauge) {
            for (const auto& [REF, POS, SIZE] : was)
                if (const auto W = REF.lock(); W && W->m_isMapped)
                    if (W->positionAnimation()->goal() != POS || W->sizeAnimation()->goal() != SIZE)
                        travel(W, POS, SIZE, gauge, false);
        }
        std::pair<Vector2D, Vector2D> seenAt(const PHLWINDOW& w) {
            if (const auto* T = travelOf(w))
                return *T->drawn;
            const Vector2D OFF = w->m_workspace && w->m_workspace->m_renderOffset ? w->m_workspace->m_renderOffset->value() : Vector2D{};
            return {w->positionAnimation()->value() + OFF, w->sizeAnimation()->value()};
        }

        // THE PLACES ARE KEPT BY NUMBER (FullscreenBook.hpp). A place with a
        // desktop in it follows that desktop through every renumber
        // (syncPlaces, DesktopOrder's after-renumber hook -- the overview's
        // drag included); a place whose desktop has gone moves with our own
        // shifts (moved).
        WORKSPACEID placeOf(const PHLWORKSPACEREF& ref, WORKSPACEID kept) {
            const auto S = ref.lock();
            return FullscreenBook::placeOf(S && DesktopOrder::ordinary(S), S ? S->m_id : WORKSPACE_INVALID, kept);
        }
        template <class T>
        WORKSPACEID placeOf(const T& t) {
            return placeOf(t.space, t.at);
        }
        void syncPlaces() {
            for (auto& T : g_trips) {
                T.at     = placeOf(T);
                T.homeAt = placeOf(T.home, T.homeAt);
            }
            for (auto& D : g_debts)
                D.at = placeOf(D);
            for (auto& L : g_links)
                L.at = placeOf(L);
        }
        // (Along `row`: kept numbers in other rows stay.)
        void moved(const Row& row, WORKSPACEID from, int delta) {
            for (auto& T : g_trips) {
                if (!T.space.lock())
                    T.at = FullscreenBook::movedPlace(row, T.at, from, delta);
                if (!T.home.lock())
                    T.homeAt = FullscreenBook::movedPlace(row, T.homeAt, from, delta);
            }
            for (auto& D : g_debts)
                if (!D.space.lock())
                    D.at = FullscreenBook::movedPlace(row, D.at, from, delta);
            for (auto& L : g_links)
                if (!L.space.lock())
                    L.at = FullscreenBook::movedPlace(row, L.at, from, delta);
        }
        bool isTripSpace(const PHLWORKSPACE& ws) {
            return std::ranges::any_of(g_trips, [&](const STrip& t) { return t.space.lock() == ws; });
        }
        // A free place: `id` has no desktop, or an empty one on `mon` that no trip holds.
        // Never a fullscreen app's home, even emptied meanwhile (still on
        // screen, or gone and its number kept): the app goes back there
        // (goHome), and another fullscreen app put there shared it.
        bool freePlace(WORKSPACEID id, const PHLMONITOR& mon) {
            if (std::ranges::any_of(g_trips, [&](const STrip& t) { return placeOf(t.home, t.homeAt) == id; }))
                return false;
            if (std::ranges::any_of(g_links, [&](const SLink& l) { return placeOf(l) == id; }))
                return false;
            const auto E = byId(id);
            return !E || (E->m_monitor.lock() == mon && othersOn(E, nullptr) == 0 && !isTripSpace(E));
        }

        void startTrip(const PHLWINDOW& w) {
            const auto HOME = w->m_workspace;
            const auto MON  = HOME->m_monitor.lock();
            const auto MODE = mode();
            // The place after home along the monitor's row: a block's last
            // desktop is followed by its overflow, never by the next block.
            const Row  ROW  = Rows::of(MON);
            const auto PLAN = FullscreenBook::start(MODE, ROW, HOME->m_id, othersOn(HOME, w), [&](WORKSPACEID id) { return freePlace(id, MON); });
            // (A home outside the row -- a visitor -- has no place after it:
            // it stays where it is, and nothing is ever taken outside a row.)
            if (PLAN.settle || !MON || !ROW.contains(PLAN.at)) {
                g_settled.emplace_back(w); // alone on its desktop: that is its own already
                return;
            }
            const WORKSPACEID at   = PLAN.at;
            bool              owes = false;
            if (PLAN.makeRoom) {
                // No room after it: the desktops from there on move along a place.
                DesktopStrip::finishNow(); // (the strip lands first: its row is numbers)
                if (!DesktopOrder::shift(ROW, at, +1)) {
                    // Refused (DesktopOrder logged why; only a real collision
                    // now -- another monitor's desktop numbered in this row):
                    // no room is made, so it stays where it is, as Hyprland
                    // has it.
                    g_settled.emplace_back(w);
                    return;
                }
                moved(ROW, at, +1);
                owes = true;
            }
            // A debt in that place is this trip's now (spamming: the place it
            // left a moment ago, not paid back yet).
            if (MODE != "free")
                if (const auto IT = std::ranges::find_if(g_debts, [&](const SDebt& d) { return placeOf(d) == at; }); IT != g_debts.end()) {
                    owes = true;
                    g_debts.erase(IT);
                }
            auto space = byId(at);
            if (!space)
                space = State::workspaceState()->create(at, MON->m_id);
            if (!space) {
                // No desktop for it after all: it stays where it is, and room
                // made for it is owed back at once (it was a silent +1).
                if (owes)
                    g_debts.push_back({.at = at, .mon = MON, .home = HOME, .cls = w->m_class, .pid = w->getPID()});
                g_settled.emplace_back(w);
                return;
            }
            g_trips.push_back({.win = w, .home = HOME, .space = space, .mon = MON, .at = space->m_id, .homeAt = HOME->m_id, .owes = owes});
            g_enteredSpace = space;
            g_enteredAt    = std::chrono::steady_clock::now();
            trace(std::format("{} fullscreen on {}: to {}{}", w->m_class, HOME->m_id, at, owes ? " (owed back)" : ""));
            const auto [SEEN, SEENSIZE] = seenAt(w);
            const auto WAS              = others(HOME, w);
            // Shown first, then moved in: a window never sits on a desktop that
            // is not on screen -- Hyprland suspends windows there, and a game
            // (Wine) stops drawing: it arrived blank for a frame or two.
            show(MON, space);
            g_carrying = {.win = w, .home = false};
            carry(w, space, true);
            g_carrying = {};
            travel(w, SEEN, SEENSIZE, space);
            travelOthers(WAS, space);
            Desktop::focusState()->fullWindowFocus(w, Desktop::FOCUS_REASON_OTHER);
        }

        // A trip is over: what it owes becomes a debt.
        void endTrip(const STrip& t, const PHLWINDOW& w) {
            if (!t.owes)
                return;
            // Its place by number: the desktop is gone already when the app
            // was closed while it was not on screen (the old "paying back
            // place -1").
            const auto S = t.space.lock();
            g_debts.push_back({.space = S, .at = placeOf(t), .mon = t.mon, .home = t.home,
                               .cls = w ? w->m_class : std::string{}, .pid = w ? w->getPID() : 0});
        }

        // Where the view goes when a fullscreen app closes on its own desktop:
        // its home; HOME GONE MEANWHILE, the desktop now in home's place, else
        // the populated desktop nearest that place on the same monitor. Not a
        // new empty desktop: leaving fullscreen re-makes the home for the
        // window to land on, but a closed app has nothing to land.
        PHLWORKSPACE backAfterClose(const PHLWORKSPACE& home, WORKSPACEID homeAt, const PHLMONITOR& mon, const PHLWORKSPACE& space) {
            if (home && home != space && home->m_monitor.lock() == mon)
                return home;
            if (const auto E = byId(homeAt); E && E != space && E->m_monitor.lock() == mon)
                return E;
            PHLWORKSPACE best;
            for (const auto& REF : State::workspaceState()->workspaces())
                if (const auto WS = REF.lock(); DesktopOrder::ordinary(WS) && WS != space && WS->m_monitor.lock() == mon && othersOn(WS, nullptr) > 0 &&
                    (!best || std::abs(WS->m_id - homeAt) < std::abs(best->m_id - homeAt)))
                    best = WS;
            return best;
        }

        // ALWAYS OPEN IN EMPTY WORKSPACE + FULLSCREEN (the dock's chips): the
        // app opens on an empty desktop and goes fullscreen there, alone -- no
        // trip (g_settled). Closed while that desktop is shown, the view goes
        // back to the desktop it was launched from (launchedFrom), as a trip's
        // close goes home. Out of fullscreen first, it is just a window there.
        struct SOrigin {
            PHLWINDOWREF    win;
            PHLWORKSPACEREF origin, space;
            WORKSPACEID     originAt = WORKSPACE_INVALID;
            PHLMONITORREF   mon;
            bool            wasFullscreen = false;
            std::optional<std::chrono::steady_clock::time_point> unmappedAt;
        };
        std::vector<SOrigin> g_origins;
        void origins() {
            const auto NOW = std::chrono::steady_clock::now();
            for (auto it = g_origins.begin(); it != g_origins.end();) {
                auto&      O = *it;
                const auto W = O.win.lock();
                if (W && W->m_isMapped) {
                    O.unmappedAt.reset();
                    if (W->m_workspace && DesktopOrder::ordinary(W->m_workspace))
                        O.space = W->m_workspace;
                    O.wasFullscreen |= fullscreen(W);
                    // Out of fullscreen (after it was), or on a trip: not ours.
                    if ((O.wasFullscreen && !fullscreen(W)) || tripOf(W)) {
                        it = g_origins.erase(it);
                        continue;
                    }
                    ++it;
                    continue;
                }
                if (W && !W->m_isMapped) {
                    if (!O.unmappedAt)
                        O.unmappedAt = NOW;
                    if (NOW - *O.unmappedAt < REMAP_GRACE) {
                        ++it;
                        continue;
                    }
                }
                const auto SPACE = O.space.lock();
                const auto MON   = O.mon.lock();
                if (O.wasFullscreen && MON && SPACE && MON->m_activeWorkspace == SPACE && othersOn(SPACE, W) == 0) {
                    trace(std::format("fullscreen app closed on the desktop it opened on: back to {}", O.originAt));
                    if (const auto BACK = backAfterClose(O.origin.lock(), O.originAt, MON, SPACE))
                        show(MON, BACK);
                }
                it = g_origins.erase(it);
            }
        }

        // Out of fullscreen on its desktop: home, travelling there.
        void goHome(const STrip& t, const PHLWINDOW& w) {
            const auto SPACE = t.space.lock();
            const auto MON   = t.mon.lock();
            auto       home  = t.home.lock();
            if (!home) {
                // ITS HOME WENT MEANWHILE (every other window there closed): a
                // desktop again in its home's place if that place is free. Not
                // "the desktop before": that could be another fullscreen app's
                // own desktop, and the two shared it. Else it stays where it
                // is, out of fullscreen: that desktop is its own now.
                const auto HMON = MON ? MON : (SPACE ? SPACE->m_monitor.lock() : nullptr);
                if (t.homeAt >= 1 && HMON) {
                    if (const auto E = byId(t.homeAt); !E)
                        home = State::workspaceState()->create(t.homeAt, HMON->m_id);
                    else if (othersOn(E, nullptr) == 0 && !isTripSpace(E))
                        home = E;
                }
                if (!home) {
                    trace(std::format("{} out of fullscreen: its home went and place {} is taken: it stays", w->m_class, t.homeAt));
                    return;
                }
                // The others that came from that home come back to this one.
                for (auto& T : g_trips)
                    if (!T.home.lock() && T.homeAt == t.homeAt)
                        T.home = home;
            }
            trace(std::format("{} out of fullscreen: home to {}", w->m_class, home->m_id));
            const auto SEEN = seenAt(w);
            const auto WAS  = others(home, w);
            if (MON && MON->m_activeWorkspace == SPACE && home->m_monitor.lock() == MON)
                show(MON, home);
            g_carrying = {.win = w, .home = true};
            carry(w, home, false);
            g_carrying = {};
            if (w->m_workspace == home)
                travel(w, SEEN.first, SEEN.second, home);
            travelOthers(WAS, home);
            if (MON && MON->m_activeWorkspace == home)
                Desktop::focusState()->fullWindowFocus(w, Desktop::FOCUS_REASON_OTHER);
        }

        bool anyDriving() {
            for (const auto& M : State::monitorState()->monitors())
                if (M && DesktopStrip::driving(M))
                    return true;
            return false;
        }
        bool onScreen(const PHLWORKSPACE& ws) {
            for (const auto& M : State::monitorState()->monitors())
                if (M && M->m_activeWorkspace == ws)
                    return true;
            return false;
        }

        // A DEBT IS PAID BACK BY NUMBER (FullscreenBook::decide), wherever its
        // desktop lives now: a debt's monitor gone, or its place moved to
        // another monitor, no longer drops it (each was a desktop owed and
        // never given back). A trip's place is paid once it holds nothing of
        // anyone else's -- the app's own leftovers (a game's small window) go
        // home first; a window of another app put there meanwhile makes it a
        // desktop of its own: nothing owed. A beside place is paid once its
        // desktop is empty. Never while the place is on screen or a strip is
        // sliding.
        enum class EPaid { Done, WaitScreen, WaitWindows };
        EPaid pay(const SDebt& d) {
            using FullscreenBook::EPay;
            const WORKSPACEID AT = placeOf(d);
            const auto        E  = AT >= 1 ? byId(AT) : nullptr;
            FullscreenBook::SFacts facts{.at = AT, .exists = !!E, .driving = anyDriving()};
            bool own = false;
            if (E) {
                const auto OWN = [&](const PHLWINDOW& W) {
                    return (d.pid > 0 && W->getPID() == d.pid) || (!d.cls.empty() && W->m_class == d.cls) ||
                        std::ranges::any_of(d.wins, [&](const PHLWINDOWREF& r) { return r.lock() == W; }) ||
                        (W->getPID() > 0 && std::ranges::find(d.pids, W->getPID()) != d.pids.end()) ||
                        (!W->m_class.empty() && std::ranges::find(d.classes, W->m_class) != d.classes.end());
                };
                for (const auto& W : Desktop::windowState()->windows())
                    if (W && W->m_isMapped && W->m_workspace == E) {
                        facts.any = true;
                        (OWN(W) ? own : facts.foreign) = true;
                    }
                facts.trip     = isTripSpace(E);
                facts.onScreen = onScreen(E);
                facts.homeGone = !d.home.lock();
            }
            switch (FullscreenBook::decide(d.beside ? FullscreenBook::EKind::Beside : FullscreenBook::EKind::Trip, facts)) {
                case EPay::Refuse:
                    // No place (it can no longer arise; never pay at one).
                    trace(std::format("a debt with no place ({}): dropped, nothing moves", AT));
                    return EPaid::Done;
                case EPay::NothingOwed:
                    trace(std::format("place {} is a desktop of its own now: nothing owed", AT));
                    return EPaid::Done;
                case EPay::ToTrip:
                    for (auto& T : g_trips)
                        if (T.space.lock() == E)
                            T.owes = true;
                    trace(std::format("place {} is a fullscreen app's desktop now: its trip owes it", AT));
                    return EPaid::Done;
                case EPay::WaitScreen: return EPaid::WaitScreen;
                case EPay::WaitWindows: return EPaid::WaitWindows;
                case EPay::Pay: break;
            }
            trace(std::format("paying back place {}{}", AT, d.beside ? " (beside)" : ""));
            if (E) {
                if (own)
                    if (const auto HOME = d.home.lock(); HOME && HOME != E)
                        for (const auto& W : Desktop::windowState()->windows())
                            if (W && W->m_isMapped && W->m_workspace == E)
                                carry(W, HOME, false);
                // The empty desktop in the place goes to the end of its row
                // (Hyprland drops it once nothing holds it); the row's ones
                // after it come back a place (other rows are not touched).
                if (!DesktopOrder::payBack(Rows::ofWorkspace(E), AT, E))
                    return EPaid::Done; // refused (logged): never half-paid
                const Row R = Rows::ofWorkspace(E);
                if (const WORKSPACEID NEXT = FullscreenBook::nextPlace(R, AT); NEXT != FullscreenBook::NO_PLACE)
                    moved(R, NEXT, -1);
                return EPaid::Done;
            }
            const Row         R    = Rows::containing(AT);
            const WORKSPACEID NEXT = FullscreenBook::nextPlace(R, AT);
            if (NEXT == FullscreenBook::NO_PLACE || !DesktopOrder::shift(R, NEXT, -1))
                return EPaid::Done; // refused (logged): never half-paid
            moved(R, NEXT, -1);
            return EPaid::Done;
        }

        // A WINDOW OPENED OVER A FULLSCREEN APP (user, 2026-11-08: "if I'm in a
        // fullscreen app and I open a window from that workspace, it opens that
        // window in fullscreen but then exits the app from fullscreen"). The
        // app's desktop is its own; the new window gets the desktop right after
        // it -- the next one if that is empty, else a new one there (the ones
        // after move along a place) -- not fullscreen, and the app stays as it
        // is. Run at window.openEarly: the window has its desktop but is not
        // laid out yet, and Hyprland's on_focus_under_fullscreen takeover comes
        // after, so it never sees a fullscreen window to take over. A window
        // the user asked for is shown there; an unasked one (Intent) is not.
        // Floating windows, dialogs and fullscreen_desktop = stay keep
        // Hyprland's own behaviour.
        // The fullscreen app on `ws` that this book keeps (a trip's, or one
        // settled alone on its desktop), if any.
        PHLWINDOW appOn(const PHLWORKSPACE& ws) {
            if (!ws || !DesktopOrder::ordinary(ws))
                return nullptr;
            const auto APP = Fullscreen::controller()->getFullscreenWindow(ws);
            if (!APP || !fullscreen(APP))
                return nullptr;
            const bool OURS = tripOf(APP) || std::ranges::any_of(g_settled, [&](const PHLWINDOWREF& r) { return r.lock() == APP; });
            return OURS ? APP : nullptr;
        }

        bool member(const SLink& l, const PHLWINDOW& w) {
            return std::ranges::any_of(l.wins, [&](const PHLWINDOWREF& r) { return r.lock() == w; }) ||
                (w->getPID() > 0 && std::ranges::find(l.pids, w->getPID()) != l.pids.end()) ||
                (!w->m_class.empty() && std::ranges::find(l.classes, w->m_class) != l.classes.end());
        }
        void join(SLink& l, const PHLWINDOW& w) {
            l.wins.emplace_back(w);
            if (w->getPID() > 0 && std::ranges::find(l.pids, w->getPID()) == l.pids.end())
                l.pids.push_back(w->getPID());
            if (!w->m_class.empty() && std::ranges::find(l.classes, w->m_class) == l.classes.end())
                l.classes.push_back(w->m_class);
        }

        // Any window -- tiled or floating, a dialog or not (Placement decided
        // it goes here; owner 2026-11-11: "if a floating window shows up over a
        // fullscreen one, it should be moved to the next desktop (new one if
        // next is not empty)"). In the app's monitor's ROW: a block's 10th
        // desktop is followed by its overflow, never by another monitor's block.
        // A second one of the same app JOINS the first one's desktop.
        PHLWORKSPACE besideFullscreen(const PHLWINDOW& w, bool prompted) {
            if (!w)
                return nullptr;
            const auto WS = w->m_workspace;
            if (!WS || !DesktopOrder::ordinary(WS) || mode() == "stay")
                return nullptr;
            const auto APP = appOn(WS);
            if (!APP || APP == w)
                return nullptr;
            const auto MON = WS->m_monitor.lock();
            if (!MON)
                return nullptr;
            PHLWORKSPACE space;
            SLink*       link = nullptr;
            for (auto& L : g_links)
                if (L.app.lock() == APP && L.appSpace.lock() == WS)
                    if (const auto S = L.space.lock(); S && DesktopOrder::ordinary(S) && S->m_monitor.lock() == MON) {
                        space = S;
                        link  = &L;
                        break;
                    }
            if (!space) {
                // ...and the room made for it is OWED BACK like a fullscreen
                // app's. Paid once the link is over and that desktop is empty.
                const Row         ROW  = Rows::of(MON);
                const auto        PLAN = FullscreenBook::beside(ROW, WS->m_id, [&](WORKSPACEID id) { return freePlace(id, MON); });
                const WORKSPACEID at   = PLAN.at;
                bool              owes = false;
                if (PLAN.settle || !ROW.contains(at))
                    return nullptr;
                if (PLAN.makeRoom) {
                    DesktopStrip::finishNow();
                    if (!DesktopOrder::shift(ROW, at, +1))
                        return nullptr; // refused (logged): Hyprland's own behaviour
                    moved(ROW, at, +1);
                    owes = true;
                }
                // A debt in that place (a trip that just ended there) is this
                // link's now: its desktop takes the place.
                if (const auto IT = std::ranges::find_if(g_debts, [&](const SDebt& d) { return placeOf(d) == at; }); IT != g_debts.end()) {
                    owes = true;
                    g_debts.erase(IT);
                }
                space = byId(at);
                if (!space)
                    space = State::workspaceState()->create(at, MON->m_id);
                if (!space || space == WS) {
                    if (owes)
                        g_debts.push_back({.at = at, .mon = MON, .beside = true});
                    arm(1);
                    return nullptr;
                }
                std::erase_if(g_given, [&](const SDebt& g) { return g.space.lock() == space; });
                g_links.push_back({.app = APP, .appSpace = WS, .space = space, .mon = MON, .at = space->m_id, .owes = owes});
                link = &g_links.back();
            }
            join(*link, w);
            link->wanted |= prompted;
            trace(std::format("{} opened over fullscreen {} on {}: to {}{}{}", w->m_class, APP->m_class, WS->m_id, space->m_id, link->owes ? " (owed back)" : "",
                              prompted ? "" : " (unasked: not shown)"));
            w->m_workspace = space;
            w->m_monitor   = MON;
            if (prompted) {
                // ...ONCE THE MAP IS OVER (the film, 2026-11-11): switched to
                // in the middle of Hyprland's map -- the view and the focus
                // moved before the window had its place in the layout -- an
                // X11 dialog was left at -10,-10, 21x21: invisible on its
                // desktop. Hyprland's own focus would switch at once (no
                // glide); showBesides glides there and focuses it.
                w->m_noInitialFocus = true;
                g_asked.emplace_back(w);
            } else {
                // UNASKED (owner: "not be focused"): it goes quietly, the view
                // and the pointer stay on the app; the dock and the strip say
                // it is there (urgent, once it is mapped: showBesides).
                w->m_noInitialFocus = true;
                g_unasked.emplace_back(w);
            }
            // SHOWN ON ITS DESKTOP (user, 2026-11-09: "it opens it in the next
            // desktop but it's not visible until i switch away then switch
            // back, OR I go to overview"). The rest of Hyprland's map still
            // judges the window by the desktop it was opened on, which has the
            // fullscreen app: it marks it not allowed over fullscreen and warps
            // its fullscreen alpha to 0 -- invisible on a desktop with no
            // fullscreen app, until a switch recomputes it. Undone once the map
            // is over (reconcile, on the event loop).
            g_besides.emplace_back(w);
            arm(1);
            return space;
        }

        // The window besideFullscreen moved, once Hyprland's map is done with
        // it: drawn and clickable like any window on a desktop with no
        // fullscreen app.
        void showBesides() {
            for (const auto& REF : g_besides) {
                const auto W = REF.lock();
                if (!W || !W->m_isMapped || !W->m_workspace || Fullscreen::controller()->hasFullscreen(W->m_workspace))
                    continue;
                W->m_allowedOverFullscreen = true;
                W->updateFullscreenInputState();
                W->alpha(Desktop::View::WINDOW_ALPHA_FULLSCREEN)->setValueAndWarp(1.F);
                trace(std::format("{} shown on {}", W->m_class, W->m_workspace->m_id));
            }
            g_besides.clear();
            for (const auto& REF : std::exchange(g_asked, {}))
                if (const auto W = REF.lock(); W && W->m_isMapped && W->m_workspace && DesktopOrder::ordinary(W->m_workspace))
                    if (const auto MON = W->m_workspace->m_monitor.lock()) {
                        show(MON, W->m_workspace);
                        Desktop::focusState()->fullWindowFocus(W, Desktop::FOCUS_REASON_OTHER);
                    }
            for (const auto& REF : std::exchange(g_unasked, {}))
                if (const auto W = REF.lock(); W && W->m_isMapped && !W->m_isUrgent && Desktop::focusState()->window() != W) {
                    W->m_isUrgent = true;
                    g_pEventManager->postEvent(SHyprIPCEvent{.event = "urgent", .data = std::format("{:x}", rc<uintptr_t>(W.get()))});
                    Event::bus()->m_events.window.urgent.emit(W);
                }
        }

        // NO EMPTY DESKTOPS AFTER A DISPLAY CHANGE (user, 2026-11-11: "every
        // time the screen display changes it gives me a bunch of empty
        // desktops" ... "just make it remove all empty desktops when changing
        // monitor config"). A monitor came, went, or changed its mode, scale,
        // position or transform: once the burst has settled (COMPACT_SETTLE
        // after its last event), the desktops holding windows become 1..N in
        // their order, the shown empty ones after them (DesktopOrder::compact).
        // Never while a strip slides, a swipe is under way, or an overview is
        // up (its cards fill the holes with desktops of its own while it is,
        // and a drag on its strip moves numbers): looked at again shortly.
        // Only here -- and on the overview strip's Clean Up button, for its
        // monitor's row alone (compactRow), with the overview up on purpose.
        //
        // The book follows (FullscreenBook::compactedPlace): a debt whose place
        // the compaction closed is settled by it and dropped; one whose place
        // holds a desktop of the row stays owed there; every trip whose desktop
        // is in the row owes its place from now on, and so does a window opened
        // over a fullscreen app on a place that was free.
        bool compactBlocked() {
            if (anyDriving() || DesktopStrip::switching())
                return true;
            if (g_pUnifiedWorkspaceSwipe && g_pUnifiedWorkspaceSwipe->isGestureInProgress())
                return true;
            return !scrollOverviews().empty(); // open or still closing
        }
        // The compaction itself and the book after it; `only`: one row (the
        // strip's Clean Up button), whose places alone are looked at -- a
        // trip, debt or given window in another row keeps what it had.
        bool compactNow(const std::string& WHY, const Row* only) {
            const auto inScope = [only](WORKSPACEID at) { return !only || only->contains(at); };
            (void)DesktopOrder::heal("before removing empty desktops");
            std::erase_if(g_given, [](const SDebt& g) { return !DesktopOrder::ordinary(g.space.lock()); });
            // Every place by its number before.
            syncPlaces();
            std::vector<WORKSPACEID> debtWas, tripWas, homeWas, givenWas;
            for (const auto& D : g_debts)
                debtWas.push_back(placeOf(D));
            for (const auto& T : g_trips) {
                tripWas.push_back(placeOf(T));
                homeWas.push_back(placeOf(T.home, T.homeAt));
            }
            for (const auto& G : g_given)
                givenWas.push_back(placeOf(G));
            std::vector<WORKSPACEID> linkWas;
            for (const auto& L : g_links)
                linkWas.push_back(placeOf(L));
            const auto RENUMBERED = DesktopOrder::compact(WHY, only);
            if (!RENUMBERED) {
                trace(std::format("{}: no empty desktop between (or refused, see the desktops log)", WHY));
                return false;
            }
            std::vector<std::pair<FullscreenBook::Id, FullscreenBook::Id>> ROW;
            for (const auto& [FROM, TO] : *RENUMBERED)
                ROW.emplace_back(FROM, TO);
            trace(std::format("{}: empty desktops removed", WHY));
            std::vector<SDebt> kept;
            for (size_t i = 0; i < g_debts.size() && i < debtWas.size(); ++i) {
                if (!inScope(debtWas[i])) {
                    kept.push_back(g_debts[i]);
                    continue;
                }
                const WORKSPACEID NOW = FullscreenBook::compactedPlace(debtWas[i], ROW);
                if (NOW == FullscreenBook::NO_PLACE) {
                    trace(std::format("debt at place {}: closed by the compaction, settled", debtWas[i]));
                    continue;
                }
                kept.push_back(g_debts[i]);
                kept.back().at = NOW;
            }
            g_debts = std::move(kept);
            for (size_t i = 0; i < g_trips.size() && i < tripWas.size(); ++i) {
                auto& T = g_trips[i];
                if (!inScope(tripWas[i]))
                    continue;
                const WORKSPACEID NOW = FullscreenBook::compactedPlace(tripWas[i], ROW);
                if (NOW != FullscreenBook::NO_PLACE && !T.owes)
                    trace(std::format("trip at place {} (now {}): owed back from now on", tripWas[i], NOW));
                T.owes = NOW != FullscreenBook::NO_PLACE;
                T.at   = NOW;
                if (!T.home.lock())
                    T.homeAt = FullscreenBook::compactedPlace(homeWas[i], ROW);
            }
            std::vector<SDebt> notHere; // given in another row: still to be counted by that row's compaction
            for (size_t i = 0; i < g_given.size() && i < givenWas.size(); ++i) {
                if (!inScope(givenWas[i])) {
                    notHere.push_back(g_given[i]);
                    continue;
                }
                const WORKSPACEID NOW = FullscreenBook::compactedPlace(givenWas[i], ROW);
                if (NOW == FullscreenBook::NO_PLACE || std::ranges::any_of(g_debts, [&](const SDebt& d) { return d.at == NOW; }))
                    continue;
                g_debts.push_back(g_given[i]);
                g_debts.back().at = NOW;
                trace(std::format("beside place {} (now {}): owed back from now on", givenWas[i], NOW));
            }
            g_given = std::move(notHere);
            for (size_t i = 0; i < g_links.size() && i < linkWas.size(); ++i) {
                if (!inScope(linkWas[i]))
                    continue; // a link in another row keeps its place
                const WORKSPACEID NOW = FullscreenBook::compactedPlace(linkWas[i], ROW);
                g_links[i].owes       = NOW != FullscreenBook::NO_PLACE;
                g_links[i].at         = NOW;
            }
            syncPlaces();
            return true;
        }

        // Returns ms until it wants another look, or 0: nothing due.
        int compactIfDue() {
            if (!g_compactDue)
                return 0;
            const auto LEFT = COMPACT_SETTLE - std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - g_compactAt);
            if (LEFT.count() > 0)
                return std::max<int>(1, LEFT.count());
            if (compactBlocked())
                return 60;
            const std::string WHY = std::exchange(g_compactDue, nullptr);
            (void)compactNow(WHY, nullptr);
            return 0;
        }

        // THE ONE PASS. Run on the event loop after every fullscreen change,
        // move and close -- never inside one: Hyprland takes a window out of
        // fullscreen to move it and puts it back after, so only the state once
        // the event is done means anything.
        // MOVED ONTO A POPULATED DESKTOP, IT LEAVES FULLSCREEN (user, 2026-11-11:
        // "if I move a fullscreen window 'to this workspace' and there's other
        // apps on that workspace it should un-fullscreen it"; "Fullscreen game
        // moved to a populated desktop yes should also leave fullscreen"). The
        // trips have this rule below ("sent elsewhere"); this is the same for
        // a fullscreen window that is not on one -- alone on its own desktop
        // (g_settled), or fullscreen under fullscreen_desktop = "stay" -- however
        // it was moved: the dock's "Move to this workspace", a drag in the
        // overview or the strip, a dispatcher, Super+Shift+N. Only a MOVE:
        // another window put on a fullscreen app's desktop is not one (a game
        // keeps its fullscreen, FullscreenGuard). Moved to an empty desktop it
        // stays fullscreen there, alone -- its own desktop again. Real
        // fullscreen only here, as for the trips; a maximized window: maximized() below.
        // ...AND STAYS OUT: a Wine game asks for fullscreen again the moment
        // it is taken out (rig: wine_state.exe was back in fullscreen and on a
        // trip of its own within a second). While it stays on that desktop
        // with others, the client's own requests for fullscreen are ignored
        // (Hyprland's SUPPRESS_FULLSCREEN, as `suppress_event fullscreen`);
        // the user's fullscreen (a keybind, the dock) still works, and so
        // does the client's again once the window is moved on or alone there.
        // The same for a maximized window whose CLIENT maximizes it again
        // right after (an X11 client insisting on _NET_WM_STATE, a Wine game
        // that runs "maximized"): within UNMAX_REASSERT_MS of being
        // un-maximized there, it is un-maximized once more and its own
        // maximize requests ignored (SUPPRESS_MAXIMIZE) while it shares that
        // desktop. A user's Super+Alt+F later on is theirs: it stands.
        constexpr float UNMAX_REASSERT_MS = 1500.F;
        bool            maximized(const PHLWINDOW& w) {
            return w && Fullscreen::controller()->getFullscreenModes(w).internal == Fullscreen::FSMODE_MAXIMIZED;
        }
        struct SHeldOut {
            PHLWINDOWREF    win;
            PHLWORKSPACEREF space;
            uint64_t        flag = Desktop::View::SUPPRESS_FULLSCREEN;
        };
        std::vector<SHeldOut> g_heldOut;
        void releaseHeld(bool all) {
            std::erase_if(g_heldOut, [&](const SHeldOut& h) {
                const auto W = h.win.lock();
                if (!W)
                    return true;
                const bool IN = h.flag == Desktop::View::SUPPRESS_MAXIMIZE ? maximized(W) : fullscreen(W);
                if (!all && W->m_isMapped && W->m_workspace == h.space.lock() && !IN && othersOn(W->m_workspace, W) > 0)
                    return false;
                W->m_suppressedEvents = sc<decltype(W->m_suppressedEvents)>(W->m_suppressedEvents & ~h.flag);
                return true;
            });
        }
        void holdOut(const PHLWINDOW& w, uint64_t flag = Desktop::View::SUPPRESS_FULLSCREEN) {
            if (!w || (w->m_suppressedEvents & flag))
                return; // the user's own rule, or ours already
            w->m_suppressedEvents = sc<decltype(w->m_suppressedEvents)>(w->m_suppressedEvents | flag);
            g_heldOut.push_back({w, w->m_workspace, flag});
        }
        // Un-maximized by a move, and when: a re-maximize soon after is the client's.
        struct SUnmaxed {
            PHLWINDOWREF    win;
            PHLWORKSPACEREF space;
            Time::steady_tp at;
        };
        std::vector<SUnmaxed> g_unmaxed;
        void unmaximize(const PHLWINDOW& w) {
            ++g_busy;
            Fullscreen::controller()->setFullscreenMode(w, Fullscreen::FSMODE_NONE, Fullscreen::FSMODE_NONE);
            --g_busy;
        }
        void reasserted() {
            std::erase_if(g_unmaxed, [](const SUnmaxed& u) {
                const auto W = u.win.lock();
                if (!W || !W->m_isMapped || W->m_workspace != u.space.lock() || Config::realElapsedMs(u.at) > UNMAX_REASSERT_MS)
                    return true;
                if (!maximized(W))
                    return false;
                trace(std::format("{} maximized itself again on {}: un-maximized, its requests ignored while it shares the desktop", W->m_class, W->m_workspace->m_id));
                unmaximize(W);
                holdOut(W, Desktop::View::SUPPRESS_MAXIMIZE);
                return true;
            });
        }

        // ...AND A MAXIMIZED ONE IS UN-MAXIMIZED (owner, 2026-11-11: "yes"):
        // moved onto a desktop with other windows it is tiled there by the
        // layout like any window that arrives (Mosaic: at its remembered size,
        // the others not resized); onto an empty desktop it stays maximized.
        // No hold-out for an ordinary app; one whose client maximizes it again
        // gets one (reasserted, above). XGeometry's storm guard only strips a
        // request for the state the window already HAS, so it never fights this.
        // A WINDOW MOVED ONTO A FULLSCREEN APP'S DESKTOP TAKES IT OUT (owner,
        // 2026-11-11: "if I move a window into a desktop with a fullscreen app
        // it should un-fullscreen the app in the target desktop"). The rule
        // above, the other way round: after a move a desktop never holds a
        // fullscreen or maximized window together with others. The app there
        // leaves fullscreen (maximize) and both are tiled by the layout
        // (Mosaic: the arriving window at its size, the app back at the size
        // it had before fullscreen). A GAME too -- a deliberate move overrides
        // the game exception -- held out (SUPPRESS_FULLSCREEN) while it shares
        // the desktop, so Wine does not put it back. An app on a fullscreen
        // TRIP ends it where it is: it no longer is alone, the desktop is its
        // own now and nothing is owed -- no flight home, no place paid back.
        // Not a move: a window opening over a fullscreen app (besideFullscreen
        // sends it on), our own trip moves (g_busy), a window coming back out
        // of the minimize stash or a scratchpad (SMoved::arrived).
        void arrivedOnFullscreen(const PHLWINDOW& w, const std::vector<std::pair<PHLWINDOWREF, bool>>& hosts) {
            for (const auto& [REF, WASFS] : hosts) {
                const auto X = REF.lock();
                if (!X || X == w || !X->m_isMapped || X->m_workspace != w->m_workspace)
                    continue;
                if (!WASFS) {
                    trace(std::format("{} moved onto {}: maximized {} un-maximized", w->m_class, w->m_workspace->m_id, X->m_class));
                    if (maximized(X))
                        unmaximize(X);
                    std::erase_if(g_unmaxed, [&](const SUnmaxed& u) { return !u.win || u.win.lock() == X; });
                    g_unmaxed.push_back({X, X->m_workspace, Time::steadyNow()});
                    continue;
                }
                const bool TRIP = std::erase_if(g_trips, [&](const STrip& t) { return t.win.lock() == X; }) > 0;
                trace(std::format("{} moved onto {}: fullscreen {} out of fullscreen{}", w->m_class, w->m_workspace->m_id, X->m_class,
                                  TRIP ? ", its trip ends here (nothing owed)" : ""));
                std::erase_if(g_settled, [&](const PHLWINDOWREF& r) { return !r || r.lock() == X; });
                if (fullscreen(X)) {
                    ++g_busy;
                    Fullscreen::controller()->setFullscreenMode(X, Fullscreen::FSMODE_NONE, Fullscreen::FSMODE_NONE);
                    --g_busy;
                }
                holdOut(X);
            }
        }

        void movedOntoOthers() {
            releaseHeld(false);
            reasserted();
            for (const auto& M : std::exchange(g_moved, {})) {
                const auto W = M.win.lock();
                if (!W || !W->m_isMapped || !DesktopOrder::ordinary(W->m_workspace) || othersOn(W->m_workspace, W) == 0)
                    continue;
                if (M.arrived && !maximized(W) && !fullscreen(W))
                    arrivedOnFullscreen(W, M.hosts);
                if (maximized(W)) {
                    trace(std::format("{} moved onto {} with other windows: un-maximized", W->m_class, W->m_workspace->m_id));
                    unmaximize(W);
                    std::erase_if(g_unmaxed, [&](const SUnmaxed& u) { return !u.win || u.win.lock() == W; });
                    g_unmaxed.push_back({W, W->m_workspace, Time::steadyNow()});
                    continue;
                }
                if (tripOf(W) || !fullscreen(W))
                    continue;
                trace(std::format("{} moved onto {} with other windows: out of fullscreen", W->m_class, W->m_workspace->m_id));
                std::erase_if(g_settled, [&](const PHLWINDOWREF& r) { return !r || r.lock() == W; });
                ++g_busy;
                Fullscreen::controller()->setFullscreenMode(W, Fullscreen::FSMODE_NONE, Fullscreen::FSMODE_NONE);
                --g_busy;
                holdOut(W);
            }
        }

        // A link is over: what it owes is a beside debt, paid once its desktop
        // is empty and off screen; a place it was given free is counted into
        // the row by a compaction like any other (g_given).
        void endLink(const SLink& l) {
            SDebt d{.space = l.space, .at = placeOf(l), .mon = l.mon, .home = l.appSpace, .pid = l.pids.empty() ? 0 : l.pids.front(),
                    .beside = true, .wins = l.wins, .pids = l.pids, .classes = l.classes, .app = l.app};
            if (!l.classes.empty())
                d.cls = l.classes.front();
            if (l.owes)
                g_debts.push_back(d);
            else if (l.space.lock())
                g_given.push_back(d);
        }

        // THE WAY BACK (owner, 2026-11-11: "if the window is like a file
        // selection prompt or something intentional and temporary, upon
        // closing the window, user should be sent back to the fullscreen
        // app"). Temporary means its desktop emptied: a window that stays keeps
        // it. Only for a link the user wanted -- asked for, or went to since --
        // and is looking at, with the app still fullscreen on its desktop; then
        // the strip glides back the way it came, the app gets the focus (a
        // game its pointer lock), and the made desktop is given back once it
        // is off screen (the debt). Everything else ends the link quietly.
        // Returns whether a link still waits on the screen or the strip.
        bool links() {
            const auto NOW   = std::chrono::steady_clock::now();
            bool       watch = false;
            for (auto it = g_links.begin(); it != g_links.end();) {
                auto&      L     = *it;
                const auto APP   = L.app.lock();
                const auto SPACE = L.space.lock();
                const auto MON   = L.mon.lock();
                FullscreenBook::SLinkFacts f{.appAlive = APP && APP->m_isMapped, .appFullscreen = APP && fullscreen(APP) && APP->m_workspace == L.appSpace.lock(),
                                             .spaceAlive = SPACE && DesktopOrder::ordinary(SPACE), .driving = anyDriving()};
                std::string foreignOne;
                if (f.spaceAlive)
                    for (const auto& W : Desktop::windowState()->windows())
                        if (W && W->m_isMapped && !W->isHidden() && W->m_workspace == SPACE) {
                            (member(L, W) ? f.any : f.foreign) = true;
                            if (!member(L, W))
                                foreignOne = std::format("{} {:x}", W->m_class, (uintptr_t)W.get());
                        }
                f.viewOn = MON && SPACE && MON->m_activeWorkspace == SPACE;
                // He went there himself (the dock, the strip, the overview): wanted.
                if (f.viewOn && !L.wanted && !f.driving) {
                    L.wanted = true;
                    for (const auto& R : L.wins)
                        if (const auto W = R.lock())
                            W->m_isUrgent = false;
                }
                f.wanted = L.wanted;
                if (f.any)
                    L.emptySince.reset();
                else if (!L.emptySince)
                    L.emptySince = NOW;
                f.settling = L.emptySince && NOW - *L.emptySince < LINK_SETTLE;
                const auto VERDICT = FullscreenBook::link(f);
                if (VERDICT == FullscreenBook::ELink::Hold) {
                    watch |= !f.any; // emptied, waiting on the settle or the strip
                    ++it;
                    continue;
                }
                if (VERDICT == FullscreenBook::ELink::ReturnToApp && MON && APP) {
                    trace(std::format("beside {} emptied: back to fullscreen {} on {}", placeOf(L), APP->m_class, APP->m_workspace->m_id));
                    show(MON, APP->m_workspace);
                    Desktop::focusState()->fullWindowFocus(APP, Desktop::FOCUS_REASON_OTHER);
                    if (FullscreenGuard::isGame(APP))
                        FullscreenGuard::resumePointerLock();
                } else
                    trace(std::format("beside link at {} ends{}", placeOf(L), f.foreign ? ": a desktop of its own now (" + foreignOne + ")" : ""));
                endLink(L);
                it = g_links.erase(it);
            }
            return watch;
        }

        int reconcile(void*) {
            if (!g_pCompositor || g_pCompositor->m_isShuttingDown)
                return 0;
            // TWO DESKTOPS ON ONE NUMBER (left by the old half-applied
            // renumber, or by anything else): pulled apart at load and when a
            // monitor comes or goes -- the fuller keeps the number, the other
            // gets the next free one above the highest. Windows are never
            // merged nor dropped.
            if (const char* WHY = std::exchange(g_healDue, nullptr))
                (void)DesktopOrder::heal(WHY);
            showBesides();
            movedOntoOthers();
            origins();
            std::erase_if(g_settled, [](const PHLWINDOWREF& r) { const auto W = r.lock(); return !W || !W->m_isMapped || !fullscreen(W); });
            // Trips.
            for (auto it = g_trips.begin(); it != g_trips.end();) {
                const STrip T     = *it;
                const auto  W     = T.win.lock();
                const auto  SPACE = T.space.lock();
                if (W && W->m_isMapped && SPACE && W->m_workspace == SPACE && fullscreen(W)) {
                    it->unmappedAt.reset();
                    ++it; // where it belongs
                    continue;
                }
                // UNMAPPED, NOT GONE: a Wine/XWayland game unmaps its window
                // and maps it again (a mode change, a launcher handing over).
                // That is no close -- the trip waits REMAP_GRACE for it.
                if (W && !W->m_isMapped && SPACE && W->m_workspace == SPACE) {
                    const auto NOW = std::chrono::steady_clock::now();
                    if (!it->unmappedAt)
                        it->unmappedAt = NOW;
                    if (NOW - *it->unmappedAt < REMAP_GRACE) {
                        ++it;
                        continue;
                    }
                }
                it = g_trips.erase(it);
                g_leftSpace = T.space;
                g_leftAt    = std::chrono::steady_clock::now();
                if (!W || !W->m_isMapped) {
                    // CLOSED (user, 2026-11-11: "once I've closed an app that was
                    // sent to another desktop to fullscreen, it should remove
                    // that desktop and return me to its origin desktop"): still
                    // looking at its desktop, home -- the trip's way back -- and
                    // its place is paid back (endTrip below; the app's own
                    // leftovers there go home first, FullscreenBook). Not looking
                    // at it: no view change, the place is paid back all the same.
                    trace("fullscreen app closed");
                    if (const auto MON = T.mon.lock(); MON && SPACE && MON->m_activeWorkspace == SPACE)
                        if (const auto BACK = backAfterClose(T.home.lock(), T.homeAt, MON, SPACE))
                            show(MON, BACK);
                } else if (SPACE && W->m_workspace == SPACE)
                    goHome(T, W);
                else if (W->m_workspace) {
                    // SENT ELSEWHERE (user, 2026-11-05: "that window should be
                    // exited from fullscreen and land in the desktop with
                    // windows that it was intended to go into"): it stays.
                    trace(std::format("{} sent to {}: it stays there", W->m_class, W->m_workspace->m_id));
                    ++g_busy;
                    if (fullscreen(W) && othersOn(W->m_workspace, W) > 0) {
                        Fullscreen::controller()->setFullscreenMode(W, Fullscreen::FSMODE_NONE, Fullscreen::FSMODE_NONE);
                        holdOut(W);
                    }
                    --g_busy;
                    if (fullscreen(W))
                        g_settled.emplace_back(W);
                    if (const auto MON = T.mon.lock(); MON && SPACE && MON->m_activeWorkspace == SPACE && W->m_workspace->m_monitor.lock() == MON)
                        show(MON, W->m_workspace);
                }
                endTrip(T, W);
            }
            // New fullscreen windows.
            for (const auto& W : Desktop::windowState()->windows())
                if (W && W->m_isMapped && fullscreen(W) && DesktopOrder::ordinary(W->m_workspace) && W->m_workspace->m_monitor.lock() && !tripOf(W) &&
                    std::ranges::none_of(g_settled, [&](const PHLWINDOWREF& r) { return r.lock() == W; }))
                    startTrip(W);
            // Beside links, before the debts they end in.
            const bool LINK_WATCH = links();
            // A display change: no empty desktops between (before the debts:
            // the ones it settles are not paid again).
            const int COMPACT_MS = compactIfDue();
            // Debts.
            syncPlaces();
            bool watch = false; // a debt waits on the screen or a strip: no event says when that ends
            for (auto it = g_debts.begin(); it != g_debts.end();) {
                const EPaid P = pay(*it);
                watch |= P == EPaid::WaitScreen;
                it = P == EPaid::Done ? g_debts.erase(it) : std::next(it);
            }
            // (A beside debt waiting for its windows to go is looked at again
            // when one closes or moves: the hooks below.)
            // Every size held for a move this pass made (or decided against).
            ConfigureHold::release();
            int next = watch || LINK_WATCH ? 60 : !g_trips.empty() || !g_origins.empty() || !g_links.empty() ? 250 : 0;
            if (COMPACT_MS > 0 && (next == 0 || COMPACT_MS < next))
                next = COMPACT_MS;
            if (g_timer && next > 0)
                wl_event_source_timer_update(g_timer, next);
            return 0;
        }
        void arm(int ms) {
            if (!g_timer && g_pCompositor)
                g_timer = wl_event_loop_add_timer(g_pCompositor->m_wlEventLoop, reconcile, nullptr);
            if (g_timer)
                wl_event_source_timer_update(g_timer, ms);
        }
    }

    void preRender(PHLMONITOR monitor) {
        for (auto it = g_travels.begin(); it != g_travels.end();) {
            const auto W = it->win.lock();
            const auto G = it->gauge.lock();
            if (!W || !W->m_isMapped || !W->m_workspace || std::chrono::steady_clock::now() - it->at > std::chrono::seconds(3)) {
                it = g_travels.erase(it);
                continue;
            }
            if (W->m_workspace->m_monitor.lock() != monitor) {
                ++it;
                continue;
            }
            const Vector2D GO   = offsetOf(G);
            const Vector2D GOAL = W->positionAnimation()->goal(), GSIZE = W->sizeAnimation()->goal();
            // In step with the slide -- but never faster than TRAVEL_MIN_MS,
            // eased: spammed, a toggle can land while the slide is still a few
            // px from the desktop it turns back to, and a trip scaled to that
            // leftover distance put the window in its place in one frame
            // (user, 2026-11-06: "sometimes spamming super+f ... snaps it to
            // fullscreen without doing the transition").
            const double   SLIDT = G && it->span > 0.5 ? std::clamp(1.0 - std::hypot(GO.x, GO.y) / it->span, 0.0, 1.0) : 1.0;
            // The floor is on the animation clock (the speed setting stretches
            // it); a snapping clock ends the trip here, slide or not: the
            // window lands on its goal, where it rides the rest of the slide
            // with its desktop.
            const double   TIMEL = std::clamp(Hyprdrive::Motion::Clock::animMsBetween<double>(std::chrono::steady_clock::now(), it->at) / TRAVEL_MIN_MS, 0.0, 1.0);
            const double   T     = Hyprdrive::Motion::Clock::enabled() ? std::min(SLIDT, Hyprdrive::Motion::outCubicPow(TIMEL)) : 1.0;
            if (it->goal && it->drawn && (it->goal->first != GOAL || it->goal->second != GSIZE)) {
                // Re-aimed: the rest of the trip sets off from where it is drawn.
                it->seen     = it->screen ? it->drawn->first : it->drawn->first - offsetOf(W->m_workspace);
                it->seenSize = it->drawn->second;
                it->t0       = std::min(it->lastT, 0.999);
            }
            it->goal  = {GOAL, GSIZE};
            it->lastT = std::max(it->lastT, T);
            if (T >= 1.0 - 1e-4) {
                W->positionAnimation()->setValueAndWarp(GOAL);
                W->sizeAnimation()->setValueAndWarp(GSIZE);
                it = g_travels.erase(it);
                continue;
            }
            // setValue: this frame's place, the goal left as the layout set it.
            const double   U     = std::clamp((T - it->t0) / (1.0 - it->t0), 0.0, 1.0);
            const Vector2D PLACE = it->seen + (GOAL - it->seen) * U;
            const Vector2D SIZE  = it->seenSize + (GSIZE - it->seenSize) * U;
            W->positionAnimation()->setValue(it->screen ? PLACE - offsetOf(W->m_workspace) : PLACE);
            W->sizeAnimation()->setValue(SIZE);
            it->drawn = {it->screen ? PLACE : PLACE + offsetOf(W->m_workspace), SIZE};
            ++it;
        }
    }

    bool travelling(const PHLWINDOW& w, const PHLMONITOR& monitor) {
        return w && monitor && w->m_workspace && w->m_workspace->m_monitor.lock() == monitor &&
            std::ranges::any_of(g_travels, [&](const STravel& t) { return t.win.lock() == w; });
    }

    bool justLeft(const PHLWORKSPACE& ws) {
        return ws && g_leftSpace.lock() == ws && std::chrono::steady_clock::now() - g_leftAt < std::chrono::seconds(1);
    }

    bool justEntered(const PHLWORKSPACE& ws) {
        return ws && g_enteredSpace.lock() == ws && std::chrono::steady_clock::now() - g_enteredAt < std::chrono::seconds(1);
    }

    void launchedFrom(const PHLWINDOW& w, const PHLWORKSPACE& origin) {
        if (!w || !DesktopOrder::ordinary(origin))
            return;
        std::erase_if(g_origins, [&](const SOrigin& o) { return !o.win || o.win.lock() == w; });
        g_origins.push_back({.win = w, .origin = origin, .originAt = origin->m_id, .mon = origin->m_monitor});
        arm(250);
    }

    bool carrying(const PHLWINDOW& w, bool home) {
        return w && g_carrying.home == home && g_carrying.win.lock() == w;
    }

    PHLWINDOW fullscreenAppOn(const PHLWORKSPACE& ws) {
        return appOn(ws);
    }

    PHLWORKSPACE beside(const PHLWINDOW& w, bool prompted) {
        return besideFullscreen(w, prompted);
    }

    void openedOn(const PHLWINDOW& w, bool prompted) {
        if (!w || !w->m_workspace)
            return;
        for (auto& L : g_links)
            if (L.space.lock() == w->m_workspace) {
                join(L, w);
                trace(std::format("{} opened on beside {}: joins it", w->m_class, w->m_workspace->m_id));
                return;
            }
        // OPENED ON A DESKTOP STILL OWED FOR A FULLSCREEN APP (the race the
        // 30-cycle gate found): the way back ended the link, and a window
        // launched from that desktop a moment before maps on it now -- during
        // the glide back, or after it. It is the link's: the debt becomes the
        // link again (what it owes kept), so the window is not "a desktop of
        // its own" that the room made for the link is never given back for.
        // Asked for, the view goes back to it (the glide reverses); not, it
        // stays there quietly, marked. Only while that app is still
        // fullscreen on its desktop; else it is an ordinary window there.
        const auto WS = w->m_workspace;
        // (By the desktop, or by its NUMBER when that desktop has gone:
        // Hyprland drops an emptied desktop the moment the view leaves it,
        // and makes a new one under that number for a window launched there.)
        for (auto it = g_debts.begin(); it != g_debts.end(); ++it) {
            const auto SPACE = it->space.lock();
            if (!it->beside || (SPACE ? SPACE != WS : placeOf(*it) != WS->m_id))
                continue;
            const auto APP  = it->app.lock();
            const auto HOME = it->home.lock();
            const auto MON  = it->mon.lock();
            if (!FullscreenBook::relink({.besideDebt    = true,
                                         .appFullscreen = APP && APP->m_isMapped && fullscreen(APP) && HOME && APP->m_workspace == HOME,
                                         .sameMonitor   = MON && WS->m_monitor.lock() == MON}))
                return;
            SLink l{.app = APP, .appSpace = HOME, .space = WS, .mon = MON, .at = WS->m_id, .owes = true, .wanted = prompted,
                    .wins = it->wins, .pids = it->pids, .classes = it->classes};
            g_debts.erase(it);
            join(l, w);
            g_links.push_back(std::move(l));
            trace(std::format("{} opened on owed beside {}: the link again{}", w->m_class, WS->m_id, prompted ? "" : " (unasked: not shown)"));
            w->m_noInitialFocus = true;
            (prompted ? g_asked : g_unasked).emplace_back(w);
            arm(1);
            return;
        }
    }

    bool isFullscreenDesktop(const PHLWORKSPACE& ws) {
        return ws && isTripSpace(ws);
    }

    namespace {
        constexpr const char* SAVED_ENV = "HYPRDRIVE_FSDESK_STATE";

        template <class T>
        std::uintptr_t addr(const T& p) {
            return reinterpret_cast<std::uintptr_t>(p.get());
        }

        // A reload hands the trips over (FullscreenBook::SSaved): matched by
        // address against the live windows, desktops and monitors.
        void restore() {
            const char* RAW = std::getenv(SAVED_ENV);
            if (!RAW)
                return;
            const auto SAVED = FullscreenBook::load(RAW);
            ::unsetenv(SAVED_ENV);
            if (!SAVED)
                return;
            const auto WIN = [](std::uintptr_t p) -> PHLWINDOW {
                if (p)
                    for (const auto& W : Desktop::windowState()->windows())
                        if (W && addr(W) == p)
                            return W;
                return nullptr;
            };
            const auto WS = [](std::uintptr_t p) -> PHLWORKSPACE {
                if (p)
                    for (const auto& REF : State::workspaceState()->workspaces())
                        if (const auto S = REF.lock(); S && addr(S) == p)
                            return S;
                return nullptr;
            };
            const auto MON = [](std::uintptr_t p) -> PHLMONITOR {
                if (p)
                    for (const auto& M : State::monitorState()->monitors())
                        if (M && addr(M) == p)
                            return M;
                return nullptr;
            };
            for (const auto& T : SAVED->trips) {
                if (const auto W = WIN(T.win))
                    g_trips.push_back({.win = W, .home = WS(T.home), .space = WS(T.space), .mon = MON(T.mon), .at = T.at, .homeAt = T.homeAt, .owes = T.owes});
                else if (T.owes) // closed meanwhile: what it owed is still owed
                    g_debts.push_back({.space = WS(T.space), .at = T.at, .mon = MON(T.mon), .home = WS(T.home)});
            }
            for (const auto& D : SAVED->debts)
                g_debts.push_back({.space = WS(D.space), .at = D.at, .mon = MON(D.mon), .home = WS(D.home), .cls = D.cls, .pid = pid_t(D.pid), .beside = D.beside});
            for (const auto P : SAVED->settled)
                if (const auto W = WIN(P))
                    g_settled.emplace_back(W);
            for (const auto& B : SAVED->besides) {
                SLink l{.app = WIN(B.app), .appSpace = WS(B.appSpace), .space = WS(B.space), .mon = MON(B.mon), .at = B.at, .owes = B.owes, .wanted = B.wanted};
                if (B.pid > 0)
                    l.pids.push_back(pid_t(B.pid));
                if (!B.cls.empty())
                    l.classes.push_back(B.cls);
                g_links.push_back(std::move(l)); // the next reconcile checks it like any other
            }
            trace(std::format("reloaded: {} trips, {} debts, {} settled, {} beside links handed over", g_trips.size(), g_debts.size(), g_settled.size(), g_links.size()));
        }

        void save() {
            syncPlaces();
            FullscreenBook::SSaved s;
            for (const auto& T : g_trips)
                if (const auto W = T.win.lock())
                    s.trips.push_back({.win = addr(W), .home = addr(T.home.lock()), .space = addr(T.space.lock()), .mon = addr(T.mon.lock()), .at = T.at, .homeAt = T.homeAt,
                                       .owes = T.owes});
            for (const auto& D : g_debts)
                s.debts.push_back({.space = addr(D.space.lock()), .mon = addr(D.mon.lock()), .home = addr(D.home.lock()), .at = D.at, .pid = D.pid, .beside = D.beside, .cls = D.cls});
            for (const auto& R : g_settled)
                if (const auto W = R.lock())
                    s.settled.push_back(addr(W));
            for (const auto& L : g_links)
                s.besides.push_back({.app = addr(L.app.lock()), .appSpace = addr(L.appSpace.lock()), .space = addr(L.space.lock()), .mon = addr(L.mon.lock()), .at = L.at,
                                     .owes = L.owes, .wanted = L.wanted, .pid = L.pids.empty() ? 0 : L.pids.front(), .cls = L.classes.empty() ? std::string{} : L.classes.front()});
            if (s.trips.empty() && s.debts.empty() && s.settled.empty() && s.besides.empty())
                ::unsetenv(SAVED_ENV);
            else
                ::setenv(SAVED_ENV, FullscreenBook::save(s).c_str(), 1);
        }
    }

    void init() {
        DesktopOrder::onRenumbered(syncPlaces);
        ConfigureHold::setGate(sizesWait);
        restore();
        g_healDue = "load";
        arm(1);
        // A display change: desktops sharing a number pulled apart at once,
        // the empty ones removed once the burst settles (compactIfDue).
        const auto DISPLAY = [](const char* why, bool heal) {
            if (!g_pCompositor || g_pCompositor->m_isShuttingDown)
                return;
            if (heal)
                g_healDue = why;
            if (!g_compactDue)
                g_compactDue = why; // the burst is named by its first event
            g_compactAt = std::chrono::steady_clock::now();
            arm(1);
        };
        g_monAddHook    = Event::bus()->m_events.monitor.added.listen([DISPLAY](PHLMONITOR) { DISPLAY("a monitor came", true); });
        g_monGoneHook   = Event::bus()->m_events.monitor.removed.listen([DISPLAY](PHLMONITOR) { DISPLAY("a monitor went", true); });
        // Mode, scale, position, transform, mirror (CMonitorRuleManager::
        // ensureMonitorStatus -- config, hyprctl, wlr-output-management --,
        // CMonitor::setMirror, CMonitorLayoutController::arrange).
        g_monLayoutHook = Event::bus()->m_events.monitor.layoutChanged.listen([DISPLAY]() { DISPLAY("the monitor layout changed", false); });
        // Every change is looked at once it has fully happened.
        g_fsHook    = Event::bus()->m_events.window.fullscreen.listen([](PHLWINDOW w) {
            if (!g_busy)
                arm(1);
        });
        g_moveHook  = Event::bus()->m_events.window.moveToWorkspace.listen([](PHLWINDOW w, PHLWORKSPACE) {
            if (g_busy)
                return;
            // Moved by anyone but us: looked at once the move is done
            // (movedOntoOthers; Hyprland takes a fullscreen window out of
            // fullscreen for the move and puts it back after).
            if (w) {
                std::erase_if(g_lastSpace, [&](const auto& e) { return !e.first || !e.second; });
                auto       LAST    = std::ranges::find_if(g_lastSpace, [&](const auto& e) { return e.first.lock() == w; });
                const bool ARRIVED = LAST == g_lastSpace.end() || DesktopOrder::ordinary(LAST->second.lock());
                if (LAST == g_lastSpace.end())
                    g_lastSpace.emplace_back(w, w->m_workspace);
                else
                    LAST->second = w->m_workspace;
                if (!tripOf(w)) {
                    SMoved m{.win = w, .arrived = ARRIVED};
                    for (const auto& X : Desktop::windowState()->windows())
                        if (X && X != w && X->m_isMapped && X->m_workspace == w->m_workspace && (fullscreen(X) || maximized(X)))
                            m.hosts.emplace_back(X, fullscreen(X));
                    g_moved.push_back(std::move(m));
                }
            }
            if (!g_trips.empty() || !g_debts.empty() || !g_moved.empty() || !g_heldOut.empty() || !g_links.empty())
                arm(1);
        });
        g_shownHook = Event::bus()->m_events.workspace.active.listen([](PHLWORKSPACE) {
            if (!g_links.empty())
                arm(1);
        });
        g_closeHook = Event::bus()->m_events.window.close.listen([](PHLWINDOW) {
            if (!g_trips.empty() || !g_debts.empty() || !g_heldOut.empty() || !g_origins.empty() || !g_links.empty())
                arm(1);
        });
    }

    bool compactRow(const PHLMONITOR& monitor, const char* why) {
        if (!monitor || !g_pCompositor || g_pCompositor->m_isShuttingDown)
            return false;
        // What the display compaction waits for, but an overview: the button
        // is ON the overview, which rebuilds its cards after (OverviewStrip.cpp).
        if (anyDriving() || DesktopStrip::switching() || (g_pUnifiedWorkspaceSwipe && g_pUnifiedWorkspaceSwipe->isGestureInProgress())) {
            trace(std::format("{}: refused, a desktop is on the move", why));
            return false;
        }
        const Row ROW = Rows::of(monitor);
        if (ROW.provisional) {
            trace(std::format("{}: refused, the monitor's block is not known yet", why));
            return false;
        }
        return compactNow(why, &ROW);
    }

    void compactSoon(const char* why) {
        if (!g_pCompositor || g_pCompositor->m_isShuttingDown || !g_timer)
            return;
        if (!g_compactDue)
            g_compactDue = why;
        g_compactAt = std::chrono::steady_clock::now();
        arm(1);
    }

    void shutdown() {
        // (Also at Hyprland's exit: the handover then goes with the process.)
        save();
        DesktopOrder::onRenumbered(nullptr);
        // Nothing stays held by a gate whose code goes with the plugin.
        ConfigureHold::setGate(nullptr);
        g_monAddHook.reset();
        g_monGoneHook.reset();
        g_monLayoutHook.reset();
        g_fsHook.reset();
        g_closeHook.reset();
        g_moveHook.reset();
        g_shownHook.reset();
        if (g_timer) {
            wl_event_source_remove(g_timer);
            g_timer = nullptr;
        }
        g_trips.clear();
        g_debts.clear();
        g_given.clear();
        g_settled.clear();
        g_compactDue = nullptr;
        g_besides.clear();
        g_unasked.clear();
        g_asked.clear();
        g_links.clear();
        g_moved.clear();
        releaseHeld(true);
        g_origins.clear();
        g_travels.clear();
    }
}
