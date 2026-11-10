// CScrollOverview: search UI: layout, draw, render, pill/letter animation, menu key handlers.
// Cut from scrollOverview.cpp by pure code motion.
#include "OverviewParts.hpp"

using namespace OverviewDetail;


// How many open windows the query reaches. Counted across every workspace, not
// just the focused one: the strip shows matching minis from elsewhere, so "no
// matches" has to agree with that.
size_t CScrollOverview::matchingWindowCount() const {
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

// Everything layoutSearch() reads that is neither a result nor a section. The
// animated values are sampled ONCE here, at a named moment, instead of each
// consumer calling ->value() whenever it happens to run.
struct SSearchLayoutInputs {
    PHLMONITOR                     monitor;
    float                          renderScale = 1.F;
    ScrollOverview::Config::ELayout layout{};
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

// LAYOUT. Pure: monitor, animation values and the section set in, boxes out. It
// reads no global and no member and writes nothing -- which is exactly what
// lets the painter, the pointer hit-test and the keyboard walk all be handed
// the same answer instead of each deriving its own.
static SSearchLayout layoutSearch(const SSearchLayoutInputs& IN, const std::vector<SSearchRow>& RESULTS, const std::vector<SSearchSection>& SECTIONS) {
    SSearchLayout out;
    out.bandH       = IN.band;
    out.rowL        = IN.rowLCarry;
    out.fade        = IN.fade;
    out.queried     = IN.queried;
    out.emptyResult = IN.emptyResult;
    out.selection   = IN.selection;

    const auto MONITOR = IN.monitor;
    out.monitorId      = MONITOR ? MONITOR->m_id : MONITOR_INVALID;

    out.placeholder = IN.query.empty(); // the root browse is queried with nothing typed: still the placeholder
    out.label       = out.placeholder ? "Search, calculate or run" : (IN.emptyResult ? std::string(IN.query) + "    no matches" : std::string(IN.query));

    // The pill is laid out in the SAME logical space as the strip it sits above.
    // That placement is load-bearing and is NOT the part X22 changed: laid out
    // at the monitor's true top edge it landed underneath the layer-shell bar,
    // which this pass draws after the whole overview, so the pill was present
    // but invisible (X10). The overview scale insets this logical row far
    // enough down to clear the bar; X22 only removed the pan (see
    // OVERVIEW_CHROME_NO_PAN).
    // The live field (SSearchFieldMotion): springs toward the query's width,
    // capped so the pill never passes OVERVIEW_PILL_MAXFRAC of the monitor.
    const float FIELDWL     = IN.fieldWL;
    const float PILLWL      = OVERVIEW_PILL_WL(FIELDWL);
    const CBox  PILLLOGICAL = {
        sc<float>(MONITOR->m_position.x) + (sc<float>(MONITOR->m_size.x) - PILLWL) / 2.F,
        sc<float>(MONITOR->m_position.y) + (OVERVIEW_SEARCH_ROWHL - OVERVIEW_PILL_HL) / 2.F,
        PILLWL,
        OVERVIEW_PILL_HL,
    };

    CBox pill = getOverviewGlobalBox(PILLLOGICAL, MONITOR, IN.renderScale, OVERVIEW_CHROME_NO_PAN, 0.F, IN.layout);

    // Kept so the dock offset below can be read back out in logical px.
    const float PILLUNDOCKEDY = sc<float>(pill.y);

    // X50: docked, the pill leaves the card's inset behind and sits just under
    // the layer-shell bar, which is what the work area's top edge means. Only y
    // moves -- the card scales about the viewport centre, so a logically centred
    // box is still centred after the transform. getOverviewBox works in
    // monitor-local RENDERED px, hence the m_scale on the way in.
    if (const float DOCK = std::clamp(IN.dock, 0.F, 1.F); DOCK > 0.001F) {
        float dockTopL = overviewTopChromeBottomL(MONITOR);
        if (const auto WS = MONITOR->m_activeWorkspace; WS && WS->m_space) {
            if (const auto WA = WS->m_space->workArea(); WA.w > 1 && WA.h > 1)
                dockTopL = std::max(dockTopL, sc<float>(WA.y)); // a bar that DOES reserve
        }

        const float DOCKY = (dockTopL - sc<float>(MONITOR->m_position.y) + OVERVIEW_SEARCH_DOCKPADL) * sc<float>(MONITOR->m_scale);
        pill.y += (DOCKY - sc<float>(pill.y)) * DOCK;
    }

    if (pill.width <= 1.F || !overviewBoxIntersectsMonitor(pill, MONITOR))
        return out; // nothing to draw, and the published heights carry unchanged

    // The transform's scale, read BEFORE the box is snapped to whole pixels.
    // Read after, it moved with the rounding of the pill's width -- and the text
    // is measured in that scale, and the measurement is what sizes the field,
    // which sizes the pill, which rounds differently: a loop that settled into
    // flipping between two widths a pixel apart, the whole bar and its text
    // twitching every few frames while nothing was being typed (user,
    // 2026-09-14, recording 14-16-59: pill edges 1117/2318 <-> 1118/2317).
    // (getOverviewGlobalBox rounds by default, so `pill` is already whole
    // pixels here -- the first attempt at this read the rounded width and the
    // loop kept running: widths 824/826, scale 1.3105/1.3135, logged
    // 2026-09-14. The unrounded transform of the same box is the real scale.)
    const float TRANSFORMSCALE = sc<float>(getOverviewGlobalBox(PILLLOGICAL, MONITOR, IN.renderScale, OVERVIEW_CHROME_NO_PAN, 0.F, IN.layout, false).width) / PILLWL;
    pill.round();

    out.drawable = true;
    out.pill     = pill;

    // Same rounding the workspace cards and the strip use: decoration:rounding
    // (0 on Omarchy) and decoration:rounding_power. Clamped per-box at draw.
    const auto CARDROUND = getOverviewCardRounding(MONITOR, IN.renderScale);
    out.rounding         = CARDROUND.rounding;
    out.roundingPower    = CARDROUND.power;

    // Everything inside the pill is measured off the box as it actually landed,
    // so the badge and the type follow the card transform for free.
    out.scale          = TRANSFORMSCALE;
    const float SCALE  = out.scale;
    const float BADGED = std::round(OVERVIEW_PILL_BADGEL * SCALE);
    const float PADL   = std::round(OVERVIEW_PILL_PADL * SCALE);
    out.gap            = std::round(OVERVIEW_PILL_GAPL * SCALE);
    out.fieldW         = std::round(FIELDWL * SCALE);

    out.fieldXExact = sc<float>(getOverviewGlobalBox(PILLLOGICAL, MONITOR, IN.renderScale, OVERVIEW_CHROME_NO_PAN, 0.F, IN.layout, false).x) + PADL + BADGED + out.gap;

    out.badge = CBox{pill.x + PADL, pill.y + std::round((pill.height - BADGED) / 2.F), BADGED, BADGED};
    out.badge.round();

    // The row the pill vacates, reclaimed -- the user asked, so the ~32px the
    // old ponytail note deferred is taken here.
    //
    // The floor is the pill's own foot, brought back into the card's logical
    // space: getOverviewGlobalBox is affine and the pill's width just measured
    // its scale, so the undocked box inverts it from one known point. Undocked
    // this evaluates to 8 + 48 + 8 = the full 64px row, so the un-queried
    // layout is unchanged by construction rather than by a special case.
    const float PILLFOOTL = PILLLOGICAL.y + OVERVIEW_PILL_HL + ((sc<float>(pill.y) - PILLUNDOCKEDY) / std::max(0.001F, SCALE));
    out.rowL              = std::clamp(PILLFOOTL - sc<float>(MONITOR->m_position.y) + OVERVIEW_SEARCH_DOCKPADL, 0.F, OVERVIEW_SEARCH_ROWHL);

    // THE NUDGE, applied only AFTER rowL: the strip, the results and the grid
    // all hang off rowL, so lifting the pill before it would lift the strip too
    // -- and the label the pill is making room for would chase it upward. Here
    // it moves the pill and nothing else. Capped at the docked height, just
    // under the bar, which is as high as the pill ever goes.
    {
        float dockTopL = overviewTopChromeBottomL(MONITOR);
        if (const auto WS = MONITOR->m_activeWorkspace; WS && WS->m_space) {
            if (const auto WA = WS->m_space->workArea(); WA.w > 1 && WA.h > 1)
                dockTopL = std::max(dockTopL, sc<float>(WA.y));
        }
        const float CEIL = (dockTopL - sc<float>(MONITOR->m_position.y) + OVERVIEW_SEARCH_DOCKPADL) * sc<float>(MONITOR->m_scale);
        out.pillRest     = out.pill;
        out.pillMaxUp    = std::max(0.F, sc<float>(out.pill.y) - CEIL);

        if (IN.nudge > 0.5F) {
            const float UP = std::round(std::clamp(IN.nudge, 0.F, out.pillMaxUp));
            out.pill.y -= UP;
            out.badge.y -= UP;
        }
    }

    // F3: the app half of the search, laid out under the pill. This is a second,
    // ordered list ALONGSIDE the window gate -- it deliberately does not change
    // which windows the grid shows (RICE_PROGRESS.md "F3-scope" §1).
    //
    // X49: one rounded card PER PROVIDER, spanning the same width the grid tiles
    // do, each with the provider's icon and name on its leading edge and its
    // rows laid out across the rest. There is no outer panel -- the cards are
    // the surface.
    const float BANDHL = std::max(0.F, out.bandH);
    if (SECTIONS.empty() || BANDHL <= (OVERVIEW_RESULT_PANELGAPL * 2.F) + 1.F)
        return out;

    // The ranking can shrink under a selection made against a longer list. It is
    // clamped by the frame that noticed and handed back by the publish step.
    if (out.selection >= sc<int>(RESULTS.size()))
        out.selection = sc<int>(RESULTS.size()) - 1;

    const float PAD_L      = SEARCH_PAD_L;
    const float DIV_GAPL   = SEARCH_DIV_GAPL;
    const float MAX_SPANWL = overviewResultsSpanW(MONITOR);

    float CONTAINER_WL = IN.width > 1.F ? std::min(IN.width, MAX_SPANWL) : 0.F;
    if (CONTAINER_WL < 8.F)
        return out;

    const float CONTAINER_XL = sc<float>(MONITOR->m_position.x) + ((sc<float>(MONITOR->m_size.x) - CONTAINER_WL) / 2.F);
    const float CONTAINER_YL = sc<float>(MONITOR->m_position.y) + out.rowL + OVERVIEW_RESULT_PANELGAPL;
    const float CONTAINER_HL = BANDHL - (OVERVIEW_RESULT_PANELGAPL * 2.F);

    const CBox CONTAINER_LOGICAL{CONTAINER_XL, CONTAINER_YL, CONTAINER_WL, CONTAINER_HL};
    CBox container = getOverviewGlobalBox(CONTAINER_LOGICAL, MONITOR, IN.renderScale, OVERVIEW_CHROME_NO_PAN, 0.F, IN.layout);
    if (container.width <= 1.F || !overviewBoxIntersectsMonitor(container, MONITOR))
        return out;
    container.round();
    out.containerBox = container;

    // THE TOOL PANE. The rows are the tool's; lay out one header and a full-width
    // entry per row -- each still a hit, so the arrows and the pointer select
    // them exactly as they select a result -- and none of the cards or the
    // inspector.
    if (!RESULTS.empty() && RESULTS[0].tool != 0) {
        out.tool.active   = true;
        out.tool.kind     = RESULTS[0].tool;
        out.tool.title    = RESULTS[0].toolTitle;
        out.tool.subtitle = RESULTS[0].toolSubtitle;

        const float LEFT  = CONTAINER_XL + OVERVIEW_TOOL_PADL;
        const float WIDTH = CONTAINER_WL - (2.F * OVERVIEW_TOOL_PADL);
        const float FOOT  = CONTAINER_YL + CONTAINER_HL - OVERVIEW_TOOL_PADL / 2.F;
        out.tool.header   = getOverviewGlobalBox(CBox{LEFT, CONTAINER_YL + OVERVIEW_TOOL_PADL, WIDTH, OVERVIEW_TOOL_HEADL - 10.F}, MONITOR, IN.renderScale,
                                                 OVERVIEW_CHROME_NO_PAN, 0.F, IN.layout);
        out.tool.header.round();

        float y = CONTAINER_YL + OVERVIEW_TOOL_PADL + OVERVIEW_TOOL_HEADL;
        for (size_t i = 0; i < RESULTS.size(); ++i) {
            // Measured against the FULL span, as the band was: the container is
            // easing toward it, and measuring the eased width would re-wrap every
            // frame of the entrance.
            const float ROWHL = overviewToolRowHL(RESULTS[i], MAX_SPANWL);
            if (y + ROWHL > FOOT)
                break; // the band decides how many fit; the rest wait for a taller one
            CBox row = getOverviewGlobalBox(CBox{LEFT, y, WIDTH, ROWHL}, MONITOR, IN.renderScale, OVERVIEW_CHROME_NO_PAN, 0.F, IN.layout);
            row.round();
            out.hits.emplace_back(SSearchRowHit{.box = row, .desktopId = "", .icon = "", .index = sc<int>(i), .column = 0, .rowInCol = sc<int>(i)});
            out.tool.entries.push_back(SSearchLayout::SToolPane::SEntry{
                .hit      = out.hits.size() - 1,
                .tag      = RESULTS[i].description,
                .text     = RESULTS[i].name,
                .example  = RESULTS[i].toolExample,
                .status   = RESULTS[i].toolStatus,
                .selected = g_search.nav.zone == EOverviewNavZone::Result && sc<int>(i) == out.selection,
            });
            y += ROWHL + OVERVIEW_TOOL_GAPL;
        }
        return out;
    }

    const auto ADDITEM = [&](const CBox& box, size_t I, int col, int rowInCol) {
        out.hits.emplace_back(SSearchRowHit{
            .box       = box,
            .desktopId = RESULTS[I].app ? RESULTS[I].app->id : "",
            .icon      = RESULTS[I].icon,
            .index     = sc<int>(I),
            .column    = col,
            .rowInCol  = rowInCol,
        });
        return SSearchLayout::SItem{
            .hit         = out.hits.size() - 1,
            .name        = RESULTS[I].name,
            .description = RESULTS[I].description,
            .icon        = RESULTS[I].icon,
            .iconFont    = RESULTS[I].iconFont,
            .selected    = (g_search.nav.zone == EOverviewNavZone::Result && sc<int>(I) == out.selection),
        };
    };

    const float HEAD_HL  = 28.F;
    const float ROW_HL   = 32.F;
    const float ROW_GAPL = 4.F;

    const SSearchSection* sectionByCol[3] = {};
    for (const auto& sec : SECTIONS) {
        if (sec.column >= 0 && sec.column < 3)
            sectionByCol[sec.column] = &sec;
    }

    const float INSP_WL = std::clamp(IN.inspectorW, 140.F, std::max(140.F, CONTAINER_WL - (PAD_L * 2.F)));
    const float INSP_XL = CONTAINER_XL + CONTAINER_WL - PAD_L - INSP_WL;
    const float COL_MAX_R = INSP_XL - DIV_GAPL;

    float colX        = CONTAINER_XL + PAD_L;
    int   drawn       = 0;
    for (int c = 0; c < 3; ++c) {
        const float COL_WL = IN.colW[c];
        if (COL_WL < SEARCH_COL_HIDE_WL)
            continue;

        const auto* SECTION = sectionByCol[c];
        if (!SECTION)
            continue;

        const float COL_XL = colX;
        const float COL_YL = CONTAINER_YL + PAD_L;
        const float COL_HL = CONTAINER_HL - (PAD_L * 2.F);
        if (COL_XL + COL_WL > COL_MAX_R + 1.F && COL_MAX_R - COL_XL < SEARCH_COL_HIDE_WL)
            break;

        const CBox COL_LOGICAL{COL_XL, COL_YL, COL_WL, COL_HL};
        CBox colBox = getOverviewGlobalBox(COL_LOGICAL, MONITOR, IN.renderScale, OVERVIEW_CHROME_NO_PAN, 0.F, IN.layout);
        colBox.round();

        SSearchLayout::SCard cardOut;
        cardOut.box   = colBox;
        cardOut.alpha = std::clamp(COL_WL / 120.F, 0.F, 1.F);
        cardOut.grid  = false;
        cardOut.title = SECTION->title;
        cardOut.icon  = SECTION->icon;
        cardOut.more  = SECTION->total > SECTION->rows.size() ? SECTION->total - SECTION->rows.size() : 0;
        if (SECTION->rows.empty())
            cardOut.emptyHint = (c == 1 && OmarchySearch::fileSearchBusy()) ? "Searching…" : "No items";

        for (size_t n = 0; n < SECTION->rows.size(); ++n) {
            const size_t I      = SECTION->rows[n];
            const float  ROW_YL = COL_YL + HEAD_HL + sc<float>(n) * (ROW_HL + ROW_GAPL);
            if (ROW_YL + ROW_HL > COL_YL + COL_HL + 2.F)
                break;

            const CBox ROWLOGICAL{COL_XL, ROW_YL, COL_WL, ROW_HL};
            CBox row = getOverviewGlobalBox(ROWLOGICAL, MONITOR, IN.renderScale, OVERVIEW_CHROME_NO_PAN, 0.F, IN.layout);
            row.round();

            cardOut.items.emplace_back(ADDITEM(row, I, SECTION->column, sc<int>(n)));
        }

        out.cards.emplace_back(std::move(cardOut));
        ++drawn;

        const float DIV_XL = COL_XL + COL_WL + (DIV_GAPL / 2.F);
        if (DIV_XL < INSP_XL - 2.F) {
            const CBox DIV_LOGICAL{DIV_XL, CONTAINER_YL + PAD_L, 1.F, CONTAINER_HL - (PAD_L * 2.F)};
            CBox divBox = getOverviewGlobalBox(DIV_LOGICAL, MONITOR, IN.renderScale, OVERVIEW_CHROME_NO_PAN, 0.F, IN.layout);
            divBox.round();
            divBox.width = std::max(1.F, std::round(1.F * out.scale));
            out.dividers.push_back(divBox);
        }

        colX += COL_WL + DIV_GAPL;
    }

    if (drawn == 0)
        return out;

    const float INSP_YL = CONTAINER_YL + PAD_L;
    const float INSP_HL = CONTAINER_HL - (PAD_L * 2.F);

    const CBox INSP_LOGICAL{INSP_XL, INSP_YL, INSP_WL, INSP_HL};
    CBox inspBox = getOverviewGlobalBox(INSP_LOGICAL, MONITOR, IN.renderScale, OVERVIEW_CHROME_NO_PAN, 0.F, IN.layout);
    inspBox.round();
    if (out.containerBox.width > 1.F)
        inspBox = inspBox.intersection(out.containerBox);
    inspBox.round();

    out.inspector.box     = inspBox;
    out.inspector.visible = inspBox.width > 8.F && inspBox.height > 8.F;
    out.inspector.alpha   = 1.F;

    const int INSP_IDX = (out.selection >= 0 && out.selection < sc<int>(RESULTS.size())) ? out.selection : 0;
    if (INSP_IDX >= 0 && INSP_IDX < sc<int>(RESULTS.size())) {
        const auto& ROW = RESULTS[INSP_IDX];
        out.inspector.title    = ROW.name;
        out.inspector.subtitle = ROW.description;
        out.inspector.icon     = ROW.icon;

        if (ROW.omarchy) {
            const auto& o = *ROW.omarchy;
            out.inspector.category     = o.category;
            out.inspector.canQuickLook = o.canQuickLook;
            if (o.kind == OmarchySearch::EItemKind::File) {
                out.inspector.detail1      = o.fileTypeStr;
                out.inspector.detail2      = o.fileSizeStr + "  ·  " + o.fileModifiedStr;
                out.inspector.detail3      = o.filePath;
                out.inspector.shortcutHint = "Space  Quick Look";
                out.inspector.shortcutHint2 = "Enter  Open";
            } else if (o.kind == OmarchySearch::EItemKind::Menu) {
                out.inspector.detail1      = "Opens this submenu in the left column";
                out.inspector.shortcutHint = "Enter  Open submenu";
                out.inspector.shortcutHint2 = "Left  Back";
            } else if (o.kind == OmarchySearch::EItemKind::Back) {
                out.inspector.detail1      = "Return to the parent menu";
                out.inspector.shortcutHint = "Enter  Go back";
            } else if (o.kind == OmarchySearch::EItemKind::Action && (o.id == "tool:define" || o.id == "tool:translate")) {
                // THE TOOLS' OWN CARD (user, 2026-09-24): how to use it, and a
                // quick answer for what is typed -- the translation into the last
                // language used. The lookup is the same cached one the panel
                // opens onto, so Enter lands on an answer already there.
                const bool  DEFINE = o.id == "tool:define";
                const auto& TEXT   = o.description;
                const auto  TQ     = ToolSearch::parse(DEFINE ? "define " + TEXT : "translate " + TEXT);
                const auto  RES    = ToolSearch::lookup(TQ);
                const auto  TO     = DEFINE ? std::string{} : ToolSearch::defaultTargetLanguage();

                out.inspector.category = DEFINE ? "Dictionary" : "Translate";
                out.inspector.title    = std::string(DEFINE ? "Define" : "Translate") + " \u201c" + TEXT + "\u201d";

                using EState = ToolSearch::SResult::EState;
                switch (RES.state) {
                    case EState::Loading: out.inspector.subtitle = DEFINE ? "Looking up\u2026" : "Translating\u2026"; break;
                    case EState::Empty: out.inspector.subtitle = DEFINE ? "No definitions found" : "No translation came back"; break;
                    case EState::Error: out.inspector.subtitle = RES.message.empty() ? "Could not reach the service" : RES.message; break;
                    case EState::Ready: {
                        const auto& FIRST      = RES.senses.front();
                        out.inspector.subtitle = DEFINE ? (FIRST.tag.empty() ? FIRST.text : FIRST.tag + "  \u00b7  " + FIRST.text) : TO + ":  " + FIRST.text;
                        if (DEFINE && RES.senses.size() > 1)
                            out.inspector.subtitle += "  (+" + std::to_string(RES.senses.size() - 1) + " more)";
                        break;
                    }
                }

                if (DEFINE) {
                    out.inspector.detail1 = "Source: Wiktionary";
                    out.inspector.detail2 = "define <word>    def <word>    dict <word>";
                } else {
                    out.inspector.detail1 = "Into " + TO + " \u2014 the last language used. Name another to switch.";
                    out.inspector.detail2 = "tr <lang> <text>    tr en:fr <text>    translate <text>";
                    out.inspector.detail3 = "Languages by code or name: es, fr, de, ro, ja, spanish, german\u2026";
                }
                out.inspector.shortcutHint = DEFINE ? "Enter  Open the dictionary" : "Enter  Open the translator";
            } else if (o.kind == OmarchySearch::EItemKind::Action) {
                out.inspector.detail1      = o.description.empty() ? o.name : o.description;
                out.inspector.detail2      = o.actionCommand;
                out.inspector.shortcutHint = "Enter  Run";
            } else if (o.kind == OmarchySearch::EItemKind::Agent) {
                out.inspector.detail1      = o.description;
                out.inspector.shortcutHint = "Enter  Launch agent";
                // Same "Key x2  Action" form as the app card's Shift x2 hint.
                if (OmarchySearch::isDefaultAgent(o))
                    out.inspector.shortcutHint2 = "Default agent";
                else if (OmarchySearch::canBeDefaultAgent(o))
                    out.inspector.shortcutHint2 = "Ctrl×2  Set as default";
            } else if (o.kind == OmarchySearch::EItemKind::Calculator) {
                out.inspector.detail1      = o.description;
                out.inspector.detail2      = "Enter copies " + o.actionCommand + " to the clipboard";
                out.inspector.shortcutHint = "Enter  Copy result";
            }
        } else if (ROW.app) {
            const auto LIVE = overviewWindowForApp(*ROW.app);
            out.inspector.category       = "Application";
            out.inspector.running        = !!LIVE;
            out.inspector.canNewInstance = !!LIVE;
            if (!ROW.app->comment.empty())
                out.inspector.subtitle = ROW.app->comment;
            else if (!ROW.app->generic.empty())
                out.inspector.subtitle = ROW.app->generic;
            if (LIVE) {
                out.inspector.status = "Open";
                if (!LIVE->m_title.empty())
                    out.inspector.detail1 = LIVE->m_title;
                if (LIVE->m_workspace)
                    out.inspector.detail2 = "Workspace " + std::to_string(LIVE->m_workspace->m_id);
                out.inspector.shortcutHint  = "Enter  Switch to window";
                out.inspector.shortcutHint2 = "Shift×2  New window";
            } else {
                out.inspector.status       = "Not running";
                out.inspector.detail1      = ROW.app->generic;
                out.inspector.shortcutHint = "Enter  Launch";
            }
            out.inspector.detail3 = ROW.app->id;
        } else {
            out.inspector.category     = "Search Provider";
            out.inspector.detail1      = ROW.provider.providerDesktopId;
            out.inspector.detail2      = ROW.provider.description;
            out.inspector.shortcutHint = "Enter  Open";
        }
    }

    return out;
}

// DRAW. Paints the layout and decides nothing: every box it uses was decided
// above, so the pixels and the hit boxes cannot drift apart.
// `part`: 0 everything, 1 the pill only, 2 the results body only (the pane's
// entrance/exit is motion-blurred on its own, the pill stays sharp).
// The output being drawn: drawSearch is handed a layout, not a monitor, and its
// border fades are per output (two monitors, two search pills).
static uint64_t renderMonitorId() {
    const auto MON = g_pHyprRenderer->m_renderData.pMonitor.lock();
    return MON ? sc<uint64_t>(MON->m_id) : 0xFFFF;
}

namespace {

// THE RESULTS EASE LIKE THE DOCK MENU (user, 2026-11-03: "add the same
// effects to the search results"). The row the selection is on -- hovered or
// arrowed to -- lifts: its fill fades in, and its icon and text slide a few px
// right and grow a touch; the row it left eases back. Wall-clock, like the
// letter bounce: drawSearch notes a change of row, the frame gates below ask
// searchHoverEasing() for frames while it runs.
    constexpr float SEARCH_HOVER_MS = 140.F;
    float           searchLitT() {
        const float T = std::clamp(SearchLetters::msSince(Time::steadyNow(), g_searchLitAt) / SEARCH_HOVER_MS, 0.F, 1.F);
        return T * T * (3.F - 2.F * T);
    }
    float searchLit(int index) {
        const float T = searchLitT();
        return (index == g_searchLitNow ? T : 0.F) + (index == g_searchLitPrev ? 1.F - T : 0.F);
    }
}
void searchPillPress(bool down) {
    if (down == g_pillPress.down)
        return;
    g_pillPress.down = down;
    if (down)
        g_pillPress.downAt = Time::steadyNow();
    g_pillPress.moving = true;
}
// The pill's tail fade returns this long after the pointer lets go of it.
constexpr float                       PILL_TAIL_RETURN_MS = 220.F;

namespace OverviewDetail {

bool pillPlaying() {
    return g_pillPress.moving || g_placeholderLetters.busy(Time::steadyNow()) ||
        (g_pillReleasedAt.time_since_epoch().count() != 0 && SearchLetters::msSince(Time::steadyNow(), g_pillReleasedAt) < PILL_TAIL_RETURN_MS + 20.F);
}

bool searchHoverEasing() {
    return g_searchLitAt.time_since_epoch().count() != 0 && SearchLetters::msSince(Time::steadyNow(), g_searchLitAt) < SEARCH_HOVER_MS + 20.F;
}
}

static void drawSearch(const SSearchLayout& L, bool pillLit, bool resultsLit, int part = 0) {
    if (!L.drawable)
        return;
    {
        int now = -1;
        for (const auto& CARD : L.cards)
            for (const auto& ITEM : CARD.items)
                if (ITEM.selected)
                    now = L.hits[ITEM.hit].index;
        for (const auto& E : L.tool.entries)
            if (E.selected)
                now = L.hits[E.hit].index;
        if (now != g_searchLitNow) {
            g_searchLitPrev = g_searchLitNow;
            g_searchLitNow  = now;
            g_searchLitAt   = Time::steadyNow();
        }
    }
    const bool DRAWPILL = part != 2;
    const bool DRAWBODY = part != 1;

    const float FADE  = L.fade;
    const float SCALE = L.scale;
    const float POWER = L.roundingPower;
    const CBox& PILL  = L.pill;
    const CBox& BADGE = L.badge;

    const auto roundFor = [&](const CBox& box) {
        return std::min(L.rounding, sc<int>(std::round(std::min(box.w, box.h) / 2.0)));
    };

    const int PILLROUND = roundFor(PILL);
    // The badge sits INSIDE the pill, so its corners are the pill's, inset:
    // the pill's radius less the gap between them, which keeps the two curves
    // parallel (user, 2026-10-14: a custom Hyprland rounding left the badge
    // rounder than the pill around it). At the default -- a fully round pill --
    // this is still a circle, so nothing changes there.
    const float INSET      = std::max(0.0, std::min(BADGE.x - PILL.x, BADGE.y - PILL.y));
    const int   BADGEROUND = std::clamp(sc<int>(std::round(PILLROUND - INSET)), 0, sc<int>(std::round(std::min(BADGE.w, BADGE.h) / 2.0)));
    const auto ACCENT    = ScrollOverview::Config::highlightColor();

    if (DRAWPILL)
        OverviewRender::themePlate(PILL, PILLROUND, POWER, FADE, false, pillLit, ScrollOverview::BorderFade::key("search-pill", renderMonitorId()));

    CRectPassElement::SRectData badgeBg;
    badgeBg.box           = BADGE;
    badgeBg.color         = ACCENT.modifyA(0.30F * FADE);
    badgeBg.round         = BADGEROUND;
    badgeBg.roundingPower = POWER;
    if (DRAWPILL)
        g_pHyprRenderer->addPassElement(makeUnique<CRectPassElement>(badgeBg));

    float      texFade = FADE;
    const auto ADDTEX  = [&](const auto& tex, const CBox& box) {
        CTexPassElement::SRenderData data;
        data.tex = tex;
        data.box = box;
        data.a   = texFade;
        g_pHyprRenderer->addPassElement(makeUnique<CTexPassElement>(data));
    };

    const auto GLYPHTEX = OverviewRender::text("\uF002", ScrollOverview::Config::highlightForeground().modifyA(0.92F), std::max(1, sc<int>(std::round(OVERVIEW_PILL_GLYPHPX * SCALE))), false,
                                                      "JetBrainsMono Nerd Font", 0);
    if (DRAWPILL && GLYPHTEX && GLYPHTEX->ok()) {
        CBox glyphBox = {
            BADGE.x + std::round((BADGE.width - sc<float>(GLYPHTEX->m_size.x)) / 2.F),
            BADGE.y + std::round((BADGE.height - sc<float>(GLYPHTEX->m_size.y)) / 2.F),
            sc<float>(GLYPHTEX->m_size.x),
            sc<float>(GLYPHTEX->m_size.y),
        };
        ADDTEX(GLYPHTEX, glyphBox.round());
    }

    if (DRAWBODY) {
    if (L.containerBox.width > 1.F) {
        OverviewRender::themePlate(L.containerBox, roundFor(L.containerBox), POWER, FADE, true, resultsLit, ScrollOverview::BorderFade::key("search-results", renderMonitorId()));

        for (const auto& divBox : L.dividers) {
            CRectPassElement::SRectData div;
            div.box   = divBox;
            div.color = ScrollOverview::Config::themeForeground().modifyA(0.12F * FADE);
            g_pHyprRenderer->addPassElement(makeUnique<CRectPassElement>(div));
        }
    }

    // THE TOOL PANE. Drawn on the container plate the cards would have used, in
    // the theme's own heading / foreground / muted colours, so the dictionary
    // and the translator read as the same surface as the results they replace.
    if (L.tool.active) {
        const CBox& HEAD   = L.tool.header;
        const auto  FGCOL  = ScrollOverview::Config::themeForeground();
        const auto  MUTED  = ScrollOverview::Config::themeMutedText();
        const auto  HEADCO = ScrollOverview::Config::themeHeading();

        // Glyph, headword, source -- and on the right, what Enter does.
        const char* GLYPH  = L.tool.kind == 1 ? "\uf02d" : "\uf1ab"; // book / language
        float       headX  = HEAD.x;
        if (const auto GTEX = OverviewRender::text(GLYPH, ACCENT, std::max(1, sc<int>(std::round(20.F * SCALE))), false, "JetBrainsMono Nerd Font", 0); GTEX && GTEX->ok()) {
            CBox g{HEAD.x, HEAD.y + std::round((HEAD.height - sc<float>(GTEX->m_size.y)) / 2.F), sc<float>(GTEX->m_size.x), sc<float>(GTEX->m_size.y)};
            ADDTEX(GTEX, g.round());
            headX += sc<float>(GTEX->m_size.x) + std::round(12.F * SCALE);
        }

        float hintW = 0.F;
        if (!L.tool.entries.empty() && !L.tool.entries.front().status) {
            if (const auto HINT = OverviewRender::text("\u21b5  copy", MUTED, std::max(1, sc<int>(std::round(11.F * SCALE))), false, "", 0, 500); HINT && HINT->ok()) {
                CBox h{HEAD.x + HEAD.width - sc<float>(HINT->m_size.x), HEAD.y + std::round((HEAD.height - sc<float>(HINT->m_size.y)) / 2.F), sc<float>(HINT->m_size.x),
                       sc<float>(HINT->m_size.y)};
                ADDTEX(HINT, h.round());
                hintW = sc<float>(HINT->m_size.x) + std::round(16.F * SCALE);
            }
        }

        const int  TITLEW = std::max(1, sc<int>(HEAD.x + HEAD.width - hintW - headX));
        const auto TITLE  = OverviewRender::text(L.tool.title, FGCOL, std::max(1, sc<int>(std::round(18.F * SCALE))), false, "", TITLEW, 600);
        const auto SUB    = OverviewRender::text(L.tool.subtitle, MUTED, std::max(1, sc<int>(std::round(11.F * SCALE))), false, "", TITLEW, 500);
        const float TH    = TITLE && TITLE->ok() ? sc<float>(TITLE->m_size.y) : 0.F;
        const float SH    = SUB && SUB->ok() ? sc<float>(SUB->m_size.y) : 0.F;
        float       ty    = HEAD.y + std::round((HEAD.height - TH - SH - (SH > 0 ? 2.F * SCALE : 0.F)) / 2.F);
        if (TH > 0) {
            ADDTEX(TITLE, CBox{headX, ty, sc<float>(TITLE->m_size.x), TH}.round());
            ty += TH + 2.F * SCALE;
        }
        if (SH > 0)
            ADDTEX(SUB, CBox{headX, ty, sc<float>(SUB->m_size.x), SH}.round());

        // The entries.
        const float TAGW   = L.tool.kind == 1 ? std::round(92.F * SCALE) : std::round(52.F * SCALE);
        const float ROWPAD = std::round(10.F * SCALE);
        for (const auto& E : L.tool.entries) {
            const CBox& ROW = L.hits[E.hit].box;

            if (const float LIT = searchLit(L.hits[E.hit].index); LIT > 0.01F) {
                CRectPassElement::SRectData sel;
                sel.box           = ROW;
                sel.color         = ACCENT.modifyA(0.22F * FADE * LIT);
                sel.round         = roundFor(ROW);
                sel.roundingPower = POWER;
                g_pHyprRenderer->addPassElement(makeUnique<CRectPassElement>(sel));
            }

            if (E.status) {
                if (const auto ST = OverviewRender::text(E.text, MUTED, std::max(1, sc<int>(std::round(13.F * SCALE))), false, "", sc<int>(ROW.width - 2.F * ROWPAD), 500);
                    ST && ST->ok())
                    ADDTEX(ST, CBox{ROW.x + ROWPAD, ROW.y + std::round((ROW.height - sc<float>(ST->m_size.y)) / 2.F), sc<float>(ST->m_size.x), sc<float>(ST->m_size.y)}.round());
                continue;
            }

            const float TEXTX = ROW.x + ROWPAD + TAGW;
            const int   TEXTW = std::max(16, sc<int>(ROW.x + ROW.width - ROWPAD - TEXTX));

            if (!E.tag.empty()) {
                if (const auto TG = OverviewRender::text(E.tag, E.selected ? FGCOL : HEADCO, std::max(1, sc<int>(std::round(10.F * SCALE))), false, "", sc<int>(TAGW - ROWPAD), 600);
                    TG && TG->ok())
                    ADDTEX(TG, CBox{ROW.x + ROWPAD, ROW.y + std::round(8.F * SCALE), sc<float>(TG->m_size.x), sc<float>(TG->m_size.y)}.round());
            }

            const int   TEXTPX = std::max(1, sc<int>(std::round(14.F * SCALE)));
            const int   EXPX   = std::max(1, sc<int>(std::round(11.F * SCALE)));
            const auto  EX     = E.example.empty() ? SP<Render::ITexture>{} :
                                                     OverviewRender::text("\u201c" + E.example + "\u201d", MUTED, EXPX, true, "", TEXTW, 400);
            const float EXH    = EX && EX->ok() ? sc<float>(EX->m_size.y) : 0.F;
            // The text wraps into whatever the example leaves of the row.
            const int   MAXH   = std::max(1, sc<int>(ROW.height - 2.F * std::round(5.F * SCALE) - EXH));
            if (const auto TX = OverviewRender::textWrapped(E.text, FGCOL, TEXTPX, std::max(16, ((TEXTW + 15) / 16) * 16), MAXH, false, "", 400); TX && TX->ok()) {
                const float TXH = sc<float>(TX->m_size.y);
                const float TOP = ROW.y + std::round((ROW.height - TXH - EXH) / 2.F);
                ADDTEX(TX, CBox{TEXTX, TOP, sc<float>(TX->m_size.x), TXH}.round());
                if (EXH > 0)
                    ADDTEX(EX, CBox{TEXTX, TOP + TXH, sc<float>(EX->m_size.x), EXH}.round());
            }
        }
    }

    for (const auto& CARD : L.cards) {
        texFade = FADE * CARD.alpha;

        // Header at top of column
        const float PADL       = std::round(8.F * SCALE);
        float       headIndent = PADL;

        const auto  HEADICON   = OverviewRender::appIcon(CARD.icon);
        const float HEAD_ICOND = std::round(16.F * SCALE);
        if (HEADICON && HEADICON->ok()) {
            CBox iconBox{CARD.box.x + headIndent, CARD.box.y + PADL + std::round((18.F * SCALE - HEAD_ICOND) / 2.F), HEAD_ICOND, HEAD_ICOND};
            ADDTEX(HEADICON, iconBox.round());
            headIndent += HEAD_ICOND + std::round(8.F * SCALE);
        }

        const int  HEADPX  = std::max(1, sc<int>(std::round(12.F * SCALE)));
        const auto HEADTEX = OverviewRender::text(CARD.title, ScrollOverview::Config::themeHeading(), HEADPX, false, "", std::max(1, sc<int>(CARD.box.width - headIndent - PADL)), 600);
        if (HEADTEX && HEADTEX->ok()) {
            CBox headText{CARD.box.x + headIndent, CARD.box.y + PADL, sc<float>(HEADTEX->m_size.x), sc<float>(HEADTEX->m_size.y)};
            ADDTEX(HEADTEX, headText.round());
        }

        if (CARD.more > 0) {
            const int   MOREPX  = std::max(1, sc<int>(std::round(11.F * SCALE)));
            std::string moreStr = "+" + std::to_string(CARD.more);
            const auto  MORETEX = OverviewRender::text(moreStr, ScrollOverview::Config::themeMutedText(), MOREPX, false, "", 100, 500);
            if (MORETEX && MORETEX->ok()) {
                CBox moreBox{(CARD.box.x + CARD.box.width) - PADL - sc<float>(MORETEX->m_size.x), CARD.box.y + PADL, sc<float>(MORETEX->m_size.x), sc<float>(MORETEX->m_size.y)};
                ADDTEX(MORETEX, moreBox.round());
            }
        }

        // Rows
        for (const auto& ITEM : CARD.items) {
            const CBox& ROW = L.hits[ITEM.hit].box;
            const float LIT = searchLit(L.hits[ITEM.hit].index);
            // The lift: ITEMSHIFT to the right, ITEMGROW about the content's
            // own centre line (GROWBOX), the fill fading in under it.
            const float ITEMSHIFT = std::round(4.F * SCALE * LIT);
            const float ITEMGROW  = 1.F + 0.035F * LIT;
            const auto  GROWBOX   = [&](CBox b) {
                const double CY = b.y + b.height / 2.0;
                b.x += ITEMSHIFT;
                b.width *= ITEMGROW;
                b.height *= ITEMGROW;
                b.y = CY - b.height / 2.0;
                return b;
            };

            if (LIT > 0.01F) {
                CRectPassElement::SRectData sel;
                sel.box           = ROW;
                const auto SELFILL = ScrollOverview::Config::themeSelectedFill();
                sel.color          = SELFILL.modifyA(SELFILL.a * FADE * CARD.alpha * LIT);
                sel.round         = roundFor(ROW);
                sel.roundingPower = POWER;
                g_pHyprRenderer->addPassElement(makeUnique<CRectPassElement>(sel));
            }

            const float ROWPAD     = std::round(6.F * SCALE);
            float       textIndent = ROWPAD;

            const float ICOND = std::round(18.F * SCALE);
            if (OmarchySearch::isGlyphIcon(ITEM.icon)) {
                const int         GLYPHPX = std::max(1, sc<int>(std::round(16.F * SCALE)));
                const std::string FAMILY  = ITEM.iconFont.empty() ? "JetBrainsMono Nerd Font" : ITEM.iconFont;
                const auto        GLYPHTEX =
                    OverviewRender::text(ITEM.icon, ITEM.selected ? ScrollOverview::Config::themeSelectedText() : ScrollOverview::Config::themeForeground(), GLYPHPX, false,
                                         FAMILY, sc<int>(ICOND + std::round(8.F * SCALE)), 400);
                if (GLYPHTEX && GLYPHTEX->ok()) {
                    CBox glyphBox{ROW.x + ROWPAD, ROW.y + std::round((ROW.height - sc<float>(GLYPHTEX->m_size.y)) / 2.F), sc<float>(GLYPHTEX->m_size.x),
                                  sc<float>(GLYPHTEX->m_size.y)};
                    ADDTEX(GLYPHTEX, GROWBOX(glyphBox));
                    textIndent += std::max(ICOND, sc<float>(GLYPHTEX->m_size.x)) + std::round(8.F * SCALE);
                }
            } else if (const auto ICONTEX = OverviewRender::appIcon(ITEM.icon); ICONTEX && ICONTEX->ok()) {
                CBox iconBox{ROW.x + ROWPAD, ROW.y + std::round((ROW.height - ICOND) / 2.F), ICOND, ICOND};
                ADDTEX(ICONTEX, GROWBOX(iconBox));
                textIndent += ICOND + std::round(8.F * SCALE);
            }

            const int  ROWPX  = std::max(1, sc<int>(std::round(13.F * SCALE)));
            const auto FG     = ITEM.selected ? ScrollOverview::Config::themeSelectedText() : ScrollOverview::Config::themeForeground();
            const auto ROWTEX = OverviewRender::text(ITEM.name, FG, ROWPX, false, "", std::max(1, sc<int>(ROW.width - textIndent) - sc<int>(ROWPAD)), 500);
            if (ROWTEX && ROWTEX->ok()) {
                CBox rowText{ROW.x + textIndent, ROW.y + std::round((ROW.height - sc<float>(ROWTEX->m_size.y)) / 2.F), sc<float>(ROWTEX->m_size.x), sc<float>(ROWTEX->m_size.y)};
                // The icon grew by (ITEMGROW - 1) of its width: the name moves
                // over with it, so the two do not touch.
                ADDTEX(ROWTEX, GROWBOX(CBox{rowText.x + ICOND * (ITEMGROW - 1.F), rowText.y, rowText.width, rowText.height}));

                if (!ITEM.description.empty()) {
                    const float DESCX = rowText.x + rowText.width + std::round(8.F * SCALE);
                    const int   DESCW = sc<int>((ROW.x + ROW.width - ROWPAD) - DESCX);
                    if (DESCW > sc<int>(std::round(24.F * SCALE))) {
                        const auto DESCFG = ITEM.selected ? FG : ScrollOverview::Config::themeMutedText();
                        const int  DESCPX = std::max(1, sc<int>(std::round(11.F * SCALE)));
                        if (const auto DESCTEX = OverviewRender::text(ITEM.description, DESCFG, DESCPX, false, "", DESCW); DESCTEX && DESCTEX->ok()) {
                            CBox descText{DESCX, ROW.y + std::round((ROW.height - sc<float>(DESCTEX->m_size.y)) / 2.F), sc<float>(DESCTEX->m_size.x), sc<float>(DESCTEX->m_size.y)};
                            // Past the grown name, on the same lift.
                            ADDTEX(DESCTEX, GROWBOX(CBox{descText.x + ICOND * (ITEMGROW - 1.F) + rowText.width * (ITEMGROW - 1.F), descText.y, descText.width, descText.height}));
                        }
                    }
                }
            }
        }

        if (CARD.items.empty()) {
            const int  EMPTYPX  = std::max(1, sc<int>(std::round(12.F * SCALE)));
            const auto EMPTYTEX = OverviewRender::text(CARD.emptyHint.empty() ? "No items" : CARD.emptyHint, ScrollOverview::Config::themeMutedText(), EMPTYPX, false, "", sc<int>(CARD.box.width - PADL * 2.F), 400);
            if (EMPTYTEX && EMPTYTEX->ok()) {
                CBox emptyBox{CARD.box.x + PADL, CARD.box.y + PADL + std::round(30.F * SCALE), sc<float>(EMPTYTEX->m_size.x), sc<float>(EMPTYTEX->m_size.y)};
                ADDTEX(EMPTYTEX, emptyBox.round());
            }
        }
    }

    // Draw Inspector Card (inside unified block on the right)
    if (L.inspector.visible) {
        texFade = FADE * L.inspector.alpha;
        const CBox& IBOX = L.inspector.box;
        const float PADL  = std::round(14.F * SCALE);
        const float ICOND = std::round(48.F * SCALE);
        const float CLIPB = IBOX.y + IBOX.height - PADL;

        const auto ADDINSP = [&](const SP<Render::ITexture>& tex, CBox box) {
            if (!tex || !tex->ok() || box.width < 1.F)
                return;
            CTexPassElement::SRenderData data;
            data.tex     = tex;
            data.box     = box;
            data.a       = texFade;
            data.clipBox = IBOX;
            g_pHyprRenderer->addPassElement(makeUnique<CTexPassElement>(data));
        };

        const auto ICONTEX = OverviewRender::appIcon(L.inspector.icon);
        if (ICONTEX && ICONTEX->ok()) {
            CBox iconBox{IBOX.x + PADL, IBOX.y + PADL, ICOND, ICOND};
            ADDINSP(ICONTEX, iconBox.round());
        } else if (!L.inspector.icon.empty()) {
            const int  GLYPHPX = std::max(1, sc<int>(std::round(28.F * SCALE)));
            const auto GLYPHTEX = OverviewRender::text(L.inspector.icon, ScrollOverview::Config::themeHeading(), GLYPHPX, false, "", sc<int>(ICOND), 400);
            if (GLYPHTEX && GLYPHTEX->ok()) {
                CBox glyphBox{IBOX.x + PADL, IBOX.y + PADL, sc<float>(GLYPHTEX->m_size.x), sc<float>(GLYPHTEX->m_size.y)};
                ADDINSP(GLYPHTEX, glyphBox.round());
            }
        }

        float textY = IBOX.y + PADL;
        const float TEXTX = IBOX.x + PADL + ICOND + std::round(12.F * SCALE);
        const int   TEXTW = std::max(1, sc<int>(IBOX.width - (TEXTX - IBOX.x) - PADL));
        const int   FULLW = std::max(1, sc<int>(IBOX.width - (PADL * 2.F)));

        const auto paintWrap = [&](const std::string& s, const CHyprColor& col, int px, int weight, float x, int w, int maxH, float gapAfter) {
            if (s.empty() || textY + sc<float>(px) > CLIPB)
                return;
            const int W = std::max(16, ((w + 15) / 16) * 16);
            const auto TEX = OverviewRender::textWrapped(s, col, px, W, maxH, false, "", weight);
            if (!TEX || !TEX->ok())
                return;
            CBox box{x, textY, sc<float>(TEX->m_size.x), sc<float>(TEX->m_size.y)};
            ADDINSP(TEX, box.round());
            textY += sc<float>(TEX->m_size.y) + gapAfter;
        };

        if (!L.inspector.category.empty()) {
            auto catCol = ScrollOverview::Config::themeHeading();
            paintWrap(L.inspector.category, catCol, std::max(1, sc<int>(std::round(10.F * SCALE))), 600, TEXTX, TEXTW, sc<int>(18.F * SCALE), std::round(3.F * SCALE));
        }
        if (!L.inspector.status.empty()) {
            const auto STCOL = L.inspector.running ? ScrollOverview::Config::themeHeading() : ScrollOverview::Config::themeMutedText();
            paintWrap(L.inspector.status, STCOL, std::max(1, sc<int>(std::round(11.F * SCALE))), 600, TEXTX, TEXTW, sc<int>(18.F * SCALE), std::round(3.F * SCALE));
        }
        paintWrap(L.inspector.title, ScrollOverview::Config::themeForeground(), std::max(1, sc<int>(std::round(15.F * SCALE))), 600, TEXTX, TEXTW, sc<int>(40.F * SCALE),
                  std::round(4.F * SCALE));

        textY = std::max(textY, sc<float>(IBOX.y + PADL + ICOND)) + std::round(10.F * SCALE);

        // THE SUBTITLE GETS WHAT THE CARD HAS LEFT (user, 2026-09-24: "wrap
        // needs to scale with panel size"). A fixed line budget cut a definition
        // off with most of the card still empty. So the details and the hints at
        // the foot are measured first, and the subtitle may fill everything
        // between the header and them -- a tall card shows the whole answer, a
        // short one ellipsizes on the last line that fits.
        const auto DETCOL  = ScrollOverview::Config::themeMutedText();
        const int  DETPX   = std::max(1, sc<int>(std::round(11.F * SCALE)));
        const int  DET3PX  = std::max(1, sc<int>(std::round(10.F * SCALE)));
        const int  HINTPX  = std::max(1, sc<int>(std::round(11.F * SCALE)));
        const auto HINTCOL = ScrollOverview::Config::themeMutedText();
        const int  WRAPW   = std::max(16, ((FULLW + 15) / 16) * 16);
        const auto measure = [&](const std::string& str, const CHyprColor& col, int px, int weight, int maxH, float gapAfter) -> float {
            if (str.empty())
                return 0.F;
            const auto TEX = OverviewRender::textWrapped(str, col, px, WRAPW, maxH, false, "", weight);
            return (TEX && TEX->ok()) ? sc<float>(TEX->m_size.y) + gapAfter : 0.F;
        };
        const int DET1H = sc<int>(40.F * SCALE), DET2H = sc<int>(44.F * SCALE), DET3H = sc<int>(36.F * SCALE), HINTH = sc<int>(36.F * SCALE);
        const float BELOW = measure(L.inspector.detail1, DETCOL, DETPX, 400, DET1H, std::round(6.F * SCALE)) +
            measure(L.inspector.detail2, DETCOL, DETPX, 400, DET2H, std::round(6.F * SCALE)) + measure(L.inspector.detail3, DETCOL, DET3PX, 400, DET3H, std::round(8.F * SCALE)) +
            measure(L.inspector.shortcutHint2, HINTCOL, HINTPX, 500, HINTH, std::round(4.F * SCALE)) +
            measure(L.inspector.shortcutHint, HINTCOL, HINTPX, 500, HINTH, std::round(4.F * SCALE)) + std::round(16.F * SCALE);
        const int SUBH = std::max(sc<int>(48.F * SCALE), sc<int>(CLIPB - textY - BELOW));

        paintWrap(L.inspector.subtitle, ScrollOverview::Config::themeMutedText(), std::max(1, sc<int>(std::round(12.F * SCALE))), 400, IBOX.x + PADL, FULLW, SUBH,
                  std::round(8.F * SCALE));
        paintWrap(L.inspector.detail1, DETCOL, DETPX, 400, IBOX.x + PADL, FULLW, DET1H, std::round(6.F * SCALE));
        paintWrap(L.inspector.detail2, DETCOL, DETPX, 400, IBOX.x + PADL, FULLW, DET2H, std::round(6.F * SCALE));
        paintWrap(L.inspector.detail3, DETCOL, DET3PX, 400, IBOX.x + PADL, FULLW, DET3H, std::round(8.F * SCALE));

        float hintY = CLIPB;
        const auto measureHint = [&](const std::string& s) -> float {
            if (s.empty())
                return 0.F;
            const int W = std::max(16, ((FULLW + 15) / 16) * 16);
            const auto TEX = OverviewRender::textWrapped(s, HINTCOL, HINTPX, W, sc<int>(36.F * SCALE), false, "", 500);
            return (TEX && TEX->ok()) ? sc<float>(TEX->m_size.y) + std::round(4.F * SCALE) : 0.F;
        };
        hintY -= measureHint(L.inspector.shortcutHint2);
        hintY -= measureHint(L.inspector.shortcutHint);
        if (hintY > textY + std::round(8.F * SCALE))
            textY = hintY;

        paintWrap(L.inspector.shortcutHint, HINTCOL, HINTPX, 500, IBOX.x + PADL, FULLW, sc<int>(36.F * SCALE), std::round(4.F * SCALE));
        paintWrap(L.inspector.shortcutHint2, HINTCOL, HINTPX, 500, IBOX.x + PADL, FULLW, sc<int>(36.F * SCALE), 0.F);
    }
    } // DRAWBODY

    if (!DRAWPILL)
        return;

    texFade = FADE;
    const CHyprColor FG = L.placeholder ? ScrollOverview::Config::themeMutedText() : (L.emptyResult ? ScrollOverview::Config::themeUrgent() : ScrollOverview::Config::themeForeground());

    const int  FONTPX = std::max(1, sc<int>(std::round(OVERVIEW_PILL_TEXTPX * SCALE)));
    const auto NOW    = Time::steadyNow();

    // Snapped for the CLIP box (a chrome edge), exact for the type in it.
    const float FIELDX = L.fieldXExact > 0.F ? L.fieldXExact : BADGE.x + BADGE.width + L.gap;
    const CBox  FIELD{std::round(FIELDX), PILL.y, L.fieldW, PILL.height};

    const auto ADDLETTER = [&](const SP<Render::ITexture>& tex, const CBox& box, float a) {
        if (!tex || !tex->ok() || a <= 0.F)
            return;
        CTexPassElement::SRenderData data;
        data.tex     = tex;
        data.box     = box;
        data.a       = texFade * a;
        data.clipBox = FIELD;
        g_pHyprRenderer->addPassElement(makeUnique<CTexPassElement>(data));
    };

    // Nothing measured is nothing to fit: the placeholder keeps the rest width.
    auto& FIELDMOTION = g_search.fieldOn(L.monitorId);
    FIELDMOTION.contentL = 0.F;
    FIELDMOTION.measured = true;

    if (!SearchLetters::g_searchLetters.empty()) {
        g_placeholderLetters.reset(); // a query took over: the placeholder starts at rest next time
        // Serpantinum Input.qml: one texture per codepoint, scale/y/opacity from
        // the add/remove transitions, packed left with 1 logical px of tracking.
        const auto  LIVE    = SearchLetters::g_searchLetters.liveGlyphs();
        const float SPACING = std::max(1.F, SCALE);
        std::vector<float> widths;
        widths.reserve(LIVE.size());
        for (const auto& G : LIVE) {
            const auto TEX = OverviewRender::text(G, FG, FONTPX, false, "", 0, 600);
            widths.push_back((TEX && TEX->ok()) ? sc<float>(TEX->m_size.x) : sc<float>(FONTPX) * 0.5F);
        }

        // What the field has to fit, handed back to next frame's springs: the
        // LIVE glyphs only (a backspaced letter flying off must not hold the pill
        // open), plus the "no matches" suffix while it is showing.
        const auto  SUFFIX    = L.emptyResult && !LIVE.empty() ? OverviewRender::text("    no matches", FG, FONTPX, false, "", 0, 600) : SP<Render::ITexture>{};
        const float LIVEENDPX = SearchLetters::g_searchLetters.liveEndX(widths, SPACING);
        FIELDMOTION.contentL = (LIVEENDPX + (SUFFIX && SUFFIX->ok() ? sc<float>(SUFFIX->m_size.x) : 0.F)) / std::max(SCALE, 0.01F);

        // Past the cap the whole run slides left by the scroll, and the glyphs
        // nearest the field's left edge fade out on a smoothstep over FADEL. The
        // fade is itself eased in by how far the text has scrolled, so it grows
        // as scrolling begins instead of popping on at the first pixel.
        const float SCROLLPX  = std::round(FIELDMOTION.drawScrollL * SCALE);
        const float FADEPX    = std::max(1.F, OVERVIEW_PILL_FADEL * SCALE);
        const float FADEON    = std::clamp(SCROLLPX / FADEPX, 0.F, 1.F);
        const auto  EDGEALPHA = [&](float centreX) {
            if (FADEON <= 0.F)
                return 1.F;
            const float T      = std::clamp((centreX - FIELDX) / FADEPX, 0.F, 1.F);
            const float SMOOTH = T * T * (3.F - 2.F * T);
            return 1.F - FADEON * (1.F - SMOOTH);
        };

        const auto PLACED = SearchLetters::g_searchLetters.place(widths, SPACING, SCALE, NOW, sc<int>(L.monitorId));
        if (Prof::enabled()) {
            Prof::retargets += SearchLetters::g_searchLetters.lastRetargets;
            Prof::pillScale = SCALE;
            Prof::fieldWL   = FIELDMOTION.drawWidthL;
        }
        for (const auto& P : PLACED) {
            const auto TEX = OverviewRender::text(P.glyph, FG, FONTPX, false, "", 0, 600);
            if (!TEX || !TEX->ok())
                continue;
            const float W  = sc<float>(TEX->m_size.x);
            const float H  = sc<float>(TEX->m_size.y);
            const float CX = FIELDX - SCROLLPX + P.x + W * 0.5F;
            const float CY = PILL.y + std::round((PILL.height - H) / 2.F) + H * 0.5F + P.y;
            const float DW = W * P.scale;
            const float DH = H * P.scale;
            ADDLETTER(TEX, CBox{CX - DW * 0.5F, CY - DH * 0.5F, DW, DH}, P.opacity * EDGEALPHA(CX));
        }

        if (SUFFIX) {
            if (SUFFIX->ok()) {
                const float X = FIELDX - SCROLLPX + LIVEENDPX;
                CBox        suffixBox{X, PILL.y + std::round((PILL.height - sc<float>(SUFFIX->m_size.y)) / 2.F), sc<float>(SUFFIX->m_size.x), sc<float>(SUFFIX->m_size.y)};
                ADDLETTER(SUFFIX, suffixBox, 1.F);
            }
        }
        return;
    }

    // THE PLACEHOLDER TYPES ITSELF (user, 2026-11-03). At rest it shows the
    // letters that fit, the last one fading out at the field's end the way a
    // long query fades at its start. Hovered, it types the rest in -- the field
    // widening with it on the query's own width spring -- and erases it again
    // on leaving; pressed, it erases itself and types "Just type something!".
    // A pill opened at rest does not type itself in: those letters are set as
    // already arrived.
    if (L.placeholder) {
        static const std::string FULL = "Search, calculate or run", HINT = "Just type something!";
        const float              SPACING = std::max(1.F, SCALE);
        const float              FADEPX  = std::max(1.F, OVERVIEW_PILL_FADEL * SCALE);
        const auto               glyphW  = [&](const std::string& g) {
            const auto T = OverviewRender::text(g, FG, FONTPX, false, "", 0, 0);
            return (T && T->ok()) ? sc<float>(T->m_size.x) : sc<float>(FONTPX) * 0.5F;
        };
        std::string want;
        float       restFitEnd = 0.F; // at rest: where the letters that fit end (the field fits these, not the faded rest of the word)
        if (g_pillPress.down)
            want = HINT;
        else if (g_pillHovered)
            want = FULL;
        else {
            // The rest text: whole letters up to the field's rest width, plus the
            // one the end fade softens -- and then the rest of that word, so it
            // reads "run", not "ru" (user, 2026-11-06: "might as well add the n
            // since it fades away"). Those letters lie past the tail fade, so
            // they are not seen, and the field does not widen for them.
            const float REST = OVERVIEW_PILL_FIELDL * SCALE + FADEPX * 0.5F;
            float       x    = 0.F;
            bool        over = false;
            for (const auto& G : SearchLetters::codepoints(FULL)) {
                const float W = glyphW(G);
                if (!over && x + W > REST)
                    over = true;
                if (over && G == " ")
                    break;
                want += G;
                if (!over)
                    restFitEnd = x + W;
                x += W + SPACING;
            }
        }
        static std::string lastWant; // what the previous frame asked for: releasing the hint clears it in one go
        static bool        lastRest = false; // the previous frame showed the rest text
        static bool        wasEngaged = false;
        static SearchLetters::clock::time_point engagedAt{};
        const bool ENGAGED = g_pillPress.down || g_pillHovered;
        if (ENGAGED && !wasEngaged)
            engagedAt = NOW;
        else if (!ENGAGED && wasEngaged)
            g_pillReleasedAt = NOW;
        wasEngaged = ENGAGED;
        auto& A          = g_placeholderLetters;
        const bool FRESH = A.empty();
        // HOVER RETYPES THE FADED TAIL (user, 2026-11-03): at rest "or run"
        // dissolves into the field's edge; hovering backspaces those faded
        // words and types them back in full, rather than only adding the
        // letters that did not fit.
        if (!FRESH && g_pillHovered && !g_pillPress.down && lastRest) {
            const auto TAIL = FULL.rfind(' ', FULL.rfind(' ') - 1);
            A.sync(FULL.substr(0, TAIL + 1), NOW);
        }
        A.sync(want, NOW);
        if (FRESH && !g_pillPress.down && !g_pillHovered) {
            for (auto& LT : A.letters)
                LT.bornAt = NOW - std::chrono::seconds(5); // already there: no type-in on open
        } else {
            // Typed, not pasted: what this sync took away is backspaced from the
            // end, quickly, and only THEN is the new text typed in a letter at a
            // time -- erasing and typing at once put two words on top of each
            // other mid-swap.
            constexpr auto TYPE  = std::chrono::milliseconds(22);
            constexpr auto ERASE = std::chrono::milliseconds(12);
            std::vector<SearchLetters::SLetter*> dying, born;
            for (auto& LT : A.letters) {
                if (LT.bornAt == NOW && !LT.dyingAt)
                    born.push_back(&LT);
                if (LT.dyingAt && *LT.dyingAt == NOW)
                    dying.push_back(&LT);
            }
            // The press and its release are not typing (user, 2026-11-03): the
            // text goes in one go, as if cleared, and the hint arrives in one
            // go, as if pasted. Only hover types and backspaces.
            const bool ONEGO = g_pillPress.down || lastWant == HINT;
            for (size_t k = 0; k < dying.size(); ++k)
                dying[k]->dyingAt = ONEGO ? NOW : NOW + ERASE * sc<int>(dying.size() - 1 - k);
            const auto AFTER = dying.empty() ? std::chrono::steady_clock::duration{} :
                                               std::chrono::duration_cast<std::chrono::steady_clock::duration>((ONEGO ? ERASE * 0 : ERASE * sc<int>(dying.size())) +
                                                                                                         std::chrono::milliseconds(sc<int>(SearchLetters::EXIT_MS * 0.6F)));
            for (size_t k = 0; k < born.size(); ++k)
                born[k]->bornAt = NOW + AFTER + (g_pillPress.down ? TYPE * 0 : TYPE * sc<int>(k));
        }

        lastWant = want;
        lastRest = !g_pillPress.down && !g_pillHovered;
        const auto         LIVE = A.liveGlyphs();
        std::vector<float> widths;
        widths.reserve(LIVE.size());
        for (const auto& G : LIVE)
            widths.push_back(glyphW(G));

        const float FIELDRIGHT = FIELDX + L.fieldW;
        const auto  ENDALPHA   = [&](float rightX) {
            const float T = std::clamp((FIELDRIGHT - rightX + FADEPX * 0.5F) / FADEPX, 0.F, 1.F);
            return T * T * (3.F - 2.F * T);
        };
        // THE TAIL FADE (user, 2026-11-03: "make the tail fade so 'or run'
        // fades away"): while the whole text does not fit the field, its last
        // stretch dissolves to nothing AT the field's edge instead of being cut
        // there. Weighted by how much of the text is still out of the field, so
        // as the hovered field widens to fit it the tail comes back to full.
        // Where the fade runs, in the rest layout: full at the start of "or",
        // gone just past the last letter the rest field holds.
        float tailFrom = 0.F, tailTo = 0.F;
        {
            const size_t TAILAT = FULL.rfind(' ', FULL.rfind(' ') - 1) + 1;
            const float  RESTW  = OVERVIEW_PILL_FIELDL * SCALE + FADEPX * 0.5F;
            float        x      = 0.F;
            size_t       byte   = 0;
            for (const auto& G : SearchLetters::codepoints(FULL)) {
                const float W = glyphW(G);
                if (byte == TAILAT)
                    tailFrom = x;
                if (x + W > RESTW)
                    break;
                tailTo = x + W * 2.F;
                x += W + SPACING;
                byte += G.size();
            }
        }
        // Engaged (hovered or held), the letters typed since are whole and the
        // faded ones on their way out stay faded; let go, the tail fades back.
        const auto TAILWEIGHT = [&](SearchLetters::clock::time_point bornAt) {
            if (ENGAGED)
                return bornAt < engagedAt ? 1.F : 0.F;
            return g_pillReleasedAt.time_since_epoch().count() == 0 ? 1.F :
                                                                       std::clamp(SearchLetters::msSince(NOW, g_pillReleasedAt) / PILL_TAIL_RETURN_MS, 0.F, 1.F);
        };
        const auto  TAILALPHA  = [&](float centreX, SearchLetters::clock::time_point bornAt) {
            if (tailTo <= tailFrom)
                return 1.F;
            // An even ramp, by each letter's middle: "o" barely touched, "u" a ghost.
            const float T = 1.F - std::clamp((centreX - FIELDX - tailFrom) / (tailTo - tailFrom), 0.F, 1.F);
            return 1.F + (T - 1.F) * TAILWEIGHT(bornAt);
        };
        float shownEnd = 0.F; // the right edge of what is on screen so far, for the field to fit
        for (const auto& P : A.place(widths, SPACING, SCALE, NOW, sc<int>(L.monitorId) + 4096)) {
            const auto TEX = OverviewRender::text(P.glyph, FG, FONTPX, false, "", 0, 0);
            if (!TEX || !TEX->ok())
                continue;
            const float W  = sc<float>(TEX->m_size.x);
            if (P.opacity > 0.3F)
                shownEnd = std::max(shownEnd, P.x + W);
            const float H  = sc<float>(TEX->m_size.y);
            const float CX = FIELDX + P.x + W * 0.5F;
            const float CY = PILL.y + std::round((PILL.height - H) / 2.F) + H * 0.5F + P.y;
            const float DW = W * P.scale;
            const float DH = H * P.scale;
            ADDLETTER(TEX, CBox{CX - DW * 0.5F, CY - DH * 0.5F, DW, DH}, P.opacity * ENDALPHA(FIELDX + P.x + W) * TAILALPHA(FIELDX + P.x + W * 0.5F, P.bornAt));
        }
        // The field fits the letters as they arrive and leave while hovered or
        // held (the query's width spring); at rest it keeps its rest width.
        // Held, the pill is squeezed AROUND this field: aimed wide enough that
        // the hint still fits at the tightest squeeze (a constant, so the
        // spring is not re-aimed every frame of the hold).
        // AT REST the text is measured from its layout, not from where its
        // letters are drawn: the letters glide to their places in device px
        // while the overview's opening zoom changes the pill's scale under them,
        // so drawn-px / scale swelled ~12% and fell back, and the field sprang
        // after it (user, 2026-11-06: "remove the overshoot of the search bar
        // when overview opens"). Only while letters are still arriving or
        // leaving does the field follow what is on screen.
        if (!ENGAGED && LIVE.size() == SearchLetters::codepoints(want).size()) {
            float laidOut = 0.F;
            for (const float W : widths)
                laidOut += W + SPACING;
            shownEnd = std::min(std::max(0.F, laidOut - SPACING), restFitEnd);
        }
        if (g_pillPress.down || g_pillHovered || shownEnd > OVERVIEW_PILL_FIELDL * SCALE + 0.5F)
            FIELDMOTION.contentL = (g_pillHovered && !g_pillPress.down ? std::max(shownEnd, OVERVIEW_PILL_FIELDL * SCALE) : shownEnd) / std::max(SCALE, 0.01F) /
                (g_pillPress.down ? 1.F - SPillPress::PRESS_MAX : 1.F);
        return;
    }
    const auto TEX = OverviewRender::text(L.label, FG, FONTPX, false, "", sc<int>(L.fieldW), L.placeholder ? 0 : 600);
    if (!TEX || !TEX->ok())
        return;

    CBox textBox = {
        FIELDX,
        PILL.y + std::round((PILL.height - sc<float>(TEX->m_size.y)) / 2.F),
        sc<float>(TEX->m_size.x),
        sc<float>(TEX->m_size.y),
    };
    ADDTEX(TEX, textBox.round());
}

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
MONITORID CScrollOverview::searchMonitorId() const {
    const auto MONITOR = pMonitor.lock();
    return MONITOR ? MONITOR->m_id : MONITOR_INVALID;
}

SSearchLayout& CScrollOverview::searchLayoutHere() const {
    return g_search.layoutOn(searchMonitorId());
}

void CScrollOverview::renderSearch(PHLMONITOR monitor, float renderScale) {
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
    const float TARGET = std::max(PANEL ? overviewResultsBandTarget(monitor, SECTIONS, GRID_TILES, TOOLPANE, toolRowsHL) : 0.F, ScrollOverview::KeyboardCat::bandL());
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
            OmarchySearch::updateSushiIfActive(RESULTS[g_search.nav.selection].omarchy->filePath);
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
            const CBox  PREV = ScrollOverview::FlightBlur::shape(ScrollOverview::FlightBlur::ESite::SEARCH_PANE, PREVBOX, SL.containerBox, PCT);
            blurred = ScrollOverview::FlightBlur::drawBlurred(monitor, nullptr, PREV, SL.containerBox, [&] { drawSearch(SL, PILLLIT, BODYLIT, 2); },
                                                              ScrollOverview::FlightBlur::layoutSamples());
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
        ScrollOverview::KeyboardCat::render(monitor, SL.pill, SL.scale, SL.fade, SL.rounding, SL.roundingPower, SL.bandH);
    }

    // The letter bounce is timestamped, not a PHLANIMVAR, so the compositor
    // will not schedule the rest of the pop on its own.
    if (SearchLetters::g_searchLetters.busy(Time::steadyNow()) || ScrollOverview::KeyboardCat::busy()) {
        damage();
        requestInputFrame();
    }
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
    OmarchySearch::toggleSushi(*PATH);
    return true;
}

bool overviewHandleSearchSpace() {
    const auto RESULTS = overviewSearchRows();
    const std::string* HOVER = searchPreviewPath(RESULTS, g_search.nav.hoverIndex);
    const std::string* SEL   = searchPreviewPath(RESULTS, g_search.nav.selection);
    if (!overviewSpacePreviews(g_search.nav.intent, HOVER != nullptr, SEL != nullptr, g_search.nav.explicitSelection)) {
        if (OmarchySearch::isSushiActive())
            OmarchySearch::closeSushi();
        return false;
    }

    const std::string* PATH = (g_search.nav.intent == ESearchIntent::Hovering && HOVER) ? HOVER : SEL;
    if (!PATH)
        return false;
    OmarchySearch::toggleSushi(*PATH);
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

    AppSearch::launch(*RESULTS[sc<size_t>(idx)].app, ScrollOverview::Config::getTerminal());
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

    const auto agentAt = [&](int idx) -> std::optional<OmarchySearch::SSearchItem> {
        if (idx < 0 || idx >= sc<int>(RESULTS.size()) || !RESULTS[sc<size_t>(idx)].omarchy)
            return std::nullopt;
        const auto& ITEM = *RESULTS[sc<size_t>(idx)].omarchy;
        return OmarchySearch::canBeDefaultAgent(ITEM) ? std::optional{ITEM} : std::nullopt;
    };

    auto item = g_search.nav.intent == ESearchIntent::Hovering ? agentAt(g_search.nav.hoverIndex) : std::nullopt;
    if (!item)
        item = agentAt(g_search.nav.selection);
    if (!item || !OmarchySearch::setDefaultAgent(*item))
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
