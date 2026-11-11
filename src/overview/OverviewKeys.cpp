// COverview: keyboard navigation.
// Cut from Overview.cpp by pure code motion.
#include "OverviewParts.hpp"

using namespace OverviewDetail;


// X61. ONE selection, moved by what is actually DRAWN ON SCREEN.
//
// What was here before was upstream's SCROLLING-LAYOUT walk, and it could not
// see this product at all: it skipped every candidate with `m_isFloating` and
// navigated by the window's REAL `middle()`. In this rice every window floats
// and the tiles are laid out in a mission-control GRID with no relation to real
// geometry, so the arrow keys were dead by construction -- and would have
// pointed the wrong way even if they had not been.
//
// Everything the user can see is collected as a box: strip cells, result rows
// and grid cells, window tiles. An arrow then picks the nearest box in that
// direction. Crossing from one surface to the next costs nothing because it is
// the same geometry either way, which is what "arrow through everything" means.
struct SOverviewNavTarget {
    CBox             box;
    EOverviewNavZone zone;
    int              index = -1; // Desktop: workspace index. Result: result index.
    PHLWINDOW        window;     // Tile
};

// Everything on screen an arrow can land on, in one list of boxes. Reads
// state; changes none.
void COverview::collectNavTargets(PHLMONITOR MONITOR, std::vector<SOverviewNavTarget>& targets) const {
    for (size_t i = 0; i < images.size(); ++i) {
        if (!images[i] || !images[i]->pWorkspace)
            continue;

        if (const CBox BOX = stripCellBox(i, MONITOR, scale->value()); BOX.width > 1.F && BOX.height > 1.F)
            targets.push_back({BOX, EOverviewNavZone::Desktop, sc<int>(i), {}});
    }

    // The results as the last frame DREW them -- the same boxes the mouse
    // hit-tests against, so the two devices cannot disagree about where a row
    // is. G5's grid cells and the list rows are both in here already.
    for (const auto& HIT : searchLayoutHere().hits)
        targets.push_back({HIT.box, EOverviewNavZone::Result, HIT.index, {}});

    // Every workspace's tiles, filtered to the ones actually ON the monitor.
    // Asking only images[viewportCurrentWorkspace] made the tiles unreachable:
    // that index is the VIEWPORT's, and a tile's box already carries its
    // workspace offset, so the honest test for "can the user see this" is
    // whether the drawn box lands on the monitor -- the same test the render
    // pass uses. Measured 2026-08-05: with the current-workspace filter, arrows
    // produced a byte-identical frame on a desktop with two visible tiles.
    //
    // ONCE PER WINDOW. With a query typed every workspace's main-grid list also
    // carries the promoted matches (B2), so the same window came back once per
    // desktop -- harmless to the pick, but it is the list the Tab entry and the
    // phantom-selection check below read, and those must see each tile once.
    std::unordered_set<const Desktop::View::CWindow*> seen;
    for (const auto& IMAGE : images) {
        if (!IMAGE || !IMAGE->pWorkspace)
            continue;

        for (const auto& WINDOW : overviewMainGridWindows(IMAGE->pWorkspace, MONITOR)) {
            if (!WINDOW || !seen.insert(WINDOW.get()).second)
                continue;
            const CBox BOX = tileBox(WINDOW);
            if (BOX.width > 1.F && BOX.height > 1.F && overviewBoxIntersectsMonitor(BOX, MONITOR))
                targets.push_back({BOX, EOverviewNavZone::Tile, -1, WINDOW});
        }
    }

}

// The tile keyboard selection starts on: top row first, then leftmost -- the
// order a filtered grid reads in. Rows are centred, so "same row" is a shared
// vertical band rather than an equal y.
PHLWINDOW COverview::firstNavTile() const {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR)
        return nullptr;

    std::vector<SOverviewNavTarget> targets;
    collectNavTargets(MONITOR, targets);

    const SOverviewNavTarget* best = nullptr;
    for (const auto& TARGET : targets) {
        if (TARGET.zone != EOverviewNavZone::Tile)
            continue;
        if (!best) {
            best = &TARGET;
            continue;
        }
        const bool SAMEROW = TARGET.box.y < best->box.y + best->box.height && best->box.y < TARGET.box.y + TARGET.box.height;
        if (SAMEROW ? TARGET.box.middle().x < best->box.middle().x : TARGET.box.middle().y < best->box.middle().y)
            best = &TARGET;
    }

    return best ? best->window : nullptr;
}

// firstNavTile's ordering, generalised to the whole desktop. Rows are centred,
// so "same row" is a shared vertical band rather than an equal y.
std::vector<PHLWINDOW> COverview::navTilesOf(const PHLWORKSPACE& workspace) const {
    std::vector<PHLWINDOW> out;
    const auto             MONITOR = pMonitor.lock();
    if (!workspace || !MONITOR)
        return out;

    struct SOrdered {
        PHLWINDOW window;
        CBox      box;
    };
    std::vector<SOrdered> ordered;

    for (const auto& WINDOW : overviewMainGridWindows(workspace, MONITOR)) {
        if (!WINDOW)
            continue;
        // The SLOT, not the drawn box: the drawn box carries the viewport pan,
        // and the desktop being walked ONTO has not been panned to yet.
        CBox slot;
        if (!overviewGridLayout(workspace, MONITOR, WINDOW, slot) || slot.width <= 1.F || slot.height <= 1.F)
            continue;
        ordered.push_back({WINDOW, slot});
    }

    std::ranges::sort(ordered, [](const SOrdered& a, const SOrdered& b) {
        const bool SAMEROW = a.box.y < b.box.y + b.box.height && b.box.y < a.box.y + a.box.height;
        return SAMEROW ? a.box.middle().x < b.box.middle().x : a.box.middle().y < b.box.middle().y;
    });

    out.reserve(ordered.size());
    for (const auto& O : ordered)
        out.push_back(O.window);
    return out;
}

// Off the end of one desktop's grid and onto the next (user, 2026-09-23). Only
// reached with no query: a filtered grid already holds the matches from every
// desktop, so "the next desktop" is not a place its walk can go.
bool COverview::stepTileDesktop(int delta) {
    if (delta == 0 || images.empty())
        return false;

    const int CUR  = sc<int>(viewportCurrentWorkspace);
    const int NEXT = CUR + delta;
    if (NEXT < 0 || NEXT >= sc<int>(images.size()))
        return false;

    const auto IMAGE = images[sc<size_t>(NEXT)];
    if (!IMAGE || !IMAGE->pWorkspace)
        return false;

    const auto TILES = navTilesOf(IMAGE->pWorkspace);

    focusOverviewWorkspace(IMAGE->pWorkspace, sc<size_t>(NEXT));
    popKeyboardBadge(sc<size_t>(NEXT));

    // Right lands on that desktop's first tile, left on its last, so the walk
    // reads as one continuous line across the desktops rather than jumping to
    // the same corner every time. An empty desktop is still a valid stop: the
    // selection simply has nothing to sit on until the next arrow.
    if (!TILES.empty()) {
        const auto TARGET         = delta > 0 ? TILES.front() : TILES.back();
        g_search.nav.zone         = EOverviewNavZone::Tile;
        g_search.nav.selection    = -1;
        g_search.nav.keyboardTile = true;
        closeOnWindow             = TARGET;
        TileChrome::setHovered(TARGET, false);
    } else {
        // An EMPTY desktop has nothing to select, and the tile left behind on
        // the previous one must not stay selected: close() commits
        // closeOnWindow, so it carried the user straight back to the desktop
        // they had just walked off (measured: arrows to empty ws 1, Escape,
        // landed on ws 2). With no window selected, close() takes its
        // no-selection path and stays on the desktop in view.
        closeOnWindow.reset();
        g_search.nav.keyboardTile = false;
        TileChrome::setHovered(PHLWINDOW{}, false);
    }

    damage();
    return true;
}

// TAB ACROSS MONITORS (user, 2026-09-27): "search -> window grid on monitor 1 ->
// window grid on monitor 2 -> search". Only with more than one overview up;
// false leaves the single-monitor cycle (results -> windows -> desktops) alone.
// Monitors run left to right (then top to bottom); a monitor with no tiles is
// passed over, and the search is the first monitor's.
bool COverview::tabAcrossMonitors() {
    std::vector<std::pair<COverview*, SP<IOverview>>> order;
    for (const auto& OV : scrollOverviews()) {
        auto* S = dynamic_cast<COverview*>(OV.get());
        if (S && !S->closing && S->pMonitor.lock())
            order.emplace_back(S, OV);
    }
    if (order.size() < 2)
        return false;
    std::ranges::sort(order, [](const auto& a, const auto& b) {
        const auto A = a.first->pMonitor->m_position, B = b.first->pMonitor->m_position;
        return A.x != B.x ? A.x < B.x : A.y < B.y;
    });
    size_t self = 0;
    for (size_t i = 0; i < order.size(); ++i)
        if (order[i].first == this)
            self = i;

    const auto HANDOVER = [](const std::pair<COverview*, SP<IOverview>>& ov) {
        // Focus first: a monitor focus change can move the cursor, and the
        // owner remembers the pointer AFTER it, or that move reads as the user
        // reaching away and hands the keyboard straight back.
        if (const auto MON = ov.first->pMonitor.lock(); MON && MON != Desktop::focusState()->monitor())
            Desktop::focusState()->rawMonitorFocus(MON);
        setKeyboardOverview(ov.second);
    };
    const auto ENTERTILES = [&](const std::pair<COverview*, SP<IOverview>>& ov) {
        auto*      OV    = ov.first;
        const auto FIRST = OV->isNavTile(OV->closeOnWindow.lock()) ? OV->closeOnWindow.lock() : OV->firstNavTile();
        Hyprdrive::Stacks::traceEvent(std::format("tab: try monitor {} closeOn={} navTile={} first={}", OV->pMonitor->m_name, (uintptr_t)OV->closeOnWindow.lock().get(),
                                                       OV->isNavTile(OV->closeOnWindow.lock()) ? 1 : 0, (uintptr_t)OV->firstNavTile().get()));
        if (!FIRST)
            return false;
        OV->closeOnWindow              = FIRST;
        g_search.nav.zone              = EOverviewNavZone::Tile;
        g_search.nav.selection         = -1;
        g_search.nav.keyboardTile      = true;
        g_search.nav.explicitSelection = true;
        TileChrome::setHovered(FIRST, false);
        HANDOVER(ov);
        return true;
    };
    const auto ENTERSEARCH = [&]() {
        auto&      FIRST   = order.front();
        const auto RESULTS = overviewSearchRows();
        g_search.nav.keyboardTile = false;
        TileChrome::setHovered(PHLWINDOW{}, false);
        if (overviewSearchPaneUp() && !RESULTS.empty()) {
            g_search.nav.zone              = EOverviewNavZone::Result;
            g_search.nav.explicitSelection = true;
            g_search.nav.intent            = ESearchIntent::Browsing;
            if (g_search.nav.selection < 0 || g_search.nav.selection >= sc<int>(RESULTS.size()))
                g_search.nav.selection = 0;
        } else {
            g_search.nav.zone      = EOverviewNavZone::Search;
            g_search.nav.selection = -1;
            g_search.nav.intent    = ESearchIntent::Typing;
        }
        HANDOVER(FIRST);
    };

    const bool IN_GRID = g_search.nav.zone == EOverviewNavZone::Tile || g_search.nav.zone == EOverviewNavZone::Desktop;
    Hyprdrive::Stacks::traceEvent(std::format("tab: from monitor {} zone={} self={}", pMonitor->m_name, (int)g_search.nav.zone, self));

    // NO RESULTS UP (user, 2026-09-27): "monitor 1 strip -> grid -> monitor 2
    // strip -> grid monitor 2" and round again -- search has nothing to offer,
    // so it is not a stop. Every strip is a stop; a grid with no tiles is not.
    if (const auto RESULTS = overviewSearchRows(); !(overviewSearchPaneUp() && !RESULTS.empty())) {
        const size_t STOPS = order.size() * 2;
        const size_t FROM  = g_search.nav.zone == EOverviewNavZone::Desktop ? self * 2 : g_search.nav.zone == EOverviewNavZone::Tile ? self * 2 + 1 : STOPS - 1;
        for (size_t step = 1; step <= STOPS; ++step) {
            const size_t STOP = (FROM + step) % STOPS;
            auto&        ov   = order[STOP / 2];
            if (STOP % 2 == 1) {
                if (ENTERTILES(ov))
                    break;
                continue;
            }
            g_search.nav.zone              = EOverviewNavZone::Desktop;
            g_search.nav.selection         = -1;
            g_search.nav.keyboardTile      = false;
            g_search.nav.explicitSelection = true;
            TileChrome::setHovered(PHLWINDOW{}, false);
            HANDOVER(ov);
            break;
        }
        for (auto& [OV, SP] : order)
            OV->damage();
        return true;
    }
    for (size_t i = IN_GRID ? self + 1 : 0; i < order.size(); ++i)
        if (ENTERTILES(order[i])) {
            for (auto& [OV, SP] : order)
                OV->damage();
            return true;
        }
    ENTERSEARCH();
    for (auto& [OV, SP] : order)
        OV->damage();
    return true;
}

bool COverview::isNavTile(const PHLWINDOW& window) const {
    const auto MONITOR = pMonitor.lock();
    if (!window || !MONITOR)
        return false;

    std::vector<SOverviewNavTarget> targets;
    collectNavTargets(MONITOR, targets);
    return std::ranges::any_of(targets, [&](const auto& TARGET) { return TARGET.zone == EOverviewNavZone::Tile && TARGET.window == window; });
}

// Where the selection is NOW. Empty when the zone has nothing selected in it,
// which is what the caller's "land somewhere" fallback keys off.
CBox COverview::currentNavBox(const std::vector<SOverviewNavTarget>& targets, PHLMONITOR MONITOR) const {
    CBox current;
    switch (g_search.nav.zone) {
        case EOverviewNavZone::Search:
            current = searchLayoutHere().pill;
            break;
        case EOverviewNavZone::Result:
            for (const auto& TARGET : targets)
                if (TARGET.zone == EOverviewNavZone::Result && TARGET.index == g_search.nav.selection)
                    current = TARGET.box;
            break;
        case EOverviewNavZone::Tile:
            if (const auto SELECTED = closeOnWindow.lock(); SELECTED)
                current = tileBox(SELECTED);
            break;
        case EOverviewNavZone::Desktop: current = stripCellBox(viewportCurrentWorkspace, MONITOR, scale->value()); break;
    }
    return current;
}

// The pick, and it is a pure function of the boxes: no compositor state, no
// members, so the walk can be reasoned about (and one day tabled) on its own.
static const SOverviewNavTarget* pickNavTarget(const std::vector<SOverviewNavTarget>& targets, const CBox& current, EOverviewNavZone zone, bool LEFT, bool RIGHT,
                                               bool DOWN) {
    const Vector2D CUR = current.middle();

    // Ordering, in this order and for this reason:
    //
    //   1. a candidate the selection OVERLAPS on the perpendicular axis beats
    //      one it does not. Scoring by weighted distance instead sent UP from a
    //      tile PAST the strip and PAST the result list to a grid cell at the
    //      very top, because that cell happened to be column-aligned with the
    //      tile and alignment was worth more than being 300px closer.
    //      Measured 2026-08-05: one UP press jumped from the tiles to the last
    //      apps cell, skipping both surfaces in between.
    //   2. then nearest in the direction of travel.
    //   3. then nearest sideways, which only ever breaks a tie.
    //
    // Same shape as upstream's own bestHasOverlap/bestPrimaryDistance walk --
    // the geometry it applied that to was simply the wrong geometry.
    struct SScore {
        bool  noOverlap = true;
        float primary   = 0.F;
        float secondary = 0.F;

        bool beats(const SScore& other) const {
            if (noOverlap != other.noOverlap)
                return !noOverlap;
            if (primary != other.primary)
                return primary < other.primary;
            return secondary < other.secondary;
        }
    };

    const SOverviewNavTarget* best = nullptr;
    SScore                    bestScore;
    const SOverviewNavTarget* near = nullptr;
    SScore                    nearScore;

    for (const auto& TARGET : targets) {
        const Vector2D CENTER  = TARGET.box.middle();
        const float    PRIMARY = RIGHT ? CENTER.x - CUR.x : LEFT ? CUR.x - CENTER.x : DOWN ? CENTER.y - CUR.y : CUR.y - CENTER.y;

        // Strictly in the direction asked for, so an arrow can never pick the
        // box it is already on or one behind it.
        if (PRIMARY <= 1.F)
            continue;

        const SScore SCORE{
            .noOverlap = !(LEFT || RIGHT ? TARGET.box.y < current.y + current.height && current.y < TARGET.box.y + TARGET.box.height :
                                           TARGET.box.x < current.x + current.width && current.x < TARGET.box.x + TARGET.box.width),
            .primary   = PRIMARY,
            .secondary = (LEFT || RIGHT) ? std::abs(CENTER.y - CUR.y) : std::abs(CENTER.x - CUR.x),
        };

        if (!best || SCORE.beats(bestScore)) {
            bestScore = SCORE;
            best      = &TARGET;
        }

        // An arrow LEAVES a surface only when that surface has nowhere left to
        // go in that direction. Without this, RIGHT on a full-width result row
        // -- whose centre is the overview's centre, so half the tiles are "to
        // the right" of it -- jumped straight out of the list and into the grid.
        // Crossing between surfaces is still the point of this walk, it just has
        // to be the LAST resort rather than the first.
        if (TARGET.zone == zone && (!near || SCORE.beats(nearScore))) {
            nearScore = SCORE;
            near      = &TARGET;
        }
    }

    return near ? near : best;
}

bool COverview::moveSelection(const std::string& direction) {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR)
        return false;

    const bool LEFT = direction == "left", RIGHT = direction == "right", UP = direction == "up", DOWN = direction == "down";
    if (!LEFT && !RIGHT && !UP && !DOWN)
        return false;

    // 2D Navigation inside the Results zone
    if (g_search.nav.zone == EOverviewNavZone::Result && !searchLayoutHere().hits.empty()) {
        const SSearchRowHit* curHit = nullptr;
        for (const auto& hit : searchLayoutHere().hits) {
            if (hit.index == g_search.nav.selection) {
                curHit = &hit;
                break;
            }
        }

        if (curHit) {
            g_search.nav.explicitSelection = true;
            g_search.nav.intent            = ESearchIntent::Browsing;

            if (UP) {
                if (curHit->rowInCol == 0) {
                    // Return focus to search bar
                    g_search.nav.zone              = EOverviewNavZone::Search;
                    g_search.nav.selection         = -1;
                    g_search.nav.explicitSelection = false;
                    g_search.nav.intent            = ESearchIntent::Typing;
                    damage();
                    return true;
                }
                for (const auto& hit : searchLayoutHere().hits) {
                    if (hit.column == curHit->column && hit.rowInCol == curHit->rowInCol - 1) {
                        g_search.nav.selection = hit.index;
                        damage();
                        return true;
                    }
                }
                return true;
            }

            if (DOWN) {
                for (const auto& hit : searchLayoutHere().hits) {
                    if (hit.column == curHit->column && hit.rowInCol == curHit->rowInCol + 1) {
                        g_search.nav.selection = hit.index;
                        damage();
                        return true;
                    }
                }
                return true;
            }

            if (RIGHT) {
                int maxCol = 0;
                for (const auto& hit : searchLayoutHere().hits)
                    maxCol = std::max(maxCol, hit.column);

                for (int targetCol = curHit->column + 1; targetCol <= maxCol; ++targetCol) {
                    const SSearchRowHit* bestHit = nullptr;
                    int bestDiff = 999;
                    for (const auto& hit : searchLayoutHere().hits) {
                        if (hit.column == targetCol) {
                            int diff = std::abs(hit.rowInCol - curHit->rowInCol);
                            if (diff < bestDiff) {
                                bestDiff = diff;
                                bestHit  = &hit;
                            }
                        }
                    }
                    if (bestHit) {
                        g_search.nav.selection = bestHit->index;
                        damage();
                        return true;
                    }
                }
                return true;
            }

            if (LEFT) {
                if (curHit->column == 0) {
                    if (SearchBackend::canNavigateMenuBack()) {
                        SearchBackend::navigateMenuBack();
                        g_search.nav.selection = 0;
                        damage();
                        return true;
                    }
                    return true;
                }

                for (int targetCol = curHit->column - 1; targetCol >= 0; --targetCol) {
                    const SSearchRowHit* bestHit = nullptr;
                    int bestDiff = 999;
                    for (const auto& hit : searchLayoutHere().hits) {
                        if (hit.column == targetCol) {
                            int diff = std::abs(hit.rowInCol - curHit->rowInCol);
                            if (diff < bestDiff) {
                                bestDiff = diff;
                                bestHit  = &hit;
                            }
                        }
                    }
                    if (bestHit) {
                        g_search.nav.selection = bestHit->index;
                        damage();
                        return true;
                    }
                }
                return true;
            }
        }
    }

    // Down drops into the results from the SEARCH FIELD only. `|| zone !=
    // Result` also matched the window grid and the strip, so with a query up
    // every Down in the grid jumped back to the results instead of walking to
    // the next row of tiles (user, 2026-09-29).
    if (g_search.nav.zone == EOverviewNavZone::Search && DOWN) {
        if (!searchLayoutHere().hits.empty()) {
            g_search.nav.zone              = EOverviewNavZone::Result;
            g_search.nav.selection         = searchLayoutHere().hits[0].index;
            g_search.nav.explicitSelection = true;
            g_search.nav.intent            = ESearchIntent::Browsing;
            damage();
            return true;
        }
    }

    // A selected tile the filter has since hidden is not a place to walk FROM:
    // its box is where it would be unfiltered, so the first arrow jumped over
    // whatever lay between that phantom and the direction pressed, and Enter
    // would have picked a window that is not on screen. Land on the first
    // visible tile instead; the NEXT arrow walks from there.
    if (g_search.nav.zone == EOverviewNavZone::Tile && !isNavTile(closeOnWindow.lock())) {
        if (const auto FIRST = firstNavTile()) {
            closeOnWindow             = FIRST;
            g_search.nav.keyboardTile = true;
            TileChrome::setHovered(FIRST, false);
            damage();
            return true;
        }
    }

    std::vector<SOverviewNavTarget> targets;
    collectNavTargets(MONITOR, targets);

    if (targets.empty())
        return false;

    CBox current = currentNavBox(targets, MONITOR);

    if (current.width <= 1.F || current.height <= 1.F)
        current = targets.front().box;

    // NAVIGATING WINDOWS STAYS IN THE WINDOWS (user, 2026-09-23: "it can also be
    // spatial but restricted to grid when I'm navigating windows"). The pick is
    // still geometric -- it is what makes a two-row grid walk the way it looks
    // -- but the strip and the results are no longer things an arrow can fall
    // onto by being nearer than the next tile. Tab is the way between the
    // three panes now, which is the whole point of the cycle.
    //
    // ...and in the windows of THIS desktop's card only (user, 2026-09-23: "if my
    // right arrow selects a window in the next workspace then we should be moved
    // to it"). The neighbouring cards peek in at the monitor's edges, so their
    // tiles are on screen and were valid spatial targets: right off the end of
    // the grid selected the nearest tile on the peeking card and left the view
    // where it was, and the step below -- which moves the view -- never ran
    // because a target had been found. Confining the pick to this card is what
    // routes "off the end" through stepTileDesktop, which changes desktop and
    // lands on that desktop's first tile (or its last, going left).
    if (g_search.nav.zone == EOverviewNavZone::Tile) {
        std::unordered_set<const Desktop::View::CWindow*> here;
        if (viewportCurrentWorkspace < images.size() && images[viewportCurrentWorkspace] && images[viewportCurrentWorkspace]->pWorkspace)
            for (const auto& W : overviewMainGridWindows(images[viewportCurrentWorkspace]->pWorkspace, MONITOR))
                if (W)
                    here.insert(W.get());
        std::erase_if(targets, [&](const SOverviewNavTarget& t) { return t.zone != EOverviewNavZone::Tile || !here.contains(t.window.get()); });
    }

    const auto* const best = pickNavTarget(targets, current, g_search.nav.zone, LEFT, RIGHT, DOWN);
    if (!best) {
        // Off the end of the grid. With no query the desktops are a line the
        // walk continues along; with one, the grid already IS every match and
        // there is nowhere further to go, so the selection stops.
        if (g_search.nav.zone == EOverviewNavZone::Tile && (LEFT || RIGHT) && g_search.query.text.empty())
            return stepTileDesktop(LEFT ? -1 : 1);
        return false;
    }

    switch (best->zone) {
        case EOverviewNavZone::Desktop:
            g_search.nav.zone      = EOverviewNavZone::Desktop;
            g_search.nav.selection = -1;
            g_search.nav.keyboardTile = false;
            TileChrome::setHovered(PHLWINDOW{}, false);
            if (const auto WORKSPACE = images[sc<size_t>(best->index)]->pWorkspace; WORKSPACE) {
                focusOverviewWorkspace(WORKSPACE, sc<size_t>(best->index));
                popKeyboardBadge(sc<size_t>(best->index));
            }
            break;
        case EOverviewNavZone::Result:
            g_search.nav.zone              = EOverviewNavZone::Result;
            g_search.nav.selection         = best->index;
            g_search.nav.explicitSelection = true;
            g_search.nav.intent            = ESearchIntent::Browsing;
            g_search.nav.keyboardTile      = false;
            TileChrome::setHovered(PHLWINDOW{}, false);
            break;
        case EOverviewNavZone::Tile:
            g_search.nav.zone         = EOverviewNavZone::Tile;
            g_search.nav.selection    = -1;
            closeOnWindow             = best->window;
            g_search.nav.keyboardTile = true;
            TileChrome::setHovered(best->window, false);
            break;
        case EOverviewNavZone::Search:
            g_search.nav.zone              = EOverviewNavZone::Search;
            g_search.nav.selection         = -1;
            g_search.nav.explicitSelection = false;
            g_search.nav.intent            = ESearchIntent::Typing;
            g_search.nav.keyboardTile      = false;
            TileChrome::setHovered(PHLWINDOW{}, false);
            break;
    }

    damage();
    return true;
}
