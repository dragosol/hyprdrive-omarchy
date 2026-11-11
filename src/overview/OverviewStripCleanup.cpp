// COverview: the strip's Clean Up button (user, 2026-11-11: "if I hold super
// over the workspace strip it puts a button under the strip near my cursor to
// clean up empty desktops. the button should follow the same rules as when the
// tooltip saying which desktop is open is displayed under the strip ... should
// bump stuff down to make way for the button too if there's anything in the
// way of the button. button should follow the same aesthetics as the context
// menu in dock").
//
// What it is, in one place:
//   - Up while SUPER is held, the pointer on the strip's plate (or on the
//     button, or crossing between them: SC::GRACE_MS, the dock pane's grace),
//     and the monitor's row has an empty desktop between the others
//     (SC::removable: never the one shown, never the trailing spare).
//   - Just under the plate, at the strip label's gap, centred on the pointer
//     and gliding after it on the strip cells' own glide; clamped to the plate.
//   - One dock-menu row on a dock-menu plate: the menu's frost, radius, font,
//     glyph column, accent border, row highlight and hover lift (DockMenu.cpp).
//   - The label's rule underneath: the grid moves down just enough when a
//     window is under the button (gridNudge, the label's own push), and not at
//     all when nothing is. Under the strip there is room for ONE plate: while
//     the button is up it takes that place, and the desktop label -- which
//     only goes there while search results own the space above the strip --
//     gives way. Above the strip the label stays as it is.
//   - A click: FullscreenDesktop::compactRow for this monitor -- the very
//     compaction a display change runs (DesktopPlan::compactMoves), for this
//     row only -- then the reorder transition, the removed cells going smaller
//     under the ones closing over them.
#include "OverviewParts.hpp"
#include "Rows.hpp"
#include "MotionClock.hpp"
#include "DockParts.hpp"
#include "StripCleanupModel.hpp"
#include "FullscreenDesktop.hpp"
#include "DesktopOrder.hpp"
#include "DesktopStrip.hpp"

#include <hyprland/src/desktop/state/WindowState.hpp>

using namespace OverviewDetail;
namespace SC = Hyprdrive::StripCleanup;

namespace {
    constexpr const char* CLEANUP_LABEL = "Clean Up Empty Desktops";
    constexpr const char* CLEANUP_GLYPH = ""; // eraser

    bool populated(const PHLWORKSPACE& ws) {
        for (const auto& W : Desktop::windowState()->windows())
            if (W && W->m_isMapped && W->m_workspace == ws)
                return true;
        return false;
    }

    SC::SBox sbox(const CBox& b) {
        return {b.x, b.y, b.width, b.height};
    }

    double msSince(const Time::steady_tp& t) {
        if (t.time_since_epoch().count() == 0)
            return 1e9;
        return std::chrono::duration<double, std::milli>(Time::steadyNow() - t).count();
    }
}

float COverview::SStripClean::alpha() const {
    const float MS = Hyprdrive::Motion::Clock::animMs(fade.start);
    const float P  = std::clamp(MS / (out() ? STRIPTIP_FADE_OUT_MS : STRIPTIP_FADE_MS), 0.F, 1.F);
    return std::clamp(fade.at(houseEase(!out(), P)), 0.F, 1.F);
}

// The strip's plate at rest -- the cells, the gaps and the panel padding --
// as stripUiContains builds it, in rendered monitor-local px.
CBox COverview::stripPlateBox(PHLMONITOR monitor, float renderScale) const {
    if (!monitor || images.empty() || overviewStripLogicalHeight(monitor) <= 0.F)
        return {};
    const auto FIRST = stripCellLogicalBox(0);
    const auto LAST  = stripCellLogicalBox(images.size() - 1);
    if (FIRST.width <= 1 || LAST.width <= 1)
        return {};
    constexpr float PAD = OVERVIEW_STRIP_PANELPADL;
    const CBox      LOGICAL{FIRST.x - PAD, FIRST.y - PAD, sc<float>(LAST.x) + sc<float>(LAST.width) - sc<float>(FIRST.x) + 2.F * PAD, sc<float>(FIRST.height) + 2.F * PAD};
    const CBox      BOX = getOverviewGlobalBox(LOGICAL, monitor, renderScale, OVERVIEW_CHROME_NO_PAN, 0.F, layout);
    return SC::drawable(sbox(BOX)) ? BOX : CBox{};
}

size_t COverview::stripRemovableEmpties() const {
    const auto MON = pMonitor.lock();
    if (!MON)
        return 0;
    const auto              ROW = Hyprdrive::Rows::of(MON);
    std::vector<SC::SDesk> desks;
    desks.reserve(images.size());
    for (const auto& I : images) {
        if (!I || !Hyprdrive::DesktopOrder::ordinary(I->pWorkspace))
            continue;
        desks.push_back({.slot = ROW.slot(I->pWorkspace->m_id), .populated = populated(I->pWorkspace), .shown = MON->m_activeWorkspace == I->pWorkspace});
    }
    return SC::removable(desks);
}

bool COverview::stripCleanupWanted(PHLMONITOR monitor, const CBox& plate) {
    stripClean.plate = plate;
    SC::SWhen w;
    w.enabled = Hyprdrive::Config::getValue<bool>("plugin:hyprdrive:strip_cleanup_button");
    w.super   = g_pInputManager && (g_pInputManager->getModsFromAllKBs() & HL_MODIFIER_META);
    const bool ONMON = monitor && isOverviewPointerOnMonitor(monitor);
    w.onStrip        = ONMON && !plate.empty() && plate.containsPoint(lastMousePosLocal);
    w.onButton       = ONMON && !stripClean.out() && !stripClean.box.empty() && stripClean.box.containsPoint(lastMousePosLocal);
    if (w.onStrip || w.onButton)
        stripClean.onAt = Time::steadyNow();
    else
        w.grace = ONMON && !stripClean.out() && SC::inGrace(msSince(stripClean.onAt));
    auto* dragContext = g_pointerGrabOverview && g_pointerGrabOverview->drag.activeWindow ? g_pointerGrabOverview : this;
    w.busy            = closing || deskDrag.pressed || deskDrag.live || static_cast<bool>(dragContext->drag.activeWindow) || Hyprdrive::DesktopStrip::switching();
    w.removable       = w.enabled && w.super ? stripRemovableEmpties() : 0;
    return SC::wanted(w);
}

// The cells the last click took away: smaller and fading, under the cells
// that close over them, on the reorder transition's own progress.
float COverview::stripCleanupReflowT() const {
    if (!stripClean.reflow || !workspaceInsertTransition.active || !workspaceInsertProgress)
        return 1.F;
    return std::clamp(workspaceInsertProgress->value(), 0.F, 1.F);
}

bool COverview::stripCleanupCellEmpty(size_t idx) const {
    return idx < images.size() && images[idx] && images[idx]->pWorkspace && !populated(images[idx]->pWorkspace);
}

void COverview::renderStripCleanupGhosts(PHLMONITOR monitor, float renderScale, const SStripStyle& style) {
    if (!stripClean.reflow || !monitor)
        return;
    const float T = stripCleanupReflowT();
    if (T >= 1.F) {
        stripClean.ghosts.clear();
        stripClean.reflow    = false;
        stripClean.plateFrom = {};
        return;
    }
    const float A = (1.F - T) * (1.F - T) * style.fade;
    const float K = 1.F - 0.35F * Hyprdrive::Motion::outCubic(T);
    for (const auto& L : stripClean.ghosts) {
        const CBox SMALL{L.x + L.width * (1.F - K) / 2.F, L.y + L.height * (1.F - K) / 2.F, L.width * K, L.height * K};
        CBox       CELL = getOverviewGlobalBox(SMALL, monitor, renderScale, OVERVIEW_CHROME_NO_PAN, 0.F, layout);
        CELL.round();
        if (!SC::drawable(sbox(CELL)))
            continue;
        renderOverviewWorkspaceShadow(monitor, CELL, renderScale, false, A);
        renderStripCellRect(CELL, Hyprdrive::Config::themeForeground().modifyA(0.08F * A), true, true, style);
    }
}

void COverview::renderStripCleanup(PHLMONITOR monitor, float renderScale, float fade) {
    if (!monitor)
        return;
    namespace D = Hyprdrive::Dock;
    auto&      S    = stripClean;
    const auto NOW  = Time::steadyNow();
    const bool WANT = S.wantNow;

    if (WANT && S.out())
        S.fade.retarget(S.alpha(), 1.F, NOW);
    else if (!WANT && !S.out())
        S.fade.retarget(S.alpha(), 0.F, NOW);
    S.bumpL = 0.F;

    const float RAW = S.alpha();
    const float A   = RAW * fade;
    if (S.out() && RAW <= 0.001F) {
        S.box     = {};
        S.placed  = false;
        S.pressed = false;
        S.lit.moveTo(Hyprdrive::Motion::SLitSet<Time::steady_tp>::NONE, NOW, D::menuHoverEased);
        return;
    }

    const CBox PLATE = stripPlateBox(monitor, renderScale);
    if (PLATE.empty())
        return;

    // THE DOCK MENU'S ROW, measured as DockMenu.cpp measures one.
    const float      SCALE  = std::max(monitor->m_scale, 0.01F);
    const int        FONTPX = std::max(1, sc<int>(std::round(D::MENU_FONT * SCALE)));
    const bool       LIGHT  = Hyprdrive::Config::isLightTheme();
    const auto       INK    = Hyprdrive::Config::themeForeground();
    const CHyprColor LABEL_COLOR{INK.r, INK.g, INK.b, 0.95F};
    const CHyprColor DIM_COLOR{INK.r, INK.g, INK.b, LIGHT ? 0.60F : 0.45F};
    const auto       LABEL = OverviewRender::text(CLEANUP_LABEL, LABEL_COLOR, FONTPX, false, "", 0, 500);
    const auto       GLYPH = OverviewRender::text(CLEANUP_GLYPH, DIM_COLOR.modifyA(0.8F), FONTPX, false, "JetBrainsMono Nerd Font", 0, 400);
    if (!LABEL || !LABEL->ok())
        return;
    const float GLYPHCOLW = std::round(sc<float>(FONTPX) * 1.9F);
    const float LW = sc<float>(LABEL->m_size.x), LH = sc<float>(LABEL->m_size.y);
    const float ROWH = std::round(LH + 2.F * D::MENU_ROW_PAD_Y * SCALE);
    // The menu's side padding both ways, the glyph column, the label, and the
    // few px a hovered row's content lifts to the right.
    const float W   = std::round(2.F * D::MENU_PAD_X * SCALE + GLYPHCOLW + LW + 4.F * SCALE);
    const float GAP = OverviewRender::LABEL_PILL_GAP * SCALE; // the strip label's gap to the plate
    const float Y   = std::round(sc<float>(PLATE.y + PLATE.height) + GAP);

    // ACROSS: after the pointer while it is on the strip (held while it is on
    // the button, so the button never runs from it), on the strip cells' glide.
    const float EDGE = std::round(D::MENU_EDGE_MARGIN * SCALE);
    const float LO   = std::max(sc<float>(PLATE.x), EDGE);
    const float HI   = std::min(sc<float>(PLATE.x + PLATE.width), sc<float>(monitor->m_size.x * SCALE) - EDGE);
    const bool  ONMON = isOverviewPointerOnMonitor(monitor);
    const bool  ONBTN = ONMON && !S.box.empty() && S.box.containsPoint(lastMousePosLocal);
    const float XWANT = sc<float>(SC::placeX(lastMousePosLocal.x, W, LO, HI));
    const float DT    = Hyprdrive::Motion::Clock::animDt(std::clamp(Hyprdrive::Motion::secondsBetween(NOW, S.animAt), 0.F, 0.05F));
    S.animAt          = NOW;
    if (!S.placed) {
        S.x      = XWANT;
        S.placed = true;
    } else if (WANT && !ONBTN && ONMON && PLATE.containsPoint(lastMousePosLocal))
        S.x = sc<float>(SC::follow(S.x, XWANT, DT));
    S.x      = std::clamp(S.x, std::min(LO, HI - W), std::max(LO, HI - W));
    S.moving = WANT && std::abs(XWANT - S.x) > 0.5F && !ONBTN;

    // In like the label, dropping the few px the menu's rows rise into place.
    const float DROP = (1.F - Hyprdrive::Motion::outCubic(RAW)) * 6.F * SCALE;
    CBox        box{std::round(S.x), std::round(Y - DROP), W, ROWH};
    if (!SC::drawable(sbox(box)))
        return;
    S.box = CBox{box.x, Y, W, ROWH};

    // THE LABEL'S PUSH: windows under the button move down just enough for
    // the label's gap below it -- never more than the button's own height and
    // its two gaps (bounded: a layout that does not move its tiles cannot run
    // it away).
    const auto& PUB = searchLayoutHere();
    if (WANT && PUB.scale > 0.01F) {
        const float              SHIFTR = gridNudge ? gridNudge->value() * PUB.scale : 0.F;
        std::vector<SC::SBox>    under;
        const CBox               MON{0, 0, monitor->m_size.x * SCALE, monitor->m_size.y * SCALE};
        const auto               DRAGGED = drag.activeWindow.lock();
        for (const auto& [WIN, LAST] : lastTileBoxes) {
            if (DRAGGED && WIN == DRAGGED.get())
                continue;
            if (LAST.box.width < 2 || LAST.box.height < 2 || LAST.box.intersection(MON).empty())
                continue;
            under.push_back({LAST.box.x, LAST.box.y - SHIFTR, LAST.box.width, LAST.box.height});
        }
        const double NEED  = SC::bumpFor(sbox(S.box), GAP, under, PLATE.y + PLATE.height);
        const float  LIMIT = ((2.F * GAP) + ROWH) / PUB.scale; // the button and a gap each side: never more
        S.bumpL            = std::clamp(sc<float>(NEED) / PUB.scale, 0.F, LIMIT);
        if (S.bumpL > 0.5F)
            searchLayoutHere().gridNudgeL = std::max(searchLayoutHere().gridNudgeL, S.bumpL);
    }

    // Hover and press, the menu's own ease.
    const bool HOT = WANT && ONBTN && RAW > 0.5F;
    S.lit.moveTo(HOT ? 1 : Hyprdrive::Motion::SLitSet<Time::steady_tp>::NONE, NOW, D::menuHoverEased);
    const float LIT     = S.lit.level(1, D::menuHoverEased);
    const bool  PRESSED = S.pressed && HOT;
    if (PRESSED) {
        // A press sinks it a hair, about its centre.
        const double K = 0.985;
        box            = CBox{box.x + box.width * (1 - K) / 2, box.y + box.height * (1 - K) / 2, box.width * K, box.height * K}.round();
    }

    const int RADIUS = sc<int>(std::round(std::min<double>(D::MENU_RADIUS * SCALE, box.height / 2.0)));
    D::frostedPlate(monitor, box, RADIUS, A);

    if (const float FILL = (PRESSED ? 0.40F : 0.26F) * LIT; FILL > 0.003F) {
        CRectPassElement::SRectData f;
        f.box           = box;
        f.color         = Hyprdrive::Config::highlightColor().modifyA(FILL * A);
        f.round         = RADIUS;
        f.roundingPower = 2.F;
        if (Hyprdrive::Stacks::drawableBox(f.box))
            g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(f));
    }

    // The row's content: glyph column, then the label; lifted on hover.
    const float GROW  = 1.F + 0.035F * LIT;
    const float TEXTX = std::round(sc<float>(box.x) + D::MENU_PAD_X * SCALE + 4.F * SCALE * LIT);
    const float TEXTY = std::round(sc<float>(box.y) + (sc<float>(box.height) - LH) / 2.F);
    if (GLYPH && GLYPH->ok()) {
        CTexPassElement::SRenderData g;
        g.tex          = GLYPH;
        const float GW = sc<float>(GLYPH->m_size.x) * GROW, GH = sc<float>(GLYPH->m_size.y) * GROW;
        g.box          = CBox{std::round(TEXTX + (GLYPHCOLW - GW) / 2.F - GLYPHCOLW * 0.15F), std::round(TEXTY + (LH - GH) / 2.F), GW, GH};
        g.a            = A;
        if (g.a > 0.001F)
            g_pHyprRenderer->m_renderPass.add(makeUnique<CTexPassElement>(g));
    }
    {
        CTexPassElement::SRenderData t;
        t.tex = LABEL;
        t.box = CBox{TEXTX + GLYPHCOLW, TEXTY - (LH * GROW - LH) / 2.F, LW * GROW, LH * GROW};
        t.a   = A;
        if (t.a > 0.001F)
            g_pHyprRenderer->m_renderPass.add(makeUnique<CTexPassElement>(t));
    }

    // The menu's outline: the theme accent at Hyprland's border weight.
    {
        CBorderPassElement::SBorderData b;
        b.box = box;
        b.grad1.reset(Hyprdrive::Config::highlightColor().modifyA(1.F));
        b.a             = A;
        b.borderSize    = std::max(1, Hyprdrive::Config::getValue<int>("general:border_size"));
        b.round         = RADIUS;
        b.roundingPower = 2.F;
        if (Hyprdrive::Stacks::drawableBox(b.box) && b.a > 0.001F)
            g_pHyprRenderer->m_renderPass.add(makeUnique<CBorderPassElement>(b));
    }
}

bool COverview::stripCleanupAnimating() const {
    const auto& S = stripClean;
    if (S.reflow)
        return true;
    const float MS = Hyprdrive::Motion::Clock::animMs(S.fade.start);
    if (!(S.out() && S.alpha() <= 0.001F) && MS < (S.out() ? STRIPTIP_FADE_OUT_MS : STRIPTIP_FADE_MS) + 16.F)
        return true;
    if (!S.out() && msSince(S.onAt) < SC::GRACE_MS + 16.0)
        return true; // the grace runs out with nothing moving
    return S.lit.anyRunning([](const Hyprdrive::Motion::STween<Time::steady_tp>& f) { return Hyprdrive::Dock::elapsedMs(f.start) < Hyprdrive::Dock::MENU_HOVER_MS + 20.F; });
}

bool COverview::stripCleanupButton(bool pressed) {
    auto& S = stripClean;
    if (pressed) {
        if (S.out() || S.alpha() < 0.5F || S.box.empty() || !S.box.containsPoint(lastMousePosLocal))
            return false;
        S.pressed = true;
        damage();
        requestInputFrame();
        return true;
    }
    if (!S.pressed)
        return false;
    S.pressed = false;
    // Let go on it, Super still held (the button is still up): the click.
    if (!S.out() && S.box.containsPoint(lastMousePosLocal))
        (void)stripCleanupClick();
    damage();
    requestInputFrame();
    return true;
}

bool COverview::stripCleanupClick() {
    const auto MON = pMonitor.lock();
    if (!MON || closing || deskDrag.pressed || deskDrag.live || drag.activeWindow)
        return false;
    // Mid-transition (a desktop switch's or a reorder's): the cells are not
    // where the planner's numbers say. Ignored; the button is still there.
    if (workspaceInsertTransition.active && workspaceInsertProgress->value() < 1.F)
        return false;
    if (stripRemovableEmpties() == 0)
        return false;

    // Where the cells that go are now, for their exit.
    const auto        ROW = Hyprdrive::Rows::of(MON);
    long long         last = 0;
    for (const auto& I : images)
        if (I && Hyprdrive::DesktopOrder::ordinary(I->pWorkspace) && populated(I->pWorkspace))
            last = std::max(last, ROW.slot(I->pWorkspace->m_id));
    std::vector<CBox> gone;
    for (size_t i = 0; i < images.size(); ++i) {
        const auto& I = images[i];
        if (!I || !Hyprdrive::DesktopOrder::ordinary(I->pWorkspace))
            continue;
        const auto SLOT = ROW.slot(I->pWorkspace->m_id);
        if (SLOT > 0 && SLOT < last && !populated(I->pWorkspace) && MON->m_activeWorkspace != I->pWorkspace)
            gone.push_back(stripCellLogicalBox(i));
    }

    // ...and the plate, logical, as stripPlateBox measures it.
    CBox plateFrom;
    if (const auto FIRST = stripCellLogicalBox(0), LAST = stripCellLogicalBox(images.size() - 1); FIRST.width > 1 && LAST.width > 1) {
        constexpr float PAD = OVERVIEW_STRIP_PANELPADL;
        plateFrom = {FIRST.x - PAD, FIRST.y - PAD, sc<float>(LAST.x) + sc<float>(LAST.width) - sc<float>(FIRST.x) + 2.F * PAD, sc<float>(FIRST.height) + 2.F * PAD};
    }

    if (!Hyprdrive::FullscreenDesktop::compactRow(MON, "the strip's Clean Up button"))
        return false;
    stripClean.ghosts    = std::move(gone);
    stripClean.plateFrom = plateFrom;
    stripClean.reflow    = true;
    // (compact asked for a rebuild; this one runs now, with the transition:
    // every cell and card glides from where it was drawn, as after a drop.)
    stripSettle = true;
    onWorkspaceChange();
    return true;
}

std::string COverview::stripCleanupReport() const {
    const auto& S = stripClean;
    return std::format("stripclean: want={} alpha={:.2f} box={:.0f},{:.0f} {:.0f}x{:.0f} plate={:.0f},{:.0f} {:.0f}x{:.0f} x={:.1f} removable={} bumpL={:.1f} "
                       "gridNudge={:.1f} pressed={} lit={:.2f} ghosts={} images={}",
                       S.wantNow ? 1 : 0, S.alpha(), S.box.x, S.box.y, S.box.width, S.box.height, S.plate.x, S.plate.y, S.plate.width, S.plate.height, S.x,
                       stripRemovableEmpties(), S.bumpL, gridNudge ? gridNudge->value() : 0.F, S.pressed ? 1 : 0, S.lit.level(1, Hyprdrive::Dock::menuHoverEased),
                       S.reflow ? S.ghosts.size() : 0, images.size());
}

std::string overviewStripCleanupReport(PHLMONITOR monitor) {
    std::string out;
    for (const auto& OV : scrollOverviews())
        if (auto* O = dynamic_cast<COverview*>(OV.get()); O && (!monitor || O->pMonitor.lock() == monitor))
            out += (out.empty() ? "" : "\n") + std::format("monitor {}: ", O->pMonitor.lock() ? O->pMonitor.lock()->m_name : "?") + O->stripCleanupReport();
    return out.empty() ? "stripclean: no overview" : out;
}
