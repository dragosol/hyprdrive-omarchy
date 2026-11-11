#pragma once
#include "globals.hpp"
// FIXED-SIZE WINDOWS TILE AT THEIR OWN SIZE (user, 2026-11-10: "super+t should
// toggle between tiling fixed-size windows at their own size and floating").
//
// A window whose program fixes its size -- X11 WM_NORMAL_HINTS min == max, an
// xdg toplevel's min == max: Battle.net's login, Wine dialogs, a game in a
// window -- cannot fill a tile. Dwindle refuses it outright (its max size is
// under the tile: "make it floating", DwindleAlgorithm.cpp), so Super+T did
// nothing there; Mosaic and scrolling took it at the tile's size, which Wine
// answers by asking for its own size again, about a thousand times a second
// (Battle.net's login, 2026-11-06 and 2026-11-10: the white flicker, the
// stalled login, GTK apps losing the wheel).
//
// So it tiles the way Hyprland tiles anything at its own size: pseudotiled,
// its pseudo size its fixed size -- centred in its tile, never stretched -- and
// no_max_size, which lets dwindle take it. Both go on just before the window is
// tiled and come off when it is floated again: Config::Actions::floatWindow is
// hooked, the one function every float toggle reaches (Super+T, hyprctl,
// Omarchy's menu, the window pill's double-click). Any other window passes
// straight through.
//
// A tile smaller than the window must not squeeze it either (Hyprland scales a
// pseudotiled window down to fit): place() grows that one window's tile for
// the one call that sizes it. Not misc:size_limits_tiled, which 20-layout.lua
// set for this on 2026-11-10: under it Hyprland also clamps EVERY tiled
// window's position into the screen's work area, which piled the infinite
// canvas's windows -- the ones it keeps past the screen's edge -- on top of
// each other (user: "something is wrong with infinite canvas ... you broke it").
namespace Layout {
    struct STargetBox;
}

namespace Hyprdrive::OwnSizeTile {
    using PlaceFn = void (*)(void* target, const Layout::STargetBox& box, uint8_t flags);

    void init();
    void shutdown();

    // CWindowTarget::setPositionGlobal's hook body (main.cpp, inside
    // DockRoom::place): `next` places the target. An own-size tile smaller
    // than its window is placed through a grown copy of its box -- the window
    // centred where the tile is, at its own size, kept on screen while the tile
    // is -- and the layout's own box is put back after. Everything else goes
    // straight to `next`.
    void place(void* target, const Layout::STargetBox& box, uint8_t flags, PlaceFn next);

    // `w` fixes its own size; `size` gets it in layout px.
    bool fixedSize(const PHLWINDOW& w, Vector2D& size);

    // `w` sits in its tile at its own size, by this module.
    bool owned(const PHLWINDOW& w);
}
