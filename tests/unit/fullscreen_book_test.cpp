// Host test for FullscreenBook.hpp: the fullscreen desktop's places are kept by
// number (a debt never has place -1, the old "paying back place -1" that ran
// shift(0, -1)), and the payback decision.
#include "FullscreenBook.hpp"
#include "DesktopPlan.hpp"

#include <cstdio>

using namespace Hyprdrive::FullscreenBook;

static int g_checks = 0, g_failed = 0;
#define CHECK(c)                                                                                                                                                                   \
    do {                                                                                                                                                                           \
        ++g_checks;                                                                                                                                                                \
        if (!(c)) {                                                                                                                                                                \
            ++g_failed;                                                                                                                                                            \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c);                                                                                                               \
        }                                                                                                                                                                          \
    } while (0)

static void places() {
    // a live desktop's number wins; a gone one's number is the one kept
    static_assert(placeOf(true, 5, 2) == 5);
    static_assert(placeOf(false, 5, 2) == 2);
    static_assert(placeOf(true, -1, 2) == 2);
    // THE OLD BUG: a trip's desktop gone when its app closed (not on screen):
    // the debt took its number from the desktop alone -> -1. Now the trip keeps
    // its number from the start, and the debt takes that.
    {
        Id   tripAt = 2;                                // startTrip: the place it was given
        bool alive  = false;                            // the app closed, its desktop went
        Id   debtAt = placeOf(alive, NO_PLACE, tripAt); // endTrip
        CHECK(debtAt == 2);
        CHECK(decide(EKind::Trip, {.at = debtAt}) == EPay::Pay);
    }
    // kept numbers move with a shift from at or below them, not above
    static_assert(movedPlace(3, 3, +1) == 4);
    static_assert(movedPlace(3, 4, +1) == 3);
    static_assert(movedPlace(5, 3, -1) == 4);
    // a debt with no place is refused, never paid
    CHECK(decide(EKind::Trip, {.at = NO_PLACE}) == EPay::Refuse);
    CHECK(decide(EKind::Beside, {.at = 0}) == EPay::Refuse);
    CHECK(decide(EKind::Trip, {.at = -1, .exists = true}) == EPay::Refuse);
}

static void decisions() {
    // Trip: a foreign window makes it a desktop of its own; own leftovers do not
    CHECK(decide(EKind::Trip, {.at = 2, .exists = true, .foreign = true, .any = true}) == EPay::NothingOwed);
    CHECK(decide(EKind::Trip, {.at = 2, .exists = true, .foreign = false, .any = true}) == EPay::Pay);
    // Beside: any window there waits (the window it was made for still lives)
    CHECK(decide(EKind::Beside, {.at = 3, .exists = true, .foreign = true, .any = true}) == EPay::WaitWindows);
    CHECK(decide(EKind::Beside, {.at = 3, .exists = true, .onScreen = true}) == EPay::WaitScreen);
    CHECK(decide(EKind::Beside, {.at = 3}) == EPay::Pay);
    // on screen or sliding: wait, either kind
    CHECK(decide(EKind::Trip, {.at = 2, .exists = true, .onScreen = true}) == EPay::WaitScreen);
    CHECK(decide(EKind::Trip, {.at = 2, .exists = true, .driving = true}) == EPay::WaitScreen);
    CHECK(decide(EKind::Trip, {.at = 2, .driving = true}) == EPay::WaitScreen);
    // a fullscreen app in the place: its trip owes it now
    CHECK(decide(EKind::Trip, {.at = 2, .exists = true, .any = true, .trip = true}) == EPay::ToTrip);
    CHECK(decide(EKind::Beside, {.at = 2, .exists = true, .any = true, .trip = true}) == EPay::ToTrip);
    // start: alone or stay settles; next makes room only when not free; free walks
    const auto NONE_FREE = [](Id) { return false; };
    const auto ALL_FREE  = [](Id) { return true; };
    CHECK(start("next", 1, 0, ALL_FREE).settle);
    CHECK(start("stay", 1, 3, ALL_FREE).settle);
    CHECK(!start("next", 1, 2, ALL_FREE).makeRoom && start("next", 1, 2, ALL_FREE).at == 2);
    CHECK(start("next", 1, 2, NONE_FREE).makeRoom && start("next", 1, 2, NONE_FREE).at == 2);
    CHECK(start("free", 1, 2, [](Id i) { return i == 5; }).at == 5);
    CHECK(!start("free", 1, 2, [](Id i) { return i == 5; }).makeRoom);
    CHECK(beside(2, NONE_FREE).at == 3 && beside(2, NONE_FREE).makeRoom);
    CHECK(!beside(2, ALL_FREE).makeRoom);
}

int main() {
    places();
    decisions();
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
