#include "GridLayout.hpp"
#include "OverviewGeometry.hpp"
#include "OverviewInternal.hpp"

#include <algorithm>
#include <ranges>

// Every body here came out of OverviewInternal.hpp unchanged except for the two
// mechanical substitutions the move forced: the four globals are now members of
// the one SGridAnimator, and the pure arithmetic (lerp, open-progress ramp,
// half-pixel comparison) forwards to OverviewGeometry. Nothing else moved, so a
// tile that lands somewhere new is a transcription slip and not a redesign.

namespace OvInternal {

    namespace {
        // The two reads overviewGridPromotable used to make per window.
        struct SPromoteCtx {
            bool         enabled = false; // B2 is live: a query is typed AND mission control is on
            PHLWORKSPACE active;
        };
    }

    SGridAnimator& grid() {
        static SGridAnimator ANIMATOR;
        return ANIMATOR;
    }

    void gridSetReflowActive(const PHLMONITOR& monitor) {
        if (monitor)
            grid().reflowActive.insert(monitor->m_id);
    }

    bool gridTakeReflowActive(const PHLMONITOR& monitor) {
        if (!monitor)
            return false;

        return grid().reflowActive.erase(monitor->m_id) > 0;
    }

    // Y9 taught overviewGridWindows and addOverviewWindow that a stashed window
    // belongs to the desktop it was minimized FROM. Two decisions kept reading
    // the raw m_workspace instead, and a stashed window's m_workspace is the
    // stash -- so it could never equal the active workspace, and a window
    // minimized FROM the active desktop answered "I live somewhere else". It
    // then took a grid slot twice: once as a home member, once as an
    // off-desktop match joining the active grid under B2. Measured with the
    // render-list shadow: 4145 RENDER-GRIDDUP lines in one soak, every one of
    // them a stashed window against its own home card. The visible cost is the
    // layout solving for N+1 -- two windows laid out as three, so the tiles sit
    // in row 0 of a two-row grid with an empty row underneath, which is the
    // user's "not centered or organized properly".
    WORKSPACEID overviewGridHomeWorkspace(const PHLWINDOW& window) {
        if (!window)
            return WORKSPACE_INVALID;

        if (const auto HOME = ScrollOverview::Minimize::originalWorkspace(window); HOME != WORKSPACE_INVALID)
            return HOME;

        return window->m_workspace ? window->m_workspace->m_id : WORKSPACE_INVALID;
    }

    // The home-workspace membership rule, into a buffer the caller owns so the
    // per-frame layout memo can rebuild its key without allocating.
    static void overviewGridWindowsInto(const PHLWORKSPACE& workspace, std::vector<PHLWINDOW>& out) {
        out.clear();
        if (!workspace)
            return;

        for (const auto& window : Desktop::windowState()->windows()) {
            if (getOverviewWindowToShow(window) != window || !shouldShowOverviewWindow(window))
                continue;

            // Y9 (user): "when an app window is minimized it should still be
            // visible there, so I can see it and filter to it". A minimized window
            // is parked on the `special:minimized` stash, so its own m_workspace can
            // never equal a real one and the plain test below dropped it from every
            // grid. It belongs to the desktop it was minimized FROM, which is
            // exactly what Minimize records -- and the only source for it, since the
            // stash has overwritten the window's own answer.
            //
            // This also subsumes X58(a), which asked only that the SEARCH reach
            // minimized windows: they are in the grid now whether a query is typed
            // or not, and the query filters them like anything else.
            if (const auto HOME = ScrollOverview::Minimize::originalWorkspace(window); HOME != WORKSPACE_INVALID) {
                if (HOME != workspace->m_id)
                    continue;
            } else if (window->m_workspace != workspace)
                continue;

            out.emplace_back(window);
        }
    }

    std::vector<PHLWINDOW> overviewGridWindows(const PHLWORKSPACE& workspace) {
        std::vector<PHLWINDOW> windows;
        overviewGridWindowsInto(workspace, windows);
        return windows;
    }

    // OVERVIEW_CPP_SPEC section 5 B2. With a query typed, matches from every
    // workspace join the ACTIVE workspace's grid. A promoted window keeps living
    // in its own workspace's window list and is still drawn in that workspace's
    // pass; what changes is that its box comes from the active grid and is placed
    // at the active card's offset. Done here rather than by shuffling windows
    // between the per-workspace loops so the B1 invariant holds: render, hit-test
    // and drag all still read the same getOverviewWindowBox and cannot disagree.
    // The two answers that are the SAME for every window in one query -- is B2
    // live at all, and which workspace is the active one -- read once and
    // carried. overviewMainGridWindows asked them per window, so a
    // getMissionControl() config lookup (a string-keyed map probe, with the key
    // built fresh each time) ran W times per grid query and the grid is queried
    // 4-6 times per tile per frame.
    static SPromoteCtx overviewGridPromoteCtx(PHLMONITOR monitor) {
        SPromoteCtx ctx;
        if (!monitor)
            return ctx;

        ctx.active  = monitor->m_activeWorkspace;
        ctx.enabled = !g_search.query.text.empty() && overviewMissionControlOn() && !!ctx.active;
        return ctx;
    }

    // Identical to the published predicate below, minus the two hoisted reads.
    static bool overviewGridPromotableIn(const SPromoteCtx& ctx, const PHLWINDOW& window, PHLMONITOR monitor) {
        if (!ctx.enabled || !window || !monitor)
            return false;

        if (window->m_monitor != monitor)
            return false;

        const auto HOME = overviewGridHomeWorkspace(window);
        return HOME != WORKSPACE_INVALID && HOME != ctx.active->m_id && shouldShowOverviewWindow(window);
    }

    bool overviewGridPromotable(const PHLWINDOW& window, PHLMONITOR monitor) {
        return overviewGridPromotableIn(overviewGridPromoteCtx(monitor), window, monitor);
    }

    int gridReleaseMs() {
        return sc<int>(sc<float>(overviewMissionControlReflowMs()) * GRID_RELEASE_SPEEDUP);
    }

    // OVERVIEW_CPP_SPEC 13.2 F3-b, the symmetric half: "released back OUT to its
    // own desktop as the query stops matching". The instant a promoted window
    // stops matching it leaves the grid, so gridReflowBox is never called for it
    // and it cuts -- the exact mirror of the pull-in defect. Keep it promoted for
    // one release after the match dies so it still has somewhere to travel FROM.
    bool overviewGridReleasing(const PHLWINDOW& window, PHLMONITOR monitor) {
        const int DURATION = gridReleaseMs();
        if (DURATION <= 0 || !window)
            return false;

        const auto NOW = Time::steadyNow();
        auto&      G   = grid();

        if (overviewGridPromotable(window, monitor)) {
            if (G.releases.size() > 64)
                std::erase_if(G.releases, [](const auto& entry) { return !entry.second.window.lock(); });

            G.releases[window.get()] = SGridRelease{window, {}, false}; // armed, not running
            return false;
        }

        const auto IT = G.releases.find(window.get());
        if (IT == G.releases.end())
            return false;

        // A recycled address must not inherit a dead window's release, and a window
        // that unmapped mid-flight must stop being drawn -- the render gate below
        // trusts this function to have checked.
        if (IT->second.window.lock() != window || !validMapped(window)) {
            G.releases.erase(IT);
            return false;
        }

        if (!IT->second.started) {
            IT->second.started = true;
            IT->second.start   = NOW; // the match died on THIS frame, whenever it is
        }

        if (overviewAnimElapsedMs(IT->second.start) > sc<float>(DURATION) + GRID_RELEASE_GRACE_MS) {
            G.releases.erase(IT);
            return false;
        }

        return true;
    }

    bool overviewGridPromoted(const PHLWINDOW& window, PHLMONITOR monitor) {
        return overviewGridPromotable(window, monitor) || overviewGridReleasing(window, monitor);
    }

    // Membership of the MAIN grid. Identical to overviewGridWindows for an empty
    // query. The strip deliberately keeps the workspace-scoped list: its minis and
    // its "no matches" count both need the per-cell answer.
    // The membership walk, into a buffer the caller owns, so the per-frame memo
    // below can rebuild its key without allocating. `out` is cleared, never
    // shrunk: the same two buffers are reused every call.
    static void overviewMainGridWindowsInto(const PHLWORKSPACE& workspace, PHLMONITOR monitor, std::vector<PHLWINDOW>& out) {
        overviewGridWindowsInto(workspace, out);

        const auto CTX = overviewGridPromoteCtx(monitor);
        if (!CTX.enabled)
            return;

        for (const auto& window : Desktop::windowState()->windows()) {
            // Promotable, not promoted: a RELEASING tile must leave the grid
            // immediately so the survivors close up WHILE it travels (F3-b-iii,
            // outbound). It keeps being drawn -- it just no longer owns a slot.
            if (getOverviewWindowToShow(window) == window && overviewGridPromotableIn(CTX, window, monitor))
                out.emplace_back(window);
        }
    }

    std::vector<PHLWINDOW> overviewMainGridWindows(const PHLWORKSPACE& workspace, PHLMONITOR monitor) {
        std::vector<PHLWINDOW> windows;
        overviewMainGridWindowsInto(workspace, monitor, windows);
        return windows;
    }

    Vector2D overviewGridWindowSize(const PHLWINDOW& window) {
        return window->geometricBox(Desktop::View::IGeometric::GEOMETRIC_CURRENT).size();
    }

    // THE SOLVER, and it is now solved ONCE for the whole grid instead of once
    // per tile. Everything below the membership walk -- the row-count search,
    // the per-row heights, the centring -- used to run in full for EVERY box
    // query, and a frame asks for a tile's box 4-6 times per tile per monitor
    // (render, the blur gate, the frame pacer, the damage gate, the surface
    // gate) plus once per pointer motion and once per client commit. At n tiles
    // that is O(n * (W + n^2)) per frame per output for an answer that cannot
    // change inside a frame.
    //
    // The arithmetic is TRANSCRIBED, not rewritten: the same accumulations in
    // the same order, so every slot lands on the same float it did before. What
    // changed is that the row walk now assigns every cell as it goes instead of
    // stopping at the target.
    namespace {
        void solveGrid(const std::vector<PHLWINDOW>& windows, const std::vector<Vector2D>& sizes, const CBox& usable, float gap, std::vector<CBox>& out) {
            out.clear();

            const size_t COUNT = windows.size();
            const float  GAP   = gap;

            // SCORED WITH THE ROWS PLACEMENT WILL ACTUALLY BUILD (user, 2026-09-23:
            // "I see 5 windows and a lot of space on the right and left of them").
            //
            // This used to give every window an EQUAL cell -- usable width over the
            // column count -- and take the smallest scale any window allowed in
            // it. With mixed widths that is the WIDEST window's scale: a
            // full-width window in a cell sized for a fifth of the row pinned the
            // scale, and the half-width windows beside it were then drawn far
            // narrower than their cells. Placement below packs by REAL widths, so
            // the row came out a good deal narrower than the screen and the
            // difference sat empty at both ends.
            //
            // Now each candidate row count is split exactly as placement splits it
            // (earlier rows take the remainder), and the scale is the largest that
            // fits every row's REAL total width and the rows' real stacked height.
            // It can only be equal or larger than the old answer: the old per-cell
            // bound was a stricter version of the same constraint.
            size_t bestRows  = 0;
            float  bestScale = 0.F;
            for (size_t rows = 1; rows <= COUNT; ++rows) {
                const size_t BASE  = COUNT / rows;
                const size_t EXTRA = COUNT % rows;

                float  scale    = 1.F;
                float  sumRowH  = 0.F;
                size_t first    = 0;
                bool   feasible = true;
                for (size_t row = 0; row < rows; ++row) {
                    const size_t N = BASE + (row < EXTRA ? 1 : 0);
                    if (N == 0) {
                        feasible = false;
                        break;
                    }

                    float rowW = 0.F, rowH = 0.F;
                    for (size_t i = first; i < first + N; ++i) {
                        rowW += std::max(1.F, sc<float>(sizes[i].x));
                        rowH = std::max(rowH, std::max(1.F, sc<float>(sizes[i].y)));
                    }
                    first += N;

                    const float ROOM = sc<float>(usable.w) - GAP * sc<float>(N - 1);
                    if (ROOM <= 1.F) {
                        feasible = false;
                        break;
                    }
                    scale = std::min(scale, ROOM / rowW);
                    sumRowH += rowH;
                }

                const float ROOMH = sc<float>(usable.h) - GAP * sc<float>(rows - 1);
                if (!feasible || ROOMH <= 1.F || sumRowH <= 0.F)
                    continue;
                scale = std::min(scale, ROOMH / sumRowH);

                if (scale > bestScale) {
                    bestScale = scale;
                    bestRows  = rows;
                }
            }

            if (bestRows == 0 || bestScale <= 0.F)
                return;

            // Spread the windows over the rows as evenly as possible, earlier rows take
            // the remainder.
            const size_t BASE  = COUNT / bestRows;
            const size_t EXTRA = COUNT % bestRows;

            out.resize(COUNT);

            size_t rowFirst = 0;
            float  y        = sc<float>(usable.y);
            for (size_t row = 0; row < bestRows; ++row) {
                const size_t ROWCOUNT = BASE + (row < EXTRA ? 1 : 0);

                float rowWidth = GAP * sc<float>(ROWCOUNT - 1);
                float rowH     = 0.F;
                for (size_t i = 0; i < ROWCOUNT; ++i) {
                    rowWidth += sc<float>(sizes[rowFirst + i].x) * bestScale;
                    rowH = std::max(rowH, sc<float>(sizes[rowFirst + i].y) * bestScale);
                }

                float x = sc<float>(usable.x) + (sc<float>(usable.w) - rowWidth) / 2.F;
                for (size_t i = rowFirst; i < rowFirst + ROWCOUNT; ++i) {
                    out[i] = CBox{x, y, sc<float>(sizes[i].x) * bestScale, sc<float>(sizes[i].y) * bestScale};
                    x += sc<float>(sizes[i].x) * bestScale + GAP;
                }

                y += rowH + GAP;
                rowFirst += ROWCOUNT;
            }
        }

        // THE MEMO, and every field of it is a KEY: the slot vector is only
        // reused when nothing the solver reads has moved. There is no
        // generation number to forget to bump, and no frame counter -- a frame
        // counter would have been wrong twice over. renderSearch publishes the
        // top-chrome height MID-frame, so two calls in one frame legitimately
        // disagree about `usable`; and a hit test between frames must see the
        // layout that is actually on screen, which is the one whose inputs are
        // unchanged.
        //
        // The key is rebuilt on every call -- that is the O(W) membership walk,
        // which no longer carries a config lookup per window -- and what it
        // buys is the O(n^2) row search, the n geometricBox reads the row walk
        // repeated, and the whole solve.
        struct SGridMemo {
            // --- key
            WORKSPACEID                          ws             = WORKSPACE_INVALID;
            MONITORID                            mon            = MONITOR_INVALID;
            std::string                          query;
            // The same argument Membership's own cache makes for keying on the
            // generation: `members` holds raw addresses, and an address can be
            // reused by a later window. Every open, close, kill and destroy
            // bumps this, so a recycled address is always looked up in a
            // generation the stale entry does not carry.
            uint64_t                             membersGen    = 0;
            Desktop::View::CWindow*              excluded       = nullptr;
            bool                                 keepExcluded   = false;
            float                                gap            = 0.F;
            CBox                                 usable;
            std::vector<Desktop::View::CWindow*> members;
            std::vector<Vector2D>                sizes;
            // --- value
            std::vector<CBox> slots;
            bool              valid = false;
        };

        bool sameBox(const CBox& a, const CBox& b) {
            return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
        }

        // Two outputs, each with the ordinary layout and the drag's own
        // one-window-more variant, is four live entries; the cap is double that
        // so a workspace switch cannot thrash it. Linear over a handful of
        // entries beats hashing a key this wide.
        constexpr size_t GRID_MEMO_MAX = 8;

        std::vector<SGridMemo>& gridMemos() {
            static std::vector<SGridMemo> MEMOS;
            return MEMOS;
        }
    }

    bool overviewGridLayout(const PHLWORKSPACE& workspace, PHLMONITOR monitor, const PHLWINDOW& target, CBox& out) {
        if (!workspace || !monitor || !target)
            return false;

        // Reused across calls: the membership walk is the one allocation this
        // function would otherwise make several dozen times a frame. EMPTIED on
        // the way out, every way out -- it holds owning window handles, and a
        // buffer that kept the last grid's would keep a closed window alive
        // until the next time an overview was opened.
        static std::vector<PHLWINDOW>                WINDOWS;
        static std::vector<Desktop::View::CWindow*>  MEMBERS;
        static std::vector<Vector2D>                 SIZES;

        auto dropWindowRefs = Hyprutils::Utils::CScopeGuard([] { WINDOWS.clear(); });

        overviewMainGridWindowsInto(workspace, monitor, WINDOWS);

        // The dragged window keeps a slot for its OWN query: the drag proxy is sized
        // off it, and a proxy sized from the N-1 layout would jump the frame the
        // drag begins. It is absent only from everyone else's layout.
        const auto EXCLUDED     = grid().dragExcluded;
        const bool KEEPEXCLUDED = EXCLUDED && target.get() == EXCLUDED;
        if (EXCLUDED && !KEEPEXCLUDED)
            std::erase_if(WINDOWS, [EXCLUDED](const auto& window) { return window.get() == EXCLUDED; });

        if (Membership::renderShadowEnabled()) {
            for (size_t i = 0; i < WINDOWS.size(); ++i)
                for (size_t j = i + 1; j < WINDOWS.size(); ++j)
                    if (WINDOWS[i] == WINDOWS[j])
                        Membership::renderDivergence("GRIDDUP", workspace->m_id, WINDOWS[i]->m_workspace ? WINDOWS[i]->m_workspace->m_id : WORKSPACE_INVALID,
                                                     WINDOWS[i]->m_class, WINDOWS[i]->m_title);
        }

        size_t index = WINDOWS.size();
        for (size_t i = 0; i < WINDOWS.size(); ++i) {
            if (WINDOWS[i] == target) {
                index = i;
                break;
            }
        }
        if (index >= WINDOWS.size())
            return false;

        CBox usable = {monitor->m_position, monitor->m_size};
        if (workspace->m_space) {
            if (const auto WORKAREA = workspace->m_space->workArea(); WORKAREA.w > 1 && WORKAREA.h > 1)
                usable = WORKAREA;
        }

        // Grid tiles must not slide under the strip band or the search row (X10) --
        // shrinking `usable` here is what makes the tiles compact to give the space
        // back.
        //
        // X60 SUPERSEDES X48's placement: the results are part of the TOP chrome
        // now, between the search row and the strip, so the whole stack below them
        // moves down and the tiles give their space back from the top. X21's reflow
        // is untouched -- the tiles still shrink and compact off the same eased
        // band, only the direction changed. X48's real constraint survives in
        // overviewResultsBandTarget's budget: the band may never grow so tall that
        // the strip and the tiles are pushed off the screen.
        //
        // The strip's reserved band is taller than the drawn panel. Inset by the
        // panel's foot plus the same gap that sits between the search pill and
        // the strip, not by the unused bottom of the band plus another
        // mission-control gap -- that pair was the hole between strip and windows.
        //
        // The stack is measured from the MONITOR's top edge -- the pill docks
        // off overviewTopChromeBottomL and the strip's band off m_position.y --
        // so it is a floor to clamp against, not a margin to add. Adding it to
        // a work area that already excluded a top bar counted that bar twice
        // and pushed every tile down by its height (22px under this rice).
        if (const float TOPCHROME = overviewStripStackL(monitor) + overviewSearchLogicalHeight(monitor) + overviewResultsLogicalHeight(monitor); TOPCHROME > 0.F) {
            const double BOTTOM = usable.y + usable.h;
            usable.y            = std::max(usable.y, monitor->m_position.y + TOPCHROME);
            usable.h            = BOTTOM - usable.y;
            if (usable.h <= 1)
                return false;
        }
        const float GAP = sc<float>(overviewMissionControlGap());

        usable.x += GAP;
        usable.w -= 2.0 * GAP;
        usable.h -= GAP;
        if (usable.w <= 1 || usable.h <= 1)
            return false;

        MEMBERS.clear();
        SIZES.clear();
        MEMBERS.reserve(WINDOWS.size());
        SIZES.reserve(WINDOWS.size());
        for (const auto& WINDOW : WINDOWS) {
            MEMBERS.emplace_back(WINDOW.get());
            SIZES.emplace_back(overviewGridWindowSize(WINDOW));
        }

        const uint64_t MEMBERSGEN = Membership::generation();

        auto&      MEMOS = gridMemos();
        SGridMemo* hit   = nullptr;
        for (auto& memo : MEMOS) {
            if (memo.ws != workspace->m_id || memo.mon != monitor->m_id || memo.excluded != EXCLUDED || memo.keepExcluded != KEEPEXCLUDED || memo.gap != GAP)
                continue;
            if (memo.membersGen != MEMBERSGEN)
                continue;
            if (!sameBox(memo.usable, usable) || memo.query != g_search.query.text)
                continue;
            if (memo.members != MEMBERS || memo.sizes != SIZES)
                continue;

            hit = &memo;
            break;
        }

        if (!hit) {
            if (MEMOS.size() >= GRID_MEMO_MAX)
                MEMOS.erase(MEMOS.begin());

            MEMOS.emplace_back();
            hit               = &MEMOS.back();
            hit->ws           = workspace->m_id;
            hit->mon          = monitor->m_id;
            hit->query        = g_search.query.text;
            hit->membersGen   = MEMBERSGEN;
            hit->excluded     = EXCLUDED;
            hit->keepExcluded = KEEPEXCLUDED;
            hit->gap          = GAP;
            hit->usable       = usable;
            hit->members      = MEMBERS;
            hit->sizes        = SIZES;

            solveGrid(WINDOWS, SIZES, usable, GAP, hit->slots);
            hit->valid = !hit->slots.empty();
        }

        if (!hit->valid || index >= hit->slots.size())
            return false;

        out = hit->slots[index];
        // The strip label's slide (gridShiftL): every slot, after the solve, so
        // the grid keeps its size and just moves.
        out.y += g_search.layoutOn(monitor->m_id).gridShiftL;
        return true;
    }

    bool overviewGridSlot(const PHLWINDOW& window, PHLMONITOR monitor, CBox& out, bool* promotedOut) {
        if (promotedOut)
            *promotedOut = false;

        if (!overviewMissionControlOn() || !monitor)
            return false;

        const auto ACTIVE = monitor->m_activeWorkspace;
        if (!ACTIVE)
            return false;

        // A promoted window is laid out in the ACTIVE workspace's grid, not its own.
        // HOME, not m_workspace: a window stashed FROM the active desktop owns a slot
        // in the active grid outright, and asking its m_workspace sent it down the
        // promoted branch instead -- the other half of the duplicate above.
        //
        // The promoted answer is HANDED BACK rather than asked for a second time
        // by the caller: overviewGridPromoted arms the release map, and
        // getOverviewWindowBox was asking it twice per box for a value that
        // cannot differ between the two calls.
        if (overviewGridHomeWorkspace(window) != ACTIVE->m_id) {
            if (!overviewGridPromoted(window, monitor))
                return false;

            if (promotedOut)
                *promotedOut = true;
        }

        return overviewGridLayout(ACTIVE, monitor, window, out);
    }

    float overviewGridProgress(float scale) {
        return ScrollOverview::Geometry::gridProgress(scale, overviewOpenScale());
    }

    CBox overviewGridLerpBox(const CBox& from, const CBox& to, float t) {
        return ScrollOverview::Geometry::lerpBox(from, to, t);
    }

    // WHY THE RELEASE NEEDS ITS OWN CURVE, and it is not a taste question.
    //
    // A reflow ENDS somewhere the user is looking at -- a slot in the grid -- so
    // the house decel curve is right: it spends its speed early and settles in
    // view. The release ENDS OFF THE MONITOR, at the window's real rect on a
    // desktop that is two cards away, and the same decel curve then spends that
    // speed getting there. Measured on 9c2742f at anim_scale 20 with the
    // render-list trace (evidence: zrig/f3b/round1/): the tile is drawn every
    // frame, correctly interpolated, and crosses the monitor edge at animated
    // elapsed 24.4ms of a 300ms curve -- 8.1% of the animation, 42% of the
    // travel, and ONE AND A HALF FRAMES at 60Hz once the slowdown is taken off.
    // That is exactly the "'Fast' is not a licence to drop to one or two frames"
    // that F3-b-ii forbids, and it is why the outbound reads as a cut.
    //
    // The house ACCEL curve (0.7,0 0.84,0) leaves slowly and then goes: the same
    // 42% of travel now takes 87.4% of the duration, so the visible half of the
    // journey gets the whole animation instead of its first twelfth. Same curve
    // the overview's own close uses, and the same direction the Apple rule asks
    // an exit to run in.
    float gridReflowEase(float t, bool exiting) {
        return houseEase(!exiting, t);
    }

    bool gridBoxesDiffer(const CBox& a, const CBox& b) {
        return ScrollOverview::Geometry::boxesDiffer(a, b);
    }

    CBox gridReflowBox(const PHLMONITOR& monitor, const PHLWINDOW& window, const CBox& slot, const CBox* enterFrom, bool promoted, bool exiting) {
        const uint64_t FRAME = overviewFrameOn(monitor);
        const int DURATION = exiting ? gridReleaseMs() : overviewMissionControlReflowMs();
        if (DURATION <= 0)
            return slot;

        const auto NOW = Time::steadyNow();
        auto&      G   = grid();

        if (G.reflows.size() > 64)
            std::erase_if(G.reflows, [](const auto& entry) { return !entry.second.window.lock(); });

        auto& entry = G.reflows[window.get()];

        // OVERVIEW_CPP_SPEC F3-b-i. A tile that was not in the grid last frame is
        // ENTERING now -- first appearance, or a search promoting it off another
        // desktop. Seeding from `slot` makes from == to and draws it straight at its
        // destination, which is exactly the "matches just spawn in" defect. Seed it
        // where it really sits and the ease below carries it in.
        //
        // Untouched-for-a-while stands in for "was not in the grid", so searching the
        // same window a second time re-enters properly instead of reusing its stale
        // settled entry. KNOWN == false also catches a recycled address handing a new
        // window a dead one's entry.
        const bool KNOWN = entry.window.lock() == window;
        // An entering tile is seeded off-screen on purpose, so it is culled -- and
        // on frames where that culls the whole workspace, its pass is skipped and
        // this function is not called at all. Reseeding then would throw away an
        // ease that is still running and draw the tile at its destination.
        const bool INFLIGHT = KNOWN && overviewAnimElapsedMs(entry.start) < sc<float>(DURATION);
        // Genuinely absent: a tile that stopped being drawn entirely, stayed gone
        // long enough to finish whatever it was doing, and is coming back.
        const bool UNDRAWN = KNOWN && !INFLIGHT && FRAME - entry.lastFrame > 1;
        // ...and the case staleness can never see, because the tile was on screen
        // the whole time: it just crossed onto the active card.
        const bool JOINED = KNOWN && promoted && !entry.promotedLast;

        if (!KNOWN || UNDRAWN || JOINED) {
            const CBox START = enterFrom ? *enterFrom : slot;
            entry            = SGridReflow{window, START, slot, NOW, FRAME, promoted};
            if (gridBoxesDiffer(START, slot))
                gridSetReflowActive(monitor);
            return START;
        }

        entry.lastFrame    = FRAME;
        entry.promotedLast = promoted;

        const float ELAPSED = overviewAnimElapsedMs(entry.start);
        const float T       = gridReflowEase(ELAPSED / sc<float>(DURATION), exiting);
        const CBox  CURRENT = overviewGridLerpBox(entry.from, entry.to, T);

        if (gridBoxesDiffer(slot, entry.to)) {
            entry.from  = CURRENT; // retarget from where the tile actually is
            entry.to    = slot;
            entry.start = NOW;
            gridSetReflowActive(monitor);
            return CURRENT;
        }

        if (T < 1.F)
            gridSetReflowActive(monitor);

        return CURRENT;
    }

} // namespace OvInternal
