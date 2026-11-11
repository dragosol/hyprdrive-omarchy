// Host test for MosaicTripHold.hpp: a window home from its fullscreen trip goes
// back to its place only when nothing changed on its desktop meanwhile.
#include "MosaicTripHold.hpp"

#include <cstdio>

using namespace Hyprdrive::MosaicTripHold;

static int g_checks = 0, g_failed = 0;
#define CHECK(c)                                                                                                                                                                   \
    do {                                                                                                                                                                           \
        ++g_checks;                                                                                                                                                                \
        if (!(c)) {                                                                                                                                                                \
            ++g_failed;                                                                                                                                                            \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c);                                                                                                               \
        }                                                                                                                                                                          \
    } while (0)

int main() {
    int a1 = 0, a2 = 0, a3 = 0, other = 0;
    const SKey K2{&a2, 596, 396, 2}, K3{&a3, 496, 346, 3};
    const SHold H{.traveller = &a1, .index = 0, .rest = {K2, K3}, .edits = 7};

    // the same window, carried home by its trip, to the same desktop: back in place
    CHECK(restorable(H, &a1, true, {K2, K3}, 7, false));
    CHECK(homeIndex(H, 2) == 0);
    // moved home by hand, not by the trip: the normal rules
    CHECK(!restorable(H, &a1, false, {K2, K3}, 7, false));
    // another window arriving
    CHECK(!restorable(H, &other, true, {K2, K3}, 7, false));
    CHECK(!restorable(H, nullptr, true, {K2, K3}, 7, false));
    // something changed meanwhile: a window closed, opened, reordered, resized
    CHECK(!restorable(H, &a1, true, {K2}, 7, false));
    CHECK(!restorable(H, &a1, true, {K2, K3, SKey{&other, 100, 100, 9}}, 7, false));
    CHECK(!restorable(H, &a1, true, {K3, K2}, 7, false));
    CHECK(!restorable(H, &a1, true, {K2, SKey{&a3, 500, 346, 3}}, 7, false));
    // the same windows, but an edit (a tile resized, a move) or a drop/swap set an arrangement
    CHECK(!restorable(H, &a1, true, {K2, K3}, 8, false));
    CHECK(!restorable(H, &a1, true, {K2, K3}, 7, true));
    // its place, clamped to the order it goes back into
    const SHold LAST{.traveller = &a1, .index = 5, .rest = {K2}, .edits = 0};
    CHECK(homeIndex(LAST, 1) == 1);
    const SHold MID{.traveller = &a2, .index = 1, .rest = {SKey{&a1, 1, 1, 1}, K3}, .edits = 0};
    CHECK(homeIndex(MID, 2) == 1);

    std::printf("mosaic_trip_hold_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
