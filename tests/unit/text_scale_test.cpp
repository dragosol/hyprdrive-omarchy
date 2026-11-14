// text_scale_test: Omarchy's [font] base-size -> Hyprdrive's text scale (#8).
#include "../../src/core/TextScale.hpp"
#include <cstdio>

static int g_checks = 0, g_fail = 0;
static void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_fail;
        std::printf("FAIL: %s\n", what);
    }
}
static bool near(float a, float b) {
    return std::fabs(a - b) < 0.001F;
}

int main() {
    using namespace Hyprdrive::TextScale;
    check(near(fromShellToml(""), 1.F), "no file text: 1");
    check(near(fromShellToml("[bar]\nheight = 30\n"), 1.F), "no [font] table: 1");
    check(near(fromShellToml("[font]\nfamily = \"X\"\n"), 1.F), "[font] without base-size: 1");
    check(near(fromShellToml("[font]\nbase-size = 12\n"), 1.F), "the default 12: 1");
    check(near(fromShellToml("[font]\nbase-size = 10\n"), 10.F / 12.F), "10 (the report): 0.833");
    check(near(fromShellToml("[font]\n  base-size=13 # big\n"), 13.F / 12.F), "13, no spaces, a trailing comment");
    check(near(fromShellToml("[bar]\nbase-size = 20\n[font]\nbase-size = 9\n"), 0.75F), "another table's base-size is not it");
    check(near(fromShellToml("[font]\n# base-size = 20\nbase-size = 14\n"), 14.F / 12.F), "a commented-out line is skipped");
    check(near(fromShellToml("[font]\nbase-size = 9\n[other]\nbase-size = 30\n"), 0.75F), "the table ends at the next header");
    check(near(fromShellToml("[ font ]\nbase-size = 9\n"), 0.75F), "spaces inside the header");
    check(near(fromShellToml("[font]\nbase-size = 18\n"), 14.F / 12.F), "18 stops at 14: the search card's rows do not grow yet");
    check(near(fromShellToml("[font]\nbase-size = 2\n"), MIN_SCALE), "a tiny size stops at the floor");
    check(near(fromShellToml("[font]\nbase-size = 200\n"), MAX_SCALE), "a huge size stops at the ceiling");
    check(near(fromShellToml("[font]\nbase-size = \"big\"\n"), 1.F), "not a number: 1");
    check(near(fromShellToml("[font]\nbase-size = 0\n"), 1.F), "zero: 1");
    check(near(fromShellToml("[font]\nbase-size = -4\n"), 1.F), "negative: 1");
    check(scaled(15, 10.F / 12.F) == 13 && scaled(12, 1.F) == 12 && scaled(1, 0.5F) == 1 && scaled(16, 1.5F) == 24, "sizes round, never below 1");
    std::printf("text_scale_test: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
