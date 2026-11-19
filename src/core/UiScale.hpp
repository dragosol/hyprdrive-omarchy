#pragma once
// THE OVERVIEW'S PROPORTIONS (owner, 2026-11-19, "option B"). The search pill,
// the results, the desktop strip and the gaps between them were drawn on a
// 3840x2160 screen at scale 1 (2160 logical px tall) and kept those logical
// sizes everywhere, so on a 1280x800 laptop the chrome took a third of the
// screen before a window preview got any. Two factors off the monitor's
// LOGICAL height fix that:
//
//   layout  uL = clamp(H / 2160, 0.5, 1): the SPACE the chrome takes -- the
//           strip band, its pad and gaps, the grid gap, the search row's
//           margins, the label gaps -- so it keeps its share of the screen;
//   type    uT = clamp(sqrt(uL), 0.9, 1): what must stay legible and easy to
//           hit -- the pill, result rows, their text, the strip's label -- at
//           half strength, and never below 90%.
//
// H is the monitor's logical height, not the work area: a bar that comes and
// goes must not resize the overview, and 3840x2160 with a top bar is still
// exactly 1 (the reference look, pixel for pixel).
//
// plugin:hyprdrive:ui_scale is "auto" (the above) or a number, which is used
// in place of H / 2160 on every monitor: 1 is the look from before this existed,
// everywhere. A number also turns off the two rules that only make sense
// against the screen (the results' 25% share and dropping the inspector on a
// narrow screen), so 1 really is the old look.
//
// Pure, for tests/unit/ui_scale_test.cpp. OverviewInternal.hpp memoises the
// setting per frame and asks this for a monitor's factors.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>

namespace Hyprdrive::UiScale {
    inline constexpr float REF_HL     = 2160.F; // the reference screen's logical height
    inline constexpr float LAYOUT_MIN = 0.5F, LAYOUT_MAX = 1.F;
    inline constexpr float TYPE_MIN   = 0.9F, TYPE_MAX = 1.F;
    inline constexpr float AUTO       = -1.F; // the setting's "auto"

    // The results band's budget: at most 360 type px, and at most this share
    // of the screen -- 0.25 under auto; the old 0.42 when the factors are pinned.
    inline constexpr float BUDGET_L = 360.F, BUDGET_SHARE = 0.25F, BUDGET_SHARE_PINNED = 0.42F;

    // Below this logical width the inspector is dropped and its room goes to the
    // result columns. At 1200 the span is ~1000 px (0.84 of the width), and three
    // columns at their 160 px floor, the chrome and a 280 px inspector leave the
    // file paths ~180 px; under it they would be cut to nothing.
    inline constexpr float INSPECTOR_MIN_WL = 1200.F;
    // A screen this narrow (or a portrait one) also shrinks the inspector and the
    // columns' floor by the type factor.
    inline constexpr float NARROW_WL = 1600.F;

    struct SFactors {
        float layout = 1.F;
        float type   = 1.F;
        bool  pinned = false; // a number, not auto
    };

    // The setting's text -> AUTO, or the pinned factor in [LAYOUT_MIN, LAYOUT_MAX].
    // Anything that is not a positive number reads as auto.
    inline float parse(const std::string& text) {
        const auto B = text.find_first_not_of(" \t");
        if (B == std::string::npos)
            return AUTO;
        const char* const V   = text.c_str() + B;
        char*             end = nullptr;
        const float       N   = std::strtof(V, &end);
        if (end == V || !std::isfinite(N) || N <= 0.F)
            return AUTO;
        for (const char* p = end; *p; ++p) {
            if (*p != ' ' && *p != '\t')
                return AUTO;
        }
        return std::clamp(N, LAYOUT_MIN, LAYOUT_MAX);
    }

    inline float typeOf(float layout) {
        return std::clamp(std::sqrt(layout), TYPE_MIN, TYPE_MAX);
    }

    // `setting`: AUTO or a value parse() returned. `logicalH` <= 0 (no monitor)
    // is the reference look.
    inline SFactors factors(float logicalH, float setting) {
        SFactors f;
        if (setting > 0.F) {
            f.pinned = true;
            f.layout = std::clamp(setting, LAYOUT_MIN, LAYOUT_MAX);
        } else if (logicalH > 0.F && std::isfinite(logicalH))
            f.layout = std::clamp(logicalH / REF_HL, LAYOUT_MIN, LAYOUT_MAX);
        f.type = typeOf(f.layout);
        return f;
    }

    // min(360 x uT, share x H).
    inline float resultsBudgetL(float logicalH, const SFactors& f) {
        return std::min(BUDGET_L * f.type, logicalH * (f.pinned ? BUDGET_SHARE_PINNED : BUDGET_SHARE));
    }

    inline bool inspectorShown(float logicalW, const SFactors& f) {
        return f.pinned || logicalW >= INSPECTOR_MIN_WL;
    }

    // What the inspector's widths and the columns' floor are multiplied by.
    inline float narrowK(float logicalW, float logicalH, const SFactors& f) {
        return (logicalW < logicalH || logicalW < NARROW_WL) ? f.type : 1.F;
    }
}
