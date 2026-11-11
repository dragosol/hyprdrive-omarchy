#pragma once

#include "HoverTargetRule.hpp"
#include "globals.hpp"

#include <optional>

// SUPER+KEY ACTS ON THE HOVERED WINDOW (user, 2026-11-11: "super+f should act
// on the hovered tile in overview"; "if I mouseover a window preview in the
// dock I should have access to all the same super+keybind functionality as I
// do in overview when I'm mousedover that window").
//
// The user's binds name no window: a Lua hl.dsp.window.fullscreen / .close /
// .float / .pin / .move, the legacy `fullscreen` / `fullscreenstate` /
// `killactive` / `togglefloating` / `pin` dispatchers -- or name the focused
// one, which from a bind means the same: Omarchy's o.workspaces.move_window
// (Super+Shift+N) and the legacy `movetoworkspace` pass the active window's
// address. ANY call that names no window counts, not only a key's (owner:
// "keep"); the plugin's own calls name theirs and are never swapped (SOwnCall).
// Hyprland's action then takes the FOCUSED one. With the pointer on an
// overview tile or on a preview cell of the dock's hover pane, the window the
// user means is that one (HoverTarget.hpp). So an action that names no window,
// taken while there is a hover target that is not the focused window, is
// given the hover target -- one argument changed, the rest of the path
// (Hyprland's action, the fullscreen controller, the layout, the fullscreen
// desktop, the overview's and the dock's own handling of a window that leaves
// or closes) the same as for the focused window. No hover target (the pointer
// over the strip, a gap, a window, the dock's icons): nothing changes.
//
// Hooked, one generic swap each (one rule: target() below, HoverTargetRule's
// retarget):
//   fullscreenWindow, both overloads  Super+F; maximize is the same action
//                                     with FSMODE_MAXIMIZED (Omarchy's Super+Alt+F)
//   closeWindow                       Super+W (inside the overview its own
//                                     close chord takes the key, OverviewInput.cpp,
//                                     and asks HoverTarget too)
//   floatWindow                       Super+T -- hooked by OwnSizeTile.cpp (one
//                                     hook per address), which asks target() first
//   pinWindow                         pin
//   moveToWorkspace                   Super+Shift+N, and the move to a monitor
//                                     (Lua's window.move{monitor=}); a hovered
//                                     window is moved silently (moveSilently)
// and the plugin's own `hyprdrive:minimize` (Super+H, Dispatchers.cpp) asks
// target() for the window to minimize.
//
// One limit: a bind's "set"/"unset" (not "toggle") is decided by Hyprland
// against the FOCUSED window before the action runs; the action then toggles
// the target. With the two in different states that is the opposite of "set";
// Super+F is a toggle.
namespace Hyprdrive::HoverActions {
    // PLUGIN_INIT: hook Hyprland's actions. Missing: Hyprland's own stands (the
    // focused window), and the log says so.
    void init();

    // The window an action called with `w` acts on: `w`, unless the rule
    // (HoverTargetRule.hpp retarget) gives it the hover target. `swapped`
    // says which. Every hooked action asks this, OwnSizeTile's float hook too.
    std::optional<PHLWINDOW> target(const std::optional<PHLWINDOW>& w, bool* swapped = nullptr);

    // THE PLUGIN'S OWN CODE IS RUNNING: no action it calls is retargeted
    // (plugin code names the window it means). Held around every hyprdrive
    // dispatcher (Config::registerDispatcher) and around each hooked action's
    // original, so a move or a fullscreen that action makes in turn is not
    // swapped a second time.
    struct SOwnCall {
        SOwnCall();
        ~SOwnCall();
        SOwnCall(const SOwnCall&)            = delete;
        SOwnCall& operator=(const SOwnCall&) = delete;
    };
}
