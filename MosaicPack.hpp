#pragma once

// MOSAIC'S GEOMETRY (user, 2026-10-11; reworked 2026-10-14), pure: no Hyprland,
// so scripts/mosaic_pack_test.cpp builds it on the host. Mosaic.cpp feeds it the
// windows' sizes and the work area and places what comes back.
//
// THE MOSAIC (after GNOME's planned Mosaic, the user's reference clip): every
// window keeps its own size -- NOBODY is ever shrunk to make room -- and they
// gather into one cluster in the centre of the desktop, every window leaning
// toward the middle, with only the theme's gap between neighbours (user,
// 2026-10-14: "all bunched in the center of the screen"). When they do not
// fit, mosaic() answers nothing and the layout tiles instead.
//
// THE TILES: a dwindle tree (CDwindleTree) -- dwindle's own behaviour.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <utility>
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
    // even as possible: a greedy fill against the average, which for a handful
    // of windows lands on the optimum or next to it.
    inline std::vector<std::vector<size_t>> splitRows(const std::vector<SRect>& sizes, size_t rows, double gap) {
        std::vector<std::vector<size_t>> out;
        double                           total = 0;
        for (const auto& S : sizes)
            total += S.w;
        total += gap * sc_double(sizes.size() > rows ? sizes.size() - rows : 0);
        const double TARGET = total / sc_double(rows);
        size_t       i      = 0;
        for (size_t r = 0; r < rows && i < sizes.size(); ++r) {
            std::vector<size_t> row;
            double              width    = 0;
            const size_t        LEFTROWS = rows - r - 1;
            while (i < sizes.size()) {
                if (!row.empty() && sizes.size() - i <= LEFTROWS)
                    break;
                const double NEXT = width + (row.empty() ? 0 : gap) + sizes[i].w;
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

    // THE MOSAIC, or nothing when the windows do not fit at their own sizes.
    // Rows, each centred, the stack of rows centred, one theme gap between
    // neighbours; the row count is the one that makes the cluster most compact
    // for the screen's shape. EVERY WINDOW LEANS TOWARD THE MIDDLE (user,
    // 2026-10-14: "I mean all bunched in the center of the screen"): in a row
    // above the middle the windows sit on the row's bottom edge, below it they
    // hang from its top, in the middle row they are centred -- so a short
    // window closes in on the centre instead of leaving a gap between rows.
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

        const double       MID = sc_double(best.size() - 1) / 2.0; // the middle row's index (x.5 between two)
        std::vector<SRect> out(sizes.size());
        double             y = area.y + (area.h - totalH) / 2.0;
        for (size_t r = 0; r < best.size(); ++r) {
            double x = area.x + (area.w - rowW[r]) / 2.0;
            for (const size_t I : best[r]) {
                const double SLACK = rowH[r] - sizes[I].h;
                const double R     = sc_double(r);
                const double DY    = R < MID - 1e-9 ? SLACK : R > MID + 1e-9 ? 0.0 : SLACK / 2.0;
                out[I]             = SRect{std::round(x), std::round(y + DY), std::round(sizes[I].w), std::round(sizes[I].h)};
                x += sizes[I].w + gap;
            }
            y += rowH[r] + gap;
        }
        return out;
    }

    // THE TILES: A DWINDLE TREE (user, 2026-10-14: "dwindle hybrid mode does not
    // actually behave like dwindle"). Binary splits, each across the longer
    // side of the space it divides, each at a ratio the user moves by resizing.
    // Like Hyprland's dwindle, a window opening in the tiles splits the FOCUSED
    // window's tile in half, and a window closing gives its space to its
    // sibling. Entering the tiles from the mosaic, every window at once, starts
    // from a BALANCED tree of even splits -- the order is the mosaic's -- since
    // opening them one by one into the newest would spiral twelve windows into
    // slivers. Ids are the caller's.
    class CDwindleTree {
      public:
        struct SNode {
            long long id     = -1; // a leaf's window; -1 for a split
            int       a      = -1, b = -1, parent = -1;
            double    ratio  = 0.5;
            bool      dead   = false;
        };

        bool empty() const {
            return m_root < 0;
        }
        void clear() {
            m_nodes.clear();
            m_root = -1;
        }
        bool has(long long id) const {
            return leaf(id) >= 0;
        }
        std::vector<long long> ids() const {
            std::vector<long long> out;
            for (const auto& N : m_nodes)
                if (!N.dead && N.id >= 0)
                    out.push_back(N.id);
            return out;
        }

        void buildBalanced(const std::vector<long long>& ids) {
            clear();
            if (!ids.empty())
                m_root = build(ids, 0, ids.size(), -1);
        }

        // A window arrives: it takes half of `splitId`'s tile (dwindle's new
        // window beside the focused one), or of the newest tile if that id is
        // not in the tree.
        void insert(long long id, long long splitId) {
            if (has(id))
                return;
            if (m_root < 0) {
                m_root = make(SNode{.id = id});
                return;
            }
            int target = leaf(splitId);
            if (target < 0)
                for (int i = sc_int(m_nodes.size()) - 1; i >= 0 && target < 0; --i)
                    if (!m_nodes[i].dead && m_nodes[i].id >= 0)
                        target = i;
            const int OLDLEAF = target, PARENT = m_nodes[OLDLEAF].parent;
            const int NEWLEAF = make(SNode{.id = id});
            const int SPLIT   = make(SNode{.a = OLDLEAF, .b = NEWLEAF, .parent = PARENT});
            m_nodes[OLDLEAF].parent = SPLIT;
            m_nodes[NEWLEAF].parent = SPLIT;
            if (PARENT < 0)
                m_root = SPLIT;
            else if (m_nodes[PARENT].a == OLDLEAF)
                m_nodes[PARENT].a = SPLIT;
            else
                m_nodes[PARENT].b = SPLIT;
        }

        // A window leaves: its sibling takes the whole split.
        void remove(long long id) {
            const int L = leaf(id);
            if (L < 0)
                return;
            m_nodes[L].dead = true;
            const int P     = m_nodes[L].parent;
            if (P < 0) {
                m_root = -1;
                return;
            }
            const int SIB = m_nodes[P].a == L ? m_nodes[P].b : m_nodes[P].a;
            const int GP  = m_nodes[P].parent;
            m_nodes[P].dead      = true;
            m_nodes[SIB].parent  = GP;
            if (GP < 0)
                m_root = SIB;
            else if (m_nodes[GP].a == P)
                m_nodes[GP].a = SIB;
            else
                m_nodes[GP].b = SIB;
        }

        // Every window's cell. Splits run across the longer side of the space
        // they divide, decided at layout time, as dwindle does without
        // preserve_split. Cells never go below `minSide` while there is room.
        std::vector<std::pair<long long, SRect>> layout(const SRect& area) const {
            std::vector<std::pair<long long, SRect>> out;
            if (m_root >= 0)
                place(m_root, area, out);
            return out;
        }

        // A tile resized to `want`: the split right above it moves so the
        // tile's side across that split comes out as asked.
        void resize(long long id, double wantW, double wantH, const SRect& area) {
            const int L = leaf(id);
            if (L < 0 || m_nodes[L].parent < 0)
                return;
            const int P = m_nodes[L].parent;
            SRect     space;
            if (!spaceOf(m_root, P, area, space))
                return;
            const bool   WIDE = space.w >= space.h;
            const double SPAN = WIDE ? space.w : space.h;
            if (SPAN < 1)
                return;
            const double R      = (WIDE ? wantW : wantH) / SPAN;
            m_nodes[P].ratio    = std::clamp(m_nodes[P].a == L ? R : 1.0 - R, 0.1, 0.9);
        }

      private:
        std::vector<SNode> m_nodes;
        int                m_root = -1;

        static int sc_int(size_t v) {
            return static_cast<int>(v);
        }
        int make(SNode n) {
            m_nodes.push_back(n);
            return sc_int(m_nodes.size() - 1);
        }
        int leaf(long long id) const {
            for (size_t i = 0; i < m_nodes.size(); ++i)
                if (!m_nodes[i].dead && m_nodes[i].id == id && id >= 0)
                    return sc_int(i);
            return -1;
        }
        int build(const std::vector<long long>& ids, size_t from, size_t to, int parent) {
            if (to - from == 1) {
                const int L        = make(SNode{.id = ids[from], .parent = parent});
                return L;
            }
            const size_t MID   = from + (to - from) / 2;
            const int    SPLIT = make(SNode{.parent = parent});
            const int    A     = build(ids, from, MID, SPLIT);
            const int    B     = build(ids, MID, to, SPLIT);
            m_nodes[SPLIT].a   = A;
            m_nodes[SPLIT].b   = B;
            return SPLIT;
        }
        static void cut(const SRect& r, double ratio, SRect& a, SRect& b) {
            if (r.w >= r.h) {
                const double CUT = std::round(r.x + r.w * ratio);
                a                = SRect{r.x, r.y, CUT - r.x, r.h};
                b                = SRect{CUT, r.y, r.x + r.w - CUT, r.h};
            } else {
                const double CUT = std::round(r.y + r.h * ratio);
                a                = SRect{r.x, r.y, r.w, CUT - r.y};
                b                = SRect{r.x, CUT, r.w, r.y + r.h - CUT};
            }
        }
        void place(int n, const SRect& r, std::vector<std::pair<long long, SRect>>& out) const {
            const auto& N = m_nodes[n];
            if (N.id >= 0) {
                out.emplace_back(N.id, r);
                return;
            }
            SRect a, b;
            cut(r, N.ratio, a, b);
            place(N.a, a, out);
            place(N.b, b, out);
        }
        bool spaceOf(int n, int want, const SRect& r, SRect& out) const {
            if (n == want) {
                out = r;
                return true;
            }
            const auto& N = m_nodes[n];
            if (N.id >= 0)
                return false;
            SRect a, b;
            cut(r, N.ratio, a, b);
            return spaceOf(N.a, want, a, out) || spaceOf(N.b, want, b, out);
        }
    };

    // THE CALCULATED LIMIT (user, 2026-10-14). While one window is being resized
    // in the mosaic, every other window keeps its size; the resized one may grow
    // only as far as SOME mosaic arrangement of all of them still fits the area
    // at those sizes -- nobody shrinks to make room. `sizes` are the windows as
    // they stand (index `idx` at its size when the drag began); `want` is the
    // size the pointer asks for. The answer is the largest size toward `want`
    // that fits: first along the straight line from the start, then each axis
    // on its own (a window boxed in sideways may still grow taller), and the
    // arrangement that holds it. `reached`: `want` itself fits.
    struct SLimit {
        double             w = 0, h = 0;
        bool               reached = false;
        std::vector<SRect> placed; // the arrangement at that size (empty if even the start does not fit)
    };

    inline SLimit limitFor(const std::vector<SRect>& sizes, size_t idx, double wantW, double wantH, const SRect& area, double gap, double minSide) {
        SLimit out;
        if (idx >= sizes.size())
            return out;
        std::vector<SRect> trial = sizes;
        const auto FITS = [&](double w, double h) -> std::optional<std::vector<SRect>> {
            trial[idx].w = std::round(w);
            trial[idx].h = std::round(h);
            return mosaic(trial, area, gap);
        };
        wantW = std::max(minSide, wantW);
        wantH = std::max(minSide, wantH);
        if (auto P = FITS(wantW, wantH)) {
            out = SLimit{.w = std::round(wantW), .h = std::round(wantH), .reached = true, .placed = std::move(*P)};
            return out;
        }
        const double W0 = sizes[idx].w, H0 = sizes[idx].h;
        auto         at = FITS(W0, H0);
        double       w = W0, h = H0;
        if (!at) {
            // Even where it started does not fit (the area shrank under it):
            // shrinking toward `want` is always allowed, growing is not.
            const double SW = std::min(W0, wantW), SH = std::min(H0, wantH);
            if (auto P = FITS(SW, SH))
                return SLimit{.w = std::round(SW), .h = std::round(SH), .reached = false, .placed = std::move(*P)};
            return SLimit{.w = std::round(W0), .h = std::round(H0)};
        }
        // Along the line start -> want: feasibility only ever runs out as the
        // window grows, so the boundary is found by bisection.
        double lo = 0.0, hi = 1.0;
        for (int i = 0; i < 24; ++i) {
            const double MID = (lo + hi) / 2.0;
            if (auto P = FITS(W0 + (wantW - W0) * MID, H0 + (wantH - H0) * MID)) {
                lo = MID;
                at = std::move(P);
            } else
                hi = MID;
        }
        w = W0 + (wantW - W0) * lo;
        h = H0 + (wantH - H0) * lo;
        // ...then each axis alone, from there.
        const auto ALONG = [&](bool horizontal) {
            const double FROM = horizontal ? w : h, TO = horizontal ? wantW : wantH;
            if (std::abs(TO - FROM) < 1)
                return;
            double a = 0.0, b = 1.0;
            for (int i = 0; i < 24; ++i) {
                const double MID = (a + b) / 2.0, V = FROM + (TO - FROM) * MID;
                if (auto P = horizontal ? FITS(V, h) : FITS(w, V)) {
                    a  = MID;
                    at = std::move(P);
                } else
                    b = MID;
            }
            (horizontal ? w : h) = FROM + (TO - FROM) * a;
        };
        ALONG(false);
        ALONG(true);
        if (auto P = FITS(w, h))
            at = std::move(P);
        out = SLimit{.w = std::round(w), .h = std::round(h), .reached = false, .placed = at ? std::move(*at) : std::vector<SRect>{}};
        return out;
    }

}
