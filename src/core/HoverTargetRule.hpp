#pragma once

// The pure rules behind HoverTarget.hpp and HoverActions.hpp, host-tested in
// tests/unit/hover_target_test.cpp.

namespace Hyprdrive::HoverTarget {
    // Where "the window the keybinds mean" comes from: the overview's selected
    // tile first, then the dock hover pane's cell under the pointer, else
    // nothing (the caller's own window: the focused one).
    enum class ESource {
        NONE,
        OVERVIEW_TILE,
        DOCK_PREVIEW,
    };

    constexpr ESource source(bool overviewTile, bool dockCell) {
        return overviewTile ? ESource::OVERVIEW_TILE : dockCell ? ESource::DOCK_PREVIEW : ESource::NONE;
    }

    // Does the dock's pane name a window right now? Only a cell under the
    // pointer of a pane that is on screen (not hidden with the dock, not
    // leaving), with no menu plate between the pointer and the cell, and no
    // preview being torn out of it. The row preview beside the app menu is
    // not a cell: it is drawn for a menu row and takes no pointer.
    constexpr bool dockCellCounts(bool paneShown, bool pointerOnMenu, bool tearingOut, bool cellUnderPointer) {
        return paneShown && !pointerOnMenu && !tearingOut && cellUnderPointer;
    }

    // Is an action given the hover target instead of what it was called
    // with? Only when it named no window (a window named is the caller's: the
    // dock's chip, the window pill, a script's address), there is a target,
    // and it is not the focused window already (then nothing changes).
    constexpr bool retarget(bool named, bool hasTarget, bool targetIsFocused) {
        return !named && hasTarget && !targetIsFocused;
    }
}
