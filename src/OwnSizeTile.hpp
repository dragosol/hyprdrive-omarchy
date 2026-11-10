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
namespace ScrollOverview::OwnSizeTile {
    void init();
    void shutdown();

    // `w` fixes its own size; `size` gets it in layout px.
    bool fixedSize(const PHLWINDOW& w, Vector2D& size);

    // `w` sits in its tile at its own size, by this module.
    bool owned(const PHLWINDOW& w);
}
