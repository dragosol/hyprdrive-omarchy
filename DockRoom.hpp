#pragma once

// WINDOWS MAKE ROOM FOR THE DOCK (user, 2026-09-30): with autohide OFF, a tiled
// window that would sit under the dock's pill is shortened to stop just above
// it -- only the windows actually in the way, and only by as much as they
// overlap. Not a reserved strip across the whole monitor: a window beside the
// pill keeps its full height, so on a scrolling desktop a window grows back to
// full height the moment the tape carries it clear of the dock. Hyprland
// animates each change, so it is smooth. Autohide ON: nothing happens, the dock
// overlaps windows as before.
//
// The layout still decides every box; this only trims one after the fact, in
// CWindowTarget::setPositionGlobal (hooked in main.cpp), where every tiled
// placement from every layout passes. dock:windows_make_room turns it off.

#include "globals.hpp"

namespace Layout {
    struct STargetBox;
}

namespace ScrollOverview::DockRoom {

    using PlaceFn = void (*)(void* target, const Layout::STargetBox& box, uint8_t flags);

    // The hook body: place the target through `place` (the original), then
    // trim it if it ended up under the pill.
    void place(void* target, const Layout::STargetBox& box, uint8_t flags, PlaceFn original);

    // Per monitor, per frame: when the pill the windows avoid changes (an icon
    // added, autohide toggled, the dock switched off), relayout that monitor.
    void watch(PHLMONITOR monitor);

    // PLUGIN_EXIT: a relayout left queued would call into unloaded code.
    void shutdown();
}
