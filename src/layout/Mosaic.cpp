// The "mosaic" layout: see Mosaic.hpp and MosaicPack.hpp.
#include "Mosaic.hpp"
#include "Intent.hpp"

#include "DesktopStrip.hpp"
#include "DebugPaths.hpp"
#include "MosaicPack.hpp"
#include "MosaicCanvas.hpp"
#include "MosaicDrive.hpp"
#include "Motion.hpp"
#include "MotionClock.hpp"
#include "globals.hpp"
#include "IOverview.hpp"
#include "Dock.hpp"
#include "DockStacks.hpp"
#include "OverviewRender.hpp"
#include "FlightController.hpp"

#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/layout/algorithm/Algorithm.hpp>
#include <hyprland/src/layout/algorithm/TiledAlgorithm.hpp>
#include <hyprland/src/layout/space/Space.hpp>
#include <hyprland/src/layout/target/Target.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/managers/fullscreen/handler/FullscreenHandler.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/animation/AnimationManager.hpp>
#include <hyprland/src/config/shared/animation/AnimationTree.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/protocols/XDGShell.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/state/ViewState.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/layout/supplementary/DragController.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>

#include <limits>
#include <hyprland/src/managers/input/UnifiedWorkspaceSwipeGesture.hpp>
#include <hyprland/src/managers/KeybindManager.hpp>
#include <hyprland/src/pointer/cursor/CursorShapeOverrideController.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/config/shared/actions/ConfigActions.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/pointer/PointerController.hpp>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/view/LayerSurface.hpp>
#include <hyprland/src/protocols/LayerShell.hpp>
#include "Config.hpp"
#include <linux/input-event-codes.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <typeinfo>
#include <unordered_map>
#include <vector>
#include "MosaicParts.hpp"

namespace Hyprdrive::Mosaic {

    using namespace MosaicDetail;

    // All of the mosaic's file-scope state lives here, in its original order (the parts reach it
    // through MosaicParts.hpp), with the helpers the layout class calls.
    namespace MosaicDetail {
    // Every live mosaic layout -- one per desktop on mosaic. Hyprland keeps the
    // algorithm behind a desktop private, so "is this window on a mosaic
    // desktop" is answered from here instead.
    std::vector<CMosaicAlgorithm*> g_instances;

        // remembered sizes (MosaicSizes.cpp)
        std::unordered_map<std::string, Vector2D> g_sizes;
        bool                                      g_sizesLoaded = false;
        std::vector<SKept> g_windowSizes;

    // TRACE, for the next odd recording: `touch $XDG_RUNTIME_DIR/hyprdrive-mosaic.on`
    // and every resize step and arrangement decision lands in
    // $XDG_RUNTIME_DIR/hyprdrive-mosaic.log (DebugPaths.hpp: ours only, 0600).
    bool traceOn() {
        static bool                                  on      = false;
        static std::chrono::steady_clock::time_point checked = {};
        const auto                                   NOW     = std::chrono::steady_clock::now();
        if (NOW - checked > std::chrono::seconds(1)) {
            checked = NOW;
            on      = Hyprdrive::DebugPaths::flagOn("hyprdrive-mosaic.on");
        }
        return on;
    }
    void trace(const std::string& line) {
        if (!traceOn())
            return;
        if (FILE* const F = Hyprdrive::DebugPaths::openLog("hyprdrive-mosaic.log")) {
            std::fprintf(F, "%lld %s\n",
                         static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count() % 100000000),
                         line.c_str());
            std::fclose(F);
        }
    }

    // THE INFINITE CANVAS (MosaicCanvas.hpp; user, 2026-10-26). Off: a mosaic
    // that no longer fits tiles, as before. On: it spills onto a canvas the view
    // pans across -- a strip (horizontal) or a grid (all directions).
    bool canvasOn() {
        return Hyprdrive::Config::getValue<bool>("plugin:hyprdrive:mosaic:infinite_canvas");
    }
    bool canvasAllDirections() {
        return Hyprdrive::Config::getValue<std::string>("plugin:hyprdrive:mosaic:canvas_directions") == "all";
    }
    // THE PAN'S MOTION (user, 2026-10-26: "when I focus on a window it pans it
    // into view smoothly with a serpantinum curve, adjustable in the animation
    // tab"): animation:canvas_curve (a Motion.hpp preset, its DECEL) over
    // animation:canvas_pan_ms on the animation clock (Motion::Clock: the
    // progress is read through the speed setting LIVE, so a change reaches a
    // pan under way). 0 when the clock snaps: every caller then takes its
    // instant path, as it did when this multiplied by a scale of 0.
    double canvasPanMs() {
        if (!Hyprdrive::Motion::Clock::enabled())
            return 0.0;
        const double MS = sc<double>(Hyprdrive::Config::getValue<Hyprlang::INT>("plugin:hyprdrive:animation:canvas_pan_ms"));
        return std::max(0.0, MS);
    }
    float canvasEase(double t) {
        return Hyprdrive::Motion::easeNamed(Hyprdrive::Config::getValue<std::string>("plugin:hyprdrive:animation:canvas_curve"),
                                                 Hyprdrive::Motion::ERole::DECEL, sc<float>(t));
    }
    // A swipe or a Super-pan is moving the view: focus landing on a window
    // under the pointer must not pull the view back to it mid-pan.
    bool g_canvasPanning = false;
    // rideCursor is warping the pointer: that motion is not the hand's.
    // ONE FLAG (2026-11-09): a class member of the same name used to hide
    // this one inside CMosaicAlgorithm, so rideCursor set the member and the
    // checks out here (superMove, focusUnderBar, hoverRefresh) never saw a
    // ride, from the day each was written.
    bool g_cursorRiding = false;
    SEdgePanLatch g_edgePan;
    // PRESSURE FIRST (user, 2026-11-08: "a light push on the bottom should do
    // nothing except show the dock, it's if i push more"): a push starts only
    // once the hand has pushed PUSH_PRESSURE past the edge in one go -- the
    // dock (or a bar) reveals on the touch, the canvas on the push. A pause of
    // EDGE_PAN_HOLD, or turning, starts the count again. One count per axis,
    // or a push into a corner never gives way (MosaicCanvas.hpp).
    MosaicPack::SEdgePressure g_edgePressure;
    // How far (logical px) the hand pushes past the edge before the canvas
    // gives way: mosaic:edge_push_resistance (user, 2026-11-09: "too easy to
    // push through, especially going down"), half again at the bottom, where
    // the dock is.
    double pushPressure(int axis, double sign) {
        const double PX = sc<double>(Hyprdrive::Config::getValue<Hyprlang::INT>("plugin:hyprdrive:mosaic:edge_push_resistance"));
        return std::max(0.0, PX) * (axis == 1 && sign > 0 ? 1.5 : 1.0);
    }
    // ON THE DOCK (user, 2026-11-09: "being mouseovered on dock, dock context
    // menus and folders should prevent focusing a window as usual"): the
    // pointer on the dock's pill, a menu, a fan or pane, or on any layer
    // surface but the bar (focusUnderBar has that one) is not on the window
    // painted underneath -- and while a menu or a fan is up nothing moves.
    bool pointerOnChrome(const Vector2D& p) {
        if (Hyprdrive::Stacks::isOpen() || Hyprdrive::Dock::menuOpen())
            return true;
        // The dock answers in monitor-local render px.
        if (const auto MON = State::monitorState()->query().vec(p).run(); MON && Hyprdrive::Dock::uiContains(MON, (p - MON->m_position) * MON->m_scale))
            return true;
        const auto PSURF = g_pSeatManager ? g_pSeatManager->m_state.pointerFocus.lock() : SP<CWLSurfaceResource>{};
        const auto HLS   = PSURF ? Desktop::View::CWLSurface::fromResource(PSURF) : SP<Desktop::View::CWLSurface>{};
        const auto VIEW  = HLS ? HLS->view() : SP<Desktop::View::IView>{};
        if (VIEW && VIEW->type() == Desktop::View::VIEW_TYPE_LAYER_SURFACE) {
            const auto LS = Desktop::View::CLayerSurface::fromView(VIEW);
            return !LS || (LS->m_namespace != "omarchy-bar" && LS->m_namespace != "waybar");
        }
        return false;
    }
    // HOVER GONE STALE (user, 2026-11-09: "when I push for a bit then move my
    // cursor over previous window it doesn't refocus it"). Hyprland hands the
    // focus to the window under the pointer only when the pointer ENTERS it.
    // When the focus moved without the pointer entering anything (a push
    // focusing the window it brought in) or a hover was refused (on the dock,
    // a sliver at the edge), the window the pointer is in never gets it back
    // by moving within it. Set then; the next motion over a window that is not
    // the focused one focuses it (hoverRefresh, from onMove).
    // Over at any focus that is not a hover (2026-11-09: g_focusReasonHook):
    // a key, a click or the dock put the focus where the user wants it, and
    // the next motion inside the window under the pointer took it back.
    bool g_hoverStale = false;
    // THE DOCK'S BAND (user, 2026-11-09: pushing down "while dock is up still
    // eventually focuses windows. it should be able to push but not focus
    // windows UNTIL I have moved my cursor outside the boundaries that
    // triggered the dock"): an autohiding dock that is up owns the screen's
    // width from its top edge down, not just its pill -- the pointer there
    // came to show the dock, and nothing under it takes the focus.
    bool pointerInDockBand(const Vector2D& p) {
        const auto MON = State::monitorState()->query().vec(p).run();
        if (!MON || !Hyprdrive::Config::getValue<bool>("plugin:hyprdrive:dock:autohide"))
            return false;
        const auto L = Hyprdrive::Dock::layout(MON);
        if (L.pill.width < 1 || L.pill.height < 1)
            return false; // away: no dock up
        const double SC = std::max(MON->m_scale, 0.01F);
        return p.y >= MON->m_position.y + L.pill.y / SC;
    }
    // A window sliding under the pointer while it pushes is not a hover.
    bool edgePanRecent() {
        return g_edgePan.who && std::chrono::steady_clock::now() - g_edgePan.at < EDGE_PAN_HOLD;
    }

    // A window's title pill at the bottom of the box it is drawn at (zoomed or
    // where it is): the overview's caption pill (OverviewRender::labelPill).
    void drawWindowLabel(const PHLMONITOR& monitor, const PHLWINDOW& w, float alpha) {
        const float    SCALE = std::max(monitor->m_scale, 0.01F);
        const Vector2D P = w->positionAnimation()->value(), S = w->sizeAnimation()->value();
        if (S.x < 48 || S.y < 32)
            return;
        const double CX     = (P.x + S.x / 2.0 - monitor->m_position.x) * SCALE;
        const double BOTTOM = (P.y + S.y - monitor->m_position.y - 14.0) * SCALE;
        OverviewRender::labelPill(monitor, w->m_title.empty() ? w->m_class : w->m_title, CX, BOTTOM, alpha, SCALE);
    }

        // corner grab, Super gesture, canvas clock (MosaicGestures.cpp)
        bool        g_ourDrag   = false;
        bool        g_hoverSet  = false;
        ESuper            g_super = ESuper::IDLE;
        // The zoom-out is the fingers' (fingerZoom*), not Super's: it stays
        // after they lift, until a click picks a window.
        bool              g_fingerZoom = false;
        Vector2D          g_superAnchor;
        CMosaicAlgorithm* g_superCanvas = nullptr; // the zoomed one
        bool              g_swallowRelease = false;
        bool              g_zoomPressing   = false; // a button went down in the zoomed-out view
        uint32_t          g_zoomButton     = 0;     // which: BTN_LEFT carries or resizes, BTN_RIGHT resizes
        PHLWINDOWREF      g_labelHover; // the window whose title the hover shows (mosaic:zoom_labels)
        bool              g_superLabels = false; // titles up: Super held and the pointer moved, or the fingers' zoom-out
        std::vector<SLabelFade> g_labelFades;
        wl_event_source*  g_canvasTimer = nullptr;
        // A Super+drag pushing at the screen's edge pans the canvas under it
        // (user, 2026-10-26: Super+drag on the canvas "doesn't behave ... like
        // regular mosaic"): the far groups are otherwise out of reach.
        Vector2D          g_edgePush;       // -1/0/1 per axis while pushing
        PHLWORKSPACEREF   g_edgeWorkspace;
        std::chrono::steady_clock::time_point g_edgeLast;
    // desktop slides (MosaicSlides.cpp)
    wl_event_source* g_slideTimer = nullptr;
        // zoomed-out keys (MosaicGestures.cpp)
        std::unordered_set<uint32_t> g_zoomKeysTaken; // presses taken: their releases too
        std::unordered_set<uint32_t> g_zoomKeysSent;  // presses handed to the picker: their releases too
    }

    void shutdown() {
        if (g_slideTimer)
            wl_event_source_remove(g_slideTimer);
        g_slideTimer = nullptr;
        // Nothing left drawn off its box, no clock left to fire into unmapped code.
        for (auto* A : g_instances)
            A->endZoomNow();
        g_superCanvas    = nullptr;
        g_super          = ESuper::IDLE;
        g_labelFades.clear();
        g_zoomPressing   = false;
        g_swallowRelease = false;
        g_edgePush       = {};
        if (g_canvasTimer) {
            wl_event_source_remove(g_canvasTimer);
            g_canvasTimer = nullptr;
        }
        if (g_ourDrag && g_layoutManager && g_layoutManager->dragController()->target())
            g_layoutManager->endDragTarget();
        g_ourDrag = false;
        if (g_hoverSet)
            Pointer::Cursor::overrideController->unsetOverride(Pointer::Cursor::CURSOR_OVERRIDE_WINDOW_EDGE);
        g_hoverSet = false;
    }

    void onWindowClose(const PHLWINDOW& w) {
        const auto APP = appOf(w);
        if (APP.empty())
            return;
        for (const auto* A : g_instances)
            if (A->holds(w)) {
                // Only a size the app or the user chose (SNatural::trusted):
                // never a tile or a size inherited from another layout.
                if ((A->arrangedAsMosaic() || A->arrangedAsCanvas()) && A->trustedOf(w))
                    if (const auto N = A->naturalOf(w))
                        saveSize(APP, *N);
                return;
            }
    }

    // FOCUS BY DIRECTION (user, 2026-10-14: "I need super+arrowkeys to be able
    // to switch focus between windows in mosaic"). Hyprland's own movefocus
    // looks for a window whose edge lines up with the focused one's across the
    // gap -- what dwindle and scrolling always have, and a mosaic's centred,
    // bunched cluster mostly does not. hl.plugin.hyprdrive.focus("l"|"r"|"u"|"d"),
    // bound to Super+arrows while Mosaic is on (the panel's fragment): on a
    // mosaic desktop, the window whose centre lies that way, nearest along the
    // direction and closest across it; anywhere else -- or with nothing that
    // way -- exactly Hyprland's movefocus, so dwindle and scrolling (and the
    // move to the next monitor at the edge) are unchanged.
    static Math::eDirection dirOf(const std::string& arg) {
        const char C = arg.empty() ? 'l' : arg[0];
        return C == 'r' ? Math::DIRECTION_RIGHT : C == 'u' ? Math::DIRECTION_UP : C == 'd' ? Math::DIRECTION_DOWN : Math::DIRECTION_LEFT;
    }

    // The window that way from `from` on its mosaic desktop: its centre lies
    // that way, nearest along the direction and closest across it. Null off a
    // mosaic desktop, or with nothing that way.
    // In the TILES the windows share edges, as in dwindle: Hyprland's own
    // movefocus/swapwindow are dwindle's, so the mosaic's centre rule stands
    // aside there (user, 2026-10-23: the tiles "should behave exactly like
    // dwindle").
    static PHLWINDOW mosaicNeighbour(const PHLWINDOW& from, Math::eDirection dir) {
        if (!from || !from->m_workspace ||
            !std::ranges::any_of(g_instances, [&](const CMosaicAlgorithm* a) { return a->holds(from) && (a->arrangedAsMosaic() || a->arrangedAsCanvas()); }))
            return nullptr;
        const Vector2D D  = dir == Math::DIRECTION_RIGHT ? Vector2D{1, 0} : dir == Math::DIRECTION_LEFT ? Vector2D{-1, 0} : dir == Math::DIRECTION_UP ? Vector2D{0, -1} : Vector2D{0, 1};
        const auto     C0 = from->middle();
        PHLWINDOW      best;
        double         bestScore = 1e18;
        for (const auto& W : Desktop::windowState()->windows()) {
            if (!W || W == from || !W->m_isMapped || W->isHidden() || W->m_workspace != from->m_workspace || !onMosaic(W))
                continue;
            const auto   V     = W->middle() - C0;
            const double ALONG = V.x * D.x + V.y * D.y;
            if (ALONG <= 1.0)
                continue;
            const double ACROSS = std::abs(V.x * D.y - V.y * D.x);
            const double SCORE  = ALONG + 2.0 * ACROSS;
            if (SCORE < bestScore) {
                bestScore = SCORE;
                best      = W;
            }
        }
        return best;
    }

    static SDispatchResult onFocusDispatcher(std::string arg) {
        const auto DIR = dirOf(arg);
        if (const auto BEST = mosaicNeighbour(Desktop::focusState()->window(), DIR)) {
            (void)::Config::Actions::focus(BEST);
            return {};
        }
        (void)::Config::Actions::moveFocus(DIR);
        return {};
    }

    // SWAP BY DIRECTION (user, 2026-10-16: "I also need windows to be switchable
    // with keyboard commands like in pure dwindle, while in mosaic").
    // hl.plugin.hyprdrive.swap("l"|"r"|"u"|"d"), bound to Super+Shift+arrows
    // with the focus binds: Hyprland's swapwindow looks for the window across a
    // shared edge, as movefocus does, and finds none in a bunched cluster. On a
    // mosaic desktop the focused window trades places with the window focus
    // would move to (Hyprland's own swap, so the layout's swapTargets: the
    // focused window is dropped where the other stood -- MosaicPack::PackHint
    // -- still one cluster with no holes);
    // anywhere else, exactly Hyprland's swapwindow.
    static SDispatchResult onSwapDispatcher(std::string arg) {
        const auto DIR     = dirOf(arg);
        const auto FOCUSED = Desktop::focusState()->window();
        if (const auto BEST = mosaicNeighbour(FOCUSED, DIR)) {
            (void)::Config::Actions::swapWith(BEST, FOCUSED);
            return {};
        }
        (void)::Config::Actions::swapInDirection(DIR);
        return {};
    }

    // A WINDOW THAT OPENS FLOATING (user, 2026-10-14: "windows that already
    // come up as floating do not get properly included into the mosaic group").
    // A float -- by a window rule, or because the app asked -- belongs to
    // Hyprland's floating layout, which the mosaic never sees, so it landed on
    // top of the cluster instead of in it. On a mosaic desktop, where every
    // window keeps its own size anyway, it joins the mosaic. Real dialogs stay
    // afloat: anything with a parent window (a transient, an X11 child), a
    // modal, an override-redirect popup, a pinned or fullscreen window. So
    // does a float a window rule tags `hyprdrive-float` (user, 2026-10-15: a
    // Sushi preview joined the mosaic and every window re-packed around it,
    // then back when it closed): a preview or a picker is a passing overlay,
    // not a member of the desktop. 25-rules.lua tags Sushi; a user tags any
    // other app the same way.
    //
    // EVERY DESKTOP WITH general:open_tiled (user, 2026-11-10: "add an option
    // to never load windows in floating mode to general"). The same windows,
    // by the same rules, tile into the desktop's own layout (dwindle,
    // scrolling) as they join a mosaic. Off by default, so a float rule still
    // does what it says.
    static CHyprSignalListener g_openLateHook;
    static CHyprSignalListener g_floatingHook; // g_windowSizes: a window floated

    static bool onMosaicWorkspace(const PHLWORKSPACE& ws) {
        return ws && std::ranges::any_of(g_instances, [&](const CMosaicAlgorithm* a) { return a->workspace() == ws; });
    }

    bool isMosaicWorkspace(const PHLWORKSPACE& ws) {
        return onMosaicWorkspace(ws);
    }

    static bool openTiled() {
        return Hyprdrive::Config::getValue<bool>("plugin:hyprdrive:general:open_tiled");
    }

    static void adoptFloating(const PHLWINDOW& w) {
        if (!w || !w->m_isMapped || !w->m_isFloating || !w->m_workspace)
            return;
        const bool MOSAIC = onMosaicWorkspace(w->m_workspace);
        if (!MOSAIC && !openTiled())
            return;
        if (w->parent() || w->isModal() || w->m_pinned || Fullscreen::controller()->isFullscreen(w) || (w->m_isX11 && (w->isX11OverrideRedirect() || w->x11Parent())))
            return;
        if (w->m_ruleApplicator && w->m_ruleApplicator->m_tagKeeper.isTagged("hyprdrive-float"))
            return;
        // A window that cannot change size is not a mosaic member, like a
        // dialog: min == max (X11 WM_NORMAL_HINTS, or an xdg-toplevel's min and
        // max). Tiled, it was stretched past its fixed size, and an X11 one
        // answered every configure with its own size -- a ConfigureRequest
        // ping-pong (~1200/s) that starved the seat (Battle.net's login window,
        // 362x693, mapped at 1726x1470; 2026-11-06).
        if (const auto MIN = w->minSize(), MAX = w->maxSize();
            MIN && MAX && MIN->x >= 1 && MIN->y >= 1 && std::abs(MAX->x - MIN->x) < 1 && std::abs(MAX->y - MIN->y) < 1) {
            trace(std::format("floating window {:x} is fixed-size ({}x{}): stays floating", (uintptr_t)w.get(), MIN->x, MIN->y));
            return;
        }
        trace(std::format("adopt floating window {:x} into the {}", (uintptr_t)w.get(), MOSAIC ? "mosaic" : "desktop's layout (general:open_tiled)"));
        (void)::Config::Actions::floatWindow(::Config::Actions::TOGGLE_ACTION_DISABLE, w);
        // The open animation, again, from the tile. onMap started it from the
        // FLOATING box before openLate, so the window grew out of the spot it
        // would have floated in and slid across into its tile (rig, 2026-11-10:
        // a rule-floated kitty came out of the screen's centre into the right
        // half). Tiling sets the tile as the goal at once (setFloating ->
        // moveTarget -> recalculate), and no frame has been drawn since onMap,
        // so this is the same popin a window that opened tiled gets.
        if (!MOSAIC && !w->m_isFloating)
            w->m_animationController.apply(w->m_animationController.animateIn());
    }

    static CHyprSignalListener g_desktopShownHook; // a held Super's zoom follows the desktop
    // HELD SUPER CARRIES THE ZOOM ACROSS DESKTOPS (user, 2026-10-30: "when I
    // super+move mouse and canvas zooms out, then I scroll (switches desktops
    // while I hold super), any other canvas that can zoom out should also be
    // zoomed out"). The one arriving zooms out if it is a canvas wider than
    // the screen. The desktop left behind slides away AS IT IS, zoomed out,
    // and drops the zoom once it is off screen (slideWatch): zooming back in
    // while it slid made its windows lag behind the slide and vanish at its
    // end (user, 2026-11-01).
    static void zoomFollowsDesktop(const PHLWORKSPACE& ws) {
        if (g_super != ESuper::ZOOMED || g_zoomPressing || !ws || ws->m_isSpecialWorkspace)
            return;
        auto* A = canvasOf(ws);
        if (A)
            A->m_slide.zoomLeaving = false; // back before it was off screen: it stays zoomed out
        if (auto* OLD = liveZoomed(); OLD && OLD != A) {
            OLD->m_slide.zoomLeaving = true;
            armSlideWatch();
        }
        if (A && A == liveZoomed())
            return;
        g_superCanvas = nullptr;
        if (A && A->zoomOut()) {
            g_superCanvas = A;
            // As when the zoom engaged: no motion reaches the clients while zoomed.
            g_pSeatManager->setPointerFocus(nullptr, {});
        }
    }

    static CHyprSignalListener g_focusReasonHook; // a focus that is not a hover: g_hoverStale is over

    void registerLayout(HANDLE handle) {
        // Before the algo exists: windows re-arrive inside addTiledAlgo.
        loadKept();
        HyprlandAPI::addTiledAlgo(handle, "mosaic", &typeid(CMosaicAlgorithm), [] { return makeUnique<CMosaicAlgorithm>(); });
        Hyprdrive::Config::registerDispatcher("focus", onFocusDispatcher);
        Hyprdrive::Config::registerDispatcher("swap", onSwapDispatcher);
        // After the window's rules are in (openLate). Directly, never a
        // doLater: a deferred call still queued when the plugin unloads jumps
        // into unmapped code (the unload SEGVs, 2026-09).
        g_openLateHook = Event::bus()->m_events.window.openLate.listen([](PHLWINDOW w) { adoptFloating(w); });
        g_desktopShownHook = Event::bus()->m_events.workspace.active.listen([](PHLWORKSPACE ws) { zoomFollowsDesktop(ws); });
        g_floatingHook = Event::bus()->m_events.window.floating.listen([](PHLWINDOW w) { onFloating(w); });
        // Every desktop, not just the canvas: the dock's veto marks a hover
        // stale anywhere (focusUnderPush, which focuses by KEYBIND, marks it
        // again after its own focus).
        g_focusReasonHook = Event::bus()->m_events.window.active.listen([](PHLWINDOW, Desktop::eFocusReason reason) {
            if (reason != Desktop::FOCUS_REASON_FFM)
                g_hoverStale = false;
        });
    }

    void unregisterLayout(HANDLE handle) {
        g_openLateHook.reset();
        g_desktopShownHook.reset();
        g_floatingHook.reset();
        g_focusReasonHook.reset();
        // Before the algo goes: removing it hands the desktops to dwindle.
        saveKept();
        HyprlandAPI::removeAlgo(handle, "mosaic");
    }
}
