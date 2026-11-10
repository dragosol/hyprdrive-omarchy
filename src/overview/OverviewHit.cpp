// CScrollOverview: hit-testing, hover, click actions.
// Cut from scrollOverview.cpp by pure code motion.
#include "OverviewParts.hpp"

using namespace OverviewDetail;


PHLWINDOW CScrollOverview::windowAtOverviewPoint(const Vector2D& point, size_t* hoveredWorkspaceIdx) const {
    size_t activeIdx = activeWorkspaceIndex();
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR)
        return nullptr;

    const auto WORKSPACEPITCH = getWorkspaceRenderedPitch(MONITOR, scale->value(), layout);
    for (size_t workspaceIdx = 0; workspaceIdx < images.size(); ++workspaceIdx) {
        const auto& wimg = images[workspaceIdx];
        const auto  offset = workspaceOverviewOffset(workspaceIdx, activeIdx, WORKSPACEPITCH);

        const auto selectWindow = [&](const PHLWINDOW& window) -> PHLWINDOW {
            if (hoveredWorkspaceIdx)
                *hoveredWorkspaceIdx = workspaceIdx;

            return window;
        };

        const auto fullscreenWindow = overviewFullscreenPathWindow(wimg->pWorkspace);

        if (fullscreenWindow) {
            for (auto it = wimg->windows.rbegin(); it != wimg->windows.rend(); ++it) {
                const auto window = getOverviewWindowToShow(it->lock());
                if (!overviewTileShown(window, MONITOR) || !window->m_isFloating)
                    continue;

                const auto texbox = getOverviewWindowBox(window, MONITOR, scale->value(), viewOffset->value(), offset, layout);

                if (texbox.containsPoint(point))
                    return selectWindow(window);
            }

            const auto texbox = getOverviewWindowBox(fullscreenWindow, MONITOR, scale->value(), viewOffset->value(), offset, layout);

            if (texbox.containsPoint(point))
                return selectWindow(fullscreenWindow);

            continue;
        }

        for (const bool floating : {true, false}) {
            for (auto it = wimg->windows.rbegin(); it != wimg->windows.rend(); ++it) {
                const auto window = getOverviewWindowToShow(it->lock());
                if (!overviewTileShown(window, MONITOR) || window->m_isFloating != floating)
                    continue;

                const auto texbox = getOverviewWindowBox(window, MONITOR, scale->value(), viewOffset->value(), offset, layout);

                if (texbox.containsPoint(point))
                    return selectWindow(window);
            }
        }
    }

    return nullptr;
}

PHLWINDOW CScrollOverview::windowAtOverviewCursor(size_t* hoveredWorkspaceIdx) {
    return windowAtOverviewPoint(lastMousePosLocal, hoveredWorkspaceIdx);
}

// The tile as DRAWN -- grown by the hover ease, shrunk by the press ease -- so
// the close button the user aims at and the circle that gets hit-tested are the
// same circle.
CBox CScrollOverview::tileBox(const PHLWINDOW& window) const {
    const auto MONITOR = pMonitor.lock();
    if (!window || !MONITOR)
        return {};

    // By HOME desktop, the same rule the grid membership uses (Y9). A minimized
    // window's m_workspace is the `special:minimized` stash, which is no card at
    // all: matching on it returned an empty box for a tile the grid was DRAWING,
    // and every keyboard walk dropped that tile as if it were not there.
    const WORKSPACEID HOME         = overviewGridHomeWorkspace(window);
    size_t            workspaceIdx = images.size();
    for (size_t i = 0; i < images.size(); ++i) {
        if (images[i] && images[i]->pWorkspace && images[i]->pWorkspace->m_id == HOME) {
            workspaceIdx = i;
            break;
        }
    }

    if (workspaceIdx >= images.size())
        return {};

    const auto OFFSET = workspaceOverviewOffset(workspaceIdx, activeWorkspaceIndex(), getWorkspaceRenderedPitch(MONITOR, scale->value(), layout));
    return TileChrome::scaledBox(window, getOverviewWindowBox(window, MONITOR, scale->value(), viewOffset->value(), OFFSET, layout));
}

CBox CScrollOverview::overviewBoxForGlobal(WORKSPACEID workspaceId, const CBox& globalBox) const {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR)
        return {};

    size_t workspaceIdx = images.size();
    for (size_t i = 0; i < images.size(); ++i) {
        if (images[i] && images[i]->pWorkspace && images[i]->pWorkspace->m_id == workspaceId) {
            workspaceIdx = i;
            break;
        }
    }

    if (workspaceIdx >= images.size())
        return {};

    const auto OFFSET = workspaceOverviewOffset(workspaceIdx, activeWorkspaceIndex(), getWorkspaceRenderedPitch(MONITOR, scale->value(), layout));
    return getOverviewGlobalBox(globalBox, MONITOR, scale->value(), viewOffset->value(), OFFSET, layout);
}

bool CScrollOverview::selectAndClose(const PHLWINDOW& window) {
    if (!window)
        return false;

    // The window's own workspace, not the one in view: X28's menu can list a
    // window on any desktop, and committing the selection without moving the
    // viewport to it would close the overview onto the wrong one.
    size_t workspaceIdx = images.size();
    for (size_t i = 0; i < images.size(); ++i) {
        if (images[i] && images[i]->pWorkspace == window->m_workspace) {
            workspaceIdx = i;
            break;
        }
    }

    if (workspaceIdx >= images.size()) {
        closeOnWindow = window;
        if (!pickKeepsFocus())
            for (const auto& overview : scrollOverviews()) {
                if (overview && overview.get() != this)
                    overview->close();
            }
        close();
        return true;
    }

    if (!selectOverviewWindow(window, workspaceIdx, true))
        return false;

    if (!pickKeepsFocus())
        for (const auto& overview : scrollOverviews()) {
            if (overview && overview.get() != this)
                overview->close();
        }
    close();
    return true;
}

CBox CScrollOverview::tileCloseButtonBox(const PHLWINDOW& window) const {
    const auto MONITOR = pMonitor.lock();
    // The HIT box, not the drawn one: this is used for the hover keep-alive and
    // for clicks, and the drawn box empties itself as the button fades.
    return MONITOR ? TileChrome::closeButtonHitBox(window, tileBox(window), MONITOR->m_scale) : CBox{};
}

// X7. Hover is hit-tested against the UNSCALED slot. Testing the grown box would
// let a tile that grew under the cursor hold its own hover, and the shrink on the
// way out would re-enter it -- a tile that flutters rather than one that bounces.
// The close button is the single exception: it is drawn on the grown box, so it
// pokes a few px past the slot, and without this the hover (and the button with
// it) would vanish exactly as the cursor arrived on it.
void CScrollOverview::updateTileHover() {
    // The pointer moved, so the pointer owns the ring again. Whatever the
    // arrows had selected keeps its role for Return and for close(); it just
    // stops being the thing drawn.
    //
    // It asks for the frame ITSELF. The motion handler's frame request is
    // conditional on the hover signature moving, and this can fire on a motion
    // over empty space where nothing else changes at all -- the arrow-selected
    // tile's ring would then stay on screen until something else repainted.
    if (g_search.nav.keyboardTile) {
        g_search.nav.keyboardTile = false;
        damage();
        requestInputFrame();
    }

    if (closing || drag.activeWindow || resize.activeWindow || pan.pointerDown) {
        TileChrome::setHovered(PHLWINDOW{});
        return;
    }

    // The linger is for reaching the close button through empty space. The
    // dock is a different surface: keep the hold and the title sits on the
    // pill, so drop it the moment the pointer is on the dock.
    if (ScrollOverview::Dock::uiContains(pMonitor.lock(), lastMousePosLocal)) {
        TileChrome::setHovered(PHLWINDOW{}, false);
        return;
    }

    // ...and the same for the desktop strip (user, 2026-09-23). It is another
    // surface, not empty space on the way to a close button, and a tile left
    // blown up behind it sits in the way of the thing the pointer went to look
    // at. The whole PLATE counts -- the gaps between cells too -- plus the
    // scratchpad cell and the other monitors' strips.
    if (stripUiContains(lastMousePosLocal)) {
        TileChrome::setHovered(PHLWINDOW{}, false);
        return;
    }

    // X62. A result under the cursor takes the selection, so pointing at a row
    // and arrowing onto it land on the SAME highlight -- there is only one
    // selection and both devices move it. It also swallows the tile hover: the
    // panel is drawn over the grid while the band eases, and a tile lighting up
    // through it would be hovering something the user cannot click.
    if (const auto* ROW = searchRowAt(searchMonitorId(), lastMousePosLocal); ROW) {
        TileChrome::setHovered(PHLWINDOW{});
        g_search.nav.zone = EOverviewNavZone::Result;
        if (g_search.nav.selection != ROW->index) {
            g_search.nav.selection = ROW->index;
            damage();
        }
        return;
    }

    // The button is drawn on the GROWN box so it pokes a few px past the slot,
    // and during the post-hover linger it belongs to a tile the hover test no
    // longer reports at all. Both cases RE-ARM rather than just bail: bailing
    // left the linger counting down under a cursor that had arrived on the
    // button, so the tile could shrink away mid-aim.
    if (const auto HOVERED = TileChrome::hovered(); HOVERED && tileCloseButtonBox(HOVERED).containsPoint(lastMousePosLocal)) {
        TileChrome::setHovered(HOVERED);
        return;
    }

    TileChrome::setHovered(windowAtOverviewPoint(lastMousePosLocal));
}

// See SHoverSignature. Read AFTER updateTileHover, so the tile half is the
// answer that frame would draw rather than the one before it.
CScrollOverview::SHoverSignature CScrollOverview::hoverSignature() const {
    SHoverSignature sig;

    const auto MONITOR = pMonitor.lock();
    if (!MONITOR)
        return sig;

    if (const auto HOVERED = TileChrome::hovered()) {
        sig.tile        = HOVERED.get();
        sig.closeButton = tileCloseButtonBox(HOVERED).containsPoint(lastMousePosLocal);
    }

    sig.searchHover     = g_search.nav.hoverIndex;
    sig.searchSelection = g_search.nav.selection;
    sig.searchZone      = sc<int>(g_search.nav.zone);

    // The two invisible reveal zones. They are read by the RENDER (from
    // updateScratchpadReveal / updateAltStripsReveal), so entering one has to
    // buy a frame or the reveal it exists to start would never begin -- and
    // leaving one has to buy a frame or it would never retract.
    const auto zoneHit = [&](const CBox& logical) {
        if (logical.width <= 1)
            return false;

        const auto ZONE = getOverviewGlobalBox(logical, MONITOR, scale->value(), OVERVIEW_CHROME_NO_PAN, 0.F, layout);
        return ZONE.width > 1 && ZONE.containsPoint(lastMousePosLocal);
    };

    sig.scratchZone = zoneHit(scratchpadZoneLogicalBox());
    sig.altZone     = zoneHit(altZoneLogicalBox());
    sig.scratchCell = scratchpadCellContains(lastMousePosLocal);

    if (const auto HIT = altCellAt(lastMousePosLocal)) {
        sig.altCellOverview = HIT->overview;
        sig.altCellIdx      = HIT->workspaceIdx;
    }

    // The numbered cells used to be absent here -- a cell only lit up under a
    // DRAG, and a drag asks for a frame on every motion regardless. They carry a
    // hover LABEL now (renderStripCellLabel), and plain motion onto a cell has
    // to buy the frame that draws it, or the label would trail the pointer by
    // however long it took something else to repaint.
    {
        size_t idx = images.size();
        if (workspaceAtStripPoint(lastMousePosLocal, &idx))
            sig.stripCell = idx;
    }

    return sig;
}

bool CScrollOverview::hoverSignatureChanged() {
    const auto SIG = hoverSignature();
    if (lastHoverSignatureValid && SIG == lastHoverSignature)
        return false;

    lastHoverSignature      = SIG;
    lastHoverSignatureValid = true;
    return true;
}

PHLWINDOW CScrollOverview::windowClosestToWorkspaceCenter(size_t workspaceIdx) const {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR || workspaceIdx >= images.size() || !images[workspaceIdx] || !images[workspaceIdx]->pWorkspace)
        return {};

    const auto& WORKSPACEIMAGE  = images[workspaceIdx];
    const auto  SCALE           = scale->value();
    const auto  WORKSPACEOFFSET =
        workspaceOverviewOffset(workspaceIdx, activeWorkspaceIndex(), getWorkspaceRenderedPitch(MONITOR, SCALE, layout));
    const auto WORKSPACEBOX = getOverviewWorkspaceBox(MONITOR, SCALE, viewOffset->value(), WORKSPACEOFFSET, layout);
    const auto FULLSCREENWINDOW  = overviewFullscreenPathWindow(WORKSPACEIMAGE->pWorkspace);
    const bool HASFULLSCREENPATH = FULLSCREENWINDOW != nullptr;

    PHLWINDOW bestWindow;
    double    bestDistance = std::numeric_limits<double>::max();

    for (const auto& windowRef : WORKSPACEIMAGE->windows) {
        const auto WINDOW = getOverviewWindowToShow(windowRef.lock());
        if (!shouldShowOverviewWindow(WINDOW))
            continue;
        if (HASFULLSCREENPATH && WINDOW != FULLSCREENWINDOW && !WINDOW->m_isFloating)
            continue;

        const auto WINDOWBOX = getOverviewWindowBox(WINDOW, MONITOR, SCALE, viewOffset->value(), WORKSPACEOFFSET, layout);
        const auto DISTANCE  = overviewBoxCenterDistanceSquared(WINDOWBOX, WORKSPACEBOX);
        if (DISTANCE >= bestDistance)
            continue;

        bestWindow   = WINDOW;
        bestDistance = DISTANCE;
    }

    return bestWindow;
}

CDropIndicator::SDropAnchor CScrollOverview::dropAnchorAtOverviewCursorOnWorkspace(size_t workspaceIdx, const PHLWINDOW& ignoredWindow,
                                                                                   CScrollOverview* dragContext) {
    CDropIndicator::SDropAnchor result;
    const auto                  DRAGCONTEXT = dragContext ? dragContext : this;

    const auto MONITOR = pMonitor.lock();
    if (!MONITOR || workspaceIdx >= images.size() || !images[workspaceIdx])
        return result;

    const auto& IMAGE     = images[workspaceIdx];
    const auto  WORKSPACE = IMAGE->pWorkspace;
    if (!WORKSPACE)
        return result;

    const auto WORKSPACE_OFFSET =
        workspaceOverviewOffset(workspaceIdx, activeWorkspaceIndex(), getWorkspaceRenderedPitch(MONITOR, scale->value(), layout));
    const auto WORKSPACEBOX = getOverviewWorkspaceBox(MONITOR, scale->value(), viewOffset->value(), WORKSPACE_OFFSET, layout);

    if (!overviewBoxFullyVisibleOnMonitor(WORKSPACEBOX, MONITOR))
        return result;

    if (!DRAGCONTEXT->drag.startedTiled)
        return result;

    const auto boxesForWindow = [&](const PHLWINDOW& window) {
        const auto TARGET = window->layoutTarget();
        return std::pair{
            getOverviewWindowBox(window, MONITOR, scale->value(), viewOffset->value(), WORKSPACE_OFFSET, layout, false),
            TARGET ? getOverviewGlobalBox(TARGET->position(), MONITOR, scale->value(), viewOffset->value(), WORKSPACE_OFFSET, layout, false) : CBox{},
        };
    };
    const auto setAnchor = [](CDropIndicator::SDropAnchor& anchor, const PHLWINDOW& window, const CBox& box, const CBox& logicalBox = {}, const std::string& direction = {}) {
        anchor.window     = window;
        anchor.box        = box;
        anchor.logicalBox = logicalBox;
        anchor.direction  = direction;
    };

    const auto directionForBox = [&](const CBox& box) {
        const auto LOCAL_X = lastMousePosLocal.x - box.x;
        const auto LOCAL_Y = lastMousePosLocal.y - box.y;

        if (LOCAL_X < box.width / 3.F)
            return std::string{"l"};
        if (LOCAL_X > box.width * 2.F / 3.F)
            return std::string{"r"};
        return LOCAL_Y < box.height / 2.F ? std::string{"u"} : std::string{"d"};
    };

    DRAGCONTEXT->refreshDragOriginalOverviewBoxes();

    const auto ORIGINALHITBOX =
        DRAGCONTEXT->drag.originalOverviewHitbox.empty() ? DRAGCONTEXT->drag.originalOverviewBox : DRAGCONTEXT->drag.originalOverviewHitbox;
    if (ignoredWindow && !DRAGCONTEXT->drag.originalOverviewBox.empty() && ORIGINALHITBOX.containsPoint(lastMousePosLocal) &&
        WORKSPACE == DRAGCONTEXT->drag.originalWorkspace.lock()) {
        setAnchor(result, ignoredWindow, DRAGCONTEXT->drag.originalOverviewBox);
        return result;
    }

    const auto ALGO = overviewScrollingAlgorithmForWorkspace(WORKSPACE);
    const bool PRIMARYHORIZONTAL = ALGO && ALGO->m_scrollingData && ALGO->m_scrollingData->controller && ALGO->m_scrollingData->controller->isPrimaryHorizontal();
    const auto pointsToOriginalStackSlot = [&](const PHLWINDOW& anchor, const std::string& direction) {
        if (!ALGO || !ALGO->m_scrollingData || !ALGO->m_scrollingData->controller || !ignoredWindow || DRAGCONTEXT->drag.originalOverviewBox.empty() ||
            WORKSPACE != DRAGCONTEXT->drag.originalWorkspace.lock() || !anchor || !anchor->layoutTarget() || !ignoredWindow->layoutTarget())
            return false;

        const auto ANCHORDATA  = ALGO->dataFor(anchor->layoutTarget());
        const auto ORIGINALDATA = ALGO->dataFor(ignoredWindow->layoutTarget());
        const auto ANCHORCOL   = ANCHORDATA ? ANCHORDATA->column.lock() : nullptr;
        const auto ORIGINALCOL = ORIGINALDATA ? ORIGINALDATA->column.lock() : nullptr;
        if (!ANCHORCOL || ANCHORCOL != ORIGINALCOL)
            return false;

        const auto ANCHORIDX   = ANCHORCOL->idx(anchor->layoutTarget());
        const auto ORIGINALIDX = ORIGINALCOL->idx(ignoredWindow->layoutTarget());
        if (ANCHORIDX == ORIGINALIDX)
            return false;

        const bool ANCHORPREVIOUS = ANCHORIDX < ORIGINALIDX;
        if ((ANCHORPREVIOUS && ANCHORIDX + 1 != ORIGINALIDX) || (!ANCHORPREVIOUS && ORIGINALIDX + 1 != ANCHORIDX))
            return false;

        if (PRIMARYHORIZONTAL)
            return (ANCHORPREVIOUS && direction == "d") || (!ANCHORPREVIOUS && direction == "u");

        return (ANCHORPREVIOUS && direction == "r") || (!ANCHORPREVIOUS && direction == "l");
    };
    const auto pointsToOriginalColumnSlot = [&](const PHLWINDOW& anchor, const std::string& direction) {
        if (!ALGO || !ALGO->m_scrollingData || !ALGO->m_scrollingData->controller || !ignoredWindow || DRAGCONTEXT->drag.originalOverviewBox.empty() ||
            WORKSPACE != DRAGCONTEXT->drag.originalWorkspace.lock() || !anchor || !anchor->layoutTarget() || !ignoredWindow->layoutTarget())
            return false;

        const auto ANCHORDATA   = ALGO->dataFor(anchor->layoutTarget());
        const auto ORIGINALDATA = ALGO->dataFor(ignoredWindow->layoutTarget());
        const auto ANCHORCOL    = ANCHORDATA ? ANCHORDATA->column.lock() : nullptr;
        const auto ORIGINALCOL  = ORIGINALDATA ? ORIGINALDATA->column.lock() : nullptr;
        if (!ANCHORCOL || !ORIGINALCOL || ORIGINALCOL->targetDatas.size() != 1)
            return false;

        const auto ANCHORCOLIDX   = ALGO->m_scrollingData->idx(ANCHORCOL);
        const auto ORIGINALCOLIDX = ALGO->m_scrollingData->idx(ORIGINALCOL);
        if (ANCHORCOLIDX < 0 || ORIGINALCOLIDX < 0 || ANCHORCOLIDX == ORIGINALCOLIDX)
            return false;

        const bool ANCHORPREVIOUS = ANCHORCOLIDX < ORIGINALCOLIDX;
        if ((ANCHORPREVIOUS && ANCHORCOLIDX + 1 != ORIGINALCOLIDX) || (!ANCHORPREVIOUS && ORIGINALCOLIDX + 1 != ANCHORCOLIDX))
            return false;

        if (PRIMARYHORIZONTAL)
            return (ANCHORPREVIOUS && direction == "r") || (!ANCHORPREVIOUS && direction == "l");

        return (ANCHORPREVIOUS && direction == "d") || (!ANCHORPREVIOUS && direction == "u");
    };
    const auto normalizeOriginalSlotAnchor = [&]() {
        if (pointsToOriginalStackSlot(result.window, result.direction) || pointsToOriginalColumnSlot(result.window, result.direction))
            setAnchor(result, ignoredWindow, DRAGCONTEXT->drag.originalOverviewBox);
    };

    float bestDistanceSq = std::numeric_limits<float>::max();
    for (auto it = IMAGE->windows.rbegin(); it != IMAGE->windows.rend(); ++it) {
        const auto WINDOW = getOverviewWindowToShow(it->lock());
        if (!shouldShowOverviewWindow(WINDOW) || WINDOW == ignoredWindow || WINDOW->m_isFloating)
            continue;

        const auto [BOX, LOGICALBOX] = boxesForWindow(WINDOW);
        const auto HITBOX            = LOGICALBOX.empty() ? BOX : LOGICALBOX;

        if (!HITBOX.containsPoint(lastMousePosLocal))
            continue;

        const auto distanceSq = overviewPointDistanceSqToBox(lastMousePosLocal, BOX);
        if (distanceSq >= bestDistanceSq)
            continue;

        setAnchor(result, WINDOW, BOX, LOGICALBOX, directionForBox(BOX));
        bestDistanceSq = distanceSq;
    }

    if (result.window) {
        normalizeOriginalSlotAnchor();
        return result;
    }

    if (!ALGO || !ALGO->m_scrollingData || !ALGO->m_scrollingData->controller)
        return result;

    const auto POINTER   = PRIMARYHORIZONTAL ? sc<float>(lastMousePosLocal.x) : sc<float>(lastMousePosLocal.y);
    float      firstEdge = std::numeric_limits<float>::max();
    float      lastEdge  = std::numeric_limits<float>::lowest();

    for (const auto& windowRef : IMAGE->windows) {
        const auto WINDOW = getOverviewWindowToShow(windowRef.lock());
        if (!shouldShowOverviewWindow(WINDOW) || WINDOW == ignoredWindow || WINDOW->m_isFloating)
            continue;

        const auto [BOX, LOGICALBOX] = boxesForWindow(WINDOW);
        const auto HITBOX            = LOGICALBOX.empty() ? BOX : LOGICALBOX;
        const auto MIN               = PRIMARYHORIZONTAL ? sc<float>(HITBOX.x) : sc<float>(HITBOX.y);
        const auto MAX               = PRIMARYHORIZONTAL ? sc<float>(HITBOX.x + HITBOX.width) : sc<float>(HITBOX.y + HITBOX.height);

        firstEdge = std::min(firstEdge, MIN);
        lastEdge  = std::max(lastEdge, MAX);
    }

    if (POINTER >= firstEdge && POINTER <= lastEdge)
        return result;

    const bool FIND_FIRST = POINTER < firstEdge;
    float      bestEdge   = FIND_FIRST ? std::numeric_limits<float>::max() : std::numeric_limits<float>::lowest();

    for (const auto& windowRef : IMAGE->windows) {
        const auto WINDOW = getOverviewWindowToShow(windowRef.lock());
        if (!shouldShowOverviewWindow(WINDOW) || WINDOW == ignoredWindow || WINDOW->m_isFloating)
            continue;

        const auto [BOX, LOGICALBOX] = boxesForWindow(WINDOW);
        const auto HITBOX            = LOGICALBOX.empty() ? BOX : LOGICALBOX;
        const auto EDGE              = PRIMARYHORIZONTAL ? sc<float>(FIND_FIRST ? HITBOX.x : HITBOX.x + HITBOX.width) :
                                                           sc<float>(FIND_FIRST ? HITBOX.y : HITBOX.y + HITBOX.height);

        if (FIND_FIRST ? EDGE < bestEdge : EDGE > bestEdge) {
            bestEdge = EDGE;
            setAnchor(result, WINDOW, BOX, LOGICALBOX);
        }
    }

    if (result.window) {
        result.direction = PRIMARYHORIZONTAL ? (FIND_FIRST ? "l" : "r") : (FIND_FIRST ? "u" : "d");
        normalizeOriginalSlotAnchor();
    }

    return result;
}

PHLWORKSPACE CScrollOverview::workspaceAtOverviewPoint(const Vector2D& point, size_t* hoveredWorkspaceIdx) const {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR)
        return nullptr;

    for (size_t workspaceIdx = 0; workspaceIdx < images.size(); ++workspaceIdx) {
        const auto& wimg = images[workspaceIdx];
        if (!wimg || !wimg->pWorkspace)
            continue;

        const auto WORKSPACEBOX = getOverviewWorkspaceUsableBox(wimg->pWorkspace, MONITOR, scale->value(), viewOffset->value(),
                                                                workspaceOverviewOffset(workspaceIdx, activeWorkspaceIndex(), getWorkspaceRenderedPitch(MONITOR, scale->value(), layout)),
                                                                layout);

        if (WORKSPACEBOX.containsPoint(point)) {
            if (hoveredWorkspaceIdx)
                *hoveredWorkspaceIdx = workspaceIdx;

            return wimg->pWorkspace;
        }
    }

    return nullptr;
}

PHLWORKSPACE CScrollOverview::workspaceAtOverviewDropPoint(const Vector2D& point, size_t* hoveredWorkspaceIdx, const PHLWINDOW& draggedWindow) const {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR)
        return nullptr;

    // X26: the dock is not a workspace, so it answers nullptr here -- and it
    // answers FIRST, because the pill floats over the main area and the card
    // underneath would otherwise claim the drop. Saying it once here is what
    // keeps the drop indicator, the mini shrink and endWindowDrag agreeing.
    if (ScrollOverview::Dock::pillContains(MONITOR, point))
        return nullptr;

    const auto ACTIVEIDX       = activeWorkspaceIndex();
    const auto WORKSPACEPITCH = getWorkspaceRenderedPitch(MONITOR, scale->value(), layout);

    // The band overlaps the main tiles it floats above, so it must be tested
    // first or a drop on the strip would land on whatever tile is underneath.
    if (const auto STRIPWORKSPACE = workspaceAtStripPoint(point, hoveredWorkspaceIdx); STRIPWORKSPACE)
        return STRIPWORKSPACE;

    for (size_t workspaceIdx = 0; workspaceIdx < images.size(); ++workspaceIdx) {
        const auto& wimg = images[workspaceIdx];
        if (!wimg || !wimg->pWorkspace)
            continue;

        const auto WORKSPACEOFFSET = workspaceOverviewOffset(workspaceIdx, ACTIVEIDX, WORKSPACEPITCH);
        for (const bool floating : {true, false}) {
            for (auto it = wimg->windows.rbegin(); it != wimg->windows.rend(); ++it) {
                const auto WINDOW = getOverviewWindowToShow(it->lock());
                if (!shouldShowOverviewWindow(WINDOW) || WINDOW == draggedWindow || WINDOW->m_isFloating != floating)
                    continue;

                const auto WINDOWBOX = getOverviewDragWindowBox(WINDOW, MONITOR, scale->value(), viewOffset->value(), WORKSPACEOFFSET, layout);
                if (!WINDOWBOX.containsPoint(point))
                    continue;

                if (hoveredWorkspaceIdx)
                    *hoveredWorkspaceIdx = workspaceIdx;

                return wimg->pWorkspace;
            }
        }
    }

    for (size_t workspaceIdx = 0; workspaceIdx < images.size(); ++workspaceIdx) {
        const auto& wimg = images[workspaceIdx];
        if (!wimg || !wimg->pWorkspace)
            continue;

        const auto WORKSPACEBOX = getOverviewWorkspaceUsableBox(wimg->pWorkspace, MONITOR, scale->value(), viewOffset->value(),
                                                                workspaceOverviewOffset(workspaceIdx, ACTIVEIDX, WORKSPACEPITCH), layout);
        const bool ONLAYOUTAXIS = layout == ScrollOverview::Config::ELayout::HORIZONTAL ?
            point.x >= WORKSPACEBOX.x && point.x <= WORKSPACEBOX.x + WORKSPACEBOX.width :
            point.y >= WORKSPACEBOX.y && point.y <= WORKSPACEBOX.y + WORKSPACEBOX.height;

        if (WORKSPACEBOX.containsPoint(point) || (ONLAYOUTAXIS && isWorkspaceScrolling(wimg->pWorkspace))) {
            if (hoveredWorkspaceIdx)
                *hoveredWorkspaceIdx = workspaceIdx;

            return wimg->pWorkspace;
        }
    }

    return nullptr;
}

PHLWORKSPACE CScrollOverview::workspaceAtOverviewCursor(size_t* hoveredWorkspaceIdx) const {
    return workspaceAtOverviewPoint(lastMousePosLocal, hoveredWorkspaceIdx);
}

bool CScrollOverview::selectOverviewWindow(PHLWINDOW window, size_t workspaceIdx, bool syncFocus) {
    if (!window)
        return false;

    closeOnWindow            = window;
    viewportCurrentWorkspace = workspaceIdx;
    rememberSelection(window);
    if (syncFocus)
        syncFocusedSelection();
    damage();
    return true;
}

bool CScrollOverview::selectWindowAtOverviewCursor(bool syncFocus) {
    lastMousePosLocal = getOverviewMousePosLocal(pMonitor.lock());

    size_t    workspaceIdx = viewportCurrentWorkspace;
    PHLWINDOW window       = windowAtOverviewCursor(&workspaceIdx);

    return selectOverviewWindow(window, workspaceIdx, syncFocus);
}

void CScrollOverview::focusOverviewWorkspace(const PHLWORKSPACE& workspace, size_t workspaceIdx) {
    if (!workspace)
        return;

    closeOnWindow.reset();
    viewportCurrentWorkspace = workspaceIdx;

    if (pMonitor && pMonitor->m_activeWorkspace != workspace) {
        if (focusSyncedFromWorkspaceID == WORKSPACE_INVALID)
            focusSyncedFromWorkspaceID = pMonitor->m_activeWorkspace ? pMonitor->m_activeWorkspace->m_id : WORKSPACE_INVALID;
        pMonitor->changeWorkspace(workspace, false, true, true);
    }

    damage();
}

// The full default click action, shared between the plain mouse path and the
// submap's `window click` dispatcher so SUPER-held clicks behave identically.
void CScrollOverview::performPrimaryClickAction() {
    // E6: the strip band floats above the main tiles, so it has to be
    // tested before them, exactly as the drop path does.
    //
    // E8-b: clicking a desktop -- on the strip or adjacent on screen --
    // switches to it AND exits. focusOverviewWorkspace changes the workspace,
    // so the viewport slide starts before the close animation takes over.
    lastMousePosLocal = getOverviewMousePosLocal(pMonitor.lock());
    size_t stripIdx   = viewportCurrentWorkspace;

    // The detached scratchpad cell, tested before the strip: workspaceAtStripPoint
    // cannot answer for it (which is exactly what keeps a swipe off it), so a
    // click would otherwise fall through to the tile behind. Clicking it pulls
    // the scratchpad up and leaves, the same as clicking a desktop card does.
    if (scratchpadCellContains(lastMousePosLocal)) {
        // ensureDefault, not scratchpadCellWorkspace: with no scratchpad yet
        // (or none left, once its last window moved out) there is nothing to
        // show, and the click has to be what brings it into being.
        ScrollOverview::Scratchpad::show(ScrollOverview::Scratchpad::ensureDefault(pMonitor.lock()));
        close();
        return;
    }

    // Another monitor's desktop: that monitor switches to it and focus goes
    // there with it -- the desktop just picked is where the user is going. Its
    // last focused window when it has one, through that monitor's own overview
    // (selectAndClose closes every overview, this one included); a desktop with
    // nothing to focus switches and closes with that overview closing LAST, so
    // it is the one whose close decides where focus lands.
    if (const auto HIT = altCellAt(lastMousePosLocal)) {
        const auto LAST = getOverviewWindowToShow(HIT->workspace->getLastFocusedWindow());
        if (LAST && LAST->m_workspace == HIT->workspace && shouldShowOverviewWindow(LAST) && HIT->overview->selectAndClose(LAST))
            return;

        HIT->overview->focusOverviewWorkspace(HIT->workspace, HIT->workspaceIdx);
        // focus_follows_pick: HIT's own close() takes the others down first,
        // so it keeps the final word on focus.
        if (!pickKeepsFocus())
            for (const auto& overview : scrollOverviews()) {
                if (overview && overview.get() != HIT->overview)
                    overview->close();
            }
        HIT->overview->close();
        return;
    }

    if (const auto STRIPWORKSPACE = workspaceAtStripPoint(lastMousePosLocal, &stripIdx); STRIPWORKSPACE) {
        focusOverviewWorkspace(STRIPWORKSPACE, stripIdx);
        close();
        return;
    }

    selectHoveredWorkspace();
    g_search.nav.zone = EOverviewNavZone::Tile;
    selectWindowAtOverviewCursor(true);
    close();
}

void CScrollOverview::selectHoveredWorkspace() {
    lastMousePosLocal = getOverviewMousePosLocal(pMonitor.lock());

    // workspaceAtOverviewCursor writes workspaceIdx; reading it in the same call
    // expression is unsequenced, and the stale index reached close() as the
    // "selected" workspace and undid the switch (X15).
    size_t     workspaceIdx = viewportCurrentWorkspace;
    const auto HOVEREDWORKSPACE = workspaceAtOverviewCursor(&workspaceIdx);
    focusOverviewWorkspace(HOVEREDWORKSPACE, workspaceIdx);
}

bool CScrollOverview::windowDispatcherAction(const std::string& action) {
    const auto CURRENTKEYBIND = g_pKeybindManager ? g_pKeybindManager->m_currentKeybind : SP<SKeybind>{};
    const bool FROMMOUSEBIND  = CURRENTKEYBIND && CURRENTKEYBIND->key.starts_with("mouse:");

    PHLWINDOW WINDOW;
    size_t    workspaceIdx = viewportCurrentWorkspace;

    if (FROMMOUSEBIND) {
        lastMousePosLocal = getOverviewMousePosLocal(pMonitor.lock());
        WINDOW            = windowAtOverviewCursor(&workspaceIdx);
    } else
        WINDOW = getOverviewWindowToShow(closeOnWindow.lock());

    if (!WINDOW)
        return false;

    if (action == "select")
        return selectOverviewWindow(WINDOW, workspaceIdx, true);

    if (action == "close") {
        WINDOW->sendClose();
        damage();
        return true;
    }

    return false;
}
