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
    // actually behave like dwindle"; again 2026-10-23: "when mosaic goes to
    // dwindle (hybrid) it should behave exactly like dwindle"). Binary splits,
    // each at a ratio the user moves by resizing, laid out as Hyprland's
    // DwindleAlgorithm lays out its nodes (src/layout/algorithm/tiled/dwindle):
    //   - a split runs across the longer side of the space it divides; with
    //     dwindle:preserve_split (STreeOpts::preserve) it keeps the direction it
    //     was made with, as dwindle's splitTop does;
    //   - a window arriving splits one tile (insertAt): the caller picks it as
    //     dwindle's addTarget picks OPENINGON, and which half it takes;
    //   - a window closing gives its space to its sibling;
    //   - resizing moves the splits dwindle's resizeTarget moves (resizeDwindle,
    //     smart_resizing or not);
    //   - togglesplit, swapsplit, rotatesplit, movetoroot and splitratio are
    //     dwindle's layoutmsgs.
    // The one thing dwindle has no counterpart for: entering the tiles from the
    // mosaic, every window at once, starts from a tree WEIGHTED by the windows'
    // mosaic areas (buildWeighted) -- the order is the mosaic's -- since opening
    // them one by one into the newest would spiral twelve windows into slivers.
    // Ids are the caller's. A split's ratio is the FIRST child's share
    // (dwindle's splitRatio / 2).
    struct STreeOpts {
        bool   preserve  = false; // dwindle:preserve_split (or smart_split / precise_mouse_move, which also keep it)
        double widthMult = 1.0;   // dwindle:split_width_multiplier
    };

    class CDwindleTree {
      public:
        struct SNode {
            long long   id     = -1; // a leaf's window; -1 for a split
            int         a      = -1, b = -1, parent = -1;
            double      ratio  = 0.5;
            bool        dead   = false;
            // A split's direction: 1 = one over the other (dwindle's splitTop),
            // 0 = side by side, -1 = not fixed yet (from its space at the next
            // layout). Only kept with STreeOpts::preserve, as dwindle keeps it.
            mutable int orient = -1;
        };

        // Dwindle clamps splitRatio to [0.1, 1.9]: a share of [0.05, 0.95].
        static constexpr double RATIO_MIN = 0.05;

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
        // not in the tree, and goes second.
        // `weight` (optional): the split takes the ratio of the old tile's
        // window to the new one, as buildWeighted does; without it, halves.
        void insert(long long id, long long splitId, const std::function<double(long long)>& weight = {}) {
            double share = 0.5;
            if (weight && has(splitId)) {
                const double WO = std::max(0.0, weight(splitId)), WN = std::max(0.0, weight(id));
                if (WO + WN > 0)
                    share = std::clamp(WN / (WO + WN), WEIGHT_MIN, 1.0 - WEIGHT_MIN);
            } else if (weight && !has(splitId) && !empty()) {
                const long long NEWEST = newestLeafId();
                const double    WO = std::max(0.0, weight(NEWEST)), WN = std::max(0.0, weight(id));
                if (WO + WN > 0)
                    share = std::clamp(WN / (WO + WN), WEIGHT_MIN, 1.0 - WEIGHT_MIN);
            }
            insertAt(id, splitId, false, share, -1);
        }

        // DWINDLE'S addTarget, once the caller has chosen OPENINGON (`nearId`;
        // the newest tile if it is not in the tree): the new leaf and the old
        // one share a new split in `nearId`'s place, the new one FIRST (left or
        // top) or second, taking `newShare` of the space. `orient` forces the
        // split's direction (dwindle's preselect / smart_split), as 1 = one
        // over the other, 0 = side by side; -1 = from the tile's shape, as
        // dwindle's NEWPARENT->splitTop = !(w > h * split_width_multiplier).
        void insertAt(long long id, long long nearId, bool newFirst, double newShare, int orient, const SRect& area = {}, const STreeOpts& o = {}) {
            if (has(id))
                return;
            if (m_root < 0) {
                m_root = make(SNode{.id = id});
                return;
            }
            int target = leaf(nearId);
            if (target < 0)
                target = leaf(newestLeafId());
            const int OLDLEAF = target, PARENT = m_nodes[OLDLEAF].parent;
            const int NEWLEAF = make(SNode{.id = id});
            const int SPLIT   = make(SNode{.parent = PARENT});
            m_nodes[SPLIT].a  = newFirst ? NEWLEAF : OLDLEAF;
            m_nodes[SPLIT].b  = newFirst ? OLDLEAF : NEWLEAF;
            const double NS   = std::clamp(newShare, RATIO_MIN, 1.0 - RATIO_MIN);
            m_nodes[SPLIT].ratio = newFirst ? NS : 1.0 - NS;
            if (orient >= 0)
                m_nodes[SPLIT].orient = orient;
            else if (area.w > 0 && area.h > 0) {
                SRect box;
                if (spaceOf(m_root, OLDLEAF, area, box, o))
                    m_nodes[SPLIT].orient = box.w > box.h * o.widthMult ? 0 : 1;
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

        // Two windows trade tiles (dwindle's swapTargets: the leaves keep their
        // place, the windows move).
        void swapIds(long long x, long long y) {
            const int LX = leaf(x), LY = leaf(y);
            if (LX < 0 || LY < 0 || LX == LY)
                return;
            std::swap(m_nodes[LX].id, m_nodes[LY].id);
        }

        // Every window's cell.
        std::vector<std::pair<long long, SRect>> layout(const SRect& area, const STreeOpts& o = {}) const {
            std::vector<std::pair<long long, SRect>> out;
            if (m_root >= 0)
                place(m_root, area, out, o);
            return out;
        }

        std::optional<SRect> cellOf(long long id, const SRect& area, const STreeOpts& o = {}) const {
            const int L = leaf(id);
            SRect     box;
            if (L < 0 || m_root < 0 || !spaceOf(m_root, L, area, box, o))
                return std::nullopt;
            return box;
        }

        // The tile at `p`, or the nearest one (dwindle's getClosestNode: the
        // least distance to the cell), skipping `skip`. -1 when there is none.
        long long leafAt(double px, double py, const SRect& area, const STreeOpts& o = {}, long long skip = -1) const {
            long long best  = -1;
            double    bestD = 0;
            for (const auto& [ID, R] : layout(area, o)) {
                if (ID == skip)
                    continue;
                const double DX = std::max({R.x - px, 0.0, px - (R.x + R.w)}), DY = std::max({R.y - py, 0.0, py - (R.y + R.h)});
                const double D  = DX * DX + DY * DY;
                if (best < 0 || D < bestD) {
                    best  = ID;
                    bestD = D;
                }
            }
            return best;
        }

        // Dwindle's moveTargetInDirection: moving straight at the split right
        // above, toward a sibling that is a single window, lands on its far
        // side (dwindle sets m_overrideDirection). dir: 0 up, 1 right, 2 down,
        // 3 left (Hyprland's Math::eDirection).
        bool towardLoneSibling(long long id, int dir, const SRect& area, const STreeOpts& o = {}) const {
            const int L = leaf(id);
            if (L < 0 || m_nodes[L].parent < 0)
                return false;
            const int  P   = m_nodes[L].parent;
            const bool TOP = splitTopOf(P, area, o);
            const bool A   = m_nodes[P].a == L;
            const int  SIB = A ? m_nodes[P].b : m_nodes[P].a;
            if (m_nodes[SIB].id < 0)
                return false;
            return (dir == 0 && TOP && !A) || (dir == 2 && TOP && A) || (dir == 3 && !TOP && !A) || (dir == 1 && !TOP && A);
        }

        // A tile resized to `want`: the split right above it moves so the
        // tile's side across that split comes out as asked.
        void resize(long long id, double wantW, double wantH, const SRect& area, const STreeOpts& o = {}) {
            const int L = leaf(id);
            if (L < 0 || m_nodes[L].parent < 0)
                return;
            const int P = m_nodes[L].parent;
            SRect     space;
            if (!spaceOf(m_root, P, area, space, o))
                return;
            const bool   WIDE = !splitTopAt(P, space, o);
            const double SPAN = WIDE ? space.w : space.h;
            if (SPAN < 1)
                return;
            const double R      = (WIDE ? wantW : wantH) / SPAN;
            m_nodes[P].ratio    = std::clamp(m_nodes[P].a == L ? R : 1.0 - R, 0.1, 0.9);
        }

        // DWINDLE'S resizeTarget: the pointer moved by (dx, dy) on the edges
        // `left/right/top/bottom` of `id`'s tile (none = a keyboard resize).
        // `smart` is dwindle:smart_resizing. `area` is the space the tiles fill,
        // which plays the monitor's work area in dwindle's STICKS tests.
        struct SEdges {
            bool left = false, right = false, top = false, bottom = false;
        };
        void resizeDwindle(long long id, double dx, double dy, SEdges e, bool smart, const SRect& area, const STreeOpts& o = {}) {
            const int L = leaf(id);
            if (L < 0)
                return;
            auto       box = boxes(area, o);
            const auto B   = box[L];
            const auto STICKS = [](double a, double b) { return std::abs(a - b) < 2.0; };
            const bool DL = STICKS(B.x, area.x), DR = STICKS(B.x + B.w, area.x + area.w), DT = STICKS(B.y, area.y), DB = STICKS(B.y + B.h, area.y + area.h);
            if (DL && DR)
                dx = 0;
            if (DT && DB)
                dy = 0;
            const auto PARENT = [&](int n) { return m_nodes[n].parent; };
            const auto SETR   = [&](int split, double r) { m_nodes[split].ratio = std::clamp(r, RATIO_MIN, 1.0 - RATIO_MIN); };

            if (smart) {
                int        vOuter = -1, vInner = -1, hOuter = -1, hInner = -1;
                const bool LEFT = e.left || DR, TOP = e.top || DB, RIGHT = e.right || DL, BOTTOM = e.bottom || DT;
                const bool NONE = !e.left && !e.right && !e.top && !e.bottom;
                for (int cur = L; cur >= 0 && PARENT(cur) >= 0; cur = PARENT(cur)) {
                    const int  P   = PARENT(cur);
                    const bool TOPS = splitTopOf(P, area, o);
                    if (vOuter < 0 && TOPS && (NONE || (TOP && m_nodes[P].b == cur) || (BOTTOM && m_nodes[P].a == cur)))
                        vOuter = cur;
                    else if (vOuter < 0 && vInner < 0 && TOPS)
                        vInner = cur;
                    else if (hOuter < 0 && !TOPS && (NONE || (LEFT && m_nodes[P].b == cur) || (RIGHT && m_nodes[P].a == cur)))
                        hOuter = cur;
                    else if (hOuter < 0 && hInner < 0 && !TOPS)
                        hInner = cur;
                    if (vOuter >= 0 && hOuter >= 0)
                        break;
                }
                if (hOuter >= 0) {
                    const int OP = PARENT(hOuter);
                    if (box[OP].w > 0)
                        SETR(OP, m_nodes[OP].ratio + dx / box[OP].w);
                    if (hInner >= 0) {
                        const double ORIGINAL = box[hInner].w;
                        box                   = boxes(area, o);
                        const int IP          = PARENT(hInner);
                        if (box[IP].w > 0)
                            SETR(IP, m_nodes[IP].a == hInner ? (ORIGINAL - dx) / box[IP].w : 1.0 - (ORIGINAL + dx) / box[IP].w);
                    }
                    box = boxes(area, o);
                }
                if (vOuter >= 0) {
                    const int OP = PARENT(vOuter);
                    if (box[OP].h > 0)
                        SETR(OP, m_nodes[OP].ratio + dy / box[OP].h);
                    if (vInner >= 0) {
                        const double ORIGINAL = box[vInner].h;
                        box                   = boxes(area, o);
                        const int IP          = PARENT(vInner);
                        if (box[IP].h > 0)
                            SETR(IP, m_nodes[IP].a == vInner ? (ORIGINAL - dy) / box[IP].h : 1.0 - (ORIGINAL + dy) / box[IP].h);
                    }
                }
                return;
            }

            const int P = PARENT(L);
            if (P < 0)
                return; // the only tile
            const bool SIDE = !splitTopOf(P, area, o);
            int        P2   = PARENT(P);
            while (P2 >= 0 && splitTopOf(P2, area, o) == !SIDE)
                P2 = PARENT(P2);
            if (P2 < 0) {
                if (SIDE && box[P].w > 0)
                    SETR(P, m_nodes[P].ratio + dx / box[P].w);
                else if (!SIDE && box[P].h > 0)
                    SETR(P, m_nodes[P].ratio + dy / box[P].h);
                return;
            }
            const int SIDEC = SIDE ? P : P2, TOPC = SIDE ? P2 : P;
            if (box[SIDEC].w > 0)
                SETR(SIDEC, m_nodes[SIDEC].ratio + dx / box[SIDEC].w);
            if (box[TOPC].h > 0)
                SETR(TOPC, m_nodes[TOPC].ratio + dy / box[TOPC].h);
        }

        // DWINDLE'S LAYOUTMSGS, on the split above `id`'s tile.
        bool toggleSplit(long long id, const SRect& area, const STreeOpts& o = {}) {
            const int L = leaf(id);
            if (L < 0 || m_nodes[L].parent < 0)
                return false;
            const int P      = m_nodes[L].parent;
            m_nodes[P].orient = splitTopOf(P, area, o) ? 0 : 1;
            return true;
        }
        bool swapSplit(long long id) {
            const int L = leaf(id);
            if (L < 0 || m_nodes[L].parent < 0)
                return false;
            auto& P = m_nodes[m_nodes[L].parent];
            std::swap(P.a, P.b);
            return true;
        }
        void rotateSplit(long long id, int angle, const SRect& area, const STreeOpts& o = {}) {
            const int L = leaf(id);
            if (L < 0 || m_nodes[L].parent < 0)
                return;
            const int  P    = m_nodes[L].parent;
            const int  N    = ((angle / 90) % 4 + 4) % 4;
            const bool TOP  = splitTopOf(P, area, o);
            bool       swap = false;
            switch (N) {
                case 1:
                    swap              = TOP;
                    m_nodes[P].orient = TOP ? 0 : 1;
                    break;
                case 2: swap = true; break;
                case 3:
                    swap              = !TOP;
                    m_nodes[P].orient = TOP ? 0 : 1;
                    break;
                default: break;
            }
            if (swap)
                std::swap(m_nodes[P].a, m_nodes[P].b);
        }
        bool moveToRoot(long long id, bool stable) {
            const int L = leaf(id);
            if (L < 0 || m_nodes[L].parent < 0 || m_nodes[m_nodes[L].parent].parent < 0)
                return false; // alone, or already a child of the root
            int ancestor = L, root = m_nodes[L].parent;
            while (m_nodes[root].parent >= 0) {
                ancestor = root;
                root     = m_nodes[root].parent;
            }
            const int SWAP = m_nodes[root].a == ancestor ? m_nodes[root].b : m_nodes[root].a;
            // The leaf and the root's other child trade places.
            const int LP = m_nodes[L].parent;
            if (m_nodes[LP].a == L)
                m_nodes[LP].a = SWAP;
            else
                m_nodes[LP].b = SWAP;
            if (m_nodes[root].a == SWAP)
                m_nodes[root].a = L;
            else
                m_nodes[root].b = L;
            m_nodes[SWAP].parent = LP;
            m_nodes[L].parent    = root;
            if (stable)
                std::swap(m_nodes[root].a, m_nodes[root].b);
            return true;
        }
        // dwindle's "splitratio <delta> [exact]", in dwindle's 0..2 units.
        bool splitRatio(long long id, double value, bool exact) {
            const int L = leaf(id);
            if (L < 0 || m_nodes[L].parent < 0)
                return false;
            const int P = m_nodes[L].parent;
            m_nodes[P].ratio = std::clamp(exact ? value / 2.0 : m_nodes[P].ratio + value / 2.0, RATIO_MIN, 1.0 - RATIO_MIN);
            return true;
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
        long long newestLeafId() const {
            for (int i = sc_int(m_nodes.size()) - 1; i >= 0; --i)
                if (!m_nodes[i].dead && m_nodes[i].id >= 0)
                    return m_nodes[i].id;
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
        // A split's direction in the space `r` it divides: without
        // preserve_split, dwindle's recalcSizePosRecursive takes it from that
        // space every time (splitTop = h * mult > w); with it, the direction is
        // the one the split was made with -- fixed here, from its first space,
        // when nothing set it.
        bool splitTopAt(int n, const SRect& r, const STreeOpts& o) const {
            const auto& N = m_nodes[n];
            if (!o.preserve)
                return r.h * o.widthMult > r.w;
            if (N.orient < 0)
                N.orient = r.w > r.h * o.widthMult ? 0 : 1;
            return N.orient == 1;
        }
        bool splitTopOf(int n, const SRect& area, const STreeOpts& o) const {
            SRect r;
            if (!spaceOf(m_root, n, area, r, o))
                return false;
            return splitTopAt(n, r, o);
        }
        void cut(int n, const SRect& r, SRect& a, SRect& b, const STreeOpts& o) const {
            const double RATIO = m_nodes[n].ratio;
            if (!splitTopAt(n, r, o)) {
                const double CUT = std::round(r.x + r.w * RATIO);
                a                = SRect{r.x, r.y, CUT - r.x, r.h};
                b                = SRect{CUT, r.y, r.x + r.w - CUT, r.h};
            } else {
                const double CUT = std::round(r.y + r.h * RATIO);
                a                = SRect{r.x, r.y, r.w, CUT - r.y};
                b                = SRect{r.x, CUT, r.w, r.y + r.h - CUT};
            }
        }
        void place(int n, const SRect& r, std::vector<std::pair<long long, SRect>>& out, const STreeOpts& o) const {
            const auto& N = m_nodes[n];
            if (N.id >= 0) {
                out.emplace_back(N.id, r);
                return;
            }
            SRect a, b;
            cut(n, r, a, b, o);
            place(N.a, a, out, o);
            place(N.b, b, out, o);
        }
        bool spaceOf(int n, int want, const SRect& r, SRect& out, const STreeOpts& o) const {
            if (n == want) {
                out = r;
                return true;
            }
            const auto& N = m_nodes[n];
            if (N.id >= 0)
                return false;
            SRect a, b;
            cut(n, r, a, b, o);
            return spaceOf(N.a, want, a, out, o) || spaceOf(N.b, want, b, out, o);
        }
        // Every node's box (dwindle's node->box), by node index.
        std::vector<SRect> boxes(const SRect& area, const STreeOpts& o) const {
            std::vector<SRect> out(m_nodes.size());
            if (m_root >= 0)
                fill(m_root, area, out, o);
            return out;
        }
        void fill(int n, const SRect& r, std::vector<SRect>& out, const STreeOpts& o) const {
            out[n] = r;
            if (m_nodes[n].id >= 0)
                return;
            SRect a, b;
            cut(n, r, a, b, o);
            fill(m_nodes[n].a, a, out, o);
            fill(m_nodes[n].b, b, out, o);
        }
    };

}

// THE PACKER: pack(), packEased(), fitLayout(), fitsEased(), limitFor(), PackHint, SLimit.
#define MOSAICPACK_HAVE_SRECT 1
#include "MosaicPackFinal.hpp"
