#pragma once

// THE OVERVIEW GRID'S PURE CORE (GridLayout.cpp is its only user in the
// plugin; tests/unit/grid_solve_test.cpp tests it on the host): the order the
// grid lays its windows out in, the solver, and the rule that makes every box
// asked for in one drawn frame come from ONE solve.
//
// WHY IT EXISTS (#9 and #7, 2026-11). A hovered tile jumped about a third of a
// tile to the right with Hyprdrive motion off, and shook with it on; only the
// last tile of a row was spared. The tile pass draws a window by moving its
// position and size onto its tile for the length of its draw
// (Window.cpp renderOverviewWindow), and a box query that arrived inside that
// draw -- for the HOVERED tile, which is drawn last and asked about most --
// solved the grid with the drawn window at its TILE size: a row narrower by
// that window's shrink, so every tile before it in the row slid right by half
// of it (1920x1080 at 1.5: 116 px, the reporter's number to the pixel). With
// motion off the reflow's one-call lag then drew that wrong slot every frame;
// with motion on the ease was re-aimed between the two every frame.
//
// Two rules close it, and both are here so they can be tested without a
// compositor:
//   order()          -- the grid's order is never Hyprland's window list,
//                       which a click or a raise reorders (z-order). It is
//                       a key the overview took when it OPENED
//                       (plugin:hyprdrive:overview_tile_order: last used,
//                       order opened, or desktop position -- recentKey(),
//                       the creation sequence, positionOrder()), so the same
//                       windows land in the same slots whatever is focused
//                       or hovered while it is up.
//   frameSolveHolds() -- inside the frame that draws (SGridOwnerScope), the
//                       first solve of a grid is the answer for every later
//                       query of that frame that has the same members, chrome
//                       and gap: a size read mid-frame cannot re-solve it.

#include <hyprutils/math/Box.hpp>
#include <hyprutils/math/Vector2D.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <optional>
#include <tuple>
#include <vector>

namespace Hyprdrive::GridSolve {

    using Hyprutils::Math::CBox;
    using Hyprutils::Math::Vector2D;

    // The grid's order: by a key (`seq` maps an element to it: the creation
    // sequence -- Hyprland's m_stableID, which counts up as windows are made --
    // or recentKey()), ties kept as given.
    template <class T, class Seq>
    void order(std::vector<T>& items, size_t first, size_t last, Seq seq) {
        if (first >= last || last > items.size())
            return;
        std::stable_sort(items.begin() + static_cast<std::ptrdiff_t>(first), items.begin() + static_cast<std::ptrdiff_t>(last),
                         [&seq](const T& a, const T& b) { return seq(a) < seq(b); });
    }

    // LAST USED FIRST. `rank` is the window's place in the focus history when
    // the overview opened, 0 the most recent; `known` is false for a window
    // that was not there then (opened since): those go after every known one,
    // in the order they were made, so a window opened while the overview is up
    // is appended and nothing already in the grid moves.
    inline std::tuple<bool, uint64_t, uint64_t> recentKey(bool known, uint64_t rank, uint64_t seq) {
        return {!known, known ? rank : 0, seq};
    }

    // A solve may answer a later query of the SAME drawn frame although the
    // sizes it was given have since read differently: only inside the frame
    // that draws (a query between frames, a hit test or a commit, solves from
    // what it reads), only for the frame it was solved in, and only for the
    // same members -- everything else in the key (chrome, gap, query) is
    // compared by the caller as before, so the strip label publishing the top
    // chrome mid-frame still re-solves.
    inline bool frameSolveHolds(bool inDrawnFrame, uint64_t solvedFrame, uint64_t frame, bool sameMembers) {
        return inDrawnFrame && solvedFrame != 0 && solvedFrame == frame && sameMembers;
    }

    // THE SOLVER, moved here from GridLayout.cpp unchanged (the same
    // accumulations in the same order, so every slot lands on the same float),
    // split in two so the position order can score a row count it chose:
    // rowsScale() is the body of the row-count search for ONE count, solveRows()
    // the search over every count, place() the row walk, and solve() the two
    // together, as before.
    //
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
    //
    // The scale `rows` rows give `sizes` (in the grid's order), 0 when they do
    // not fit.
    inline float rowsScale(const std::vector<Vector2D>& sizes, size_t rows, const CBox& usable, float gap) {
        const size_t COUNT = sizes.size();
        const float  GAP   = gap;
        if (rows == 0 || rows > COUNT)
            return 0.F;

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
                rowW += std::max(1.F, static_cast<float>(sizes[i].x));
                rowH = std::max(rowH, std::max(1.F, static_cast<float>(sizes[i].y)));
            }
            first += N;

            const float ROOM = static_cast<float>(usable.w) - GAP * static_cast<float>(N - 1);
            if (ROOM <= 1.F) {
                feasible = false;
                break;
            }
            scale = std::min(scale, ROOM / rowW);
            sumRowH += rowH;
        }

        const float ROOMH = static_cast<float>(usable.h) - GAP * static_cast<float>(rows - 1);
        if (!feasible || ROOMH <= 1.F || sumRowH <= 0.F)
            return 0.F;
        return std::min(scale, ROOMH / sumRowH);
    }

    // The row walk: `sizes` over `rows` rows at `scale`, one slot each.
    inline void place(const std::vector<Vector2D>& sizes, size_t rows, float scale, const CBox& usable, float gap, std::vector<CBox>& out) {
        out.clear();

        const size_t COUNT = sizes.size();
        const float  GAP   = gap;
        if (rows == 0 || rows > COUNT || scale <= 0.F)
            return;

        // Spread the windows over the rows as evenly as possible, earlier rows take
        // the remainder.
        const size_t BASE  = COUNT / rows;
        const size_t EXTRA = COUNT % rows;

        out.resize(COUNT);

        size_t rowFirst = 0;
        float  y        = static_cast<float>(usable.y);
        for (size_t row = 0; row < rows; ++row) {
            const size_t ROWCOUNT = BASE + (row < EXTRA ? 1 : 0);

            float rowWidth = GAP * static_cast<float>(ROWCOUNT - 1);
            float rowH     = 0.F;
            for (size_t i = 0; i < ROWCOUNT; ++i) {
                rowWidth += static_cast<float>(sizes[rowFirst + i].x) * scale;
                rowH = std::max(rowH, static_cast<float>(sizes[rowFirst + i].y) * scale);
            }

            float x = static_cast<float>(usable.x) + (static_cast<float>(usable.w) - rowWidth) / 2.F;
            for (size_t i = rowFirst; i < rowFirst + ROWCOUNT; ++i) {
                out[i] = CBox{x, y, static_cast<float>(sizes[i].x) * scale, static_cast<float>(sizes[i].y) * scale};
                x += static_cast<float>(sizes[i].x) * scale + GAP;
            }

            y += rowH + GAP;
            rowFirst += ROWCOUNT;
        }
    }

    // The row count solve() picks for `sizes` (0: nothing fits), and its scale.
    inline size_t solveRows(const std::vector<Vector2D>& sizes, const CBox& usable, float gap, float* scaleOut = nullptr) {
        size_t bestRows  = 0;
        float  bestScale = 0.F;
        for (size_t rows = 1; rows <= sizes.size(); ++rows) {
            if (const float SCALE = rowsScale(sizes, rows, usable, gap); SCALE > bestScale) {
                bestScale = SCALE;
                bestRows  = rows;
            }
        }
        if (scaleOut)
            *scaleOut = bestScale;
        return bestRows;
    }

    // `sizes` in the grid's order; `out` gets one slot per size, or nothing.
    inline void solve(const std::vector<Vector2D>& sizes, const CBox& usable, float gap, std::vector<CBox>& out) {
        out.clear();

        float        bestScale = 0.F;
        const size_t BESTROWS  = solveRows(sizes, usable, gap, &bestScale);
        if (BESTROWS == 0 || bestScale <= 0.F)
            return;

        place(sizes, BESTROWS, bestScale, usable, gap, out);
    }

    // MATCH THE DESKTOP. `centres[i]` is where window i's centre sat on its
    // desktop when the overview opened, or nothing (opened since, or promoted
    // by a search from another desktop): those keep their given order, after
    // every placed one. The input is in creation order, so ties -- windows
    // stacked on the same centre -- stay in creation order too.
    //
    // For each row count the grid could have, the placed windows are taken
    // top to bottom by their centre's y and dealt into the rows exactly as the
    // solver fills them (earlier rows take the remainder); each row is then
    // read left to right by the centre's x. A dealing is only taken if the
    // plain solve() of that order builds the same row count (so the rows the
    // windows were dealt into are the rows drawn); of those, the one with the
    // largest tiles wins, the solver's tie rule (fewer rows) on a tie. If no
    // dealing is self-consistent -- not seen on any real desktop, but sizes
    // can be anything -- the largest-tile one is taken anyway.
    //
    // The result is an ORDER, not a layout: `perm[k]` is the input index that
    // goes k-th, and the caller lays the grid out with solve() over it like
    // any other order. GridLayout.cpp takes it once per desktop per overview
    // session and keeps it, so a window opened meanwhile is appended, as in
    // the other orders, instead of re-dealing the rows under the pointer.
    // Answers the row count it was dealt for (0: nothing fits).
    inline size_t positionOrder(const std::vector<Vector2D>& sizes, const std::vector<std::optional<Vector2D>>& centres, const CBox& usable, float gap,
                                std::vector<size_t>& perm) {
        const size_t COUNT = sizes.size();
        perm.clear();
        if (COUNT == 0 || centres.size() != COUNT)
            return 0;

        const auto PLACEDFIRST = [&centres](size_t a, size_t b, bool byX) {
            if (centres[a].has_value() != centres[b].has_value())
                return centres[a].has_value();
            if (!centres[a])
                return false;
            return byX ? centres[a]->x < centres[b]->x : centres[a]->y < centres[b]->y;
        };

        // Top to bottom, the unplaced after them in their given order.
        std::vector<size_t> byY(COUNT);
        std::iota(byY.begin(), byY.end(), size_t{0});
        std::stable_sort(byY.begin(), byY.end(), [&](size_t a, size_t b) { return PLACEDFIRST(a, b, false); });

        std::vector<size_t>   candidate, anyPerm;
        std::vector<Vector2D> permuted;
        size_t                bestRows = 0, anyRows = 0;
        float                 bestScale = 0.F, anyScale = 0.F;
        for (size_t rows = 1; rows <= COUNT; ++rows) {
            candidate = byY;
            // Each row left to right; a row's unplaced (only the last rows can
            // hold any) stay at its end.
            const size_t BASE  = COUNT / rows;
            const size_t EXTRA = COUNT % rows;
            size_t       first = 0;
            for (size_t row = 0; row < rows; ++row) {
                const size_t N = BASE + (row < EXTRA ? 1 : 0);
                std::stable_sort(candidate.begin() + static_cast<std::ptrdiff_t>(first), candidate.begin() + static_cast<std::ptrdiff_t>(first + N),
                                 [&](size_t a, size_t b) { return PLACEDFIRST(a, b, true); });
                first += N;
            }

            permuted.clear();
            for (const size_t I : candidate)
                permuted.emplace_back(sizes[I]);
            const float SCALE = rowsScale(permuted, rows, usable, gap);
            if (SCALE <= 0.F)
                continue;
            if (SCALE > anyScale) {
                anyScale = SCALE;
                anyRows  = rows;
                anyPerm  = candidate;
            }
            if (SCALE > bestScale && solveRows(permuted, usable, gap) == rows) {
                bestScale = SCALE;
                bestRows  = rows;
                perm      = candidate;
            }
        }

        if (bestRows == 0) {
            perm = anyPerm;
            return anyRows;
        }
        return bestRows;
    }

} // namespace Hyprdrive::GridSolve
