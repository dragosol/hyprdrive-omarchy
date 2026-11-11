// Mosaic: the corner grab, the Super gesture and its zoom, the canvas clock, the edge push
// and the hover veto -- the gestures that span desktops. Split out of Mosaic.cpp; the state
// is still there (MosaicParts.hpp).
#include "MosaicParts.hpp"
#include "MotionTimings.hpp"
#include "MotionClock.hpp"


// main.cpp: Super held to pan the canvas is not a Super tap.
namespace Hyprdrive {
    void cancelSuperTap();
}

namespace Hyprdrive::Mosaic {

    using namespace MosaicDetail;

    static bool pickerOpen(); // a screenshot's region picker is up (below)
    // A plain click: the canvas window under the pointer pans into view.
    static void canvasClick();

    // ------------------------------------------------------------ corner grab --
    // CORNER RESIZE (user, 2026-10-11: "if I hold down super and go to the
    // corner of a window i can resize it, that or corner resize is always
    // available in mosaic"). Both:
    //   - on a mosaic desktop, a plain left press on a window's corner resizes
    //     it, and the pointer shows the resize cursor over a corner;
    //   - with Super held, the same press resizes on ANY layout, in place of
    //     Omarchy's Super+drag move -- which still moves from anywhere else.
    // The resize is Hyprland's own drag (beginDragTarget with the corner
    // forced), so tiled layouts get resizeTarget and floating windows their
    // usual live resize. The press is claimed, so Hyprland's bind handler never
    // sees it and would never end the drag: the matching release ends it here.
    namespace MosaicDetail {
        // How far INSIDE a window a corner reaches. Outside, it is Hyprland's
        // own border grab width; inside, only this much, so an app's own
        // controls in its corners keep their clicks.
        constexpr double INNER = 6.0;

        bool onMosaic(const PHLWINDOW& w) {
            return w && std::ranges::any_of(g_instances, [&](const CMosaicAlgorithm* a) { return a->holds(w); });
        }

        // The corner -- or EDGE -- of `w` under `p`, or CORNER_NONE. EDGES (user,
        // 2026-10-15: "drag edges to resize in mosaic ... it only works on
        // corners and not on top/bottom edges"): a side reaches out into the
        // gap like a corner, but INSIDE the window only with Super held, so an
        // app's own top row (a browser's tabs) keeps its clicks.
        Layout::eRectCorner cornerAt(const PHLWINDOW& w, const Vector2D& p, bool superHeld) {
            static auto PBORDER = CConfigValue<::Config::INTEGER>("general:border_size");
            static auto PEXTEND = CConfigValue<::Config::INTEGER>("general:extend_border_grab_area");
            const double OUT    = std::max<double>(8.0, sc<double>(*PBORDER + *PEXTEND));
            const CBox   B      = w->geometricBox(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
            const bool   LEFT   = p.x >= B.x - OUT && p.x <= B.x + INNER;
            const bool   RIGHT  = p.x >= B.x + B.w - INNER && p.x <= B.x + B.w + OUT;
            const bool   TOP    = p.y >= B.y - OUT && p.y <= B.y + INNER;
            const bool   BOTTOM = p.y >= B.y + B.h - INNER && p.y <= B.y + B.h + OUT;
            if (TOP && LEFT)
                return Layout::CORNER_TOPLEFT;
            if (TOP && RIGHT)
                return Layout::CORNER_TOPRIGHT;
            if (BOTTOM && LEFT)
                return Layout::CORNER_BOTTOMLEFT;
            if (BOTTOM && RIGHT)
                return Layout::CORNER_BOTTOMRIGHT;
            const double IN    = superHeld ? INNER : 0.0;
            const bool   SPANX = p.x >= B.x && p.x <= B.x + B.w, SPANY = p.y >= B.y && p.y <= B.y + B.h;
            if (SPANX && p.y >= B.y - OUT && p.y <= B.y + IN)
                return Layout::CORNER_TOP;
            if (SPANX && p.y >= B.y + B.h - IN && p.y <= B.y + B.h + OUT)
                return Layout::CORNER_BOTTOM;
            if (SPANY && p.x >= B.x - OUT && p.x <= B.x + IN)
                return Layout::CORNER_LEFT;
            if (SPANY && p.x >= B.x + B.w - IN && p.x <= B.x + B.w + OUT)
                return Layout::CORNER_RIGHT;
            return Layout::CORNER_NONE;
        }

        const char* cursorFor(Layout::eRectCorner c) {
            switch (c) {
                case Layout::CORNER_TOPLEFT: return "nw-resize";
                case Layout::CORNER_TOPRIGHT: return "ne-resize";
                case Layout::CORNER_BOTTOMLEFT: return "sw-resize";
                case Layout::CORNER_TOP: return "n-resize";
                case Layout::CORNER_BOTTOM: return "s-resize";
                case Layout::CORNER_LEFT: return "w-resize";
                case Layout::CORNER_RIGHT: return "e-resize";
                default: return "se-resize";
            }
        }

        // Which window's corner the pointer is on, if any. The corner zone
        // reaches past the window into the gap, where the hit test finds no
        // window -- so every window of the visible desktops is asked, the
        // focused one first (floating windows overlap).
        std::pair<PHLWINDOW, Layout::eRectCorner> cornerTarget(const Vector2D& p, bool superHeld) {
            const auto TRY = [&](const PHLWINDOW& w) -> Layout::eRectCorner {
                if (!w || !w->m_isMapped || w->isHidden() || !w->m_workspace || !w->m_workspace->isVisible() || Fullscreen::controller()->isFullscreen(w) ||
                    (w->m_isX11 && w->isX11OverrideRedirect()) || !w->layoutTarget())
                    return Layout::CORNER_NONE;
                // Without Super only a TILED window of a mosaic desktop has corners.
                if (!superHeld && (w->layoutTarget()->floating() || !onMosaic(w)))
                    return Layout::CORNER_NONE;
                return cornerAt(w, p, superHeld);
            };
            if (const auto F = Desktop::focusState()->window(); F)
                if (const auto C = TRY(F); C != Layout::CORNER_NONE)
                    return {F, C};
            for (const auto& W : Desktop::windowState()->windows())
                if (const auto C = TRY(W); C != Layout::CORNER_NONE)
                    return {W, C};
            return {nullptr, Layout::CORNER_NONE};
        }

        // Plain (no modifier) or Super alone; anything else is someone's chord.
        int modsNow() {
            return sc<int>(g_pInputManager->getModsFromAllKBs());
        }
    }

    // A mosaic resize ends when the drag controller lets the window go --
    // whoever started the drag (our corner grab, or Super + right-drag). Asked
    // when our own drag ends, and on every pointer motion.
    static void checkResizeEnded() {
        const auto DRAGGED = g_layoutManager ? g_layoutManager->dragController()->target() : SP<Layout::ITarget>{};
        for (auto* A : g_instances)
            if (A->resizing() && !(DRAGGED && A->resizingTarget(DRAGGED)))
                A->finishResize();
    }

    static void canvasClick() {
        const auto W = Desktop::viewState()->hitTest().windowAt(g_pInputManager->getMouseCoordsInternal(),
                                                                 Desktop::View::RESERVED_EXTENTS | Desktop::View::INPUT_EXTENTS | Desktop::View::ALLOW_FLOATING);
        if (!W)
            return;
        for (auto* A : g_instances)
            if (A->holds(W)) {
                A->clickReveal(W);
                return;
            }
    }

    // ------------------------------------------------ Super held, pointer moved --
    // What a held Super is doing on the canvas. ARMED: Super is down, nothing
    // decided yet. A button pressed then (Super+drag, Super+click) SPENDS it:
    // no zoom until Super is let go -- the window being aimed at stays
    // put, and so does the canvas after the drop (user, 2026-10-26: Super+drag
    // "is buggy"; the pan used to fire on the way to the window and again
    // after the drop). Moving SUPER_SLOP px with no button ENGAGES it: the
    // whole canvas shrinks onto the screen, hovering a window focuses it, and
    // letting go of Super zooms back in on it. (A second mode, where the
    // pointer's place on the screen was the view's place on the canvas, was
    // retired: user, 2026-10-29. mosaic:canvas_super_move is still accepted
    // so older configs load, and ignored.)
    namespace {
        constexpr double  SUPER_SLOP = 32.0;

        // THE TITLES FADE (user, 2026-11-01: "those labels should adopt the
        // same fade in/out animations as the rest of the labels in
        // hyprdrive"): the dock tooltip's clock and curves (Dock.cpp
        // TOOLTIP_FADE_MS / _OUT_MS, houseEase), one per window. A title wanted
        // again mid-exit turns around from the alpha it had, no dip to zero.
        constexpr float LABEL_FADE_MS     = Hyprdrive::Motion::LABEL_FADE_IN_MS;  // MotionTimings.hpp
        constexpr float LABEL_FADE_OUT_MS = Hyprdrive::Motion::LABEL_FADE_OUT_MS; // exits ~25% quicker, house rule

        float labelFadeProgress(const SLabelFade& f) {
            // The animation clock, like the dock tooltip it copies.
            const float MS = Hyprdrive::Motion::Clock::animMs(f.since);
            return std::clamp(MS / (f.out ? LABEL_FADE_OUT_MS : LABEL_FADE_MS), 0.F, 1.F);
        }
        float labelFadeAlpha(const SLabelFade& f) {
            const float P = labelFadeProgress(f);
            return f.out ? f.from * (1.F - houseEase(false, P)) : f.from + (1.F - f.from) * houseEase(true, P);
        }
        void labelFadeTo(SLabelFade& f, bool out) {
            f.from  = labelFadeAlpha(f);
            f.out   = out;
            f.since = std::chrono::steady_clock::now();
        }
    }

    // A SCREEN PICKER IS UP (user, 2026-10-27: "fix taking screenshots while
    // zoomed out"): a region picker (slurp, hyprpicker's freeze) is a layer
    // surface over a whole monitor on the overlay layer. While one is up the
    // zoomed-out canvas stays drawn for the screenshot, but the pointer and
    // its buttons are the picker's.
    static bool pickerOpen() {
        for (const auto& M : State::monitorState()->monitors())
            for (const auto& LSREF : M->m_layerSurfaceLayers[ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY]) {
                const auto LS = LSREF.lock();
                if (!Desktop::View::validMapped(LS))
                    continue;
                const auto& G = LS->m_geometry;
                if (G.w >= M->m_size.x - 1 && G.h >= M->m_size.y - 1)
                    return true;
            }
        return false;
    }

    // The titles come down (Super let go, or spent on a click or a drag).
    static void dropSuperLabels() {
        if (!g_superLabels)
            return;
        g_superLabels = false;
        g_labelHover.reset();
        // Their exits start now, not at the next frame drawn without the
        // overview: one that opens on this release must not leave a stale
        // title to fade out when it closes.
        for (auto& f : g_labelFades)
            if (!f.out)
                labelFadeTo(f, true);
        for (const auto& M : State::monitorState()->monitors())
            if (M)
                g_pHyprRenderer->damageMonitor(M);
    }


    namespace MosaicDetail {
    CMosaicAlgorithm* liveZoomed() {
        return g_superCanvas && std::ranges::find(g_instances, g_superCanvas) != g_instances.end() ? g_superCanvas : nullptr;
    }
    }

    // The gesture is over (Super let go, another modifier joined, unload).
    static void superEnd(bool superStillDown) {
        dropSuperLabels();
        if (auto* A = liveZoomed()) {
            // Let go of Super mid-drag: the drag is off; the button's release,
            // still to come, is ours.
            if (g_zoomPressing) {
                A->zoomDragCancel();
                g_zoomPressing   = false;
                g_swallowRelease = true;
            }
            A->zoomIn();
        }
        g_superCanvas = nullptr;
        if (g_super == ESuper::ZOOMED)
            g_canvasPanning = false;
        g_super      = superStillDown && !g_fingerZoom ? ESuper::SPENT : ESuper::IDLE;
        g_fingerZoom = false;
        // Desktops kept zoomed out off screen for a swipe end now (slideWatch).
        armSlideWatch();
    }

    void onSuperKey(bool pressed) {
        if (pressed) {
            if (g_super == ESuper::IDLE) {
                g_super       = ESuper::ARMED;
                g_superAnchor = g_pInputManager->getMouseCoordsInternal();
            }
            return;
        }
        superEnd(false);
    }

    // AN OVERVIEW OPENS, THE ZOOM ENDS (2026-11-09). The fingers' zoom stays
    // up after they lift, and a Super zoom is still up when a Super chord or a
    // hot corner opens the overview. Nothing ended either: the overview took
    // each press, and so did the zoom under it (onButton, which runs first) --
    // a press grabbed or picked the canvas window under that spot, a tile
    // dragged in the overview carried a canvas window along, and the zoom came
    // back in on the wrong window. superEnd(true): a Super still held for the
    // zoom stays spent, so the canvas does not zoom out again the moment the
    // overview closes (the fingers' zoom goes back to idle either way).
    void onOverviewOpen() {
        if (g_super == ESuper::ZOOMED || liveZoomed())
            superEnd(true);
    }


    // ---- THE THREE-FINGER ZOOM (CCanvasZoomGesture, OverviewGesture.cpp)
    // fingerZoomBegin: the gesture starts on the focused monitor. 1 = zooming
    // out, -1 = zooming back in (it was zoomed out by the fingers already),
    // 0 = not a canvas that can zoom out (the caller falls back).
    int fingerZoomBegin() {
        const auto MON = Desktop::focusState()->monitor();
        if (!MON || MON->m_activeSpecialWorkspace || openScrollOverviewForMonitor(MON))
            return 0;
        auto* A = canvasOf(MON->m_activeWorkspace);
        if (!A)
            return 0;
        if (g_fingerZoom && liveZoomed() == A && g_super == ESuper::ZOOMED) {
            if (!A->zoomFollowStart())
                return 0;
            // BACK IN ON THE WINDOW THE POINTER PICKED (user, 2026-11-09:
            // "does not zoom back in to the location of my cursor it zooms back
            // in to where I zoomed out from"). Its hover gave it the focus
            // (superMove), but the zoom in was aimed only as the fingers
            // lifted: they drew the canvas in on the old view, and then it
            // jumped. Aimed now, the fingers draw it in toward that window.
            A->aimZoomIn();
            return -1;
        }
        if (g_super != ESuper::IDLE || !A->zoomFollowStart())
            return 0;
        g_super       = ESuper::ZOOMED;
        g_superCanvas = A;
        g_fingerZoom  = true;
        // THE TITLES, AS FOR SUPER'S ZOOM (user, 2026-11-09: "zooming out with 3
        // fingers should show labels on mouseovered windows or all windows").
        // mosaic:zoom_labels picks which; they fade in over the zoom's second
        // half (zoomLabelAlpha) and out when it ends (superEnd).
        g_superLabels = true;
        g_labelHover.reset();
        g_pSeatManager->setPointerFocus(nullptr, {});
        g_canvasPanning = true;
        g_pHyprRenderer->damageMonitor(MON);
        return 1;
    }
    void fingerZoomFollow(double z) {
        if (auto* A = liveZoomed(); A && g_fingerZoom)
            A->zoomFollow(z);
    }
    void fingerZoomEnd(bool out) {
        auto* A = liveZoomed();
        if (!A || !g_fingerZoom)
            return;
        if (out) {
            A->zoomSettle(true);
            return;
        }
        superEnd(false); // back in, on the focused window
    }
    double fingerZoomLevel() {
        const auto* A = liveZoomed();
        return A && g_fingerZoom ? A->zoomLevel() : 0.0;
    }

    void renderZoomLabels(PHLMONITOR monitor) {
        if (!monitor)
            return;
        auto*      A      = liveZoomed();
        const auto WS     = A ? A->workspace() : nullptr;
        const bool ZOOMED = WS && monitor->m_activeWorkspace == WS && !monitor->m_activeSpecialWorkspace;
        const auto SHOWN  = monitor->m_activeSpecialWorkspace ? monitor->m_activeSpecialWorkspace : monitor->m_activeWorkspace;

        // The windows that want a title this frame. None once Super is let go:
        // the titles still up fade out below.
        std::vector<PHLWINDOW> want;
        const auto             MODE = Hyprdrive::Config::getValue<std::string>("plugin:hyprdrive:mosaic:zoom_labels");
        if (g_superLabels && g_super != ESuper::IDLE && g_super != ESuper::SPENT && !openScrollOverviewForMonitor(monitor) && (MODE == "hover" || MODE == "all")) {
            // Looked up fresh, not g_labelHover (that only asks for the redraws):
            // a desktop switch under a still pointer changes the window under it.
            const Vector2D P     = g_pInputManager->getMouseCoordsInternal();
            const auto     HOVER = ZOOMED ? A->zoomedWindowAt(P) :
                                            Desktop::viewState()->hitTest().windowAt(P, Desktop::View::RESERVED_EXTENTS | Desktop::View::INPUT_EXTENTS | Desktop::View::ALLOW_FLOATING);
            // The zoomed-out canvas: on the zoomed boxes.
            if (ZOOMED)
                A->zoomLabelWindows(MODE == "all", HOVER, want);
            // Any other desktop, any layout: the windows where they are. Under a
            // fullscreen window only it and what floats over it are seen.
            else if (SHOWN) {
                const auto FS = Fullscreen::controller()->getFullscreenWindow(SHOWN);
                for (const auto& W : Desktop::windowState()->windows()) {
                    if (!W || !W->m_isMapped || W->isHidden() || W->m_workspace != SHOWN || (FS && W != FS && !W->m_isFloating))
                        continue;
                    if (MODE == "hover" && W != HOVER)
                        continue;
                    want.push_back(W);
                }
            }
        }

        // Each title in or out on its own fade. Only this monitor's windows:
        // another monitor's render decides theirs.
        std::erase_if(g_labelFades, [](const SLabelFade& f) { return !f.window.lock() || (f.out && labelFadeProgress(f) >= 1.F); });
        for (const auto& W : want) {
            auto it = std::ranges::find_if(g_labelFades, [&](const SLabelFade& f) { return f.window.lock() == W; });
            if (it == g_labelFades.end())
                g_labelFades.push_back({.window = W, .since = std::chrono::steady_clock::now()});
            else if (it->out)
                labelFadeTo(*it, false);
        }
        for (auto& f : g_labelFades) {
            const auto W = f.window.lock();
            if (!f.out && W->m_monitor == monitor && std::ranges::find(want, W) == want.end())
                labelFadeTo(f, true);
        }

        for (const auto& f : g_labelFades) {
            const auto W = f.window.lock();
            if (W->m_monitor != monitor || !W->m_isMapped)
                continue;
            // On the zoomed canvas under the zoom's own fade; elsewhere only a
            // window on the desktop shown, as it is.
            const bool ONZOOM = ZOOMED && A->holds(W);
            if (!ONZOOM && (W->isHidden() || W->m_workspace != SHOWN))
                continue;
            const float ALPHA = labelFadeAlpha(f) * (ONZOOM ? A->zoomLabelAlpha() : 1.F);
            if (ALPHA > 0.005F)
                drawWindowLabel(monitor, W, ALPHA);
        }
    }

    // One frame past the end too, so the last one drawn is the settled alpha
    // (COverview::stripTipAnimating).
    bool labelsAnimating() {
        const auto NOW = std::chrono::steady_clock::now();
        return std::ranges::any_of(g_labelFades, [&](const SLabelFade& f) {
            return f.window.lock() && Hyprdrive::Motion::Clock::animMsBetween(NOW, f.since) < (f.out ? LABEL_FADE_OUT_MS : LABEL_FADE_MS) + 16.F;
        });
    }

    PHLWINDOW zoomCarried() {
        auto* A = liveZoomed();
        return A && g_zoomPressing ? A->zoomCarried() : nullptr;
    }

    // A ZOOMED-OUT DRAG HELD AT THE SCREEN'S EDGE (user, 2026-10-29: pushing a
    // window to the next desktop "doesn't work while zoomed out in infinite
    // canvas"; "take into account when I'm pushing the window to a desktop
    // that isn't set to mosaic"). The zoomed drag is the canvas's own, and a
    // switch needs a window Hyprland is moving (Spring: the pin carries it,
    // the release is finished across the switch). So the hold, complete, hands
    // it over: the zoom ends where it is and the window becomes an ordinary
    // Super+drag under the hand -- which any desktop, mosaic or not, takes as
    // it takes any dragged window, and can be pushed on again.
    bool handZoomDragOff() {
        auto*      A = liveZoomed();
        const auto W = zoomCarried();
        if (!A || !W || !W->layoutTarget() || !g_layoutManager || g_layoutManager->dragController()->target())
            return false;
        const CBox HERE = A->zoomCarriedBox(g_pInputManager->getMouseCoordsInternal());
        A->zoomDragCancel();
        g_zoomPressing = false;
        A->endZoomNow();
        g_superCanvas   = nullptr;
        g_canvasPanning = false;
        g_super         = ESuper::SPENT; // Super still down: no new zoom until it is let go
        dropSuperLabels();
        g_layoutManager->beginDragTarget(W->layoutTarget(), MBIND_MOVE);
        // It grows from where it was drawn, under the hand, to its own size
        // there -- not from its place on the canvas (off the screen, as often
        // as not), which the zoom's end had put it back to.
        if (W->positionAnimation() && W->sizeAnimation() && HERE.w >= 1 && HERE.h >= 1) {
            W->positionAnimation()->setValue(HERE.pos());
            W->sizeAnimation()->setValue(HERE.size());
        }
        // Its release is ours (onButton), as a corner resize's is.
        g_ourDrag = g_layoutManager->dragController()->target() != nullptr;
        return g_ourDrag;
    }

    // ALL DIRECTIONS (user, 2026-10-26: "I can hold down super and my cursor
    // movement maps to the entire collection of the screens so that ... the
    // top/bottom portions of the screen result in my cursor being at the
    // top/bottom of the canvas"). Super alone held, no button: the pointer's
    // place on its monitor is the view's place on the canvas, both axes.
    bool onButton(const IPointer::SButtonEvent& e, bool blocked) {
        const bool PRESSED = e.state == WL_POINTER_BUTTON_STATE_PRESSED;
        // THE SUPER GESTURE. Zoomed out, a click picks the window under it and
        // zooms back in on it -- the windows are drawn small, the app would
        // get the click at the wrong place. Armed, a press is a Super+click
        // or Super+drag: no pan or zoom until Super is let go.
        // Zoomed out, the LEFT button grabs the window under it (zoomPress):
        // dragged, it is dropped where it is let go; a click picks it.
        // The RIGHT button resizes the window under it from its nearest
        // corner (Super + right-drag, as on the desktop).
        const bool ZOOMBUTTON = e.button == BTN_LEFT || e.button == BTN_RIGHT;
        if (g_super == ESuper::ZOOMED && !g_zoomPressing && !g_swallowRelease && pickerOpen())
            return false; // a screenshot's region picker: its click
        if (ZOOMBUTTON && PRESSED && g_super == ESuper::ZOOMED && !g_zoomPressing)
            if (auto* A = liveZoomed(); A && A->zoomPress(g_pInputManager->getMouseCoordsInternal(), e.button == BTN_RIGHT)) {
                g_zoomPressing = true;
                g_zoomButton   = e.button;
                return true;
            }
        if (!PRESSED && g_zoomPressing && e.button == g_zoomButton) {
            g_zoomPressing = false;
            auto*          A = liveZoomed();
            const Vector2D P = g_pInputManager->getMouseCoordsInternal();
            // A right click that never moved resizes nothing: still zoomed out.
            // A click picks, as CLICK: passed as a hover it met the dock band's
            // veto, and a pick below the dock's top edge was refused (2026-11-09).
            if (A && !A->zoomRelease(P) && e.button == BTN_LEFT) {
                if (const auto W = A->zoomedWindowAt(P))
                    Desktop::focusState()->fullWindowFocus(W, Desktop::FOCUS_REASON_CLICK);
                superEnd(true);
            }
            return true;
        }
        if (PRESSED && g_super == ESuper::ZOOMED) {
            if (auto* A = liveZoomed()) {
                if (const auto W = A->zoomedWindowAt(g_pInputManager->getMouseCoordsInternal()))
                    Desktop::focusState()->fullWindowFocus(W, Desktop::FOCUS_REASON_CLICK);
            }
            superEnd(true);
            g_swallowRelease = true;
            return true;
        }
        if (!PRESSED && g_swallowRelease) {
            g_swallowRelease = false;
            return true;
        }
        if (PRESSED && g_super == ESuper::ARMED) {
            g_super = ESuper::SPENT;
            dropSuperLabels(); // a Super+click or Super+drag: no titles over it
        }
        // A plain left click on a canvas window pans it into view (clickReveal).
        if (PRESSED && !blocked && e.button == BTN_LEFT && modsNow() == 0 && !(g_layoutManager && g_layoutManager->dragController()->target()))
            canvasClick();
        if (!PRESSED) {
            if (g_ourDrag && e.button == BTN_LEFT) {
                g_ourDrag = false;
                if (g_layoutManager && g_layoutManager->dragController()->target())
                    g_layoutManager->endDragTarget();
                // Our drag is over now: settle the resize at once. (A drag
                // Hyprland started -- Super+right-drag -- ends after this hook;
                // the next pointer motion settles it. No doLater: one still
                // queued at unload jumps into unmapped code.)
                checkResizeEnded();
                return true;
            }
            return false;
        }
        if (blocked || e.button != BTN_LEFT || !g_layoutManager || g_layoutManager->dragController()->target())
            return false;
        const int MODS = modsNow();
        if (MODS != 0 && MODS != HL_MODIFIER_META)
            return false;
        const auto [W, CORNER] = cornerTarget(g_pInputManager->getMouseCoordsInternal(), MODS == HL_MODIFIER_META);
        if (!W || CORNER == Layout::CORNER_NONE)
            return false;
        g_layoutManager->beginDragTarget(W->layoutTarget(), MBIND_RESIZE, CORNER);
        g_ourDrag = g_layoutManager->dragController()->target() != nullptr;
        return g_ourDrag;
    }

    // ------------------------------------------------------------ the canvas view --
    namespace MosaicDetail {
    CMosaicAlgorithm* canvasOf(const PHLWORKSPACE& ws) {
        if (!ws)
            return nullptr;
        for (auto* A : g_instances)
            if (A->workspace() == ws && A->arrangedAsCanvas())
                return A;
        return nullptr;
    }
    }

    std::optional<CBox> canvasBox(const PHLWORKSPACE& ws) {
        const auto* A = canvasOf(ws);
        const auto  V = A ? A->canvasViewNow() : std::nullopt;
        const auto  S = A ? A->canvasSize() : Vector2D{};
        if (!V || S.x < 1 || S.y < 1)
            return std::nullopt;
        return CBox{V->originX - A->canvasOffset().x, V->originY - A->canvasOffset().y, S.x, S.y};
    }

    // ---------------------------------------------------- the canvas's clock --
    namespace {
        constexpr int     CANVAS_TICK_MS = 8; // half a 60 Hz frame: always one booked
        constexpr double  EDGE_ZONE  = 4.0;    // px from the outer edge
        constexpr double  EDGE_SPEED = 1400.0; // canvas px per second
    }
    static bool edgeTick(std::chrono::steady_clock::time_point now);

    static int canvasTickCb(void*) {
        const auto NOW  = std::chrono::steady_clock::now();
        bool       busy = false;
        // A copy: a tick can re-lay a desktop out, never add or drop a layout,
        // but be safe against it anyway.
        for (auto* A : std::vector<CMosaicAlgorithm*>{g_instances})
            busy |= A->tick(NOW);
        busy |= edgeTick(NOW);
        if (busy && g_canvasTimer)
            wl_event_source_timer_update(g_canvasTimer, CANVAS_TICK_MS);
        return 0;
    }

    namespace MosaicDetail {
    void armCanvasTick() {
        if (!g_canvasTimer)
            g_canvasTimer = wl_event_loop_add_timer(g_pCompositor->m_wlEventLoop, canvasTickCb, nullptr);
        if (g_canvasTimer)
            wl_event_source_timer_update(g_canvasTimer, 1);
    }
    }

    static CMosaicAlgorithm* reachingOn(const PHLWORKSPACE& ws);

    // ONLY AS FAR AS THE HAND MOVES (user, 2026-10-27: "it shouldn't just keep
    // expanding as long as my drag is held against the corner, it leads to
    // uncontrollable resizing. It should only expand as far as I'm moving the
    // cursor"). The pointer stops at the screen's edge; what the mouse moved
    // past it -- `delta` minus what the pointer actually moved -- is what the
    // resize reaches past the edge by. Held still, nothing grows.
    // HOVER FOCUS VETO (user, 2026-11-09: "right now if I push through the
    // lower boundary it selects the window ... even if I'm mouseovered the
    // dock"): focus following the mouse does not go to a canvas window that
    // only peeks in at the screen's edge, nor to anything while the pointer is
    // on the dock, a dock menu or a folder's fan. A click, a key or the dock
    // still focuses it.
    // ZOOMED OUT, NONE OF IT APPLIES (2026-11-09): the pointer is the zoom's
    // (zoomOwnsPointer), and its hover picks a window drawn small, not the
    // one under the pointer at full size. The dock still reveals over the
    // zoomed canvas, and its band refused every pick below the pill's top
    // edge -- the zoom then came back in on the window focused before.
    // NO PASS FOR A RIDE (2026-11-09): a focus that rideCursor's warp caused
    // is the hand's even less than a hover is, so the ride does not let it
    // past the dock and the edge rules. (The old `g_cursorRiding` pass here
    // never fired: the flag it read was shadowed. Hyprland's warpTo sends no
    // motion, so today no focus comes of a ride at all.)
    bool vetoHoverFocus(const PHLWINDOW& w) {
        if (!w || zoomOwnsPointer())
            return false;
        if (pointerOnChrome(g_pInputManager->getMouseCoordsInternal()) || pointerInDockBand(g_pInputManager->getMouseCoordsInternal())) {
            g_hoverStale = true;
            return true;
        }
        // Only while the pointer is pressed against an outer edge -- pushing.
        // A window showing a strip at the side is hovered as any other
        // (user, 2026-11-09: the left-edge windows could not be focused).
        // The edge hoverRefresh and the focus hook's push test use too
        // (pointerAtOuterEdge), so the three cannot drift apart.
        const bool  EDGE = pointerAtOuterEdge(g_pInputManager->getMouseCoordsInternal());
        const auto* C    = EDGE && w->m_workspace ? canvasOf(w->m_workspace) : nullptr;
        const bool  V    = C && C->isSliver(w);
        g_hoverStale |= V;
        return V;
    }

    void onPointerDelta(const Vector2D& delta, const Vector2D& moved) {
        const Vector2D LOST = delta - moved;
        const bool     NONE = std::abs(LOST.x) < 0.01 && std::abs(LOST.y) < 0.01;
        if (NONE && (std::abs(moved.x) < 0.01 && std::abs(moved.y) < 0.01))
            return;
        const Vector2D P   = g_pInputManager->getMouseCoordsInternal();
        const auto     MON = State::monitorState()->query().vec(P).run();
        if (!MON || MON->m_activeSpecialWorkspace)
            return;
        if (NONE) {
            if (auto* C = canvasOf(MON->m_activeWorkspace))
                C->backFromPush(P, MON->logicalBox());
            return;
        }
        auto* A = reachingOn(MON->m_activeWorkspace);
        // No resize reaching: a canvas desktop may be pushed through instead
        // (pushThrough) -- not under a Super+drag, which pans by the clock
        // (updateEdgePush).
        auto* C = A ? nullptr : canvasOf(MON->m_activeWorkspace);
        if (C && g_layoutManager && g_layoutManager->dragController()->target())
            C = nullptr;
        if (!A && !C)
            return;
        // Only past an outer edge (no monitor that way) the pointer is at.
        const CBox B     = MON->logicalBox();
        const auto OUTER = [&](Math::eDirection d) {
            const auto M = State::monitorState()->query().relativeTo(MON).inDirection(d).run();
            return !M || M == MON;
        };
        Vector2D r;
        if (LOST.x > 0 && P.x >= B.x + B.w - 2 && OUTER(Math::DIRECTION_RIGHT))
            r.x = LOST.x;
        else if (LOST.x < 0 && P.x <= B.x + 1 && OUTER(Math::DIRECTION_LEFT))
            r.x = LOST.x;
        if (canvasAllDirections()) {
            if (LOST.y > 0 && P.y >= B.y + B.h - 2 && OUTER(Math::DIRECTION_DOWN))
                r.y = LOST.y;
            else if (LOST.y < 0 && P.y <= B.y + 1 && OUTER(Math::DIRECTION_UP))
                r.y = LOST.y;
        }
        if (r.x == 0 && r.y == 0)
            return;
        if (A)
            A->resizeReach(r);
        else if (C->pushThrough(r))
            trace(std::format("push through the edge {}x{} -> view {}x{}", r.x, r.y, C->canvasOffset().x, C->canvasOffset().y));
    }

    // The desktop on `ws` whose resize may reach past the screen's edge.
    static CMosaicAlgorithm* reachingOn(const PHLWORKSPACE& ws) {
        if (!ws)
            return nullptr;
        for (auto* A : g_instances)
            if (A->workspace() == ws && A->resizeCanReach())
                return A;
        return nullptr;
    }

    static bool edgeTick(std::chrono::steady_clock::time_point now) {
        if (g_edgePush.x == 0 && g_edgePush.y == 0)
            return false;
        const auto DRAGGED = g_layoutManager ? g_layoutManager->dragController()->target() : SP<Layout::ITarget>{};
        auto*      A       = canvasOf(g_edgeWorkspace.lock());
        // A resize is never panned by the clock: it reaches past the edge by
        // the motion the edge swallows (onPointerDelta).
        if (!DRAGGED || !A || reachingOn(g_edgeWorkspace.lock())) {
            g_edgePush = {};
            return false;
        }
        const double DT = std::min(0.05, Hyprdrive::Motion::secondsBetween<double>(now, g_edgeLast));
        g_edgeLast      = now;
        const auto O    = A->canvasOffset();
        A->setCanvasOffset(O + g_edgePush * (EDGE_SPEED * DT), true);
        return true;
    }

    bool canvasMotion(const PHLWINDOW& w) {
        if (!w || !w->m_workspace)
            return false;
        for (const auto* A : g_instances)
            if (A->holds(w))
                return (A->arrangedAsCanvas() || A->resizeCanReach()) &&
                    (A->viewMoving() || g_canvasPanning || ((g_edgePush.x != 0 || g_edgePush.y != 0) && g_edgeWorkspace.lock() == w->m_workspace));
        return false;
    }

    // Is the pointer pushing at an outer edge of its monitor (no monitor
    // beyond it) while a Super+drag is over a canvas desktop?
    static void updateEdgePush() {
        const auto DRAGGED = g_layoutManager ? g_layoutManager->dragController()->target() : SP<Layout::ITarget>{};
        Vector2D   push;
        PHLWORKSPACE ws;
        if (DRAGGED && DRAGGED->window()) {
            const Vector2D P   = g_pInputManager->getMouseCoordsInternal();
            const auto     MON = State::monitorState()->query().vec(P).run();
            if (MON && !MON->m_activeSpecialWorkspace && canvasOf(MON->m_activeWorkspace) && !reachingOn(MON->m_activeWorkspace)) {
                const CBox B       = MON->logicalBox();
                // An outer edge: no monitor that way (vec() would answer the
                // CLOSEST monitor, this one, for a point past the edge).
                const auto OUTER = [&](Math::eDirection d) {
                    const auto M = State::monitorState()->query().relativeTo(MON).inDirection(d).run();
                    return !M || M == MON;
                };
                if (P.x <= B.x + EDGE_ZONE && OUTER(Math::DIRECTION_LEFT))
                    push.x = -1;
                else if (P.x >= B.x + B.w - EDGE_ZONE && OUTER(Math::DIRECTION_RIGHT))
                    push.x = 1;
                if (canvasAllDirections()) {
                    if (P.y <= B.y + EDGE_ZONE && OUTER(Math::DIRECTION_UP))
                        push.y = -1;
                    else if (P.y >= B.y + B.h - EDGE_ZONE && OUTER(Math::DIRECTION_DOWN))
                        push.y = 1;
                }
                ws = MON->m_activeWorkspace;
            }
        }
        const bool START = (push.x != 0 || push.y != 0) && g_edgePush.x == 0 && g_edgePush.y == 0;
        g_edgePush       = push;
        g_edgeWorkspace  = ws;
        if (START) {
            g_edgeLast = std::chrono::steady_clock::now();
            armCanvasTick();
        }
    }

    // The Super gesture, on every pointer motion: the canvas shrinks onto the
    // screen once, and hovering a window focuses it.
    static void superMove(bool blocked) {
        // ZOOMED OUT BY THE FINGERS: no Super is held, and none is needed --
        // it stays until a click picks a window (onButton) or Super is let go.
        if (g_fingerZoom) {
            auto* A = liveZoomed();
            if (!A || g_super != ESuper::ZOOMED) {
                g_fingerZoom = false;
                return;
            }
            const Vector2D P = g_pInputManager->getMouseCoordsInternal();
            if (g_zoomPressing) {
                A->zoomMove(P);
                return;
            }
            if (pickerOpen() || blocked)
                return;
            const auto W = A->zoomedWindowAt(P);
            // The hovered title (mosaic:zoom_labels "hover"): a new one redraws,
            // so the last one's pill goes with it.
            if (W != g_labelHover.lock()) {
                g_labelHover = W;
                if (const auto WS = A->workspace())
                    if (const auto MON = WS->m_monitor.lock())
                        g_pHyprRenderer->damageMonitor(MON);
            }
            if (W && W != Desktop::focusState()->window())
                Desktop::focusState()->fullWindowFocus(W, Desktop::FOCUS_REASON_FFM);
            return;
        }
        const int MODS = modsNow();
        if (!(MODS & HL_MODIFIER_META)) {
            if (g_super != ESuper::IDLE)
                superEnd(false);
            return;
        }
        if (MODS != HL_MODIFIER_META) {
            // Another modifier joined: someone's chord, not this. Zoomed out,
            // the view stays until Super is let go: Super+Shift+S takes a
            // screenshot of the zoomed-out canvas, not of the view behind it.
            if (g_super != ESuper::IDLE && g_super != ESuper::SPENT && g_super != ESuper::ZOOMED)
                superEnd(true);
            return;
        }
        const Vector2D P = g_pInputManager->getMouseCoordsInternal();
        if (g_super == ESuper::IDLE) {
            // Super went down before we heard it (held across a reload).
            g_super       = ESuper::ARMED;
            g_superAnchor = P;
        }
        if (g_super == ESuper::SPENT || blocked || g_cursorRiding || (g_layoutManager && g_layoutManager->dragController()->target()))
            return;
        const auto MON = State::monitorState()->query().vec(P).run();
        if (!MON || openScrollOverviewForMonitor(MON))
            return;
        // TITLES WHILE SUPER IS HELD (user, 2026-10-30: "have them appear when
        // I'm holding super and moving my mouse, even if it doesn't zoom out,
        // and have the labels work in any desktop mode"): past the slop they
        // come up on any desktop, canvas or not; the zoom below is the
        // canvas's own. mosaic:zoom_labels picks none, the hovered window, or
        // all. The hovered one is tracked here, and a change redraws the whole
        // screen so the last pill goes with it.
        if (!g_superLabels) {
            const Vector2D D = P - g_superAnchor;
            if (g_super == ESuper::ZOOMED || D.x * D.x + D.y * D.y >= SUPER_SLOP * SUPER_SLOP) {
                g_superLabels = true;
                g_pHyprRenderer->damageMonitor(MON);
            }
        }
        if (g_superLabels) {
            const auto* Z = liveZoomed();
            const auto  W = Z ? Z->zoomedWindowAt(P) :
                                Desktop::viewState()->hitTest().windowAt(P, Desktop::View::RESERVED_EXTENTS | Desktop::View::INPUT_EXTENTS | Desktop::View::ALLOW_FLOATING);
            if (W != g_labelHover.lock()) {
                g_labelHover = W;
                g_pHyprRenderer->damageMonitor(MON);
            }
        }
        if (MON->m_activeSpecialWorkspace)
            return;
        auto*      A = canvasOf(MON->m_activeWorkspace);
        const auto V = A ? A->canvasViewNow() : std::nullopt;
        if (!V || (V->maxX <= 1.0 && V->maxY <= 1.0))
            return;

        if (g_super == ESuper::ARMED) {
            const Vector2D D = P - g_superAnchor;
            if (D.x * D.x + D.y * D.y < SUPER_SLOP * SUPER_SLOP)
                return;
            if (!A->zoomOut())
                return;
            g_super       = ESuper::ZOOMED;
            g_superCanvas = A;
            // The window under the pointer at full size lets go of it
            // (zoomOwnsPointer: no motion reaches the clients while zoomed).
            g_pSeatManager->setPointerFocus(nullptr, {});
            g_canvasPanning = true;
            // Super was held for this, not tapped: no overview on its release.
            Hyprdrive::cancelSuperTap();
        }

        if (g_super == ESuper::ZOOMED && g_zoomPressing) {
            A->zoomMove(P); // carrying a window (zoomPress)
            return;
        }
        if (g_super == ESuper::ZOOMED && pickerOpen())
            return; // the picker has the pointer: no hover picks under it
        if (g_super == ESuper::ZOOMED) {
            // Hovering a window focuses it (as focus follows the mouse, but
            // whatever input:follow_mouse says: this is the zoom's own pick).
            if (const auto W = A->zoomedWindowAt(P); W && W != Desktop::focusState()->window())
                Desktop::focusState()->fullWindowFocus(W, Desktop::FOCUS_REASON_FFM);
            return;
        }
    }

    // THE PANEL TURNS THE CANVAS ON OR OFF LIVE (user, 2026-10-26: "when
    // turning off infinite canvas in settings, it should immediately go back
    // to regular mosaic"): the panel applies a key with `hyprctl eval
    // hl.config(...)`, which is no config reload, so the reload hook never
    // heard it. Two cached reads per pointer motion (the hand that just
    // clicked the switch moves on) see the change instead.
    static void watchCanvasKeys() {
        if (!Hyprdrive::Config::g_pluginKeysRegistered)
            return;
        static auto&       ON   = Hyprdrive::Config::valueRef<Hyprlang::INT>("plugin:hyprdrive:mosaic:infinite_canvas");
        static auto&       DIRS = Hyprdrive::Config::valueRef<std::string>("plugin:hyprdrive:mosaic:canvas_directions");
        static int64_t     lastOn   = -1;
        static std::string lastDirs;
        const int64_t      NOWON    = *ON;
        const std::string& NOWDIRS  = *DIRS;
        if (NOWON == lastOn && NOWDIRS == lastDirs)
            return;
        const bool FIRST = lastOn < 0;
        lastOn           = NOWON;
        lastDirs         = NOWDIRS;
        if (FIRST)
            return;
        for (auto* A : std::vector<CMosaicAlgorithm*>{g_instances})
            A->canvasSettingsChanged();
    }

    // THE ZOOMED CANVAS OWNS THE POINTER (user, 2026-10-27: "when I hold super
    // it keeps selecting the browser even though my cursor is hovering over
    // another window"). The windows are only DRAWN small: at their real, full
    // sized places the pointer was over the browser, so Hyprland's own focus
    // follows mouse focused it on every motion, against the zoom's hover pick
    // (zoomedWindowAt), and the browser got the hover (its sidebar opened).
    // THE USER'S SCREENSHOT KEY, ZOOMED OUT (user, 2026-10-27: "I meant
    // whatever the user has for screenshots in Omarchy. For example I do 3
    // fingers press on track pad or press print screen"). Super is held for
    // the zoom, so Print arrives as Super+Print -- Omarchy's colour picker.
    // While zoomed out, a key whose Super chord is not a screenshot bind but
    // whose plain chord (the same mods without Super) is, runs that
    // screenshot. A screenshot bind is one whose description or command says
    // so; whatever keys the user gave it, they work.
    namespace {
        bool isScreenshotBind(const SP<SKeybind>& k) {
            std::string s = k->description + " " + k->arg;
            std::ranges::transform(s, s.begin(), [](unsigned char c) { return sc<char>(std::tolower(c)); });
            for (const char* W : {"screenshot", "hyprshot", "grimblast", "flameshot", "grim "})
                if (s.find(W) != std::string::npos)
                    return true;
            return false;
        }
        SP<SKeybind> bindFor(uint32_t mods, xkb_keysym_t sym) {
            if (!g_pKeybindManager)
                return nullptr;
            for (const auto& K : g_pKeybindManager->m_keybinds) {
                if (!K || !K->enabled || K->mouse || K->release || !K->submap.name.empty() || K->modmask != mods || K->key.empty())
                    continue;
                if (xkb_keysym_from_name(K->key.c_str(), XKB_KEYSYM_CASE_INSENSITIVE) == sym)
                    return K;
            }
            return nullptr;
        }
    }

    bool onZoomKey(uint32_t keycode, bool pressed, uint32_t timeMs) {
        if (!pressed) {
            if (g_zoomKeysSent.erase(keycode) > 0) {
                if (g_pSeatManager)
                    g_pSeatManager->sendKeyboardKey(timeMs, keycode, WL_KEYBOARD_KEY_STATE_RELEASED);
                return true;
            }
            return g_zoomKeysTaken.erase(keycode) > 0;
        }
        if (g_super != ESuper::ZOOMED || !liveZoomed())
            return false;
        const auto KB = g_pSeatManager ? g_pSeatManager->m_keyboard.lock() : nullptr;
        if (!KB || !KB->m_xkbState)
            return false;
        const xkb_keysym_t SYM  = xkb_state_key_get_one_sym(KB->m_xkbState, keycode + 8);
        const uint32_t     MODS = g_pInputManager->getModsFromAllKBs();
        if (SYM == XKB_KEY_NoSymbol || !(MODS & HL_MODIFIER_META))
            return false;
        // THE PICKER'S OWN KEYS (user, 2026-10-28: Escape out of the picker
        // opened the System menu): while it is up, Super is only held for the
        // zoom, so a key is what it would be without Super -- the picker's
        // binds (Omarchy's Return, Tab, arrows), or else the key itself, sent
        // to the picker (Escape cancels it), never a Super bind.
        if (pickerOpen()) {
            if (!bindFor(MODS, SYM))
                return false; // no Super bind to steal it: Hyprland delivers it
            if (const auto PLAIN = bindFor(MODS & ~HL_MODIFIER_META, SYM)) {
                const auto IT = g_pKeybindManager->m_dispatchers.find(PLAIN->handler);
                if (IT != g_pKeybindManager->m_dispatchers.end()) {
                    trace(std::format("zoomed picker key {} -> {} {}", PLAIN->key, PLAIN->handler, PLAIN->description));
                    IT->second(PLAIN->arg);
                    g_zoomKeysTaken.insert(keycode);
                    return true;
                }
            }
            trace(std::format("zoomed picker key {} -> the picker", keycode));
            g_pSeatManager->sendKeyboardKey(timeMs, keycode, WL_KEYBOARD_KEY_STATE_PRESSED);
            g_zoomKeysSent.insert(keycode);
            return true;
        }
        if (const auto OWN = bindFor(MODS, SYM); OWN && isScreenshotBind(OWN))
            return false; // the Super chord is a screenshot itself (Super+Shift+S): Hyprland runs it
        const auto PLAIN = bindFor(MODS & ~HL_MODIFIER_META, SYM);
        if (!PLAIN || !isScreenshotBind(PLAIN))
            return false;
        const auto IT = g_pKeybindManager->m_dispatchers.find(PLAIN->handler);
        if (IT == g_pKeybindManager->m_dispatchers.end())
            return false;
        trace(std::format("zoomed screenshot key {} -> {} {}", PLAIN->key, PLAIN->handler, PLAIN->description));
        IT->second(PLAIN->arg);
        g_zoomKeysTaken.insert(keycode);
        return true;
    }

    // THROUGH THE BAR (user, 2026-11-03: "hovering over this topbar area still
    // focuses the visible windows there (just hover) but clicks and hovers on
    // topbar are unaffected"). A canvas desktop's windows reach up under the
    // Omarchy bar and show through it, but the bar is one surface the width of
    // the screen, so the pointer on it never let focus follow onto them. With
    // the pointer on the bar, the window under it takes the KEYBOARD focus;
    // the bar keeps the pointer, so its own hovers and clicks are as before.
    // And the canvas brings it into view as any hover does (the pointer rides
    // the pan, so it stays on the window it hovered).
    void focusUnderBar() {
        static auto PFOLLOW = CConfigValue<::Config::INTEGER>("input:follow_mouse");
        if (*PFOLLOW != 1 || g_cursorRiding || zoomOwnsPointer())
            return;
        const auto PSURF = g_pSeatManager ? g_pSeatManager->m_state.pointerFocus.lock() : SP<CWLSurfaceResource>{};
        const auto HLS   = PSURF ? Desktop::View::CWLSurface::fromResource(PSURF) : SP<Desktop::View::CWLSurface>{};
        const auto VIEW  = HLS ? HLS->view() : SP<Desktop::View::IView>{};
        if (!VIEW || VIEW->type() != Desktop::View::VIEW_TYPE_LAYER_SURFACE)
            return;
        const auto LS = Desktop::View::CLayerSurface::fromView(VIEW);
        if (!LS || (LS->m_namespace != "omarchy-bar" && LS->m_namespace != "waybar"))
            return;
        const Vector2D P   = g_pInputManager->getMouseCoordsInternal();
        const auto     MON = LS->m_monitor.lock();
        if (!MON || !MON->m_activeWorkspace || MON->m_activeSpecialWorkspace)
            return;
        const auto W = Desktop::viewState()->hitTest().windowAt(P, Desktop::View::RESERVED_EXTENTS | Desktop::View::INPUT_EXTENTS | Desktop::View::ALLOW_FLOATING);
        if (!W || !W->m_isMapped || W->isHidden() || W->m_workspace != MON->m_activeWorkspace || W == Desktop::focusState()->window())
            return;
        Desktop::focusState()->fullWindowFocus(W, Desktop::FOCUS_REASON_FFM);
    }

    bool zoomOwnsPointer() {
        const auto* A = liveZoomed();
        return A && A->zoomed() && !pickerOpen();
    }

    namespace MosaicDetail {
    // See g_hoverStale. Focus follows the mouse (input:follow_mouse = 1) only.
    // No wait for the push's hold to run out (user, 2026-11-09: "if i exit
    // push fast enough it still doesn't refocus the previous app"): the
    // pointer off the screen's edge is the push over.
    bool pointerAtOuterEdge(const Vector2D& p) {
        const auto MON = State::monitorState()->query().vec(p).run();
        if (!MON)
            return false;
        const CBox B = MON->logicalBox();
        return p.x <= B.x + 1 || p.y <= B.y + 1 || p.x >= B.x + B.w - 2 || p.y >= B.y + B.h - 2;
    }
    bool hoverRefresh(bool blocked) {
        static auto PFOLLOW = CConfigValue<::Config::INTEGER>("input:follow_mouse");
        if (!g_hoverStale || *PFOLLOW != 1 || blocked || g_cursorRiding || zoomOwnsPointer())
            return false;
        if (g_layoutManager && g_layoutManager->dragController()->target())
            return false;
        // A BUTTON HELD (2026-11-09): Hyprland keeps the focus where a press
        // landed until every button is up -- a text selection dragged into
        // the window beside it stays in its own. This runs from the motion
        // listener, ahead of that rule, and moved the keyboard focus mid-drag.
        if (g_pInputManager->hasHeldButtons())
            return false;
        const Vector2D P = g_pInputManager->getMouseCoordsInternal();
        if (pointerOnChrome(P) || pointerInDockBand(P) || pointerAtOuterEdge(P))
            return false;
        const auto W = Desktop::viewState()->hitTest().windowAt(P, Desktop::View::RESERVED_EXTENTS | Desktop::View::INPUT_EXTENTS | Desktop::View::ALLOW_FLOATING);
        if (!W || !W->m_isMapped || W->isHidden() || W == Desktop::focusState()->window())
            return false;
        if (vetoHoverFocus(W))
            return false;
        g_hoverStale = false;
        trace(std::format("hover refresh: {} under the pointer takes the focus", W->m_class));
        Desktop::focusState()->fullWindowFocus(W, Desktop::FOCUS_REASON_FFM);
        return true;
    }
    }

    void onMove(bool blocked) {
        watchCanvasKeys();
        hoverRefresh(blocked);
        superMove(blocked);
        updateEdgePush();
        if (std::ranges::any_of(g_instances, [](const CMosaicAlgorithm* a) { return a->resizing(); }))
            checkResizeEnded();
        // A drag hold whose window was let go elsewhere, or carried to another
        // desktop: close up now.
        for (auto* A : g_instances)
            if (A->holdingFor() && !A->dragHoldLive())
                A->releaseDragHold();
        const bool DRAGGING = g_layoutManager && g_layoutManager->dragController()->target();
        Layout::eRectCorner corner = Layout::CORNER_NONE;
        if (const auto* Z = liveZoomed(); Z && Z->zoomed()) {
            // Zoomed out: the corners are the drawn ones (and a resize under
            // way keeps its cursor).
            corner = Z->zoomResizing() ? Z->zoomCorner() : Z->zoomCornerAt(g_pInputManager->getMouseCoordsInternal());
        } else if (!blocked && !DRAGGING) {
            const int MODS = modsNow();
            if (MODS == 0 || MODS == HL_MODIFIER_META)
                corner = cornerTarget(g_pInputManager->getMouseCoordsInternal(), MODS == HL_MODIFIER_META).second;
        }
        if (corner != Layout::CORNER_NONE) {
            Pointer::Cursor::overrideController->setOverride(cursorFor(corner), Pointer::Cursor::CURSOR_OVERRIDE_WINDOW_EDGE);
            g_hoverSet = true;
        } else if (g_hoverSet && !DRAGGING) {
            Pointer::Cursor::overrideController->unsetOverride(Pointer::Cursor::CURSOR_OVERRIDE_WINDOW_EDGE);
            g_hoverSet = false;
        }
    }

}
