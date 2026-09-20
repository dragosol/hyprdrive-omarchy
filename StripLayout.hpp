#pragma once

// THE DESKTOP STRIP'S ROW, as a pure function: the numbered cells, the detached
// scratchpad cell on the right, and -- user, 2026-09-14 -- one detached strip per
// OTHER monitor on the left, each showing that monitor's desktops.
//
// All three share one row and one budget, which is why they are one function:
// every detached strip grows out of the row and the row re-centres around the
// lot, and when the monitor is too narrow for everything the CELLS shrink
// together while the gaps between them keep their size.
//
// With both reveals at 0 this is exactly the strip-only layout scrollOverview.cpp
// used before it (scripts/strip_layout_test.cpp holds the old formula and
// compares against it), so a strip on one monitor cannot move by a pixel.

#include <algorithm>
#include <cstddef>
#include <vector>

namespace ScrollOverview::StripLayout {

struct SBox {
    double x = 0, y = 0, w = 0, h = 0;

    bool   empty() const {
        return w <= 0 || h <= 0;
    }
    double right() const {
        return x + w;
    }
};

// Another monitor's strip: how many desktops it has and the shape of its screen.
struct SAlt {
    size_t cells  = 0;
    double aspect = 1.6; // width / height of THAT monitor
};

struct SInput {
    // This monitor, logical px.
    double monX = 0, monY = 0, monW = 0;
    double aspect = 1.6;

    double top      = 0;    // search row + results band above the strip
    double band     = 0;    // strip band height
    double padFrac  = 0;    // OVERVIEW_STRIP_BAND_PAD_FRAC
    double gap      = 0;    // strip gap between cells
    double panelPad = 10;   // each plate's padding around its cells
    double detach   = 2;    // visible gaps between two plates, in strip gaps

    std::vector<double> ratios; // one per numbered cell: its scrolling span ratio (1 = plain)

    double            scratchReveal = 0; // 0..1
    std::vector<SAlt> alts;              // left to right, as they are drawn
    double            altReveal = 0;     // 0..1, one reveal for all of them
};

struct SOutput {
    bool                           valid = false;
    double                         cellH = 0;
    std::vector<SBox>              cells;    // empty box where a cell is too thin to draw
    SBox                           scratch;  // empty when not revealed
    std::vector<std::vector<SBox>> altCells; // [alt][desktop], empty boxes while hidden
    double                         altPlatePad = 0;    // the padding the alternate plates are drawn with
    bool                           altDrawable = false; // the alternate plates clear the strip's own plate
};

// How much of the reveal it takes for the room between the other monitors'
// strips and this one to reach two full plate paddings. See the comment where
// it is used.
inline constexpr double ALT_PAD_RAMP = 0.25;

inline SOutput layout(const SInput& in) {
    SOutput out;
    out.cells.assign(in.ratios.size(), SBox{});
    out.altCells.resize(in.alts.size());
    for (size_t k = 0; k < in.alts.size(); ++k)
        out.altCells[k].assign(in.alts[k].cells, SBox{});

    if (in.band <= 0 || in.ratios.empty())
        return out;

    const double R      = std::clamp(in.scratchReveal, 0.0, 1.0);
    const double A      = std::clamp(in.altReveal, 0.0, 1.0);
    const double PAD    = in.band * in.padFrac;
    const double DETACH = 2.0 * in.panelPad + in.gap * in.detach; // both plates' padding + the visible gap
    const size_t COUNT  = in.ratios.size();

    // The room left of this strip grows FASTER than the alternate strips do. At
    // a plain `A * DETACH` the strip's own plate -- always fully padded -- sat
    // over the growing strips for the first half of the reveal. The padding part
    // reaches its full size a quarter of the way out; the visible gap still
    // grows with the reveal, so the strip slides continuously from where it was.
    const double ALTDETACH = 2.0 * in.panelPad * std::min(1.0, A / ALT_PAD_RAMP) + A * in.gap * in.detach;

    const double CELLH0 = in.band - 2.0 * PAD;
    const double BASEW0 = CELLH0 * in.aspect;

    // What scales with the cells, and what does not.
    double scaled = R * BASEW0;
    for (const double RATIO : in.ratios)
        scaled += BASEW0 * RATIO;
    double fixed = in.gap * double(COUNT - 1) + R * DETACH;
    for (const auto& ALT : in.alts) {
        if (ALT.cells == 0)
            continue;
        scaled += A * double(ALT.cells) * CELLH0 * ALT.aspect;
        fixed += A * in.gap * double(ALT.cells - 1) + ALTDETACH;
    }

    double s = 1.0;
    if (const double MAXW = in.monW - 2.0 * PAD; scaled + fixed > MAXW && scaled > 0.0)
        s = std::max(0.0, (MAXW - fixed) / scaled);

    const double CELLH = CELLH0 * s;
    const double BASEW = BASEW0 * s;
    if (CELLH <= 1.0)
        return out;

    out.valid = true;
    out.cellH = CELLH;

    std::vector<double> widths(COUNT);
    double              sumW = 0;
    for (size_t i = 0; i < COUNT; ++i) {
        widths[i] = BASEW * in.ratios[i];
        sumW += widths[i];
    }

    double leftW = 0;
    for (const auto& ALT : in.alts) {
        if (ALT.cells == 0)
            continue;
        leftW += A * (double(ALT.cells) * CELLH * ALT.aspect + in.gap * double(ALT.cells - 1)) + ALTDETACH;
    }

    const double SCRATCHW = R * BASEW;
    const double TOTAL    = leftW + sumW + in.gap * double(COUNT - 1) + R * DETACH + SCRATCHW;
    const double Y        = in.monY + in.top + PAD + (in.band - 2.0 * PAD - CELLH) / 2.0;
    const double X0       = in.monX + (in.monW - TOTAL) / 2.0;

    // The other monitors' strips GROW out of the row's left end, cells and gaps
    // alike, so the reveal and the re-centring are one motion -- the scratchpad
    // cell's trick, mirrored. Their plates pad by altReveal * panelPad, so two of
    // them never overlap each other at any reveal.
    double x = X0;
    for (size_t k = 0; k < in.alts.size(); ++k) {
        const auto& ALT = in.alts[k];
        if (ALT.cells == 0)
            continue;
        const double CW = A * CELLH * ALT.aspect;
        for (size_t j = 0; j < ALT.cells; ++j) {
            if (CW > 1.0)
                out.altCells[k][j] = SBox{x, Y, CW, CELLH};
            x += CW + (j + 1 < ALT.cells ? A * in.gap : 0.0);
        }
        x += ALTDETACH;
    }

    out.altPlatePad = A * in.panelPad;

    x = X0 + leftW;
    for (size_t i = 0; i < COUNT; ++i) {
        if (widths[i] > 1.0)
            out.cells[i] = SBox{x, Y, widths[i], CELLH};
        x += widths[i] + in.gap;
    }

    // Drawable once the last alternate plate is clear of this strip's plate --
    // from about a seventh of the way out. Before that they are a sliver at a
    // seventh of their opacity, and not drawing them is invisible.
    {
        double lastRight = -1e18;
        for (const auto& CELLS : out.altCells)
            for (const auto& C : CELLS)
                if (!C.empty())
                    lastRight = std::max(lastRight, C.right());
        const auto FIRST = std::find_if(out.cells.begin(), out.cells.end(), [](const SBox& b) { return !b.empty(); });
        out.altDrawable  = lastRight > -1e17 && FIRST != out.cells.end() && lastRight + out.altPlatePad <= FIRST->x - in.panelPad + 1e-9;
    }

    if (SCRATCHW > 1.0)
        out.scratch = SBox{X0 + leftW + sumW + in.gap * double(COUNT - 1) + R * DETACH, Y, SCRATCHW, CELLH};

    return out;
}

} // namespace ScrollOverview::StripLayout
