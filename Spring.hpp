#pragma once

// SPRING LOADING (user, 2026-10-07). While something is being dragged on the
// plain desktop -- a client's file/folder drag, or a window under Hyprland's
// interactive move (Super+drag, a dock tear-out):
//   * held at the left/right screen edge for drag:spring_delay_ms, the monitor
//     switches to the desktop that way (past the last one: a new empty one),
//     and holding on keeps going, one desktop per delay;
//   * a dragged WINDOW comes along to every desktop it is switched to (also by a
//     swipe or a keybind mid-drag);
//   * a file drag held over a running app in the dock springs it open
//     (Dock::springUpdate).
// A pointer resting at an edge sends no motion, so a timer keeps asking.
namespace ScrollOverview::Spring {
    void init();
    void shutdown();
}
