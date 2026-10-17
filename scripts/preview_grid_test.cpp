// Host test for PreviewGrid.hpp (the dock's hover previews in rows).
#include "../PreviewGrid.hpp"

#include <cstdio>

using namespace ScrollOverview::PreviewGrid;

static int g_checks = 0, g_failed = 0;
static void expect(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failed;
        std::printf("FAIL %s\n", what);
    }
}

int main() {
    const float GAP = 10.F;
    expect(rows({}, GAP, 1000.F).empty(), "no windows: no rows");
    expect(rows({150.F, 150.F, 150.F}, GAP, 1920.F) == std::vector<size_t>{3}, "a few: one row");
    // The user's 22 ghostty windows on 4K: 145 px cells, half of 3840 = 1920.
    const std::vector<float> G22(22, 145.F);
    const auto               R = rows(G22, GAP, 1920.F);
    expect(R == std::vector<size_t>{11, 11}, "22 ghostty: two even rows of 11");
    expect(widest(G22, R, GAP) <= 1920.F, "each row fits half the screen");
    size_t total = 0;
    for (const size_t C : R)
        total += C;
    expect(total == 22, "every window has a cell");
    // Uneven: 23 -> 12 + 11 (extra in the first row).
    expect(rows(std::vector<float>(23, 145.F), GAP, 1920.F) == std::vector<size_t>{12, 11}, "an uneven count: the first row takes the extra");
    // Many: the fewest rows that fit (14 a row would be 2160 px, so four of 10).
    const auto R3 = rows(std::vector<float>(40, 145.F), GAP, 1920.F);
    expect(R3 == std::vector<size_t>{10, 10, 10, 10}, "40 windows: four even rows of 10, the fewest that fit");
    // Exactly at the limit stays one row.
    expect(rows({955.F, 955.F}, GAP, 1920.F) == std::vector<size_t>{2}, "a row exactly at the limit stays one row");
    // A cell wider than the limit alone still gets a row.
    expect(rows({3000.F, 100.F}, GAP, 1920.F) == std::vector<size_t>{1, 1}, "an over-wide cell gets its own row");
    std::printf("preview_grid_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
