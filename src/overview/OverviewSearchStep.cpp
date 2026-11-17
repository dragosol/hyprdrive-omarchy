// COverview: the search render step (renderSearch: ANIMATE -> LAYOUT -> PUBLISH ->
// DRAW) and the search's key taps (Space, Shift x2, Ctrl x2, menu back).
// Cut from OverviewSearchView.cpp by pure code motion; the layout, the draw and
// the debug report it calls are in search/SearchView.cpp.
#include "OverviewParts.hpp"
#include "Rows.hpp"
#include "MotionCurves.hpp"
#include "SearchView.hpp"

using namespace OverviewDetail;

// How many open windows the query reaches. Counted across every workspace, not
// just the focused one: the strip shows matching minis from elsewhere, so "no
// matches" has to agree with that.
size_t COverview::matchingWindowCount() const {
    if (g_search.query.text.empty())
        return 0;

    // N walks of the whole window list, once per desktop, every frame on every
    // output -- and the answer moves only when the query does or when a window
    // joins, leaves, is minimized or is retitled, all of which bump the
    // membership generation this keys on.
    const uint64_t MEMBERS = Membership::generation();
    if (matchCountFilled && matchCountQuery == g_search.query.text && matchCountMembers == MEMBERS && matchCountImages == images.size())
        return matchCountValue;

    size_t matches = 0;
    for (const auto& wimg : images) {
        if (wimg && wimg->pWorkspace)
            matches += overviewGridWindows(wimg->pWorkspace).size();
    }

    matchCountQuery   = g_search.query.text;
    matchCountMembers = MEMBERS;
    matchCountImages  = images.size();
    matchCountValue   = matches;
    matchCountFilled  = true;

    return matches;
}

// ---------------------------------------------------------------------------
// The search render step, in four named stages: ANIMATE -> LAYOUT -> PUBLISH ->
// DRAW.
//
// This was one 450-line function that computed a box, drew it, wrote a global,
// computed the next box out of that global, and drew that. The stages are not
// decoration. The two heights the GRID and the STRIP lay themselves out against
// are produced HERE, and they are read by code that ran EARLIER in the same
// frame -- the only thing that ever made that work was the order the statements
// happened to be written in. It is a signature now: layoutSearch() is handed
// everything it reads and returns everything it produces, and exactly one
// assignment makes any of it visible to anybody else.
//
// The frame order itself is preserved verbatim, because it is load-bearing:
//   * the band target and X64's section trim are computed against the PREVIOUS
//     frame's pill row -- they run before the pill is laid out, as they always
//     have;
//   * the results panel is placed against THIS frame's pill row, which is why
//     that row is a parameter and not a read-back of the published value;
//   * the publish lands where the old scattered writes landed, so the grid and
//     the strip -- which render before this step -- go on reading the frame
//     before's values. Publishing earlier would slide the grid's climb one
//     frame ahead of the pill it is following, which is a pixel change to X50.

// OVERVIEW_CPP_SPEC §5 F1 + §13.4 X2. The search bar: a field carrying an
// accent badge with a Nerd Font magnifying glass, a placeholder while empty,
// and a fixed field width that widens once a query is typed. Rounding, border,
// fill and frost come from the Hyprland theme (decoration:rounding, blur,
// general:border_size, col.inactive_border) rather than a forced capsule.
// The reference's metrics are the OVERVIEW_PILL_* family.
// ponytail: the width change is a hard cut; the reference eases it over 300ms.
// The reference's two trailing icon buttons (Google Lens, SongRec) are not
// drawn -- neither action exists in this overview, so they would be decoration
// for nothing.
MONITORID COverview::searchMonitorId() const {
    const auto MONITOR = pMonitor.lock();
    return MONITOR ? MONITOR->m_id : MONITOR_INVALID;
}

SSearchLayout& COverview::searchLayoutHere() const {
    return g_search.layoutOn(searchMonitorId());
}

void COverview::renderSearch(PHLMONITOR monitor, float renderScale) {
    Prof::Timer PROF{Prof::srchNs, Prof::srchCalls};
    if (!monitor)
        return;

    // G12. Cleared here rather than where the rows are drawn: every early
    // return below is a frame with no rows on it, and a stale box would hand
    // the next press a result that is no longer on screen.
    searchLayoutHere().hits.clear();

    // User, 2026-08-06: the pill fades in with the overview, and reverses if
    // the overview is toggled off mid-flight. See renderStrip for why the open
    // scale is the driver rather than a fade var of its own.
    const float FADE = overviewGridProgress(renderScale);
    if (FADE < 0.01F)
        return;

    // ---- ANIMATE: re-aim the search animations from this frame's results.
    // Results exist once there is query text, or while browsing an Omarchy
    // submenu after Enter (query is cleared so children are listed).
    const bool QUERIED = overviewSearchPaneUp(); // a query, a submenu, or the root browse (search:browse_on_click)

    const size_t MATCHES = !g_search.query.text.empty() ? matchingWindowCount() : 0;
    const auto   RESULTS = QUERIED ? overviewSearchRows() : std::vector<SSearchRow>{};

    // An app hit is a hit: "no matches" must not contradict a listed result.
    const bool EMPTYRESULT = QUERIED && !g_search.query.text.empty() && MATCHES == 0 && RESULTS.empty();

    // How many previews the grid below is showing, which is what the row budget
    // is a ratio OF. Needed BEFORE the trim: the trim is what decides how many
    // rows exist, so a cap computed after it could never raise the ceiling.
    const size_t GRID_TILES = QUERIED && monitor->m_activeWorkspace ? overviewMainGridWindows(monitor->m_activeWorkspace, monitor).size() : 0;

    auto SECTIONS = overviewSearchSections(RESULTS);
    overviewTrimSectionsToFit(SECTIONS, monitor, GRID_TILES);

    // Typing arms first-app selection; results land over the next frames, so
    // the spend lives here -- once, on the first frame that actually has an
    // Omarchy/apps row. Not Browsing: auto-select is for Enter, not SPACE.
    if (g_search.nav.selectFirstPending) {
        const int FIRST = overviewFirstAppIndex(RESULTS);
        if (FIRST >= 0) {
            g_search.nav.selection         = FIRST;
            g_search.nav.zone              = EOverviewNavZone::Result;
            g_search.nav.selectFirstPending = false;
            g_search.nav.explicitSelection = false;
        }
    }

    searchLayoutHere().gridTiles = GRID_TILES;

    // Show Windows has no panel at all: no band to reserve between the pill and
    // the strip (user, 2026-09-24: "all this empty unused space").
    const bool  PANEL  = QUERIED && !g_search.query.windowsOnly;
    const bool TOOLPANE = !RESULTS.empty() && RESULTS[0].tool != 0;
    float      toolRowsHL = -1.F;
    if (PANEL && TOOLPANE) {
        const float SPAN = overviewResultsSpanW(monitor);
        toolRowsHL       = 0.F;
        for (const auto& ROW : RESULTS)
            toolRowsHL += overviewToolRowHL(ROW, SPAN) + OVERVIEW_TOOL_GAPL;
    }
    // The easter egg's frame takes the same band (KeyboardCat::bandL), so the
    // strip makes room for it the way it does for results.
    const float TARGET = std::max(PANEL ? overviewResultsBandTarget(monitor, SECTIONS, GRID_TILES, TOOLPANE, toolRowsHL) : 0.F, Hyprdrive::KeyboardCat::bandL());
    if (std::abs(resultsBand->goal() - TARGET) > 0.5F)
        *resultsBand = TARGET;

    const float WIDTH_TARGET = PANEL ? overviewResultsWidthTarget(monitor, SECTIONS) : 0.F;
    // Grow from the pill, not from 0: a 0→span ease spends its first frames
    // on an invisible sliver, then pops. Warp onto the pill width once.
    if (WIDTH_TARGET > 50.F && resultsWidth->value() < 50.F)
        resultsWidth->setValueAndWarp(std::min(WIDTH_TARGET, overviewPillTargetWL(true)));
    if (std::abs(resultsWidth->goal() - WIDTH_TARGET) > 0.5F)
        *resultsWidth = WIDTH_TARGET;

    if (const float DOCKGOAL = QUERIED && !EMPTYRESULT && !RESULTS.empty() ? 1.F : 0.F; std::abs(searchDock->goal() - DOCKGOAL) > 0.01F)
        *searchDock = DOCKGOAL;

    // The strip ran first this frame and said how far its label needs the pill
    // out of the way (0 when no label is up, so the pill settles back down).
    if (std::abs(pillNudge->goal() - pillNudgeWant) > 0.5F)
        *pillNudge = pillNudgeWant;
    // The grid's slide, on the pill's curve: the two move out of the label's way
    // as one gesture.
    if (std::abs(gridNudge->goal() - searchLayoutHere().gridNudgeL) > 0.5F)
        *gridNudge = searchLayoutHere().gridNudgeL;

    const uint8_t COL_MASK = QUERIED ? overviewSearchColMask(SECTIONS) : 0;
    const auto    COL_TGT  = overviewSearchColWidths(resultsWidth->value(), COL_MASK);

    // The LIVE FIELD. Aimed off the text as it was drawn last frame (the draw
    // pass is the only place a glyph's width is known), which one frame behind
    // a spring cannot show. The field hugs the query from its rest width up to
    // the cap; whatever the query needs beyond the cap becomes scroll.
    auto&       FIELDMOTION = g_search.fieldOn(monitor->m_id);
    const float MAXFIELDL   = std::max(OVERVIEW_PILL_FIELDL, sc<float>(monitor->m_size.x) * OVERVIEW_PILL_MAXFRAC - OVERVIEW_PILL_WL(0.F));
    // Whole logical px: the measurement is a glyph run divided by a float scale,
    // and a target that wanders by a fraction of a pixel re-arms the spring --
    // which snaps within its epsilon -- every frame it steps.
    const float NEEDL       = FIELDMOTION.contentL > 0.F ? std::ceil(FIELDMOTION.contentL + OVERVIEW_PILL_ENDROOML) : 0.F;
    // While the overview zooms in or out around a pill at rest, the field
    // LANDS on its width: the glyphs are re-rasterised at each step of the
    // zoom, and a whole-px font size divided by a float scale wobbles a few
    // logical px -- an underdamped spring turned that into a visible bounce.
    const bool  FIELDLANDS  = scale->isBeingAnimated() && !g_pillHovered && !g_pillPress.down && g_search.query.text.empty();
    if (FIELDMOTION.stepTo(std::clamp(NEEDL, OVERVIEW_PILL_FIELDL, MAXFIELDL), std::max(0.F, NEEDL - MAXFIELDL), Time::steadyNow(), FIELDLANDS))
        requestInputFrame();

    // ---- LAYOUT: the animated values are sampled once, right here.
    const SSearchLayoutInputs IN{
        .monitor     = monitor,
        .renderScale = renderScale,
        .layout      = this->layout,
        .fade        = FADE,
        .dock        = searchDock->value(),
        .nudge       = pillNudge->value(),
        .band        = resultsBand->value(),
        .width       = resultsWidth->value(),
        .colW        = {COL_TGT.col[0], COL_TGT.col[1], COL_TGT.col[2]},
        .inspectorW  = COL_TGT.inspector,
        .rowLCarry   = searchLayoutHere().rowL,
        .query       = g_search.query.text,
        .queried     = QUERIED,
        .emptyResult = EMPTYRESULT,
        .selection   = g_search.nav.selection,
        .fieldWL     = FIELDMOTION.drawWidthL,
    };

    SSearchLayout LAYOUT = layoutSearch(IN, RESULTS, SECTIONS);

    // ---- PUBLISH: the one assignment the rest of the overview can see.
    //
    // CARRY, exactly as rowLCarry does above. This assignment replaces the WHOLE
    // published struct, so any field written by somebody other than layoutSearch
    // is wiped by it -- which is what silently happened to both of these when
    // they were added: stripPlateL is the strip step's output from earlier this
    // frame, gridTiles was measured at the top of this one, and both are read by
    // NEXT frame's band and strip. A field added here tomorrow needs the same
    // line, so keep them together.
    LAYOUT.stripPlateL = searchLayoutHere().stripPlateL;
    LAYOUT.gridTiles   = GRID_TILES;
    LAYOUT.gridNudgeL  = searchLayoutHere().gridNudgeL;
    LAYOUT.gridShiftL  = gridNudge->value();

    g_search.nav.selection = LAYOUT.selection;
    searchLayoutHere()     = std::move(LAYOUT);

    if (QUERIED && g_search.nav.selection >= 0 && g_search.nav.selection < sc<int>(RESULTS.size())) {
        if (RESULTS[g_search.nav.selection].omarchy && !RESULTS[g_search.nav.selection].omarchy->filePath.empty()) {
            SearchBackend::updateSushiIfActive(RESULTS[g_search.nav.selection].omarchy->filePath);
        }
    }

    // ---- DRAW: out of the published object, so the painter, the pointer and
    // the keyboard are provably looking at the same boxes.
    SearchLetters::g_searchLetters.sync(g_search.query.text, Time::steadyNow());
    {
        const auto& SL   = searchLayoutHere();
        const bool  KEYS = activeScrollOverview().get() == this;
        const bool PILLLIT = (KEYS && g_search.nav.zone == EOverviewNavZone::Search) || pointerOnChrome(SL.pill);
        g_pillHovered      = KEYS && pointerOnChrome(SL.pill) && g_search.query.text.empty();
        g_pillPress.step(Time::steadyNow());
        if (pillPlaying())
            requestInputFrame();
        const bool BODYLIT = (KEYS && g_search.nav.zone == EOverviewNavZone::Result) || pointerOnChrome(SL.containerBox);
        // THE RESULTS PANE'S ENTRANCE AND EXIT, motion-blurred (user,
        // 2026-09-30: "0.5 motion blur on the search results pane appearing /
        // disappearing"): while its band or width animates, the body is drawn
        // into a nested pass smeared from last frame's box -- half strength and
        // the half-duration fade, like the scratchpad. The pill stays sharp.
        const CBox PREVBOX = searchPaneLastBox;
        searchPaneLastBox  = SL.containerBox;
        const bool GROWING = (resultsBand->isBeingAnimated() || resultsWidth->isBeingAnimated()) && PREVBOX.w >= 2 && PREVBOX.h >= 2 && SL.containerBox.w >= 2 &&
            SL.containerBox.h >= 2;
        bool blurred = false;
        if (GROWING) {
            const float PCT  = std::max(resultsBand->isBeingAnimated() ? resultsBand->getPercent() : 0.F, resultsWidth->isBeingAnimated() ? resultsWidth->getPercent() : 0.F);
            const CBox  PREV = Hyprdrive::FlightBlur::shape(Hyprdrive::FlightBlur::ESite::SEARCH_PANE, PREVBOX, SL.containerBox, PCT);
            blurred = Hyprdrive::FlightBlur::drawBlurred(monitor, nullptr, PREV, SL.containerBox, [&] { drawSearch(SL, PILLLIT, BODYLIT, 2); },
                                                              Hyprdrive::FlightBlur::layoutSamples());
        }
        if (std::abs(g_pillPress.x - 1.F) > 0.0005F) {
            // The pill, squeezed about its own centre: its BOXES are scaled, not
            // the draw (a render modifier scales each box but not the rounded
            // mask or the clip, which left square corners poking past the
            // outline). Every corner is drawn from these boxes as usual; the
            // type keeps its size and is clipped to the narrower field.
            if (!blurred)
                drawSearch(SL, PILLLIT, BODYLIT, 2);
            const float    S  = g_pillPress.x;
            const Vector2D C  = SL.pill.middle();
            const auto     SQ = [&](const CBox& b) {
                return CBox{C.x + (b.x - C.x) * S, C.y + (b.y - C.y) * S, b.width * S, b.height * S}.round();
            };
            SSearchLayout squeezed = SL;
            squeezed.pill          = SQ(SL.pill);
            squeezed.badge         = SQ(SL.badge);
            squeezed.gap           = SL.gap * S;
            squeezed.fieldW        = std::max(1.F, SL.fieldW * S);
            if (SL.fieldXExact > 0.F)
                squeezed.fieldXExact = sc<float>(C.x + (SL.fieldXExact - C.x) * S);
            drawSearch(squeezed, PILLLIT, BODYLIT, 1);
        } else
            drawSearch(SL, PILLLIT, BODYLIT, blurred ? 1 : 0);
        // The easter egg (KeyboardCat.hpp) opens out of the pill, over it.
        Hyprdrive::KeyboardCat::render(monitor, SL.pill, SL.scale, SL.fade, SL.rounding, SL.roundingPower, SL.bandH);
    }

    // The letter bounce is timestamped, not a PHLANIMVAR, so the compositor
    // will not schedule the rest of the pop on its own. Its frames come from
    // the letter tick (SearchInput.cpp letterTickCb, off the render pass, armed
    // by the edit), which damages every overview while the letters or the easter
    // egg are busy -- never from inside this pass.
}

static const std::string* searchPreviewPath(const std::vector<SSearchRow>& results, int index) {
    if (index < 0 || index >= sc<int>(results.size()))
        return nullptr;
    const auto& ROW = results[sc<size_t>(index)];
    if (ROW.omarchy && ROW.omarchy->canQuickLook && !ROW.omarchy->filePath.empty())
        return &ROW.omarchy->filePath;
    return nullptr;
}

bool overviewToggleSushiForSelection() {
    const auto        RESULTS = overviewSearchRows();
    const std::string* PATH   = searchPreviewPath(RESULTS, g_search.nav.selection);
    if (!PATH)
        return false;
    SearchBackend::toggleSushi(*PATH);
    return true;
}

bool overviewHandleSearchSpace() {
    const auto RESULTS = overviewSearchRows();
    const std::string* HOVER = searchPreviewPath(RESULTS, g_search.nav.hoverIndex);
    const std::string* SEL   = searchPreviewPath(RESULTS, g_search.nav.selection);
    if (!overviewSpacePreviews(g_search.nav.intent, HOVER != nullptr, SEL != nullptr, g_search.nav.explicitSelection)) {
        if (SearchBackend::isSushiActive())
            SearchBackend::closeSushi();
        return false;
    }

    const std::string* PATH = (g_search.nav.intent == ESearchIntent::Hovering && HOVER) ? HOVER : SEL;
    if (!PATH)
        return false;
    SearchBackend::toggleSushi(*PATH);
    return true;
}

bool overviewHandleShiftTap() {
    if (g_search.query.text.empty())
        return false;

    const auto RESULTS = overviewSearchRows();
    int        idx     = g_search.nav.selection;
    if (idx < 0 || idx >= sc<int>(RESULTS.size()))
        idx = overviewFirstAppIndex(RESULTS);
    if (idx < 0 || idx >= sc<int>(RESULTS.size()) || !RESULTS[sc<size_t>(idx)].app)
        return false;

    AppSearch::launch(*RESULTS[sc<size_t>(idx)].app, Hyprdrive::Config::getTerminal());
    for (const auto& overview : scrollOverviews()) {
        if (overview)
            overview->close();
    }
    return true;
}

// Ctrl x2: make the AI agent row being browsed Omarchy's default coding agent.
// The row is SPACE's: the one under the pointer while the pointer is doing the
// browsing, the keyboard selection otherwise. The agent then leads the AI &
// Tools column, and the selection follows it there -- left where it was, the
// highlight would sit on whichever agent slid into the old slot.
bool overviewHandleCtrlTap() {
    const auto RESULTS = overviewSearchRows();

    const auto agentAt = [&](int idx) -> std::optional<SearchBackend::SSearchItem> {
        if (idx < 0 || idx >= sc<int>(RESULTS.size()) || !RESULTS[sc<size_t>(idx)].omarchy)
            return std::nullopt;
        const auto& ITEM = *RESULTS[sc<size_t>(idx)].omarchy;
        return SearchBackend::canBeDefaultAgent(ITEM) ? std::optional{ITEM} : std::nullopt;
    };

    auto item = g_search.nav.intent == ESearchIntent::Hovering ? agentAt(g_search.nav.hoverIndex) : std::nullopt;
    if (!item)
        item = agentAt(g_search.nav.selection);
    if (!item || !SearchBackend::setDefaultAgent(*item))
        return false;

    const auto REORDERED = overviewSearchRows();
    for (size_t i = 0; i < REORDERED.size(); ++i) {
        if (REORDERED[i].omarchy && REORDERED[i].omarchy->id == item->id) {
            g_search.nav.selection = sc<int>(i);
            break;
        }
    }

    for (const auto& overview : scrollOverviews()) {
        if (overview)
            overview->damage();
    }
    return true;
}

