#pragma once

// MOSAIC'S GEOMETRY (user, 2026-10-11), pure: no Hyprland, so
// scripts/mosaic_pack_test.cpp builds it on the host. Mosaic.cpp feeds it the
// windows' natural sizes and the work area and places what comes back.
//
// THE MOSAIC (after GNOME's planned Mosaic, the user's reference clip): every
// window keeps its own size, and they gather into one cluster centred on the
// desktop -- rows, each centred, the whole stack centred, `gap` between
// neighbours. The row count is chosen to make the cluster as compact as the
// screen's own shape allows. When no row count fits -- too many windows, or
// one too big -- mosaic() answers nothing and the layout tiles instead.
//
// THE TILES (the fallback): a grid sized so its cells come out closest to a
// landscape window's shape; the last row shares the full width.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <vector>

namespace ScrollOverview::MosaicPack {

    struct SRect {
        double x = 0, y = 0, w = 0, h = 0;
    };

    // (named so it cannot collide with the plugin's sc<> helper when included there)
    inline double sc_double(size_t v) {
        return static_cast<double>(v);
    }

    // Windows in order, split into `rows` consecutive runs whose widths are as
    // even as possible (the widest run as narrow as possible): a greedy fill
    // against the average, which for a handful of windows lands on the optimum
    // or next to it.
    inline std::vector<std::vector<size_t>> splitRows(const std::vector<SRect>& sizes, size_t rows, double gap) {
        std::vector<std::vector<size_t>> out;
        double                           total = 0;
        for (const auto& S : sizes)
            total += S.w;
        total += gap * sc_double(sizes.size() > rows ? sizes.size() - rows : 0);
        const double TARGET = total / sc_double(rows);

        size_t i = 0;
        for (size_t r = 0; r < rows && i < sizes.size(); ++r) {
            std::vector<size_t> row;
            double              width = 0;
            const size_t        LEFTROWS = rows - r - 1;
            while (i < sizes.size()) {
                // Leave at least one window for each row still to come.
                if (!row.empty() && sizes.size() - i <= LEFTROWS)
                    break;
                const double NEXT = width + (row.empty() ? 0 : gap) + sizes[i].w;
                // Stop before the run passes the average by more than half the
                // window that would take it there.
                if (!row.empty() && r + 1 < rows && NEXT - TARGET > sizes[i].w / 2.0)
                    break;
                row.push_back(i);
                width = NEXT;
                ++i;
            }
            out.push_back(std::move(row));
        }
        while (i < sizes.size())
            out.back().push_back(i++);
        return out;
    }

    // The mosaic, or nothing when no row count fits the area.
    inline std::optional<std::vector<SRect>> mosaic(const std::vector<SRect>& sizes, const SRect& area, double gap) {
        if (sizes.empty())
            return std::vector<SRect>{};
        for (const auto& S : sizes)
            if (S.w <= 0 || S.h <= 0 || S.w > area.w || S.h > area.h)
                return std::nullopt;

        double                           bestScore = std::numeric_limits<double>::infinity();
        std::vector<std::vector<size_t>> best;
        for (size_t rows = 1; rows <= sizes.size(); ++rows) {
            const auto SPLIT = splitRows(sizes, rows, gap);
            double     bw = 0, bh = 0;
            for (const auto& ROW : SPLIT) {
                double w = 0, h = 0;
                for (size_t k = 0; k < ROW.size(); ++k) {
                    w += sizes[ROW[k]].w + (k ? gap : 0);
                    h = std::max(h, sizes[ROW[k]].h);
                }
                bw = std::max(bw, w);
                bh += h;
            }
            bh += gap * sc_double(SPLIT.size() - 1);
            if (bw > area.w || bh > area.h)
                continue;
            // Compact for THIS screen: the cluster's larger share of its axis.
            const double SCORE = std::max(bw / area.w, bh / area.h);
            if (SCORE < bestScore - 1e-9) {
                bestScore = SCORE;
                best      = SPLIT;
            }
        }
        if (best.empty())
            return std::nullopt;

        std::vector<double> rowW(best.size()), rowH(best.size());
        double              totalH = 0;
        for (size_t r = 0; r < best.size(); ++r) {
            for (size_t k = 0; k < best[r].size(); ++k) {
                rowW[r] += sizes[best[r][k]].w + (k ? gap : 0);
                rowH[r] = std::max(rowH[r], sizes[best[r][k]].h);
            }
            totalH += rowH[r] + (r ? gap : 0);
        }

        std::vector<SRect> out(sizes.size());
        double             y = area.y + (area.h - totalH) / 2.0;
        for (size_t r = 0; r < best.size(); ++r) {
            double x = area.x + (area.w - rowW[r]) / 2.0;
            for (const size_t I : best[r]) {
                out[I] = SRect{std::round(x), std::round(y + (rowH[r] - sizes[I].h) / 2.0), std::round(sizes[I].w), std::round(sizes[I].h)};
                x += sizes[I].w + gap;
            }
            y += rowH[r] + gap;
        }
        return out;
    }

    // THE MOSAIC THAT GIVES A LITTLE (2026-10-11): at natural size two foot
    // terminals (700x500) already overflow a 1376 px laptop desktop, and a mosaic
    // that tiles at the second window is no mosaic. So before giving up, every
    // window shrinks by the same factor, in 5% steps down to `minScale`; they
    // keep their proportions to each other, and only past that floor do they
    // tile. The scale used comes back in `scaleOut`.
    inline std::optional<std::vector<SRect>> mosaicScaled(const std::vector<SRect>& sizes, const SRect& area, double gap, double minScale, double* scaleOut = nullptr) {
        for (double scale = 1.0; scale >= minScale - 1e-9; scale -= 0.05) {
            std::vector<SRect> scaled = sizes;
            for (auto& S : scaled) {
                S.w = std::round(S.w * scale);
                S.h = std::round(S.h * scale);
            }
            if (auto M = mosaic(scaled, area, gap)) {
                if (scaleOut)
                    *scaleOut = scale;
                return M;
            }
        }
        return std::nullopt;
    }

    // The tile grid: cells (gaps are Hyprland's to apply, as for any tile).
    inline std::vector<SRect> tiles(size_t n, const SRect& area) {
        std::vector<SRect> out;
        if (n == 0)
            return out;
        constexpr double WANT = 1.5; // a landscape window's shape
        size_t           bestCols = 1;
        double           bestErr  = std::numeric_limits<double>::infinity();
        for (size_t cols = 1; cols <= n; ++cols) {
            const size_t ROWS = (n + cols - 1) / cols;
            const double ERR  = std::abs(std::log((area.w / sc_double(cols)) / (area.h / sc_double(ROWS)) / WANT));
            if (ERR < bestErr) {
                bestErr  = ERR;
                bestCols = cols;
            }
        }
        const size_t ROWS = (n + bestCols - 1) / bestCols;
        // Edges rounded once and shared by the two cells either side of them, so
        // rounding never opens a 1 px seam or overlap between neighbours.
        const auto EDGE = [](double from, double span, size_t i, size_t count) { return std::round(from + span * sc_double(i) / sc_double(count)); };
        for (size_t r = 0; r < ROWS; ++r) {
            const size_t FIRST = r * bestCols;
            const size_t COUNT = std::min(bestCols, n - FIRST); // the last row may be short: it spreads out
            const double Y0 = EDGE(area.y, area.h, r, ROWS), Y1 = EDGE(area.y, area.h, r + 1, ROWS);
            for (size_t c = 0; c < COUNT; ++c) {
                const double X0 = EDGE(area.x, area.w, c, COUNT), X1 = EDGE(area.x, area.w, c + 1, COUNT);
                out.push_back(SRect{X0, Y0, X1 - X0, Y1 - Y0});
            }
        }
        return out;
    }
}
