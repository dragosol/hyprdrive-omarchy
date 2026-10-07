#include "Spring.hpp"

#include "Config.hpp"
#include "Dock.hpp"
#include "DockDrop.hpp"
#include "IOverview.hpp"

#include <cairo/cairo.h>
#include <cmath>

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/layout/supplementary/DragController.hpp>
#include <hyprland/src/layout/target/Target.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/managers/input/UnifiedWorkspaceSwipeGesture.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/pass/TexPassElement.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/state/WorkspaceState.hpp>

namespace ScrollOverview::Spring {

    namespace {
        constexpr const char* KEY_DOCK  = "plugin:hyprdrive:drag:spring_dock";
        constexpr const char* KEY_EDGES = "plugin:hyprdrive:drag:spring_edges";
        constexpr const char* KEY_DELAY = "plugin:hyprdrive:drag:spring_delay_ms";
        constexpr int         TICK_MS   = 40;
        constexpr int         FAST_MS   = 16;  // a hold, a slide, a window held over a switch
        constexpr double      EDGE_PX   = 2.0; // logical: the cursor is clamped to the output
        constexpr float       GRACE_MS  = 220.F; // brushing past an edge moves nothing
        constexpr float       SLIDE_MS  = 300.F;
        constexpr float       REWIND_MS = 180.F;
        constexpr double      PEEK      = 0.07; // of the swipe distance: the sliver revealed by the end of the hold

        CHyprSignalListener g_move, g_workspace, g_button, g_render;
        wl_event_source*    g_tick = nullptr;

        struct SEdge {
            MONITORID                      monitor = MONITOR_INVALID;
            int                            dir     = 0;
            std::optional<Time::steady_tp> since;
            double                         y = 0; // logical, the pointer's height on the edge
        };
        SEdge g_edge;
        CBox  g_lineDamage; // logical, what the last edge line covered

        // THE HOLD, SHOWN AS WHERE IT GOES (user, 2026-10-07: the first glow was
        // "kinda ugly"). Held at an edge, the desktop eases aside and a sliver of
        // the next one shows -- Hyprland's own swipe engine, driven to a few per
        // cent of its distance -- with a thin accent light running along the
        // seam. When the hold completes the same swipe carries on into the
        // switch (Omarchy turns the workspace animation off, so changeWorkspace
        // alone would be a jump cut; this is the slide the fingers make); let go
        // of the edge first and it eases back.
        enum class EPhase : uint8_t {
            Peek,
            Commit,
            Rewind,
        };
        struct SSwipe {
            PHLMONITORREF   monitor;
            int             dir   = 0;
            EPhase          phase = EPhase::Peek;
            Time::steady_tp phaseStart;
            double          from = 0, cur = 0; // swipe delta units
        };
        std::optional<SSwipe> g_swipe;

        // A desktop switch in the middle of Hyprland's interactive move leaves
        // the drag stuck: changeWorkspace releases every mouse button, so the
        // real release later finds no pressed bind and never ends the drag --
        // the window follows the pointer until the next click (measured nested,
        // a Super+number switch with this plugin's code out of the way: stock
        // behaviour). The release is finished here when Hyprland's own is not.
        bool         g_switchedMidDrag = false;
        uint64_t     g_releaseLater    = 0; // the queued doLater, cancelled at unload
        PHLWINDOWREF g_releaseWindow;

        // The dragged window PINNED for as long as a switch is in flight: a
        // pinned window is drawn without its desktop's slide offset and
        // changeWorkspace carries it across, so it stays under the hand instead
        // of sliding off with the desktop it is leaving.
        PHLWINDOWREF g_pinned;

        // Buttons held right now. A client's drag-and-drop begins a round trip
        // AFTER the press, so a pointer that stopped at the edge before the drag
        // existed sends no motion to start the tick -- a held button keeps it.
        int g_buttonsDown = 0;

        PHLWINDOW draggedWindow() {
            if (!g_layoutManager || !g_layoutManager->dragController())
                return nullptr;
            const auto& CTRL = g_layoutManager->dragController();
            if (CTRL->mode() != MBIND_MOVE)
                return nullptr;
            const auto TARGET = CTRL->target();
            return TARGET ? TARGET->window() : nullptr;
        }

        bool fileDrag() {
            return DockDrop::dndInProgress();
        }

        bool dragging() {
            return fileDrag() || draggedWindow();
        }

        bool swipeInFlight() {
            return g_swipe.has_value() || (g_pUnifiedWorkspaceSwipe && g_pUnifiedWorkspaceSwipe->isGestureInProgress());
        }

        void unpin() {
            if (const auto W = g_pinned.lock(); W)
                W->m_pinned = false;
            g_pinned.reset();
        }

        void syncPin() {
            const auto WINDOW = draggedWindow();
            if (WINDOW && swipeInFlight()) {
                if (!g_pinned && WINDOW->m_isFloating && !WINDOW->m_pinned) {
                    WINDOW->m_pinned = true;
                    g_pinned         = WINDOW;
                }
                return;
            }
            if (g_pinned)
                unpin();
        }

        // -1 / +1 when the pointer is pressed against a left/right edge that
        // has no monitor beyond it (between two monitors it just crosses).
        int edgeDir(const PHLMONITOR& monitor, const Vector2D& point) {
            const auto BOX = monitor->logicalBox();
            int        dir = 0;
            if (point.x <= BOX.x + EDGE_PX)
                dir = -1;
            else if (point.x >= BOX.x + BOX.w - 1 - EDGE_PX)
                dir = 1;
            if (!dir)
                return 0;
            const Vector2D BEYOND{dir < 0 ? BOX.x - 4 : BOX.x + BOX.w + 4, point.y};
            for (const auto& M : State::monitorState()->monitors())
                if (M && M != monitor && M->m_enabled && M->logicalBox().containsPoint(BEYOND))
                    return 0;
            return dir;
        }

        double swipeDistance() {
            return std::max<int64_t>(1, Config::getValue<Hyprlang::INT>("gestures:workspace_swipe_distance"));
        }

        // Is there a desktop that way? Right: always (Hyprland's swipe opens a
        // new one past the last). Left: one of this monitor's with a lower id --
        // the swipe's own m-1.
        bool canGo(const PHLMONITOR& monitor, int dir) {
            const auto CURRENT = monitor->m_activeWorkspace;
            if (!CURRENT || monitor->m_activeSpecialWorkspace)
                return false;
            if (dir > 0)
                return true;
            for (const auto& W : State::workspaceState()->workspaces())
                if (const auto WS = W.lock(); WS && !WS->m_isSpecialWorkspace && WS->m_id > 0 && WS->m_id < CURRENT->m_id && WS->m_monitor.lock() == monitor)
                    return true;
            return false;
        }

        void setDelta(double d) {
            g_swipe->cur = d;
            g_pUnifiedWorkspaceSwipe->update(d);
        }

        bool beginSwipe(const PHLMONITOR& monitor, int dir) {
            if (!g_pUnifiedWorkspaceSwipe || g_pUnifiedWorkspaceSwipe->isGestureInProgress())
                return false;
            // The swipe engine works on the FOCUSED monitor.
            if (Desktop::focusState()->monitor() != monitor)
                Desktop::focusState()->rawMonitorFocus(monitor);
            g_pUnifiedWorkspaceSwipe->begin();
            if (!g_pUnifiedWorkspaceSwipe->isGestureInProgress())
                return false;
            g_swipe = SSwipe{monitor, dir, EPhase::Peek, Time::steadyNow()};
            syncPin();
            return true;
        }

        void toPhase(EPhase phase) {
            g_swipe->phase      = phase;
            g_swipe->phaseStart = Time::steadyNow();
            g_swipe->from       = g_swipe->cur;
        }

        float sincePhase() {
            return std::chrono::duration<float, std::milli>(Time::steadyNow() - g_swipe->phaseStart).count();
        }

        // Commit and Rewind play on their own; Peek follows the hold.
        void stepSwipe() {
            if (!g_swipe)
                return;
            if (!g_pUnifiedWorkspaceSwipe || !g_pUnifiedWorkspaceSwipe->isGestureInProgress() || !g_swipe->monitor) {
                g_swipe.reset();
                return;
            }
            if (g_swipe->phase == EPhase::Commit) {
                const float  P = std::clamp(sincePhase() / SLIDE_MS, 0.F, 1.F);
                const double E = 1.0 - std::pow(1.0 - P, 3.0);
                setDelta(g_swipe->from + (g_swipe->dir * swipeDistance() - g_swipe->from) * E);
                if (P >= 1.F) {
                    g_pUnifiedWorkspaceSwipe->end(); // the whole distance: it commits
                    g_swipe.reset();
                    g_edge = {}; // still at the edge: the next hold starts from zero
                }
            } else if (g_swipe->phase == EPhase::Rewind) {
                const float  P = std::clamp(sincePhase() / REWIND_MS, 0.F, 1.F);
                const double E = 1.0 - std::pow(1.0 - P, 2.0);
                setDelta(g_swipe->from * (1.0 - E));
                if (P >= 1.F) {
                    g_pUnifiedWorkspaceSwipe->end(); // under 2 units: it reverts in place
                    g_swipe.reset();
                }
            }
        }

        float holdMs() {
            return g_edge.since ? std::chrono::duration<float, std::milli>(Time::steadyNow() - *g_edge.since).count() : 0.F;
        }

        static double smoothstep(double t) {
            t = std::clamp(t, 0.0, 1.0);
            return t * t * (3.0 - 2.0 * t);
        }

        // The edge light's texture: a thin line of the theme accent with a soft
        // falloff inward and faded ends. Rebuilt when the accent changes.
        struct SLineTex {
            uint32_t             rgba = 0;
            SP<Render::ITexture> tex;
        };
        SLineTex g_lineTex[2];

        SP<Render::ITexture> lineTexture(int dir) {
            const auto C    = Dock::accentColor();
            const auto RGBA = (sc<uint32_t>(C.r * 255) << 24) | (sc<uint32_t>(C.g * 255) << 16) | (sc<uint32_t>(C.b * 255) << 8) | 0xff;
            auto&      slot = g_lineTex[dir > 0 ? 1 : 0];
            if (slot.tex && slot.rgba == RGBA)
                return slot.tex;

            constexpr int W = 48, H = 512;
            auto*         surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, W, H);
            auto*         cr   = cairo_create(surf);
            cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
            cairo_paint(cr);
            cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
            // Across: a bright 2px core on the seam, then a short soft falloff.
            auto* across = cairo_pattern_create_linear(dir > 0 ? W : 0, 0, dir > 0 ? 0 : W, 0);
            cairo_pattern_add_color_stop_rgba(across, 0.0, (1 + C.r) / 2, (1 + C.g) / 2, (1 + C.b) / 2, 0.95);
            cairo_pattern_add_color_stop_rgba(across, 0.05, C.r, C.g, C.b, 0.75);
            cairo_pattern_add_color_stop_rgba(across, 0.25, C.r, C.g, C.b, 0.22);
            cairo_pattern_add_color_stop_rgba(across, 1.0, C.r, C.g, C.b, 0.0);
            // Along: the ends fade out, so it reads as light, not a bar.
            auto* along = cairo_pattern_create_linear(0, 0, 0, H);
            cairo_pattern_add_color_stop_rgba(along, 0.0, 0, 0, 0, 0.0);
            cairo_pattern_add_color_stop_rgba(along, 0.35, 0, 0, 0, 1.0);
            cairo_pattern_add_color_stop_rgba(along, 0.65, 0, 0, 0, 1.0);
            cairo_pattern_add_color_stop_rgba(along, 1.0, 0, 0, 0, 0.0);
            cairo_set_source(cr, across);
            cairo_mask(cr, along);
            cairo_pattern_destroy(across);
            cairo_pattern_destroy(along);
            cairo_destroy(cr);
            cairo_surface_flush(surf);
            slot.tex  = g_pHyprRenderer->createTexture(surf);
            slot.rgba = RGBA;
            cairo_surface_destroy(surf);
            return slot.tex;
        }

        // 0..1 through the hold; the light's box rides the seam of the desktop
        // that is easing aside.
        CBox lineBoxLogical(const PHLMONITOR& monitor, double p) {
            const auto   BOX  = monitor->logicalBox();
            const double E    = smoothstep(p);
            const double H    = BOX.h * (0.16 + 0.34 * E);
            const double W    = 22.0;
            const double GAP  = Config::getValue<Hyprlang::INT>("general:gaps_workspaces");
            const double SEAM = g_swipe && g_swipe->phase == EPhase::Peek ? std::abs(g_swipe->cur) / swipeDistance() * (BOX.w + GAP) : 0.0;
            const double X    = g_edge.dir < 0 ? BOX.x + SEAM : BOX.x + BOX.w - SEAM - W;
            const double Y    = std::clamp(g_edge.y - H / 2.0, BOX.y, BOX.y + BOX.h - H);
            return CBox{X, Y, W, H};
        }

        void damageLine(const PHLMONITOR& monitor) {
            if (g_lineDamage.width > 0)
                g_pHyprRenderer->damageBox(g_lineDamage);
            g_lineDamage = {};
            if (!monitor || !g_edge.dir)
                return;
            auto box = lineBoxLogical(monitor, 1.0);
            box.x -= 8;
            box.w += 16 + PEEK * (monitor->logicalBox().w + 50); // the seam travels
            if (g_edge.dir > 0)
                box.x -= PEEK * (monitor->logicalBox().w + 50);
            g_lineDamage = box;
            g_pHyprRenderer->damageBox(g_lineDamage);
        }

        void renderLine(const PHLMONITOR& monitor) {
            if (!monitor || !g_edge.dir || monitor->m_id != g_edge.monitor || (g_swipe && g_swipe->phase != EPhase::Peek))
                return;
            const int    DELAY = std::clamp<int>(Config::getValue<int>(KEY_DELAY), 200, 10000);
            const double P     = std::clamp(holdMs() / DELAY, 0.F, 1.F);
            const double A     = smoothstep((holdMs() - GRACE_MS * 0.5) / 260.0); // fades in just after the grace
            if (A <= 0.0)
                return;
            const auto TEX = lineTexture(g_edge.dir);
            if (!TEX)
                return;
            const auto L = lineBoxLogical(monitor, P);
            CBox       box{(L.x - monitor->m_position.x) * monitor->m_scale, (L.y - monitor->m_position.y) * monitor->m_scale, L.w * monitor->m_scale,
                     L.h * monitor->m_scale};
            box.round();
            if (box.width < 1 || box.height < 1)
                return; // a zero box to a pass element aborts the compositor
            CTexPassElement::SRenderData d;
            d.tex = TEX;
            d.box = box;
            d.a   = sc<float>(A * (0.55 + 0.45 * P));
            g_pHyprRenderer->m_renderPass.add(makeUnique<CTexPassElement>(d));
        }

        void stopTick() {
            if (g_tick)
                wl_event_source_timer_update(g_tick, 0);
        }

        int tick(void*) {
            stepSwipe();
            syncPin();

            const bool DRAG  = dragging();
            const auto POINT = g_pInputManager->getMouseCoordsInternal();
            PHLMONITOR monitor;
            for (const auto& M : State::monitorState()->monitors())
                if (M && M->logicalBox().containsPoint(POINT))
                    monitor = M;
            const bool DESKTOP = DRAG && monitor && !scrollOverviewForMonitor(monitor);
            const int  DELAY   = std::clamp<int>(Config::getValue<int>(KEY_DELAY), 200, 10000);
            bool       fast    = g_swipe.has_value() || (draggedWindow() && swipeInFlight());

            // The dock: client file drags only.
            if (DESKTOP && fileDrag() && Config::getValue<bool>(KEY_DOCK))
                fast |= Dock::springUpdate(monitor, (POINT - monitor->m_position) * monitor->m_scale, DockDrop::offerIsFolders(), DELAY);
            else
                Dock::springReset();

            // The edges: file drags and window drags. A swipe the FINGERS are
            // making is theirs; one sliding to its end is left to finish.
            const bool FINGERS = !g_swipe && g_pUnifiedWorkspaceSwipe && g_pUnifiedWorkspaceSwipe->isGestureInProgress();
            const bool PLAYING = g_swipe && g_swipe->phase != EPhase::Peek;
            int        dir     = DESKTOP && !FINGERS && !PLAYING && Config::getValue<bool>(KEY_EDGES) ? edgeDir(monitor, POINT) : 0;
            if (dir && !canGo(monitor, dir))
                dir = 0;

            if (!PLAYING) {
                const bool MOVED = dir && (g_edge.dir != dir || g_edge.monitor != monitor->m_id);
                if (g_swipe && (!dir || MOVED))
                    toPhase(EPhase::Rewind); // left the edge mid-hold: ease back
                if (!dir) {
                    if (g_edge.dir)
                        damageLine(nullptr);
                    g_edge = {};
                } else {
                    if (MOVED || !g_edge.since)
                        g_edge = {monitor->m_id, dir, Time::steadyNow(), POINT.y};
                    g_edge.y      = POINT.y;
                    const float T = holdMs();
                    if (T >= GRACE_MS && !g_swipe)
                        beginSwipe(monitor, dir);
                    if (g_swipe && g_swipe->phase == EPhase::Peek) {
                        const double P = smoothstep((T - GRACE_MS) / std::max(1.F, DELAY - GRACE_MS));
                        setDelta(dir * PEEK * swipeDistance() * P);
                        if (T >= DELAY)
                            toPhase(EPhase::Commit);
                    }
                    damageLine(monitor);
                    fast = true;
                }
            } else if (g_edge.dir) {
                damageLine(nullptr);
            }

            if (DRAG || fast || g_pinned || g_buttonsDown > 0)
                wl_event_source_timer_update(g_tick, fast ? FAST_MS : TICK_MS);
            else
                stopTick();
            return 0;
        }

        void armTick(int ms) {
            if (g_tick)
                wl_event_source_timer_update(g_tick, ms);
        }
    }

    void init() {
        if (!g_tick)
            g_tick = wl_event_loop_add_timer(g_pCompositor->m_wlEventLoop, tick, nullptr);

        // Motion only STARTS the tick; the tick does the work and stops itself.
        g_move = Event::bus()->m_events.input.mouse.move.listen([](Vector2D, Event::SCallbackInfo&) {
            if (dragging())
                armTick(TICK_MS);
        });

        g_workspace = Event::bus()->m_events.workspace.active.listen([](PHLWORKSPACE) {
            if (const auto W = draggedWindow(); W) {
                g_switchedMidDrag = true;
                g_releaseWindow   = W;
            }
        });

        g_button = Event::bus()->m_events.input.mouse.button.listen([](IPointer::SButtonEvent e, Event::SCallbackInfo&) {
            if (e.state == WL_POINTER_BUTTON_STATE_PRESSED) {
                ++g_buttonsDown;
                armTick(TICK_MS);
                return;
            }
            g_buttonsDown = std::max(0, g_buttonsDown - 1);
            // Let go mid-switch: the window is the user's again, not pinned.
            if (g_pinned)
                unpin();
            if (!g_switchedMidDrag)
                return;
            // After Hyprland has had its go at this release.
            g_releaseLater = g_pEventLoopManager->doLater([] {
                g_releaseLater    = 0;
                g_switchedMidDrag = false;
                if (g_layoutManager && g_layoutManager->dragController() && g_layoutManager->dragController()->target())
                    g_layoutManager->endDragTarget();
                // The window was carried while its old desktop went hidden: the
                // client may still be suspended (Ghostty then never draws its
                // new size) or never have been told the size it was tiled to.
                if (const auto W = g_releaseWindow.lock(); W && W->m_isMapped) {
                    if (W->m_workspace && W->m_workspace->isVisible())
                        W->setSuspended(false);
                    W->sendWindowSize(true);
                }
                g_releaseWindow.reset();
            });
        });

        g_render = Event::bus()->m_events.render.stage.listen([](eRenderStage stage) {
            if (stage == RENDER_LAST_MOMENT && g_edge.dir)
                renderLine(g_pHyprRenderer->m_renderData.pMonitor.lock());
        });
    }

    void shutdown() {
        g_move.reset();
        g_workspace.reset();
        g_button.reset();
        g_render.reset();
        if (g_tick)
            wl_event_source_remove(g_tick);
        g_tick = nullptr;
        if (g_swipe && g_pUnifiedWorkspaceSwipe && g_pUnifiedWorkspaceSwipe->isGestureInProgress()) {
            g_pUnifiedWorkspaceSwipe->update(0); // unloading mid-hold: back where it was
            g_pUnifiedWorkspaceSwipe->end();
        }
        g_swipe.reset();
        unpin();
        Dock::springReset();
        g_edge = {};
        g_buttonsDown     = 0;
        g_switchedMidDrag = false;
        g_releaseWindow.reset();
        if (g_releaseLater && g_pEventLoopManager)
            g_pEventLoopManager->removeDoLater(g_releaseLater);
        g_releaseLater = 0;
        for (auto& t : g_lineTex)
            t = {};
    }
}
