// Host-side unit test for scrolling layout strip math:
// - Aspect ratio preservation & conditional stretching
// - Cumulative strip cell layout & GAP preservation
// - Proportional scaling under overflow
// - Invariant mini-window positioning across scroll camera offsets
//
// Needs no compositor: compiles with hyprutils alone.

#include <hyprutils/math/Box.hpp>
#include <hyprutils/math/Vector2D.hpp>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include <algorithm>

using Hyprutils::Math::CBox;
using Hyprutils::Math::Vector2D;

static int g_fail = 0, g_run = 0;

static void ok(bool cond, const std::string& what) {
    ++g_run;
    if (!cond) {
        ++g_fail;
        std::printf("FAIL  %s\n", what.c_str());
    }
}

static bool near(double a, double b, double eps = 1e-4) {
    return std::abs(a - b) <= eps;
}

// Emulate span ratio calculation
static float computeSpanRatio(float monitorW, float tapeRight, float minX, size_t tiledCount) {
    if (monitorW <= 0.F || tiledCount == 0)
        return 1.0F;

    const float leftMargin = std::max(0.F, minX);
    const float totalVirtualWidth = tapeRight + leftMargin;

    if (totalVirtualWidth <= monitorW)
        return 1.0F;

    return totalVirtualWidth / monitorW;
}

// Emulate multi-cell strip layout
struct SCellLayout {
    std::vector<float> widths;
    std::vector<float> xs;
    float height = 0.F;
    float totalWidth = 0.F;
};

static SCellLayout layoutStrip(float monitorW, float bandH, float gap, const std::vector<float>& spanRatios) {
    const size_t count = spanRatios.size();
    if (count == 0)
        return {};

    const float pad = bandH * 0.12F;
    const float aspect = 1.6F; // 1800 / 1125
    float cellH = bandH - 2.F * pad;
    const float baseW = cellH * aspect;

    std::vector<float> widths(count, baseW);
    float sumW = 0.F;
    for (size_t i = 0; i < count; ++i) {
        widths[i] = baseW * spanRatios[i];
        sumW += widths[i];
    }

    float total = sumW + gap * static_cast<float>(count - 1);
    const float maxW = monitorW - 2.F * pad;
    if (total > maxW && sumW > 0.F) {
        const float scaleDown = (maxW - gap * static_cast<float>(count - 1)) / sumW;
        cellH *= scaleDown;
        for (size_t i = 0; i < count; ++i)
            widths[i] *= scaleDown;
        total = maxW;
    }

    std::vector<float> xs(count);
    float curX = (monitorW - total) / 2.F;
    for (size_t i = 0; i < count; ++i) {
        xs[i] = curX;
        curX += widths[i] + gap;
    }

    return SCellLayout{widths, xs, cellH, total};
}

int main() {
    const float MON_W = 1800.F;
    const float MON_H = 1125.F;

    // 1. Zero windows or non-scrolling: span ratio is exactly 1.0
    ok(near(computeSpanRatio(MON_W, 0.F, 0.F, 0), 1.0), "0 windows returns 1.0");

    // 2. 1 window fitting within 1800px: no stretch
    // Window from 12 to 875 -> right=875, left=12, total=887 <= 1800
    ok(near(computeSpanRatio(MON_W, 875.F, 12.F, 1), 1.0), "1 window fitting screen returns 1.0");

    // 3. 2 windows fitting within 1800px: no stretch
    // Window 1: 12..875, Window 2: 889..1747 -> right=1747, left=12, total=1759 <= 1800
    ok(near(computeSpanRatio(MON_W, 1747.F, 12.F, 2), 1.0), "2 windows fitting screen returns 1.0");

    // 4. 3 windows extending past 1800px: stretches
    // Window 3: right=2625, left=12, total=2637 > 1800
    const float ratio3 = computeSpanRatio(MON_W, 2625.F, 12.F, 3);
    ok(near(ratio3, 2637.F / 1800.F), "3 windows extending past screen stretches by 2637/1800 (1.465x)");
    ok(ratio3 > 1.0F, "ratio3 is strictly greater than 1.0");

    // 5. 2 very wide windows (1200px each) extending past 1800px: stretches
    const float ratioWide = computeSpanRatio(MON_W, 2426.F, 12.F, 2);
    ok(ratioWide > 1.0F, "2 wide windows extending past screen also stretch");

    // 6. Camera scroll translation invariance
    // When tape is scrolled by offset 877px:
    // Window 1 rendered x = 12 - 877 = -865
    // Camera trans = -877
    // Untranslated = -865 - (-877) = 12
    const float rawGeomX = -865.F;
    const float camTransX = -877.F;
    const float untranslatedX = rawGeomX - camTransX;
    ok(near(untranslatedX, 12.F), "camera scroll translation accurately recovers static tape X");

    // 7. Strip cell layout with 4 workspaces (ws 2 stretched)
    std::vector<float> ratios = {1.0F, ratio3, 1.0F, 1.0F};
    const float GAP = 14.F;
    const float BAND_H = 120.F;
    auto l = layoutStrip(MON_W, BAND_H, GAP, ratios);

    ok(l.widths.size() == 4, "4 cell widths produced");
    ok(l.widths[0] == l.widths[2], "ws 1 and ws 3 have identical widths");
    ok(l.widths[1] > l.widths[0], "ws 2 is wider than ws 1");
    ok(near(l.widths[1] / l.widths[0], ratio3), "ws 2 width ratio matches ratio3 exactly");

    // Check gaps between adjacent cells
    for (size_t i = 0; i + 1 < l.xs.size(); ++i) {
        float cellRight = l.xs[i] + l.widths[i];
        float nextLeft = l.xs[i + 1];
        ok(near(nextLeft - cellRight, GAP), "gap between cell " + std::to_string(i) + " and " + std::to_string(i + 1) + " is exactly GAP");
    }

    // Centering on monitor
    float leftMargin = l.xs[0];
    float rightMargin = MON_W - (l.xs.back() + l.widths.back());
    ok(near(leftMargin, rightMargin, 1e-2), "entire strip is centered on monitor");

    // 8. Overflow handling: 10 workspaces each with 2.0x stretch
    std::vector<float> bigRatios(10, 2.0F);
    auto lOverflow = layoutStrip(MON_W, BAND_H, GAP, bigRatios);
    const float pad = BAND_H * 0.12F;
    const float maxW = MON_W - 2.F * pad;
    ok(near(lOverflow.totalWidth, maxW), "overflowing strip clamped to maxW");
    for (size_t i = 0; i + 1 < lOverflow.xs.size(); ++i) {
        float cellRight = lOverflow.xs[i] + lOverflow.widths[i];
        float nextLeft = lOverflow.xs[i + 1];
        ok(near(nextLeft - cellRight, GAP), "gap preserved even under overflow scaling");
    }

    // 9. Mini-window 1:1 aspect ratio inside stretched cell
    const float cellW = l.widths[1];
    const float cellH = l.height;
    const float virtualW = MON_W * ratio3;
    const float scaleX = cellW / virtualW;
    const float scaleY = cellH / MON_H;
    ok(near(scaleX, scaleY), "scaleX equals scaleY (no stretching of mini windows)");

    // Mini-window 3 fits completely within cell
    // Window 3: untranslated x=1761, w=864
    const float miniX = (1761.F) * scaleX;
    const float miniW = 864.F * scaleX;
    ok(miniX + miniW <= cellW, "mini-window 3 right edge fits inside cell width");

    std::printf("strip_scrolling_test: %d checks OK\n", g_run);
    return g_fail;
}
