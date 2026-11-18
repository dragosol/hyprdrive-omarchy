// The search half of the overview's render step (SearchFrame.hpp). Cut out of
// COverview::renderSearch with the statement order kept verbatim.
#include "OverviewParts.hpp"
#include "SearchFrame.hpp"

using namespace OverviewDetail;

Search::SFrameRows Search::frameRows(CSession& S, PHLMONITOR monitor, MONITORID view, const std::function<size_t()>& matchingWindows,
                                     const std::function<size_t()>& gridWindows) {
    const bool QUERIED = overviewSearchPaneUp(S); // a query, a submenu, or the root browse (search:browse_on_click)

    const size_t MATCHES = !S.query.text.empty() ? matchingWindows() : 0;
    auto         RESULTS = QUERIED ? overviewSearchRows(S) : std::vector<SSearchRow>{};

    // An app hit is a hit: "no matches" must not contradict a listed result.
    const bool EMPTYRESULT = QUERIED && !S.query.text.empty() && MATCHES == 0 && RESULTS.empty();

    // How many previews the grid below is showing, which is what the row budget
    // is a ratio OF. Needed BEFORE the trim: the trim is what decides how many
    // rows exist, so a cap computed after it could never raise the ceiling.
    const size_t GRID_TILES = QUERIED && monitor->m_activeWorkspace ? gridWindows() : 0;

    auto SECTIONS = overviewSearchSections(RESULTS);
    overviewTrimSectionsToFit(S, SECTIONS, monitor, GRID_TILES);

    // Typing arms first-app selection; results land over the next frames, so
    // the spend lives here -- once, on the first frame that actually has an
    // Omarchy/apps row. Not Browsing: auto-select is for Enter, not SPACE.
    if (S.nav.selectFirstPending) {
        const int FIRST = overviewFirstAppIndex(RESULTS);
        if (FIRST >= 0) {
            S.nav.selection         = FIRST;
            S.nav.zone              = EOverviewNavZone::Result;
            S.nav.selectFirstPending = false;
            S.nav.explicitSelection = false;
        }
    }

    S.layoutOn(view).gridTiles = GRID_TILES;

    // Show Windows has no panel at all: no band to reserve between the pill and
    // the strip (user, 2026-09-24: "all this empty unused space").
    const bool  PANEL  = QUERIED && !S.query.windowsOnly;
    const bool TOOLPANE = !RESULTS.empty() && RESULTS[0].tool != 0;
    float      toolRowsHL = -1.F;
    if (PANEL && TOOLPANE) {
        const float SPAN = overviewResultsSpanW(monitor);
        toolRowsHL       = 0.F;
        for (const auto& ROW : RESULTS)
            toolRowsHL += overviewToolRowHL(ROW, SPAN) + OVERVIEW_TOOL_GAPL;
    }
    return {
        .queried     = QUERIED,
        .emptyResult = EMPTYRESULT,
        .gridTiles   = GRID_TILES,
        .results     = std::move(RESULTS),
        .sections    = std::move(SECTIONS),
        .panel       = PANEL,
        .toolPane    = TOOLPANE,
        .toolRowsHL  = toolRowsHL,
    };
}

float Search::bandTarget(Search::CSession& S, const SFrameRows& F, PHLMONITOR monitor) {
    return std::max(F.panel ? overviewResultsBandTarget(S, monitor, F.sections, F.gridTiles, F.toolPane, F.toolRowsHL) : 0.F, Hyprdrive::KeyboardCat::bandL());
}

float Search::widthTarget(const SFrameRows& F, PHLMONITOR monitor) {
    return F.panel ? overviewResultsWidthTarget(monitor, F.sections) : 0.F;
}

float Search::dockGoal(const SFrameRows& F) {
    return F.queried && !F.emptyResult && !F.results.empty() ? 1.F : 0.F;
}

bool Search::stepField(CSession& S, PHLMONITOR monitor, bool zooming) {
    auto&       FIELDMOTION = S.fieldOn(monitor->m_id);
    const float MAXFIELDL   = std::max(OVERVIEW_PILL_FIELDL, sc<float>(monitor->m_size.x) * OVERVIEW_PILL_MAXFRAC - OVERVIEW_PILL_WL(0.F));
    // Whole logical px: the measurement is a glyph run divided by a float scale,
    // and a target that wanders by a fraction of a pixel re-arms the spring --
    // which snaps within its epsilon -- every frame it steps.
    const float NEEDL       = FIELDMOTION.contentL > 0.F ? std::ceil(FIELDMOTION.contentL + OVERVIEW_PILL_ENDROOML) : 0.F;
    // While the overview zooms in or out around a pill at rest, the field
    // LANDS on its width: the glyphs are re-rasterised at each step of the
    // zoom, and a whole-px font size divided by a float scale wobbles a few
    // logical px -- an underdamped spring turned that into a visible bounce.
    const bool  FIELDLANDS  = zooming && !g_pillHovered && !g_pillPress.down && S.query.text.empty();
    return FIELDMOTION.stepTo(std::clamp(NEEDL, OVERVIEW_PILL_FIELDL, MAXFIELDL), std::max(0.F, NEEDL - MAXFIELDL), Time::steadyNow(), FIELDLANDS);
}

void Search::publish(CSession& S, MONITORID view, SSearchLayout&& LAYOUT, size_t GRID_TILES, float gridShiftL, bool QUERIED, const std::vector<SSearchRow>& RESULTS) {
    LAYOUT.stripPlateL = S.layoutOn(view).stripPlateL;
    LAYOUT.gridTiles   = GRID_TILES;
    LAYOUT.gridNudgeL  = S.layoutOn(view).gridNudgeL;
    LAYOUT.gridShiftL  = gridShiftL;

    S.nav.selection = LAYOUT.selection;
    S.layoutOn(view)     = std::move(LAYOUT);

    if (QUERIED && S.nav.selection >= 0 && S.nav.selection < sc<int>(RESULTS.size())) {
        if (RESULTS[S.nav.selection].omarchy && !RESULTS[S.nav.selection].omarchy->filePath.empty()) {
            SearchBackend::updateSushiIfActive(RESULTS[S.nav.selection].omarchy->filePath);
        }
    }
}
