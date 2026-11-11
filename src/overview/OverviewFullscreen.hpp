#pragma once

// SUPER+F ACTS ON THE HOVERED TILE IN THE OVERVIEW (user, 2026-11-11: "super+f
// should act on the hovered tile in overview").
//
// The user's fullscreen bind -- a Lua hl.dsp.window.fullscreen, the legacy
// `fullscreen` / `fullscreenstate` dispatchers -- stays live in the overview's
// submap (SubmapBorrow.hpp) and names no window: Hyprland's action then takes
// the FOCUSED one. With the overview open, the window the user means is the
// tile the overview shows as selected (selectedOverviewTile: the one under the
// pointer, or the keyboard's after the arrows), as for Super+W. So a fullscreen
// action that names no window, taken while an overview is open and shows a
// selected tile, is given that tile -- one argument changed, the rest of the
// path (Hyprland's action, the fullscreen controller, the layout, the
// fullscreen desktop, the overview's own fullscreen handling) the same as for
// the focused window. No tile selected (the pointer over the strip, the dock,
// a gap): nothing changes, the focused window it is.
//
// One limit: a bind's "set"/"unset" (not "toggle") is decided by Hyprland
// against the FOCUSED window before the action runs; the action then toggles
// the tile. With the tile and the focused window in different states that is
// the opposite of "set"; Super+F is a toggle.
namespace Hyprdrive::OverviewFullscreen {
    // Is the action given the overview's tile instead of what it was called
    // with? Only when it named no window (a window named is the caller's: the
    // dock's chip, the window pill), an overview is open, it shows a selected
    // tile, and that tile is not the focused window already.
    constexpr bool retarget(bool named, bool overviewOpen, bool hasTile, bool tileIsFocused) {
        return !named && overviewOpen && hasTile && !tileIsFocused;
    }

    // PLUGIN_INIT: hook Hyprland's two fullscreen actions. Missing: Hyprland's
    // own stands (the focused window), and the log says so.
    void init();
}
