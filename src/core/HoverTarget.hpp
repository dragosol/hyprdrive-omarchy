#pragma once

#include "globals.hpp"

// THE WINDOW THE USER'S KEYBINDS MEAN RIGHT NOW (user, 2026-11-11: "super+f
// should act on the hovered tile in overview"; "if I mouseover a window preview
// in the dock I should have access to all the same super+keybind functionality
// as I do in overview when I'm mousedover that window").
//
// One provider for every Super+key action that acts on "the hovered window,
// else the focused one": the overview's selected tile (the one under the
// pointer, or the keyboard's after the arrows; IOverview::actionTile) -- else
// nothing, and the caller falls back to the focused window.
//
// Its users today: Super+F (OverviewFullscreen.hpp, both of Hyprland's
// fullscreen actions). The overview's own close chord (Super+W) and its
// `window select|close` dispatcher read the same tile directly
// (selectedOverviewTile). NOT yet a source: the dock's hover-preview windows
// (DockPreview.cpp) -- the second source this is shaped for.
namespace Hyprdrive::HoverTarget {
    PHLWINDOW window();
}
