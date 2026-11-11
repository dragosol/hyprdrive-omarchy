// Host test for OverviewFullscreen.hpp: when a fullscreen action is given the
// overview's selected tile instead of the window it was called with.
#include "OverviewFullscreen.hpp"

#include <cstdio>

using Hyprdrive::OverviewFullscreen::retarget;

// named, overviewOpen, hasTile, tileIsFocused
static_assert(retarget(false, true, true, false));   // Super+F over another window's tile
static_assert(!retarget(false, true, true, true));   // over the focused window's own tile: as before
static_assert(!retarget(false, true, false, false)); // over no tile (strip, dock, a gap): the focused window
static_assert(!retarget(false, false, true, false)); // no overview open: Hyprland's own
static_assert(!retarget(true, true, true, false));   // a window named (the dock's chip, the pill): the caller's
static_assert(!retarget(true, false, false, false));

int main() {
    int checks = 0, failed = 0;
    for (int m = 0; m < 16; ++m) {
        const bool NAMED = m & 1, OPEN = m & 2, TILE = m & 4, FOCUSED = m & 8;
        ++checks;
        // the only combination that changes the target
        if (retarget(NAMED, OPEN, TILE, FOCUSED) != (!NAMED && OPEN && TILE && !FOCUSED)) {
            ++failed;
            std::printf("FAIL named=%d open=%d tile=%d focused=%d\n", NAMED, OPEN, TILE, FOCUSED);
        }
    }
    std::printf("overview_fullscreen_test: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
