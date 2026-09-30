#pragma once

// MOTION BLUR FOR THE MINIMIZE / RESTORE FLIGHT (user, 2026-09-29: "add motion
// blur to the maximize and minimize animations (both in and outside of
// overview) - same motion blur as my dock fan stacks").
//
// The same blur the fan wears -- the mean of N instants across the time one
// frame covers -- but not the same machinery, because a flight is a WINDOW:
// shadow, rounded corners, border, subsurfaces, frost. Hyprland 0.56 already
// knows how to motion-blur a window (decoration:motion_blur): it renders the
// window into a work buffer and composites that buffer with a shader that
// averages N samples along the previous->current box. That setting is global
// and off, and turning it on would blur every window the user drags. So the
// plugin drives the same renderer path itself, for the flying window only:
//
//   * PROXY flights (every minimize; a restore with the overview open) are
//     drawn by the dock. Their whole draw -- shadow, window, border -- is
//     redirected into a nested pass and queued as ONE transformed-window
//     element carrying the motion. Everything smears together.
//   * DRIVEN flights (a restore on the desktop) are the real window, drawn by
//     Hyprland's own window pass. It gets a window transformer the plugin owns
//     for as long as it flies; the transformer hands the renderer the motion.
//
// Nothing here runs when dock:minimize_motion_blur is 0 or the window is not
// moving.

#include "globals.hpp"

#include <functional>
#include <vector>

namespace Render {
    class IWindowTransformer;
}

namespace ScrollOverview::FlightBlur {

    // dock:minimize_motion_blur, clamped; below 2 means off.
    int samples();

    // Draws `draw` (which queues the flight's shadow, window and border) as one
    // motion-blurred element moving prevBox -> box, both monitor-local RENDER
    // px. Returns false, having queued nothing, when blur is off or there is no
    // motion -- the caller then draws the ordinary way.
    bool drawBlurred(PHLMONITOR monitor, const PHLWINDOW& window, const CBox& prevBox, const CBox& box, const std::function<void()>& draw);

    // A driven flight moved the real window prevBox -> box (monitor-local
    // RENDER px) this frame; the NEXT window pass draws it there, blurred.
    void recordDriven(PHLMONITOR monitor, const PHLWINDOW& window, const CBox& prevBox, const CBox& box);

    // Drop the transformer from every window `stillFlying` says no to. Called
    // after each flight frame, so a landed or aborted flight never leaves a
    // window rendering through an offscreen buffer.
    void sweep(const std::function<bool(const PHLWINDOW&)>& stillFlying);

    // PLUGIN_EXIT: no window may keep an object whose code is being unloaded.
    void detachAll();

    // LAYOUT MOTION (user, 2026-09-30: "add motion blur to windows resizing in
    // dwindle/scrolling workspace mode"). Each frame of `monitor`, before its
    // windows are drawn: every window whose position or size is ANIMATING (a
    // retile, a scrolling tape move, an open) gets the same transformer, fed
    // where it was drawn last frame and where it is now. Not an interactive
    // move/resize, and not whatever `skip` says (a flying window feeds itself).
    // plugin:hyprdrive:layout_motion_blur samples, 0 = off. Also sweeps.
    void trackLayoutMotion(PHLMONITOR monitor, const std::function<bool(const PHLWINDOW&)>& skip);

    // The transformer is for Hyprland's OWN window pass only. hyprdrive's own
    // draws of the same window (a dock hover preview, an overview tile) hold one
    // of these around renderWindow: with the transformer on, renderWindow routes
    // the window through an offscreen buffer that ignores the preview's scale
    // and clip, and the preview went dark for the whole restore (user,
    // 2026-09-30).
    class CSuspend {
      public:
        explicit CSuspend(const PHLWINDOW& window);
        ~CSuspend();
        CSuspend(const CSuspend&)            = delete;
        CSuspend& operator=(const CSuspend&) = delete;

      private:
        PHLWINDOWREF                                m_window;
        std::vector<UP<Render::IWindowTransformer>> m_held;
    };
}
