#pragma once

#include "HoverTargetRule.hpp"

// SUPER+KEY ACTS ON THE HOVERED WINDOW (user, 2026-11-11: "super+f should act
// on the hovered tile in overview"; "if I mouseover a window preview in the
// dock I should have access to all the same super+keybind functionality as I
// do in overview when I'm mousedover that window").
//
// The user's binds name no window: a Lua hl.dsp.window.fullscreen / .close,
// the legacy `fullscreen` / `fullscreenstate` / `killactive` dispatchers.
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
// Hooked, one generic swap each: both fullscreenWindow overloads (Super+F) and
// closeWindow (Super+W). Inside the overview its own close chord takes Super+W
// at the key (OverviewInput.cpp) and asks HoverTarget too; outside it, the
// user's close bind reaches closeWindow and this hook.
//
// One limit: a bind's "set"/"unset" (not "toggle") is decided by Hyprland
// against the FOCUSED window before the action runs; the action then toggles
// the target. With the two in different states that is the opposite of "set";
// Super+F is a toggle.
namespace Hyprdrive::HoverActions {
    // PLUGIN_INIT: hook Hyprland's actions. Missing: Hyprland's own stands (the
    // focused window), and the log says so.
    void init();
}
