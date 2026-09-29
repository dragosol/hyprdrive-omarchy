#pragma once

// SMOOTH FRAMES (user, 2026-09-29).
//
// On a GPU that sometimes cannot finish a frame inside one refresh (measured on
// the XPS: 2880x1800, Xe), Hyprland's classic scheduler turns every late frame
// into a whole missed refresh, and the overview's open/close animation drops to
// 30 fps. render:new_render_scheduling ("should improve FPS on underpowered
// devices") renders ahead instead: the A/B on the XPS drew 60-71 frames per
// 1.5 s open against 50-51, with 23% late against 55%.
//
// Two halves:
//   * plugin:hyprdrive:smooth_frames -- when on, the new scheduler is enabled
//     for the whole session (once, at load or on the toggle, never per open)
//     and the user's own value is put back when it is turned off or on unload.
//   * a one-time hint: if several overview animations in a row drop frames on a
//     machine still on the classic scheduler, a notification points at the
//     Hyprdrive panel's "Smooth frames" toggle.
//
// Why not toggle per open: the scheduler keeps two flags (m_renderAtFrame,
// m_pendingThird) that the option switching never resets. Off while a late
// frame is in flight and back on later leaves m_renderAtFrame false -- no frame
// is rendered, so no fence ever resets it, and the monitor stops updating
// (MonitorFrameScheduler.cpp). Switching ON here always clears both first.

#include <hyprland/src/helpers/memory/Memory.hpp>

namespace Monitor {
    class CMonitor;
}

namespace ScrollOverview::FramePacing {
    // Apply plugin:hyprdrive:smooth_frames (load, config reload, panel toggle).
    void apply();
    // Unload: the user's own scheduler value goes back.
    void shutdown();

    // A frame the overview drew while its open/close animation ran.
    void noteAnimatedFrame(const SP<Monitor::CMonitor>& monitor);
    // A frame outside the animation: the next animated frame starts a new run.
    void noteIdleFrame(const SP<Monitor::CMonitor>& monitor);
    // The last overview went away: judge the session just ended.
    void endSession();
}
