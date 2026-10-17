#pragma once

// MOSAIC'S GEOMETRY (user, 2026-10-11; reworked 2026-10-14; the packer rebuilt
// 2026-10-16), pure: no Hyprland, so scripts/mosaic_pack_test.cpp builds it on
// the host. Mosaic.cpp feeds the windows' sizes and the work area to
// MosaicPackFinal.hpp's pack() / packEased() and places what comes back; this
// file keeps the shared rect, the size each window arrives with, and the tiles.
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
#include <cstdint>
#include <functional>
#include <limits>
#include <numeric>
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
    // caller can tell a window it already sized from a new one.
    struct SSize {
        double w = 0, h = 0;
    };
    //
    // TRUSTED (user, 2026-10-15): only a size the app or the user chose may
    // become its app's size on close (Mosaic.cpp onWindowClose). The app's own
    // size at a fresh open, its stored app size, and a kept size that was
    // trusted are; a tile it inherited (desired / floating once laid out
    // elsewhere -- dwindle, the tiles, a plugin reload) and the fallback are
    // not. Mosaic.cpp also untrusts a size from resizing a tile, and trusts
    // the user's own mosaic resize.
    struct SNaturalInputs {
        std::optional<SSize> kept, app, desired;
        SSize                floating, fallback;
        bool                 fresh       = true;
        bool                 keptTrusted = true;
    };
    struct SNatural {
        SSize size;
        bool  kept    = false;
        bool  trusted = false;
    };
    inline SNatural chooseNatural(const SNaturalInputs& in, double minSide) {
        const auto VALID = [&](const std::optional<SSize>& s) { return s && s->w >= minSide && s->h >= minSide; };
        if (VALID(in.kept))
            return {*in.kept, true, in.keptTrusted};
        if (VALID(in.app))
            return {*in.app, false, true};
        if (!in.fresh && VALID(in.floating))
            return {in.floating, false, false};
        if (VALID(in.desired))
            return {*in.desired, false, in.fresh};
        if (VALID(in.floating))
            return {in.floating, false, false};
        return {in.fallback, false, false};
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

        // THE TILES LOOK LIKE THE MOSAIC (user, 2026-10-15: "dwindle
        // arrangement of tiles has to as closely as possible reflect pure
        // mosaic sizes"). Each split divides the windows where their summed
        // weight (Mosaic.cpp: each window's mosaic area) is closest to half,
        // at the ratio of the two sides' weights -- so a big window keeps a
        // big tile and a small one a small tile, in the mosaic's order. Kept
        // within [WEIGHT_MIN, 1 - WEIGHT_MIN] so no tile becomes a sliver.
        static constexpr double WEIGHT_MIN = 0.15;
        void buildWeighted(const std::vector<long long>& ids, const std::vector<double>& weights) {
            clear();
            if (ids.empty())
                return;
            if (weights.size() != ids.size()) {
                buildBalanced(ids);
                return;
            }
            m_root = buildW(ids, weights, 0, ids.size(), -1);
        }

        // A window arrives: it takes half of `splitId`'s tile (dwindle's new
        // window beside the focused one), or of the newest tile if that id is
        // not in the tree.
        // `weight` (optional): the split takes the ratio of the old tile's
        // window to the new one, as buildWeighted does; without it, halves.
        void insert(long long id, long long splitId, const std::function<double(long long)>& weight = {}) {
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
            if (weight) {
                const double WO = std::max(0.0, weight(m_nodes[OLDLEAF].id)), WN = std::max(0.0, weight(id));
                if (WO + WN > 0)
                    m_nodes[SPLIT].ratio = std::clamp(WO / (WO + WN), WEIGHT_MIN, 1.0 - WEIGHT_MIN);
            }
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
        int buildW(const std::vector<long long>& ids, const std::vector<double>& w, size_t from, size_t to, int parent) {
            if (to - from == 1)
                return make(SNode{.id = ids[from], .parent = parent});
            double total = 0;
            for (size_t i = from; i < to; ++i)
                total += std::max(0.0, w[i]);
            // The cut nearest half the weight, one window at least each side.
            size_t mid = from + 1;
            double run = std::max(0.0, w[from]), best = std::abs(run - total / 2.0);
            for (size_t m = from + 2; m < to; ++m) {
                run += std::max(0.0, w[m - 1]);
                if (const double D = std::abs(run - total / 2.0); D < best - 1e-9) {
                    best = D;
                    mid  = m;
                }
            }
            double left = 0;
            for (size_t i = from; i < mid; ++i)
                left += std::max(0.0, w[i]);
            const int SPLIT     = make(SNode{.parent = parent});
            m_nodes[SPLIT].ratio = total > 0 ? std::clamp(left / total, WEIGHT_MIN, 1.0 - WEIGHT_MIN) : 0.5;
            const int A         = buildW(ids, w, from, mid, SPLIT);
            const int B         = buildW(ids, w, mid, to, SPLIT);
            m_nodes[SPLIT].a    = A;
            m_nodes[SPLIT].b    = B;
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

}

// THE PACKER: pack(), packEased(), fitLayout(), fitsEased(), limitFor(), PackHint, SLimit.
#define MOSAICPACK_HAVE_SRECT 1
#include "MosaicPackFinal.hpp"
