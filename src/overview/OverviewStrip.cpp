// COverview: top strip, scratchpad cell, alt strips, detent badge.
// Cut from Overview.cpp by pure code motion.
#include "OverviewParts.hpp"
#include "MotionClock.hpp"

using namespace OverviewDetail;


// The VISIBLE space between the strip's plate and the scratchpad's own, in
// strip gaps -- on top of the two plates' padding, which is what the detach has
// to clear.
//
// Measured cell-to-cell it did not: each plate expands by OVERVIEW_STRIP_PANELPADL
// (10 logical px) on every side, so 3 gaps at the default 5px left the two plates
// overlapping by 5 (user, 2026-09-12). The separation is plate-to-plate now, so
// it holds whatever the theme's gap is.
static constexpr float OVERVIEW_SCRATCHPAD_DETACH_GAPS = 2.F;

// The reveal zone starts one strip gap past the strip's PLATE edge -- clear of
// the plate, not merely of the last cell. Still inside the detach distance, so
// the zone begins before the cell it reveals and the two are one continuous
// region for the pointer.
static constexpr float OVERVIEW_SCRATCHPAD_ZONE_PAD_GAPS = 1.F;

float COverview::scratchpadRevealValue() const {
    return scratchpadReveal ? std::clamp(scratchpadReveal->value(), 0.F, 1.F) : 0.F;
}

// ONE body for both the strip's cells and the detached scratchpad cell, because
// the two geometries are mutually dependent: the cell hangs off the strip's
// right edge, and the room it takes is what slides the strip left to keep the
// pair centred. As two functions they would have had to call each other.
//
// With REVEAL 0 every added term vanishes and this is exactly the strip-only
// layout it replaced -- which is what the geometry host tests assert.
CBox COverview::stripSlotLogicalBox(size_t workspaceIdx, bool scratchSlot, float revealOverride) const {
    const auto& ROW = stripRow(revealOverride, -1.F);
    if (!ROW.valid)
        return {};

    const auto BOX = scratchSlot ? ROW.scratch : (workspaceIdx < ROW.cells.size() ? ROW.cells[workspaceIdx] : Hyprdrive::StripLayout::SBox{});
    return BOX.empty() ? CBox{} : CBox{BOX.x, BOX.y, BOX.w, BOX.h};
}

// The whole row: this monitor's cells, the scratchpad cell, and every other
// monitor's strip -- StripLayout.hpp, the pure function with the host test. This
// only gathers what it needs to know.
const Hyprdrive::StripLayout::SOutput& COverview::stripRow(float scratchRevealOverride, float altRevealOverride) const {
    // A row with no cells in it, for the outputs that have nothing to lay out.
    // Returned by reference like every other answer here: the SOutput carries
    // three vectors, and stripSlotLogicalBox asks for one cell at a time.
    static const Hyprdrive::StripLayout::SOutput EMPTY{};

    const auto MONITOR = pMonitor.lock();
    if (!MONITOR || images.empty())
        return EMPTY;

    // THE MEMO. Everything below is pure arithmetic over these scalars plus the
    // per-image span ratios, and none of it can change inside one drawn frame,
    // so the whole row is built once and handed out. The key carries the top
    // chrome and the band explicitly even though the frame counter is in it:
    // renderSearch publishes the row height MID-frame, which is the one way two
    // calls in the same frame are entitled to disagree.
    const uint64_t FRAME     = overviewFrameOn(MONITOR);
    const float    SCRATCH   = scratchRevealOverride >= 0.F ? std::clamp(scratchRevealOverride, 0.F, 1.F) : scratchpadRevealValue();
    const float    ALTREVEAL = altRevealOverride >= 0.F ? std::clamp(altRevealOverride, 0.F, 1.F) : altStripsRevealValue();
    const float    TOPL      = overviewSearchLogicalHeight(MONITOR) + overviewResultsLogicalHeight(MONITOR);
    const float    BANDL     = overviewStripLogicalHeight(MONITOR);
    const float    GAPL      = sc<float>(Hyprdrive::Config::getStripGap());
    const double   ASPECT    = MONITOR->m_size.y > 1 ? sc<double>(MONITOR->m_size.x) / sc<double>(MONITOR->m_size.y) : 1.6;
    // The other monitors' strips, as one number: how many there are, how many
    // cells each has, and where each output sits -- the only things about them
    // this layout reads. Read off the memoised altStrips() so the key and the
    // layout below cannot disagree about the list.
    size_t altSignature = 0;
    if (ALTREVEAL > 0.F) {
        for (const auto& ALT : altStrips()) {
            const auto& M = ALT.monitor;

            altSignature = altSignature * 1000003 + sc<size_t>(M->m_id) + 1;
            altSignature = altSignature * 1000003 + ALT.overview->images.size();
            altSignature = altSignature * 1000003 + sc<size_t>(sc<long long>(M->m_size.x));
            altSignature = altSignature * 1000003 + sc<size_t>(sc<long long>(M->m_size.y));
        }
    }

    if (stripRowMemo.filled && stripRowMemo.frame == FRAME && stripRowMemo.scratchArg == scratchRevealOverride && stripRowMemo.altArg == altRevealOverride &&
        stripRowMemo.scratch == SCRATCH && stripRowMemo.alt == ALTREVEAL && stripRowMemo.top == TOPL && stripRowMemo.band == BANDL && stripRowMemo.gap == GAPL &&
        stripRowMemo.monX == MONITOR->m_position.x && stripRowMemo.monY == MONITOR->m_position.y && stripRowMemo.monW == MONITOR->m_size.x &&
        stripRowMemo.aspect == ASPECT && stripRowMemo.images == images.size() && stripRowMemo.altSignature == altSignature)
        return stripRowMemo.out;

    Hyprdrive::StripLayout::SInput in;
    in.monX     = MONITOR->m_position.x;
    in.monY     = MONITOR->m_position.y;
    in.monW     = MONITOR->m_size.x;
    in.aspect   = ASPECT;
    in.top      = TOPL;
    in.band     = BANDL;
    in.padFrac  = OVERVIEW_STRIP_BAND_PAD_FRAC;
    in.gap      = Hyprdrive::Config::getStripGap();
    in.panelPad = OVERVIEW_STRIP_PANELPADL;
    in.detach   = OVERVIEW_SCRATCHPAD_DETACH_GAPS;

    in.ratios.reserve(images.size());
    for (const auto& IMG : images)
        in.ratios.push_back(IMG ? workspaceScrollingSpanRatio(IMG->pWorkspace, MONITOR) : 1.0);

    in.scratchReveal = SCRATCH;

    if (ALTREVEAL > 0.F) {
        for (const auto& ALT : altStrips()) {
            const auto& M = ALT.monitor;
            in.alts.push_back({ALT.overview->images.size(), M->m_size.y > 1 ? sc<double>(M->m_size.x) / sc<double>(M->m_size.y) : 1.6});
        }
        in.altReveal = ALTREVEAL;
    }

    stripRowMemo.frame        = FRAME;
    stripRowMemo.scratchArg   = scratchRevealOverride;
    stripRowMemo.altArg       = altRevealOverride;
    stripRowMemo.scratch      = SCRATCH;
    stripRowMemo.alt          = ALTREVEAL;
    stripRowMemo.top          = TOPL;
    stripRowMemo.band         = BANDL;
    stripRowMemo.gap          = GAPL;
    stripRowMemo.monX         = MONITOR->m_position.x;
    stripRowMemo.monY         = MONITOR->m_position.y;
    stripRowMemo.monW         = MONITOR->m_size.x;
    stripRowMemo.aspect       = in.aspect;
    stripRowMemo.images       = images.size();
    stripRowMemo.altSignature = altSignature;
    stripRowMemo.out          = Hyprdrive::StripLayout::layout(in);
    stripRowMemo.filled       = true;

    // What the plate actually reaches, for the grid below it. StripLayout puts
    // the cells at PAD + (band - 2*PAD - cellH)/2 into the band and the plate
    // pads them by panelPad, so this is that foot -- and it collapses to the
    // old PAD + cellH + panelPad the moment the row stops shrinking.
    if (const auto& OUT = stripRowMemo.out; OUT.valid && OUT.cellH > 0.0) {
        // PAD, then the cells as they actually came out, then the plate's pad.
        // No centring term: StripLayout top-aligns the row in the band, so the
        // whole of the shrink is below the plate and this is its real foot.
        const float PAD = BANDL * OVERVIEW_STRIP_BAND_PAD_FRAC;
        searchLayoutHere().stripPlateL = PAD + sc<float>(OUT.cellH) + OVERVIEW_STRIP_PANELPADL;
    } else
        searchLayoutHere().stripPlateL = 0.F;

    return stripRowMemo.out;
}

CBox COverview::stripCellLogicalBox(size_t workspaceIdx) const {
    return stripSlotLogicalBox(workspaceIdx, false, -1.F);
}

CBox COverview::scratchpadCellLogicalBox(float revealOverride) const {
    return stripSlotLogicalBox(0, true, revealOverride);
}

CBox COverview::scratchpadCellBox(PHLMONITOR monitor, float renderScale, float revealOverride) const {
    const auto LOGICAL = scratchpadCellLogicalBox(revealOverride);
    if (!monitor || LOGICAL.width <= 0)
        return {};

    return getOverviewGlobalBox(LOGICAL, monitor, renderScale, OVERVIEW_CHROME_NO_PAN, 0.F, layout);
}

// Half out does not take a click: a cell the user can barely see must not be
// the thing that swallows a press meant for the tile behind it.
bool COverview::scratchpadCellContains(const Vector2D& point) const {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR || scratchpadRevealValue() <= 0.5F)
        return false;

    const auto BOX = scratchpadCellBox(MONITOR, scale->value(), -1.F);
    return BOX.width > 1 && BOX.containsPoint(point);
}

PHLWORKSPACE COverview::scratchpadCellWorkspace() const {
    // The one already up wins, so the cell keeps naming what the user can see.
    if (const auto SHOWN = Hyprdrive::Scratchpad::shownOn(pMonitor.lock()))
        return SHOWN;

    const auto POPULATED = Hyprdrive::Scratchpad::populated();
    if (!POPULATED.empty())
        return POPULATED.front();

    const auto ALL = Hyprdrive::Scratchpad::workspaces();
    return ALL.empty() ? nullptr : ALL.front();
}

// THE INVISIBLE REVEAL ZONE: everything right of the strip's last cell, from a
// clear padding gap out to the monitor's edge.
//
// Anchored to where the strip is THIS FRAME, not to where the cell will land.
// Measured at full reveal it sat ON the last cell -- the strip slides left to
// make room, so at rest that space is still the last workspace -- and ate the
// clicks meant for it: with 9 workspaces the 9th could not be clicked at all
// (user, 2026-09-12). The padding keeps it off that cell at every reveal, and
// running out to the monitor edge puts the revealed cell inside its own zone,
// so a pointer resting on the cell is what holds it out.
CBox COverview::scratchpadZoneLogicalBox() const {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR || images.empty())
        return {};

    const auto LAST = stripCellLogicalBox(images.size() - 1);
    if (LAST.width <= 1)
        return {};

    const float GAP = sc<float>(Hyprdrive::Config::getStripGap());
    const float PAD = OVERVIEW_STRIP_PANELPADL + std::max(GAP * OVERVIEW_SCRATCHPAD_ZONE_PAD_GAPS, 1.F);
    const float X   = sc<float>(LAST.x) + sc<float>(LAST.width) + PAD;
    const float W   = sc<float>(MONITOR->m_position.x) + sc<float>(MONITOR->m_size.x) - X;

    return W <= 1.F ? CBox{} : CBox{X, sc<float>(LAST.y), W, sc<float>(LAST.height)};
}

void COverview::updateScratchpadReveal() {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR || !scratchpadReveal)
        return;

    // TWO triggers and nothing else. A window DRAG, because the cell is its
    // drop target; and the invisible zone the cell itself will occupy, because
    // there is no drag in progress when the user simply wants to open the
    // scratchpad, and a cell that only ever existed mid-drag could never be
    // clicked. The zone is measured at FULL reveal, so entering it cannot fight
    // the animation it just started.
    const bool DRAGGING = !!drag.activeWindow;
    bool       want     = DRAGGING && !closing;

    if (!want && !closing && isOverviewPointerOnMonitor(MONITOR)) {
        if (const auto LOGICAL = scratchpadZoneLogicalBox(); LOGICAL.width > 1) {
            const auto ZONE = getOverviewGlobalBox(LOGICAL, MONITOR, scale->value(), OVERVIEW_CHROME_NO_PAN, 0.F, layout);
            want            = ZONE.width > 1 && ZONE.containsPoint(lastMousePosLocal);
        }
    }

    // NO gate on a scratchpad existing. It used to refuse to reveal without
    // one, reasoning that an empty cell is a control that does nothing -- but
    // Hyprland REAPS a special workspace when its last window leaves, so moving
    // the final window out killed the zone for good: nothing could reveal the
    // cell, and only a drag had ever bypassed the gate (user, 2026-09-12). The
    // cell is a permanent affordance now, and clicking it creates the scratchpad
    // the same way dropping a window on it already did.

    if (const float TARGET = want ? 1.F : 0.F; std::abs(scratchpadReveal->goal() - TARGET) > 0.01F)
        *scratchpadReveal = TARGET;
}

// ------------------------------------------------ other monitors' strips --

// Every OTHER monitor that has an overview up and desktops to show, left to
// right the way the monitors themselves are arranged. Closing overviews stay in:
// all of them close together, and a strip that vanished the instant its
// overview started to leave would jerk this row a whole strip sideways mid-fade.
const std::vector<COverview::SAltStrip>& COverview::altStrips() const {
    // MEMOISED, because this is asked for on every POINTER MOTION and motion
    // arrives far faster than frames do: the reveal zone's box and the alt-cell
    // hit test both start here, and each call allocated a vector and sorted it.
    // Keyed on the overview frame counter (which only moves inside render) and
    // on how many overviews are registered, so it is rebuilt once per drawn
    // frame per output and whenever a monitor's overview comes or goes.
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR) {
        altStripsBuf.clear();
        altStripsFilled = false;
        return altStripsBuf;
    }

    const size_t COUNT = scrollOverviews().size();
    if (altStripsFilled && altStripsFrame == g_overviewFrame && altStripsCount == COUNT)
        return altStripsBuf;

    altStripsFrame  = g_overviewFrame;
    altStripsCount  = COUNT;
    altStripsFilled = true;

    auto& out = altStripsBuf;
    out.clear();

    for (const auto& OV : scrollOverviews()) {
        const auto OTHER = OV ? dynamic_cast<COverview*>(OV.get()) : nullptr;
        if (!OTHER || OTHER == this)
            continue;
        const auto OTHERMON = OTHER->pMonitor.lock();
        if (!OTHERMON || OTHERMON == MONITOR || !OTHERMON->m_enabled || OTHER->images.empty())
            continue;
        out.push_back({OTHER, OTHERMON});
    }

    std::ranges::sort(out, [](const SAltStrip& a, const SAltStrip& b) {
        if (a.monitor->m_position.x != b.monitor->m_position.x)
            return a.monitor->m_position.x < b.monitor->m_position.x;
        if (a.monitor->m_position.y != b.monitor->m_position.y)
            return a.monitor->m_position.y < b.monitor->m_position.y;
        return a.monitor->m_id < b.monitor->m_id;
    });
    return out;
}

float COverview::altStripsRevealValue() const {
    return altStripsReveal ? std::clamp(altStripsReveal->value(), 0.F, 1.F) : 0.F;
}

// The reveal zone: left of the strip's first cell, a clear padding gap off its
// plate, out to the monitor's edge -- the scratchpad zone, mirrored, and for the
// same reasons (see scratchpadZoneLogicalBox). Nothing at all with no other
// monitor: hovering there on a one-monitor setup must do nothing.
CBox COverview::altZoneLogicalBox() const {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR || images.empty() || altStrips().empty())
        return {};

    const auto FIRST = stripCellLogicalBox(0);
    if (FIRST.width <= 1)
        return {};

    const float GAP = sc<float>(Hyprdrive::Config::getStripGap());
    const float PAD = OVERVIEW_STRIP_PANELPADL + std::max(GAP * OVERVIEW_SCRATCHPAD_ZONE_PAD_GAPS, 1.F);
    const float X   = sc<float>(MONITOR->m_position.x);
    const float W   = sc<float>(FIRST.x) - PAD - X;

    return W <= 1.F ? CBox{} : CBox{X, sc<float>(FIRST.y), W, sc<float>(FIRST.height)};
}

// Half out does not take a click, same as the scratchpad cell.
std::optional<COverview::SAltHit> COverview::altCellAt(const Vector2D& point) const {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR || altStripsRevealValue() <= 0.5F)
        return std::nullopt;

    const auto& STRIPS = altStrips();
    const auto  ROW    = stripRow(-1.F, -1.F);
    if (!ROW.valid || !ROW.altDrawable || ROW.altCells.size() != STRIPS.size())
        return std::nullopt;

    for (size_t k = 0; k < STRIPS.size(); ++k) {
        auto* const OTHER = STRIPS[k].overview;
        for (size_t j = 0; j < ROW.altCells[k].size() && j < OTHER->images.size(); ++j) {
            const auto& B = ROW.altCells[k][j];
            if (B.empty() || !OTHER->images[j] || !OTHER->images[j]->pWorkspace)
                continue;
            const auto CELL = getOverviewGlobalBox(CBox{B.x, B.y, B.w, B.h}, MONITOR, scale->value(), OVERVIEW_CHROME_NO_PAN, 0.F, layout);
            if (CELL.width <= 1 || !CELL.containsPoint(point))
                continue;
            return SAltHit{.overview = OTHER, .monitor = STRIPS[k].monitor, .workspaceIdx = j, .workspace = OTHER->images[j]->pWorkspace, .cell = CELL};
        }
    }
    return std::nullopt;
}

void COverview::updateAltStripsReveal() {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR || !altStripsReveal)
        return;

    bool want = false;
    // No other monitor, no strips -- and no zone either, so nothing can ask.
    if (!closing && !altStrips().empty() && isOverviewPointerOnMonitor(MONITOR)) {
        // A window drag on this monitor, whichever overview it started in: the
        // strips are drop targets for sending a window to another monitor.
        const bool DRAGGING = drag.activeWindow || (g_pointerGrabOverview && g_pointerGrabOverview->drag.activeWindow);
        want                = DRAGGING;

        if (!want) {
            if (const auto LOGICAL = altZoneLogicalBox(); LOGICAL.width > 1) {
                const auto ZONE = getOverviewGlobalBox(LOGICAL, MONITOR, scale->value(), OVERVIEW_CHROME_NO_PAN, 0.F, layout);
                want            = ZONE.width > 1 && ZONE.containsPoint(lastMousePosLocal);
            }
        }
    }

    if (const float TARGET = want ? 1.F : 0.F; std::abs(altStripsReveal->goal() - TARGET) > 0.01F)
        *altStripsReveal = TARGET;
}

// What a monitor is called on its strip's label. The connector name alone
// ("DP-1") says nothing to a person; the model does, except for a laptop panel
// whose "model" is a hex id.
static std::string overviewMonitorLabel(const PHLMONITOR& monitor) {
    if (!monitor)
        return {};

    const auto& NAME = monitor->m_name;
    if (NAME.starts_with("eDP") || NAME.starts_with("LVDS") || NAME.starts_with("DSI"))
        return "Built-in Display";

    const auto MAKE  = monitor->m_output ? monitor->m_output->make : std::string{};
    const auto MODEL = monitor->m_output ? monitor->m_output->model : std::string{};
    const auto BRAND = MAKE.substr(0, MAKE.find(' '));
    if (!MODEL.empty() && !MODEL.starts_with("0x"))
        return BRAND.empty() ? MODEL : BRAND + " " + MODEL;
    if (!BRAND.empty())
        return BRAND + " (" + NAME + ")";
    return NAME;
}

void COverview::renderAltStrips(PHLMONITOR monitor, float renderScale, const Time::steady_tp& now, const SStripStyle& STYLE) {
    const float REVEAL = altStripsRevealValue();
    if (!monitor || REVEAL <= 0.01F)
        return;

    const auto STRIPS = altStrips();
    const auto ROW    = stripRow(-1.F, -1.F);
    if (STRIPS.empty() || !ROW.valid || !ROW.altDrawable || ROW.altCells.size() != STRIPS.size())
        return;

    const float FADE = STYLE.fade * REVEAL;

    auto*      dragContext = g_pointerGrabOverview && g_pointerGrabOverview->drag.activeWindow ? g_pointerGrabOverview : this;
    const auto DRAGGED     = getOverviewWindowToShow(dragContext->drag.activeWindow.lock());
    const auto HOVER       = isOverviewPointerOnMonitor(monitor) ? altCellAt(lastMousePosLocal) : std::nullopt;

    for (size_t k = 0; k < STRIPS.size(); ++k) {
        auto* const OTHER    = STRIPS[k].overview;
        const auto  OTHERMON = STRIPS[k].monitor;
        const auto& CELLS    = ROW.altCells[k];

        // Its own plate, padded by how far out it is (StripLayout.hpp), so it
        // never sits over this strip's plate or the next monitor's.
        double x0 = 1e18, x1 = -1e18, y0 = 0, h = 0;
        for (const auto& B : CELLS) {
            if (B.empty())
                continue;
            x0 = std::min(x0, B.x);
            x1 = std::max(x1, B.right());
            y0 = B.y;
            h  = B.h;
        }
        if (x1 <= x0)
            continue;

        const CBox PLATELOGICAL{x0 - ROW.altPlatePad, y0 - ROW.altPlatePad, x1 - x0 + 2.0 * ROW.altPlatePad, h + 2.0 * ROW.altPlatePad};
        const auto PLATE = getOverviewGlobalBox(PLATELOGICAL, monitor, renderScale, OVERVIEW_CHROME_NO_PAN, 0.F, layout);
        if (PLATE.width <= 1 || !overviewBoxIntersectsMonitor(PLATE, monitor))
            continue;

        const auto FIRSTCELL  = getOverviewGlobalBox(CBox{CELLS.front().x, CELLS.front().y, CELLS.front().w, CELLS.front().h}, monitor, renderScale, OVERVIEW_CHROME_NO_PAN, 0.F,
                                                     layout);
        const int  PADPX      = std::max(0, sc<int>(std::round(sc<float>(FIRSTCELL.x) - sc<float>(PLATE.x))));
        const int  PLATEROUND = STYLE.round <= 0 ? 0 : STYLE.round + PADPX;
        OverviewRender::themePlate(PLATE, PLATEROUND, STYLE.power, FADE, true, pointerOnChrome(PLATE),
                                   Hyprdrive::BorderFade::key("strip-alt", (sc<uint64_t>(monitor->m_id) << 16) ^ (OTHERMON ? sc<uint64_t>(OTHERMON->m_id) : 0xFFFF)));

        const float MINIRENDERSCALE = renderScale * sc<float>(FIRSTCELL.height) / std::max(1.F, sc<float>(OTHERMON->m_size.y) * monitor->m_scale);

        for (size_t j = 0; j < CELLS.size() && j < OTHER->images.size(); ++j) {
            const auto& WIMG = OTHER->images[j];
            if (CELLS[j].empty() || !WIMG || !WIMG->pWorkspace)
                continue;

            const auto CELL = getOverviewGlobalBox(CBox{CELLS[j].x, CELLS[j].y, CELLS[j].w, CELLS[j].h}, monitor, renderScale, OVERVIEW_CHROME_NO_PAN, 0.F, layout);
            if (CELL.width <= 1 || !overviewBoxIntersectsMonitor(CELL, monitor))
                continue;

            const bool ATLEFT  = j == 0;
            const bool ATRIGHT = j + 1 == CELLS.size();

            renderOverviewWorkspaceShadow(monitor, CELL, renderScale, false, FADE);

            // The desktop's number, as every strip cell carries it.
            if (const auto NUMTEX = OverviewRender::text(std::to_string(WIMG->pWorkspace->m_id), Hyprdrive::Config::themeForeground().modifyA(0.20F),
                                                         std::max(1, sc<int>(std::round(sc<float>(CELL.height) * 0.156F))), false, "", 0, 600);
                NUMTEX && NUMTEX->ok()) {
                CTexPassElement::SRenderData num;
                num.tex = NUMTEX;
                num.box = CBox{CELL.x + std::round((sc<float>(CELL.width) - sc<float>(NUMTEX->m_size.x)) / 2.F),
                               CELL.y + std::round((sc<float>(CELL.height) - sc<float>(NUMTEX->m_size.y)) / 2.F), sc<float>(NUMTEX->m_size.x),
                               sc<float>(NUMTEX->m_size.y)}
                              .round();
                num.a = FADE;
                g_pHyprRenderer->m_renderPass.add(makeUnique<CTexPassElement>(num));
            }

            // Drawn by THAT monitor's overview. It already forces these windows
            // visible for its own strip and restores them when it closes; a
            // second owner recording the forced state as "original" would put
            // the forced state back.
            for (const auto& REF : WIMG->windows) {
                const auto WINDOW = getOverviewWindowToShow(REF.lock());
                if (!overviewStripMiniShown(WINDOW, WIMG->pWorkspace) || WINDOW == DRAGGED)
                    continue;

                const auto MINI = OTHER->stripMiniBox(CELL, WINDOW, OTHERMON);
                if (MINI.width < 2 || MINI.height < 2 || MINI.intersection(CELL).empty())
                    continue;

                OTHER->renderWindowLive(monitor, WINDOW, MINI.copy().round(), MINIRENDERSCALE, now, &CELL, false, FADE);
            }

            // A drop target lights like one; the source desktop of the drag does not.
            const bool HOT = HOVER && HOVER->overview == OTHER && HOVER->workspaceIdx == j &&
                (!DRAGGED || WIMG->pWorkspace != dragContext->drag.originalWorkspace.lock());
            // Tint and outline fade in and out on Hyprland's `border` animation,
            // as a window's border does when it takes focus (BorderFade.hpp).
            const uint64_t CELLKEY = (sc<uint64_t>(monitor->m_id) << 40) ^ (sc<uint64_t>(OTHERMON->m_id) << 32) ^ sc<uint64_t>(sc<uint32_t>(WIMG->pWorkspace->m_id));
            const float    HOTMIX  = Hyprdrive::BorderFade::mix(monitor, Hyprdrive::BorderFade::key("strip-alt-hot", CELLKEY), HOT);
            if (HOTMIX > 0.F)
                renderStripCellRect(CELL, Hyprdrive::Config::highlightColor().modifyA(0.22F * FADE * HOTMIX), ATLEFT, ATRIGHT, STYLE);

            // That monitor's active desktop, marked the way its own strip marks it.
            const bool ACTIVE = OTHERMON->m_activeWorkspace && WIMG->pWorkspace->m_id == OTHERMON->m_activeWorkspace->m_id;
            renderStripCellBorder(monitor, CELL, ATLEFT, ATRIGHT, STYLE,
                                  Hyprdrive::BorderFade::mix(monitor, Hyprdrive::BorderFade::key("strip-alt-ring", CELLKEY), HOT || ACTIVE));
        }

        // Which monitor this is, over its plate, the scratchpad label's plate
        // and gap -- always, because unlike the scratchpad it has no other name.
        OverviewRender::labelPill(monitor, overviewMonitorLabel(OTHERMON), sc<float>(PLATE.x) + sc<float>(PLATE.width) / 2.F,
                                  sc<float>(PLATE.y) - OverviewRender::LABEL_PILL_GAP * monitor->m_scale, FADE, monitor->m_scale);
    }
}

void COverview::renderScratchpadCell(PHLMONITOR monitor, float renderScale, const Time::steady_tp& now, const SStripStyle& STYLE) {
    const float REVEAL = scratchpadRevealValue();
    if (!monitor || REVEAL <= 0.01F)
        return;

    const auto CELL = scratchpadCellBox(monitor, renderScale, -1.F);
    if (CELL.width <= 1 || !overviewBoxIntersectsMonitor(CELL, monitor))
        return;

    const float FADE      = STYLE.fade * REVEAL;
    const auto  WORKSPACE = scratchpadCellWorkspace();
    const auto  PARKED    = Hyprdrive::Scratchpad::windowsOn(WORKSPACE);

    // Its OWN plate. The cell is detached, so it cannot borrow the strip's --
    // which is the whole visual point: two plates read as two things, and a
    // swipe stops at the edge of the first.
    const auto PLATELOGICAL = scratchpadCellLogicalBox(-1.F).copy().expand(OVERVIEW_STRIP_PANELPADL);
    const auto PLATE        = getOverviewGlobalBox(PLATELOGICAL, monitor, renderScale, OVERVIEW_CHROME_NO_PAN, 0.F, layout);
    if (PLATE.width > 1) {
        const int PADPX      = std::max(0, sc<int>(std::round(sc<float>(CELL.x) - sc<float>(PLATE.x))));
        const int PLATEROUND = STYLE.round <= 0 ? 0 : STYLE.round + PADPX;
        OverviewRender::themePlate(PLATE, PLATEROUND, STYLE.power, FADE, true, pointerOnChrome(PLATE), Hyprdrive::BorderFade::key("strip-scratchpad", sc<uint64_t>(monitor->m_id)));
    }

    // From here down the cell is built in the SAME order renderStripCell builds
    // a numbered one -- shadow, fill, label under the previews, border last --
    // because the user's requirement is that it read as one of the strip's
    // cards, not as a panel that happens to sit beside them.
    renderOverviewWorkspaceShadow(monitor, CELL, renderScale, false, FADE);

    const bool HOT = isOverviewPointerOnMonitor(monitor) && scratchpadCellContains(lastMousePosLocal);

    // The same accent wash the strip's own cells use, so a drop target here and
    // a drop target there read as one control. A lone cell is large on both
    // ends: there is no neighbour for a tight facing corner.
    renderStripCellRect(CELL, Hyprdrive::Config::highlightColor().modifyA((HOT ? 0.30F : 0.12F) * FADE), true, true, STYLE);

    // A WORD, not a number: it is not one of the numbered desktops and must not
    // be mistaken for one. Same size and weight as a cell's workspace number
    // (0.156 of the cell height, DemiBold) -- user, 2026-09-12 -- and drawn
    // UNDER the previews exactly as the numbers are, so a full scratchpad shows
    // its windows and an empty one still says what it is. Carried at a higher
    // alpha than a number because a word is read, not just recognised.
    const float FONTPX = std::max(1.F, sc<float>(CELL.height) * 0.156F);
    if (const auto TEX = OverviewRender::text(Hyprdrive::Scratchpad::label(WORKSPACE), Hyprdrive::Config::themeForeground().modifyA(0.45F),
                                              sc<int>(std::round(FONTPX)), false, "", sc<int>(CELL.width), 600);
        TEX && TEX->ok()) {
        CTexPassElement::SRenderData label;
        label.tex = TEX;
        label.box = CBox{std::round(sc<float>(CELL.x) + (sc<float>(CELL.width) - sc<float>(TEX->m_size.x)) / 2.F),
                         std::round(sc<float>(CELL.y) + (sc<float>(CELL.height) - sc<float>(TEX->m_size.y)) / 2.F), sc<float>(TEX->m_size.x), sc<float>(TEX->m_size.y)};
        label.a   = FADE;
        g_pHyprRenderer->m_renderPass.add(makeUnique<CTexPassElement>(label));
    }

    // The parked windows, as live minis -- the same mapping, render scale and
    // retile pump the numbered cells use (renderStripCell), so a preview in
    // this cell cannot drift from a preview in those. A window being dragged is
    // skipped: it is already drawn under the pointer.
    const auto DRAGGED = getOverviewWindowToShow(drag.activeWindow.lock());
    const float MINIRENDERSCALE = renderScale * sc<float>(CELL.height) / std::max(1.F, sc<float>(monitor->m_size.y) * monitor->m_scale);

    for (const auto& WINDOW : PARKED) {
        const auto SHOWN = getOverviewWindowToShow(WINDOW);
        if (!SHOWN || SHOWN == DRAGGED || !Desktop::View::validMapped(SHOWN))
            continue;

        const auto MINI = stripMiniBox(CELL, SHOWN, monitor);
        if (MINI.width < 2 || MINI.height < 2 || MINI.intersection(CELL).empty())
            continue;

        renderWindowLive(monitor, SHOWN, MINI.copy().round(), MINIRENDERSCALE, now, &CELL, false, FADE);
        Hyprdrive::Dock::pumpRetiled(SHOWN, monitor, now);
    }

    // After the minis, so the target reads as a target even over a cell full of
    // previews -- the same reason renderStripCell borders a hovered cell last.
    renderStripCellBorder(monitor, CELL, true, true, STYLE);

    // A label plate ABOVE the cell -- OverviewRender::labelPill, the very plate
    // the dock's icon labels use, so the two are one piece of chrome (user,
    // 2026-09-12). Only while something is parked: an empty cell already says
    // what it is in the middle of itself, and a plate over nothing would be the
    // clutter the hover zone exists to avoid.
    //
    // It floats above the PLATE by the dock tooltip's own gap (user,
    // 2026-09-14). Anchored on the cell it sat flush on the plate's top edge:
    // the cell is inset from the plate by exactly the panel padding it was
    // lifted by.
    if (!PARKED.empty()) {
        const float TOP = PLATE.width > 1 ? sc<float>(PLATE.y) : sc<float>(CELL.y) - OVERVIEW_STRIP_PANELPADL * monitor->m_scale;
        OverviewRender::labelPill(monitor, Hyprdrive::Scratchpad::label(WORKSPACE), sc<float>(CELL.x) + sc<float>(CELL.width) / 2.F,
                                  TOP - OverviewRender::LABEL_PILL_GAP * monitor->m_scale, FADE, monitor->m_scale);
    }
}

// X22: screen-fixed. The strip used to ride the active card, which meant a
// desktop pan carried it clean off the monitor along with the search bar.
// The order the cards are drawn in: the row's, except that a desktop being
// born slides out from BENEATH the one it came from.
// DEPTH: how far behind a card is, from how much it overlaps the card it
// passes under -- its current offset against that card's, in pitches. Fully
// under it, the card is DEPTH_SHRINK smaller; a pitch away, at rest. One rule
// for both: a fullscreen app's new desktop starts beneath the one it came out
// of and grows as that one slides off it; a desktop moved along the strip
// shrinks as it crosses behind the desktop in view and grows back past it.
size_t COverview::depthAnchor() const {
    if (!workspaceInsertTransition.active || !workspaceInsertTransition.reorder || workspaceInsertProgress->value() >= 1.F)
        return SIZE_MAX;
    const size_t A = activeWorkspaceIndex();
    return A < images.size() ? A : SIZE_MAX;
}

float COverview::cardDepth(size_t workspaceIdx) const {
    if (!workspaceInsertTransition.active || workspaceInsertProgress->value() >= 1.F || workspaceIdx >= images.size() || !images[workspaceIdx] ||
        !images[workspaceIdx]->pWorkspace)
        return 1.F;
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR)
        return 1.F;
    const float  PITCH  = getWorkspaceRenderedPitch(MONITOR, scale->value(), layout);
    const size_t ACTIVE = activeWorkspaceIndex();
    if (PITCH < 1.F)
        return 1.F;
    const auto under = [&](size_t other) {
        const float A = workspaceOverviewOffset(workspaceIdx, ACTIVE, PITCH), B = workspaceOverviewOffset(other, ACTIVE, PITCH);
        const float O = std::clamp(1.F - std::abs(A - B) / PITCH, 0.F, 1.F);
        return 1.F - DEPTH_SHRINK * O * O * (3.F - 2.F * O); // smoothstep: eases in and out of the dip
    };
    if (workspaceInsertTransition.bornUnder != WORKSPACE_INVALID) {
        if (workspaceKey(images[workspaceIdx]->pWorkspace) != workspaceInsertTransition.transitionWorkspaceID)
            return 1.F;
        for (size_t i = 0; i < images.size(); ++i)
            if (images[i] && workspaceKey(images[i]->pWorkspace) == workspaceInsertTransition.bornUnder)
                return under(i);
        return 1.F;
    }
    if (workspaceInsertTransition.reorder && workspaceIdx != ACTIVE && ACTIVE < images.size())
        return under(ACTIVE);
    return 1.F;
}

std::vector<size_t> COverview::cardOrder() const {
    std::vector<size_t> order(images.size());
    std::iota(order.begin(), order.end(), size_t{0});
    if (!workspaceInsertTransition.active || workspaceInsertTransition.bornUnder == WORKSPACE_INVALID || workspaceInsertProgress->value() >= 1.F)
        return order;
    size_t born = SIZE_MAX, under = SIZE_MAX;
    for (size_t i = 0; i < images.size(); ++i) {
        if (!images[i] || !images[i]->pWorkspace)
            continue;
        const auto KEY = workspaceKey(images[i]->pWorkspace);
        if (KEY == workspaceInsertTransition.transitionWorkspaceID)
            born = i;
        if (KEY == workspaceInsertTransition.bornUnder)
            under = i;
    }
    if (born != SIZE_MAX && under != SIZE_MAX && born > under) {
        order.erase(order.begin() + sc<std::ptrdiff_t>(born));
        order.insert(order.begin() + sc<std::ptrdiff_t>(under), born);
    }
    return order;
}

CBox COverview::stripCellBox(size_t workspaceIdx, PHLMONITOR monitor, float renderScale) const {
    auto LOGICAL = stripCellLogicalBox(workspaceIdx);
    // A desktop insert/remove: the cells slide from where they were.
    if (workspaceInsertTransition.active && LOGICAL.width > 0 && workspaceIdx < images.size() && images[workspaceIdx] && images[workspaceIdx]->pWorkspace)
        if (const auto IT = workspaceInsertTransition.oldStripBoxes.find(workspaceKey(images[workspaceIdx]->pWorkspace)); IT != workspaceInsertTransition.oldStripBoxes.end()) {
            const float T   = std::clamp(workspaceInsertProgress->value(), 0.F, 1.F);
            const CBox& OLD = IT->second;
            LOGICAL         = CBox{OLD.x + (LOGICAL.x - OLD.x) * T, OLD.y + (LOGICAL.y - OLD.y) * T, OLD.width + (LOGICAL.width - OLD.width) * T,
                                   OLD.height + (LOGICAL.height - OLD.height) * T};
        }
    if (!monitor || LOGICAL.width <= 0)
        return {};

    return getOverviewGlobalBox(LOGICAL, monitor, renderScale, OVERVIEW_CHROME_NO_PAN, 0.F, layout);
}

// The strip as one surface: its plate (the cells, the gaps between them and
// the panel padding -- built exactly as renderStripPanel builds it), the
// scratchpad cell, and the other monitors' strips.
// Is the pointer on this chrome box (monitor-local, as lastMousePosLocal)?
// lastMousePosLocal keeps its last value when the pointer leaves for another
// monitor, so the pointer has to be on this one first.
bool COverview::pointerOnChrome(const CBox& box) const {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR || !g_pInputManager || box.width <= 1 || !MONITOR->logicalBox().containsPoint(g_pInputManager->getMouseCoordsInternal()))
        return false;
    return box.containsPoint(lastMousePosLocal);
}

bool COverview::stripUiContains(const Vector2D& point) const {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR || overviewStripLogicalHeight(MONITOR) <= 0.F || images.empty())
        return false;

    const auto FIRST = stripCellLogicalBox(0);
    const auto LAST  = stripCellLogicalBox(images.size() - 1);
    if (FIRST.width > 1 && LAST.width > 1) {
        constexpr float PAD = OVERVIEW_STRIP_PANELPADL;
        const CBox      PANELLOGICAL{FIRST.x - PAD, FIRST.y - PAD, sc<float>(LAST.x) + sc<float>(LAST.width) - sc<float>(FIRST.x) + 2.F * PAD,
                                sc<float>(FIRST.height) + 2.F * PAD};
        if (getOverviewGlobalBox(PANELLOGICAL, MONITOR, scale->value(), OVERVIEW_CHROME_NO_PAN, 0.F, layout).containsPoint(point))
            return true;
    }

    return scratchpadCellContains(point) || altCellAt(point).has_value();
}

PHLWORKSPACE COverview::workspaceAtStripPoint(const Vector2D& point, size_t* hoveredWorkspaceIdx) const {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR || overviewStripLogicalHeight(MONITOR) <= 0.F)
        return nullptr;

    for (size_t workspaceIdx = 0; workspaceIdx < images.size(); ++workspaceIdx) {
        const auto& wimg = images[workspaceIdx];
        if (!wimg || !wimg->pWorkspace)
            continue;

        if (!stripCellBox(workspaceIdx, MONITOR, scale->value()).containsPoint(point))
            continue;

        if (hoveredWorkspaceIdx)
            *hoveredWorkspaceIdx = workspaceIdx;

        return wimg->pWorkspace;
    }

    return nullptr;
}

PHLWORKSPACE COverview::stripWorkspaceAt(const Vector2D& point) const {
    return workspaceAtStripPoint(point);
}

// Minis map the window's REAL desktop rect into the cell, not its grid slot: a
// thumbnail of the desktop, not a second copy of the grid. Shared by the render
// and the D6 hit-test so the two can never disagree about where a mini is.
CBox COverview::stripMiniBox(const CBox& cell, PHLWINDOW window, PHLMONITOR monitor) const {
    if (!window || !monitor || cell.width <= 0 || cell.height <= 0)
        return {};

    const float SCALEY = sc<float>(cell.height) / std::max(1.F, sc<float>(monitor->m_size.y));
    const auto  GEOM   = window->geometricBox(Desktop::View::IGeometric::GEOMETRIC_CURRENT);

    const auto WORKSPACE = window->m_workspace;

    // A Mosaic desktop on its infinite canvas: the whole canvas, with the
    // screen it pans across, zoomed out into the cell at one scale and centred
    // -- every window on it shows, as the tape's minis do for a scrolling one.
    if (const auto CANVAS = Hyprdrive::Mosaic::canvasBox(WORKSPACE)) {
        const CBox   MON{monitor->m_position, monitor->m_size};
        const double X0 = std::min(CANVAS->x, MON.x), Y0 = std::min(CANVAS->y, MON.y);
        const double W  = std::max(CANVAS->x + CANVAS->w, MON.x + MON.w) - X0, H = std::max(CANVAS->y + CANVAS->h, MON.y + MON.h) - Y0;
        const double S  = std::min(cell.width / std::max(1.0, W), cell.height / std::max(1.0, H));
        const double OX = cell.x + (cell.width - W * S) / 2.0, OY = cell.y + (cell.height - H * S) / 2.0;
        return CBox{OX + (GEOM.x - X0) * S, OY + (GEOM.y - Y0) * S, GEOM.w * S, GEOM.h * S};
    }

    const float SPAN_RATIO = workspaceScrollingSpanRatio(WORKSPACE, monitor);

    if (SPAN_RATIO > 1.0F) {
        const auto ALGO = overviewScrollingAlgorithmForWorkspace(WORKSPACE);
        Vector2D cameraTrans{};
        if (ALGO && ALGO->m_scrollingData && ALGO->m_scrollingData->controller)
            cameraTrans = ALGO->m_scrollingData->controller->getCameraTranslation(ALGO->usableArea());

        const float VIRTUAL_WIDTH = sc<float>(monitor->m_size.x) * SPAN_RATIO;
        const float SCALEX = sc<float>(cell.width) / std::max(1.F, VIRTUAL_WIDTH);
        const float UNTRANSLATED_X = sc<float>(GEOM.x) - sc<float>(cameraTrans.x);

        return CBox{
            sc<float>(cell.x) + (UNTRANSLATED_X - sc<float>(monitor->m_position.x)) * SCALEX,
            sc<float>(cell.y) + (sc<float>(GEOM.y) - sc<float>(monitor->m_position.y)) * SCALEY,
            sc<float>(GEOM.w) * SCALEX,
            sc<float>(GEOM.h) * SCALEY,
        };
    }

    const float SCALEX = sc<float>(cell.width) / std::max(1.F, sc<float>(monitor->m_size.x));
    return CBox{
        sc<float>(cell.x) + (sc<float>(GEOM.x) - sc<float>(monitor->m_position.x)) * SCALEX,
        sc<float>(cell.y) + (sc<float>(GEOM.y) - sc<float>(monitor->m_position.y)) * SCALEY,
        sc<float>(GEOM.w) * SCALEX,
        sc<float>(GEOM.h) * SCALEY,
    };
}

// D6: which window mini is under the cursor, topmost first.
PHLWINDOW COverview::windowAtStripPoint(const Vector2D& point, CBox* miniBox) const {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR || overviewStripLogicalHeight(MONITOR) <= 0.F)
        return nullptr;

    // The detached scratchpad cell answers here too, FIRST, because it is the
    // only way a window gets back OUT of the scratchpad by hand: D6 picks up
    // the mini a drag starts on, and with the cell absent from this walk a
    // press on one of its previews picked up whatever main tile the band floats
    // over instead -- so a parked window could not be dragged out at all
    // (user, 2026-09-12). Topmost first, same as the numbered cells below.
    if (scratchpadRevealValue() > 0.5F) {
        const auto CELL = scratchpadCellBox(MONITOR, scale->value(), -1.F);
        if (CELL.width > 1 && CELL.containsPoint(point)) {
            const auto PARKED = Hyprdrive::Scratchpad::windowsOn(scratchpadCellWorkspace());

            for (auto it = PARKED.rbegin(); it != PARKED.rend(); ++it) {
                const auto WINDOW = getOverviewWindowToShow(*it);
                if (!WINDOW || !Desktop::View::validMapped(WINDOW))
                    continue;

                const auto MINI = stripMiniBox(CELL, WINDOW, MONITOR);
                if (MINI.width < 2 || MINI.height < 2 || !MINI.containsPoint(point))
                    continue;

                if (miniBox)
                    *miniBox = MINI;

                return WINDOW;
            }

            // On the cell but not on a preview: the click belongs to the cell
            // (it opens the scratchpad), never to a tile behind the band.
            return nullptr;
        }
    }

    // The other monitors' strips, the same way and for the same reason: a
    // press on one of their previews picks THAT window up, so it can be dragged
    // over to this monitor.
    if (const auto HIT = altCellAt(point)) {
        const auto& WIMG = HIT->overview->images[HIT->workspaceIdx];
        for (auto it = WIMG->windows.rbegin(); it != WIMG->windows.rend(); ++it) {
            const auto WINDOW = getOverviewWindowToShow(it->lock());
            if (!overviewStripMiniShown(WINDOW, WIMG->pWorkspace))
                continue;

            const auto MINI = HIT->overview->stripMiniBox(HIT->cell, WINDOW, HIT->monitor);
            if (MINI.width < 2 || MINI.height < 2 || !MINI.containsPoint(point))
                continue;

            if (miniBox)
                *miniBox = MINI;

            return WINDOW;
        }

        return nullptr;
    }

    for (size_t workspaceIdx = 0; workspaceIdx < images.size(); ++workspaceIdx) {
        const auto& wimg = images[workspaceIdx];
        if (!wimg || !wimg->pWorkspace)
            continue;

        const auto CELL = stripCellBox(workspaceIdx, MONITOR, scale->value());
        if (CELL.width <= 1 || !CELL.containsPoint(point))
            continue;

        for (auto it = wimg->windows.rbegin(); it != wimg->windows.rend(); ++it) {
            const auto WINDOW = getOverviewWindowToShow(it->lock());
            if (!overviewStripMiniShown(WINDOW, wimg->pWorkspace))
                continue;

            const auto MINI = stripMiniBox(CELL, WINDOW, MONITOR);
            if (MINI.width < 2 || MINI.height < 2 || !MINI.containsPoint(point))
                continue;

            if (miniBox)
                *miniBox = MINI;

            return WINDOW;
        }

        return nullptr;
    }

    return nullptr;
}

// X3: the reference wraps the whole strip in ONE rounded container panel
// (ii/modules/ii/overview/OverviewWidget.qml:126-135) -- padding 10, radius
// largeWorkspaceRadius + padding, a translucent surface colour. Drawn once,
// before the cells, so the cells read as sitting on it.
void COverview::renderStripPanel(PHLMONITOR monitor, float renderScale, const SStripStyle& STYLE) {
    const float FADE  = STYLE.fade;
    const int   ROUND = STYLE.round;

    if (!images.empty()) {
        const auto FIRSTLOGICAL = stripCellLogicalBox(0);
        const auto LASTLOGICAL  = stripCellLogicalBox(images.size() - 1);

        if (FIRSTLOGICAL.width > 1 && LASTLOGICAL.width > 1) {
            // ponytail: absolute logical px, not a fraction of the cell. E8-c
            // already settled that the theme's absolute radii are used unshrunk;
            // a padding derived from them has to follow the same rule or the
            // panel's curve stops being concentric with the corner cells.
            constexpr float PANELPAD = OVERVIEW_STRIP_PANELPADL;

            const CBox PANELLOGICAL = {
                FIRSTLOGICAL.x - PANELPAD,
                FIRSTLOGICAL.y - PANELPAD,
                sc<float>(LASTLOGICAL.x) + sc<float>(LASTLOGICAL.width) - sc<float>(FIRSTLOGICAL.x) + 2.F * PANELPAD,
                sc<float>(FIRSTLOGICAL.height) + 2.F * PANELPAD,
            };

            const auto PANEL = getOverviewGlobalBox(PANELLOGICAL, monitor, renderScale, OVERVIEW_CHROME_NO_PAN, 0.F, layout);

            if (PANEL.width > 1 && overviewBoxIntersectsMonitor(PANEL, monitor)) {
                // Radius in RENDER space = the cells' radius plus the padding as
                // it actually landed after the transform. Measuring the gap off
                // the transformed boxes avoids duplicating the scale maths.
                // (The resting box, not stripCellBox: that one glides during a
                // transition, and a first desktop moving away turned the gap --
                // and the radius -- into a stadium.)
                const auto FIRSTCELL = getOverviewGlobalBox(FIRSTLOGICAL, monitor, renderScale, OVERVIEW_CHROME_NO_PAN, 0.F, layout);
                // Concentric with the cells only when the theme actually rounds.
                // Omarchy's squared default (decoration:rounding 0) must stay
                // squared; inflating by the padding used to force a curve.
                const int PADPX      = std::max(0, sc<int>(std::round(sc<float>(FIRSTCELL.x) - sc<float>(PANEL.x))));
                const int PANELROUND = ROUND <= 0 ? 0 : ROUND + PADPX;

                // Same plate as the dock (Omarchy terminal fill, accent border,
                // frost only if window blur is on). Rounding is the theme's,
                // not the dock's Dash-to-Dock 24.
                // Lit only while the pointer is on it or Tab has the keyboard
                // in it; otherwise the resting tile border (user, 2026-09-27).
                const bool LIT = (g_search.nav.zone == EOverviewNavZone::Desktop && activeScrollOverview().get() == this) || pointerOnChrome(PANEL);
                OverviewRender::themePlate(PANEL, PANELROUND, STYLE.power, FADE, true, LIT, Hyprdrive::BorderFade::key("strip-desktops", sc<uint64_t>(monitor->m_id)));
            }
        }
    }
}

// X3: the reference rounds only the GRID's outer corners at the large
// radius and every inner corner at verysmall (OverviewWidget.qml:171-175).
// The strip is one row, so cell 0 is large on its left corners, the last
// cell large on its right, and every facing corner is tight.
//
// CRectPassElement carries ONE radius, so the asymmetry is drawn as two
// rects of the SAME box clipped to opposite halves. The clips do not
// overlap, which is the point: a translucent backdrop drawn twice over the
// same pixel would blend to the wrong alpha.
void COverview::renderStripCellRect(const CBox& cell, const CHyprColor& color, bool largeLeft, bool largeRight, const SStripStyle& STYLE) const {
    if (largeLeft == largeRight) {
        CRectPassElement::SRectData r;
        r.box           = cell;
        r.color         = color;
        r.round         = largeLeft ? STYLE.round : STYLE.innerRound;
        r.roundingPower = STYLE.power;
        g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(r));
        return;
    }

    const float MID = sc<float>(cell.x) + std::round(sc<float>(cell.width) / 2.F);

    for (int half = 0; half < 2; ++half) {
        const bool LEFT = half == 0;

        CRectPassElement::SRectData r;
        r.box           = cell;
        r.color         = color;
        r.round         = (LEFT ? largeLeft : largeRight) ? STYLE.round : STYLE.innerRound;
        r.roundingPower = STYLE.power;
        r.clipBox       = LEFT ? CBox{sc<float>(cell.x), sc<float>(cell.y), MID - sc<float>(cell.x), sc<float>(cell.height)} :
                                 CBox{MID, sc<float>(cell.y), sc<float>(cell.x) + sc<float>(cell.width) - MID, sc<float>(cell.height)};
        g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(r));
    }
}

    // X3: a 2px accent border on a cell, the reference's border.width 2. Used
    // both for the focused desktop (OverviewWidget.qml:432-433) and for the
    // drag-hover state (OverviewWidget.qml:179-180) -- one function so the two
    // cannot drift apart.
    // BUG (user, 2026-08-02): this used ONE radius for every cell, so the
    // outline traced a shape the cell underneath does not have -- large curves
    // over the tight facing corners of an inner cell, and the wrong radius on
    // the inside edge of each end cell. It takes the SAME largeLeft/largeRight
    // the fill does, and COverviewSplitBorderPassElement draws the asymmetric
    // case as two damage-clipped halves.
    // (user, 2026-08-03): the stroke sits on the cell's INNER edge, not around
    // its outside.
    // X56 (user, 2026-08-06): and it is the THEME's accent again, not the flat
    // white that instruction had also asked for -- the strip outline, the dock
    // highlight and the tile ring now all read general:col.active_border, so a
    // theme switch moves them together. The 2026-08-03 white is superseded
    // here deliberately; do not restore it as a regression fix.
// `mix` is the cell's BorderFade: a cell at rest has no outline, so its fade is
// the outline's alpha.
void COverview::renderStripCellBorder(PHLMONITOR monitor, const CBox& cell, bool largeLeft, bool largeRight, const SStripStyle& STYLE, float mix) const {
    if (mix <= 0.F)
        return;
    const auto OUTLINE = Hyprdrive::Config::highlightColor();
    const int  BORDER  = std::max(1, Hyprdrive::Config::getValue<int>("general:border_size"));
    // Inset by the full stroke width: renderBorder centres nothing, it grows
    // outward from the box, so the box has to be the INNER rectangle for the
    // stroke to land inside the tile. Radii shrink with it or the curve would
    // no longer be concentric with the fill's.
    const CBox INNER = cell.copy().expand(-BORDER);

    g_pHyprRenderer->m_renderPass.add(makeUnique<COverviewSplitBorderPassElement>(COverviewSplitBorderPassElement::SData{
        .box           = INNER,
        .color         = ::Config::CGradientValueData{OUTLINE.modifyA(1.F)},
        .roundLeft     = std::max(0, (largeLeft ? STYLE.round : STYLE.innerRound) - BORDER),
        .roundRight    = std::max(0, (largeRight ? STYLE.round : STYLE.innerRound) - BORDER),
        .roundingPower = STYLE.power,
        .borderSize    = BORDER,
        .a             = STYLE.fade * mix,
    }));
}

// One strip cell: backdrop, workspace number, the minis of every window on it,
// and the hover/active outlines. Drawn in this order for a reason -- the
// outlines have to land on top of a cell full of previews.
void COverview::renderStripCell(PHLMONITOR monitor, size_t workspaceIdx, const CBox& CELL, const PHLWINDOW& DRAGGED, bool hovered, float renderScale,
                                      const Time::steady_tp& now, const SStripStyle& STYLE) {
    const auto& wimg = images[workspaceIdx];
    const float FADE = STYLE.fade;

    renderOverviewWorkspaceShadow(monitor, CELL, renderScale, false, FADE);

    // (A cell gliding to another place takes that place's corners; the lifted
    // one is a card of its own, every corner round.)
    const bool ATLEFT  = stripCellEdges >= 0 ? (stripCellEdges & 1) != 0 : workspaceIdx == 0;
    const bool ATRIGHT = stripCellEdges >= 0 ? (stripCellEdges & 2) != 0 : workspaceIdx + 1 == images.size();

    // Resting fill is the strip panel (the same Omarchy terminal plate as the
    // dock). Only hover tints the cell itself.

    // X3: the deprecated overview centres a large, faint workspace number in
    // every cell (ii/modules/ii/overview/OverviewWidget.qml:182-193): 250px
    // type at the overview's own scale, i.e. 250/1600 = 15.6% of a cell's
    // height, DemiBold, foreground at alpha 0.2. Reproduced as a fraction of
    // the strip cell so it tracks the strip instead of a fixed pixel size.
    const auto NUMTEX = OverviewRender::text(std::to_string(wimg->pWorkspace->m_id), Hyprdrive::Config::themeForeground().modifyA(0.20F),
                                                    std::max(1, sc<int>(std::round(sc<float>(CELL.height) * 0.156F))), false, "", 0, 600);
    // Only the cell the badge is sitting on fades, and only while it is there.
    const float NUMFADE = restingNumberFade(workspaceIdx);
    // Faded fully out: no quad, no pass element.
    if (NUMFADE > 0.01F && NUMTEX && NUMTEX->ok()) {
        CBox numBox = {
            CELL.x + std::round((sc<float>(CELL.width) - sc<float>(NUMTEX->m_size.x)) / 2.F),
            CELL.y + std::round((sc<float>(CELL.height) - sc<float>(NUMTEX->m_size.y)) / 2.F),
            sc<float>(NUMTEX->m_size.x),
            sc<float>(NUMTEX->m_size.y),
        };

        CTexPassElement::SRenderData num;
        num.tex = NUMTEX;
        num.box = numBox.round();
        num.a   = FADE * NUMFADE;
        g_pHyprRenderer->m_renderPass.add(makeUnique<CTexPassElement>(num));
    }

    const float MINIRENDERSCALE = renderScale * sc<float>(CELL.height) / std::max(1.F, sc<float>(monitor->m_size.y) * monitor->m_scale);

    for (const auto& windowRef : wimg->windows) {
        const auto WINDOW = getOverviewWindowToShow(windowRef.lock());
        if (!overviewStripMiniShown(WINDOW, wimg->pWorkspace) || WINDOW == DRAGGED)
            continue;

        const auto MINI = stripMiniBox(CELL, WINDOW, monitor);

        if (MINI.width < 2 || MINI.height < 2 || MINI.intersection(CELL).empty())
            continue;

        renderWindowLive(monitor, WINDOW, MINI.copy().round(), MINIRENDERSCALE, now, &CELL, false, FADE);

        // A window retiled onto a desktop nobody is looking at has a new size and
        // an old buffer. Drawing it here shows that old buffer letterboxed inside
        // the new frame; this is what actually asks the client to redraw, for the
        // length of its grace and no longer.
        Hyprdrive::Dock::pumpRetiled(WINDOW, monitor, now);
    }

    // Tint and outline fade in and out on Hyprland's `border` animation, as a
    // window's border does when it takes focus (BorderFade.hpp).
    // Keyed by the desktop itself, not its number: a desktop moved along the
    // strip keeps its tint and ring through the renumbering.
    const uint64_t CELLKEY = (sc<uint64_t>(monitor->m_id) << 32) ^ sc<uint64_t>(workspaceKey(wimg->pWorkspace));
    if (const float HOVERMIX = Hyprdrive::BorderFade::mix(monitor, Hyprdrive::BorderFade::key("strip-hover", CELLKEY), hovered); HOVERMIX > 0.F) {
        // X4: the hover tint is themed, from the same general:col.active_border
        // that DropIndicator already reads. White is only the fallback for a
        // theme that defines no gradient.
        const auto ACCENT = Hyprdrive::Config::highlightColor();

        renderStripCellRect(CELL, ACCENT.modifyA(0.22F * FADE * HOVERMIX), ATLEFT, ATRIGHT, STYLE);
    }

    // X3: the reference marks the focused desktop with a 2px accent border
    // drawn over the windows (OverviewWidget.qml:414-436, colSecondary).
    // The accent here is general:col.active_border -- the same source the
    // hover tint above reads, so both follow the theme (X4).
    // ponytail: no slide between cells. The reference animates x/y because
    // its indicator is one moving item; here the cell rects are already
    // laid out, so the border simply draws on whichever cell is active.
    // ponytail: the compositor's active workspace does not move until a
    // 3-finger pan COMMITS on release, so reading it here left the strip
    // marking the desktop the pan started from for the whole gesture -- the
    // one part of the overview that visibly did not follow the fingers.
    // Follow the detent while one is running; it is the same index the card
    // ring and the badge use, so all three move together.
    const bool ISACTIVE = trackpadDetentActive ? workspaceIdx == trackpadDetentWorkspace :
                                                 (monitor->m_activeWorkspace && wimg->pWorkspace->m_id == monitor->m_activeWorkspace->m_id);
    // X3 delta 3: the reference borders the hovered cell as well as tinting
    // it. One outline for both: drawn after the tint and after the minis, so
    // the drop target reads as a target even over a cell full of previews.
    renderStripCellBorder(monitor, CELL, ATLEFT, ATRIGHT, STYLE,
                          Hyprdrive::BorderFade::mix(monitor, Hyprdrive::BorderFade::key("strip-ring", CELLKEY), hovered || ISACTIVE));
}

namespace {

    // The strip cell already carries a faint resting number at 15.6% of its
    // height. The badge is the same glyph at the size the gesture deserves --
    // near the full cell -- so the two read as one thing turning up rather
    // than two numbers on one card.
    constexpr float DETENT_BADGE_FRACTION = 0.70F;
    // How solid the coloured glyph sits ON TOP of its own frost. Below 1 so the
    // blurred backdrop reads through the digit -- that is what makes it frosted
    // glass rather than a flat number.
    constexpr float DETENT_BADGE_GLYPH_A  = 0.62F;
    // How long the resting numbers take to get out of the way and come back.
    // Shorter than the badge's own 420ms entrance on purpose: the cell should
    // already be clear by the time the popped number is readable, otherwise the
    // two are briefly legible at once, which is the juxtaposition being avoided.
    constexpr float RESTING_NUMBER_FADE_MS = 160.F;

    // The animation clock (the speed setting; 0 snaps), as is the badge's pop.
    // How long a keyboard badge stays up (KEY_BADGE_MS) is a hold: real time.
    float restingNumberEased(const Hyprdrive::Motion::STween<Time::steady_tp>& fade) {
        return SearchLetters::easeOutCubic(SearchLetters::clamp01(Hyprdrive::Motion::Clock::animMs(fade.start) / RESTING_NUMBER_FADE_MS));
    }
}

// How long a keyboard-popped number stays up after the last step.
static constexpr float KEY_BADGE_MS = 900.F;

void COverview::popKeyboardBadge(size_t workspaceIdx) {
    if (keyBadgeLive && keyBadgeWorkspace == workspaceIdx)
        return;
    keyBadgeWorkspace = workspaceIdx;
    keyBadgeBornAt    = Time::steadyNow();
    keyBadgeLive      = true;
    damage();
    requestInputFrame();
}

size_t COverview::badgeWorkspace() const {
    return trackpadDetentActive ? trackpadDetentWorkspace : keyBadgeWorkspace;
}

Time::steady_tp COverview::badgeBornAt() const {
    return trackpadDetentActive ? trackpadDetentBornAt : keyBadgeBornAt;
}

bool COverview::detentBadgeVisible() const {
    const bool   KEYS = keyBadgeLive && Hyprdrive::Motion::msBetween(Time::steadyNow(), keyBadgeBornAt) < KEY_BADGE_MS;
    const size_t IDX  = badgeWorkspace();
    return (trackpadDetentActive || KEYS) && Hyprdrive::Config::getGestureDetentLabel() && IDX < images.size() && images[IDX] && images[IDX]->pWorkspace;
}

void COverview::updateRestingFade() {
    const size_t NONE   = std::numeric_limits<size_t>::max();
    const size_t TARGET = detentBadgeVisible() ? badgeWorkspace() : NONE;
    // A keyboard badge times out without any input to ask for the frame that
    // takes it down.
    if (keyBadgeLive) {
        if (Hyprdrive::Motion::msBetween(Time::steadyNow(), keyBadgeBornAt) >= KEY_BADGE_MS)
            keyBadgeLive = false;
        else
            requestInputFrame();
    }

    // The cell being vacated fades back IN from where it is, so the number the
    // badge just uncovered comes back rather than snapping on behind it.
    restingFade.moveTo(TARGET == NONE ? decltype(restingFade)::NONE : sc<long>(TARGET), Time::steadyNow(), restingNumberEased);

    // Mid-fade the compositor has no other reason to draw -- the fade BACK in
    // runs after the gesture has ended, so there is no finger left to schedule
    // frames. Same pairing renderDetentBadge and renderSearch use.
    if (restingFade.anyRunning([](const auto& fade) { return Hyprdrive::Motion::Clock::animMs(fade.start) < RESTING_NUMBER_FADE_MS; })) {
        damage();
        requestInputFrame();
    }
}

float COverview::restingNumberFade(size_t workspaceIdx) const {
    // The badged cell goes 1 -> 0; one just vacated comes back to 1.
    return restingFade.level(sc<long>(workspaceIdx), restingNumberEased);
}

void COverview::renderDetentBadge(PHLMONITOR monitor, float renderScale) {
    if (!monitor || !detentBadgeVisible())
        return;

    // No strip, nothing to draw on.
    if (overviewStripLogicalHeight(monitor) <= 0.F)
        return;

    // Same driver the strip itself fades on, so the badge cannot outlive the
    // row it is sitting on when the overview is toggled shut mid-drag.
    const float FADE = overviewGridProgress(renderScale);
    if (FADE < 0.01F)
        return;

    const auto& wimg = images[badgeWorkspace()];

    const auto CELL = stripCellBox(badgeWorkspace(), monitor, renderScale);
    if (CELL.width < 1.0 || CELL.height < 1.0)
        return;

    const auto NAME = wimg->pWorkspace->m_name.empty() ? std::to_string(wimg->pWorkspace->m_id) : wimg->pWorkspace->m_name;
    const int  SIZE = std::max(1, sc<int>(std::round(sc<float>(CELL.height) * DETENT_BADGE_FRACTION)));
    const auto TEX  = OverviewRender::text(NAME, Hyprdrive::Config::themeForeground().modifyA(1.F), SIZE, false, "", 0, 700);
    if (!TEX || !TEX->ok())
        return;

    const float MS   = Hyprdrive::Motion::Clock::animMs(badgeBornAt());
    const auto  POSE = SearchLetters::entrancePose(MS);

    // Scale about the glyph's own centre; POSE.y is logical px, +down, so it
    // takes the render scale exactly like the search field's letters do.
    const float TW = sc<float>(TEX->m_size.x) * POSE.scale;
    const float TH = sc<float>(TEX->m_size.y) * POSE.scale;

    const double CX = CELL.x + CELL.width / 2.0;
    const double CY = CELL.y + CELL.height / 2.0 + sc<double>(POSE.y) * sc<double>(renderScale);

    const float A = FADE * POSE.opacity;

    // FROST THE NUMBER ITSELF -- no plate, no box.
    //
    // blurAlphaMatte masks the blur to a texture's ALPHA, so handing it the
    // glyph makes the frost take the shape of the digit rather than a rectangle
    // behind it. blurredBG is the same precomputed overview blur the strip
    // panel and the search chrome sample (captured before the strip, so it is
    // already on hand here). The glyph is then composited over its own frost at
    // DETENT_BADGE_GLYPH_A, which is what keeps the theme colour on it.
    //
    // blurA is full while `a` is not: the frost stays solid while the colour
    // layer stays translucent, so the digit reads as glass rather than paint.
    CTexPassElement::SRenderData label;
    label.tex                   = TEX;
    label.box                   = CBox{std::round(CX - TW / 2.0), std::round(CY - TH / 2.0), TW, TH};
    label.a                     = A * DETENT_BADGE_GLYPH_A;
    label.blur                  = true;
    label.blurA                 = A;
    label.blurredBG             = OverviewRender::frameChromeBlur();
    label.blurAlphaMatte        = TEX;
    // The badge is drawn after the strip, so the optimised (precomputed) path
    // would sample a capture that predates it. Force the real thing.
    label.blockBlurOptimization = true;
    // blurAlphaMatte alone left the blur covering the glyph's bounding BOX
    // (photographed: the page text behind smeared in a rectangle). Discarding
    // fully-transparent fragments is the other half -- it should clip both the
    // texture and the blur under it to the inked pixels.
    label.discardActive         = true;
    label.discardMode           = DISCARD_ALPHA;
    label.discardOpacity        = 0.f;
    label.ignoreAlpha           = 0.f;
    g_pHyprRenderer->m_renderPass.add(makeUnique<CTexPassElement>(std::move(label)));

    // The pop is timestamped, not a PHLANIMVAR: if the finger stops mid-drag
    // the compositor has no reason to schedule the rest of it. Same pairing
    // renderSearch uses for the letter bounce.
    if (MS < SearchLetters::ENTRANCE_MS) {
        damage();
        requestInputFrame();
    }
}

// The strip: one container panel, then one pass per cell. The style the two
// share is resolved once here; nothing else about the order moved.
void COverview::renderStrip(PHLMONITOR monitor, float renderScale, const Time::steady_tp& now) {
    if (!monitor || overviewStripLogicalHeight(monitor) <= 0.F)
        return;

    // Ahead of every box below: the reveal is what the strip's own layout reads
    // to know how much room to leave, so it has to settle before the cells do.
    updateScratchpadReveal();
    updateAltStripsReveal();

    // User, 2026-08-06: the strip FADES in, and a toggle halfway through
    // reverses the fade. Driven off the open scale the overview already
    // animates, so the reversal is not a second state that could disagree with
    // the first -- the close retargets that scale from wherever it is and this
    // follows. Everything the strip draws is multiplied by it, minis included.
    const float FADE = overviewGridProgress(renderScale);
    if (FADE < 0.01F)
        return;

    auto*      dragContext = g_pointerGrabOverview && g_pointerGrabOverview->drag.activeWindow ? g_pointerGrabOverview : this;
    const auto DRAGGED     = getOverviewWindowToShow(dragContext->drag.activeWindow.lock());

    size_t hoveredIdx = images.size();
    if (DRAGGED && isOverviewPointerOnMonitor(monitor)) {
        const auto HOVERED = workspaceAtStripPoint(lastMousePosLocal, &hoveredIdx);

        // X3 delta 5: the reference sets hoveredWhileDragging only when the cell is
        // NOT the drag's own source workspace (OverviewWidget.qml:215). Dropping a
        // window back where it came from is a no-op, so it must not light up as a
        // target. Only the HIGHLIGHT is suppressed -- the cell stays a live drop
        // target, exactly as the reference still claims it.
        if (HOVERED && HOVERED == dragContext->drag.originalWorkspace.lock())
            hoveredIdx = images.size();
    }

    const auto  CARDROUND = getOverviewCardRounding(monitor, renderScale);
    SStripStyle style;
    style.round = CARDROUND.rounding;
    style.power = CARDROUND.power;
    style.fade  = FADE;
    updateRestingFade();
    // ponytail: the inner radius is the reference's verysmall:large ratio
    // (8:23) applied to whatever the outer radius resolves to, rather than a
    // literal 8. E8-c pinned the outer radius to the live desktop curve, so a
    // hardcoded inner would stop tracking the theme the moment that changes.
    style.innerRound = std::max(0, sc<int>(std::round(sc<float>(style.round) * 8.F / 23.F)));

    renderStripPanel(monitor, renderScale, style);

    // A REMOVED DESKTOP'S CELL shrinks back into the cell of the desktop shown
    // next (a fullscreen app's desktop going away, the reverse of how it grew
    // out of it), fading -- beneath the cells it slides under.
    if (workspaceInsertTransition.active && !workspaceInsertTransition.transitionFadeIn && workspaceInsertTransition.ghostCellTo != WORKSPACE_INVALID &&
        workspaceInsertProgress->value() < 1.F)
        if (const auto FROM = workspaceInsertTransition.oldStripBoxes.find(workspaceInsertTransition.transitionWorkspaceID); FROM != workspaceInsertTransition.oldStripBoxes.end()) {
            for (size_t i = 0; i < images.size(); ++i) {
                if (!images[i] || workspaceKey(images[i]->pWorkspace) != workspaceInsertTransition.ghostCellTo)
                    continue;
                const CBox  TO   = stripCellLogicalBox(i);
                const float T    = std::clamp(workspaceInsertProgress->value(), 0.F, 1.F);
                const CBox& F    = FROM->second;
                const CBox  BOX  = {F.x + (TO.x - F.x) * T, F.y + (TO.y - F.y) * T, F.width + (TO.width - F.width) * T, F.height + (TO.height - F.height) * T};
                const CBox  CELL = getOverviewGlobalBox(BOX, monitor, renderScale, OVERVIEW_CHROME_NO_PAN, 0.F, layout);
                if (CELL.width > 1 && CELL.height > 1)
                    renderOverviewWorkspaceShadow(monitor, CELL, renderScale, false, style.fade * (1.F - T));
                break;
            }
        }

    // SUPER OVER A DESKTOP: the one to move (see SDeskDrag).
    size_t superIdx = images.size();
    if (!DRAGGED && !deskDrag.pressed && isOverviewPointerOnMonitor(monitor) && g_pInputManager && (g_pInputManager->getModsFromAllKBs() & HL_MODIFIER_META)) {
        workspaceAtStripPoint(lastMousePosLocal, &superIdx);
        if (!deskMovable(superIdx))
            superIdx = images.size();
    }
    // Eased per frame, by wall clock: the cells move like things with a little
    // weight -- the lifted one trails the pointer by a hair, the others glide.
    const auto  DESKNOW = Time::steadyNow();
    // The animation clock's step: the speed setting stretches the glide, and a
    // snapping clock's step (SNAP_MS) takes both smoothings to K = 1 at once.
    const float DT      = Hyprdrive::Motion::Clock::animDt(std::clamp(Hyprdrive::Motion::secondsBetween(DESKNOW, deskAnimAt), 0.F, 0.05F));
    deskAnimAt          = DESKNOW;
    const float KFOLLOW = 1.F - std::exp(-DT / 0.035F);
    const float KGLIDE  = 1.F - std::exp(-DT / 0.085F);
    bool        moving  = false;
    const bool  LIVE    = deskDrag.live && deskDrag.from < images.size() && deskDrag.to < images.size();
    const auto  edgesOf = [&](size_t slot) { return (slot == 0 ? 1 : 0) | (slot + 1 == images.size() ? 2 : 0); };

    struct SPlaced {
        size_t idx;
        CBox   cell;
        int    edges;
        bool   hovered;
    };
    std::vector<SPlaced> placed;
    const auto drawCell = [&](size_t workspaceIdx) {
        const auto& wimg = images[workspaceIdx];
        if (!wimg || !wimg->pWorkspace)
            return;
        const CBox HOME = stripCellBox(workspaceIdx, monitor, renderScale);
        if (HOME.width <= 1)
            return;
        CBox        cell  = HOME;
        const auto  KEY   = workspaceKey(wimg->pWorkspace);
        const auto  CK    = (sc<uint64_t>(monitor->m_id) << 32) ^ sc<uint64_t>(KEY);
        const bool  MINE  = LIVE && workspaceIdx == deskDrag.from;
        int         edges = -1;
        if (MINE) {
            const CBox FIRST = stripCellBox(0, monitor, renderScale), LASTB = stripCellBox(images.size() - 1, monitor, renderScale);
            const float WANT = std::clamp(sc<float>(lastMousePosLocal.x - deskDrag.grab.x), sc<float>(FIRST.x), sc<float>(LASTB.x));
            if (!deskDrag.placed) {
                deskDrag.x      = sc<float>(HOME.x);
                deskDrag.placed = true;
            }
            deskDrag.x += (WANT - deskDrag.x) * KFOLLOW;
            moving |= std::abs(WANT - deskDrag.x) > 0.5F;
            cell.x = deskDrag.x;
            edges  = 3; // in the hand: a card of its own, every corner round
        } else if (LIVE) {
            size_t slot = workspaceIdx;
            if (deskDrag.from < deskDrag.to && workspaceIdx > deskDrag.from && workspaceIdx <= deskDrag.to)
                slot = workspaceIdx - 1;
            else if (deskDrag.to < deskDrag.from && workspaceIdx >= deskDrag.to && workspaceIdx < deskDrag.from)
                slot = workspaceIdx + 1;
            const float WANT = sc<float>(stripCellBox(slot, monitor, renderScale).x - HOME.x);
            float&      s    = deskDrag.slide[KEY];
            s += (WANT - s) * KGLIDE;
            moving |= std::abs(WANT - s) > 0.5F;
            cell.x += s;
            edges = edgesOf(slot);
        }
        if (LIVE) {
            // Where it was drawn, in the strip's own (logical) terms, unscaled:
            // the drop's transition starts every cell from here.
            const CBox L = stripCellLogicalBox(workspaceIdx);
            if (HOME.width > 0 && HOME.height > 0)
                deskDrag.drawn[KEY] = {L.x + (cell.x - HOME.x) * L.width / HOME.width, L.y + (cell.y - HOME.y) * L.height / HOME.height, L.width, L.height};
        }
        // A LITTLE BIGGER: lifted under Super, a touch more in the hand; both
        // on Hyprland's border curve, so they swell and settle like the ring.
        const float LIFT = Hyprdrive::BorderFade::mix(monitor, Hyprdrive::BorderFade::key("strip-lift", CK), workspaceIdx == superIdx || MINE);
        const float HELD = Hyprdrive::BorderFade::mix(monitor, Hyprdrive::BorderFade::key("strip-held", CK), MINE);
        if (const float G = 1.F + 0.04F * LIFT + 0.03F * HELD; G > 1.0005F) {
            const double W = cell.width * G, H = cell.height * G;
            cell           = {cell.x + (cell.width - W) / 2.0, cell.y + (cell.height - H) / 2.0, W, H};
        }
        // Whole pixels: a ring drawn on a fractional box seams (a bright dot
        // mid-edge on the lifted cell).
        cell.round();
        if (cell.width <= 1 || !overviewBoxIntersectsMonitor(cell, monitor))
            return;
        placed.push_back({workspaceIdx, cell, edges, workspaceIdx == hoveredIdx || workspaceIdx == superIdx || MINE});
    };
    // The lifted cell over its neighbours: drawn last.
    const size_t TOP = LIVE ? deskDrag.from : superIdx;
    for (size_t workspaceIdx = 0; workspaceIdx < images.size(); ++workspaceIdx)
        if (workspaceIdx != TOP)
            drawCell(workspaceIdx);
    if (TOP < images.size())
        drawCell(TOP);
    // Not a frame request from inside the pass: onPreRender reads this and
    // joins the reflow pacer while the cells glide.
    deskGlideMoving = moving;
    for (const auto& P : placed) {
        stripCellEdges = P.edges;
        renderStripCell(monitor, P.idx, P.cell, DRAGGED, P.hovered, renderScale, now, style);
        stripCellEdges = -1;
    }

    renderScratchpadCell(monitor, renderScale, now, style);
    renderAltStrips(monitor, renderScale, now, style);

    // Last, so the plate sits over everything the strip drew. Not during a drag:
    // the cell's highlight is the answer to "where will this land", and a label
    // would sit in the way of reading it.
    pillNudgeWant                  = 0.F; // the label below re-asks every frame; no label, no lift
    searchLayoutHere().gridNudgeL  = 0.F; // ...and no push on the grid
    if (!DRAGGED && !deskDrag.pressed)
        renderStripCellLabel(monitor, renderScale, FADE);
}

// THE DESKTOP'S NAME AND WHAT IS ON IT, over a hovered strip cell (user,
// 2026-09-22: "the same kind of mouseover tooltip available in scratchpad
// (non-empty desktops only)"). Same plate as the scratchpad's label and the
// dock's icon labels -- OverviewRender::labelPill -- at the same gap above the
// strip plate, so all three read as one piece of chrome.
//
// Empty desktops get nothing, as asked: the empty cell already shows its number
// in the middle of itself, and a plate over nothing is clutter.
bool COverview::stripCellLabelLive(PHLMONITOR monitor, float renderScale, std::string& keyOut, std::string& labelOut, float& cxOut, float& bottomOut) {
    if (!monitor || !isOverviewPointerOnMonitor(monitor))
        return false;

    size_t idx = images.size();
    if (!workspaceAtStripPoint(lastMousePosLocal, &idx) || idx >= images.size() || !images[idx] || !images[idx]->pWorkspace)
        return false;

    // App names, in window order, each once. Three at most -- the plate is a
    // glance, not a list -- and the rest counted.
    std::vector<std::string> names;
    size_t                   windows = 0;
    for (const auto& REF : images[idx]->windows) {
        const auto W = REF.lock();
        if (!W || !W->m_isMapped)
            continue;
        ++windows;
        auto NAME = AppSearch::appNameForClass(AppSearch::identityClass(W->m_class, W->m_title));
        if (NAME.empty())
            NAME = W->m_class;
        if (!NAME.empty() && std::ranges::find(names, NAME) == names.end())
            names.push_back(NAME);
    }
    if (windows == 0)
        return false;

    std::string label = "Desktop " + images[idx]->pWorkspace->m_name;
    for (size_t i = 0; i < names.size() && i < 3; ++i)
        label += (i == 0 ? "  ·  " : ", ") + names[i];
    if (names.size() > 3)
        label += std::format("  +{}", names.size() - 3);
    // Super held: it says what Super does here.
    if (g_pInputManager && (g_pInputManager->getModsFromAllKBs() & HL_MODIFIER_META) && deskMovable(idx))
        label = "Move this desktop";

    const CBox CELL = stripCellBox(idx, monitor, renderScale);
    if (CELL.width <= 1)
        return false;

    // From the PLATE, built exactly the way renderScratchpadCell builds the
    // scratchpad's -- the LOGICAL cell expanded by the panel padding, THEN
    // transformed -- so the gap between plate and label is the same number on
    // every desktop and on the scratchpad (user, 2026-09-23). The first version
    // subtracted a rendered 10px from the cell, which is not the same thing once
    // the overview's card transform has scaled the padding.
    const CBox  PLATE  = getOverviewGlobalBox(stripCellLogicalBox(idx).copy().expand(OVERVIEW_STRIP_PANELPADL), monitor, renderScale, OVERVIEW_CHROME_NO_PAN,
                                              0.F, layout);
    const float CX     = sc<float>(CELL.x) + sc<float>(CELL.width) / 2.F;
    const float GAP    = OverviewRender::LABEL_PILL_GAP * monitor->m_scale; // the scratchpad label's gap
    const float ABOVE  = sc<float>(PLATE.y) - GAP;
    float       bottom = ABOVE;

    // ...unless above is where the search lives. The centre cells sit right
    // under the pill, and once a query is typed the results panel's foot sits
    // in that same gap -- a label there would be drawn over both. Below the
    // plate is the gap to the windows, which is the same size by construction
    // (overviewChromeGapL), so the label fits there instead.
    const CBox  WANT = OverviewRender::labelPillBox(monitor, label, CX, ABOVE, monitor->m_scale);
    const auto& PUB  = searchLayoutHere();

    if (WANT.width > 1 && PUB.containerBox.width > 1 && !WANT.intersection(PUB.containerBox).empty()) {
        // The results panel owns that gap and cannot move -- it hangs off the
        // pill's row -- so the label goes under the plate instead, into the gap
        // to the windows, at the same gap.
        bottom = sc<float>(PLATE.y + PLATE.height) + GAP + sc<float>(WANT.height);

        // ...and the windows make room for it: label to windows gets the same
        // gap as plate to label. The grid already sits one chrome gap below the
        // plate, so only the difference is asked for, in LOGICAL px -- the
        // label is measured in rendered px, and the pill's published scale is
        // rendered-per-logical under the same card transform.
        if (PUB.scale > 0.01F) {
            const float NEEDL             = ((2.F * GAP) + sc<float>(WANT.height)) / PUB.scale - overviewChromeGapL(monitor);
            searchLayoutHere().gridNudgeL = std::max(0.F, NEEDL);
        }
    } else if (WANT.width > 1 && PUB.pillRest.width > 1 && WANT.x < PUB.pillRest.x + PUB.pillRest.width && PUB.pillRest.x < WANT.x + WANT.width) {
        // The pill is above this cell: it gets gently shoved up, and the label
        // keeps the SAME gap to the pill as to the plate (user, 2026-09-23).
        //
        // Both gaps are GAP when there is room. When there is not -- the pill
        // can only rise to just under the bar -- they shrink TOGETHER, so the
        // label always sits centred between the lifted pill and the plate
        // rather than hard against one of them.
        const float H          = sc<float>(WANT.height);
        const float RESTBOTTOM = sc<float>(PUB.pillRest.y + PUB.pillRest.height);
        const float CEILBOTTOM = RESTBOTTOM - PUB.pillMaxUp; // the pill's foot at its highest
        const float ROOM       = sc<float>(PLATE.y) - CEILBOTTOM - H;
        const float G          = std::clamp(ROOM / 2.F, 0.F, GAP);

        bottom                 = sc<float>(PLATE.y) - G;
        const float LABELTOP   = bottom - H;
        pillNudgeWant          = std::clamp(RESTBOTTOM + G - LABELTOP, 0.F, PUB.pillMaxUp);
    }

    keyOut    = std::to_string(images[idx]->pWorkspace->m_id);
    labelOut  = label;
    cxOut     = CX;
    bottomOut = bottom;
    return true;
}

// THE DOCK TOOLTIP'S FADE, for the strip's labels (user, 2026-09-24: "apply
// those same fade rules to the strip labels"). The same state machine as
// Dock.cpp's renderTooltip: one plate; a new target gets its own entrance; a
// changed label on the same target keeps its alpha; leaving fades the plate
// where it was, 110ms against the entrance's 150 -- exits ~25% quicker, the
// house rule -- on the same houseEase curves.
void COverview::renderStripCellLabel(PHLMONITOR monitor, float renderScale, float fade) {
    if (!monitor || fade < 0.01F)
        return;

    std::string key, label;
    float       cx = 0.F, bottom = 0.F;
    const bool  LIVE = stripCellLabelLive(monitor, renderScale, key, label, cx, bottom);

    auto& T = stripTip;
    if (LIVE) {
        if (key != T.key || T.out()) {
            // Back onto the same cell mid-exit: turn from the alpha it has. A
            // new cell has its own entrance, from 0.
            const float NOW = key == T.key && !T.text.empty() ? T.alpha() : 0.F;
            T.key           = key;
            T.fade.retarget(NOW, 1.F, Time::steadyNow());
        }
        T.text   = label;
        T.cx     = cx;
        T.bottom = bottom;
    } else {
        if (T.text.empty())
            return;
        if (!T.out()) {
            const float NOW = T.alpha(); // out from where it is, not from 1
            T.fade.retarget(NOW, 0.F, Time::steadyNow());
        }
    }

    if (T.out() && T.progress() >= 1.F) {
        T.text.clear();
        T.key.clear();
        return;
    }

    OverviewRender::labelPill(monitor, T.text, T.cx, T.bottom, T.alpha() * fade, monitor->m_scale);
}

// The animation clock, like the dock tooltip it copies (Dock.cpp).
float COverview::SStripTip::progress() const {
    const float MS = Hyprdrive::Motion::Clock::animMs(fade.start);
    return std::clamp(MS / (out() ? STRIPTIP_FADE_OUT_MS : STRIPTIP_FADE_MS), 0.F, 1.F);
}
float COverview::SStripTip::alpha() const {
    // Entrances decelerate, exits accelerate (the house rule).
    return fade.at(houseEase(!out(), progress()));
}

// Whether the strip label is mid-fade, which needs frames nothing else would
// ask for (a pointer resting on a cell emits no motion).
bool COverview::stripTipAnimating() const {
    if (stripTip.text.empty())
        return false;
    const float MS = Hyprdrive::Motion::Clock::animMs(stripTip.fade.start);
    return MS < (stripTip.out() ? STRIPTIP_FADE_OUT_MS : STRIPTIP_FADE_MS) + 16.F;
}
