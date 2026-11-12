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
//   order()          -- the grid's order is the windows' CREATION order, not
//                       Hyprland's window list, which a focus or a raise
//                       reorders (z-order). The same windows always land in
//                       the same slots, whatever was focused or hovered.
//   frameSolveHolds() -- inside the frame that draws (SGridOwnerScope), the
//                       first solve of a grid is the answer for every later
//                       query of that frame that has the same members, chrome
//                       and gap: a size read mid-frame cannot re-solve it.

#include <hyprutils/math/Box.hpp>
#include <hyprutils/math/Vector2D.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace Hyprdrive::GridSolve {

    using Hyprutils::Math::CBox;
    using Hyprutils::Math::Vector2D;

    // The grid's order: by creation sequence (Hyprland's m_stableID, which
    // counts up as windows are made), ties kept as given. `seq` maps an
    // element to its sequence.
    template <class T, class Seq>
    void order(std::vector<T>& items, size_t first, size_t last, Seq seq) {
        if (first >= last || last > items.size())
            return;
        std::stable_sort(items.begin() + static_cast<std::ptrdiff_t>(first), items.begin() + static_cast<std::ptrdiff_t>(last),
                         [&seq](const T& a, const T& b) { return seq(a) < seq(b); });
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
    // accumulations in the same order, so every slot lands on the same float).
    // `sizes` in the grid's order; `out` gets one slot per size, or nothing.
    inline void solve(const std::vector<Vector2D>& sizes, const CBox& usable, float gap, std::vector<CBox>& out) {
        out.clear();

        const size_t COUNT = sizes.size();
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
        float  y        = static_cast<float>(usable.y);
        for (size_t row = 0; row < bestRows; ++row) {
            const size_t ROWCOUNT = BASE + (row < EXTRA ? 1 : 0);

            float rowWidth = GAP * static_cast<float>(ROWCOUNT - 1);
            float rowH     = 0.F;
            for (size_t i = 0; i < ROWCOUNT; ++i) {
                rowWidth += static_cast<float>(sizes[rowFirst + i].x) * bestScale;
                rowH = std::max(rowH, static_cast<float>(sizes[rowFirst + i].y) * bestScale);
            }

            float x = static_cast<float>(usable.x) + (static_cast<float>(usable.w) - rowWidth) / 2.F;
            for (size_t i = rowFirst; i < rowFirst + ROWCOUNT; ++i) {
                out[i] = CBox{x, y, static_cast<float>(sizes[i].x) * bestScale, static_cast<float>(sizes[i].y) * bestScale};
                x += static_cast<float>(sizes[i].x) * bestScale + GAP;
            }

            y += rowH + GAP;
            rowFirst += ROWCOUNT;
        }
    }

} // namespace Hyprdrive::GridSolve
