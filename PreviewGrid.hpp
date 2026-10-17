#pragma once

// THE DOCK'S HOVER PREVIEWS IN ROWS (user, 2026-10-15: "I have 22 ghostty
// windows open ... live previews on dock shows less than the total number of
// windows ... add rows once number of windows exceeds ~50% screen width area.
// once a row is added split windows evenly among rows"). Pure, so
// scripts/preview_grid_test.cpp builds it on the host; Dock.cpp feeds it each
// preview's width at the panel's cell height.

#include <algorithm>
#include <cstddef>
#include <vector>

namespace ScrollOverview::PreviewGrid {

    // The width of `count` cells from `from`, `gap` between neighbours.
    inline float rowWidth(const std::vector<float>& widths, size_t from, size_t count, float gap) {
        float w = 0.F;
        for (size_t i = from; i < from + count && i < widths.size(); ++i)
            w += widths[i] + (i > from ? gap : 0.F);
        return w;
    }

    // How many cells each row holds, top row first. One row while it fits
    // `maxRowW`; otherwise the fewest rows that each fit, the windows split
    // EVENLY among them in order (an uneven count puts the extra ones in the
    // first rows). A single cell wider than `maxRowW` still gets its row.
    inline std::vector<size_t> rows(const std::vector<float>& widths, float gap, float maxRowW) {
        const size_t N = widths.size();
        if (N == 0)
            return {};
        for (size_t r = 1; r <= N; ++r) {
            const size_t        BASE = N / r, EXTRA = N % r;
            std::vector<size_t> out;
            bool                ok   = true;
            size_t              from = 0;
            for (size_t k = 0; k < r; ++k) {
                const size_t COUNT = BASE + (k < EXTRA ? 1 : 0);
                if (COUNT > 1 && rowWidth(widths, from, COUNT, gap) > maxRowW)
                    ok = false;
                out.push_back(COUNT);
                from += COUNT;
            }
            if (ok)
                return out;
        }
        return std::vector<size_t>(N, 1);
    }

    // The widest row of an arrangement.
    inline float widest(const std::vector<float>& widths, const std::vector<size_t>& rowCounts, float gap) {
        float  w    = 0.F;
        size_t from = 0;
        for (const size_t C : rowCounts) {
            w = std::max(w, rowWidth(widths, from, C, gap));
            from += C;
        }
        return w;
    }
}
