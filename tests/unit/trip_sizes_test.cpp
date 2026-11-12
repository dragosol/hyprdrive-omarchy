// Host test for TripSizes.hpp: a window's sizes wait between its fullscreen
// change and the trip's move, and only then.
#include "TripSizes.hpp"

#include <cstdio>

using namespace Hyprdrive::TripSizes;

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
    // fullscreen among others, before the pass that starts its trip: waits
    CHECK(wait({.mapped = true, .fullscreen = true, .ordinary = true, .others = 2}));
    // ...not when it is settled there (alone, "stay", outside the row)
    CHECK(!wait({.mapped = true, .fullscreen = true, .ordinary = true, .others = 2, .settled = true}));
    // ...nor alone on its desktop, nor on a special one, nor not fullscreen
    CHECK(!wait({.mapped = true, .fullscreen = true, .ordinary = true, .others = 0}));
    CHECK(!wait({.mapped = true, .fullscreen = true, .ordinary = false, .others = 2}));
    CHECK(!wait({.mapped = true, .fullscreen = false, .ordinary = true, .others = 2}));
    // the trip's own move: waits whatever the rest says
    CHECK(wait({.mapped = true, .carrying = true}));
    CHECK(wait({.mapped = true, .carrying = true, .onTrip = true, .onTripSpace = true, .fullscreen = true}));
    // on its trip, fullscreen on its desktop: where it belongs, sizes go
    CHECK(!wait({.mapped = true, .onTrip = true, .onTripSpace = true, .fullscreen = true, .ordinary = true}));
    // out of fullscreen on its trip's desktop: going home next pass, waits
    CHECK(wait({.mapped = true, .onTrip = true, .onTripSpace = true, .fullscreen = false, .ordinary = true}));
    // on a trip but sent elsewhere: stays there, sizes go
    CHECK(!wait({.mapped = true, .onTrip = true, .onTripSpace = false, .fullscreen = false, .ordinary = true, .others = 3}));
    // unmapped (a Wine remap): nothing to hold
    CHECK(!wait({.mapped = false, .carrying = true}));
    CHECK(!wait({.mapped = false, .onTrip = true, .onTripSpace = true}));
    std::printf("trip_sizes_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
