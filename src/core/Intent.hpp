#pragma once

#include "globals.hpp"

// WINDOWS YOU DID NOT ASK FOR (user, 2026-11-04: "when a window opens
// unprompted (e.g. loading screen or some other windows that I didn't
// explicitly launch via keyboard or via hyprdrive), mosaic should keep me
// focused on the window I was last focused on and not try to take my attention
// away, including keeping my cursor on the last focused window if I wasn't
// moving it").
//
// Hyprland focuses every window that maps. On a Mosaic desktop this decides,
// as the window maps (window.open_early, before the layout places it and
// before Hyprland's focus), whether YOU asked for it. You did when:
//   * something was launched for you lately -- any exec Hyprland ran (a
//     keybind, the Lua config, hyprctl dispatch: CExecutor::spawnRawProc),
//     a launch of Hyprdrive's own (the dock, the overview's search: Launch::wrap)
//     or Enter / a click in a launcher or the bar (a layer surface). Such a
//     grant lasts GRANT_S; the first window that maps takes it, and that app's
//     further windows ride on it (a splash, then its main window);
//   * it is a dialog of the window you are in (its parent), or a new window of
//     the app you are using right after you clicked or pressed Enter in it;
//   * you clicked or pressed Enter anywhere just before it mapped (a link in
//     a terminal opening a browser window).
// Anything else -- a game's loading screen, an app restoring itself, a window
// a background process opens -- maps WITHOUT focus (m_noInitialFocus): the
// window you were in keeps it, Mosaic does not place the newcomer at the
// pointer or pan to it, and the pointer, if you were not moving it, rides the
// window it was on while the mosaic makes room. mosaic:quiet_new_windows.
namespace Hyprdrive::Intent {
    // PLUGIN_INIT (after Mosaic::registerLayout) / PLUGIN_EXIT.
    void init();
    void shutdown();
    // The user launched something (also reached through Launch::onLaunch).
    void granted(const char* why);
    // `w` mapped unasked on a Mosaic desktop (and is still mapping).
    bool quiet(const PHLWINDOW& w);
    // Every frame of `monitor` (render.pre): the pointer rides its window.
    void preRender(PHLMONITOR monitor);
}
