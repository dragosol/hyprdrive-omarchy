// MotionSpacing: no two consecutive motion-blur copies more than MAX_GAP device
// px apart, on any edge, for any move or resize -- the rule that keeps the
// smear a smear.
#include "../MotionSpacing.hpp"

#include <cstdio>
#include <random>

using namespace ScrollOverview::MotionSpacing;

static int checks = 0, failed = 0;
static void expect(bool ok, const char* what) {
    ++checks;
    if (!ok) {
        ++failed;
        std::printf("FAIL %s\n", what);
    }
}

int main() {
    // The recording's case: a 600 px jump at 16 samples used to be 37 px gaps.
    {
        const SBox  PREV{100, 100, 800, 500}, CUR{700, 100, 800, 500};
        const auto  P   = plan(PREV, CUR, 1.6, 16);
        const double GAP = travel(P.previous, CUR, 1.6) / P.samples;
        expect(P.samples == MAX_SAMPLES && GAP <= MAX_GAP + 1e-9, "a 600px jump is never striped");
        expect(P.keep < 1.0 && P.keep > 0.0, "a jump too long for 64 samples gets a shorter trail");
    }
    // A short move keeps the configured count and the whole trail.
    {
        const auto P = plan(SBox{0, 0, 400, 300}, SBox{10, 0, 400, 300}, 1.0, 16);
        expect(P.samples == 16 && P.keep == 1.0, "a short move keeps its samples and its trail");
    }
    // A resize counts: one edge moving far needs the samples even with x,y still.
    {
        const SBox  PREV{0, 0, 400, 300}, CUR{0, 0, 900, 300};
        const auto  P = plan(PREV, CUR, 2.0, 8);
        expect(travel(P.previous, CUR, 2.0) / P.samples <= MAX_GAP + 1e-9, "a resize is spaced like a move");
    }
    // Still: no motion, the minimum, nothing shortened.
    {
        const auto P = plan(SBox{5, 5, 50, 50}, SBox{5, 5, 50, 50}, 1.5, 4);
        expect(P.samples == 4 && P.keep == 1.0, "no motion: the configured count");
    }
    // Every random move and resize keeps the gap -- the guarantee itself.
    {
        std::mt19937                           rng(14);
        std::uniform_real_distribution<double> POS(-3000, 3000), SIZE(1, 3000), SCALE(0.5, 3.0);
        std::uniform_int_distribution<int>     N(0, 40);
        bool                                   ok = true;
        for (int i = 0; i < 20000; ++i) {
            const SBox   PREV{POS(rng), POS(rng), SIZE(rng), SIZE(rng)}, CUR{POS(rng), POS(rng), SIZE(rng), SIZE(rng)};
            const double S = SCALE(rng);
            const auto   P = plan(PREV, CUR, S, N(rng));
            ok &= P.samples >= 2 && P.samples <= MAX_SAMPLES;
            ok &= travel(P.previous, CUR, S) / P.samples <= MAX_GAP + 1e-6;
            ok &= P.keep > 0.0 && P.keep <= 1.0;
        }
        expect(ok, "20000 random moves and resizes: never a gap over MAX_GAP");
    }

    std::printf("motion_spacing_test: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
