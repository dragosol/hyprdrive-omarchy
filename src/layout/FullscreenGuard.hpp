#pragma once

#include "globals.hpp"

#include <hyprland/src/desktop/state/FocusState.hpp>

// FULLSCREEN AND EVERYTHING ELSE ON ITS DESKTOP (user, 2026-10-29: "if I'm on
// youtube and i have a video in fullscreen on a desktop with other apps, and i
// press super and click on another app on that same desktop, browser minimizes
// itself instead of the video exiting fullscreen ... when windows or apps are
// in fullscreen (with the exception of games) should take the app out of
// fullscreen when i switch to another one").
//
// Hyprland takes a fullscreen window out of fullscreen when another window on
// its desktop is focused (misc:on_focus_under_fullscreen 2, the default). That
// is the answer for a video. It is the wrong one for a game: a Wine game puts
// itself straight back, takes the focus with it, and keeps the pointer clipped
// to the box it had for that moment (XGeometry.hpp) -- the mouse then acts
// somewhere else than the cursor is. Measured on fedora with Overwatch: a dock
// click on Discord under the game left Wine's clip at the game's tile box.
//
// So a fullscreen GAME goes to the dock instead, still fullscreen, before the
// other window takes the focus. Every route -- the overview's pick, the dock, a
// keybind, an activation request, a notification -- comes through
// CFocusState::fullWindowFocus, which installFocusHook() hooks.
namespace Hyprdrive::FullscreenGuard {
    // A game, as far as a window tells: the content type it set, a Steam game,
    // gamescope, or a Windows program under XWayland.
    bool      isGame(const PHLWINDOW& w);
    // The fullscreen game a focus on `target` would take out of fullscreen:
    // `target` tiled, on the desktop of a fullscreen (not maximized) game.
    PHLWINDOW gameCover(const PHLWINDOW& target);
    // The fullWindowFocus hook, before Hyprland acts on it.
    void      beforeFocus(const PHLWINDOW& target, Desktop::eFocusReason reason);
    // PLUGIN_INIT: hook CFocusState::fullWindowFocus (Hyprland unhooks it at unload).
    void      installFocusHook();

    // A POINTER LOCK LEFT OFF (2026-10-29, fedora, Overwatch): in mouse-look a
    // Wine game hides the cursor and warps the pointer, and XWayland emulates
    // that with a pointer lock, following only relative motion from then on.
    // The overview switches every lock off while it is open
    // (OvInternal::releasePointerLocks), and Hyprland switches one back on only
    // when the keyboard focus moves onto its surface -- which a way back that
    // never moved the focus does not do. The lock stayed off while XWayland
    // still followed only relative motion: X's pointer drifted from Hyprland's
    // cursor (measured 400,0 against 2420,272) and the game's menus answered
    // somewhere else than the cursor was. Hyprland's own rule is that a lock is
    // on while its surface has the keyboard focus; on pointer motion (main.cpp)
    // with no overview up, the one under the pointer goes back on.
    void      resumePointerLock();
}
