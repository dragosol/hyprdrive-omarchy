// Search UI: layout, draw, the pill/row-lift animation, and the debug report.
// Cut from Overview.cpp, then from OverviewSearchView.cpp (the render step and
// the key taps went to overview/OverviewSearchStep.cpp), by pure code motion.
#include "OverviewParts.hpp"
#include "SearchView.hpp"
#include "Rows.hpp"
#include "MotionCurves.hpp"

using namespace OverviewDetail;


// LAYOUT. Monitor, animation values, the keyboard's zone and the section set
// in, boxes out. It reads no search state and no member and writes nothing --
// which is exactly what lets the painter, the pointer hit-test and the keyboard
// walk all be handed the same answer instead of each deriving its own. It is not
// free of Hyprland: the helpers it calls read the monitor's top-chrome layers
// and the window list, and the tool rows rasterise text, all as it runs.
SSearchLayout layoutSearch(const SSearchLayoutInputs& IN, const std::vector<SSearchRow>& RESULTS, const std::vector<SSearchSection>& SECTIONS) {
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
    //
    // THE PROPORTIONS (UiScale.hpp). The pill is laid out at the TYPE factor --
    // PILLWL and the metrics in SearchField.hpp stay its own units, and the box
    // is that much smaller -- and its margins at the SPACE factor. Everything
    // drawn inside the pill is measured off the landed box (TRANSFORMSCALE), so
    // the badge, the glyph and the text follow the type factor for free.
    const auto  UI          = overviewUi(MONITOR);
    const float FIELDWL     = IN.fieldWL;
    const float PILLWL      = OVERVIEW_PILL_WL(FIELDWL);
    const float PILLHL      = OVERVIEW_PILL_HL * UI.type;
    const float MARGINL     = (OVERVIEW_SEARCH_ROWHL - OVERVIEW_PILL_HL) / 2.F * UI.layout; // above and below the pill
    const float ROWHL       = PILLHL + 2.F * MARGINL;                                       // 48 + 8 + 8 at the reference
    const CBox  PILLLOGICAL = {
        sc<float>(MONITOR->m_position.x) + (sc<float>(MONITOR->m_size.x) - PILLWL * UI.type) / 2.F,
        sc<float>(MONITOR->m_position.y) + MARGINL,
        PILLWL * UI.type,
        PILLHL,
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
        if (const auto WS = MONITOR->m_activeWorkspace; WS && WS->space()) {
            if (const auto WA = WS->space()->workArea(); WA.w > 1 && WA.h > 1)
                dockTopL = std::max(dockTopL, sc<float>(WA.y)); // a bar that DOES reserve
        }

        // Docking never moves the pill below where it rests (owner, 2026-11-19).
        // On a short logical screen (1920x1200 at 1.0667, with a top bar) the
        // rest row lies ABOVE the docked line, and easing toward it docked the
        // pill DOWN as the first key was typed. There the pill stays at rest.
        const float DOCKY = std::min((dockTopL - sc<float>(MONITOR->m_position.y) + overviewSearchDockPadL(MONITOR)) * sc<float>(MONITOR->m_scale), PILLUNDOCKEDY);
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
    // PILLWL is the pill's width in its own units, so this is the card transform
    // times the type factor; CARDSCALE is the transform alone.
    const float TRANSFORMSCALE = sc<float>(getOverviewGlobalBox(PILLLOGICAL, MONITOR, IN.renderScale, OVERVIEW_CHROME_NO_PAN, 0.F, IN.layout, false).width) / PILLWL;
    const float CARDSCALE      = TRANSFORMSCALE / UI.type;
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
    out.cardScale      = CARDSCALE;
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
    const float PILLFOOTL = PILLLOGICAL.y + PILLHL + ((sc<float>(pill.y) - PILLUNDOCKEDY) / std::max(0.001F, CARDSCALE));
    out.rowL              = std::clamp(PILLFOOTL - sc<float>(MONITOR->m_position.y) + MARGINL, 0.F, ROWHL);

    // THE NUDGE, applied only AFTER rowL: the strip, the results and the grid
    // all hang off rowL, so lifting the pill before it would lift the strip too
    // -- and the label the pill is making room for would chase it upward. Here
    // it moves the pill and nothing else. NOT capped by the room above the
    // pill: it rises as far as the label needs, under the bar or off the top of
    // the screen if that is what it takes (owner, 2026-11-19: "you can push the
    // search bar up regardless, doesn't need to be fully visible or even on
    // screen when hovering over the strip"). The cap at the docked line left a
    // short screen no lift at all, and the label squeezed against the pill.
    {
        out.pillRest = out.pill;

        if (IN.nudge > 0.5F) {
            const float UP = std::round(std::max(0.F, IN.nudge));
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
    const float BANDHL   = std::max(0.F, out.bandH);
    const float PANELGAP = OVERVIEW_RESULT_PANELGAPL * UI.layout; // the gaps around the results, space factor
    if (SECTIONS.empty() || BANDHL <= (PANELGAP * 2.F) + 1.F)
        return out;

    // The ranking can shrink under a selection made against a longer list. It is
    // clamped by the frame that noticed and handed back by the publish step.
    if (out.selection >= sc<int>(RESULTS.size()))
        out.selection = sc<int>(RESULTS.size()) - 1;

    // Inside the results everything is part of the rows: type factor.
    const float UT         = UI.type;
    const float PAD_L      = SEARCH_PAD_L * UT;
    const float DIV_GAPL   = SEARCH_DIV_GAPL * UT;
    const float MAX_SPANWL = overviewResultsSpanW(MONITOR);

    float CONTAINER_WL = IN.width > 1.F ? std::min(IN.width, MAX_SPANWL) : 0.F;
    if (CONTAINER_WL < 8.F)
        return out;

    const float CONTAINER_XL = sc<float>(MONITOR->m_position.x) + ((sc<float>(MONITOR->m_size.x) - CONTAINER_WL) / 2.F);
    const float CONTAINER_YL = sc<float>(MONITOR->m_position.y) + out.rowL + PANELGAP;
    const float CONTAINER_HL = BANDHL - (PANELGAP * 2.F);

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

        const float TPADL = OVERVIEW_TOOL_PADL * UT;
        const float LEFT  = CONTAINER_XL + TPADL;
        const float WIDTH = CONTAINER_WL - (2.F * TPADL);
        const float FOOT  = CONTAINER_YL + CONTAINER_HL - TPADL / 2.F;
        out.tool.header   = getOverviewGlobalBox(CBox{LEFT, CONTAINER_YL + TPADL, WIDTH, (OVERVIEW_TOOL_HEADL - 10.F) * UT}, MONITOR, IN.renderScale,
                                                 OVERVIEW_CHROME_NO_PAN, 0.F, IN.layout);
        out.tool.header.round();

        float y = CONTAINER_YL + TPADL + OVERVIEW_TOOL_HEADL * UT;
        for (size_t i = 0; i < RESULTS.size(); ++i) {
            // Measured against the FULL span, as the band was: the container is
            // easing toward it, and measuring the eased width would re-wrap every
            // frame of the entrance.
            const float ROWHL = overviewToolRowHL(RESULTS[i], MAX_SPANWL, UT);
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
                .selected = IN.zone == EOverviewNavZone::Result && sc<int>(i) == out.selection,
            });
            y += ROWHL + OVERVIEW_TOOL_GAPL * UT;
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
            .selected    = (IN.zone == EOverviewNavZone::Result && sc<int>(I) == out.selection),
        };
    };

    const float HEAD_HL  = 28.F * UT;
    const float ROW_HL   = 32.F * UT;
    const float ROW_GAPL = 4.F * UT;

    const SSearchSection* sectionByCol[3] = {};
    for (const auto& sec : SECTIONS) {
        if (sec.column >= 0 && sec.column < 3)
            sectionByCol[sec.column] = &sec;
    }

    // No inspector (a narrow screen, UiScale::inspectorShown: the column
    // widths came with 0 for it): the columns run to the container's padding,
    // and the last one has no divider after it.
    const bool  INSP_ON = IN.inspectorW > 0.5F;
    const float INSP_WL = INSP_ON ? std::clamp(IN.inspectorW, 140.F, std::max(140.F, CONTAINER_WL - (PAD_L * 2.F))) : 0.F;
    const float INSP_XL = INSP_ON ? CONTAINER_XL + CONTAINER_WL - PAD_L - INSP_WL : CONTAINER_XL + CONTAINER_WL - PAD_L + DIV_GAPL;
    const float COL_MAX_R = INSP_XL - DIV_GAPL;
    int         lastCol   = -1;
    for (int c = 0; c < 3; ++c) {
        if (IN.colW[c] >= SEARCH_COL_HIDE_WL && sectionByCol[c])
            lastCol = c;
    }

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
            cardOut.emptyHint = (c == 1 && SearchBackend::fileSearchBusy()) ? "Searching…" : "No items";

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
        if (DIV_XL < INSP_XL - 2.F && (INSP_ON || c != lastCol)) {
            const CBox DIV_LOGICAL{DIV_XL, CONTAINER_YL + PAD_L, 1.F, CONTAINER_HL - (PAD_L * 2.F)};
            CBox divBox = getOverviewGlobalBox(DIV_LOGICAL, MONITOR, IN.renderScale, OVERVIEW_CHROME_NO_PAN, 0.F, IN.layout);
            divBox.round();
            divBox.width = std::max(1.F, std::round(1.F * out.cardScale)); // a 1 px rule at any type size
            out.dividers.push_back(divBox);
        }

        colX += COL_WL + DIV_GAPL;
    }

    if (drawn == 0 || !INSP_ON)
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
            if (o.kind == SearchBackend::EItemKind::File) {
                out.inspector.detail1      = o.fileTypeStr;
                out.inspector.detail2      = o.fileSizeStr + "  ·  " + o.fileModifiedStr;
                out.inspector.detail3      = o.filePath;
                out.inspector.shortcutHint = "Space  Quick Look";
                out.inspector.shortcutHint2 = "Enter  Open";
            } else if (o.kind == SearchBackend::EItemKind::Menu) {
                out.inspector.detail1      = "Opens this submenu in the left column";
                out.inspector.shortcutHint = "Enter  Open submenu";
                out.inspector.shortcutHint2 = "Left  Back";
            } else if (o.kind == SearchBackend::EItemKind::Back) {
                out.inspector.detail1      = "Return to the parent menu";
                out.inspector.shortcutHint = "Enter  Go back";
            } else if (o.kind == SearchBackend::EItemKind::Action && (o.id == "tool:define" || o.id == "tool:translate")) {
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
            } else if (o.kind == SearchBackend::EItemKind::Action) {
                out.inspector.detail1      = o.description.empty() ? o.name : o.description;
                out.inspector.detail2      = o.actionCommand;
                out.inspector.shortcutHint = "Enter  Run";
            } else if (o.kind == SearchBackend::EItemKind::Agent) {
                out.inspector.detail1      = o.description;
                out.inspector.shortcutHint = "Enter  Launch agent";
                // Same "Key x2  Action" form as the app card's Shift x2 hint.
                if (SearchBackend::isDefaultAgent(o))
                    out.inspector.shortcutHint2 = "Default agent";
                else if (SearchBackend::canBeDefaultAgent(o))
                    out.inspector.shortcutHint2 = "Ctrl×2  Set as default";
            } else if (o.kind == SearchBackend::EItemKind::Calculator) {
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
                if (!LIVE->metadata().title().empty())
                    out.inspector.detail1 = LIVE->metadata().title();
                if (LIVE->m_workspace)
                    out.inspector.detail2 = "Workspace " + Hyprdrive::Rows::label(LIVE->m_workspace);
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
    const auto MON = g_pHyprRenderer->context().m_data.pMonitor.lock();
    return MON ? sc<uint64_t>(MON->m_id) : 0xFFFF;
}

namespace {

// THE RESULTS EASE LIKE THE DOCK MENU (user, 2026-11-03: "add the same
// effects to the search results"). The row the selection is on -- hovered or
// arrowed to -- lifts: its fill fades in, and its icon and text slide a few px
// right and grow a touch; the row it left eases back. Wall-clock, like the
// letter bounce: drawSearch notes a change of row, the frame gates below ask
// searchHoverEasing() for frames while it runs.
//
// Each row eases from the level it HAS when the selection moves (inventory
// O12: going back to a row mid-fade used to restart it -- the row popped to 0,
// the one it left to 1). An uninterrupted move is the same cross-fade as ever:
// the new row 0 -> 1 and the old one 1 -> 0 on one smoothstep.
    constexpr float SEARCH_HOVER_MS = 140.F;
    float           searchLitEased(const Hyprdrive::Motion::STween<Time::steady_tp>& fade) {
        return Hyprdrive::Motion::smoothstep(std::clamp(Hyprdrive::Motion::Clock::animMs(fade.start) / SEARCH_HOVER_MS, 0.F, 1.F));
    }
    float searchLit(int index) {
        return g_searchLit.level(index, searchLitEased);
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
        (g_pillReleasedAt.time_since_epoch().count() != 0 && Hyprdrive::Motion::Clock::animMs(g_pillReleasedAt) < PILL_TAIL_RETURN_MS + 20.F);
}

bool searchHoverEasing() {
    return g_searchLit.anyRunning([](const auto& fade) { return Hyprdrive::Motion::Clock::animMs(fade.start) < SEARCH_HOVER_MS + 20.F; });
}
}

void drawSearch(Search::CSession& S, const SSearchLayout& L, bool pillLit, bool resultsLit, int part) {
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
        g_searchLit.moveTo(now, Time::steadyNow(), searchLitEased);
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
    const auto ACCENT    = Hyprdrive::Config::highlightColor();

    if (DRAWPILL)
        OverviewRender::themePlate(PILL, PILLROUND, POWER, FADE, false, pillLit, Hyprdrive::BorderFade::key("search-pill", renderMonitorId()));

    CRectPassElement::SRectData badgeBg;
    badgeBg.box           = BADGE;
    badgeBg.color         = ACCENT.modifyA(0.30F * FADE);
    badgeBg.round         = BADGEROUND;
    badgeBg.roundingPower = POWER;
    if (DRAWPILL)
        Render::IHyprRenderer::addPassElement(g_pHyprRenderer->context(), makeUnique<CRectPassElement>(badgeBg));

    float      texFade = FADE;
    const auto ADDTEX  = [&](const auto& tex, const CBox& box) {
        CTexPassElement::SRenderData data;
        data.tex = tex;
        data.box = box;
        data.a   = texFade;
        Render::IHyprRenderer::addPassElement(g_pHyprRenderer->context(), makeUnique<CTexPassElement>(data));
    };

    const auto GLYPHTEX = OverviewRender::text("\uF002", Hyprdrive::Config::highlightForeground().modifyA(0.92F), std::max(1, sc<int>(std::round(OVERVIEW_PILL_GLYPHPX * SCALE))), false,
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
        OverviewRender::themePlate(L.containerBox, roundFor(L.containerBox), POWER, FADE, true, resultsLit, Hyprdrive::BorderFade::key("search-results", renderMonitorId()));

        for (const auto& divBox : L.dividers) {
            CRectPassElement::SRectData div;
            div.box   = divBox;
            div.color = Hyprdrive::Config::themeForeground().modifyA(0.12F * FADE);
            Render::IHyprRenderer::addPassElement(g_pHyprRenderer->context(), makeUnique<CRectPassElement>(div));
        }
    }

    // THE TOOL PANE. Drawn on the container plate the cards would have used, in
    // the theme's own heading / foreground / muted colours, so the dictionary
    // and the translator read as the same surface as the results they replace.
    if (L.tool.active) {
        const CBox& HEAD   = L.tool.header;
        const auto  FGCOL  = Hyprdrive::Config::themeForeground();
        const auto  MUTED  = Hyprdrive::Config::themeMutedText();
        const auto  HEADCO = Hyprdrive::Config::themeHeading();

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
                Render::IHyprRenderer::addPassElement(g_pHyprRenderer->context(), makeUnique<CRectPassElement>(sel));
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
        const auto HEADTEX = OverviewRender::text(CARD.title, Hyprdrive::Config::themeHeading(), HEADPX, false, "", std::max(1, sc<int>(CARD.box.width - headIndent - PADL)), 600);
        if (HEADTEX && HEADTEX->ok()) {
            CBox headText{CARD.box.x + headIndent, CARD.box.y + PADL, sc<float>(HEADTEX->m_size.x), sc<float>(HEADTEX->m_size.y)};
            ADDTEX(HEADTEX, headText.round());
        }

        if (CARD.more > 0) {
            const int   MOREPX  = std::max(1, sc<int>(std::round(11.F * SCALE)));
            std::string moreStr = "+" + std::to_string(CARD.more);
            const auto  MORETEX = OverviewRender::text(moreStr, Hyprdrive::Config::themeMutedText(), MOREPX, false, "", 100, 500);
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
                const auto SELFILL = Hyprdrive::Config::themeSelectedFill();
                sel.color          = SELFILL.modifyA(SELFILL.a * FADE * CARD.alpha * LIT);
                sel.round         = roundFor(ROW);
                sel.roundingPower = POWER;
                Render::IHyprRenderer::addPassElement(g_pHyprRenderer->context(), makeUnique<CRectPassElement>(sel));
            }

            const float ROWPAD     = std::round(6.F * SCALE);
            float       textIndent = ROWPAD;

            const float ICOND = std::round(18.F * SCALE);
            if (SearchBackend::isGlyphIcon(ITEM.icon)) {
                const int         GLYPHPX = std::max(1, sc<int>(std::round(16.F * SCALE)));
                const std::string FAMILY  = ITEM.iconFont.empty() ? "JetBrainsMono Nerd Font" : ITEM.iconFont;
                const auto        GLYPHTEX =
                    OverviewRender::text(ITEM.icon, ITEM.selected ? Hyprdrive::Config::themeSelectedText() : Hyprdrive::Config::themeForeground(), GLYPHPX, false,
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
            const auto FG     = ITEM.selected ? Hyprdrive::Config::themeSelectedText() : Hyprdrive::Config::themeForeground();
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
                        const auto DESCFG = ITEM.selected ? FG : Hyprdrive::Config::themeMutedText();
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
            const auto EMPTYTEX = OverviewRender::text(CARD.emptyHint.empty() ? "No items" : CARD.emptyHint, Hyprdrive::Config::themeMutedText(), EMPTYPX, false, "", sc<int>(CARD.box.width - PADL * 2.F), 400);
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
            Render::IHyprRenderer::addPassElement(g_pHyprRenderer->context(), makeUnique<CTexPassElement>(data));
        };

        const auto ICONTEX = OverviewRender::appIcon(L.inspector.icon);
        if (ICONTEX && ICONTEX->ok()) {
            CBox iconBox{IBOX.x + PADL, IBOX.y + PADL, ICOND, ICOND};
            ADDINSP(ICONTEX, iconBox.round());
        } else if (!L.inspector.icon.empty()) {
            const int  GLYPHPX = std::max(1, sc<int>(std::round(28.F * SCALE)));
            const auto GLYPHTEX = OverviewRender::text(L.inspector.icon, Hyprdrive::Config::themeHeading(), GLYPHPX, false, "", sc<int>(ICOND), 400);
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
            auto catCol = Hyprdrive::Config::themeHeading();
            paintWrap(L.inspector.category, catCol, std::max(1, sc<int>(std::round(10.F * SCALE))), 600, TEXTX, TEXTW, sc<int>(18.F * SCALE), std::round(3.F * SCALE));
        }
        if (!L.inspector.status.empty()) {
            const auto STCOL = L.inspector.running ? Hyprdrive::Config::themeHeading() : Hyprdrive::Config::themeMutedText();
            paintWrap(L.inspector.status, STCOL, std::max(1, sc<int>(std::round(11.F * SCALE))), 600, TEXTX, TEXTW, sc<int>(18.F * SCALE), std::round(3.F * SCALE));
        }
        paintWrap(L.inspector.title, Hyprdrive::Config::themeForeground(), std::max(1, sc<int>(std::round(15.F * SCALE))), 600, TEXTX, TEXTW, sc<int>(40.F * SCALE),
                  std::round(4.F * SCALE));

        textY = std::max(textY, sc<float>(IBOX.y + PADL + ICOND)) + std::round(10.F * SCALE);

        // THE SUBTITLE GETS WHAT THE CARD HAS LEFT (user, 2026-09-24: "wrap
        // needs to scale with panel size"). A fixed line budget cut a definition
        // off with most of the card still empty. So the details and the hints at
        // the foot are measured first, and the subtitle may fill everything
        // between the header and them -- a tall card shows the whole answer, a
        // short one ellipsizes on the last line that fits.
        const auto DETCOL  = Hyprdrive::Config::themeMutedText();
        const int  DETPX   = std::max(1, sc<int>(std::round(11.F * SCALE)));
        const int  DET3PX  = std::max(1, sc<int>(std::round(10.F * SCALE)));
        const int  HINTPX  = std::max(1, sc<int>(std::round(11.F * SCALE)));
        const auto HINTCOL = Hyprdrive::Config::themeMutedText();
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

        paintWrap(L.inspector.subtitle, Hyprdrive::Config::themeMutedText(), std::max(1, sc<int>(std::round(12.F * SCALE))), 400, IBOX.x + PADL, FULLW, SUBH,
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
    const CHyprColor FG = L.placeholder ? Hyprdrive::Config::themeMutedText() : (L.emptyResult ? Hyprdrive::Config::themeUrgent() : Hyprdrive::Config::themeForeground());

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
        Render::IHyprRenderer::addPassElement(g_pHyprRenderer->context(), makeUnique<CTexPassElement>(data));
    };

    // Nothing measured is nothing to fit: the placeholder keeps the rest width.
    auto& FIELDMOTION = S.fieldOn(L.monitorId);
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
                                                                       std::clamp(Hyprdrive::Motion::Clock::animMsBetween(NOW, g_pillReleasedAt) / PILL_TAIL_RETURN_MS, 0.F, 1.F);
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

// `hyprdrive:debug search` (tests/gates/search*.sh). READ-ONLY: it reads the
// session, the layouts the last frames published (find, never layoutOn/fieldOn,
// which would create a slot) and the ranked rows the render step memoises --
// asked for only while the pane is up, the same condition the render step asks
// them under, so the memo it reads is the one the frame built.
namespace {
    const char* searchZoneName(EOverviewNavZone zone) {
        switch (zone) {
            case EOverviewNavZone::Tile: return "tile";
            case EOverviewNavZone::Result: return "result";
            case EOverviewNavZone::Desktop: return "desktop";
            case EOverviewNavZone::Search: return "search";
        }
        return "?";
    }
    const char* searchIntentName(ESearchIntent intent) {
        switch (intent) {
            case ESearchIntent::Typing: return "typing";
            case ESearchIntent::Browsing: return "browsing";
            case ESearchIntent::Hovering: return "hovering";
        }
        return "?";
    }
    std::string searchQuoted(std::string_view s) {
        std::string out = "\"";
        for (const char C : s)
            out += C == '"' ? std::string{"\\\""} : C == '\n' ? std::string{"\\n"} : std::string(1, C);
        return out + "\"";
    }
    std::string searchRowKind(const SSearchRow& row) {
        if (row.tool)
            return row.toolStatus ? "toolstatus" : "tool";
        if (row.app)
            return overviewWindowForApp(*row.app) ? "app-running" : "app";
        if (row.omarchy) {
            switch (row.omarchy->kind) {
                case SearchBackend::EItemKind::App: return "item-app";
                case SearchBackend::EItemKind::Action: return "action";
                case SearchBackend::EItemKind::Menu: return "menu";
                case SearchBackend::EItemKind::Back: return "back";
                case SearchBackend::EItemKind::Agent: return "agent";
                case SearchBackend::EItemKind::Calculator: return "calc";
                case SearchBackend::EItemKind::File: return "file";
                case SearchBackend::EItemKind::Provider: return "provider";
            }
        }
        return "provider";
    }
}

std::string COverview::searchReport() const {
    auto& S = m_search;
    const auto  MON  = pMonitor.lock();
    std::string out  = std::format("mon {} closing={} keys={} tile={} matches={} band={:.1f}>{:.1f} width={:.1f} dock={:.2f}>{:.2f} pillNudgeWant={:.1f}",
                                   MON ? MON->m_name : std::string{"?"}, isClosing() ? 1 : 0, activeScrollOverview().get() == this ? 1 : 0,
                                   closeOnWindow.lock() ? closeOnWindow.lock()->metadata().appID() : std::string{"-"}, matchingWindowCount(),
                                   resultsBand ? resultsBand->value() : 0.F, resultsBand ? resultsBand->goal() : 0.F, resultsWidth ? resultsWidth->value() : 0.F,
                                   searchDock ? searchDock->value() : 0.F, searchDock ? searchDock->goal() : 0.F, pillNudgeWant);
    const auto IT = S.layouts.find(searchMonitorId());
    if (IT == S.layouts.end())
        return out + " layout=none";
    const auto& L = IT->second;
    out += std::format(" drawable={} queried={} empty={} placeholder={} label={} rowL={:.1f} bandH={:.1f} pill={:.0f},{:.0f},{:.0f},{:.0f} fieldW={:.1f} "
                       "container={:.0f},{:.0f},{:.0f},{:.0f} gridTiles={} stripPlateL={:.1f} gridNudgeL={:.1f} gridShiftL={:.1f} sel={} hits={} cards={} tool={} inspector={}",
                       L.drawable ? 1 : 0, L.queried ? 1 : 0, L.emptyResult ? 1 : 0, L.placeholder ? 1 : 0, searchQuoted(L.label), L.rowL, L.bandH, L.pill.x, L.pill.y,
                       L.pill.width, L.pill.height, L.fieldW, L.containerBox.x, L.containerBox.y, L.containerBox.width, L.containerBox.height, L.gridTiles,
                       L.stripPlateL, L.gridNudgeL, L.gridShiftL, L.selection, L.hits.size(), L.cards.size(), L.tool.active ? 1 : 0, L.inspector.visible ? 1 : 0);
    if (const auto F = S.fields.find(searchMonitorId()); F != S.fields.end())
        out += std::format(" field={:.1f} moving={}", F->second.drawWidthL, F->second.moving ? 1 : 0);
    // The proportions (UiScale.hpp) and the strip label's plate as last drawn,
    // rendered px (tests/gates/overview_proportions.sh).
    if (MON) {
        const auto UI = overviewUi(MON);
        out += std::format(" ui={:.3f},{:.3f}", UI.layout, UI.type);
        const CBox TIP = stripTip.text.empty() ? CBox{} : OverviewRender::labelPillBox(MON, stripTip.text, stripTip.cx, stripTip.bottom, MON->m_scale, UI.type);
        out += TIP.width > 0 ? std::format(" tip={:.0f},{:.0f},{:.0f},{:.0f}", TIP.x, TIP.y, TIP.width, TIP.height) : std::string{" tip=none"};
    }
    for (size_t i = 0; i < L.hits.size() && i < 16; ++i) {
        const auto& H = L.hits[i];
        out += std::format("\n    hit {} col {} row {} box {:.0f},{:.0f},{:.0f},{:.0f}", H.index, H.column, H.rowInCol, H.box.x, H.box.y, H.box.width, H.box.height);
    }
    return out;
}

std::string overviewSearchReport() {
    auto& S = g_overviewRegistry.search;
    const auto& Q    = S.query;
    const auto& N    = S.nav;
    const bool  PANE = overviewSearchPaneUp(S);
    const auto  ROWS = PANE ? overviewSearchRows(S) : std::vector<SSearchRow>{};
    std::string out  = std::format("hyprdrive search: overviews={} q={} active={} windowsOnly={} pane={} menuBack={} zone={} sel={} explicit={} firstPending={} "
                                   "intent={} hover={} kbTile={} drag={} rows={}",
                                   scrollOverviews().size(), searchQuoted(Q.text), Q.active ? 1 : 0, Q.windowsOnly ? 1 : 0, PANE ? 1 : 0,
                                   SearchBackend::canNavigateMenuBack() ? 1 : 0, searchZoneName(N.zone), N.selection, N.explicitSelection ? 1 : 0,
                                   N.selectFirstPending ? 1 : 0, searchIntentName(N.intent), N.hoverIndex, N.keyboardTile ? 1 : 0, S.pointer.drag ? 1 : 0, ROWS.size());
    if (N.selection >= 0 && N.selection < sc<int>(ROWS.size()))
        out += " selRow=" + searchQuoted(ROWS[sc<size_t>(N.selection)].name);
    for (size_t i = 0; i < ROWS.size() && i < 24; ++i)
        out += std::format("\n  row {} c{} {} {}", i, ROWS[i].column, searchRowKind(ROWS[i]), searchQuoted(ROWS[i].name));
    for (const auto& OV : scrollOverviews())
        if (const auto* O = dynamic_cast<const COverview*>(OV.get()))
            out += "\n  " + O->searchReport();
    return out;
}
