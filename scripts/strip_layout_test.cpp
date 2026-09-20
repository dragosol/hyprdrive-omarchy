// Host test for StripLayout::layout -- the desktop strip row with the detached
// scratchpad cell and the other monitors' strips. Needs no compositor.
//
// The reference below is the strip-only formula scrollOverview.cpp shipped
// before the other-monitor strips existed, copied verbatim in shape: with no
// alternate strips the new layout must reproduce it exactly.

#include "../StripLayout.hpp"

#include <cmath>
#include <cstdio>
#include <string>

using namespace ScrollOverview::StripLayout;

static int g_fail = 0, g_run = 0;

static void ok(bool cond, const std::string& what) {
    ++g_run;
    if (!cond) {
        ++g_fail;
        std::printf("FAIL  %s\n", what.c_str());
    }
}

static bool near(double a, double b, double eps = 1e-6) {
    return std::abs(a - b) <= eps;
}

static bool sameBox(const SBox& a, const SBox& b) {
    if (a.empty() && b.empty())
        return true;
    return near(a.x, b.x) && near(a.y, b.y) && near(a.w, b.w) && near(a.h, b.h);
}

// The pre-2026-09-14 stripSlotLogicalBox, one slot at a time.
static SBox reference(const SInput& in, size_t idx, bool scratchSlot) {
    if (in.ratios.empty() || in.band <= 0)
        return {};
    const double REVEAL = std::clamp(in.scratchReveal, 0.0, 1.0);
    if (scratchSlot && REVEAL <= 0.001)
        return {};
    const double PAD   = in.band * in.padFrac;
    const size_t COUNT = in.ratios.size();
    double       cellH = in.band - 2 * PAD;
    double       baseW = cellH * in.aspect;
    std::vector<double> w(COUNT);
    double       sum = 0;
    for (size_t i = 0; i < COUNT; ++i) {
        w[i] = baseW * in.ratios[i];
        sum += w[i];
    }
    const double GAPS   = in.gap * double(COUNT - 1);
    const double DETACH = REVEAL * (2 * in.panelPad + in.gap * in.detach);
    if (const double MAXW = in.monW - 2 * PAD; sum + GAPS + DETACH + REVEAL * baseW > MAXW && sum + REVEAL * baseW > 0) {
        const double S = std::max(0.0, (MAXW - GAPS - DETACH) / (sum + REVEAL * baseW));
        cellH *= S;
        baseW *= S;
        for (auto& x : w)
            x *= S;
        sum *= S;
    }
    if (cellH <= 1)
        return {};
    const double SCRATCHW = REVEAL * baseW;
    const double TOTAL    = sum + GAPS + DETACH + SCRATCHW;
    const double Y        = in.monY + in.top + PAD + (in.band - 2 * PAD - cellH) / 2;
    double       x        = in.monX + (in.monW - TOTAL) / 2;
    if (scratchSlot)
        return SCRATCHW <= 1 ? SBox{} : SBox{x + sum + GAPS + DETACH, Y, SCRATCHW, cellH};
    if (w[idx] <= 1)
        return {};
    for (size_t i = 0; i < idx; ++i)
        x += w[i] + in.gap;
    return SBox{x, Y, w[idx], cellH};
}

static SBox plate(const std::vector<SBox>& cells, double pad) {
    double x0 = 1e18, x1 = -1e18;
    SBox   any;
    for (const auto& C : cells) {
        if (C.empty())
            continue;
        x0  = std::min(x0, C.x);
        x1  = std::max(x1, C.right());
        any = C;
    }
    if (any.empty())
        return {};
    return SBox{x0 - pad, any.y - pad, x1 - x0 + 2 * pad, any.h + 2 * pad};
}

static SInput base() {
    SInput in;
    in.monX = 1440, in.monY = 0, in.monW = 1440, in.aspect = 1.6;
    in.top = 60, in.band = 150, in.padFrac = 0.08, in.gap = 5, in.panelPad = 10, in.detach = 2;
    in.ratios = {1, 1, 1, 1};
    return in;
}

int main() {
    // 1. No other monitors: identical to the old formula, every reveal, many counts.
    for (size_t count = 1; count <= 24; ++count) {
        for (double reveal : {0.0, 0.0005, 0.3, 0.5, 1.0}) {
            for (double monW : {800.0, 1440.0, 2752.0}) {
                auto in          = base();
                in.monW          = monW;
                in.ratios        = std::vector<double>(count, 1.0);
                if (count > 3)
                    in.ratios[2] = 1.7; // a scrolling desktop wider than the screen
                in.scratchReveal = reveal;
                const auto OUT   = layout(in);
                for (size_t i = 0; i < count; ++i)
                    ok(sameBox(OUT.cells[i], reference(in, i, false)),
                       "parity: cell " + std::to_string(i) + "/" + std::to_string(count) + " reveal " + std::to_string(reveal) + " monW " + std::to_string(monW));
                ok(sameBox(OUT.scratch, reference(in, 0, true)), "parity: scratch cell, count " + std::to_string(count) + " reveal " + std::to_string(reveal));
            }
        }
    }

    // 2. Alternate strips present but hidden: still identical.
    {
        auto in = base();
        in.alts = {{3, 2.39}};
        const auto OUT = layout(in);
        for (size_t i = 0; i < in.ratios.size(); ++i)
            ok(sameBox(OUT.cells[i], reference(in, i, false)), "hidden alts: cell " + std::to_string(i) + " does not move");
        for (const auto& C : OUT.altCells[0])
            ok(C.empty(), "hidden alts: no alternate cell is drawn");
    }

    // 3. Fully revealed, every configuration: nothing overlaps, everything fits,
    //    plates keep their visible gap, order is alts -> strip -> scratchpad.
    for (size_t count = 1; count <= 12; ++count) {
        for (size_t alts = 1; alts <= 3; ++alts) {
            for (double scratch : {0.0, 1.0}) {
                for (double monW : {1440.0, 2752.0, 900.0}) {
                    auto in          = base();
                    in.monW          = monW;
                    in.ratios        = std::vector<double>(count, 1.0);
                    in.scratchReveal = scratch;
                    in.altReveal     = 1.0;
                    for (size_t k = 0; k < alts; ++k)
                        in.alts.push_back({1 + (k * 3 + count) % 6, k == 1 ? 0.625 : 2.39});
                    const auto  OUT  = layout(in);
                    std::string name = "count " + std::to_string(count) + " alts " + std::to_string(alts) + " scratch " + std::to_string(scratch) + " monW " +
                        std::to_string(monW);
                    ok(OUT.valid, name + ": valid");
                    if (!OUT.valid)
                        continue;

                    std::vector<SBox> plates;
                    for (const auto& A : OUT.altCells)
                        plates.push_back(plate(A, in.panelPad));
                    plates.push_back(plate(OUT.cells, in.panelPad));
                    if (!OUT.scratch.empty())
                        plates.push_back(plate({OUT.scratch}, in.panelPad));

                    const double PAD = in.band * in.padFrac;
                    for (size_t p = 0; p < plates.size(); ++p) {
                        ok(!plates[p].empty(), name + ": plate " + std::to_string(p) + " drawn");
                        if (p + 1 < plates.size()) {
                            const double VISIBLE = plates[p + 1].x - plates[p].right();
                            ok(near(VISIBLE, in.gap * in.detach, 1e-6), name + ": visible gap between plates " + std::to_string(p) + " and " + std::to_string(p + 1) +
                                   " is " + std::to_string(VISIBLE));
                        }
                    }
                    const double LEFT  = plates.front().x + in.panelPad;
                    const double RIGHT = plates.back().right() - in.panelPad;
                    ok(LEFT >= in.monX + PAD - 1e-6 && RIGHT <= in.monX + in.monW - PAD + 1e-6, name + ": the whole row fits the monitor");
                    ok(near((LEFT - in.monX) - PAD, (in.monX + in.monW - RIGHT) - PAD, 1e-6) || LEFT - in.monX > PAD, name + ": centred");
                    ok(near(LEFT - in.monX, in.monX + in.monW - RIGHT, 1e-6), name + ": centred on the monitor");

                    // Within one strip: cells in order, one gap apart, same row.
                    auto rowOk = [&](const std::vector<SBox>& cells, const std::string& what) {
                        for (size_t j = 0; j + 1 < cells.size(); ++j)
                            ok(near(cells[j + 1].x - cells[j].right(), in.gap, 1e-6) && near(cells[j].y, cells[j + 1].y), name + ": " + what + " cells " +
                                   std::to_string(j) + "," + std::to_string(j + 1) + " one gap apart");
                    };
                    rowOk(OUT.cells, "strip");
                    for (size_t k = 0; k < OUT.altCells.size(); ++k) {
                        rowOk(OUT.altCells[k], "alt " + std::to_string(k));
                        for (const auto& C : OUT.altCells[k])
                            ok(near(C.w / C.h, in.alts[k].aspect, 1e-6), name + ": alt cells keep their monitor's shape");
                    }
                }
            }
        }
    }

    // 4. The reveal is monotone: as the alternate strips come out the main strip
    //    only ever slides right and never grows, alternate plates never overlap
    //    each other, and once drawable they never overlap the strip's plate --
    //    which they are from early in the reveal on.
    for (double monW : {1440.0, 2752.0, 900.0}) {
        auto in  = base();
        in.monW  = monW;
        in.alts  = {{4, 2.39}, {2, 1.6}, {1, 0.625}};
        double lastX = -1e18, lastW = 1e18;
        for (int step = 0; step <= 200; ++step) {
            in.altReveal   = step / 200.0;
            const auto  OUT  = layout(in);
            std::string name = "reveal monW " + std::to_string(monW) + " step " + std::to_string(step);
            ok(OUT.cells[0].x >= lastX - 1e-9, name + ": strip slides right monotonically");
            ok(OUT.cells[0].w <= lastW + 1e-9, name + ": cells never grow while revealing");
            lastX = OUT.cells[0].x;
            lastW = OUT.cells[0].w;

            std::vector<SBox> plates;
            for (const auto& A : OUT.altCells)
                if (const auto P = plate(A, OUT.altPlatePad); !P.empty())
                    plates.push_back(P);
            for (size_t p = 0; p + 1 < plates.size(); ++p)
                ok(plates[p].right() <= plates[p + 1].x + 1e-9, name + ": alternate plates never overlap each other");

            const auto MAIN = plate(OUT.cells, in.panelPad);
            if (OUT.altDrawable && !plates.empty())
                ok(plates.back().right() <= MAIN.x + 1e-9, name + ": a drawable alternate plate is clear of the strip's plate");
            if (in.altReveal >= 0.15)
                ok(OUT.altDrawable, name + ": drawable from 15% of the reveal on");
        }
    }

    // 5. Degenerate inputs never produce garbage.
    {
        auto in   = base();
        in.ratios = {};
        ok(!layout(in).valid, "no desktops: invalid");
        in        = base();
        in.band   = 0;
        ok(!layout(in).valid, "no band: invalid");
        in           = base();
        in.alts      = {{0, 1.6}};
        in.altReveal = 1;
        const auto OUT = layout(in);
        for (size_t i = 0; i < in.ratios.size(); ++i)
            ok(sameBox(OUT.cells[i], reference(in, i, false)), "an alternate monitor with no desktops takes no room");
        in           = base();
        in.monW      = 40;
        in.alts      = {{30, 2.39}};
        in.altReveal = 1;
        ok(!layout(in).valid || layout(in).cellH > 1, "a monitor too narrow for anything is invalid, not negative");
    }

    std::printf("strip_layout_test: %d checks, %d failed\n", g_run, g_fail);
    return g_fail ? 1 : 0;
}
