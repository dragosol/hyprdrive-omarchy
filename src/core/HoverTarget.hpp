#pragma once

#include "globals.hpp"
#include "HoverTargetRule.hpp"

// THE WINDOW THE USER'S KEYBINDS MEAN RIGHT NOW (user, 2026-11-11: "super+f
// should act on the hovered tile in overview"; "if I mouseover a window preview
// in the dock I should have access to all the same super+keybind functionality
// as I do in overview when I'm mousedover that window").
//
// One provider for every Super+key action that acts on "the hovered window,
// else the focused one", in this order (HoverTargetRule.hpp):
//   1. the overview's selected tile (the one under the pointer, or the
//      keyboard's after the arrows; IOverview::actionTile);
//   2. the dock hover pane's preview cell under the pointer
//      (Dock::previewUnderPointer) -- outside the overview too;
//   3. nothing: the caller falls back to the focused window.
//
// Its users: Hyprland's fullscreen and close actions when they name no window
// (HoverActions.hpp: Super+F, Super+W from the user's own binds), the
// overview's own close chord (OverviewInput.cpp), and the `window
// select|close` dispatcher.
namespace Hyprdrive::HoverTarget {
    PHLWINDOW window();
}
