#pragma once

#include "globals.hpp"

#include <hyprland/src/devices/IPointer.hpp>

// MOSAIC (user, 2026-10-11): a Hyprland tiled layout named "mosaic", after
// GNOME's planned Mosaic. Windows keep their natural size and gather into one
// cluster centred on the desktop; when they no longer fit -- too many, or one
// too big -- the same layout tiles them, and it returns to the mosaic by itself
// once they fit again. The desktop's layout stays "mosaic" throughout: the tile
// arrangement is its second half, not a switch to another layout.
//
// The geometry is MosaicPack.hpp (pure, host-tested). Super+L's three-way
// cycle, the settings section and "Mosaic as the default" live in the panel and
// its fragments; this side only provides the layout.
//
// Symcheck rule: the algorithm class is defined OUTSIDE any anonymous namespace
// in Mosaic.cpp -- Hyprland holds it by its vtable and its typeid.
namespace Hyprdrive::Mosaic {
    // PLUGIN_INIT: register "mosaic" with Hyprland's layout matcher.
    void registerLayout(HANDLE handle);
    // PLUGIN_EXIT, FIRST: every desktop on mosaic is handed back to the default
    // layout -- its algorithm objects destroyed -- while this code is mapped.
    void unregisterLayout(HANDLE handle);

    // CORNER RESIZE (Mosaic.cpp). A mouse button event, before Hyprland sees
    // it: true = claimed (a corner resize started, or ours ended). `blocked`:
    // the overview, the dock or a lock screen owns the pointer.
    bool onButton(const IPointer::SButtonEvent& e, bool blocked);
    // A key while the canvas is zoomed out (Super held): true = taken.
    bool onZoomKey(uint32_t keycode, bool pressed, uint32_t timeMs);
    // Pointer motion: the resize cursor while over a grabbable corner.
    void onMove(bool blocked);
    // Super zoomed the canvas out: pointer motion is the zoom's alone, never
    // Hyprland's focus follows mouse over the windows' real (full size) places.
    bool zoomOwnsPointer();
    // The pointer on the Omarchy bar: the window showing through it under the
    // pointer takes the keyboard focus (the bar keeps the pointer), and is
    // brought into view as any hovered canvas window is.
    void focusUnderBar();
    // Is `ws` laid out by Mosaic (Intent.hpp asks as a window maps)?
    bool isMosaicWorkspace(const PHLWORKSPACE& ws);
    // The three-finger zoom-out (CCanvasZoomGesture): begin answers 1 (zooming
    // out), -1 (zooming back in) or 0 (no canvas to zoom here); follow takes
    // the zoom level 0..1; end settles out (it stays until a click) or in.
    int    fingerZoomBegin();
    void   fingerZoomFollow(double z);
    void   fingerZoomEnd(bool out);
    double fingerZoomLevel();
    // Focus following the mouse onto a window that only peeks in at the
    // screen's edge, or through the dock: refused (DesktopSwitch's focus hook).
    bool vetoHoverFocus(const PHLWINDOW& w);
    // Raw pointer motion: `delta` asked, `moved` what the pointer really moved
    // (less at a screen's edge). A resize reaches past the edge by the rest.
    void onPointerDelta(const Vector2D& delta, const Vector2D& moved);
    // A window is closing (Hyprland's window.close, before the layout drops
    // it): on a desktop in the mosaic arrangement, its size becomes its app's
    // size in mosaic from now on.
    void onWindowClose(const PHLWINDOW& w);
    // PLUGIN_EXIT: our drag ended, our cursor override dropped.
    void shutdown();

    // THE INFINITE CANVAS (MosaicCanvas.hpp, mosaic:infinite_canvas). A mosaic
    // desktop on its canvas: the view follows the focus and the Super zoom.
    // Neither the desktop swipe (user, 2026-10-29) nor Super+scroll (user,
    // 2026-10-30) pans it -- both switch desktops.
    // Super went down / came up (main.cpp's key hook): the canvas's Super
    // gesture -- hold and move to zoom out,
    // never while a Super+click or Super+drag has used the press.
    void onSuperKey(bool pressed);
    // An overview is opening (COverview's constructor and reopen(), the
    // two doors every overview comes through): a zoomed-out canvas ends, as
    // when Super is let go.
    void onOverviewOpen();
    // A zoomed-out canvas drag (Spring): the window it carries, and handing it
    // to Hyprland as an ordinary Super+drag so a desktop switch takes it along.
    PHLWINDOW zoomCarried();
    bool      handZoomDragOff();
    // Is `w` on a canvas whose view is moving (a pan, the zoom, a swipe, a
    // drag pushing at the edge)? The layout motion blur leaves it alone.
    bool canvasMotion(const PHLWINDOW& w);
    // A desktop slide (a swipe, a switch) is carrying this window's canvas
    // desktop and the window is off its view: not drawn (main.cpp's
    // shouldRenderWindow hook), or the slide brings it onto the desktop
    // next door. Also where a slide is noticed (its tail, the zoom left
    // behind): Mosaic.cpp, DESKTOP SLIDES.
    bool hideDuringSlide(const PHLWINDOW& w, const PHLMONITOR& monitor);
    // render.pre, before every frame of a monitor: a canvas desktop sliding in
    // starts past its overhang, so its windows slide in (Mosaic.cpp, THEY SLIDE IN).
    void preRender(PHLMONITOR monitor);
    // How far the windows of a canvas desktop that are in view at home hang
    // past its `side` edge (-1 left/top, +1 right/bottom) along the slide's
    // axis: the desktop strip leaves that much room beside it. 0 off a canvas.
    double slideOverhang(const PHLWORKSPACE& ws, const PHLMONITOR& monitor, int side, bool xax);
    // A swipe let go on `monitor` (after Hyprland's end): each desktop the swipe
    // stretched starts its release from its stretched place, not the raw one
    // the last finger event left (Mosaic.cpp, THE RELEASE STARTS WHERE IT IS DRAWN).
    void swipeReleased(const PHLMONITOR& monitor);
    // How far the swipe stretches `ws` at `offset` (1 = not at all): a drawn
    // offset divided by it is the swipe's own.
    double swipeStretch(const PHLWORKSPACE& ws, const PHLMONITOR& monitor, const Vector2D& offset);
    // The whole canvas in global layout coordinates at the current view, for
    // the overview's strip thumbnail (the whole canvas, zoomed out). Nothing
    // when the desktop is not on its canvas.
    std::optional<CBox> canvasBox(const PHLWORKSPACE& ws);
    // main.cpp's RENDER_POST_WINDOWS (before the dock): window titles while
    // Super is held and the pointer moves -- any desktop, the zoomed-out
    // canvas included -- as mosaic:zoom_labels asks.
    void renderZoomLabels(PHLMONITOR monitor);
    // A title mid-fade: Dock::needsFramesReason asks for its frames.
    bool labelsAnimating();
}
