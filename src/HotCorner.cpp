#include "HotCorner.hpp"
#include "HotCornerModel.hpp"

#include "Config.hpp"
#include "Dock.hpp"
#include "DockDrop.hpp"
#include "IOverview.hpp"
#include "Ripple.hpp"

#include <algorithm>

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/layout/supplementary/DragController.hpp>
#include <hyprland/src/managers/KeybindManager.hpp>
#include <hyprland/src/managers/SessionLockManager.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/managers/input/UnifiedWorkspaceSwipeGesture.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/protocols/PointerConstraints.hpp>
#include <hyprland/src/state/MonitorState.hpp>

namespace ScrollOverview::HotCorner {

    namespace {
        constexpr const char* KEY_ENABLED = "plugin:hyprdrive:hot_corner:enabled";
        constexpr const char* KEY_CORNER  = "plugin:hyprdrive:hot_corner:corner";
        constexpr const char* KEY_DELAY   = "plugin:hyprdrive:hot_corner:delay_ms";
        constexpr const char* KEY_RIPPLE  = "plugin:hyprdrive:hot_corner:ripple";

        CHyprSignalListener g_move, g_button;
        wl_event_source*    g_timer = nullptr;

        int  g_buttonsDown = 0;
        // Waiting out the delay for the pointer resting in the corner.
        bool g_pending = false;
        // Fired (or cancelled by a click) on this visit: the pointer must leave
        // the corner before it can fire again, or an overview closed with the
        // pointer still parked there would reopen at once.
        bool g_latched = false;

        SRect rectOf(const CBox& box) {
            return SRect{box.x, box.y, box.w, box.h};
        }

        // The monitor whose OUTER chosen corner the pointer is in, or nullptr.
        PHLMONITOR cornerMonitor(const Vector2D& point, ECorner corner) {
            for (const auto& M : State::monitorState()->monitors()) {
                if (!M || !M->m_enabled)
                    continue;
                const auto BOX = rectOf(M->logicalBox());
                if (!inCorner(point.x, point.y, BOX, corner))
                    continue;
                for (const auto& P : probes(BOX, corner))
                    for (const auto& OTHER : State::monitorState()->monitors())
                        if (OTHER && OTHER != M && OTHER->m_enabled && OTHER->logicalBox().containsPoint(Vector2D{P.x, P.y}))
                            return nullptr;
                return M;
            }
            return nullptr;
        }

        bool pointerLocked() {
            if (!g_pInputManager)
                return false;
            return std::ranges::any_of(g_pInputManager->m_constraints, [](const auto& c) {
                const auto C = c.lock();
                return C && C->isActive();
            });
        }

        // An overview is up: the corner closes it (user, 2026-10-13: "hitting it
        // again should close overview if overview is open"), as GNOME's does.
        bool overviewUp() {
            return !scrollOverviews().empty();
        }

        // Every reason the corner must stay quiet, asked again when it fires:
        // the world can change during the delay.
        bool blocked(const PHLMONITOR& monitor, const Vector2D& point) {
            if (g_pSessionLockManager && g_pSessionLockManager->isSessionLocked())
                return true;
            // A held button is a drag (a window in the overview, a file, a
            // selection) on its way somewhere, never a trip to the corner.
            if (g_buttonsDown > 0)
                return true;
            if (g_pUnifiedWorkspaceSwipe && g_pUnifiedWorkspaceSwipe->isGestureInProgress())
                return true;
            if (g_layoutManager && g_layoutManager->dragController() && g_layoutManager->dragController()->target())
                return true;
            if (DockDrop::dndInProgress())
                return true;
            if (overviewUp()) {
                // Closing already, or mid-swipe: a toggle now would reverse it.
                for (const auto& OV : scrollOverviews())
                    if (OV && (OV->isClosing() || OV->m_isSwiping))
                        return true;
                return false;
            }
            if (pointerLocked())
                return true;
            // A fullscreen video or game owns that screen's corners too.
            if (Fullscreen::controller()->hasFullscreen(monitor))
                return true;
            const Vector2D LOCAL = (point - monitor->m_position) * monitor->m_scale;
            return Dock::occupiesPointer(monitor, LOCAL);
        }

        void cancel() {
            g_pending = false;
            if (g_timer)
                wl_event_source_timer_update(g_timer, 0);
        }

        void toggleOverview() {
            // Outside the motion emit (a fresh overview registers its own motion
            // listener), and through the dispatcher so it is exactly a Super tap:
            // opens it everywhere, or closes it where it is open everywhere.
            if (!g_pKeybindManager)
                return;
            const auto IT = g_pKeybindManager->m_dispatchers.find("hyprdrive:overview");
            if (IT != g_pKeybindManager->m_dispatchers.end())
                IT->second("toggle all");
        }

        // The screen corner itself (global logical px): where the ripple starts.
        Vector2D cornerPoint(const PHLMONITOR& monitor, ECorner corner) {
            const auto BOX  = monitor->logicalBox();
            const bool LEFT = corner == ECorner::TopLeft || corner == ECorner::BottomLeft;
            const bool TOP  = corner == ECorner::TopLeft || corner == ECorner::TopRight;
            return Vector2D{LEFT ? BOX.x : BOX.x + BOX.w, TOP ? BOX.y : BOX.y + BOX.h};
        }

        int fire(void*) {
            if (!g_pending)
                return 0;
            g_pending = false;
            if (!Config::getValue<bool>(KEY_ENABLED) || !g_pInputManager)
                return 0;
            const auto POINT   = g_pInputManager->getMouseCoordsInternal();
            const auto CORNER  = cornerFromName(Config::getValue<std::string>(KEY_CORNER));
            const auto MONITOR = cornerMonitor(POINT, CORNER);
            if (!MONITOR || blocked(MONITOR, POINT))
                return 0;
            g_latched = true;
            if (Config::getValue<bool>(KEY_RIPPLE))
                Ripple::trigger(MONITOR, cornerPoint(MONITOR, CORNER));
            toggleOverview();
            return 0;
        }

        void onMove() {
            if (!g_pInputManager || !Config::getValue<bool>(KEY_ENABLED)) {
                cancel();
                g_latched = false;
                return;
            }
            const auto POINT   = g_pInputManager->getMouseCoordsInternal();
            const auto MONITOR = cornerMonitor(POINT, cornerFromName(Config::getValue<std::string>(KEY_CORNER)));
            if (!MONITOR) {
                cancel();
                g_latched = false;
                return;
            }
            if (g_latched || g_pending || blocked(MONITOR, POINT))
                return;
            g_pending = true;
            // 0 disarms a wl timer, so "no delay" is the next loop turn.
            wl_event_source_timer_update(g_timer, std::clamp<int>(Config::getValue<int>(KEY_DELAY), 1, 2000));
        }
    }

    void init() {
        if (!g_timer)
            g_timer = wl_event_loop_add_timer(g_pCompositor->m_wlEventLoop, fire, nullptr);

        g_move = Event::bus()->m_events.input.mouse.move.listen([](Vector2D, Event::SCallbackInfo&) { onMove(); });

        g_button = Event::bus()->m_events.input.mouse.button.listen([](IPointer::SButtonEvent e, Event::SCallbackInfo&) {
            if (e.state == WL_POINTER_BUTTON_STATE_PRESSED) {
                ++g_buttonsDown;
                // A click in the corner was meant for whatever is there (the
                // bar's menu button): this visit is spent.
                if (g_pending) {
                    cancel();
                    g_latched = true;
                }
                return;
            }
            g_buttonsDown = std::max(0, g_buttonsDown - 1);
        });
    }

    void shutdown() {
        g_move.reset();
        g_button.reset();
        if (g_timer)
            wl_event_source_remove(g_timer);
        g_timer       = nullptr;
        g_pending     = false;
        g_latched     = false;
        g_buttonsDown = 0;
    }
}
