// ui_scale_test: the overview's proportions (UiScale.hpp): the two factors off
// the monitor's logical height, the ui_scale setting, the results budget and
// the inspector's narrow-screen rules.
#include "../../src/core/UiScale.hpp"
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
    return std::fabs(a - b) < 0.0005F;
}

int main() {
    using namespace Hyprdrive::UiScale;

    // The reference is exactly 1, bit for bit: every product with it is the old number.
    const auto REF = factors(2160.F, AUTO);
    check(REF.layout == 1.F && REF.type == 1.F && !REF.pinned, "3840x2160 at scale 1: both factors exactly 1");
    check(factors(2880.F, AUTO).layout == 1.F && factors(2880.F, AUTO).type == 1.F, "taller than the reference: capped at 1");
    check(factors(0.F, AUTO).layout == 1.F && factors(-5.F, AUTO).type == 1.F, "no monitor: the reference look");

    // The geometries the gate runs (logical heights).
    check(near(factors(1440.F, AUTO).layout, 1440.F / 2160.F), "2560x1440: space 0.667");
    check(near(factors(1440.F, AUTO).type, 0.9F), "2560x1440: type clamps at 0.9 (sqrt 0.816)");
    check(near(factors(1920.F, AUTO).layout, 0.8889F) && near(factors(1920.F, AUTO).type, 0.9428F), "1080x1920 portrait: 0.889 / 0.943");
    check(near(factors(1125.F, AUTO).layout, 0.5208F) && near(factors(1125.F, AUTO).type, 0.9F), "1800x1125 (1920x1200@1.0667, 2880x1800@1.6)");
    check(near(factors(1080.F, AUTO).layout, 0.5F) && near(factors(800.F, AUTO).layout, 0.5F), "1080 and 800 tall: the space factor's floor, 0.5");
    check(near(factors(800.F, AUTO).type, 0.9F), "the type factor's floor, 0.9");
    check(near(factors(1749.6F, AUTO).type, 0.9F) && factors(1760.F, AUTO).type > 0.9F, "type leaves its floor at 0.81 x 2160");

    // The setting.
    check(parse("auto") == AUTO && parse("") == AUTO && parse("  ") == AUTO, "auto, empty: auto");
    check(parse("big") == AUTO && parse("0") == AUTO && parse("-1") == AUTO && parse("0.8x") == AUTO, "not a positive number: auto");
    check(near(parse("1"), 1.F) && near(parse(" 0.8 "), 0.8F) && near(parse("0.85"), 0.85F), "numbers, spaces around them");
    check(near(parse("0.2"), LAYOUT_MIN) && near(parse("3"), LAYOUT_MAX), "a number is clamped to 0.5 .. 1");
    const auto ONE = factors(800.F, parse("1"));
    check(ONE.pinned && ONE.layout == 1.F && ONE.type == 1.F, "ui_scale = 1 on a short screen: exactly the reference");
    const auto HALF = factors(2160.F, parse("0.5"));
    check(HALF.pinned && near(HALF.layout, 0.5F) && near(HALF.type, 0.9F), "ui_scale = 0.5 on 4K: what a 1080 screen gets");

    // The results budget: min(360 x uT, 0.25 x H); pinned, the old 0.42 share.
    check(resultsBudgetL(2160.F, REF) == 360.F, "4K: 360, as before");
    check(near(resultsBudgetL(800.F, factors(800.F, AUTO)), 200.F), "800 tall: a quarter of the screen");
    check(near(resultsBudgetL(1440.F, factors(1440.F, AUTO)), 324.F), "1440 tall: 360 x 0.9");
    check(near(resultsBudgetL(800.F, ONE), 336.F), "ui_scale = 1 at 800 tall: the old min(360, 0.42 H)");

    // The inspector.
    check(inspectorShown(1280.F, factors(800.F, AUTO)) && inspectorShown(1200.F, factors(800.F, AUTO)), "1280 and 1200 wide: inspector shown");
    check(!inspectorShown(1080.F, factors(1920.F, AUTO)), "1080 wide (portrait): no inspector");
    check(inspectorShown(1080.F, factors(1920.F, parse("1"))), "pinned: the inspector is always there, as before");
    check(narrowK(3840.F, 2160.F, REF) == 1.F && narrowK(1920.F, 1080.F, factors(1080.F, AUTO)) == 1.F, "wide screens: the inspector keeps its widths");
    check(near(narrowK(1280.F, 800.F, factors(800.F, AUTO)), 0.9F) && near(narrowK(1080.F, 1920.F, factors(1920.F, AUTO)), 0.9428F), "narrow or portrait: the type factor");

    std::printf("ui_scale_test: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
