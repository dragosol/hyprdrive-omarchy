#pragma once

// The search view's two stages, LAYOUT and DRAW (SearchView.cpp), as the render
// step (OverviewSearchStep.cpp) calls them. Cut from OverviewSearchView.cpp by
// pure code motion.

#include "OverviewParts.hpp"

#include <string_view>
#include <vector>

// Everything layoutSearch() reads that is neither a result nor a section. The
// animated values are sampled ONCE here, at a named moment, instead of each
// consumer calling ->value() whenever it happens to run.
struct SSearchLayoutInputs {
    PHLMONITOR                     monitor;
    float                          renderScale = 1.F;
    Hyprdrive::Config::ELayout layout{};
    float                          fade = 0.F;  // the overview's open progress
    float                          dock = 0.F;  // X50: searchDock->value()
    float                          nudge = 0.F; // pillNudge->value(): rendered px the pill is lifted for a strip label
    float                          band = 0.F;  // X21/X23: resultsBand->value()
    float                          width = 0.F; // animated container width
    float                          colW[3] = {};
    float                          inspectorW = SEARCH_INSP_WL;
    // The published pill row is kept, not recomputed, when the pill culls: that
    // frame has no answer of its own and the grid must not be told it does.
    float                          rowLCarry = OVERVIEW_SEARCH_ROWHL;
    std::string_view               query;
    bool                           queried = false, emptyResult = false;
    int                            selection = -1;
    // The live field's width, logical px, as its spring has it THIS frame.
    float                          fieldWL = OVERVIEW_PILL_FIELDL;
};

SSearchLayout layoutSearch(const SSearchLayoutInputs& IN, const std::vector<SSearchRow>& RESULTS, const std::vector<SSearchSection>& SECTIONS);

// `part`: 0 everything, 1 the pill only, 2 the results body only.
void drawSearch(const SSearchLayout& L, bool pillLit, bool resultsLit, int part = 0);
