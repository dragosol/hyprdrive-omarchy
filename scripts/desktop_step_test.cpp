// Host test for DesktopStepModel.hpp: the Super+scroll desktop walk (range,
// clamping, desktops on other monitors), one step per wheel notch, and which
// desktop changes the two animation switches snap.
#include "../DesktopStepModel.hpp"

#include <cstdio>
#include <set>

using namespace ScrollOverview::DesktopStep;

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
    const auto NONE = [](int) { return false; };

    // Empty desktops are visited: 1 -> 2 -> 3 with nothing on 2 or 3.
    CHECK(target(1, +1, 1, NONE) == 2);
    CHECK(target(2, +1, 2, NONE) == 3);
    CHECK(target(3, -1, 3, NONE) == 2);
    // Clamped, never wrapped.
    CHECK(target(1, -1, 1, NONE) == 1);
    CHECK(target(10, +1, 4, NONE) == 10);
    CHECK(target(9, +1, 4, NONE) == 10);
    // Past 10 only as far as the highest desktop that exists.
    CHECK(target(12, +1, 12, NONE) == 12);
    CHECK(target(11, +1, 12, NONE) == 12);
    CHECK(target(10, +1, 12, NONE) == 11);
    static_assert(top(3) == 10 && top(14) == 14);
    // Out-of-range starts step to the nearest end.
    CHECK(target(-98, +1, 1, NONE) == 1);
    CHECK(target(-98, -1, 1, NONE) == -98);
    CHECK(target(20, -1, 4, NONE) == 10);
    // Desktops on another monitor are skipped; none left = stay.
    const std::set<int> OTHER = {2, 3};
    const auto          ELSEWHERE = [&](int id) { return OTHER.contains(id); };
    CHECK(target(1, +1, 1, ELSEWHERE) == 4);
    CHECK(target(4, -1, 4, ELSEWHERE) == 1);
    const std::set<int> ALL = {1, 3, 4, 5, 6, 7, 8, 9, 10};
    CHECK(target(2, +1, 2, [&](int id) { return ALL.contains(id); }) == 2);
    CHECK(target(5, 0, 5, NONE) == 5);

    // One step per notch.
    {
        SNotchAccumulator a;
        CHECK(a.feed(120, 0) == 1);   // a classic wheel click
        CHECK(a.feed(120, 50) == 1);
        CHECK(a.feed(-120, 100) == -1);
    }
    {
        SNotchAccumulator a; // hi-res: eight 15ths make one notch
        int                n = 0;
        for (int i = 0; i < 8; ++i)
            n += a.feed(15, i * 8);
        CHECK(n == 1);
        CHECK(a.acc == 0);
        CHECK(a.feed(60, 70) == 0);   // half a notch is nothing
        CHECK(a.feed(-60, 80) == 0);  // reversal starts over...
        CHECK(a.feed(-60, 90) == -1); // ...so this is a whole notch back
    }
    {
        SNotchAccumulator a; // a stale half notch does not count after a pause
        CHECK(a.feed(60, 0) == 0);
        CHECK(a.feed(60, 5000) == 0);
        CHECK(a.feed(60, 5010) == 1);
    }
    {
        SNotchAccumulator a; // one big event (a fast free spin): its notches, signed
        CHECK(a.feed(360, 0) == 3);
        CHECK(a.feed(-250, 10) == -2);
        CHECK(a.acc == -10);
        CHECK(a.feed(0, 20) == 0);
    }

    // Cause, and what snaps.
    static_assert(classify(true, false, true) == ECause::Keyboard);
    CHECK(classify(false, true, true) == ECause::Keyboard);
    CHECK(classify(false, false, true) == ECause::Focus);
    CHECK(classify(false, false, false) == ECause::Other);
    CHECK(!snaps(ECause::Keyboard, true, false));
    CHECK(snaps(ECause::Keyboard, false, true));
    CHECK(!snaps(ECause::Focus, false, true));
    CHECK(snaps(ECause::Focus, true, false));
    CHECK(!snaps(ECause::Other, false, false));

    if (g_failed) {
        std::printf("%d of %d checks failed\n", g_failed, g_checks);
        return 1;
    }
    std::printf("%d checks\n", g_checks);
    return 0;
}
