// PlateColor + the label fade's first sample: a ringed plate (the dock
// tooltip, every label pill, the tile's close button) fades as ONE surface --
// its body never shows more of the ring's accent than it settles with -- and
// the tooltip's STween / a list's SLitSet start a leg at its `from`, so a new
// plate's first frame is alpha 0, not 1.
#include "PlateColor.hpp"
#include "MotionTween.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace Hyprdrive::PlateColor;
using Hyprdrive::Motion::SLitSet;
using Hyprdrive::Motion::STween;

static int  checks = 0, failed = 0;
static void expect(bool ok, const char* what) {
    ++checks;
    if (!ok) {
        ++failed;
        std::printf("FAIL %s\n", what);
    }
}
static bool near(const SRGBA& a, const SRGBA& b, float eps = 1e-5F) {
    return std::fabs(a.r - b.r) < eps && std::fabs(a.g - b.g) < eps && std::fabs(a.b - b.b) < eps;
}
// How much of the accent's chroma (R - (G+B)/2 for a red/orange accent) a colour holds.
static float chroma(const SRGBA& c) {
    return c.r - (c.g + c.b) / 2.F;
}

int main() {
    // The label pill: 90% black over the ring; an orange (Omarchy) and a red accent.
    const SRGBA FILL{0.F, 0.F, 0.F, 0.90F};
    const SRGBA ACCENTS[] = {{1.F, 0.125F, 0.F, 1.F}, {1.F, 0.55F, 0.1F, 1.F}};
    const SRGBA BACKDROPS[] = {{0.106F, 0.129F, 0.114F, 1.F}, {1.F, 1.F, 1.F, 1.F}, {0.F, 0.F, 0.F, 1.F}, {0.2F, 0.4F, 0.9F, 1.F}};

    for (const auto& RING : ACCENTS) {
        const SRGBA SETTLED = settledBody(FILL, RING);
        expect(near(SETTLED, {RING.r * 0.1F, RING.g * 0.1F, RING.b * 0.1F, 1.F}), "settled body = 10% of the ring under 90% black");
        for (const auto& BG : BACKDROPS) {
            // The ends: alpha 0 is the backdrop, alpha 1 the very pixels the
            // old drawing settled at (the fix leaves the settled plate alone).
            expect(near(bodyAt(FILL, RING, BG, 0.F), BG), "alpha 0: the body is the backdrop");
            expect(near(bodyAt(FILL, RING, BG, 1.F), bodyAtRingUnder(FILL, RING, BG, 1.F)), "alpha 1: the same body as before the fix");
            expect(near(bodyAt(FILL, RING, BG, 1.F), SETTLED), "alpha 1: the settled body");

            // Every alpha between: a plain opacity fade, so the accent in the
            // body never exceeds both ends. Before the fix it peaked at alpha
            // 5/9 with ~2.8x the settled accent (the flash).
            float worstNew = -1e9F, worstOld = -1e9F;
            for (int i = 0; i <= 100; ++i) {
                const float A    = static_cast<float>(i) / 100.F;
                const SRGBA NEW  = bodyAt(FILL, RING, BG, A);
                const SRGBA LERP = {BG.r + (SETTLED.r - BG.r) * A, BG.g + (SETTLED.g - BG.g) * A, BG.b + (SETTLED.b - BG.b) * A, 1.F};
                expect(near(NEW, LERP), "the body is backdrop -> settled, linearly in alpha");
                const float ENDS = std::max(chroma(BG), chroma(SETTLED));
                worstNew         = std::max(worstNew, chroma(NEW) - ENDS);
                worstOld         = std::max(worstOld, chroma(bodyAtRingUnder(FILL, RING, BG, A)) - ENDS);
            }
            expect(worstNew <= 1e-5F, "fixed: the body never holds more accent than at its ends");
            // The control: the old drawing does flash on a dark neutral
            // backdrop (the rig's kitty, black).
            if (BG.r < 0.5F && std::fabs(chroma(BG)) < 0.05F)
                expect(worstOld > 0.1F, "control: the ring-under drawing shows the accent through the body mid-fade");
        }
    }

    // The fade's first sample is its `from`: a new tooltip leg (from 0) draws
    // nothing on its first frame, and a turn starts where it was drawn.
    {
        STween<int> tip{.from = 0.F, .to = 1.F, .start = 0};
        expect(tip.at(0.F) == 0.F && tip.at(1.F) == 1.F, "STween: e=0 is from, e=1 is to");
        tip.retarget(0.37F, 0.F, 5);
        expect(tip.at(0.F) == 0.37F && !tip.rising(), "STween: a turn starts at the value as drawn");
    }
    {
        SLitSet<int> rows;
        float        e     = 0.F;
        const auto   EASED = [&](const STween<int>&) { return e; };
        expect(rows.level(7, EASED) == 0.F, "SLitSet: a key never lit reads rest");
        rows.moveTo(7, 0, EASED);
        expect(rows.level(7, EASED) == 0.F, "SLitSet: a newly lit key's first sample is rest, not on");
        e = 1.F;
        expect(rows.level(7, EASED) == 1.F, "SLitSet: ... and it arrives at on");
        rows.moveTo(8, 1, EASED);   // the old key turns from 1 as drawn
        e = 0.F;                    // the new leg's first sample
        expect(rows.level(7, EASED) == 1.F && rows.level(8, EASED) == 0.F, "SLitSet: a move starts both keys where they were");
    }

    std::printf("plate_color_test: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
