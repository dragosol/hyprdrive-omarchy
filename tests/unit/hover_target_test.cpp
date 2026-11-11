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

// named, namedFocusedInBind, ownCall, hasTarget, targetIsFocused
static_assert(retarget(false, false, false, true, false));  // Super+F over another window's tile or preview
static_assert(!retarget(false, false, false, true, true));  // over the focused window's own: as before
static_assert(!retarget(false, false, false, false, false)); // no target: the focused window
static_assert(!retarget(true, false, false, true, false));  // a window named (the dock's chip, the pill): the caller's
static_assert(retarget(true, true, false, true, false));    // a bind naming the active window (Omarchy's move_window)
static_assert(!retarget(true, true, true, true, false));    // ... from the plugin's own dispatcher: plugin code names its window
static_assert(!retarget(false, false, true, true, false));  // nested in an action already retargeted, or a hyprdrive dispatcher

// retargeted, silent
static_assert(moveSilently(true, false));  // a hovered window moved by Super+Shift+N: no follow
static_assert(!moveSilently(false, false)); // the focused one: the bind's follow
static_assert(moveSilently(false, true));

int main() {
    int checks = 0, failed = 0;
    for (int m = 0; m < 16; ++m) {
        const bool A = m & 1, B = m & 2, C = m & 4, D = m & 8;
        ++checks;
        if (dockCellCounts(A, B, C, D) != (A && !B && !C && D)) {
            ++failed;
            std::printf("FAIL dockCellCounts %d %d %d %d\n", A, B, C, D);
        }
        for (int f = 0; f < 2; ++f) {
            ++checks;
            const bool NAMED = A, STANDIN = B, OWN = C, TARGET = D, FOCUSED = f;
            if (retarget(NAMED, STANDIN, OWN, TARGET, FOCUSED) != ((!NAMED || STANDIN) && !OWN && TARGET && !FOCUSED)) {
                ++failed;
                std::printf("FAIL retarget named=%d standin=%d own=%d target=%d focused=%d\n", NAMED, STANDIN, OWN, TARGET, FOCUSED);
            }
        }
        if (m < 4) {
            ++checks;
            if (moveSilently(A, B) != (A || B)) {
                ++failed;
                std::printf("FAIL moveSilently %d %d\n", A, B);
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
