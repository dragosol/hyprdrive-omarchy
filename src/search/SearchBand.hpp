#pragma once

// The results band's maths: card and row metrics, column widths, the row budget,
// row cap and trim, and the band and width targets.
// Cut from OverviewInternal.hpp by pure code motion. Not standalone: that header
// includes this one at the point the block stood, after everything it reads.

namespace OvInternal {

// X21: the results panel is part of the LAYOUT STACK, not an overlay. Its band
// sits between the search row and the strip, so a growing list pushes the strip
// down and shrinks the grid instead of covering them. The EASED value (X23) is
// mirrored out of COverview::resultsBand into the published layout -- the
// free layout helpers below and the hit-test paths all read it, and none of them
// can reach a member.
inline constexpr float OVERVIEW_RESULT_ROWHL = 34.F, OVERVIEW_RESULT_ROWGAPL = 6.F, OVERVIEW_RESULT_PANELPADL = 8.F, OVERVIEW_RESULT_PANELGAPL = 10.F;
// X49. The results are WIDE and CATEGORISED: one rounded card per provider, the
// provider's icon and name on the card's leading edge, its rows across the rest
// of the width. HEADERWL is that leading column; HEADERHL is the floor a card
// gets, so a one-row section still has room for a 32px provider icon.
inline constexpr float OVERVIEW_RESULT_SECTIONGAPL = 8.F, OVERVIEW_RESULT_HEADERWL = 168.F, OVERVIEW_RESULT_HEADERHL = 44.F;
// G5's OTHER layout. Apps are a grid of icon cells, not rows: a 48px icon with
// its name under it. CELLWL is the widest a cell gets -- the grid caps at 6 AND
// at whatever fits, so a narrow monitor wraps instead of overflowing.
inline constexpr float OVERVIEW_RESULT_CELLWL = 132.F, OVERVIEW_RESULT_CELLHL = 78.F, OVERVIEW_RESULT_CELLICONL = 48.F;

// The rest of the card's metrics, which the draw pass used to spell out as bare
// literals eight scroll-screens away from the family above. Every one of these
// is multiplied by the card transform's SCALE at the point of use -- they are
// LOGICAL px like the OVERVIEW_RESULT_* family, not rendered px.
//
// The repeats are named apart on purpose: the icon-to-title gap, the icon-to-row
// -text gap and the name-to-description gap were all the literal 10, and reading
// them as one number is how "nudge the description over" quietly moves the G6
// header too.
inline constexpr float OVERVIEW_CARD_PADL         = 12.F; // inner pad: card edge -> icon, row edge -> icon
inline constexpr float OVERVIEW_CARD_HEADICONL    = 32.F; // G6 provider icon on the leading edge
inline constexpr float OVERVIEW_CARD_HEADGAPL     = 10.F; // that icon -> the provider name
inline constexpr float OVERVIEW_CARD_HEADPX       = 14.F; // the provider name
inline constexpr float OVERVIEW_CARD_MOREPX       = 12.F; // G6's "%d more", one step down
inline constexpr float OVERVIEW_CARD_HEADLINEGAPL = 2.F;  // name -> "%d more", so the pair centres as a block
inline constexpr float OVERVIEW_CARD_ROWPX        = 14.F; // a list row's name and description
inline constexpr float OVERVIEW_CARD_ROWICONL     = 22.F; // the app icon on a list row
inline constexpr float OVERVIEW_CARD_ROWICONGAPL  = 10.F; // that icon -> the name
inline constexpr float OVERVIEW_CARD_DESCGAPL     = 10.F; // the name -> the dimmer description beside it
inline constexpr float OVERVIEW_CARD_DESCMINL     = 24.F; // below this much room the description is dropped, not squeezed
inline constexpr float OVERVIEW_CARD_CELLPADL     = 6.F;  // G5 grid cell: its own inset
inline constexpr float OVERVIEW_CARD_CELLGAPL     = 4.F;  // that cell's icon -> its label
inline constexpr float OVERVIEW_CARD_CELLPX       = 13.F; // that label

inline float overviewResultsLogicalHeight(PHLMONITOR monitor) {
    auto& S = searchSession();
    return monitor ? std::max(0.F, S.layoutOn(monitor->m_id).bandH) : 0.F;
}

// The width the result cards span: the monitor inset by the mission-control
// gap, which is the same span the grid tiles get (X49). The band maths and the
// render pass both need it, and a grid's height depends on it.
inline float overviewResultsSpanW(PHLMONITOR monitor) {
    if (!monitor)
        return 1.F;

    const float GAPL = sc<float>(Hyprdrive::Config::getMissionControlGap());
    return std::clamp(sc<float>(monitor->m_size.x) * 0.84F, 880.F, std::max(1.F, sc<float>(monitor->m_size.x) - (GAPL * 2.F)));
}

// A DICTIONARY/TRANSLATOR ROW IS AS TALL AS ITS TEXT (user, 2026-09-24:
// "better wrapping on definitions - we're leaving space on the table"). The
// rows were a fixed OVERVIEW_TOOL_ROWHL, so a long definition was cut at two
// lines and a short one sat in a gap. Measured at LOGICAL scale with the same
// fonts and columns the drawing uses (Overview.cpp's tool pane), so the
// layout and the pixels agree; OVERVIEW_TOOL_ROWHL stays the floor.
inline constexpr float OVERVIEW_TOOL_ROWPADL = 10.F, OVERVIEW_TOOL_TEXTPX = 14.F, OVERVIEW_TOOL_EXPX = 11.F, OVERVIEW_TOOL_VPADL = 5.F;
inline constexpr float OVERVIEW_TOOL_MAXLINES = 6.F;
inline float overviewToolTagWL(uint8_t kind) {
    return kind == 1 ? 92.F : 52.F;
}
inline float overviewToolTextWL(uint8_t kind, float containerWL) {
    return std::max(16.F, containerWL - (2.F * OVERVIEW_TOOL_PADL) - (2.F * OVERVIEW_TOOL_ROWPADL) - overviewToolTagWL(kind));
}
inline float overviewToolRowHL(const SSearchRow& row, float containerWL) {
    if (row.toolStatus || row.name.empty())
        return OVERVIEW_TOOL_ROWHL;
    const int   W    = std::max(16, ((sc<int>(overviewToolTextWL(row.tool, containerWL)) + 15) / 16) * 16);
    const auto  TX   = OverviewRender::textWrapped(row.name, CHyprColor{1.F, 1.F, 1.F, 1.F}, sc<int>(OVERVIEW_TOOL_TEXTPX), W,
                                                   sc<int>(OVERVIEW_TOOL_TEXTPX * 1.35F * OVERVIEW_TOOL_MAXLINES), false, "", 400);
    const float TXH  = TX && TX->ok() ? sc<float>(TX->m_size.y) : OVERVIEW_TOOL_TEXTPX * 1.35F;
    const float EXH  = row.toolExample.empty() ? 0.F : OVERVIEW_TOOL_EXPX * 1.5F;
    return std::max(OVERVIEW_TOOL_ROWHL, TXH + EXH + (2.F * OVERVIEW_TOOL_VPADL) + 8.F);
}

// Unified search panel: visible columns + inspector fill the container.
// Empty columns ease to width 0; leftover is split by flex (files take more
// when present so paths fit).
inline constexpr float SEARCH_PAD_L        = 14.F;
inline constexpr float SEARCH_DIV_GAPL     = 14.F;
inline constexpr float SEARCH_INSP_WL      = 280.F;
inline constexpr float SEARCH_INSP_MID_WL  = 340.F; // 2 result columns
inline constexpr float SEARCH_INSP_WIDE_WL = 400.F; // 1 result column
inline constexpr float SEARCH_COL_FLEX[3]  = {1.F, 1.45F, 1.F};
inline constexpr float SEARCH_COL_HIDE_WL  = 8.F;

struct SSearchColWidths {
    float col[3]    = {};
    float inspector = SEARCH_INSP_WL;
    float container = 0.F;
    int   visible   = 0;
};

inline uint8_t overviewSearchColMask(const std::vector<SSearchSection>& sections) {
    uint8_t mask = 0;
    for (const auto& sec : sections) {
        if (!sec.rows.empty() && sec.column >= 0 && sec.column < 3)
            mask |= static_cast<uint8_t>(1u << sec.column);
    }
    return mask;
}

inline SSearchColWidths overviewSearchColWidths(float containerWL, uint8_t mask) {
    SSearchColWidths w;
    w.container = containerWL;
    int   n    = 0;
    float flex = 0.F;
    for (int i = 0; i < 3; ++i) {
        if (mask & (1u << i)) {
            ++n;
            flex += SEARCH_COL_FLEX[i];
        }
    }
    w.visible = n;
    if (n == 0 || containerWL < SEARCH_COL_HIDE_WL)
        return w;

    const float CHROME = (SEARCH_PAD_L * 2.F) + (SEARCH_DIV_GAPL * sc<float>(n));
    float       insp   = SEARCH_INSP_WL;
    if (n == 1)
        insp = SEARCH_INSP_WIDE_WL;
    else if (n == 2)
        insp = SEARCH_INSP_MID_WL;
    const float MIN_COLS = 160.F * sc<float>(n);
    insp                 = std::min(insp, std::max(200.F, containerWL - CHROME - MIN_COLS));
    float inner          = containerWL - CHROME - insp;
    if (inner < 120.F) {
        insp  = std::clamp(containerWL * 0.28F, 160.F, insp);
        inner = containerWL - CHROME - insp;
    }
    inner         = std::max(0.F, inner);
    w.inspector   = insp;
    if (flex <= 0.F)
        return w;
    for (int i = 0; i < 3; ++i) {
        if (mask & (1u << i))
            w.col[i] = inner * (SEARCH_COL_FLEX[i] / flex);
    }
    return w;
}

// G5: the grid "caps at 6 and at whatever fits the width". The 6 is applied
// when the rows are gathered; this is the width half.
inline size_t overviewResultsGridCols(size_t cells, float spanWL) {
    const float INNER = std::max(1.F, spanWL - (OVERVIEW_RESULT_PANELPADL * 2.F));
    const auto  FITS  = sc<size_t>(std::max(1.F, std::floor(INNER / OVERVIEW_RESULT_CELLWL)));
    return std::max(sc<size_t>(1), std::min(cells, FITS));
}

// The logical height one card needs. A LIST card is its rows, its padding, and
// a floor so the G6 provider icon on the leading edge always fits a one-row
// section. A GRID card is however many cell rows the width forced it into.
inline float overviewResultsSectionHeight(const SSearchSection& section, float spanWL) {
    if (section.grid) {
        const float ROWS = std::ceil(sc<float>(section.rows.size()) / sc<float>(overviewResultsGridCols(section.rows.size(), spanWL)));
        return (OVERVIEW_RESULT_PANELPADL * 2.F) + (ROWS * OVERVIEW_RESULT_CELLHL) + ((ROWS - 1.F) * OVERVIEW_RESULT_ROWGAPL);
    }

    const float N    = sc<float>(section.rows.size());
    const float BODY = (N * OVERVIEW_RESULT_ROWHL) + ((N - 1.F) * OVERVIEW_RESULT_ROWGAPL);
    return (OVERVIEW_RESULT_PANELPADL * 2.F) + std::max(BODY, OVERVIEW_RESULT_HEADERHL);
}

// The height the band is easing TOWARD -- X49 sums the SECTION CARDS rather
// than a flat row count -- still capped by a BUDGET so that however many rows
// the query matches, the desktop strip and the window tiles stay on screen
// (DoD clause 23, whose PLACEMENT half X60 rewrote and whose "the overview
// stays visible" half is unchanged).
// X64. USER-REPORTED: "it should show just like gnome with results in
// categories" -- against a build that showed the apps icons and nothing else.
//
// renderSearch() drops any card that will not fit the band, and the apps grid is
// drawn FIRST, so an overflowing stack loses the PROVIDER categories entirely.
// G5 turned that from a latent ceiling into a daily one by raising the caps from
// 5 SHARED to 6 apps + 5 rows PER PROVIDER.
//
// So the stack is made to FIT before anything measures it: rows are given up
// from the tallest section first, and no section is ever taken below one row.
// Every category survives; the rows given up are what G6's "%d more" reports.
// HOW TALL THE PANEL MAY GET, and HOW MANY ROWS THAT BUYS. Both the trim and
// the band ask these, so the two can never disagree about the answer -- which
// is exactly how the flat 8 above outlived the ratio ceiling that replaced it.
//
// The panel and the window previews share a single column of screen, and the
// old flat "8 rows, 360px" answered a one-letter query by spending 40% of the
// monitor on menu rows nobody asked for while the windows actually being
// filtered were squeezed into what was left (user, 2026-09-23). So the ceiling
// asks what the grid below is doing:
//
//   crowded  -- more than 4 previews are up, and they are the small ones. 6.
//   normal   -- a few previews. 8, the old number.
//   alone    -- NOTHING matched, so there are no previews to protect and the
//               grid's height goes to zero: the leftover is the panel's.
//
// The alone cap is a ceiling on the WORK, not on the space -- each row
// rasterises text and uploads a texture -- so the geometry below almost always
// binds first and the 20 only matters on a very tall panel.
inline float overviewResultsBudgetL(PHLMONITOR monitor, size_t gridTiles) {
    if (!monitor)
        return 0.F;

    float budget = std::min(360.F, sc<float>(monitor->m_size.y) * 0.42F);

    // With no previews the grid's own height goes to ZERO, so everything under
    // the strip is the panel's (user, 2026-09-23). What stays behind is one
    // mission-control gap, so the strip never sits flush against the desktop
    // card's bottom edge when a long list pushes the whole stack down.
    //
    // The strip term is the stack AS DRAWN rather than its ceiling, which is
    // safe only on this branch and is why the branch exists: overviewStripStackL
    // -> overviewStripLogicalHeight reads the published band to size its
    // leftover ceiling, but that ceiling is gated on gridTiles > 0, so with no
    // tiles the stack depends on nothing this budget can move.
    if (gridTiles == 0) {
        const float SPARE = sc<float>(monitor->m_size.y) - OVERVIEW_SEARCH_ROWHL - overviewStripStackL(monitor) - sc<float>(overviewMissionControlGap());
        budget            = std::max(budget, SPARE);
    }

    return budget;
}

inline size_t overviewResultsRowCap(PHLMONITOR monitor, size_t gridTiles) {
    const size_t RATIO = gridTiles == 0                         ? OVERVIEW_RESULT_ROWS_ALONE :
        gridTiles > OVERVIEW_RESULT_TILES_CROWDED               ? OVERVIEW_RESULT_ROWS_CROWDED :
                                                                  OVERVIEW_RESULT_ROWS_NORMAL;

    // The ratio is the intent; the room is the hard limit. Trimming to more
    // rows than the budget can draw would spill the card past its container.
    const float  ROOM = overviewResultsBudgetL(monitor, gridTiles) - OVERVIEW_RESULT_CARD_CHROMEL - (OVERVIEW_RESULT_PANELGAPL * 2.F);
    const size_t FITS = ROOM >= OVERVIEW_RESULT_ROWHL_FULL ? sc<size_t>(ROOM / OVERVIEW_RESULT_ROWHL_FULL) : 1;

    return std::max<size_t>(1, std::min(RATIO, FITS));
}

inline void overviewTrimSectionsToFit(std::vector<SSearchSection>& sections, PHLMONITOR monitor, size_t gridTiles) {
    auto& S = searchSession();
    if (sections.empty() || !monitor)
        return;

    // THE CAP COMES FROM THE SAME PLACE THE BAND'S DOES. This used to be a flat
    // `MAX_ROWS = 8` and it runs BEFORE the band is sized, so it silently
    // decided the answer: the band measured an already-truncated list and no
    // ceiling above 8 could ever be reached, however much room there was
    // (user, 2026-09-23: "I never saw more than 8 rows in the results pane").
    const size_t MAX_ROWS = overviewResultsRowCap(monitor, gridTiles);
    for (auto& sec : sections) {
        if (sec.rows.size() > MAX_ROWS)
            sec.rows.resize(MAX_ROWS);
    }
}

inline float overviewResultsWidthTarget(PHLMONITOR monitor, const std::vector<SSearchSection>& sections) {
    if (!monitor)
        return 0.F;

    // Always the full span once any column exists: hiding empty columns and
    // growing the box as files arrived was the janky expand. Files take the
    // leftover inside a stable outer width.
    bool any = false;
    for (const auto& s : sections) {
        if (!s.rows.empty()) {
            any = true;
            break;
        }
    }
    if (!any)
        return 0.F;

    return overviewResultsSpanW(monitor);
}

inline float overviewResultsBandTarget(PHLMONITOR monitor, const std::vector<SSearchSection>& sections, size_t gridTiles, bool toolPane = false, float toolRowsHL = -1.F) {
    if (sections.empty() || !monitor)
        return 0.F;

    size_t maxRows = 0;
    for (const auto& sec : sections)
        maxRows = std::max(maxRows, sec.rows.size());
    if (maxRows == 0)
        maxRows = 1;

    // ...and never more rows than there ARE results: a six-row budget spent on
    // three answers is the same hole seen from the other side. The sections
    // arrive already trimmed to overviewResultsRowCap, so this only has to
    // honour what actually survived.
    const float CARD_H = toolPane ?
        (2.F * OVERVIEW_TOOL_PADL) + OVERVIEW_TOOL_HEADL + (toolRowsHL >= 0.F ? toolRowsHL : sc<float>(maxRows) * (OVERVIEW_TOOL_ROWHL + OVERVIEW_TOOL_GAPL)) +
            (OVERVIEW_RESULT_PANELGAPL * 2.F) :
        OVERVIEW_RESULT_CARD_CHROMEL + (sc<float>(maxRows) * OVERVIEW_RESULT_ROWHL_FULL) + (OVERVIEW_RESULT_PANELGAPL * 2.F);

    return std::min(CARD_H, overviewResultsBudgetL(monitor, gridTiles));
}

} // namespace OvInternal
