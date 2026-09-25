// The stack PANE's geometry (dock stacks A): where the plate, header, search
// field and grid sit for a given item count. Pure layout -- DockStacks.cpp fills
// the cells and draws.
#include "DockStacks.hpp"

#include <hyprland/src/output/Monitor.hpp>

#include <algorithm>
#include <cmath>

namespace ScrollOverview::Stacks::Pane {

    SPaneLayout layout(PHLMONITOR monitor, const CBox& anchor, const CBox& pill, size_t items, float progress, bool leaving) {
        SPaneLayout out;
        const float S     = std::max(monitor->m_scale, 0.01F);
        const float MONW  = sc<float>(monitor->m_size.x) * S;
        const float MONH  = sc<float>(monitor->m_size.y) * S;
        const float CELLW = CELL_W * S, CELLH = CELL_H * S, SP = SPACING * S, P = PAD * S;

        // Spec: <= 6 columns and <= 60% of the monitor's width; roughly as wide as
        // it is tall for a small stack, so eight files are not one long row.
        const int MAXCOLS = std::max(1, std::min(6, sc<int>((MONW * 0.60F - 2.F * P + SP) / (CELLW + SP))));
        out.cols          = std::clamp(sc<int>(std::ceil(std::sqrt(sc<float>(std::max<size_t>(items, 1)) * 1.6F))), 1, MAXCOLS);
        const int ROWS    = std::max(1, sc<int>((items + out.cols - 1) / out.cols));

        const float W      = 2.F * P + out.cols * CELLW + (out.cols - 1) * SP;
        const float CHROME = 2.F * P + (HEADER_H + FIELD_H + 2.F * GAP) * S;
        const float MAXH   = MONH * 0.55F; // spec: <= 55% of the monitor's height
        const float GRIDH  = std::min(ROWS * CELLH + (ROWS - 1) * SP, std::max(CELLH, MAXH - CHROME));
        const float H      = CHROME + GRIDH;

        // Grows out of the stack's cell, like the popup always has.
        const float E     = leaving ? 1.F - houseEase(false, progress) : houseEase(true, progress);
        const float CX    = sc<float>(anchor.x) + sc<float>(anchor.width) / 2.F;
        const float RESTY = sc<float>(pill.y) - GAP * S - H;
        CBox        plate{sc<float>(anchor.x) + (CX - W / 2.F - sc<float>(anchor.x)) * E, sc<float>(anchor.y) + (RESTY - sc<float>(anchor.y)) * E,
                   sc<float>(anchor.width) + (W - sc<float>(anchor.width)) * E, sc<float>(anchor.height) + (H - sc<float>(anchor.height)) * E};
        plate.x   = std::clamp(plate.x, 0.0, std::max(0.0, sc<double>(MONW) - plate.width));
        out.plate = plate.round();

        out.header = CBox{plate.x + P, plate.y + P, plate.width - 2 * P, HEADER_H * S}.round();
        out.field  = CBox{plate.x + P, out.header.y + out.header.height + GAP * S, plate.width - 2 * P, FIELD_H * S}.round();
        const double GY = out.field.y + out.field.height + GAP * S;
        out.grid        = CBox{plate.x + P, GY, plate.width - 2 * P, std::max(0.0, plate.y + plate.height - P - GY)}.round();
        out.cellW       = CELLW;
        out.cellH       = CELLH;
        return out;
    }
}
