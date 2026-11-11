// Host test for HoverTargetRule.hpp: which window a Super+key action that
// names none acts on -- the overview's tile, else the dock preview under the
// pointer, else the focused one -- and when the dock's pane names one.
#include "HoverTargetRule.hpp"

#include <cstdio>

using namespace Hyprdrive::HoverTarget;

// overview tile > dock preview > none
static_assert(source(true, false) == ESource::OVERVIEW_TILE);
static_assert(source(true, true) == ESource::OVERVIEW_TILE); // a tile and a cell both under the pointer: the tile
static_assert(source(false, true) == ESource::DOCK_PREVIEW);
static_assert(source(false, false) == ESource::NONE);

// paneShown, pointerOnMenu, tearingOut, cellUnderPointer
static_assert(dockCellCounts(true, false, false, true));   // the pointer on a cell of the pane
static_assert(!dockCellCounts(true, false, false, false)); // a gap, the pane's edge, the row preview beside a menu
static_assert(!dockCellCounts(false, false, false, true)); // hidden with the dock, or leaving
static_assert(!dockCellCounts(true, true, false, true));   // a menu's plate over the cell
static_assert(!dockCellCounts(true, false, true, true));   // a preview being torn out

// named, hasTarget, targetIsFocused
static_assert(retarget(false, true, false));   // Super+F over another window's tile or preview
static_assert(!retarget(false, true, true));   // over the focused window's own: as before
static_assert(!retarget(false, false, false)); // no target: the focused window
static_assert(!retarget(true, true, false));   // a window named (the dock's chip, the pill): the caller's

int main() {
    int checks = 0, failed = 0;
    for (int m = 0; m < 16; ++m) {
        const bool A = m & 1, B = m & 2, C = m & 4, D = m & 8;
        ++checks;
        if (dockCellCounts(A, B, C, D) != (A && !B && !C && D)) {
            ++failed;
            std::printf("FAIL dockCellCounts %d %d %d %d\n", A, B, C, D);
        }
        if (m < 8) {
            ++checks;
            if (retarget(A, B, C) != (!A && B && !C)) {
                ++failed;
                std::printf("FAIL retarget named=%d target=%d focused=%d\n", A, B, C);
            }
        }
        if (m < 4) {
            ++checks;
            const auto WANT = A ? ESource::OVERVIEW_TILE : B ? ESource::DOCK_PREVIEW : ESource::NONE;
            if (source(A, B) != WANT) {
                ++failed;
                std::printf("FAIL source tile=%d cell=%d\n", A, B);
            }
        }
    }
    std::printf("hover_target_test: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
