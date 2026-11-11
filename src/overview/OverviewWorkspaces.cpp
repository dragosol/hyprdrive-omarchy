// COverview: workspace model: offsets, phantoms, plans, trailing/gap workspaces, images.
// Cut from Overview.cpp by pure code motion.
#include "OverviewParts.hpp"

using namespace OverviewDetail;


size_t COverview::activeWorkspaceIndex() const {
    for (size_t i = 0; i < images.size(); ++i) {
        if (images[i]->pWorkspace && images[i]->pWorkspace == startedOn)
            return i;
    }

    return 0;
}

bool COverview::isSelectedWorkspace(const PHLWORKSPACE& workspace) const {
    return workspace && viewportCurrentWorkspace < images.size() && images[viewportCurrentWorkspace] &&
        images[viewportCurrentWorkspace]->pWorkspace == workspace;
}

// The insert transition's key for a desktop: the object itself (its number can
// change under the transition, DesktopOrder.hpp).

namespace OverviewDetail {

CBox depthScaled(const CBox& box, const Vector2D& centre, float k) {
    if (k >= 0.9999F)
        return box;
    return {centre.x + (box.x - centre.x) * k, centre.y + (box.y - centre.y) * k, box.w * k, box.h * k};
}

WORKSPACEID workspaceKey(const PHLWORKSPACE& ws) {
    return ws ? sc<WORKSPACEID>(rc<intptr_t>(ws.get())) : WORKSPACE_INVALID;
}
}

float COverview::workspaceOverviewOffset(size_t workspaceIdx, size_t activeIdx, float workspacePitch) const {
    const auto MONITOR             = pMonitor.lock();
    const auto MONITORSCALE        = MONITOR ? std::max(MONITOR->m_scale, 0.01F) : 1.F;
    const auto MONITORSIZE         = MONITOR ? axisSize(MONITOR->m_size, layout) : 0.F;
    const auto RENDERSCALE         = MONITOR && MONITORSIZE > 0 ?
        std::max(0.01F, (workspacePitch / MONITORSCALE - sc<float>(Hyprdrive::Config::getWorkspaceGap())) / sc<float>(MONITORSIZE)) :
        std::max(scale->value(), 0.01F);
    const auto LOGICALPITCH        = MONITOR ? getWorkspaceLogicalPitch(MONITOR, RENDERSCALE, layout) : workspacePitch / std::max(RENDERSCALE * MONITORSCALE, 0.01F);
    const auto RENDEREDLOGICALUNIT = RENDERSCALE * MONITORSCALE;
    const auto DEFAULTOFFSET       = workspaceOverviewLogicalOffset(workspaceIdx, activeIdx, LOGICALPITCH) * RENDEREDLOGICALUNIT;

    if (!workspaceInsertTransition.active || workspaceIdx >= images.size() || !images[workspaceIdx] || !images[workspaceIdx]->pWorkspace)
        return DEFAULTOFFSET;

    const auto WORKSPACEID = workspaceKey(images[workspaceIdx]->pWorkspace);
    const auto NEWIT       = workspaceInsertTransition.newRelativeOffsets.find(WORKSPACEID);
    if (NEWIT == workspaceInsertTransition.newRelativeOffsets.end())
        return DEFAULTOFFSET;

    const float T         = std::clamp(workspaceInsertProgress->value(), 0.F, 1.F);
    const float NEWOFFSET = NEWIT->second * RENDEREDLOGICALUNIT;

    if (const auto OLDIT = workspaceInsertTransition.oldRelativeOffsets.find(WORKSPACEID); OLDIT != workspaceInsertTransition.oldRelativeOffsets.end()) {
        const float OLDOFFSET = OLDIT->second * RENDEREDLOGICALUNIT;
        return OLDOFFSET + (NEWOFFSET - OLDOFFSET) * T;
    }

    return NEWOFFSET;
}

float COverview::workspaceOverviewLogicalOffset(size_t workspaceIdx, size_t activeIdx, float workspacePitch) const {
    const auto EXTRAINTERVAL = [this](size_t workspaceIdx_) -> float {
        if (workspaceIdx_ + 1 >= images.size() || !images[workspaceIdx_] || !images[workspaceIdx_ + 1])
            return 0.F;

        if (layout == Hyprdrive::Config::ELayout::HORIZONTAL)
            return images[workspaceIdx_]->overflowRight + images[workspaceIdx_ + 1]->overflowLeft;

        return images[workspaceIdx_]->overflowBottom + images[workspaceIdx_ + 1]->overflowTop;
    };

    float offset = 0.F;

    if (workspaceIdx > activeIdx) {
        for (size_t i = activeIdx; i < workspaceIdx; ++i)
            offset += workspacePitch + EXTRAINTERVAL(i);
    } else {
        for (size_t i = workspaceIdx; i < activeIdx; ++i)
            offset -= workspacePitch + EXTRAINTERVAL(i);
    }

    return offset;
}

float COverview::workspaceOverviewAlpha(size_t workspaceIdx) const {
    if (!workspaceInsertTransition.active || workspaceIdx >= images.size() || !images[workspaceIdx] || !images[workspaceIdx]->pWorkspace)
        return 1.F;

    if (workspaceKey(images[workspaceIdx]->pWorkspace) != workspaceInsertTransition.transitionWorkspaceID)
        return 1.F;

    // The emptied desktop a ghost left (REUSEDEXIT): fades in at its place as
    // the ghost finishes going under.
    if (!workspaceInsertTransition.transitionFadeIn) {
        const float T = std::clamp((workspaceInsertProgress->value() - 0.6F) / 0.4F, 0.F, 1.F);
        return T * T * (3.F - 2.F * T);
    }

    if (workspaceInsertTransition.oldRelativeOffsets.contains(workspaceInsertTransition.transitionWorkspaceID))
        return 1.F;

    return std::clamp(workspaceInsertFadeProgress->value(), 0.F, 1.F);
}

namespace OverviewDetail {

void forgetPhantomsOf(const COverview* overview) {
    g_phantomsByOverview.erase(overview);
}
}

static bool isHeldPhantom(const PHLWORKSPACE& workspace) {
    for (const auto& [_, REFS] : g_phantomsByOverview) {
        for (const auto& REF : REFS) {
            if (REF.lock() == workspace)
                return true;
        }
    }
    return false;
}

namespace OverviewDetail {

Hyprdrive::WorkspacePlan::SRow workspacePlanFor(const PHLMONITOR& monitor) {
    namespace WP = Hyprdrive::WorkspacePlan;
    if (!monitor)
        return {};

    std::vector<WP::ID> monitors;
    for (const auto& MON : State::monitorState()->monitors()) {
        if (MON)
            monitors.push_back(MON->m_id);
    }

    std::vector<WP::SWorkspace> facts;
    for (const auto& w : State::workspaceState()->workspaces()) {
        const auto WORKSPACE = w.lock();
        if (!valid(WORKSPACE) || WORKSPACE->m_isSpecialWorkspace || WORKSPACE->m_id <= 0)
            continue;
        const auto OWNER = WORKSPACE->m_monitor.lock();
        facts.push_back({.id = WORKSPACE->m_id, .monitor = OWNER ? OWNER->m_id : -1, .windows = sc<size_t>(WORKSPACE->getWindowCount()), .held = isHeldPhantom(WORKSPACE)});
    }

    // Rule 2: `create` quietly moves a rule-bound id to its bound output.
    const WP::BoundMonitor BOUND = [](WP::ID id) -> std::optional<WP::ID> {
        if (const auto BOUNDTO = ::Config::workspaceRuleMgr()->getBoundMonitorForWS(std::to_string(id)); BOUNDTO)
            return BOUNDTO->m_id;
        return std::nullopt;
    };

    auto       PLAN = WP::plan(monitors, facts, BOUND);
    const auto IT   = PLAN.find(monitor->m_id);
    return IT == PLAN.end() ? WP::SRow{} : IT->second;
}
}

// The last line of defence under the plan: Hyprland's create() makes a SECOND
// workspace with an id that already exists rather than refusing, so nothing
// here creates without checking first.
static PHLWORKSPACE createWorkspaceIfFree(WORKSPACEID id, const PHLMONITOR& monitor) {
    for (const auto& w : State::workspaceState()->workspaces()) {
        if (const auto WORKSPACE = w.lock(); valid(WORKSPACE) && WORKSPACE->m_id == id)
            return nullptr;
    }

    const auto CREATED = State::workspaceState()->create(id, monitor->m_id, std::to_string(id));
    // And a rule that sent it elsewhere after all is not this monitor's to hold.
    if (CREATED && CREATED->m_monitor.lock() != monitor)
        return nullptr;
    return CREATED;
}

static PHLWORKSPACE workspaceWithID(WORKSPACEID id) {
    for (const auto& w : State::workspaceState()->workspaces()) {
        if (const auto WORKSPACE = w.lock(); valid(WORKSPACE) && WORKSPACE->m_id == id && !WORKSPACE->m_isSpecialWorkspace)
            return WORKSPACE;
    }
    return nullptr;
}

// ponytail: E2 keeps the GNOME invariant "the highest occupied workspace is
// followed by one empty" by creating the real workspace, not a phantom strip
// card. The strong ref below is the only thing keeping it alive -- Hyprland
// reaps empty workspaces the moment nothing owns them -- so closing the
// overview cleans it up for free. Which id it is comes from the plan: with more
// than one monitor it is the first one no other monitor owns (user,
// 2026-09-14), and an empty desktop already sitting above the occupied ones is
// reused rather than joined by another.
void COverview::ensureTrailingWorkspace() {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR || closing)
        return;

    const auto ROW = workspacePlanFor(MONITOR);
    if (!ROW.spare) {
        trailingWorkspace.reset();
        return;
    }

    if (trailingWorkspace && (trailingWorkspace->m_id != *ROW.spare || trailingWorkspace->m_monitor.lock() != MONITOR))
        trailingWorkspace.reset();

    if (ROW.existing) {
        // Adopt it only if this overview made it -- a phantom that stopped
        // being a hole, typically. The user's own empty desktop stays theirs.
        const auto EXISTING = workspaceWithID(*ROW.spare);
        if (!trailingWorkspace && EXISTING && std::find(gapWorkspaces.begin(), gapWorkspaces.end(), EXISTING) != gapWorkspaces.end())
            trailingWorkspace = EXISTING;
        return;
    }

    if (!trailingWorkspace)
        trailingWorkspace = createWorkspaceIfFree(*ROW.spare, MONITOR);
}

// ponytail: X3 population parity. The reference grid is indexed by workspace
// NUMBER, not by the workspaces that happen to exist -- it lays out cells 1..N
// and only hides those past the dynamic count (OverviewWidget.qml:33-36, :161,
// :167), so jumping straight to desktop 5 shows five desktops, not two.
// Materialising the missing ids as real workspaces buys that with no new
// placeholder-card path through the renderer: the strong refs below are the only
// thing keeping them alive, exactly as E2's trailing workspace works, so closing
// the overview reaps them for free.
//
// v1.1 item 0 bounded it (WorkspacePlan::GAP_CEILING, 32): a phantom per id up
// to a desktop parked at 300 cost 0.68 s/s of compositor CPU while the overview
// was up (zrig/v6/item0). And with more than one monitor a hole is only filled
// when this monitor owns the desktops on both sides of it (WorkspacePlan rule
// 5) -- the id between the laptop's 3 and the external's 4 is nobody's.
void COverview::ensureNoWorkspaceGaps() {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR || closing) {
        gapWorkspaces.clear();
        forgetPhantomsOf(this);
        return;
    }

    const auto                ROW = workspacePlanFor(MONITOR);
    std::vector<PHLWORKSPACE> kept;
    for (const auto ID : ROW.gaps) {
        // Keep paying for the ones already materialised; anything else that
        // exists is someone's and must not be adopted.
        if (const auto EXISTING = workspaceWithID(ID)) {
            if (std::find(gapWorkspaces.begin(), gapWorkspaces.end(), EXISTING) != gapWorkspaces.end())
                kept.emplace_back(EXISTING);
            continue;
        }

        if (const auto CREATED = createWorkspaceIfFree(ID, MONITOR))
            kept.emplace_back(CREATED);
    }

    gapWorkspaces = std::move(kept);

    auto& refs = g_phantomsByOverview[this];
    refs.clear();
    for (const auto& W : gapWorkspaces)
        refs.emplace_back(W);
}

void COverview::rebuildWorkspaceImages() {
    ensureTrailingWorkspace();
    ensureNoWorkspaceGaps();
    // Workspaces created after we opened (the trailing one, or any made while the
    // overview is up) were never shown, so their alpha is still 0 and
    // renderWindow would multiply their windows away.
    if (sharedStateOwner)
        forceWorkspaceAlphaVisible();
    const auto selectedWorkspace = closeOnWindow ? closeOnWindow->m_workspace : startedOn;
    const auto selectedWindow    = closeOnWindow;
    const auto viewportWorkspace = viewportCurrentWorkspace < images.size() ? images[viewportCurrentWorkspace]->pWorkspace : startedOn;
    const auto REMOVEDWORKSPACE  = pendingRemovedWorkspace.lock();

    images.clear();

    for (const auto& w : State::workspaceState()->workspaces()) {
        const auto WORKSPACE = w.lock();
        if (!valid(WORKSPACE) || WORKSPACE->m_monitor != pMonitor || WORKSPACE->m_isSpecialWorkspace)
            continue;

        if (WORKSPACE == REMOVEDWORKSPACE)
            continue;

        images.emplace_back(makeShared<SWorkspaceImage>(WORKSPACE));
    }

    std::sort(images.begin(), images.end(), [](const auto& a, const auto& b) { return a->pWorkspace->m_id < b->pWorkspace->m_id; });

    if (images.empty()) {
        viewportCurrentWorkspace = 0;
        closeOnWindow.reset();
        return;
    }

    viewportCurrentWorkspace = 0;
    for (size_t i = 0; i < images.size(); ++i) {
        if (images[i]->pWorkspace == viewportWorkspace) {
            viewportCurrentWorkspace = i;
            break;
        }
    }
    if (images[viewportCurrentWorkspace]->pWorkspace != viewportWorkspace) {
        for (size_t i = 0; i < images.size(); ++i) {
            if (images[i]->pWorkspace == selectedWorkspace) {
                viewportCurrentWorkspace = i;
                break;
            }
        }
    }

    closeOnWindow = selectedWindow;
}

void COverview::seedRememberedSelections() {
    for (const auto& img : images) {
        if (!img->pWorkspace)
            continue;

        const auto WORKSPACEID = img->pWorkspace->m_id;

        if (const auto it = rememberedSelection.find(WORKSPACEID); it != rememberedSelection.end()) {
            const auto rememberedWindow = getOverviewWindowToShow(it->second.lock());
            if (rememberedWindow && rememberedWindow->m_workspace == img->pWorkspace && shouldShowOverviewWindow(rememberedWindow))
                continue;
        }

        const auto lastFocusedWindow = getOverviewWindowToShow(img->pWorkspace->getLastFocusedWindow());
        if (!lastFocusedWindow || lastFocusedWindow->m_workspace != img->pWorkspace || !shouldShowOverviewWindow(lastFocusedWindow))
            continue;

        rememberedSelection[WORKSPACEID] = lastFocusedWindow;
    }
}

void COverview::rememberSelection(PHLWINDOW window) {
    window = getOverviewWindowToShow(window);

    if (!window || !window->m_workspace)
        return;

    rememberedSelection[window->m_workspace->m_id] = window;
}

void COverview::updateWorkspaceOverflow() {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR)
        return;

    for (const auto& img : images) {
        if (!img)
            continue;

        img->overflowLeft   = 0.F;
        img->overflowRight  = 0.F;
        img->overflowTop    = 0.F;
        img->overflowBottom = 0.F;
    }

    for (const auto& img : images) {
        if (!img || !img->pWorkspace)
            continue;

        // A Mosaic desktop on its infinite canvas: windows past the screen's
        // edge are the canvas out of view, not a desktop hanging over its
        // neighbours -- room made for them was an empty gap between desktops
        // (user, 2026-10-29).
        if (Hyprdrive::Mosaic::canvasBox(img->pWorkspace))
            continue;

        for (const auto& windowRef : img->windows) {
            const auto window = getOverviewWindowToShow(windowRef.lock());
            if (!shouldShowOverviewWindow(window) || window->m_isFloating)
                continue;

            const auto POS  = window->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT) - MONITOR->m_position;
            const auto SIZE = window->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
            // Windows hidden under a fullscreen window can get sentinel geometry like -2x-2.
            if (SIZE.x <= 0 || SIZE.y <= 0)
                continue;

            img->overflowLeft   = std::max(img->overflowLeft, std::max(0.F, sc<float>(-POS.x)));
            img->overflowRight  = std::max(img->overflowRight, std::max(0.F, sc<float>(POS.x + SIZE.x - MONITOR->m_size.x)));
            img->overflowTop    = std::max(img->overflowTop, std::max(0.F, sc<float>(-POS.y)));
            img->overflowBottom = std::max(img->overflowBottom, std::max(0.F, sc<float>(POS.y + SIZE.y - MONITOR->m_size.y)));
        }
    }
}

CBox COverview::workspaceOverviewVisibleBox(size_t workspaceIdx, const CBox& workspaceBox, float renderScale, PHLMONITOR monitor) const {
    if (workspaceIdx >= images.size() || !images[workspaceIdx] || !monitor)
        return workspaceBox;

    auto box = workspaceBox;
    const auto LEFT   = images[workspaceIdx]->overflowLeft * renderScale * monitor->m_scale;
    const auto RIGHT  = images[workspaceIdx]->overflowRight * renderScale * monitor->m_scale;
    const auto TOP    = images[workspaceIdx]->overflowTop * renderScale * monitor->m_scale;
    const auto BOTTOM = images[workspaceIdx]->overflowBottom * renderScale * monitor->m_scale;

    box.x -= LEFT;
    box.y -= TOP;
    box.width += LEFT + RIGHT;
    box.height += TOP + BOTTOM;

    return box;
}
