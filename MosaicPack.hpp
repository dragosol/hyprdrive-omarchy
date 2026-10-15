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

    // THE FIT, EXACTLY (2026-10-15). splitRows() tries ONE split per row count,
    // balanced by width, and missed arrangements that do fit: kitty 900x600,
    // calculator 380x636, text editor 595x442 and a 604x374 window fit as
    // [three in a row] + [one] in 1900x1060, but the balanced splits were
    // [2]+[2] (1088 high) and [1]+[2]+[1] -- so a desktop whose windows had been
    // shrunk back to fitting stayed in the tiles. This is the fallback when
    // none of the balanced splits fits: for every row count, the consecutive
    // split (the windows keep their order) with the LEAST total height whose
    // rows all fit the width -- a dynamic programme, so it is exact for that
    // order -- and of those the most compact for the screen's shape, scored as
    // mosaic() scores. Empty: no split in this order fits.
    inline std::vector<std::vector<size_t>> fittingRows(const std::vector<SRect>& sizes, const SRect& area, double gap) {
        const size_t N   = sizes.size();
        const double INF = std::numeric_limits<double>::infinity();
        if (N == 0)
            return {};
        // width[i][j], height[i][j]: windows i..j-1 as one row.
        std::vector<std::vector<double>> rowW(N + 1, std::vector<double>(N + 1, INF)), rowH(N + 1, std::vector<double>(N + 1, 0.0));
        for (size_t i = 0; i < N; ++i) {
            double w = 0, h = 0;
            for (size_t j = i + 1; j <= N; ++j) {
                w += sizes[j - 1].w + (j - 1 > i ? gap : 0);
                h = std::max(h, sizes[j - 1].h);
                rowW[i][j] = w;
                rowH[i][j] = h;
            }
        }
        // best[k][j]: least summed row height for the first j windows in k rows.
        std::vector<std::vector<double>> best(N + 1, std::vector<double>(N + 1, INF));
        std::vector<std::vector<size_t>> from(N + 1, std::vector<size_t>(N + 1, 0));
        best[0][0] = 0;
        for (size_t k = 1; k <= N; ++k)
            for (size_t j = k; j <= N; ++j)
                for (size_t i = k - 1; i < j; ++i) {
                    if (best[k - 1][i] == INF || rowW[i][j] > area.w)
                        continue;
                    const double H = best[k - 1][i] + rowH[i][j];
                    if (H < best[k][j]) {
                        best[k][j] = H;
                        from[k][j] = i;
                    }
                }
        double                           bestScore = INF;
        std::vector<std::vector<size_t>> out;
        for (size_t k = 1; k <= N; ++k) {
            if (best[k][N] == INF)
                continue;
            const double BH = best[k][N] + gap * sc_double(k - 1);
            if (BH > area.h)
                continue;
            std::vector<std::vector<size_t>> rows(k);
            double                           bw = 0;
            for (size_t r = k, j = N; r > 0; --r) {
                const size_t I = from[r][j];
                for (size_t m = I; m < j; ++m)
                    rows[r - 1].push_back(m);
                bw = std::max(bw, rowW[I][j]);
                j  = I;
            }
            const double SCORE = std::max(bw / area.w, BH / area.h);
            if (SCORE < bestScore - 1e-9) {
                bestScore = SCORE;
                out       = std::move(rows);
            }
        }
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
            best = fittingRows(sizes, area, gap);
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

    // THE SIZE A WINDOW ARRIVES WITH, in order of precedence (Mosaic.cpp feeds
    // it; the sources are Hyprland's):
    //   kept     -- this window's own mosaic size from earlier in the session
    //               (2026-10-15: a desktop toggled to dwindle and back, or a
    //               window moved away and back, is the SAME window, and comes
    //               back at its mosaic size, not at the tile it just left);
    //   app      -- the size its app last closed at in a mosaic (mosaic-sizes);
    //   floating -- moved in (not fresh): its remembered floating size;
    //   desired  -- the client's own size: its choice when it is NEW, but
    //               only its current tile once it has been laid out elsewhere;
    //   floating -- (fresh) still better than nothing;
    //   fallback.
    // A size counts only with both sides >= minSide. `kept` is reported so the
    // caller never shrinks such a window as a newcomer.
    struct SSize {
        double w = 0, h = 0;
    };
    struct SNaturalInputs {
        std::optional<SSize> kept, app, desired;
        SSize                floating, fallback;
        bool                 fresh = true;
    };
    struct SNatural {
        SSize size;
        bool  kept = false;
    };
    inline SNatural chooseNatural(const SNaturalInputs& in, double minSide) {
        const auto VALID = [&](const std::optional<SSize>& s) { return s && s->w >= minSide && s->h >= minSide; };
        if (VALID(in.kept))
            return {*in.kept, true};
        if (VALID(in.app))
            return {*in.app, false};
        if (!in.fresh && VALID(in.floating))
            return {in.floating, false};
        if (VALID(in.desired))
            return {*in.desired, false};
        if (VALID(in.floating))
            return {in.floating, false};
        return {in.fallback, false};
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
