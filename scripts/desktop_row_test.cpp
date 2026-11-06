// Host test for DesktopRowModel.hpp: which desktops the wheel's row holds.
#include "../src/DesktopRowModel.hpp"

#include <cstdio>
#include <vector>

using namespace ScrollOverview;

static int g_checks = 0, g_failed = 0;
#define CHECK(c)                                                                                                                                                                   \
    do {                                                                                                                                                                           \
        ++g_checks;                                                                                                                                                                \
        if (!(c)) {                                                                                                                                                                \
            ++g_failed;                                                                                                                                                            \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c);                                                                                                               \
        }                                                                                                                                                                          \
    } while (0)

using V = std::vector<int>;

int main() {
    // Windows on 1, 2 and 5; desktop 3 exists empty; we are on 2.
    DesktopRow::SFacts f{.populated = {1, 2, 5}, .existing = {1, 2, 3, 5}, .elsewhere = {}, .current = 2};

    // Defaults: stop at the first empty one past the last with windows (6),
    // every number between visited.
    CHECK(DesktopRow::firstEmpty(f) == 6);
    CHECK(DesktopRow::row(f) == (V{1, 2, 3, 4, 5, 6}));

    // Populated only: 1, 2, 5 and the first empty 6.
    f.populatedOnly = true;
    CHECK(DesktopRow::row(f) == (V{1, 2, 5, 6}));
    // ... and the desktop shown, empty or not, so the wheel can leave it.
    f.current = 3;
    CHECK(DesktopRow::row(f) == (V{1, 2, 3, 5, 6}));
    f.current       = 2;
    f.populatedOnly = false;

    // Not stopping at the empty one: up to 10, or the highest that exists.
    f.stopAtEmpty = false;
    CHECK(DesktopRow::row(f) == (V{1, 2, 3, 4, 5, 6, 7, 8, 9, 10}));
    f.existing.push_back(14);
    CHECK(DesktopRow::row(f).back() == 14);
    f.populatedOnly = true; // all populated + the first empty, however far the range goes
    CHECK(DesktopRow::row(f) == (V{1, 2, 5, 6}));
    f.populatedOnly = false;
    f.existing.pop_back();
    f.stopAtEmpty = true;

    // Nothing anywhere: desktop 1 is the first empty one and the whole row.
    CHECK(DesktopRow::row(DesktopRow::SFacts{.current = 1}) == (V{1}));

    // Another monitor's desktops are not in this row, and the first empty one
    // skips past them.
    f.elsewhere = {6, 4};
    CHECK(DesktopRow::firstEmpty(f) == 7);
    CHECK(DesktopRow::row(f) == (V{1, 2, 3, 5, 7}));
    f.elsewhere.clear();

    // The shown desktop past the end (it was reached before its windows left)
    // stays in the row so the wheel can come back from it.
    f.current = 9;
    CHECK(DesktopRow::row(f) == (V{1, 2, 3, 4, 5, 6, 9}));
    f.current = 2;

    // Neighbours along a row, the ends returning the id itself.
    const V ROW{1, 2, 5, 6};
    CHECK(DesktopRow::neighbour(ROW, 2, 1) == 5);
    CHECK(DesktopRow::neighbour(ROW, 5, -1) == 2);
    CHECK(DesktopRow::neighbour(ROW, 6, 1) == 6);
    CHECK(DesktopRow::neighbour(ROW, 1, -1) == 1);
    CHECK(DesktopRow::neighbour(ROW, 3, 1) == 5); // not in the row: the nearest that way
    CHECK(DesktopRow::neighbour(ROW, -98, 1) == 1); // a named desktop

    std::printf("desktop_row_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
