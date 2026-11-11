#pragma once

#include "globals.hpp"
#include "PlacementModel.hpp"

// WHERE A WINDOW GOES: ONE DECISION, ONE OPENER (PlacementModel.hpp has the
// rules and the owner's words).
//
// window.openEarly -- after Hyprland's float decision and the window rules,
// before the layout files the window and before its fullscreen takeover -- is
// the only place a new window's floating, tiling and desktop are decided:
//   1. Intent::judge: did the user ask for it (on every desktop);
//      Intent::opened: Mosaic's quiet focus for an unasked one;
//   2. PlacementModel::decide;
//   3. applied: Tile clears the floating flag (fixed-size windows are set up
//      to tile at their own size first, OwnSizeTile), so the window is tiled
//      from its first frame -- no replayed open animation; Beside moves it to
//      the next desktop (FullscreenDesktop::beside) and decides again there.
// Placement registers the one openEarly listener for all of this, so no
// registration order decides anything.
//
// After the map, one funnel (CWindowTarget::setFloating, hooked): a window
// that became floating without the user -- a layout's refusal, a group, a
// swap -- is decided again on the event loop and tiled back where the
// enforcer is on, within a budget (PlacementModel::CBudget) that gives a
// fighting window up, floating, logged once. The user's own floats and tiles
// (window.floating: Super+T, hyprctl, the pill, the trackpad float, a drag)
// are his INTENT, kept for the window's life and, for an X11 window, across a
// Wine remap (a new window for the same X window).
namespace Hyprdrive::WindowPlacement {
    // PLUGIN_INIT (after registerConfig, Mosaic, Intent, FullscreenDesktop,
    // OwnSizeTile) / PLUGIN_EXIT.
    void init();
    void shutdown();

    // A MAIN WINDOW of its app -- what the dock's "Always Open in" chips act
    // on: tiled (as placed), no parent, not modal, not on a special desktop.
    // The one "is this a dialog" test.
    bool mainWindow(const PHLWINDOW& w);

    // What was decided for `w` as it mapped (Leave when it was not seen).
    PlacementModel::SOutcome outcomeOf(const PHLWINDOW& w);
}
