#include "FullscreenDesktop.hpp"
#include "MotionCurves.hpp"
#include "MotionClock.hpp"
#include "DesktopOrder.hpp"
#include "FullscreenBook.hpp"
#include "DesktopStrip.hpp"
#include "Config.hpp"
#include "DebugPaths.hpp"
#include "IOverview.hpp"

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
#include <hyprland/src/Compositor.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
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
        };
        struct SDebt {
            PHLWORKSPACEREF space; // the desktop in the owed place, while it lives
            WORKSPACEID     at = WORKSPACE_INVALID; // the owed place (kept up as numbers move)
            PHLMONITORREF   mon;
            PHLWORKSPACEREF home;  // where the app's own leftovers go
            std::string     cls;
            pid_t           pid    = 0;
            bool            beside = false; // a window opened over a fullscreen app was given this place: paid once it is empty
        };
        std::vector<STrip>   g_trips;
        std::vector<SDebt>   g_debts;
        std::vector<PHLWINDOWREF> g_settled; // fullscreen, deliberately left where it is
        std::vector<PHLWINDOWREF> g_besides; // opened over a fullscreen app and moved on: shown once mapped
        PHLWORKSPACEREF          g_leftSpace; // the desktop the last trip ended on (justLeft)
        std::chrono::steady_clock::time_point g_leftAt;
        PHLWORKSPACEREF          g_enteredSpace; // the desktop the last trip went to (justEntered)
        std::chrono::steady_clock::time_point g_enteredAt;
        int                g_busy  = 0; // our own moves: their fullscreen changes are not the user's
        wl_event_source*   g_timer = nullptr;
        CHyprSignalListener g_fsHook, g_closeHook, g_moveHook, g_openHook;
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
        }
        void moved(WORKSPACEID from, int delta) {
            for (auto& T : g_trips) {
                if (!T.space.lock())
                    T.at = FullscreenBook::movedPlace(T.at, from, delta);
                if (!T.home.lock())
                    T.homeAt = FullscreenBook::movedPlace(T.homeAt, from, delta);
            }
            for (auto& D : g_debts)
                if (!D.space.lock())
                    D.at = FullscreenBook::movedPlace(D.at, from, delta);
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
            const auto E = byId(id);
            return !E || (E->m_monitor.lock() == mon && othersOn(E, nullptr) == 0 && !isTripSpace(E));
        }

        void startTrip(const PHLWINDOW& w) {
            const auto HOME = w->m_workspace;
            const auto MON  = HOME->m_monitor.lock();
            const auto MODE = mode();
            const auto PLAN = FullscreenBook::start(MODE, HOME->m_id, othersOn(HOME, w), [&](WORKSPACEID id) { return freePlace(id, MON); });
            if (PLAN.settle || !MON) {
                g_settled.emplace_back(w); // alone on its desktop: that is its own already
                return;
            }
            const WORKSPACEID at   = PLAN.at;
            bool              owes = false;
            if (PLAN.makeRoom) {
                // No room after it: the desktops from there on move along a place.
                DesktopStrip::finishNow(); // (the strip lands first: its row is numbers)
                if (!DesktopOrder::shift(at, +1)) {
                    // Refused (DesktopOrder logged why): no room is made, so it
                    // stays where it is, as Hyprland has it.
                    g_settled.emplace_back(w);
                    return;
                }
                moved(at, +1);
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
            carry(w, space, true);
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
            carry(w, home, false);
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
                const auto OWN = [&](const PHLWINDOW& W) { return (d.pid > 0 && W->getPID() == d.pid) || (!d.cls.empty() && W->m_class == d.cls); };
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
                // The empty desktop in the place goes to the end of the row
                // (Hyprland drops it once nothing holds it); the ones after
                // it come back a place.
                std::vector<std::pair<PHLWORKSPACE, WORKSPACEID>> moves;
                WORKSPACEID                                       top = AT;
                for (const auto& REF : State::workspaceState()->workspaces())
                    if (const auto WS = REF.lock(); DesktopOrder::ordinary(WS) && WS != E && WS->m_id > AT) {
                        top = std::max(top, WS->m_id);
                        moves.emplace_back(WS, WS->m_id - 1);
                    }
                if (top > AT) {
                    moves.emplace_back(E, top);
                    if (!DesktopOrder::renumber(moves))
                        return EPaid::Done; // refused (logged): never half-paid
                }
            } else if (!DesktopOrder::shift(AT + 1, -1))
                return EPaid::Done; // refused (logged): never half-paid
            moved(AT + 1, -1);
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
        void besideFullscreen(const PHLWINDOW& w) {
            if (!w || w->m_isFloating || w->m_pinned || w->parent() || w->isModal() || (w->m_isX11 && (w->isX11OverrideRedirect() || w->x11Parent())))
                return;
            const auto WS = w->m_workspace;
            if (!WS || !DesktopOrder::ordinary(WS) || mode() == "stay")
                return;
            const auto APP = Fullscreen::controller()->getFullscreenWindow(WS);
            if (!APP || APP == w || !fullscreen(APP))
                return;
            const bool OURS = tripOf(APP) || std::ranges::any_of(g_settled, [&](const PHLWINDOWREF& r) { return r.lock() == APP; });
            if (!OURS)
                return;
            const auto MON = WS->m_monitor.lock();
            if (!MON)
                return;
            // ...and the room made for it is OWED BACK like a fullscreen
            // app's (it was never: each window opened over a fullscreen app
            // with a desktop after it left every desktop after it a place up
            // for good). Paid once that desktop is empty.
            const auto        PLAN = FullscreenBook::beside(WS->m_id, [&](WORKSPACEID id) { return freePlace(id, MON); });
            const WORKSPACEID at   = PLAN.at;
            bool              owes = false;
            if (PLAN.settle)
                return;
            if (PLAN.makeRoom) {
                DesktopStrip::finishNow();
                if (!DesktopOrder::shift(at, +1))
                    return; // refused (logged): Hyprland's own behaviour
                moved(at, +1);
                owes = true;
            }
            // A debt in that place (a trip that just ended there) is this
            // window's now: its desktop takes the place.
            if (const auto IT = std::ranges::find_if(g_debts, [&](const SDebt& d) { return placeOf(d) == at; }); IT != g_debts.end()) {
                owes = true;
                g_debts.erase(IT);
            }
            auto space = byId(at);
            if (!space)
                space = State::workspaceState()->create(at, MON->m_id);
            if (!space || space == WS) {
                if (owes)
                    g_debts.push_back({.at = at, .mon = MON, .beside = true});
                arm(1);
                return;
            }
            if (owes)
                g_debts.push_back({.space = space, .at = at, .mon = MON, .home = WS, .cls = w->m_class, .pid = w->getPID(), .beside = true});
            trace(std::format("{} opened over fullscreen {} on {}: to {}{}{}", w->m_class, APP->m_class, WS->m_id, at, owes ? " (owed back)" : "",
                              w->m_noInitialFocus ? " (unasked: not shown)" : ""));
            w->m_workspace = space;
            w->m_monitor   = MON;
            if (!w->m_noInitialFocus)
                show(MON, space);
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
        }

        // THE ONE PASS. Run on the event loop after every fullscreen change,
        // move and close -- never inside one: Hyprland takes a window out of
        // fullscreen to move it and puts it back after, so only the state once
        // the event is done means anything.
        int reconcile(void*) {
            showBesides();
            std::erase_if(g_settled, [](const PHLWINDOWREF& r) { const auto W = r.lock(); return !W || !W->m_isMapped || !fullscreen(W); });
            // Trips.
            for (auto it = g_trips.begin(); it != g_trips.end();) {
                const STrip T     = *it;
                const auto  W     = T.win.lock();
                const auto  SPACE = T.space.lock();
                if (W && W->m_isMapped && SPACE && W->m_workspace == SPACE && fullscreen(W)) {
                    ++it; // where it belongs
                    continue;
                }
                it = g_trips.erase(it);
                g_leftSpace = T.space;
                g_leftAt    = std::chrono::steady_clock::now();
                if (!W || !W->m_isMapped) {
                    trace("fullscreen app closed");
                    // Still looking at its emptied desktop: home.
                    if (const auto MON = T.mon.lock(); MON && T.home.lock() && SPACE && MON->m_activeWorkspace == SPACE && T.home.lock()->m_monitor.lock() == MON)
                        show(MON, T.home.lock());
                } else if (SPACE && W->m_workspace == SPACE)
                    goHome(T, W);
                else if (W->m_workspace) {
                    // SENT ELSEWHERE (user, 2026-11-05: "that window should be
                    // exited from fullscreen and land in the desktop with
                    // windows that it was intended to go into"): it stays.
                    trace(std::format("{} sent to {}: it stays there", W->m_class, W->m_workspace->m_id));
                    ++g_busy;
                    if (fullscreen(W) && othersOn(W->m_workspace, W) > 0)
                        Fullscreen::controller()->setFullscreenMode(W, Fullscreen::FSMODE_NONE, Fullscreen::FSMODE_NONE);
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
            if (g_timer && (watch || !g_trips.empty()))
                wl_event_source_timer_update(g_timer, watch ? 60 : 250);
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

    bool isFullscreenDesktop(const PHLWORKSPACE& ws) {
        return ws && isTripSpace(ws);
    }

    void init() {
        DesktopOrder::onRenumbered(syncPlaces);
        // Every change is looked at once it has fully happened.
        g_fsHook    = Event::bus()->m_events.window.fullscreen.listen([](PHLWINDOW w) {
            if (!g_busy)
                arm(1);
        });
        g_moveHook  = Event::bus()->m_events.window.moveToWorkspace.listen([](PHLWINDOW, PHLWORKSPACE) {
            if (!g_busy && (!g_trips.empty() || !g_debts.empty()))
                arm(1);
        });
        g_openHook  = Event::bus()->m_events.window.openEarly.listen([](PHLWINDOW w) { besideFullscreen(w); });
        g_closeHook = Event::bus()->m_events.window.close.listen([](PHLWINDOW) {
            if (!g_trips.empty() || !g_debts.empty())
                arm(1);
        });
    }

    void shutdown() {
        DesktopOrder::onRenumbered(nullptr);
        g_fsHook.reset();
        g_openHook.reset();
        g_closeHook.reset();
        g_moveHook.reset();
        if (g_timer) {
            wl_event_source_remove(g_timer);
            g_timer = nullptr;
        }
        g_trips.clear();
        g_debts.clear();
        g_settled.clear();
        g_besides.clear();
        g_travels.clear();
    }
}
