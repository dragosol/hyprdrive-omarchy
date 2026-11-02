#include "DesktopStrip.hpp"

#include "Mosaic.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/config/shared/animation/AnimationTree.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprland/src/managers/input/UnifiedWorkspaceSwipeGesture.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/state/WorkspaceState.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <optional>

namespace ScrollOverview::DesktopStrip {
    namespace {
        using namespace StripModel;
        using Clock = std::chrono::steady_clock;

        constexpr int    TICK_MS     = 16;  // the strip's clock: input timeouts and settling (frames come from the pump)
        constexpr double MARGIN      = 4.0; // past a canvas desktop's overhang (Mosaic's SLIDE_MARGIN)
        // The row settles in this share of the desktop animation's time (user,
        // 2026-11-02: "the mouse+scroll needs to be faster" -- the hand drives).
        constexpr double SETTLE_FRAC = 0.6;
        // Stiffness for that settle: a step from rest is 95% there by then, and
        // under a third of a pixel off a little after.
        constexpr double OMEGA_K = 4.7;
        // Smooth mode follows the wheel this much stiffer while it turns: it
        // only rounds off the wheel's steps, it never lags behind the hand.
        constexpr double DRAG_STIFFER = 2.5;
        constexpr double SETTLED_PX   = 0.3;

        struct SState {
            bool                         on = false;
            PHLMONITORREF                mon;
            std::vector<int>             row;
            SDrive                       drive;
            SSpring                      spring;
            SSettings                    settings;
            bool                         xax   = true;
            double                       omega = 13;
            Clock::time_point            last;
            std::vector<PHLWORKSPACEREF> forced; // desktops drawn because the row shows them
        } g;

        FRow             g_row;
        FGo              g_go;
        int              g_switching = 0;
        wl_event_source* g_timer     = nullptr;
        uint64_t         g_pumpSeq   = 0;
        // For `debug desktop`.
        int g_wheels = 0, g_gos = 0, g_takes = 0, g_lands = 0;

        uint32_t nowMs() {
            return sc<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count());
        }

        PHLWORKSPACE desktop(const PHLMONITOR& mon, int id) {
            for (const auto& REF : State::workspaceState()->workspaces()) {
                const auto WS = REF.lock();
                if (WS && !WS->m_isSpecialWorkspace && WS->m_id == id && WS->m_monitor.lock() == mon)
                    return WS;
            }
            return nullptr;
        }

        int indexOf(const std::vector<int>& row, int id) {
            const auto IT = std::ranges::find(row, id);
            return IT == row.end() ? -1 : sc<int>(IT - row.begin());
        }

        // `row` with `id` in its place (ids ascend).
        std::vector<int> withId(std::vector<int> row, int id) {
            if (indexOf(row, id) < 0)
                row.insert(std::ranges::upper_bound(row, id), id);
            return row;
        }

        // The workspaces slide's own settings (workspacesIn inherits them).
        SP<Hyprutils::Animation::SAnimationPropertyConfig> slideValues() {
            const auto CFG = ::Config::animationTree()->getAnimationPropertyConfig("workspacesIn");
            return CFG && CFG->pValues ? CFG->pValues.lock() : CFG;
        }

        // Is it a slide, and along which axis?
        std::optional<bool> slideAxisX() {
            static auto PENABLED = CConfigValue<::Config::INTEGER>("animations:enabled");
            const auto  VALS     = slideValues();
            if (!*PENABLED || !VALS || !VALS->internalEnabled)
                return std::nullopt;
            const std::string& STYLE = VALS->internalStyle;
            if (STYLE.starts_with("slidefade") || STYLE.starts_with("fade") || STYLE.starts_with("popin"))
                return std::nullopt;
            return !(STYLE.starts_with("slidevert") || STYLE.find(" top") != std::string::npos || STYLE.find(" bottom") != std::string::npos);
        }

        double omegaNow() {
            const auto   VALS  = slideValues();
            const double SPEED = VALS ? std::max(0.1F, VALS->internalSpeed) : 6.0; // deciseconds
            return OMEGA_K / std::max(0.05, SPEED * 0.1 * SETTLE_FRAC);
        }

        // ---- the row's geometry: where each desktop sits along it, in px
        struct SGeom {
            std::vector<PHLWORKSPACE> ws;
            std::vector<double>       x, lo, hi; // place; overhang past the near (left/top) and far edge
            double                    span = 0, size = 0;
        };

        SGeom geometry(const PHLMONITOR& mon) {
            static auto PGAP = CConfigValue<::Config::INTEGER>("general:gaps_workspaces");
            SGeom       G;
            const auto  N = g.row.size();
            G.size        = g.xax ? mon->m_size.x : mon->m_size.y;
            G.span        = G.size + sc<double>(*PGAP);
            G.ws.resize(N);
            G.x.assign(N, 0.0);
            G.lo.assign(N, 0.0);
            G.hi.assign(N, 0.0);
            for (size_t i = 0; i < N; ++i) {
                G.ws[i] = desktop(mon, g.row[i]);
                if (!G.ws[i])
                    continue; // nothing there yet: an empty desktop, just the wallpaper
                G.lo[i] = Mosaic::slideOverhang(G.ws[i], mon, -1, g.xax);
                G.hi[i] = Mosaic::slideOverhang(G.ws[i], mon, +1, g.xax);
            }
            // Neighbours a screen (and the workspace gap) apart, plus whatever
            // hangs past the edges they share.
            for (size_t i = 1; i < N; ++i) {
                const double OVER = G.hi[i - 1] + G.lo[i];
                G.x[i]            = G.x[i - 1] + G.span + OVER + (OVER >= 1 ? MARGIN : 0.0);
            }
            return G;
        }

        // Row position (index units) -> place (px); past the ends a screen per desktop.
        double placeAt(const SGeom& G, double pos) {
            const int N = sc<int>(G.x.size());
            if (N == 0)
                return 0;
            if (pos <= 0)
                return pos * G.span;
            if (pos >= N - 1)
                return G.x[N - 1] + (pos - (N - 1)) * G.span;
            const int    I = sc<int>(std::floor(pos));
            const double F = pos - I;
            return G.x[I] + F * (G.x[I + 1] - G.x[I]);
        }

        double posAt(const SGeom& G, double place) {
            const int N = sc<int>(G.x.size());
            if (N == 0 || G.span < 1)
                return 0;
            if (place <= 0)
                return place / G.span;
            if (place >= G.x[N - 1])
                return (N - 1) + (place - G.x[N - 1]) / G.span;
            for (int i = 0; i + 1 < N; ++i)
                if (place <= G.x[i + 1])
                    return i + (place - G.x[i]) / std::max(1.0, G.x[i + 1] - G.x[i]);
            return N - 1;
        }

        // ---- on screen
        // Every desktop of the row at its place for the spring's position; the
        // ones in view drawn although Hyprland does not show them.
        void apply() {
            const auto MON = g.mon.lock();
            if (!MON)
                return;
            const auto                   G      = geometry(MON);
            const double                 XP     = placeAt(G, g.spring.pos);
            const auto                   ACTIVE = MON->m_activeWorkspace;
            std::vector<PHLWORKSPACEREF> forced;
            for (size_t i = 0; i < G.ws.size(); ++i) {
                const auto& WS = G.ws[i];
                if (!WS || !WS->m_renderOffset)
                    continue;
                const double   OFF = G.x[i] - XP;
                const Vector2D V   = g.xax ? Vector2D{OFF, 0.0} : Vector2D{0.0, OFF};
                if (WS->m_renderOffset->isBeingAnimated() || WS->m_renderOffset->value().distance(V) > 0.001)
                    WS->m_renderOffset->setValueAndWarp(V);
                if (WS == ACTIVE)
                    continue;
                if (OFF + G.size + G.hi[i] > 0 && OFF - G.lo[i] < G.size) {
                    WS->m_forceRendering = true;
                    if (WS->m_alpha && (WS->m_alpha->isBeingAnimated() || WS->m_alpha->value() != 1.F))
                        WS->m_alpha->setValueAndWarp(1.F);
                    forced.emplace_back(WS);
                }
            }
            for (const auto& REF : g.forced) {
                const auto WS = REF.lock();
                if (WS && std::ranges::find_if(forced, [&](const auto& f) { return f.lock() == WS; }) == forced.end())
                    WS->m_forceRendering = false;
            }
            g.forced = std::move(forced);
        }

        void advance(Clock::time_point now) {
            const double DT = std::clamp(std::chrono::duration<double>(now - g.last).count(), 0.0, 0.25);
            g.last          = now;
            const double W  = g.settings.smooth && g.drive.dragging ? g.omega * DRAG_STIFFER : g.omega;
            g.spring.step(g.drive.goal(g.settings), W, DT);
        }

        void end() {
            for (const auto& REF : g.forced)
                if (const auto WS = REF.lock())
                    WS->m_forceRendering = false;
            g.forced.clear();
            g.on = false;
            g.mon.reset();
            g.row.clear();
            if (g_timer)
                wl_event_source_timer_update(g_timer, 0);
        }

        // At rest where it was going: the shown desktop home, the rest away.
        void land() {
            const auto MON = g.mon.lock();
            if (MON) {
                if (!g.row.empty())
                    g.spring = {.pos = sc<double>(std::clamp(g.drive.target, 0, sc<int>(g.row.size()) - 1)), .vel = 0};
                apply();
                // Shown something else meanwhile (nobody told the strip): home.
                if (const auto ACTIVE = MON->m_activeWorkspace; ACTIVE && ACTIVE->m_renderOffset && ACTIVE->m_renderOffset->value().size() > 0.001)
                    ACTIVE->m_renderOffset->setValueAndWarp(Vector2D{});
                g_pHyprRenderer->damageMonitor(MON);
            }
            ++g_lands;
            end();
        }

        // The strip's own `workspace N`.
        void go(int idx) {
            const auto MON = g.mon.lock();
            if (!MON || !g_go || idx < 0 || idx >= sc<int>(g.row.size()))
                return;
            if (MON->m_activeWorkspace && MON->m_activeWorkspace->m_id == g.row[idx])
                return;
            ++g_switching;
            g_go(MON, g.row[idx]);
            --g_switching;
            ++g_gos;
            apply(); // the desktop just shown, at its place for this very frame
        }

        bool settled(const PHLMONITOR& mon) {
            const double EPS = SETTLED_PX / std::max(1.0, (g.xax ? mon->m_size.x : mon->m_size.y));
            return !g.drive.holding(g.settings) && g.spring.settled(g.drive.goal(g.settings), EPS);
        }

        int onTick(void*) {
            if (!g.on)
                return 0;
            const auto MON = g.mon.lock();
            if (!MON || !MON->m_activeWorkspace) {
                end();
                return 0;
            }
            // Something else showed another desktop without telling the strip
            // (take()): it lets go, that one home.
            if (g.row.empty() || MON->m_activeWorkspace->m_id != g.row[std::clamp(g.drive.target, 0, sc<int>(g.row.size()) - 1)]) {
                land();
                return 0;
            }
            advance(Clock::now());
            if (g.drive.tick(nowMs(), g.settings))
                go(g.drive.target);
            if (settled(MON)) {
                land();
                return 0;
            }
            apply();
            // Frames come from the pump (preRender); this restarts them after
            // a pause in the goal (a peek let go).
            const double EPS = SETTLED_PX / std::max(1.0, (g.xax ? MON->m_size.x : MON->m_size.y));
            if (!g.spring.settled(g.drive.goal(g.settings), EPS))
                g_pHyprRenderer->damageMonitor(MON);
            wl_event_source_timer_update(g_timer, TICK_MS);
            return 0;
        }

        void arm() {
            if (!g_timer && g_pCompositor)
                g_timer = wl_event_loop_add_timer(g_pCompositor->m_wlEventLoop, onTick, nullptr);
            if (g_timer)
                wl_event_source_timer_update(g_timer, TICK_MS);
        }

        // One frame after another while the row moves: damage once this frame
        // is out, never from inside it.
        void pump() {
            if (g_pumpSeq || !g_pEventLoopManager)
                return;
            g_pumpSeq = g_pEventLoopManager->doLater([] {
                g_pumpSeq = 0;
                if (!g.on)
                    return;
                if (const auto MON = g.mon.lock())
                    g_pHyprRenderer->damageMonitor(MON);
            });
        }

        // The strip takes the desktops of `mon`, `current` shown, the row at
        // `pos` (default: where the shown desktop is on screen now).
        bool start(const PHLMONITOR& mon, std::vector<int> row, int current, std::optional<double> pos) {
            const auto AXIS = slideAxisX();
            const int  IDX  = indexOf(row, current);
            if (!AXIS || IDX < 0)
                return false;
            // A slide of Hyprland's still running here: it ends where it is, the
            // row takes over from there.
            for (const auto& REF : State::workspaceState()->workspaces()) {
                const auto WS = REF.lock();
                if (WS && !WS->m_isSpecialWorkspace && WS->m_monitor.lock() == mon && WS->m_renderOffset && WS->m_renderOffset->isBeingAnimated())
                    WS->m_renderOffset->setValueAndWarp(WS->m_renderOffset->value());
            }
            g.on       = true;
            g.mon      = mon;
            g.row      = std::move(row);
            g.xax      = *AXIS;
            g.omega    = omegaNow();
            g.last     = Clock::now();
            g.settings = {};
            g.drive.reset(sc<int>(g.row.size()), IDX);
            g.spring = {.pos = sc<double>(IDX), .vel = 0};
            if (pos)
                g.spring.pos = *pos;
            else if (const auto ACTIVE = mon->m_activeWorkspace; ACTIVE && ACTIVE->m_renderOffset) {
                const auto   V   = ACTIVE->m_renderOffset->value();
                const double OFF = g.xax ? V.x : V.y;
                if (std::abs(OFF) >= 0.5) {
                    const auto G = geometry(mon);
                    g.spring.pos = posAt(G, G.x[IDX] - OFF);
                }
            }
            arm();
            return true;
        }
    }

    // ---------------------------------------------------------- stacking --
    // THE DESKTOP SWITCHED TO IS ON TOP (user, 2026-11-02: "previous mosaic
    // infinite canvas desktop's windows should always be under the desktop that
    // was switched to"; a terminal of the desktop left behind, carried off by
    // Mosaic's slide tail, crossed the new desktop's windows OVER them for two
    // frames). Hyprland draws every window it shows in one pass, in the global
    // window order, whichever desktop it is on -- so while a switch shows two
    // desktops, a window of the old one could cover the new one wherever they
    // met (a tail, an overhang, a zoom). While a monitor shows more than one
    // desktop, its pass sees the window list ordered desktop by desktop, bottom
    // to top; the list is put back the moment the pass returns.
    //
    // AND IT NEVER FLIPS (user, 2026-11-02: "if I decide not to switch to next
    // desktop, next desktop's windows go below the current desktop's, on a peek
    // of the next desktop which is sudden"). A desktop's place is set when it
    // comes ON SCREEN -- on top if it is the destination then (the one shown, or
    // the one a live swipe's fingers are bringing in), under the others if not
    // -- and kept until it leaves the screen. A swipe let go without switching,
    // or a scroll turned back, leaves the peeked desktop on top while it slides
    // away; a real switch has its destination on top from its first frame.
    namespace {
        CFunctionHook* g_stackHook = nullptr;
        using origRenderWorkspaceWindows = void (*)(void*, PHLMONITOR, PHLWORKSPACE, const Time::steady_tp&);

        struct SStack {
            PHLMONITORREF                mon;
            std::vector<PHLWORKSPACEREF> order; // bottom first
        };
        std::vector<SStack> g_stacks;

        bool drawn(const PHLWORKSPACE& ws) {
            return ws->isVisible() || ws->m_forceRendering || (ws->m_renderOffset && ws->m_renderOffset->isBeingAnimated()) ||
                (ws->m_alpha && ws->m_alpha->isBeingAnimated());
        }

        // Is any of `ws` on the screen at its offset -- its windows hanging past
        // its edges included?
        bool onScreen(const PHLWORKSPACE& ws, const PHLMONITOR& mon) {
            const Vector2D V   = ws->m_renderOffset ? ws->m_renderOffset->value() : Vector2D{};
            const bool     XAX = std::abs(V.x) >= std::abs(V.y);
            const double   D = XAX ? V.x : V.y, O = XAX ? V.y : V.x;
            const double   SIZE = XAX ? mon->m_size.x : mon->m_size.y, ACROSS = XAX ? mon->m_size.y : mon->m_size.x;
            if (std::abs(O) >= ACROSS)
                return false;
            return D - Mosaic::slideOverhang(ws, mon, -1, XAX) < SIZE && D + SIZE + Mosaic::slideOverhang(ws, mon, +1, XAX) > 0;
        }

        void hkRenderWorkspaceWindows(void* self, PHLMONITOR mon, PHLWORKSPACE ws, const Time::steady_tp& time) {
            const auto ORIGINAL = rc<origRenderWorkspaceWindows>(g_stackHook->m_original);
            if (!mon || !ws || ws->m_isSpecialWorkspace) {
                ORIGINAL(self, mon, ws, time);
                return;
            }
            std::erase_if(g_stacks, [](const SStack& s) { return !s.mon.lock(); });
            auto IT = std::ranges::find_if(g_stacks, [&mon](const SStack& s) { return s.mon.lock() == mon; });

            // The desktops this monitor draws, and the ones of them on screen.
            const auto                ACTIVE = mon->m_activeWorkspace;
            std::vector<PHLWORKSPACE> shown, visible;
            for (const auto& REF : State::workspaceState()->workspaces()) {
                const auto WS = REF.lock();
                if (!WS || WS->m_isSpecialWorkspace || WS->m_monitor.lock() != mon || !drawn(WS))
                    continue;
                shown.push_back(WS);
                if (WS == ACTIVE || onScreen(WS, mon))
                    visible.push_back(WS);
            }
            if (shown.size() < 2) {
                if (IT != g_stacks.end())
                    g_stacks.erase(IT); // one desktop again: the next switch starts afresh
                ORIGINAL(self, mon, ws, time);
                return;
            }
            if (IT == g_stacks.end())
                IT = g_stacks.insert(g_stacks.end(), SStack{.mon = mon, .order = {}});
            auto& ORDER = IT->order;

            // The destination now: the one shown -- or, under a live swipe
            // (Hyprland forces the desktop it began on drawn), the neighbour the
            // fingers are bringing in, the one most on screen.
            PHLWORKSPACE DEST = ACTIVE;
            if (ACTIVE && ACTIVE->m_forceRendering && g_pUnifiedWorkspaceSwipe && g_pUnifiedWorkspaceSwipe->isGestureInProgress()) {
                PHLWORKSPACE incoming;
                for (const auto& WS : visible)
                    if (WS != ACTIVE && WS->m_renderOffset && (!incoming || WS->m_renderOffset->value().size() < incoming->m_renderOffset->value().size()))
                        incoming = WS;
                if (incoming)
                    DEST = incoming;
            }

            // Off screen: its place is forgotten. Newly on screen: top if it is
            // the destination, else bottom. Everything placed keeps its place.
            std::erase_if(ORDER, [&visible](const PHLWORKSPACEREF& r) {
                const auto WS = r.lock();
                return !WS || std::ranges::find(visible, WS) == visible.end();
            });
            for (const auto& WS : visible) {
                if (std::ranges::find_if(ORDER, [&WS](const PHLWORKSPACEREF& r) { return r.lock() == WS; }) != ORDER.end())
                    continue;
                if (WS == DEST)
                    ORDER.emplace_back(WS);
                else
                    ORDER.insert(ORDER.begin(), PHLWORKSPACEREF{WS});
            }
            if (ORDER.size() < 2) {
                ORIGINAL(self, mon, ws, time);
                return;
            }

            const auto RANK = [&ORDER](const PHLWINDOW& w) -> int {
                if (!w || !w->m_workspace)
                    return -1;
                for (size_t i = 0; i < ORDER.size(); ++i)
                    if (ORDER[i].lock() == w->m_workspace)
                        return sc<int>(i);
                return -1; // a desktop not on screen: below them all
            };
            // CWindowState's list is its own (non-const) member; only its getter is const.
            auto&      list  = const_cast<std::vector<PHLWINDOW>&>(Desktop::windowState()->windows());
            const auto SAVED = list;
            std::ranges::stable_sort(list, {}, RANK);
            ORIGINAL(self, mon, ws, time);
            if (list.size() == SAVED.size())
                list = SAVED;
        }
    }

    // ------------------------------------------------- the swipe's release --
    // A QUICK WAY BACK (user, 2026-11-02: "depending how I swipe sometimes the
    // windows from the next door desktop will move back super slowly instead of
    // quickly with the motion blur"). Hyprland sends a let-go swipe's desktops
    // home on the workspaces animation's fixed time, however short the way and
    // however fast the fingers were going: a small peek let go crawled back for
    // all of it. Each release slide now takes the time its distance calls for
    // (a square-root scale: a short way is quick, a whole screen as before) and,
    // with the fingers still moving its way, starts at their speed. Hyprland's
    // own curve and places are kept, so nothing jumps.
    namespace {
        struct SSwipeSamples {
            PHLMONITORREF     mon;
            double            o[2] = {0, 0}; // the swiped desktop's offset along the slide
            Clock::time_point t[2];
            int               n = 0;
        } g_swipe;
        // Animated variables hold their config weakly: these keep the release's alive.
        std::vector<SP<Hyprutils::Animation::SAnimationPropertyConfig>> g_releaseCfgs;
        constexpr double RELEASE_MIN_FRAC = 0.35; // the shortest way back takes this share of the full time, at least
        constexpr double RELEASE_MIN_S    = 0.08;
        constexpr double EASE_SLOPE       = 4.0; // an ease-out's starting speed, in distances per duration

        // render.pre while the fingers are down: where the swiped desktop is.
        void sampleSwipe(const PHLMONITOR& mon) {
            const auto ACTIVE = mon->m_activeWorkspace;
            if (!ACTIVE || !ACTIVE->m_forceRendering || !ACTIVE->m_renderOffset || !g_pUnifiedWorkspaceSwipe || !g_pUnifiedWorkspaceSwipe->isGestureInProgress())
                return;
            if (g_swipe.mon.lock() != mon)
                g_swipe = {.mon = mon};
            const Vector2D V  = ACTIVE->m_renderOffset->value();
            const bool     XA = slideAxisX().value_or(true);
            g_swipe.o[0]      = g_swipe.o[1];
            g_swipe.t[0]      = g_swipe.t[1];
            g_swipe.o[1]      = XA ? V.x : V.y;
            g_swipe.t[1]      = Clock::now();
            ++g_swipe.n;
        }

        SP<Hyprutils::Animation::SAnimationPropertyConfig> timedConfig(const SP<Hyprutils::Animation::SAnimationPropertyConfig>& base, double seconds) {
            const auto VALS = base && base->pValues ? base->pValues.lock() : base;
            if (!VALS)
                return nullptr;
            auto CFG             = makeShared<Hyprutils::Animation::SAnimationPropertyConfig>();
            CFG->overridden      = true;
            CFG->internalBezier  = VALS->internalBezier;
            CFG->internalStyle   = VALS->internalStyle;
            CFG->internalEnabled = VALS->internalEnabled;
            CFG->internalSpeed   = sc<float>(seconds * 10.0); // deciseconds
            CFG->pValues         = CFG;
            return CFG;
        }
    }

    void quickenSwipeRelease(const PHLMONITOR& mon) {
        if (!mon)
            return;
        // The fingers' speed as they let go: the last two frames, if recent.
        double v = 0;
        if (g_swipe.mon.lock() == mon && g_swipe.n >= 2) {
            const double DT  = std::chrono::duration<double>(g_swipe.t[1] - g_swipe.t[0]).count();
            const double AGE = std::chrono::duration<double>(Clock::now() - g_swipe.t[1]).count();
            if (DT > 0.001 && AGE < 0.06)
                v = (g_swipe.o[1] - g_swipe.o[0]) / DT;
        }
        g_swipe = {};
        g_releaseCfgs.clear();
        static auto PGAP = CConfigValue<::Config::INTEGER>("general:gaps_workspaces");
        const bool  XA   = slideAxisX().value_or(true);
        const double SPAN = (XA ? mon->m_size.x : mon->m_size.y) + sc<double>(*PGAP);
        for (const auto& REF : State::workspaceState()->workspaces()) {
            const auto WS = REF.lock();
            if (!WS || WS->m_isSpecialWorkspace || WS->m_monitor.lock() != mon || !WS->m_renderOffset || !WS->m_renderOffset->isBeingAnimated())
                continue;
            const auto   BASE = WS->m_renderOffset->getConfig().lock();
            const auto   VALS = BASE && BASE->pValues ? BASE->pValues.lock() : BASE;
            const double T0   = (VALS ? std::max(0.1F, VALS->internalSpeed) : 4.0) * 0.1;
            const Vector2D G = WS->m_renderOffset->goal(), V = WS->m_renderOffset->value();
            const double   TO = XA ? G.x - V.x : G.y - V.y, D = std::abs(TO);
            if (D < 1 || SPAN < 1)
                continue;
            double t = T0 * std::clamp(std::sqrt(D / SPAN), RELEASE_MIN_FRAC, 1.0);
            if (v * TO > 0) // the fingers were going its way: on at their speed
                t = std::min(t, EASE_SLOPE * D / std::abs(v));
            t = std::clamp(t, RELEASE_MIN_S, T0);
            if (t >= T0 * 0.98)
                continue;
            if (auto CFG = timedConfig(BASE, t)) {
                WS->m_renderOffset->setConfig(CFG);
                g_releaseCfgs.push_back(CFG);
            }
        }
    }

    void init(FRow row, FGo go) {
        g_row = std::move(row);
        g_go  = std::move(go);
        // Not fatal when missing: desktops then stack in Hyprland's order.
        for (const auto& FN : HyprlandAPI::findFunctionsByName(SCROLLOVERVIEW_HANDLE, "renderWorkspaceWindows")) {
            if (FN.demangled.find("renderWorkspaceWindows(") == std::string::npos)
                continue; // not renderWorkspaceWindowsFullscreen
            g_stackHook = HyprlandAPI::createFunctionHook(SCROLLOVERVIEW_HANDLE, FN.address, rc<void*>(hkRenderWorkspaceWindows));
            if (g_stackHook && !g_stackHook->hook())
                g_stackHook = nullptr;
            break;
        }
    }

    void shutdown() {
        if (g.on)
            land();
        if (g_stackHook) {
            // Unhooked here; Hyprland frees it with the plugin's other hooks.
            g_stackHook->unhook();
            g_stackHook = nullptr;
        }
        g_stacks.clear();
        g_releaseCfgs.clear();
        g_swipe = {};
        if (g_timer) {
            wl_event_source_remove(g_timer);
            g_timer = nullptr;
        }
        if (g_pumpSeq && g_pEventLoopManager)
            g_pEventLoopManager->removeDoLater(g_pumpSeq);
        g_pumpSeq   = 0;
        g_switching = 0;
        g_row       = nullptr;
        g_go        = nullptr;
    }

    bool available(const PHLMONITOR& mon) {
        return mon && slideAxisX().has_value();
    }

    namespace {
        // NO SLIDE TO DRAW (animations off, or the workspaces animation a
        // fade): the same row, ticks and stickiness, each desktop shown at once.
        struct SInstant {
            PHLMONITORREF    mon;
            std::vector<int> row;
            SDrive           drive;
        } g_instant;

        bool instantWheel(const PHLMONITOR& mon, int v120, SSettings settings) {
            const int      CURRENT = sc<int>(mon->m_activeWorkspace->m_id);
            const uint32_t NOW     = nowMs();
            auto&          I       = g_instant;
            // Smooth has nothing to follow without a slide: two notches a desktop.
            if (settings.smooth) {
                settings.smooth = false;
                settings.ticks  = SMOOTH_V120 / NOTCH;
            }
            settings.bounce = false;
            const bool FRESH = I.mon.lock() != mon || !I.drive.seen || NOW < I.drive.lastMs || NOW - I.drive.lastMs >= BURST_MS || I.drive.target < 0 ||
                I.drive.target >= sc<int>(I.row.size()) || I.row[I.drive.target] != CURRENT;
            if (FRESH) {
                I.mon = mon;
                I.row = g_row(mon, CURRENT);
                const int IDX = indexOf(I.row, CURRENT);
                if (IDX < 0)
                    return false;
                I.drive.reset(sc<int>(I.row.size()), IDX);
            }
            ++g_wheels;
            if (I.drive.feed(v120, NOW, settings) && g_go) {
                ++g_switching;
                g_go(mon, I.row[I.drive.target]);
                --g_switching;
                ++g_gos;
            }
            return true;
        }
    }

    bool onWheel(const PHLMONITOR& mon, int v120, const SSettings& settings) {
        if (!mon || !mon->m_activeWorkspace || !g_row)
            return false;
        if (!g.on && !available(mon))
            return instantWheel(mon, v120, settings);
        if (g.on && g.mon.lock() != mon)
            land();
        if (!g.on) {
            const int CURRENT = sc<int>(mon->m_activeWorkspace->m_id);
            if (!start(mon, g_row(mon, CURRENT), CURRENT, std::nullopt))
                return false;
        }
        advance(Clock::now());
        g.settings = settings;
        ++g_wheels;
        if (g.drive.feed(v120, nowMs(), g.settings))
            go(g.drive.target);
        if (g.drive.kick) {
            // Into an end: thrown a little past it, back on the spring.
            g.drive.kick      = false;
            const int    DIR  = g.drive.kickDir;
            const double KICK = kickFor(BOUNCE, g.omega);
            g.spring.vel      = DIR * std::max(DIR * g.spring.vel, KICK);
        }
        apply();
        if (const auto MON = g.mon.lock())
            g_pHyprRenderer->damageMonitor(MON);
        arm();
        return true;
    }

    bool claims(const PHLMONITOR& mon, int from, int to, bool jumpThrough) {
        if (g_switching)
            return true;
        if (!mon || from == to || !g_row)
            return false;
        if (g.on && g.mon.lock() == mon)
            return true;
        if (!jumpThrough || from < 1 || to < 1 || !available(mon))
            return false;
        const auto ROW = withId(g_row(mon, to), from);
        const int  A = indexOf(ROW, from), B = indexOf(ROW, to);
        return A >= 0 && B >= 0 && std::abs(B - A) > 1;
    }

    void take(const PHLMONITOR& mon, int from, int to, const Vector2D& fromOffset, bool jumpThrough) {
        if (g_switching || !claims(mon, from, to, jumpThrough))
            return;
        ++g_takes;
        if (g.on && g.mon.lock() == mon) {
            // Moving already: on toward this one, from where the row is.
            advance(Clock::now());
            if (indexOf(g.row, to) < 0) {
                const int    I    = std::clamp(sc<int>(std::floor(g.spring.pos)), 0, sc<int>(g.row.size()) - 1);
                const int    ID   = g.row[I];
                const double FRAC = g.spring.pos - I;
                g.row             = withId(std::move(g.row), to);
                g.spring.pos      = indexOf(g.row, ID) + FRAC;
            }
            g.drive.reset(sc<int>(g.row.size()), indexOf(g.row, to));
            arm();
            return;
        }
        // Through the ones between, from where `from` was on screen.
        auto ROW = withId(g_row(mon, to), from);
        if (!start(mon, ROW, to, std::nullopt))
            return;
        const auto   G   = geometry(mon);
        const int    A   = indexOf(g.row, from);
        const double OFF = g.xax ? fromOffset.x : fromOffset.y;
        g.spring.pos     = posAt(G, G.x[A] - OFF);
        apply();
        g_pHyprRenderer->damageMonitor(mon);
    }

    bool driving(const PHLMONITOR& mon) {
        return g.on && mon && g.mon.lock() == mon;
    }

    bool drives(const PHLWORKSPACE& ws) {
        return ws && !ws->m_isSpecialWorkspace && driving(ws->m_monitor.lock());
    }

    bool switching() {
        return g_switching > 0;
    }

    void finishNow() {
        if (g.on)
            land();
    }

    bool catchForSwipe(const PHLMONITOR& mon) {
        if (!driving(mon) || g.row.empty())
            return false;
        advance(Clock::now());
        const int NEAR = std::clamp(sc<int>(std::lround(g.spring.pos)), 0, sc<int>(g.row.size()) - 1);
        g.drive.reset(sc<int>(g.row.size()), NEAR);
        go(NEAR);
        apply();
        return true;
    }

    void letGo(const PHLMONITOR& mon) {
        if (driving(mon))
            end();
    }

    void preRender(const PHLMONITOR& mon) {
        if (mon)
            sampleSwipe(mon);
        if (!driving(mon))
            return;
        advance(Clock::now());
        apply();
        const double EPS = SETTLED_PX / std::max(1.0, (g.xax ? mon->m_size.x : mon->m_size.y));
        if (!g.spring.settled(g.drive.goal(g.settings), EPS))
            pump();
    }

    std::string debugReport() {
        std::string row;
        for (const int ID : g.row)
            row += std::format("{}{}", row.empty() ? "" : ",", ID);
        return std::format("strip: {}{} row [{}] target {} pos {:.3f} vel {:.2f} goal {:.3f} omega {:.1f} | wheel {} switches {} taken {} landed {}", g.on ? "moving" : "idle", g_stackHook ? "" : " (no stacking hook)", row,
                           g.on && !g.row.empty() ? g.row[std::clamp(g.drive.target, 0, sc<int>(g.row.size()) - 1)] : 0, g.spring.pos, g.spring.vel,
                           g.on ? g.drive.goal(g.settings) : 0.0, g.omega, g_wheels, g_gos, g_takes, g_lands);
    }
}
