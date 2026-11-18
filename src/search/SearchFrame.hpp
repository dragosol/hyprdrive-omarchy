#pragma once

// The search's half of the overview's search render step (COverview::renderSearch,
// OverviewSearchStep.cpp): the rows and sections of this frame and the targets
// the overview aims its five animated values at (ANIMATE), the live field's
// springs, and the PUBLISH of the frame's layout. The overview keeps the
// PHLANIMVARs (they carry its animation config), applies the targets in the
// order and with the thresholds it always did, samples them for LAYOUT, and
// draws. Nothing here reorders the frame: each function runs where its lines
// ran.

#include "SearchSession.hpp"

#include <cstddef>
#include <functional>
#include <vector>

namespace OvInternal {
    struct SSearchRow;
    struct SSearchSection;
}

namespace Search {
    // ANIMATE's search half, as one frame sees it.
    struct SFrameRows {
        bool                                    queried = false, emptyResult = false;
        size_t                                  gridTiles = 0;
        std::vector<OvInternal::SSearchRow>     results;
        std::vector<OvInternal::SSearchSection> sections;
        bool                                    panel = false, toolPane = false;
        float                                   toolRowsHL = -1.F;
    };

    // The pane test, the rows, the window count ("no matches"), the grid's
    // preview count, the sections trimmed to fit (which scrolls nav.colTop),
    // the first-app spend, and the gridTiles carry into `view`'s layout -- in
    // that order. `matchingWindows`/`gridWindows` are asked exactly where the
    // render step asked them (only with something typed / with the pane up and
    // a desktop on the monitor).
    SFrameRows frameRows(CSession& S, PHLMONITOR monitor, MONITORID view, const std::function<size_t()>& matchingWindows,
                         const std::function<size_t()>& gridWindows);

    // The band's target (the easter egg's frame takes the same band), the
    // container width's, and the pill's dock goal.
    float bandTarget(Search::CSession& S, const SFrameRows& F, PHLMONITOR monitor);
    float widthTarget(const SFrameRows& F, PHLMONITOR monitor);
    float dockGoal(const SFrameRows& F);

    // The live field's springs for this frame; `zooming`: the overview's
    // scale is animating. True while they move (the overview asks for frames).
    bool stepField(CSession& S, PHLMONITOR monitor, bool zooming);

    // PUBLISH: the carries, the selection clamp written back to the shared
    // nav, the one assignment the rest of the overview can see, and Quick
    // Look following the selection.
    void publish(CSession& S, MONITORID view, SSearchLayout&& LAYOUT, size_t gridTiles, float gridShiftL, bool queried, const std::vector<OvInternal::SSearchRow>& RESULTS);
}
