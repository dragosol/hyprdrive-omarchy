#pragma once

#include "globals.hpp"

// A DROPPED WINDOW'S TILE IS THE LAYOUT'S DECISION, not the drop point's.
//
// Hyprland's dwindle reads the cursor on a drop ON PURPOSE, in two places, and
// both survive this rice's otherwise cursor-free config (force_split 2,
// smart_split off, precise_mouse_move off) -- read from DwindleAlgorithm.cpp,
// v0.56.2, addTarget:
//   * WHICH window it splits. use_active_for_splits splits the FOCUSED window,
//     and with input:follow_mouse 1 that is whatever is under the pointer at
//     release -- or, given a focal point, the node closest to it.
//   * WHICH SIDE. `force_split == 0 || m_overrideFocalPoint ||
//     dragController()->wasDraggingWindow()` takes the branch that compares
//     the cursor to the split's centre, so right after a drag the side follows
//     the mouse whatever force_split says. Hyprland's own dragEnd raises that
//     flag and re-tiles the window under it.
// So a window dragged into a desktop landed wherever the pointer happened to
// be, and a quick drop near a window's edge flipped the split (user,
// 2026-09-12: "I want dwindle to decide how it's tiled when I let go").
//
// retileAsNew re-files a just-dropped window the way OPENING one would place
// it: out of the tiling tree and back in, split off the window the user was
// working in, on the side force_split names. It does that without touching
// focus -- the overview listens for focus changes -- by handing dwindle that
// window's own CENTRE as the focal point: the closest node is then the anchor
// itself, and the side test compares its centre with its centre, which falls
// through to "new window second", i.e. force_split 2.
namespace ScrollOverview::Placement {

// The window focused as a drag STARTS. Focus follows the pointer for the rest
// of the gesture, so this is the only moment it still means "the window the
// user was working in".
void      noteDragAnchor(const PHLWINDOW& window);
PHLWINDOW takeDragAnchor();

// Re-file `window` on its own workspace as a new tile. `preferredAnchor` is used
// if it is tiled on that workspace; otherwise the workspace's last-focused
// window, then its first tiled one. Scrolling workspaces, floating windows and
// special workspaces are left alone.
void      retileAsNew(const PHLWINDOW& window, const PHLWINDOW& preferredAnchor);

}
