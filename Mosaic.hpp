#pragma once

#include "globals.hpp"

#include <hyprland/src/devices/IPointer.hpp>

// MOSAIC (user, 2026-10-11): a Hyprland tiled layout named "mosaic", after
// GNOME's planned Mosaic. Windows keep their natural size and gather into one
// cluster centred on the desktop; when they no longer fit -- too many, or one
// too big -- the same layout tiles them, and it returns to the mosaic by itself
// once they fit again. The desktop's layout stays "mosaic" throughout: the tile
// arrangement is its second half, not a switch to another layout.
//
// The geometry is MosaicPack.hpp (pure, host-tested). Super+L's three-way
// cycle, the settings section and "Mosaic as the default" live in the panel and
// its fragments; this side only provides the layout.
//
// Symcheck rule: the algorithm class is defined OUTSIDE any anonymous namespace
// in Mosaic.cpp -- Hyprland holds it by its vtable and its typeid.
namespace ScrollOverview::Mosaic {
    // PLUGIN_INIT: register "mosaic" with Hyprland's layout matcher.
    void registerLayout(HANDLE handle);
    // PLUGIN_EXIT, FIRST: every desktop on mosaic is handed back to the default
    // layout -- its algorithm objects destroyed -- while this code is mapped.
    void unregisterLayout(HANDLE handle);

    // CORNER RESIZE (Mosaic.cpp). A mouse button event, before Hyprland sees
    // it: true = claimed (a corner resize started, or ours ended). `blocked`:
    // the overview, the dock or a lock screen owns the pointer.
    bool onButton(const IPointer::SButtonEvent& e, bool blocked);
    // Pointer motion: the resize cursor while over a grabbable corner.
    void onMove(bool blocked);
    // A window is closing (Hyprland's window.close, before the layout drops
    // it): on a desktop in the mosaic arrangement, its size becomes its app's
    // size in mosaic from now on.
    void onWindowClose(const PHLWINDOW& w);
    // PLUGIN_EXIT: our drag ended, our cursor override dropped.
    void shutdown();
}
