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

#include <cstdint>
#include <functional>
#include <vector>

namespace Render {
    class IWindowTransformer;
}

namespace ScrollOverview::FlightBlur {

    // THE BLUR SITES (user, 2026-09-30: "an entire motion blur section with
    // per-animation curve and strengths with our defaults configured"). Every
    // place hyprdrive smears motion, with the strength and fade it shipped
    // with. Config.cpp registers motion_blur:<name>:strength / :fade from this
    // table; shape() reads them.
    enum class ESite : uint8_t {
        MINIMIZE,       // the minimize / restore flight and the labels riding it
        LAYOUT,         // a retile, a window opening (dwindle, scrolling)
        FLICK,          // the scrolling tape, above the flick speed
        SWIPE,          // a trackpad desktop swipe
        SCRATCHPAD,     // the scratchpad sliding in and out
        OVERVIEW,       // the overview opening / closing from a keypress
        DESKTOP_SWITCH, // switching desktops in the overview by keyboard
        SEARCH_TILES,   // tiles moving as a search filters them
        SEARCH_PANE,    // the search results pane growing / shrinking
        DRAG,           // the overview's drag proxy resizing
        STACK_PANE,     // a dock stack's grid pane opening / closing
    };
    struct SSite {
        const char* name;
        const char* label;
        float       strength;
        const char* fade;
    };
    inline constexpr SSite SITES[] = {
        {"minimize", "minimize / restore flight", 1.F, "early"},
        {"layout", "window retile and open", 1.F, "early"},
        {"flick", "scrolling layout flick", 0.5F, "early"},
        {"swipe", "trackpad desktop swipe", 0.5F, "constant"},
        {"scratchpad", "scratchpad slide", 0.5F, "early"},
        {"overview", "overview open / close", 0.25F, "early"},
        {"desktop_switch", "overview keyboard desktop switch", 0.25F, "constant"},
        {"search_tiles", "search tile motion", 0.25F, "constant"},
        {"search_pane", "search results pane", 0.5F, "early"},
        {"drag", "overview drag resize", 1.F, "constant"},
        {"stack_pane", "dock stack grid pane", 1.F, "early"},
    };

    // `site`'s share of the motion prev -> cur at `progress` (0..1 through the
    // animation; pass 0 where there is none): strength x fade(progress), 0
    // when motion_blur:enabled is off.
    float amount(ESite site, float progress);

    // prev pulled toward cur so the blur covers amount(site, progress) of the
    // motion; returns cur (no smear) when that is ~0.
    CBox shape(ESite site, const CBox& prev, const CBox& cur, float progress);

    // dock:minimize_motion_blur, clamped; below 2 means off.
    int samples();

    // Draws `draw` (which queues the flight's shadow, window and border) as one
    // motion-blurred element moving prevBox -> box, both monitor-local RENDER
    // px. Returns false, having queued nothing, when blur is off or there is no
    // motion -- the caller then draws the ordinary way.
    // `window` may be null (the dock stack's pane is not a window).
    // `samplesOverride` >= 0 replaces dock:minimize_motion_blur (the overview's
    // drag proxy uses the layout blur's samples).
    bool drawBlurred(PHLMONITOR monitor, const PHLWINDOW& window, const CBox& prevBox, const CBox& box, const std::function<void()>& draw, int samplesOverride = -1);

    // plugin:hyprdrive:layout_motion_blur, clamped.
    int layoutSamples();

    // HALF AS LONG (user, 2026-09-30): the smear is full at the start of an
    // animation and eased to nothing by its halfway point. `progress` is how far
    // the animation is, 0..1; the returned box is `prev` pulled toward `cur` so
    // that motion blur covers only what is left of the smear.
    CBox fadeMotion(const CBox& prev, const CBox& cur, float progress);

    // A driven flight moved the real window prevBox -> box (monitor-local
    // RENDER px) this frame; the NEXT window pass draws it there, blurred.
    void recordDriven(PHLMONITOR monitor, const PHLWINDOW& window, const CBox& prevBox, const CBox& box);

    // Drop the transformer from every window `stillFlying` says no to. Called
    // after each flight frame, so a landed or aborted flight never leaves a
    // window rendering through an offscreen buffer.
    void sweep(const std::function<bool(const PHLWINDOW&)>& stillFlying);

    // PLUGIN_EXIT: no window may keep an object whose code is being unloaded.
    void detachAll();
    // The frost behind a window our transformer is on (FlightBlur.cpp,
    // onPreWindow): init after registerConfig(), shutdown in PLUGIN_EXIT.
    void init();
    void shutdown();

    // Take `window`'s smear off NOW: a flight reversed or restarted. The last
    // flight's boxes must never be applied to the next one (user, 2026-09-30:
    // spam-minimizing, every other flight smeared off toward the top left).
    void reset(const PHLWINDOW& window);

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
