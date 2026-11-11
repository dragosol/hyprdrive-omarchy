#pragma once

// THE DESKTOP STRIP (user, 2026-11-02: "put on a senior ux/product designer hat
// and refine the mechanics significantly. think about it from all angles";
// always reversible and interruptible, motion blur everywhere, scroll ticks
// accumulate, gestures smooth and 1:1). Shared verbatim by both trees.
//
// Hyprland slides ONE pair of desktops at a time, each switch restarting its
// curve from a standstill: two quick notches stalled a frame and then leapt a
// thousand pixels, and a notch back mid-way slid from wherever the first slide
// had got to. Here the desktops of a monitor are cards in one row
// (DesktopRowModel.hpp) and the WHEEL moves the row: one critically damped
// spring (DesktopStripModel.hpp) carries it, every desktop drawn at its place
// in the row -- several at once on a fast spin -- so notches add up without a
// stop, a reverse turns it round where it is, and it lands on one desktop.
//
// The desktop the row is heading for is the one SHOWN: Hyprland's own
// `workspace N` runs the moment it changes (focus and everything else as
// always), its slide suppressed while switching() -- the strip draws it. Keys,
// the dock and focus keep Hyprland's slide, unless the strip is already moving
// (it carries on toward their desktop), or animation:desktop_jump_through asks
// a jump of several desktops to pass through the ones between.
//
// Each desktop's place in the row leaves room for the windows a canvas desktop
// has hanging past the screen's edges (Mosaic::slideOverhang), so they slide in
// and out with their desktop instead of popping.
//
// And whenever a monitor shows more than one desktop -- the strip, Hyprland's
// slide, a swipe, Mosaic's slide tail -- the desktop being switched to is drawn
// over the others (DesktopStrip.cpp, STACKING).

#include "globals.hpp"

#include "DesktopStripModel.hpp"

#include <functional>
#include <string>
#include <vector>

namespace Hyprdrive::DesktopStrip {
    // The row of desktop ids on `mon`, `current` among them
    // (DesktopRowModel.hpp over the live desktops).
    using FRow = std::function<std::vector<int>(const PHLMONITOR& mon, int current)>;
    // Show desktop `id` on `mon` -- Hyprland's `workspace N`. switching() is
    // true for its duration: its slide is the strip's.
    using FGo = std::function<void(const PHLMONITOR& mon, int id)>;

    // PLUGIN_INIT / PLUGIN_EXIT. shutdown() lands the row and drops the
    // timer and the frame pump while this code is mapped.
    void init(FRow row, FGo go);
    void shutdown();

    // Hyprland would slide desktops on `mon` at all: animations on and the
    // workspaces animation a slide (not a fade).
    bool available(const PHLMONITOR& mon);

    // A wheel event on the desktop chord, `v120` signed (+ = next). false: the
    // strip cannot run here (available(), or a named desktop shown) -- the
    // caller steps the old way.
    bool onWheel(const PHLMONITOR& mon, int v120, const StripModel::SSettings& settings);

    // A desktop change someone else made on `mon` (a key, the dock, focus):
    // `from` -> `to`. claims() answers whether the strip draws it -- it is
    // moving there already, or `jumpThrough` and the jump is more than one
    // desktop along the row -- so the caller suppresses Hyprland's slide.
    // take() then starts it, `fromOffset` being where `from` was on screen
    // before Hyprland touched it. Both are no-ops inside switching().
    bool claims(const PHLMONITOR& mon, int from, int to, bool jumpThrough);
    void take(const PHLMONITOR& mon, int from, int to, const Vector2D& fromOffset, bool jumpThrough);
    // Slide the row to desktop `to` on `mon` and show it (a fullscreen app's
    // own desktop, FullscreenDesktop.hpp). Moving already: on toward it from
    // where the row is -- a change of mind turns the slide round in place.
    // false: the strip cannot run here; the caller switches the plain way.
    bool glideTo(const PHLMONITOR& mon, int to);

    // The strip is moving the desktops of `mon` / of `ws`'s monitor.
    bool driving(const PHLMONITOR& mon);
    bool drives(const PHLWORKSPACE& ws);
    // Inside the strip's own `workspace N`.
    bool switching();

    // Land where it is going, now (the overview opening).
    void finishNow();
    // A trackpad swipe starting on `mon` while the row moves: the desktop most
    // on screen becomes the shown one, left exactly where it is. true: it was
    // moving. Once the swipe has really begun, letGo(): the strip stops there
    // and the swipe catches the desktop from its place; a swipe that did not
    // begin leaves the row to land as before.
    bool catchForSwipe(const PHLMONITOR& mon);
    void letGo(const PHLMONITOR& mon);

    // A trackpad swipe on `mon` was just let go, after Hyprland's own end: its
    // release slides take the time their distance and the fingers' speed call
    // for, not the animation's fixed one (DesktopStrip.cpp, A QUICK WAY BACK).
    void quickenSwipeRelease(const PHLMONITOR& mon);

    // render.pre: the row's place for this frame.
    void preRender(const PHLMONITOR& mon);
    // render.pre, AFTER Mosaic::preRender: where a live swipe has the desktop,
    // for the fingers' speed at its release.
    void sampleSwipe(const PHLMONITOR& mon);

    std::string debugReport();
}
