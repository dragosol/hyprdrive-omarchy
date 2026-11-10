// Z19 (FlightController.cpp): this block must be FIRST. m_Value is private in
// hyprutils and m_realPosition protected in Hyprland's window, and show desktop
// moves only the DRAWN position -- a goal change would configure every client
// on the desktop. Every std header goes in clean first; then everything that
// defines those classes is included under private=public, so this TU's view of
// them is the open one from their first inclusion on.
#include <sstream>
#include <functional>
#include <memory>
#include <chrono>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>
#include <any>
#include <cstdio>
#include <format>
#include <optional>
#include <ranges>
#define private public
#define protected public
#include <hyprutils/animation/AnimatedVariable.hpp>
#include "ShowDesktop.hpp"
#include "ShowDesktopMath.hpp"
#include "Config.hpp"
#include "DebugPaths.hpp"
#include "IOverview.hpp"
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/render/Renderer.hpp>
#undef protected
#undef private

namespace ScrollOverview::ShowDesktop {
    namespace {
        using namespace ShowDesktopMath;

        // Past the edge by this much more, so a window's shadow or its blur's
        // spill goes out of view with it.
        constexpr double MARGIN = 48;

        struct SWin {
            PHLWINDOWREF window;
            Vector2D     out;            // its way out of view, from its place
            bool         hidden = false; // hidden by us, so shown again by us
        };

        struct SState {
            PHLWORKSPACEREF   workspace;
            PHLMONITORREF     monitor;
            std::vector<SWin> wins;
            PHLWINDOWREF      focused;        // the focus when the desktop was shown
            bool              out     = true; // heading out of view, or back
            bool              resting = false; // out of view, hidden, nothing to draw
            SSpring           spring;
            Time::steady_tp   last;
        };

        std::optional<SState> g_state;
        CHyprSignalListener   g_openHook, g_focusHook, g_desktopHook, g_monitorHook;

        // THE TICK (main.cpp, "THE LETTER TICK"): a frame asked for from inside
        // the render pass is absorbed by the one being drawn, so with nothing
        // else on the screen moving the slide stalled part way (rig,
        // 2026-11-10: on an idle desktop the windows were still on their way
        // 1.4 s after the key). An event-loop timer books the next frame
        // instead, every half 60 Hz frame while the windows move, and lapses
        // by itself when they rest.
        wl_event_source* g_tick   = nullptr;
        constexpr int    TICK_MS  = 8;

        int tick(void*) {
            if (!g_state || g_state->resting)
                return 0;
            if (const auto MON = g_state->monitor.lock()) {
                g_pHyprRenderer->damageMonitor(MON);
                MON->scheduleFrame(Aquamarine::IOutput::AQ_SCHEDULE_ANIMATION);
            }
            if (g_tick)
                wl_event_source_timer_update(g_tick, TICK_MS);
            return 0;
        }

        // Armed by what sets the windows moving -- the key, a window opening,
        // one being picked -- all of it off the render pass.
        void armTick() {
            if (!g_pCompositor)
                return;
            if (!g_tick)
                g_tick = wl_event_loop_add_timer(g_pCompositor->m_wlEventLoop, tick, nullptr);
            if (g_tick)
                wl_event_source_timer_update(g_tick, TICK_MS);
        }

        void trace(const std::string& s) {
            if (!DebugPaths::flagOn("hyprdrive-mosaic.on"))
                return;
            if (FILE* const F = DebugPaths::openLog("hyprdrive-mosaic.log")) {
                std::fprintf(F, "showdesktop %s\n", s.c_str());
                std::fclose(F);
            }
        }

        bool ours(const PHLWINDOW& w) {
            if (!g_state || !w)
                return false;
            return std::ranges::any_of(g_state->wins, [&](const SWin& e) { return e.window.lock() == w; });
        }

        void damage() {
            if (const auto MON = g_state ? g_state->monitor.lock() : PHLMONITOR{})
                g_pHyprRenderer->damageMonitor(MON);
        }

        // Where `e`'s window is drawn at progress `x`: its place (the goal -- a
        // window still gliding into its tile counts from where it is going)
        // plus that much of its way out.
        void place(const SWin& e, double x) {
            const auto W = e.window.lock();
            if (!W || !W->m_isMapped || !W->m_realPosition)
                return;
            W->m_realPosition->m_Value = W->m_realPosition->goal() + e.out * x;
        }

        // The focus they had, given back -- unless something else was chosen
        // meanwhile (a new window, one picked in the dock), and only while that
        // desktop is on its screen: focusing a window on a desktop you have
        // just left takes you back to it.
        void giveFocusBack(const SState& s) {
            const auto F   = s.focused.lock();
            const auto WS  = s.workspace.lock();
            const auto MON = s.monitor.lock();
            if (!F || !WS || !MON || MON->m_activeWorkspace != WS)
                return;
            if (F->m_isMapped && !F->isHidden() && F->m_workspace == WS && !Desktop::focusState()->window())
                Desktop::focusState()->fullWindowFocus(F, Desktop::FOCUS_REASON_OTHER);
        }

        // Every window exactly in its place and shown: the end of the way back,
        // or at once when there is nothing to animate (the desktop off screen,
        // the overview opening, the plugin unloading).
        void finish(const char* why) {
            if (!g_state)
                return;
            SState s = std::move(*g_state);
            g_state.reset();
            for (const auto& E : s.wins) {
                const auto W = E.window.lock();
                if (!W)
                    continue;
                if (E.hidden)
                    W->setHidden(false);
                if (W->m_realPosition)
                    W->m_realPosition->m_Value = W->m_realPosition->goal();
            }
            giveFocusBack(s);
            if (const auto MON = s.monitor.lock())
                g_pHyprRenderer->damageMonitor(MON);
            trace(std::format("back in place ({})", why));
        }

        // Out of view: hidden, so not drawn and not hit, their clients idle.
        // The keyboard lets go of whichever had it (setHidden only drops a copy
        // of the focus), and the pointer, over the desktop now, is re-aimed.
        void hideAll() {
            auto& S = *g_state;
            const auto FOCUSED = Desktop::focusState()->window();
            for (auto& E : S.wins) {
                const auto W = E.window.lock();
                if (!W || E.hidden || !W->m_isMapped)
                    continue;
                W->setHidden(true);
                E.hidden = true;
            }
            if (FOCUSED && ours(FOCUSED))
                Desktop::focusState()->fullWindowFocus(nullptr, Desktop::FOCUS_REASON_OTHER);
            g_pInputManager->simulateMouseMovement();
            S.resting = true;
            trace(std::format("out of view: {} windows hidden", S.wins.size()));
        }

        // Turned round toward their places, from wherever they are: shown again
        // there (still out of view), and the focus back at once, so typing
        // reaches it while it slides in.
        void comeBack(const char* why) {
            if (!g_state || !g_state->out)
                return;
            auto& S   = *g_state;
            S.out     = false;
            S.resting = false;
            for (auto& E : S.wins) {
                place(E, S.spring.x);
                if (E.hidden) {
                    if (const auto W = E.window.lock())
                        W->setHidden(false);
                    E.hidden = false;
                }
            }
            giveFocusBack(S);
            S.last = Time::steadyNow();
            damage();
            armTick();
            trace(std::format("coming back from {:.3f} ({})", S.spring.x, why));
        }

        void begin(const PHLMONITOR& mon) {
            const auto WS = mon ? mon->m_activeWorkspace : PHLWORKSPACE{};
            if (!WS)
                return;
            SState      s;
            const SRect SCREEN{mon->m_position.x, mon->m_position.y, mon->m_size.x, mon->m_size.y};
            for (const auto& W : Desktop::windowState()->windows()) {
                // A pinned window is on every desktop on purpose; a hidden one
                // (a group's other tabs) is not on view to begin with.
                if (!W || !W->m_isMapped || W->isHidden() || W->m_pinned || W->m_workspace != WS || !W->m_realPosition)
                    continue;
                // Its whole drawn box (border, decorations), at its place.
                const CBox     B     = W->getFullWindowBoundingBox();
                const Vector2D SHIFT = W->m_realPosition->goal() - W->m_realPosition->value();
                const SVec     D     = outOf({B.x + SHIFT.x, B.y + SHIFT.y, B.w, B.h}, SCREEN, MARGIN);
                SWin e;
                e.window = W;
                e.out    = Vector2D{D.x, D.y};
                s.wins.push_back(e);
            }
            if (s.wins.empty())
                return; // nothing between you and the desktop already
            s.workspace = WS;
            s.monitor   = mon;
            s.focused   = Desktop::focusState()->window();
            s.last      = Time::steadyNow();
            g_state     = std::move(s);
            damage();
            armTick();
            trace(std::format("showing the desktop of {}: {} windows", WS->m_name, g_state->wins.size()));
        }

        // Focus that is somebody's choice brings the windows back; focus that
        // merely follows the pointer, or falls somewhere because a window left
        // or a desktop changed, does not (a window sliding out under a still
        // pointer must not call them all back).
        bool deliberate(Desktop::eFocusReason r) {
            switch (r) {
                case Desktop::FOCUS_REASON_UNKNOWN:
                case Desktop::FOCUS_REASON_FFM:
                case Desktop::FOCUS_REASON_DESKTOP_STATE_CHANGE:
                case Desktop::FOCUS_REASON_GROUP_CURRENT_WINDOW_CHANGE:
                case Desktop::FOCUS_REASON_WORKSPACE_CHANGE:
                case Desktop::FOCUS_REASON_TOGGLE_SPECIAL_WORKSPACE:
                case Desktop::FOCUS_REASON_UNMAP_WINDOW_TILING:
                case Desktop::FOCUS_REASON_UNMAP_WINDOW_FLOATING:
                case Desktop::FOCUS_REASON_UNMAP_GROUPED_WINDOW:
                case Desktop::FOCUS_REASON_GHOSTS: return false;
                default: return true;
            }
        }
    }

    void init() {
        g_openHook = Event::bus()->m_events.window.openLate.listen([](PHLWINDOW w) {
            // A window opening on the shown desktop: the others come back to
            // meet it (it opens as any window does).
            if (g_state && g_state->out && w && w->m_workspace == g_state->workspace.lock() && !ours(w))
                comeBack("a window opened");
        });
        g_focusHook = Event::bus()->m_events.window.active.listen([](PHLWINDOW w, Desktop::eFocusReason reason) {
            // One of them chosen -- in the dock, by a notification, a keybind,
            // a click while it slides -- brings them all back.
            if (g_state && g_state->out && w && ours(w) && deliberate(reason))
                comeBack("one of them was focused");
        });
        g_desktopHook = Event::bus()->m_events.workspace.active.listen([](PHLWORKSPACE) {
            if (!g_state)
                return;
            const auto WS  = g_state->workspace.lock();
            const auto MON = g_state->monitor.lock();
            if (!WS || !MON || MON->m_activeWorkspace != WS)
                finish("the desktop left the screen");
        });
        g_monitorHook = Event::bus()->m_events.monitor.removed.listen([](PHLMONITOR mon) {
            if (g_state && g_state->monitor.lock() == mon)
                finish("its screen went away");
        });
    }

    void shutdown() {
        g_openHook.reset();
        g_focusHook.reset();
        g_desktopHook.reset();
        g_monitorHook.reset();
        // Never leave a window hidden behind a plugin that is gone, nor a timer
        // that would call into it.
        finish("unloading");
        if (g_tick) {
            wl_event_source_remove(g_tick);
            g_tick = nullptr;
        }
    }

    SDispatchResult dispatch(std::string arg) {
        if (arg.empty())
            arg = "toggle";
        const auto MON = Desktop::focusState()->monitor();
        if (!MON)
            return {};
        // In the overview the windows are its tiles: the key has nothing to do
        // there, and hiding them would take the tiles with them.
        if (!::scrollOverviews().empty())
            return {};
        // Shown on another screen or desktop: that one back at once, so this
        // press is about the desktop in front of you.
        if (g_state && (g_state->monitor.lock() != MON || g_state->workspace.lock() != MON->m_activeWorkspace))
            finish("the key was pressed on another desktop");
        const bool OUT = arg == "on" ? true : arg == "off" ? false : !(g_state && g_state->out);
        if (!OUT) {
            comeBack("the key");
            return {};
        }
        if (!g_state)
            begin(MON);
        else if (!g_state->out) {
            // Turned round again on the way back: out from where they are.
            g_state->out  = true;
            g_state->last = Time::steadyNow();
            damage();
            armTick();
            trace(std::format("out again from {:.3f}", g_state->spring.x));
        }
        return {};
    }

    void preRender(PHLMONITOR monitor) {
        if (!g_state || !monitor || g_state->monitor.lock() != monitor)
            return;
        auto&      S  = *g_state;
        const auto WS = S.workspace.lock();
        if (!WS || monitor->m_activeWorkspace != WS) {
            finish("the desktop left the screen");
            return;
        }
        if (S.resting)
            return;

        const double TARGET = S.out ? 1.0 : 0.0;
        const auto   NOW    = Time::steadyNow();
        // A stalled frame does not throw the windows: at most 50 ms a step.
        const double DT    = std::min(0.05, std::chrono::duration<double>(NOW - S.last).count());
        S.last             = NOW;
        const float  SCALE = Config::animScale();
        if (SCALE <= 0.F || !Config::motionEnabled())
            S.spring.snap(TARGET);
        else
            S.spring.step(DT / SCALE, TARGET, S.out ? OUT_OMEGA : IN_OMEGA);
        if (S.spring.settled(TARGET))
            S.spring.snap(TARGET);

        for (const auto& E : S.wins)
            if (!E.hidden)
                place(E, S.spring.x);
        g_pHyprRenderer->damageMonitor(monitor);

        if (S.spring.x == TARGET && S.spring.v == 0) {
            if (S.out)
                hideAll();
            else
                finish("arrived");
        }
    }

    void restoreNow() {
        finish("the overview opened");
    }

    bool active() {
        return g_state.has_value();
    }
}
