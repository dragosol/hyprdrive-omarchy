// COverview: window drag, resize, hold-to-swap, scrolling pan, drop commit.
// Cut from Overview.cpp by pure code motion.
#include "OverviewParts.hpp"

using namespace OverviewDetail;


Vector2D COverview::overviewPointToGlobal(size_t workspaceIdx, const Vector2D& pointLocal) const {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR)
        return pointLocal;

    const auto  SAFE_SCALE       = std::max(scale->value(), 0.01F);
    const auto  SAFE_MON_SCALE   = std::max(MONITOR->m_scale, 0.01F);
    const auto  VIEWPORT_CENTER  = CBox{{}, MONITOR->m_size * MONITOR->m_scale}.middle();
    const auto  VIEWPORT_CENTER_LOGICAL = CBox{{}, MONITOR->m_size}.middle();
    const auto  WORKSPACE_OFFSET   = workspaceOverviewOffset(workspaceIdx, activeWorkspaceIndex(), getWorkspaceRenderedPitch(MONITOR, scale->value(), layout));

    return ((pointLocal - axisOffsetVector(WORKSPACE_OFFSET, layout) + viewOffset->value() * scale->value() * SAFE_MON_SCALE - VIEWPORT_CENTER) * (1.F / (SAFE_SCALE * SAFE_MON_SCALE))) +
        VIEWPORT_CENTER_LOGICAL + MONITOR->m_position;
}

CBox COverview::draggedWindowBox(size_t workspaceIdx) const {
    const auto WINDOW  = getOverviewWindowToShow(drag.activeWindow.lock());
    const auto MONITOR = pMonitor.lock();
    if (!WINDOW || !MONITOR || workspaceIdx >= images.size())
        return {};

    const auto WORKSPACE_OFFSET = workspaceOverviewOffset(workspaceIdx, activeWorkspaceIndex(), getWorkspaceRenderedPitch(MONITOR, scale->value(), layout));
    auto       box               = getOverviewDragWindowBox(WINDOW, MONITOR, scale->value(), viewOffset->value(), WORKSPACE_OFFSET, layout);
    box.x = lastMousePosLocal.x - drag.grabOffsetLocal.x;
    box.y = lastMousePosLocal.y - drag.grabOffsetLocal.y;

    return box;
}

CBox COverview::draggedWindowBoxFor(PHLWINDOW window, size_t workspaceIdx, const Vector2D& pointLocal, const Vector2D& grabRatio) const {
    window             = getOverviewWindowToShow(window);
    const auto MONITOR = pMonitor.lock();
    if (!window || !MONITOR || workspaceIdx >= images.size())
        return {};

    const auto WORKSPACEOFFSET = workspaceOverviewOffset(workspaceIdx, activeWorkspaceIndex(), getWorkspaceRenderedPitch(MONITOR, scale->value(), layout));
    auto       box             = getOverviewDragWindowBox(window, MONITOR, scale->value(), viewOffset->value(), WORKSPACEOFFSET, layout);
    box.x = pointLocal.x - box.width * std::clamp(grabRatio.x, 0.0, 1.0);
    box.y = pointLocal.y - box.height * std::clamp(grabRatio.y, 0.0, 1.0);
    return box;
}

// D'1/D'2: the drag proxy is main-tile sized over its own workspace and shrinks
// to mini scale while it hovers a foreign one, so a drop target reads as a target
// before the button comes up. Wall clock like the grid reflow, so a motionless
// cursor still finishes the shrink.
float COverview::dragMiniProgress() const {
    const int   DURATION = Hyprdrive::Config::getDragMiniMs();
    const float TARGET   = drag.miniOver ? 1.F : 0.F;
    if (DURATION <= 0)
        return TARGET;

    const float ELAPSED = Hyprdrive::Config::animElapsedMs(drag.miniChangedAt);
    const float T       = gridReflowEase(ELAPSED / sc<float>(DURATION));
    if (T < 1.F)
        gridSetReflowActive(pMonitor.lock()); // rides the overview's existing next-frame request

    return drag.miniFrom + (TARGET - drag.miniFrom) * T;
}

// X3 delta 6. The reference slides the proxy's grab point to its centre while it
// is over a drop target and keeps the raw grab fraction when it is free of one
// (Overview.qml:391-397), "because an edge grab looks wrong once the proxy is
// thumbnail-sized". Its predicate is `overTarget`, i.e. targetKind "cell" OR
// "tiles" (OverviewDrag.qml:48-49) -- the main area counts, so this is NOT the
// same predicate as the mini shrink, which fires only on a FOREIGN workspace.
// Hence its own progress rather than a reuse of dragMiniProgress().
//
// Deviation from the reference, deliberate: its Behavior carries no easing curve,
// so the QML default makes the slide LINEAR. This uses the house gridReflowEase
// that every other overview motion uses, including the size shrink this rides
// alongside on the same proxy -- two coupled motions on one object reading on two
// different curves is worse than the deviation.
float COverview::dragRecentreProgress() const {
    const int   DURATION = Hyprdrive::Config::getDragRecentreMs();
    const float TARGET   = drag.recentreOver ? 1.F : 0.F;
    if (DURATION <= 0)
        return TARGET;

    const float ELAPSED = Hyprdrive::Config::animElapsedMs(drag.recentreChangedAt);
    const float T       = gridReflowEase(ELAPSED / sc<float>(DURATION));
    if (T < 1.F)
        gridSetReflowActive(pMonitor.lock()); // rides the overview's existing next-frame request

    return drag.recentreFrom + (TARGET - drag.recentreFrom) * T;
}

Vector2D COverview::dragEffectiveGrabRatio() const {
    const float T = std::clamp(dragRecentreProgress(), 0.F, 1.F);
    return Vector2D{
        std::clamp(drag.grabRatio.x, 0.0, 1.0) + (0.5 - std::clamp(drag.grabRatio.x, 0.0, 1.0)) * T,
        std::clamp(drag.grabRatio.y, 0.0, 1.0) + (0.5 - std::clamp(drag.grabRatio.y, 0.0, 1.0)) * T,
    };
}

CBox COverview::draggedWindowGlobalBox() const {
    const auto WINDOW  = getOverviewWindowToShow(drag.activeWindow.lock());
    const auto MONITOR = pMonitor.lock();
    if (!WINDOW || !MONITOR)
        return {};

    const auto WORKSPACEIDX = dragWorkspaceIndex(WINDOW);
    if (WORKSPACEIDX >= images.size())
        return {};

    const auto WORKSPACEOFFSET =
        workspaceOverviewOffset(WORKSPACEIDX, activeWorkspaceIndex(), getWorkspaceRenderedPitch(MONITOR, scale->value(), layout));
    auto SOURCEBOX =
        getOverviewDragWindowBox(WINDOW, MONITOR, scale->value(), viewOffset->value(), WORKSPACEOFFSET, layout, false);

    // A dock tear-out is not coming from a card, so its source box is the
    // geometry it had on its OWN desktop scaled into the overview -- and a
    // window that was alone there fills a whole card, which is larger than
    // anything else on screen. Size it by the slot the grid would actually give
    // it on the card being looked at instead. Falls through to the old box if
    // the grid cannot answer, so this can only ever make the proxy smaller.
    // WHOSE SIZE IS THIS? getOverviewWindowBox above returns the window's GRID
    // SLOT only when overviewGridSlot answers -- and that resolves against the
    // ACTIVE card's grid. A window on any other card falls through to its RAW
    // desktop geometry, which is a whole card's worth.
    //
    // So the same window came out at two different sizes depending on which
    // desktop happened to be in view when it was grabbed: slot-sized off its own
    // card, full-sized off the strip. And because drag_mini_scale is a FRACTION
    // of that base, the mini-shrink was wrong by the same factor -- which is why
    // both the full size and the mini scale had two values for one window.
    //
    // Anything without a slot of its own -- a strip grab, a dock tear-out -- is
    // sized by the slot it would get on the card being looked at instead.
    CBox ownSlot;
    if ((drag.fromDockPreview || !overviewGridSlot(WINDOW, MONITOR, ownSlot)) && viewportCurrentWorkspace < images.size() && images[viewportCurrentWorkspace]) {
        const auto& DESTIMG = images[viewportCurrentWorkspace];
        CBox        slot;
        bool        sized = false;

        // The grid can only answer about a window it already OWNS, and a torn-out
        // one is not in the destination's grid yet -- which is why asking it
        // about this window returned nothing and the proxy stayed huge. So ask
        // about the windows that card already has: their slot is the size this
        // one is joining at.
        for (const auto& REF : DESTIMG->windows) {
            const auto W = REF.lock();
            if (W && W != WINDOW && overviewGridSlot(W, MONITOR, slot) && slot.width >= 1 && slot.height >= 1) {
                sized = true;
                break;
            }
        }

        // Empty card: nothing to measure against, so ask the grid directly.
        if (!sized) {
            if (const auto DEST = DESTIMG->pWorkspace; DEST && overviewGridLayout(DEST, MONITOR, WINDOW, slot) && slot.width >= 1 && slot.height >= 1)
                sized = true;
        }

        if (sized)
            SOURCEBOX = slot;
        else {
            // Last resort, and still a real bound: a tile is never bigger than
            // the card it lives on. Aspect preserved, never scaling up.
            const auto CARDOFFSET = workspaceOverviewOffset(viewportCurrentWorkspace, activeWorkspaceIndex(),
                                                            getWorkspaceRenderedPitch(MONITOR, scale->value(), layout));
            const auto CARD       = getOverviewWorkspaceBox(MONITOR, scale->value(), viewOffset->value(), CARDOFFSET, layout);
            if (CARD.width >= 1 && CARD.height >= 1 && SOURCEBOX.width >= 1 && SOURCEBOX.height >= 1) {
                const double FIT = std::min(1.0, std::min(CARD.width / SOURCEBOX.width, CARD.height / SOURCEBOX.height));
                SOURCEBOX.width *= FIT;
                SOURCEBOX.height *= FIT;
            }
        }
    }

    // THE MINI SIZE IS THE STRIP'S, not a fraction of wherever the drag came
    // from (user, 2026-09-23: "ALWAYS have the same ratio of window height to
    // strip height regardless if im dragging TO or from it"). As a fraction --
    // drag_mini_scale x SOURCEBOX -- the size over the strip inherited every
    // difference between the sources: a grid slot shrinks as the grid fills, a
    // dock tear-out and a strip grab are sized by the destination card's slot,
    // so one window reached the strip at three different sizes.
    //
    // The strip already has one answer for "how big is this window in a cell":
    // stripMiniBox scales every window by cell height / monitor height. The
    // proxy now eases toward exactly that, so what hangs under the cursor over
    // a cell is what will land in it, from any source. drag_mini_scale stays as
    // the fallback for a session with the strip turned off.
    const float P          = dragMiniProgress();
    Vector2D    miniTarget = SOURCEBOX.size() * Hyprdrive::Config::getDragMiniScale();
    if (!images.empty() && overviewStripLogicalHeight(MONITOR) > 0.F) {
        const CBox CELL = stripCellBox(0, MONITOR, scale->value());
        const auto GEOM = WINDOW->geometricBox(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
        if (CELL.height > 1 && GEOM.w >= 1 && GEOM.h >= 1) {
            const double K = CELL.height / std::max(1.0, MONITOR->m_size.y); // rendered cell px per logical monitor px
            miniTarget     = Vector2D{GEOM.w * K, GEOM.h * K};
        }
    }
    const Vector2D RENDERED   = SOURCEBOX.size() + (miniTarget - SOURCEBOX.size()) * P;
    const auto     GLOBALSIZE = RENDERED * (1.F / std::max(MONITOR->m_scale, 0.01F));
    const auto CURSOR     = g_pInputManager->getMouseCoordsInternal();

    const auto GRAB = dragEffectiveGrabRatio();

    return {
        CURSOR.x - GLOBALSIZE.x * GRAB.x,
        CURSOR.y - GLOBALSIZE.y * GRAB.y,
        GLOBALSIZE.x,
        GLOBALSIZE.y,
    };
}

void COverview::refreshDragOriginalOverviewBoxes() {
    const auto WINDOW    = getOverviewWindowToShow(drag.activeWindow.lock());
    const auto WORKSPACE = drag.originalWorkspace.lock();
    const auto MONITOR   = pMonitor.lock();
    if (!WINDOW || !WORKSPACE || !MONITOR || drag.originalVisualBox.empty() || drag.originalBox.empty())
        return;

    size_t workspaceIdx = images.size();
    for (size_t i = 0; i < images.size(); ++i) {
        if (images[i] && images[i]->pWorkspace == WORKSPACE) {
            workspaceIdx = i;
            break;
        }
    }

    if (workspaceIdx >= images.size())
        return;

    Vector2D tapeDelta;
    if (const auto ALGO = overviewScrollingAlgorithmForWorkspace(WORKSPACE); ALGO && ALGO->m_scrollingData && ALGO->m_scrollingData->controller)
        tapeDelta = ALGO->m_scrollingData->controller->getCameraTranslation(ALGO->usableArea()) - drag.originalTapeTranslation;

    auto visualBox = drag.originalVisualBox;
    auto hitbox    = drag.originalBox;
    visualBox.translate(tapeDelta);
    hitbox.translate(tapeDelta);

    const auto WORKSPACEOFFSET = workspaceOverviewOffset(workspaceIdx, activeWorkspaceIndex(), getWorkspaceRenderedPitch(MONITOR, scale->value(), layout));

    // OVERVIEW_CPP_SPEC 13.5 X17. These MUST use the same grid-aware mapping as
    // getOverviewWindowBox(), which is what drew the tile and what
    // windowAtOverviewPoint() hit-tested. Mapping the raw desktop geometry with
    // getOverviewGlobalBox() put this box wherever the window sits on the DESKTOP,
    // not where its tile sits in the overview grid -- measured 800+ px apart on a
    // 6-window workspace. Consequence: the "cursor is still over where the drag
    // started" no-op guard in dropAnchorAtOverviewCursorOnWorkspace() essentially
    // never fired, so every drag, however short, fell through to anchor selection
    // and swapped the window into another tiling slot. Both windows then took
    // their new slots' sizes -- the user sees "dragging resizes my windows".
    drag.originalOverviewBox    = getOverviewWindowBox(WINDOW, MONITOR, scale->value(), viewOffset->value(), WORKSPACEOFFSET, layout, true, &visualBox);
    drag.originalOverviewHitbox = getOverviewWindowBox(WINDOW, MONITOR, scale->value(), viewOffset->value(), WORKSPACEOFFSET, layout, false, &hitbox);
}

CBox COverview::resizedWindowBox() const {
    const auto WINDOW  = getOverviewWindowToShow(resize.activeWindow.lock());
    const auto MONITOR = pMonitor.lock();
    if (!WINDOW || !MONITOR || resize.workspaceIdx >= images.size() || !WINDOW->m_isFloating)
        return {};

    const float SCALEFACTOR = std::max(scale->value(), 0.01F) * std::max(MONITOR->m_scale, 0.01F);
    const auto  TARGET      = WINDOW->layoutTarget();
    const auto  DELTA       = lastMousePosLocal - resize.startMouseLocal;

    Vector2D    minSizePx   = Vector2D{1.F, 1.F};
    if (TARGET) {
        if (const auto MINSIZE = TARGET->minSize(); MINSIZE.has_value())
            minSizePx = Vector2D{std::max(1.F, sc<float>(MINSIZE->x * SCALEFACTOR)), std::max(1.F, sc<float>(MINSIZE->y * SCALEFACTOR))};
    }

    std::optional<Vector2D> maxSizePx;
    if (TARGET) {
        if (const auto MAXSIZE = TARGET->maxSize(); MAXSIZE.has_value() && MAXSIZE->x > 0.F && MAXSIZE->y > 0.F)
            maxSizePx = Vector2D{std::max(minSizePx.x, sc<double>(MAXSIZE->x * SCALEFACTOR)), std::max(minSizePx.y, sc<double>(MAXSIZE->y * SCALEFACTOR))};
    }

    auto box = resizedOverviewBoxFromCorner(resize.originalBox, DELTA, resize.corner, minSizePx, maxSizePx);

    const auto WORKSPACEOFFSET = workspaceOverviewOffset(resize.workspaceIdx, activeWorkspaceIndex(), getWorkspaceRenderedPitch(MONITOR, scale->value(), layout));
    const auto WORKSPACEBOX = getOverviewWorkspaceBox(MONITOR, scale->value(), viewOffset->value(), WORKSPACEOFFSET, layout);
    const auto BORDERMARGIN = WINDOW->getRealBorderSize() * MONITOR->m_scale * scale->value();

    return clampResizedOverviewBoxToWorkspace(box, WORKSPACEBOX, resize.corner, BORDERMARGIN);
}

// The dock's tear-out, which is a strip-mini grab by another name: same mini
// origin, same growth on the way out.
void COverview::beginWindowDragFrom(const PHLWINDOW& window, const CBox& miniOrigin) {
    beginWindowDrag(window, &miniOrigin);
    // AFTER: beginWindowDrag rebuilds the drag state from scratch.
    if (drag.activeWindow)
        drag.fromDockPreview = true;
}

void COverview::beginWindowDrag(PHLWINDOW window, const CBox* stripMiniOrigin) {
    const auto WINDOW = getOverviewWindowToShow(window);
    if (!shouldShowOverviewWindow(WINDOW) || !WINDOW->layoutTarget())
        return;

    // Before the drag moves anything: the window the user was working in, which
    // an arriving window tiles beside (Placement.hpp).
    Hyprdrive::Placement::noteDragAnchor(Desktop::focusState()->window());

    closeOnWindow = WINDOW;
    rememberSelection(WINDOW);

    size_t workspaceIdx = viewportCurrentWorkspace;
    for (size_t i = 0; i < images.size(); ++i) {
        if (images[i]->pWorkspace == WINDOW->m_workspace) {
            // D6: a strip grab must NOT drag the main view onto the mini's
            // workspace — the whole point is to pull the window out of there.
            if (!stripMiniOrigin)
                viewportCurrentWorkspace = i;
            workspaceIdx = i;
            break;
        }
    }

    const auto TARGET             = WINDOW->layoutTarget();
    drag.startedTiled              = !TARGET->floating();
    drag.originalFloatSize         = TARGET->lastFloatingSize();
    drag.originalTapeTranslation  = Vector2D{};
    drag.originalWorkspace         = WINDOW->m_workspace;
    drag.originalBox               = TARGET->position();
    drag.originalVisualBox         = WINDOW->m_group ? TARGET->position() : WINDOW->geometricBox(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
    drag.originalOverviewBox       = CBox{};
    drag.originalOverviewHitbox    = CBox{};
    drag.activeWindow              = WINDOW;


    const auto MONITOR = pMonitor.lock();
    if (MONITOR && workspaceIdx < images.size()) {
        const auto WORKSPACEOFFSET = workspaceOverviewOffset(workspaceIdx, activeWorkspaceIndex(), getWorkspaceRenderedPitch(MONITOR, scale->value(), layout));
        // D1 stays cursor-anchored for a strip grab too: the ratio has to come
        // from the mini the cursor actually pressed, not from the tile the
        // window would occupy in a card that may be off-screen.
        const auto WINDOWBOX       = stripMiniOrigin ? *stripMiniOrigin :
                                                       getOverviewDragWindowBox(WINDOW, MONITOR, scale->value(), viewOffset->value(), WORKSPACEOFFSET, layout);
        drag.grabOffsetLocal        = drag.startMouseLocal - WINDOWBOX.pos();
        drag.grabRatio              = Vector2D{
            WINDOWBOX.width > 0.0 ? drag.grabOffsetLocal.x / WINDOWBOX.width : 0.5,
            WINDOWBOX.height > 0.0 ? drag.grabOffsetLocal.y / WINDOWBOX.height : 0.5,
        };
        if (const auto ALGO = overviewScrollingAlgorithmForWorkspace(WINDOW->m_workspace); ALGO && ALGO->m_scrollingData && ALGO->m_scrollingData->controller)
            drag.originalTapeTranslation = ALGO->m_scrollingData->controller->getCameraTranslation(ALGO->usableArea());
        refreshDragOriginalOverviewBoxes();
    } else {
        drag.grabOffsetLocal = Vector2D{};
        drag.grabRatio       = Vector2D{0.5, 0.5};
    }

    // A strip grab is already at mini scale, so it starts there and grows on the
    // way into the main area instead of popping to full size for one frame.
    drag.miniOver      = stripMiniOrigin != nullptr;
    drag.miniFrom      = drag.miniOver ? 1.F : 0.F;
    drag.miniChangedAt = Time::steadyNow();

    // The grab starts at the fraction the cursor actually pressed (D1); the
    // updateWindowDrag() below is what notices a drop target and starts the slide.
    drag.recentreOver      = false;
    drag.recentreFrom      = 0.F;
    drag.recentreChangedAt = Time::steadyNow();

    // Last, after refreshDragOriginalOverviewBoxes() above has read the boxes it
    // needs out of the full grid. With hold to swap on, a drag that starts in
    // the main grid keeps its slot while it stays there: closing the gap slid
    // the other tiles out from under the pointer, so the window a drag was
    // aimed at was never the one held over (user, 2026-10-27: hold to swap
    // should "work between existing windows in the same workspace and in ANY
    // desktop mode" -- a canvas desktop's tiles reflowed and re-scaled). The
    // gap closes once the drag heads for the strip, the dock or another
    // desktop (updateWindowDrag), as it did before.
    if (!Hyprdrive::Config::getValue<bool>("plugin:hyprdrive:drag:hold_to_swap") || drag.miniOver)
        grid().dragExcluded = WINDOW.get();
    TileChrome::setHovered(PHLWINDOW{});

    updateWindowDrag();
}

void COverview::beginWindowResize() {
    const auto WINDOW  = getOverviewWindowToShow(resize.pendingWindow.lock());
    const auto MONITOR = pMonitor.lock();
    if (!shouldShowOverviewWindow(WINDOW) || shouldShowPinnedFloatingOverviewWindow(WINDOW) || !WINDOW->layoutTarget() || !MONITOR || resize.workspaceIdx >= images.size())
        return;

    closeOnWindow = WINDOW;
    rememberSelection(WINDOW);

    for (size_t i = 0; i < images.size(); ++i) {
        if (images[i]->pWorkspace == WINDOW->m_workspace) {
            viewportCurrentWorkspace = i;
            resize.workspaceIdx       = i;
            break;
        }
    }

    const auto WORKSPACEOFFSET = workspaceOverviewOffset(resize.workspaceIdx, activeWorkspaceIndex(), getWorkspaceRenderedPitch(MONITOR, scale->value(), layout));
    resize.originalBox   = getOverviewWindowBox(WINDOW, MONITOR, scale->value(), viewOffset->value(), WORKSPACEOFFSET, layout);
    resize.activeWindow  = WINDOW;
    resize.lastMouseLocal = lastMousePosLocal;

    updateWindowResize();
}

void COverview::updateWindowDrag() {
    if (!drag.activeWindow)
        return;

    size_t     dropWorkspaceIdx = 0;
    const auto DROPWORKSPACE    = workspaceAtOverviewDropPoint(lastMousePosLocal, &dropWorkspaceIdx, getOverviewWindowToShow(drag.activeWindow.lock()));

    // D'1/D'2: mini scale means "over a strip cell or a foreign card", i.e.
    // anywhere that is not the main area. Keyed off the VIEWPORT workspace, not
    // the drag's origin — for a D6 strip grab the origin is foreign by
    // definition, and keying off it would keep the proxy shrunk all the way
    // into the main area. Identical for a main-grid grab, where the two agree.
    // X26: the dock takes the same shrink the strip gets, which is the spec's
    // "not a new animation" -- the proxy is already thumbnail-sized by the time
    // the button comes up, so the minimize continues a motion instead of
    // starting one.
    const bool OVERDOCK = Hyprdrive::Dock::pillContains(pMonitor.lock(), lastMousePosLocal);

    // A strip cell is "not the main area" even when it is the cell OF the
    // workspace currently in the viewport -- and there DROPWORKSPACE ==
    // VIEWPORTWORKSPACE, so the foreign-card test alone answers false. That is
    // the D6 strip grab's own cell: beginWindowDrag() seeds miniOver = true so
    // the proxy "grows out of the cell", and the first updateWindowDrag() then
    // cleared it, popping the proxy to full main-tile size while the cursor was
    // still inside a cell a few hundred px wide. Ask the strip directly, which
    // is the same test the drop path runs first for the same reason.
    // The detached scratchpad cell counts as the strip here: the proxy shrinking
    // to mini scale is how a cell says "this is a drop target", and over the
    // scratchpad it stayed full size (user, 2026-09-12).
    const bool OVERSTRIP = workspaceAtStripPoint(lastMousePosLocal) != nullptr || scratchpadCellContains(lastMousePosLocal) || altCellAt(lastMousePosLocal).has_value();

    const auto VIEWPORTWORKSPACE = viewportCurrentWorkspace < images.size() && images[viewportCurrentWorkspace] ? images[viewportCurrentWorkspace]->pWorkspace : PHLWORKSPACE{};
    if (const bool OVERFOREIGN = OVERDOCK || OVERSTRIP || (DROPWORKSPACE && DROPWORKSPACE != VIEWPORTWORKSPACE); OVERFOREIGN != drag.miniOver) {
        drag.miniFrom      = dragMiniProgress(); // from where it actually is, not from the end
        drag.miniOver      = OVERFOREIGN;
        drag.miniChangedAt = Time::steadyNow();
    }
    // Leaving its own desktop's tiles: now the survivors close the gap (the
    // slot kept for hold to swap, beginWindowDrag).
    if (drag.miniOver && !grid().dragExcluded)
        grid().dragExcluded = drag.activeWindow.lock().get();

    // X3 delta 6. Any drop target, own workspace included -- the reference's
    // "tiles" DropArea spans the whole main area (WindowTiles.qml:444-452).
    if (const bool OVERTARGET = OVERDOCK || DROPWORKSPACE != nullptr; OVERTARGET != drag.recentreOver) {
        drag.recentreFrom      = dragRecentreProgress(); // from where it actually is
        drag.recentreOver      = OVERTARGET;
        drag.recentreChangedAt = Time::steadyNow();
    }

    // THIS overview always: the proxy is under the cursor and follows it at
    // motion rate, which is faster than the output refreshes.
    requestInputFrame();
    damage();

    // Every OTHER one only when the drag can actually be seen on it, which on
    // a two-monitor session is almost never: a full repaint of the other
    // overview was being asked for on every motion event of every drag.
    //
    // The PREVIOUS box counts as well as this one -- the frame that erases the
    // proxy off a monitor is the frame after it left.
    const auto DRAGBOX = draggedWindowGlobalBox();
    const auto PREVBOX = drag.lastGlobalBox;
    drag.lastGlobalBox = DRAGBOX;

    for (const auto& overview : scrollOverviews()) {
        auto* const OTHER = overview ? dynamic_cast<COverview*>(overview.get()) : nullptr;
        if (!OTHER || OTHER == this)
            continue;

        const auto OTHERMON = OTHER->pMonitor.lock();
        if (!OTHERMON)
            continue;

        const auto BOUNDS  = OTHERMON->logicalBox();
        const bool VISIBLE = (!DRAGBOX.empty() && !DRAGBOX.copy().intersection(BOUNDS).empty()) || (!PREVBOX.empty() && !PREVBOX.copy().intersection(BOUNDS).empty());

        // ...and its own chrome, which a drag anywhere can be driving: the
        // other monitors' strips and the scratchpad cell are drop targets, and
        // they are revealed BY the drag.
        const bool CHROME = OTHER->altStripsRevealValue() > 0.01F || OTHER->scratchpadRevealValue() > 0.01F || OTHER->altStripsReveal->isBeingAnimated() ||
            OTHER->scratchpadReveal->isBeingAnimated();

        if (!VISIBLE && !CHROME)
            continue;

        OTHER->requestInputFrame();
        OTHER->damage();
    }
}

void COverview::updateWindowResize() {
    const auto WINDOW  = getOverviewWindowToShow(resize.activeWindow.lock());
    const auto TARGET  = WINDOW ? WINDOW->layoutTarget() : nullptr;
    const auto MONITOR = pMonitor.lock();

    if (!WINDOW || !TARGET || !MONITOR || resize.workspaceIdx >= images.size())
        return;

    TARGET->damageEntire();

    if (WINDOW->m_isFloating) {
        const auto RESIZEDBOX  = resizedWindowBox();
        const auto SCALEFACTOR = std::max(scale->value(), 0.01F) * std::max(MONITOR->m_scale, 0.01F);
        const auto GLOBALBOX   = CBox{overviewPointToGlobal(resize.workspaceIdx, RESIZEDBOX.pos()), RESIZEDBOX.size() * (1.F / SCALEFACTOR)};

        TARGET->rememberFloatingSize(GLOBALBOX.size());
        TARGET->setPositionGlobal(GLOBALBOX);
        TARGET->warpPositionSize();
    } else {
        const auto DELTALOCAL = lastMousePosLocal - resize.lastMouseLocal;
        const auto SAFEFACTOR = std::max(scale->value(), 0.01F) * std::max(MONITOR->m_scale, 0.01F);
        const auto DELTAGLOBAL = DELTALOCAL * (1.F / SAFEFACTOR);

        if (std::abs(DELTAGLOBAL.x) > 0.01F || std::abs(DELTAGLOBAL.y) > 0.01F) {
            g_layoutManager->resizeTarget(DELTAGLOBAL, TARGET, resize.corner);
            syncWorkspaceGeometry(TARGET->workspace());
            resize.lastMouseLocal = lastMousePosLocal;
        }
    }

    TARGET->damageEntire();
    damage();
}

void COverview::clearDragPending() {
    // The last frame of the drag. updateWindowDrag only pays for the outputs
    // the proxy was actually on, so the one it is leaving has to be told the
    // proxy is gone -- once, here, rather than on every motion of the drag.
    if (!drag.lastGlobalBox.empty()) {
        for (const auto& overview : scrollOverviews()) {
            if (!overview || overview.get() == this)
                continue;

            overview->requestInputFrame();
            overview->damage();
        }
    }
    drag.lastGlobalBox = CBox{};

    grid().dragExcluded        = nullptr;
    drag.pendingPrimary          = false;
    drag.activeWindow.reset();
    drag.originalWorkspace.reset();
    drag.startedTiled            = false;
    drag.originalFloatSize       = Vector2D{};
    drag.originalTapeTranslation = Vector2D{};
    drag.grabOffsetLocal         = Vector2D{};
    drag.grabRatio               = Vector2D{0.5, 0.5};
    drag.recentreOver            = false;
    drag.recentreFrom            = 0.F;
    drag.originalBox             = CBox{};
    drag.originalVisualBox       = CBox{};
    drag.originalOverviewBox     = CBox{};
    drag.originalOverviewHitbox  = CBox{};
    drag.swapCandidate.reset();
    drag.swapTargetBox           = CBox{};
    drag.swapArmed               = false;
}

namespace {

// ============================================================ HOLD TO SWAP
// (user, 2026-10-13, asked for on r/omarchy). Rearranging was taken out of the
// overview in 1.9.2 (8068af5) because a drop that swapped by accident is worse
// than none; the hold is what makes it deliberate. A plain drop still only moves
// a window between desktops (DropDecision.hpp).

    constexpr const char* KEY_HOLD_TO_SWAP    = "plugin:hyprdrive:drag:hold_to_swap";
    constexpr const char* KEY_HOLD_TO_SWAP_MS = "plugin:hyprdrive:drag:hold_to_swap_ms";
    constexpr float       SWAP_DIM_A          = 0.62F;

    // A zero, negative or non-finite box handed to a pass element aborts the
    // compositor (Hyprland's RASSERTs), so every box drawn below asks first.
    bool swapDrawable(const CBox& b) {
        return std::isfinite(b.x) && std::isfinite(b.y) && std::isfinite(b.w) && std::isfinite(b.h) && b.w >= 2 && b.h >= 2;
    }

    // Two windows trade places only when the trade means the same thing for
    // both: both tiled (their layout slots) or both floating (their spots).
    // Fullscreen, pinned and special-desktop (scratchpad) windows stay put.
    bool swappable(const PHLWINDOW& w) {
        return w && Desktop::View::validMapped(w) && w->m_workspace && !w->m_workspace->m_isSpecialWorkspace && !w->m_pinned &&
            !Fullscreen::controller()->isFullscreen(w) && w->layoutTarget();
    }
}

PHLWINDOW COverview::swapCandidateAt(const Vector2D& point, CBox* targetBox) {
    const auto MONITOR = pMonitor.lock();
    const auto DRAGGED = getOverviewWindowToShow(drag.activeWindow.lock());
    if (!MONITOR || !DRAGGED || Hyprdrive::Dock::pillContains(MONITOR, point) || scratchpadCellContains(point))
        return nullptr;

    PHLWINDOW hit;
    CBox      hitBox;

    // The strip first: its band floats over the grid's tiles (the drop path
    // says the same). On a cell but not on one of its minis is no target.
    if (overviewStripLogicalHeight(MONITOR) > 0.F) {
        CBox mini;
        if (const auto W = windowAtStripPoint(point, &mini)) {
            hit    = W;
            hitBox = mini;
        } else if (workspaceAtStripPoint(point) || altCellAt(point))
            return nullptr;
    }

    // Then the grid, as workspaceAtOverviewDropPoint walks it: floating over
    // tiled, topmost first, never the window being dragged.
    if (!hit) {
        const auto ACTIVEIDX = activeWorkspaceIndex();
        const auto PITCH     = getWorkspaceRenderedPitch(MONITOR, scale->value(), layout);
        for (size_t i = 0; i < images.size() && !hit; ++i) {
            const auto& wimg = images[i];
            if (!wimg || !wimg->pWorkspace)
                continue;
            const auto OFFSET = workspaceOverviewOffset(i, ACTIVEIDX, PITCH);
            for (const bool floating : {true, false}) {
                for (auto it = wimg->windows.rbegin(); it != wimg->windows.rend() && !hit; ++it) {
                    const auto W = getOverviewWindowToShow(it->lock());
                    if (!shouldShowOverviewWindow(W) || W == DRAGGED || W->m_isFloating != floating)
                        continue;
                    const auto BOX = getOverviewDragWindowBox(W, MONITOR, scale->value(), viewOffset->value(), OFFSET, layout);
                    if (BOX.containsPoint(point)) {
                        hit    = W;
                        hitBox = BOX;
                    }
                }
                if (hit)
                    break;
            }
        }
    }

    if (!hit || hit == DRAGGED || !swappable(hit) || !swappable(DRAGGED) || hit->m_isFloating != DRAGGED->m_isFloating)
        return nullptr;
    if (targetBox)
        *targetBox = hitBox;
    return hit;
}

// Per FRAME, not per motion: a pointer resting on the target sends no motion,
// and the hold has to complete under a still hand.
void COverview::updateSwapHold() {
    if (!drag.activeWindow || !Hyprdrive::Config::getValue<bool>(KEY_HOLD_TO_SWAP)) {
        drag.swapCandidate.reset();
        drag.swapTargetBox = CBox{};
        drag.swapArmed     = false;
        return;
    }

    CBox       box;
    const auto CANDIDATE = swapCandidateAt(lastMousePosLocal, &box);
    const auto NOW       = Time::steadyNow();
    if (CANDIDATE != drag.swapCandidate.lock()) {
        drag.swapCandidate = CANDIDATE;
        drag.swapSince     = NOW;
        drag.swapArmed     = false;
    }
    drag.swapTargetBox = CANDIDATE ? box : CBox{};
    if (!CANDIDATE)
        return;

    const int HOLDMS = std::clamp<int>(Hyprdrive::Config::getValue<int>(KEY_HOLD_TO_SWAP_MS), 300, 5000);
    if (!drag.swapArmed && Hyprdrive::Motion::msBetween(NOW, drag.swapSince) >= sc<float>(HOLDMS)) {
        drag.swapArmed   = true;
        drag.swapArmedAt = NOW;
    }

    // Keep the frames coming until the hold has completed and the label has
    // finished popping in; after that a still pointer costs nothing.
    // The hold is real time (above); the label's pop is an animation.
    if (!drag.swapArmed || Hyprdrive::Motion::Clock::animMsBetween(NOW, drag.swapArmedAt) < SearchLetters::ENTRANCE_MS) {
        damage();
        requestInputFrame();
    }
}

namespace {

    // The window's own corner radius, scaled to the box it is drawn into here.
    int swapRound(const PHLWINDOW& w, const CBox& box, const PHLMONITOR& monitor) {
        const double REALW = std::max(1.0, sc<double>(w->m_realSize->value().x) * monitor->m_scale);
        const double R     = sc<double>(Hyprdrive::Config::themeRounding()) * monitor->m_scale * (box.w / REALW);
        return sc<int>(std::round(std::clamp(R, 0.0, std::min(box.w, box.h) / 2.0)));
    }

    // Dimmed in the theme's own background colour: dark themes darken, and the
    // label (theme foreground) reads on it in light themes too.
    void swapDim(const CBox& box, int round, float a) {
        if (!swapDrawable(box) || a < 0.01F)
            return;
        CRectPassElement::SRectData r;
        r.box           = box;
        r.color         = Hyprdrive::Config::dockBackgroundColor().modifyA(SWAP_DIM_A * a);
        r.round         = round;
        r.roundingPower = Hyprdrive::Config::themeRoundingPower();
        g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(r));
    }
}

void COverview::renderSwapTargetDim(PHLMONITOR monitor) {
    const auto TARGET = drag.swapCandidate.lock();
    if (!monitor || !drag.swapArmed || !TARGET)
        return;
    const float A = SearchLetters::entrancePose(Hyprdrive::Motion::Clock::animMs(drag.swapArmedAt)).opacity;
    swapDim(drag.swapTargetBox, swapRound(TARGET, drag.swapTargetBox, monitor), A);
}

// After the dragged window is drawn: its dim, then the label on it -- the
// window under the hand is where the eye is.
void COverview::renderSwapLabel(PHLMONITOR monitor) {
    const auto DRAGGED = getOverviewWindowToShow(drag.activeWindow.lock());
    if (!monitor || !drag.swapArmed || !drag.swapCandidate.lock() || !DRAGGED)
        return;

    // The proxy's box exactly as renderDraggedWindow draws it.
    const auto GLOBALBOX = draggedWindowGlobalBox();
    if (GLOBALBOX.empty())
        return;
    auto box = CBox{(GLOBALBOX.pos() - monitor->m_position) * monitor->m_scale, GLOBALBOX.size() * monitor->m_scale};
    box.round();

    const float MS   = Hyprdrive::Motion::Clock::animMs(drag.swapArmedAt);
    const auto  POSE = SearchLetters::entrancePose(MS);
    swapDim(box, swapRound(DRAGGED, box, monitor), POSE.opacity);

    // The detent number's entrance (SearchLetters::entrancePose, OutBack):
    // icon and word scaled together about their shared centre.
    const float SCALE = sc<float>(monitor->m_scale);
    const int   SIZE  = sc<int>(std::round(std::clamp(sc<float>(box.h) * 0.10F, 14.F * SCALE, 40.F * SCALE)));
    const auto  TEXT  = OverviewRender::text("Swap", Hyprdrive::Config::themeForeground().modifyA(1.F), SIZE, false, "", 0, 700);
    const auto  ICON  = OverviewRender::appIcon(std::string(Hyprdrive::BuiltinIcons::SWAP));
    if (!TEXT || !TEXT->ok())
        return;

    const float ICONPX = ICON && ICON->ok() ? sc<float>(SIZE) * 1.25F : 0.F;
    const float GAP    = ICONPX > 0.F ? sc<float>(SIZE) * 0.4F : 0.F;
    const float TW     = sc<float>(TEXT->m_size.x);
    const float TH     = sc<float>(TEXT->m_size.y);
    const float GROUPW = ICONPX + GAP + TW;

    const double CX = box.x + box.w / 2.0;
    const double CY = box.y + box.h / 2.0 + sc<double>(POSE.y) * SCALE;
    const float  K  = POSE.scale;
    const double LEFT = CX - GROUPW * K / 2.0;

    if (ICONPX > 0.F) {
        CTexPassElement::SRenderData icon;
        icon.tex = ICON;
        icon.box = CBox{std::round(LEFT), std::round(CY - ICONPX * K / 2.0), ICONPX * K, ICONPX * K};
        icon.a   = POSE.opacity;
        if (swapDrawable(icon.box))
            g_pHyprRenderer->m_renderPass.add(makeUnique<CTexPassElement>(std::move(icon)));
    }

    CTexPassElement::SRenderData label;
    label.tex = TEXT;
    label.box = CBox{std::round(LEFT + (ICONPX + GAP) * K), std::round(CY - TH * K / 2.0), TW * K, TH * K};
    label.a   = POSE.opacity;
    if (swapDrawable(label.box))
        g_pHyprRenderer->m_renderPass.add(makeUnique<CTexPassElement>(std::move(label)));
}

bool COverview::commitSwap(const PHLWINDOW& a, const PHLWINDOW& b) {
    const auto TA  = a ? a->layoutTarget() : nullptr;
    const auto TB  = b ? b->layoutTarget() : nullptr;
    const auto WSA = a ? a->m_workspace : PHLWORKSPACE{};
    const auto WSB = b ? b->m_workspace : PHLWORKSPACE{};
    if (!TA || !TB || !WSA || !WSB || !g_layoutManager)
        return false;

    // Moving a window can change a monitor's active desktop; the swap must not.
    std::vector<std::pair<PHLMONITORREF, PHLWORKSPACEREF>> actives;
    for (const auto& M : State::monitorState()->monitors())
        if (M)
            actives.emplace_back(M, M->m_activeWorkspace);
    const auto RESTORE = [&] {
        for (const auto& [MREF, WSREF] : actives) {
            const auto M = MREF.lock();
            const auto W = WSREF.lock();
            if (M && W && M->m_activeWorkspace != W)
                M->changeWorkspace(W, false, true, true);
        }
    };

    TA->damageEntire();
    TB->damageEntire();

    if (!a->m_isFloating) {
        if (WSA == WSB)
            g_layoutManager->switchTargets(TA, TB, true);
        else {
            // Across desktops: bring A over, trade slots there (A takes B's), and
            // send B back to file where A was, as an arrival is filed.
            Desktop::globalWindowController()->moveWindowToWorkspace(a, WSB);
            RESTORE();
            g_layoutManager->switchTargets(TA, TB, true);
            Desktop::globalWindowController()->moveWindowToWorkspace(b, WSA);
            RESTORE();
            Hyprdrive::Placement::retileAsNew(b, Hyprdrive::Placement::takeDragAnchor());
        }
    } else {
        // Floating: each keeps its size and takes the other's centre.
        const CBox BOXA{a->m_realPosition->goal(), a->m_realSize->goal()};
        const CBox BOXB{b->m_realPosition->goal(), b->m_realSize->goal()};
        if (WSA != WSB) {
            Desktop::globalWindowController()->moveWindowToWorkspace(a, WSB);
            RESTORE();
            Desktop::globalWindowController()->moveWindowToWorkspace(b, WSA);
            RESTORE();
        }
        auto NEWA = CBox{BOXB.middle() - BOXA.size() / 2.0, BOXA.size()};
        auto NEWB = CBox{BOXA.middle() - BOXB.size() / 2.0, BOXB.size()};
        NEWA      = clampBoxToWorkspace(NEWA, WSB, WSB->m_monitor.lock(), a->getRealBorderSize());
        NEWB      = clampBoxToWorkspace(NEWB, WSA, WSA->m_monitor.lock(), b->getRealBorderSize());
        TA->setPositionGlobal(NEWA);
        TB->setPositionGlobal(NEWB);
    }

    TA->warpPositionSize();
    TB->warpPositionSize();
    TA->damageEntire();
    TB->damageEntire();
    WSA->updateWindows();
    if (WSB != WSA)
        WSB->updateWindows();
    Hyprdrive::Dock::noteRetiled(a);
    Hyprdrive::Dock::noteRetiled(b);
    RESTORE();

    clearDragPending();
    for (const auto& overview : scrollOverviews()) {
        if (auto* const OTHER = overview ? dynamic_cast<COverview*>(overview.get()) : nullptr) {
            OTHER->rebuildPending = true;
            OTHER->damage();
        }
    }
    return true;
}

void COverview::updateScrollingPan() {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR)
        return;

    const auto WORKSPACE = pan.workspace.lock();
    const auto ALGO      = overviewScrollingAlgorithmForWorkspace(WORKSPACE);
    if (!ALGO || !ALGO->m_scrollingData || !ALGO->m_scrollingData->controller) {
        pan.lastMouseLocal = lastMousePosLocal;
        return;
    }

    const auto DELTALOCAL = lastMousePosLocal - pan.lastMouseLocal;
    const auto SAFEFACTOR = std::max(scale->value(), 0.01F) * std::max(MONITOR->m_scale, 0.01F);
    const auto DELTAGLOBAL = DELTALOCAL * (1.F / SAFEFACTOR);
    const auto DELTA       = ALGO->m_scrollingData->controller->isPrimaryHorizontal() ? DELTAGLOBAL.x : DELTAGLOBAL.y;

    if (std::abs(DELTA) > 0.01F) {
        const double OFFSET = ALGO->m_scrollingData->controller->getOffset() - DELTA;
        ALGO->m_scrollingData->controller->setOffset(OFFSET);
        ALGO->m_scrollingData->lockedCameraOffset = OFFSET;
        ALGO->m_scrollingData->recalculate(true);
        pan.lastMouseLocal = lastMousePosLocal;
        markBlurDirty();
        damage();
    }
}

void COverview::beginScrollingPan(PHLWORKSPACE workspace) {
    const auto ALGO = overviewScrollingAlgorithmForWorkspace(workspace);
    if (!ALGO)
        return;

    pan.pointerDown    = true;
    pan.workspace      = workspace;
    pan.lastMouseLocal = lastMousePosLocal;
    pan.initialWindow  = getOverviewWindowToShow(closeOnWindow.lock());
    if (!pan.initialWindow || pan.initialWindow->m_workspace != workspace)
        pan.initialWindow = getOverviewWindowToShow(Desktop::focusState()->window());
    if (pan.initialWindow && pan.initialWindow->m_workspace != workspace)
        pan.initialWindow.reset();

    ALGO->m_scrollingData->lockedCameraOffset = ALGO->m_scrollingData->controller->getOffset();

    if (Desktop::focusState()->window() && Desktop::focusState()->window()->m_workspace == workspace)
        Desktop::focusState()->fullWindowFocus(nullptr, Desktop::FOCUS_REASON_DESKTOP_STATE_CHANGE);
}

void COverview::endScrollingPan() {
    const auto WORKSPACE = pan.workspace.lock();
    const auto ALGO      = overviewScrollingAlgorithmForWorkspace(WORKSPACE);
    if (ALGO) {
        const double OFFSET = clampOverviewScrollingOffset(ALGO, ALGO->m_scrollingData->controller->getOffset());
        ALGO->m_scrollingData->controller->setOffset(OFFSET);
        ALGO->m_scrollingData->lockedCameraOffset.reset();
        ALGO->m_scrollingData->recalculate();
    }

    pan.pointerDown    = false;
    pan.lastMouseLocal = Vector2D{};
    pan.workspace.reset();

    focusMostVisibleScrollingWindow(WORKSPACE);
    pan.initialWindow.reset();
}

void COverview::focusMostVisibleScrollingWindow(const PHLWORKSPACE& workspace) {
    const auto MONITOR = workspace && workspace->m_monitor ? workspace->m_monitor.lock() : pMonitor.lock();
    if (!workspace || !MONITOR)
        return;

    PHLWINDOW  bestFullWindow;
    PHLWINDOW  bestPartialWindow;
    double     bestFullDistance     = std::numeric_limits<double>::max();
    double     bestPartialVisibleArea = 0.0;
    double     bestPartialDistance  = std::numeric_limits<double>::max();
    const auto MONITORBOX           = MONITOR->logicalBox();
    const auto ALGO                 = overviewScrollingAlgorithmForWorkspace(workspace);
    auto       WORKSPACEBOX         = ALGO ? ALGO->usableArea() : MONITORBOX;
    if (ALGO)
        WORKSPACEBOX.translate(MONITOR->m_position);

    const auto preferredWindow = getOverviewWindowToShow(pan.initialWindow.lock());
    if (shouldShowOverviewWindow(preferredWindow) && preferredWindow->m_workspace == workspace && !preferredWindow->m_isFloating && preferredWindow->layoutTarget()) {
        const auto WINDOWBOX   = preferredWindow->layoutTarget()->position();
        const auto AREA        = overviewBoxArea(WINDOWBOX);
        const auto VISIBLEAREA = overviewBoxIntersectionArea(WINDOWBOX, WORKSPACEBOX);
        if (AREA > 0.0 && std::abs(VISIBLEAREA - AREA) < 0.5) {
            closeOnWindow = preferredWindow;
            rememberSelection(preferredWindow);
            Desktop::focusState()->fullWindowFocus(preferredWindow, Desktop::FOCUS_REASON_DESKTOP_STATE_CHANGE);
            return;
        }
    }

    for (const auto& windowRef : Desktop::windowState()->windows()) {
        const auto WINDOW = getOverviewWindowToShow(windowRef);
        if (!shouldShowOverviewWindow(WINDOW) || WINDOW->m_workspace != workspace || WINDOW->m_isFloating || !WINDOW->layoutTarget())
            continue;

        const auto WINDOWBOX = WINDOW->layoutTarget()->position();
        const auto AREA      = overviewBoxArea(WINDOWBOX);
        if (AREA <= 0.0)
            continue;

        const auto VISIBLEAREA = overviewBoxIntersectionArea(WINDOWBOX, WORKSPACEBOX);
        if (VISIBLEAREA <= 0.0)
            continue;

        const auto DISTANCE = overviewBoxCenterDistanceSquared(WINDOWBOX, WORKSPACEBOX);
        if (std::abs(VISIBLEAREA - AREA) < 0.5) {
            if (DISTANCE < bestFullDistance) {
                bestFullWindow   = WINDOW;
                bestFullDistance = DISTANCE;
            }
            continue;
        }

        if (VISIBLEAREA > bestPartialVisibleArea || (std::abs(VISIBLEAREA - bestPartialVisibleArea) < 0.5 && DISTANCE < bestPartialDistance)) {
            bestPartialWindow     = WINDOW;
            bestPartialVisibleArea = VISIBLEAREA;
            bestPartialDistance   = DISTANCE;
        }
    }

    const auto bestWindow = bestFullWindow ? bestFullWindow : bestPartialWindow;
    if (!bestWindow)
        return;

    closeOnWindow = bestWindow;
    rememberSelection(bestWindow);
    Desktop::focusState()->fullWindowFocus(bestWindow, Desktop::FOCUS_REASON_DESKTOP_STATE_CHANGE);
}

bool COverview::moveScrollingColumnSelection(bool next) {
    if (images.empty() || viewportCurrentWorkspace >= images.size())
        return false;

    const auto& WORKSPACEIMAGE = images[viewportCurrentWorkspace];
    if (!WORKSPACEIMAGE || !WORKSPACEIMAGE->pWorkspace)
        return false;

    const auto ALGO = overviewScrollingAlgorithmForWorkspace(WORKSPACEIMAGE->pWorkspace);
    if (!ALGO || !ALGO->m_scrollingData || ALGO->m_scrollingData->columns.empty())
        return false;

    if (!closeOnWindow || closeOnWindow->m_workspace != WORKSPACEIMAGE->pWorkspace || !shouldShowOverviewWindow(closeOnWindow.lock()) || closeOnWindow->m_isFloating)
        syncSelectionToViewport();

    const auto CURRENT = getOverviewWindowToShow(closeOnWindow.lock());
    const auto CURRENTDATA = CURRENT && CURRENT->layoutTarget() ? ALGO->dataFor(CURRENT->layoutTarget()) : nullptr;
    const auto CURRENTCOL  = CURRENTDATA ? CURRENTDATA->column.lock() : ALGO->currentColumn();
    if (!CURRENTCOL)
        return false;

    const auto firstWindowInColumn = [&](const SP<Layout::Tiled::SColumnData>& column) -> PHLWINDOW {
        if (!column)
            return {};

        for (const auto& targetData : column->targetDatas) {
            if (!targetData || !targetData->target)
                continue;

            for (const auto& windowRef : WORKSPACEIMAGE->windows) {
                const auto WINDOW = getOverviewWindowToShow(windowRef.lock());
                if (shouldShowOverviewWindow(WINDOW) && !WINDOW->m_isFloating && WINDOW->layoutTarget() == targetData->target && WINDOW->m_workspace == WORKSPACEIMAGE->pWorkspace)
                    return WINDOW;
            }
        }

        return {};
    };

    std::vector<SP<Layout::Tiled::SColumnData>> columns;
    for (const auto& column : ALGO->m_scrollingData->columns) {
        if (firstWindowInColumn(column))
            columns.emplace_back(column);
    }

    const bool PRIMARYHORIZONTAL = ALGO->m_scrollingData->controller && ALGO->m_scrollingData->controller->isPrimaryHorizontal();
    std::ranges::sort(columns, [PRIMARYHORIZONTAL](const auto& a, const auto& b) {
        const auto getColumnPosition = [PRIMARYHORIZONTAL](const auto& column) {
            if (!column || column->targetDatas.empty() || !column->targetDatas[0])
                return std::numeric_limits<double>::max();

            return PRIMARYHORIZONTAL ? column->targetDatas[0]->layoutBox.x : column->targetDatas[0]->layoutBox.y;
        };

        return getColumnPosition(a) < getColumnPosition(b);
    });

    const auto CURRENTIT = std::ranges::find(columns, CURRENTCOL);
    if (CURRENTIT == columns.end())
        return false;

    const auto CURRENTIDX = sc<int64_t>(std::distance(columns.begin(), CURRENTIT));
    const auto TARGETIDX  = CURRENTIDX + (next ? 1 : -1);
    if (TARGETIDX < 0 || TARGETIDX >= sc<int64_t>(columns.size()))
        return false;

    const auto TARGETCOL = columns[TARGETIDX];
    const auto WINDOW    = firstWindowInColumn(TARGETCOL);
    if (!WINDOW)
        return false;

    ALGO->m_scrollingData->centerOrFitCol(TARGETCOL);
    ALGO->m_scrollingData->recalculate();

    closeOnWindow = WINDOW;
    rememberSelection(WINDOW);
    syncFocusedSelection();
    damage();

    return true;
}

// The live handles a drop needs, filled in by the three gather steps below in
// the order endWindowDrag() runs them. The pure half -- what to DO with them --
// is Hyprdrive::Drop::resolve(facts) in DropDecision.hpp.
struct SOverviewDropSite {
    Hyprdrive::Drop::SFacts facts;

    // stage 1
    PHLWINDOW              window;
    SP<Layout::ITarget>    target;
    SP<Layout::CSpace>     space;
    SP<Layout::CAlgorithm> algo;
    COverview*       dropOverview = nullptr;
    PHLMONITOR             dropMonitor;
    Vector2D               dropPointLocal;

    // stage 2
    size_t                              dropWorkspaceIdx = 0;
    PHLWORKSPACE                        dropWorkspace;
    Layout::Tiled::CScrollingAlgorithm* dropScrollingAlgo = nullptr;
    bool                                dropScrollingPrimaryHorizontal = false;
    PHLWORKSPACE                        originalWorkspace;
    CBox                                dragBox;

    // stage 3
    PHLWINDOW                   sourceFullscreenWindow;
    PHLMONITOR                  monitor;
    PHLWORKSPACE                activeWorkspaceBeforeDrop;
    PHLWORKSPACE                dropActiveWorkspaceBeforeDrop;
};

// Stage 1. The window that was dragged, its layout handles, and the overview
// the button came up over. Deliberately asks nothing about a workspace: a
// release on the dock pill returns before that question is worth money.
void COverview::gatherDropSite(SOverviewDropSite& site) {
    site.window = getOverviewWindowToShow(drag.activeWindow.lock());
    site.target = site.window ? site.window->layoutTarget() : nullptr;
    site.space  = site.target ? site.target->space() : nullptr;
    site.algo   = site.space ? site.space->algorithm() : nullptr;

    const auto TARGETOVERVIEW = scrollOverviewAt(g_pInputManager->getMouseCoordsInternal());
    site.dropOverview         = TARGETOVERVIEW ? dynamic_cast<COverview*>(TARGETOVERVIEW.get()) : nullptr;

    site.dropMonitor    = site.dropOverview ? site.dropOverview->pMonitor.lock() : PHLMONITOR{};
    site.dropPointLocal = getOverviewMousePosLocal(site.dropMonitor);

    site.facts.haveWindow = static_cast<bool>(site.window);
    site.facts.haveTarget = static_cast<bool>(site.target);
    // OVERVIEW_CPP_SPEC X26. Released on the dock: minimize, and nothing else
    // runs -- the window must not also be moved to whatever card the pill floats
    // over. Everything else about a missed drop is unchanged, because
    // workspaceAtOverviewDropPoint already answers nullptr over the pill.
    // Asked only with a window in hand, as the guard it replaced was.
    site.facts.overPill = site.window && Hyprdrive::Dock::pillContains(site.dropMonitor, site.dropPointLocal);
}

// Stage 2. Which workspace the cursor is over and what kind of layout it runs.
void COverview::gatherDropWorkspace(SOverviewDropSite& site) {
    site.dropWorkspace = site.dropOverview ? site.dropOverview->workspaceAtOverviewDropPoint(site.dropPointLocal, &site.dropWorkspaceIdx, site.window) : PHLWORKSPACE{};
    site.dropScrollingAlgo = overviewScrollingAlgorithmForWorkspace(site.dropWorkspace);
    site.dropScrollingPrimaryHorizontal = site.dropScrollingAlgo && site.dropScrollingAlgo->m_scrollingData && site.dropScrollingAlgo->m_scrollingData->controller &&
        site.dropScrollingAlgo->m_scrollingData->controller->isPrimaryHorizontal();
    site.originalWorkspace = drag.originalWorkspace.lock();
    site.dragBox = site.dropWorkspace ? site.dropOverview->draggedWindowBoxFor(site.window, site.dropWorkspaceIdx, site.dropPointLocal, dragEffectiveGrabRatio()) : CBox{};

    site.facts.haveDropWorkspace = static_cast<bool>(site.dropWorkspace);
    site.facts.retileOnEnd       = drag.startedTiled && site.target && site.space && site.algo;
    site.facts.moveWorkspace     = site.dropWorkspace && site.dropWorkspace != site.originalWorkspace;
    site.facts.startedTiled      = drag.startedTiled;
    site.facts.scrollingLayout   = site.dropScrollingAlgo != nullptr;
    site.facts.haveOriginalBox   = !drag.originalBox.empty();
}

// Stage 3. Where on the destination card, and everything the commit has to
// have read BEFORE it starts moving things: the source workspace's fullscreen
// window and both monitors' active workspaces.
void COverview::gatherDropAnchor(SOverviewDropSite& site) {
    site.sourceFullscreenWindow =
        site.originalWorkspace ? getOverviewWindowToShow(Fullscreen::controller()->getFullscreenWindow(site.originalWorkspace)) : PHLWINDOW{};
    site.facts.restoreSourceFullscreenFocus = site.window && site.window->m_isFloating && site.facts.moveWorkspace &&
        shouldShowOverviewWindow(site.sourceFullscreenWindow) && site.sourceFullscreenWindow != site.window &&
        site.sourceFullscreenWindow->m_workspace == site.originalWorkspace && Fullscreen::controller()->isFullscreen(site.sourceFullscreenWindow);

    site.monitor                       = pMonitor.lock();
    site.activeWorkspaceBeforeDrop     = site.monitor ? site.monitor->m_activeWorkspace : PHLWORKSPACE{};
    site.dropActiveWorkspaceBeforeDrop = site.dropMonitor ? site.dropMonitor->m_activeWorkspace : PHLWORKSPACE{};

    // Z3. Whether the drop point can be read as a point INSIDE the destination
    // card, which is the only thing that makes mapping the cursor into that
    // workspace meaningful. Two conditions, and X42 only had the first:
    //
    //   * the card is fully on screen, and
    //   * the drop did not land on the DESKTOP STRIP.
    //
    // X42 assumed the second followed from the first -- "a strip-mini drop
    // targets a card that is off-screen by construction". It does not: the strip
    // shows every desktop including the one whose card fills the viewport, so
    // dropping on THAT mini gave a fully-visible card and a cursor sitting up in
    // the strip band. overviewPointToGlobal then mapped a strip coordinate into
    // the workspace and the window landed jammed against the top edge --
    // measured (200,150) -> (324,1) -- which is the user's "subsequent moves of
    // that same window do not remember coordinates". The first move is to
    // another desktop, whose card is off-screen, so it kept its position; moving
    // it back to the desktop you are looking at is the one that loses it.
    //
    // workspaceAtStripPoint is the honest test rather than "is the point in the
    // card box": the strip band deliberately OVERLAPS the tiles it floats above,
    // which workspaceAtOverviewDropPoint already has to say out loud for the
    // same reason.
    if (site.dropMonitor) {
        const auto WORKSPACEBOX = getOverviewWorkspaceBox(
            site.dropMonitor, site.dropOverview->scale->value(), site.dropOverview->viewOffset->value(),
            site.dropOverview->workspaceOverviewOffset(site.dropWorkspaceIdx, site.dropOverview->activeWorkspaceIndex(),
                                                       getWorkspaceRenderedPitch(site.dropMonitor, site.dropOverview->scale->value(), site.dropOverview->layout)),
            site.dropOverview->layout);
        site.facts.dropPointMapsIntoCard =
            overviewBoxFullyVisibleOnMonitor(WORKSPACEBOX, site.dropMonitor) && !site.dropOverview->workspaceAtStripPoint(site.dropPointLocal);
    }
}

void COverview::commitDropMinimize(const SOverviewDropSite& site) {
    // Y5/Y8. Was a bare Minimize::minimize, which is exactly what X54's
    // "everything that minimizes must come through these" forbids -- so a
    // window dropped on the dock TELEPORTED while a clicked one flew. Now it
    // flies like every other minimize, and `true` asks the icon it lands in
    // to bounce once.
    // From where it was let go: the dragged proxy, not the grid slot it left
    // (user, 2026-09-30: the flight started back at the tile, and with motion
    // blur that read as the whole window smearing down into the icon).
    std::optional<CBox> startBox;
    if (site.window && (!site.dropOverview || site.dropOverview == this))
        startBox = draggedWindowBox(dragWorkspaceIndex(site.window));
    Hyprdrive::Dock::minimizeWithFlight(site.dropMonitor, site.window, true, startBox);
    clearDragPending();
    damage();
    if (site.dropOverview && site.dropOverview != this)
        site.dropOverview->damage();
}

void COverview::commitDropCancel(const SOverviewDropSite& site) {
    // OVERVIEW_CPP_SPEC D5. Nothing is moved, so the window keeps its
    // workspace, position and size — the drop is correctly a no-op. Without
    // the seed below the tile still teleports home in one frame (measured:
    // 326 px in 1 frame at 60fps), which reads as a glitch rather than a
    // rejected drop. U4's reflow already eases a tile from wherever it is to
    // its slot, so hand it the proxy's box and it carries the return.
    if (const auto MON = pMonitor.lock(); site.window && MON) {
        if (CBox slot; overviewGridSlot(site.window, MON, slot)) {
            const auto  WSIDX    = dragWorkspaceIndex(site.window);
            const auto  FLOATBOX = draggedWindowBox(WSIDX);
            const float SCALEDIV = std::max(scale->value(), 0.01F) * std::max(MON->m_scale, 0.01F);

            gridReflowSeed(site.window, MON, CBox{overviewPointToGlobal(WSIDX, FLOATBOX.pos()), FLOATBOX.size() * (1.F / SCALEDIV)}, slot);
        }
    }

    clearDragPending();
    damage();
    if (site.dropOverview && site.dropOverview != this)
        site.dropOverview->damage();
}

void COverview::commitDrop(const SOverviewDropSite& site, const Hyprdrive::Drop::SDecision& DECISION) {
    const auto  WINDOW        = site.window;
    const auto& TARGET        = site.target;
    const auto& SPACE         = site.space;
    const auto  DROPWORKSPACE = site.dropWorkspace;
    const auto  DROPMONITOR   = site.dropMonitor;
    const auto  MONITOR       = site.monitor;
    auto* const dropOverview  = site.dropOverview;

    const auto RESTOREACTIVEWORKSPACE = [&]() {
        if (MONITOR && site.activeWorkspaceBeforeDrop && MONITOR->m_activeWorkspace != site.activeWorkspaceBeforeDrop)
            MONITOR->changeWorkspace(site.activeWorkspaceBeforeDrop, false, true, true);
        if (DROPMONITOR && DROPMONITOR != MONITOR && site.dropActiveWorkspaceBeforeDrop && DROPMONITOR->m_activeWorkspace != site.dropActiveWorkspaceBeforeDrop)
            DROPMONITOR->changeWorkspace(site.dropActiveWorkspaceBeforeDrop, false, true, true);
    };
    const auto RESTOREVIEWPORTWORKSPACE = [&]() {
        if (!site.activeWorkspaceBeforeDrop)
            return;

        for (size_t i = 0; i < images.size(); ++i) {
            if (images[i] && images[i]->pWorkspace == site.activeWorkspaceBeforeDrop) {
                viewportCurrentWorkspace = i;
                return;
            }
        }
    };

    using enum Hyprdrive::Drop::EAction;
    using EPlacement = Hyprdrive::Drop::EPlacement;

    switch (DECISION.action) {
        case RetileMove: {
            Desktop::globalWindowController()->moveWindowToWorkspace(WINDOW, DROPWORKSPACE);
            RESTOREACTIVEWORKSPACE();

            if (DECISION.placement == EPlacement::ScrollEdgeFar)
                moveOverviewScrollingTargetToWorkspaceEdge(TARGET, 1);

            // An ARRIVAL is the layout's to place, not the drop point's: the move
            // above filed it wherever dwindle's cursor-reading insert put it, so
            // re-file it the way opening it on that desktop would (Placement.hpp).
            // Scrolling desktops take it at the far end of the tape, above.
            if (!site.facts.scrollingLayout)
                Hyprdrive::Placement::retileAsNew(WINDOW, Hyprdrive::Placement::takeDragAnchor());

            TARGET->rememberFloatingSize(drag.originalFloatSize);

            // The destination is usually NOT the active workspace, and an
            // inactive workspace is not walked for layout the way the active one
            // is -- so a window dropped on a distant desktop kept the geometry
            // it had on the one it left, and the overview kept drawing it that
            // way. Ask the destination to re-tile explicitly. This is the same
            // call the local-drop path below already makes; it was simply never
            // made for the cross-workspace one.
            if (DROPWORKSPACE)
                DROPWORKSPACE->updateWindows();

            // The retile above gives it a new SIZE; this is what lets it draw at
            // that size while its desktop is off screen.
            Hyprdrive::Dock::noteRetiled(WINDOW);

            RESTOREACTIVEWORKSPACE();
            RESTOREVIEWPORTWORKSPACE();
            break;
        }
        case FloatMove: {
            Desktop::globalWindowController()->moveWindowToWorkspace(WINDOW, DROPWORKSPACE);
            RESTOREACTIVEWORKSPACE();
            if (TARGET) {
                // X41/X42: the proxy box is the MISSION-CONTROL SLOT, i.e. real size
                // x bestScale (overviewGridLayout), so descaling it by the overview
                // and monitor scale alone still commits a shrunken window. bestScale
                // is capped at 1.0, which is why uniform fixture tiles never caught
                // this and only a large window shows it. A drag moves a window; it
                // must never resize it.
                const auto GLOBALSIZE = DECISION.useOriginalBox ?
                    drag.originalBox.size() :
                    site.dragBox.size() * (1.F / (std::max(dropOverview->scale->value(), 0.01F) * std::max(DROPMONITOR ? DROPMONITOR->m_scale : 1.F, 0.01F)));
                // X42: a strip-mini drop targets a card that is off-screen by
                // construction, so there is no in-card point to map the cursor to.
                // Keeping the window where it already was is what stops every
                // arrival stacking up on the workspace centre.
                auto GLOBALBOX = DECISION.mapPointIntoCard ? CBox{dropOverview->overviewPointToGlobal(site.dropWorkspaceIdx, site.dragBox.pos()), GLOBALSIZE} :
                    DECISION.useOriginalBox                ? CBox{drag.originalBox.pos(), GLOBALSIZE} :
                                                             centerBoxInWorkspace(CBox{Vector2D{}, GLOBALSIZE}, DROPWORKSPACE, DROPMONITOR);
                GLOBALBOX      = clampBoxToWorkspace(GLOBALBOX, DROPWORKSPACE, DROPMONITOR, WINDOW->getRealBorderSize());


                TARGET->setPositionGlobal(GLOBALBOX);
                TARGET->warpPositionSize();
            }

            RESTOREACTIVEWORKSPACE();
            RESTOREVIEWPORTWORKSPACE();
            break;
        }
        default: break;
    }

    if (DECISION.restoreSourceFullscreenFocus) {
        const bool POSTHIDDENEVENT = MONITOR && site.originalWorkspace == MONITOR->m_activeWorkspace;

        if (POSTHIDDENEVENT) {
            closeOnWindow = site.sourceFullscreenWindow;
            rememberSelection(site.sourceFullscreenWindow);
            focusOverviewFullscreenWindowIfActiveWorkspace(site.sourceFullscreenWindow, site.originalWorkspace, MONITOR);
            emitFullscreenVisibilityState(site.sourceFullscreenWindow, true);
        }
    }

    if (DROPWORKSPACE && DROPMONITOR && DROPWORKSPACE == DROPMONITOR->m_activeWorkspace) {
        const auto FULLSCREENWINDOW = getOverviewWindowToShow(Fullscreen::controller()->getFullscreenWindow(DROPWORKSPACE));
        if (shouldShowOverviewWindow(FULLSCREENWINDOW) && FULLSCREENWINDOW->m_workspace == DROPWORKSPACE)
            dropOverview->emitFullscreenVisibilityState(FULLSCREENWINDOW, true);
    }

    clearDragPending();
    rebuildPending = true;
    damage();
    if (dropOverview != this) {
        dropOverview->rebuildPending = true;
        dropOverview->damage();
    }
}

// Gather, decide, commit -- and ask the pure resolver after each gathering
// stage, so the two early exits are the same ladder the final one is rather
// than a second copy of it.
void COverview::endWindowDrag() {
    // A completed hold, let go on the same window it was held over: swap. Asked
    // BEFORE the grid closes the dragged window's gap below, or the tiles under
    // the pointer would shift out from under it at the last moment.
    if (drag.swapArmed && Hyprdrive::Config::getValue<bool>(KEY_HOLD_TO_SWAP)) {
        const auto DRAGGED = getOverviewWindowToShow(drag.activeWindow.lock());
        const auto OTHER   = swapCandidateAt(lastMousePosLocal, nullptr);
        if (DRAGGED && OTHER && OTHER == drag.swapCandidate.lock() && commitSwap(DRAGGED, OTHER))
            return;
    }

    // Before anything below reads a slot: the D5 return seed wants this window's
    // slot in the FULL grid, not the gap it has been leaving open.
    grid().dragExcluded = nullptr;

    SOverviewDropSite site;

    gatherDropSite(site);

    // Released on ANOTHER monitor's strip: it is exactly a drop on that desktop's
    // cell in that monitor's own strip, which the cross-monitor drop path already
    // handles end to end. So hand the rest of the ladder that site -- that
    // overview, that monitor, and the middle of that cell there -- instead of
    // teaching every stage below about a second kind of cell.
    if (const auto HIT = site.window ? altCellAt(site.dropPointLocal) : std::nullopt) {
        const auto THERE = HIT->overview->stripCellBox(HIT->workspaceIdx, HIT->monitor, HIT->overview->scale->value());
        if (THERE.width > 1 && THERE.height > 1) {
            site.dropOverview   = HIT->overview;
            site.dropMonitor    = HIT->monitor;
            site.dropPointLocal = THERE.middle();
            site.facts.overPill = false;
        }
    }

    // The detached scratchpad cell is not one of `images`, so the decision table
    // below cannot see it and must not be asked about it. A release on the cell
    // parks the window there and the drag ends here -- ensureDefault because a
    // user with no scratchpad yet is exactly who drags a window onto it first.
    if (scratchpadCellContains(site.dropPointLocal)) {
        if (const auto SCRATCH = Hyprdrive::Scratchpad::ensureDefault(pMonitor.lock())) {
            Hyprdrive::Scratchpad::moveWindowTo(site.window, SCRATCH);
            clearDragPending();
            damage();
            return;
        }
    }

    if (Hyprdrive::Drop::resolve(site.facts).action == Hyprdrive::Drop::EAction::Minimize) {
        commitDropMinimize(site);
        return;
    }

    gatherDropWorkspace(site);
    if (Hyprdrive::Drop::resolve(site.facts).action == Hyprdrive::Drop::EAction::CancelToGrid) {
        commitDropCancel(site);
        return;
    }

    gatherDropAnchor(site);
    commitDrop(site, Hyprdrive::Drop::resolve(site.facts));
}

void COverview::endWindowResize() {
    const auto WINDOW  = getOverviewWindowToShow(resize.activeWindow.lock());
    const auto TARGET  = WINDOW ? WINDOW->layoutTarget() : nullptr;
    const auto MONITOR = pMonitor.lock();

    if (WINDOW && TARGET && WINDOW->m_isFloating && resize.workspaceIdx < images.size()) {
        const auto RESIZEDBOX  = resizedWindowBox();
        const auto SCALEFACTOR = std::max(scale->value(), 0.01F) * std::max(MONITOR ? MONITOR->m_scale : 1.F, 0.01F);
        auto       GLOBALBOX   = CBox{overviewPointToGlobal(resize.workspaceIdx, RESIZEDBOX.pos()), RESIZEDBOX.size() * (1.F / SCALEFACTOR)};

        TARGET->damageEntire();
        TARGET->rememberFloatingSize(GLOBALBOX.size());
        TARGET->setPositionGlobal(GLOBALBOX);
        TARGET->warpPositionSize();
        TARGET->damageEntire();
    }

    resize.pointerDown = false;
    resize.pendingWindow.reset();
    resize.activeWindow.reset();
    resize.originalBox  = CBox{};
    resize.lastMouseLocal = Vector2D{};
    resize.corner       = Layout::CORNER_NONE;
    rebuildPending     = true;
    damage();
}
