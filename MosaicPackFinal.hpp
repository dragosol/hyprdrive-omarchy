#pragma once
// HYPRDRIVE MOSAIC PACKER -- the final pick of the 2026-10-16 redesign.
//
// Every window keeps its OWN size; they gather into ONE tight cluster in the
// centre of the work area with only the theme gap between neighbours; when no
// arrangement fits, the answer is nullopt and the desktop tiles.
//
//   pack(sizes, area, gap, hint) -> one rect per input window, IN INPUT ORDER,
//   w/h exactly as given, integer x/y, all inside `area`, every pair >= gap
//   apart -- or nullopt. `hint` (optional): window `idx` was Super+dragged and
//   dropped at the global point (x, y). Deterministic, no clocks, no Hyprland.
//
// HOW (two independent searches, then an exact contract check):
//   1. Box  (the "boxpack" design, the winner): MaxRects / strip / any-order
//      guillotine packings into the smallest screen-shaped box, compacted
//      toward the centre, judged exactly (one cluster, enclosed holes,
//      pockets, screen share, fill, centring, elongation), then polished by
//      relocating and swapping windows. Drops pin the dropped window to the
//      side it was dropped on. With the user's one-sided squareness rule: 3+
//      windows never come out as a strip longer than the screen's own shape.
//   2. Beam (the "beamcontact" design, a fit fallback only): when Box finds
//      nothing, the cluster is grown from the centre one touching window at a
//      time (then inside the area from a corner). The two searches miss
//      different tight sets; together they missed 1 of 1898 fittable random
//      sets where each alone missed 8-9.
//   3. The contract, checked exactly (sizes, integer positions, inside the
//      area, the gap). A layout that fails it is never returned.

// ==========================================================================
// PART 1 -- Box: the winner ("boxpack"), + the strip rule, the lean pass, the fast fit test
// ==========================================================================
// BOXPACK -- hyprdrive's MOSAIC packer, rebuilt from scratch (2026-10-16).
//
// Every window keeps its own size; they gather into ONE tight cluster in the
// centre of the area, only the theme gap between neighbours. Nothing fits ->
// nullopt (the desktop tiles). How:
//
//  1. CANDIDATES -- pack into the smallest screen-shaped box:
//     * MaxRects (best short side, bottom-left, contact point, best area) over
//       sorted + seeded-shuffle window orders; for each, binary-search the
//       smallest box with the area's aspect (and slightly narrower / shorter
//       ones). Windows are inflated by the gap, so neighbours end up >= gap apart.
//     * strips: a fixed width filled top-down / a fixed height filled left-right.
//     * guillotine: every way to cut the windows into two groups side by side or
//       stacked, recursively, any order -- over COUNTS per distinct size, so ten
//       terminals cost no more than one (Pareto front of block shapes, capped).
//  2. COMPACT -- centre each packing; slide every window toward the cluster
//     centre (or one side) until blocked, so no slack is left between windows.
//  3. JUDGE -- what makes a mosaic look right, exactly as the bench scores it:
//     one connected cluster, no enclosed holes, no pockets (empty space with
//     windows on both sides), small screen share, high fill, centred.
//  4. POLISH the best few: RELOCATE single windows to any spot inside the
//     cluster where they touch / line up with a neighbour, SWAP two windows
//     (same size: free), compact again -- while it improves. Sets whose best
//     layout still has a hole / split / pocket get a second, harder look.
//  A small reading-order term keeps the windows' own order where it is free
//  (the caller keeps them in the last layout's reading order: less churn when
//  a window opens or closes); mirror images are picked the same way.
//
// Drops (hint): the side is read off the drop point against where the cluster
// sits; the dropped window is pinned to the free region on that side of the box
// (or at the point, for a drop in the middle) and the rest packed around it;
// mirror images and swaps of the plain packings compete. The objective adds the
// bench's drop terms: wrong side, distance to the point, fill lost vs no drop.
//
// Deterministic (iteration budgets, no clocks, seeded shuffles), integer x/y,
// header-only C++23, no Hyprland deps. Fedora (-O2): max ~1.1 ms for n <= 8,
// ~9 ms for n <= 20 on the bench. Effort knobs: the BOXPACK_* macros below.
// BOXPACK_ASPECT_W / _TOL (8 / 0, on in the final packer): 3+ windows are not
// laid out as ONE ROW longer than the screen's own shape (the user's
// three-in-a-row complaint) when a grouped cluster exists; see stripPenalty().
#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <optional>
#include <vector>

// effort knobs (all deterministic iteration counts, no clocks)
#ifndef BOXPACK_ORDER_W
#define BOXPACK_ORDER_W 0.1
#endif
#ifndef BOXPACK_GUILL_MAX
#define BOXPACK_GUILL_MAX 20
#endif
#ifndef BOXPACK_GUILL_CAP6
#define BOXPACK_GUILL_CAP6 12
#endif
#ifndef BOXPACK_GUILL_CAP7
#define BOXPACK_GUILL_CAP7 8
#endif
#ifndef BOXPACK_GUILL_CAP8
#define BOXPACK_GUILL_CAP8 6
#endif
#ifndef BOXPACK_GUILL_STATES
#define BOXPACK_GUILL_STATES 1200
#endif
#ifndef BOXPACK_GUILL_PAIRS
#define BOXPACK_GUILL_PAIRS 25000
#endif
#ifndef BOXPACK_GUILL_PAIRS_WIDE
#define BOXPACK_GUILL_PAIRS_WIDE 8000
#endif
#ifndef BOXPACK_GUILL_CAP_WIDE
#define BOXPACK_GUILL_CAP_WIDE 10
#endif
#ifndef BOXPACK_MARGIN
#define BOXPACK_MARGIN 0.07
#endif
#ifndef BOXPACK_POS_SMALL
#define BOXPACK_POS_SMALL 100000
#endif
#ifndef BOXPACK_POS_BIG
#define BOXPACK_POS_BIG 400000
#endif
#ifndef BOXPACK_GUILL_ROOTS
#define BOXPACK_GUILL_ROOTS 8
#endif
#ifndef BOXPACK_GUILL_CAP_BIG
#define BOXPACK_GUILL_CAP_BIG 10
#endif
#ifndef BOXPACK_ASPECT_W
#define BOXPACK_ASPECT_W 8.0
#endif
#ifndef BOXPACK_ASPECT_TOL
#define BOXPACK_ASPECT_TOL 0.0
#endif
#ifndef BOXPACK_HARD_HOLE
#define BOXPACK_HARD_HOLE 0.15
#endif
#ifndef BOXPACK_SHUF2_SMALL
#define BOXPACK_SHUF2_SMALL 24
#endif
#ifndef BOXPACK_SHUF2_BIG
#define BOXPACK_SHUF2_BIG 10
#endif
#ifndef BOXPACK_POLISH2_TOP
#define BOXPACK_POLISH2_TOP 5
#endif
#ifndef BOXPACK_JUDGE2_SMALL
#define BOXPACK_JUDGE2_SMALL 3000
#endif
#ifndef BOXPACK_JUDGE2_BIG
#define BOXPACK_JUDGE2_BIG 1600
#endif
#ifndef BOXPACK_SHUF_SMALL
#define BOXPACK_SHUF_SMALL 10
#endif
#ifndef BOXPACK_SHUF_BIG
#define BOXPACK_SHUF_BIG 4
#endif
#ifndef BOXPACK_SHAPES_BIG
#define BOXPACK_SHAPES_BIG 3
#endif
#ifndef BOXPACK_DROP_ORDERS
#define BOXPACK_DROP_ORDERS 3
#endif
#ifndef BOXPACK_POLISH_TOP
#define BOXPACK_POLISH_TOP 3
#endif
#ifndef BOXPACK_JUDGE_SMALL
#define BOXPACK_JUDGE_SMALL 1500
#endif
#ifndef BOXPACK_JUDGE_BIG
#define BOXPACK_JUDGE_BIG 700
#endif
#ifndef BOXPACK_STRIP_SMALL
#define BOXPACK_STRIP_SMALL 10
#endif
#ifndef BOXPACK_STRIP_BIG
#define BOXPACK_STRIP_BIG 6
#endif
#ifndef BOXPACK_STRIP_STEPS
#define BOXPACK_STRIP_STEPS 10
#endif
#ifndef BOXPACK_POOL_SMALL
#define BOXPACK_POOL_SMALL 80
#endif
#ifndef BOXPACK_POOL_BIG
#define BOXPACK_POOL_BIG 50
#endif

namespace ScrollOverview::MosaicPack::Box {

    struct SRect {
        double x = 0, y = 0, w = 0, h = 0;
    };
    struct PackHint {
        size_t idx = 0;
        double x = 0, y = 0;
    };

    namespace BoxPack {

        struct IR {
            int x = 0, y = 0, w = 0, h = 0;
        };
        enum class EHeur : uint8_t {
            BSSF, // best short side fit
            BL,   // bottom-left (top-left on screen)
            CP,   // contact point
            BAF,  // best area fit
            LB,   // left-bottom: leftmost first (fills a height-limited strip)
        };

        struct Ctx {
            int                 N = 0, G = 0;          // windows, gap (whole px)
            int                 AX = 0, AY = 0, AW = 0, AH = 0;
            double              gap = 0, areaW = 0, areaH = 0;
            std::vector<int>    W, H;                  // window sizes, whole px (ceil)
            int                 maxW = 0, maxH = 0;
            // scratch
            std::vector<IR>     fr, nx, used;
            std::vector<double> xs, ys;
            std::vector<char>   occ, out, pk, seen;
            std::vector<int>    st, par, sufW, sufH;
            long long           runs = 0;
            bool                slowHoles = false; // tests: the cell-by-cell reference
            long                posBudget = 0;     // relocation spots left to look at (the whole call)
            double              area0 = 0;         // the windows' own area
            struct Params {
                int shufSmall = BOXPACK_SHUF_SMALL, shufBig = BOXPACK_SHUF_BIG;       // extra shuffled orders (n <= 8 / bigger)
                int shapesSmall = 5, shapesBig = BOXPACK_SHAPES_BIG;                  // box shapes tried
                int dropOrders = BOXPACK_DROP_ORDERS;                                 // orders for the pinned drop packings
                int polishTop = BOXPACK_POLISH_TOP;                                   // candidates polished
                int judgeSmall = BOXPACK_JUDGE_SMALL, judgeBig = BOXPACK_JUDGE_BIG;   // judge calls for relocation
                int stripOrdersSmall = BOXPACK_STRIP_SMALL, stripOrdersBig = BOXPACK_STRIP_BIG, stripSteps = BOXPACK_STRIP_STEPS; // strip sweeps
                int judgePoolSmall = BOXPACK_POOL_SMALL, judgePoolBig = BOXPACK_POOL_BIG; // packings judged in full
                // the second look, for sets whose best layout still has a hole / split / pocket
                double hardHole = BOXPACK_HARD_HOLE;
                int    shuf2Small = BOXPACK_SHUF2_SMALL, shuf2Big = BOXPACK_SHUF2_BIG, strip2Small = 16, strip2Big = 10;
                int    polish2Top = BOXPACK_POLISH2_TOP, judge2Small = BOXPACK_JUDGE2_SMALL, judge2Big = BOXPACK_JUDGE2_BIG;
                double orderWeight = BOXPACK_ORDER_W; // reading order kept (stability) vs the score
                int    guillMax = BOXPACK_GUILL_MAX, guillCap6 = BOXPACK_GUILL_CAP6, guillCap7 = BOXPACK_GUILL_CAP7, guillCap8 = BOXPACK_GUILL_CAP8; // guillotine search
                long   guillStates = BOXPACK_GUILL_STATES, guillPairs = BOXPACK_GUILL_PAIRS;
                long   guillPairsWide = BOXPACK_GUILL_PAIRS_WIDE; // past this many cuts, the front is capped at guillCapWide
                int    guillCapWide = BOXPACK_GUILL_CAP_WIDE;
                int    guillRoots = BOXPACK_GUILL_ROOTS;
                int    guillCapBig = BOXPACK_GUILL_CAP_BIG; // n > 8 (repeated sizes)
                double aspectWeight = BOXPACK_ASPECT_W, aspectTol = BOXPACK_ASPECT_TOL; // the one-long-row rule (stripPenalty)
                double margin = BOXPACK_MARGIN;       // box search: skip what cannot get within this share of the best
                long   posSmall = BOXPACK_POS_SMALL, posBig = BOXPACK_POS_BIG; // relocation spots per call
            } prm;
        };

        // ---------------------------------------------------------- MaxRects
        // Pack windows (in `order`) into a BW x BH box: positions relative to the
        // box. Internally everything is inflated by G (bin BW+G x BH+G, items w+G x
        // h+G), so any two windows end up >= G apart and inside BW x BH.
        // pinIdx >= 0: that window is placed first at `pin` (box coordinates).
        inline bool maxRects(Ctx& c, const int* order, int BW, int BH, EHeur heur, std::vector<IR>& res, int pinIdx = -1, IR pin = {}) {
            ++c.runs;
            const int G = c.G, FW = BW + G, FH = BH + G;
            auto&     fr = c.fr;
            auto&     nx = c.nx;
            auto&     used = c.used;
            fr.clear(), used.clear();
            fr.push_back({0, 0, FW, FH});
            res.assign(static_cast<size_t>(c.N), IR{});
            // the smallest remaining inflated item: free rects narrower/shorter are dead
            const auto PLACE = [&](const IR& P, int minIW, int minIH) {
                nx.clear();
                for (const IR& F : fr) {
                    if (P.x >= F.x + F.w || P.x + P.w <= F.x || P.y >= F.y + F.h || P.y + P.h <= F.y) {
                        nx.push_back(F);
                        continue;
                    }
                    if (P.x > F.x)
                        nx.push_back({F.x, F.y, P.x - F.x, F.h});
                    if (P.x + P.w < F.x + F.w)
                        nx.push_back({P.x + P.w, F.y, F.x + F.w - P.x - P.w, F.h});
                    if (P.y > F.y)
                        nx.push_back({F.x, F.y, F.w, P.y - F.y});
                    if (P.y + P.h < F.y + F.h)
                        nx.push_back({F.x, P.y + P.h, F.w, F.y + F.h - P.y - P.h});
                }
                fr.clear();
                const size_t M = nx.size();
                for (size_t a = 0; a < M; ++a) {
                    const IR& A = nx[a];
                    if (A.w < minIW || A.h < minIH)
                        continue;
                    bool contained = false;
                    for (size_t b = 0; b < M && !contained; ++b) {
                        if (a == b)
                            continue;
                        const IR& B = nx[b];
                        if (A.x >= B.x && A.y >= B.y && A.x + A.w <= B.x + B.w && A.y + A.h <= B.y + B.h)
                            contained = !(A.x == B.x && A.y == B.y && A.w == B.w && A.h == B.h) || b < a;
                    }
                    if (!contained)
                        fr.push_back(A);
                }
                used.push_back(P);
            };
            // suffix minima of the inflated sizes along the order
            const int N = c.N;
            auto&     sufW = c.sufW;
            auto&     sufH = c.sufH;
            sufW.resize(static_cast<size_t>(N) + 1), sufH.resize(static_cast<size_t>(N) + 1);
            sufW[N] = sufH[N] = 0;
            {
                int mw = INT_MAX, mh = INT_MAX;
                for (int k = N - 1; k >= 0; --k) {
                    const int I = order[k];
                    if (I != pinIdx)
                        mw = std::min(mw, c.W[I] + G), mh = std::min(mh, c.H[I] + G);
                    sufW[k] = mw, sufH[k] = mh;
                }
            }
            const auto SUF = [&](int k, const std::vector<int>& s) { return s[static_cast<size_t>(k)]; };
            if (pinIdx >= 0) {
                const IR P{pin.x, pin.y, c.W[pinIdx] + G, c.H[pinIdx] + G};
                if (P.x < 0 || P.y < 0 || P.x + P.w > FW || P.y + P.h > FH)
                    return false;
                PLACE(P, SUF(0, sufW), SUF(0, sufH));
                res[static_cast<size_t>(pinIdx)] = {pin.x, pin.y, c.W[pinIdx], c.H[pinIdx]};
            }
            for (int k = 0; k < N; ++k) {
                const int I = order[k];
                if (I == pinIdx)
                    continue;
                const int IW = c.W[I] + G, IH = c.H[I] + G;
                long long b1 = LLONG_MAX, b2 = LLONG_MAX;
                int       bx = 0, by = 0;
                bool      found = false;
                for (const IR& F : fr) {
                    if (IW > F.w || IH > F.h)
                        continue;
                    long long s1 = 0, s2 = 0;
                    switch (heur) {
                        case EHeur::BSSF: {
                            const int LW = F.w - IW, LH = F.h - IH;
                            s1 = std::min(LW, LH), s2 = std::max(LW, LH);
                            break;
                        }
                        case EHeur::BL: s1 = F.y + IH, s2 = F.x; break;
                        case EHeur::LB: s1 = F.x + IW, s2 = F.y; break;
                        case EHeur::BAF: s1 = static_cast<long long>(F.w) * F.h - static_cast<long long>(IW) * IH, s2 = std::min(F.w - IW, F.h - IH); break;
                        case EHeur::CP: {
                            long long  ct = 0;
                            const int X0 = F.x, Y0 = F.y, X1 = F.x + IW, Y1 = F.y + IH;
                            if (X0 == 0 || X1 == FW)
                                ct += IH;
                            if (Y0 == 0 || Y1 == FH)
                                ct += IW;
                            for (const IR& U : used) {
                                if (U.x == X1 || U.x + U.w == X0)
                                    ct += std::max(0, std::min(U.y + U.h, Y1) - std::max(U.y, Y0));
                                if (U.y == Y1 || U.y + U.h == Y0)
                                    ct += std::max(0, std::min(U.x + U.w, X1) - std::max(U.x, X0));
                            }
                            s1 = -ct, s2 = F.y + F.x;
                            break;
                        }
                    }
                    if (s1 < b1 || (s1 == b1 && s2 < b2)) {
                        b1 = s1, b2 = s2, bx = F.x, by = F.y;
                        found = true;
                    }
                }
                if (!found)
                    return false;
                PLACE(IR{bx, by, IW, IH}, SUF(k + 1, sufW), SUF(k + 1, sufH));
                res[static_cast<size_t>(I)] = {bx, by, c.W[I], c.H[I]};
            }
            return true;
        }

        // ---------------------------------------------------------- judging
        struct Judged {
            double score = 1e300, S = 0, fill = 0, hole = 0; // hole: the holes + pockets part of the score
            int    comps = 0;
        };
        inline IR bbox(const std::vector<IR>& r) {
            int x0 = INT_MAX, y0 = INT_MAX, x1 = INT_MIN, y1 = INT_MIN;
            for (const IR& R : r)
                x0 = std::min(x0, R.x), y0 = std::min(y0, R.y), x1 = std::max(x1, R.x + R.w), y1 = std::max(y1, R.y + R.h);
            return {x0, y0, x1 - x0, y1 - y0};
        }
        inline int sepX(const IR& a, const IR& b) {
            return std::max(b.x - (a.x + a.w), a.x - (b.x + b.w));
        }
        inline int sepY(const IR& a, const IR& b) {
            return std::max(b.y - (a.y + a.h), a.y - (b.y + b.h));
        }

        // What a mosaic should look like, as one number (lower is better):
        // a split cluster 25+, any real enclosed hole 6 + 30 x its share,
        // pockets 10 x their share, 5 x screen share, 2 x empty share, centring.
        // Holes and pockets on 64-bit row masks (the grid of inflated edges has at
        // most 2n-1 columns): the same numbers as the cell-by-cell version.
        inline double holesFast(Ctx& c, const std::vector<IR>& r, double D, int NX, int NY, double BA) {
            const auto& xs = c.xs;
            const auto& ys = c.ys;
            uint64_t    occ[64], out[64], pk[64], encl[64], upc[64];
            const auto  RANGE = [](int a, int b) -> uint64_t { // bits [a, b)
                if (b <= a)
                    return 0;
                const uint64_t HI = b >= 64 ? ~0ULL : ((1ULL << b) - 1);
                const uint64_t LO = a >= 64 ? ~0ULL : ((1ULL << a) - 1);
                return HI & ~LO;
            };
            const uint64_t FULL = RANGE(0, NX);
            for (int j = 0; j < NY; ++j)
                occ[j] = 0;
            for (const IR& R : r) {
                const int      I0 = static_cast<int>(std::lower_bound(xs.begin(), xs.end(), R.x - D) - xs.begin());
                const int      I1 = static_cast<int>(std::lower_bound(xs.begin(), xs.end(), R.x + R.w + D) - xs.begin());
                const int      J0 = static_cast<int>(std::lower_bound(ys.begin(), ys.end(), R.y - D) - ys.begin());
                const int      J1 = static_cast<int>(std::lower_bound(ys.begin(), ys.end(), R.y + R.h + D) - ys.begin());
                const uint64_t M  = RANGE(I0, I1);
                for (int j = J0; j < J1; ++j)
                    occ[j] |= M;
            }
            const auto SPREAD = [](uint64_t x, uint64_t f) { // grow x along the row inside f
                for (;;) {
                    const uint64_t Y = (x | (x << 1) | (x >> 1)) & f;
                    if (Y == x)
                        return x;
                    x = Y;
                }
            };
            // flood `m` (seeded) inside the free mask `f`, rows linked vertically
            const auto FLOOD = [&](uint64_t* m, const uint64_t* f) {
                for (bool changed = true; changed;) {
                    changed = false;
                    for (int pass = 0; pass < 2; ++pass)
                        for (int q = 0; q < NY; ++q) {
                            const int j = pass == 0 ? q : NY - 1 - q;
                            uint64_t  x = (m[j] | (j > 0 ? m[j - 1] : 0) | (j < NY - 1 ? m[j + 1] : 0)) & f[j];
                            x           = SPREAD(x, f[j]);
                            if (x != m[j])
                                m[j] = x, changed = true;
                        }
                }
            };
            uint64_t freeM[64];
            const uint64_t EDGE = (1ULL << 0) | (1ULL << (NX - 1));
            bool           anyEncl = false;
            for (int j = 0; j < NY; ++j) {
                freeM[j] = ~occ[j] & FULL;
                out[j]   = freeM[j] & ((j == 0 || j == NY - 1) ? FULL : EDGE);
            }
            FLOOD(out, freeM);
            uint64_t up = 0;
            for (int j = 0; j < NY; ++j) {
                upc[j] = up;
                up |= occ[j];
            }
            uint64_t down = 0;
            for (int j = NY - 1; j >= 0; --j) {
                const uint64_t O   = occ[j];
                uint64_t       row = 0;
                if (O) {
                    const int LO = __builtin_ctzll(O), HI = 63 - __builtin_clzll(O);
                    row          = RANGE(LO + 1, HI) & ~O;
                }
                encl[j] = freeM[j] & ~out[j];
                anyEncl |= encl[j] != 0;
                pk[j] = row | (freeM[j] & upc[j] & down) | encl[j];
                down |= O;
            }
            double enclA = 0, pocketA = 0;
            for (int j = 0; j < NY; ++j) {
                const double H = ys[j + 1] - ys[j];
                for (uint64_t m = pk[j]; m; m &= m - 1) {
                    const int i = __builtin_ctzll(m);
                    pocketA += (xs[i + 1] - xs[i]) * H;
                }
                for (uint64_t m = encl[j]; m; m &= m - 1) {
                    const int i = __builtin_ctzll(m);
                    enclA += (xs[i + 1] - xs[i]) * H;
                }
            }
            int sig = 0;
            if (anyEncl) { // significant enclosed holes: area >= (2 gap)^2, both sides >= gap
                const double MINA = 4 * c.gap * c.gap;
                uint64_t     rem[64], comp[64];
                for (int j = 0; j < NY; ++j)
                    rem[j] = encl[j];
                for (int j0 = 0; j0 < NY && !sig; ++j0)
                    while (rem[j0] && !sig) {
                        for (int j = 0; j < NY; ++j)
                            comp[j] = 0;
                        comp[j0] = rem[j0] & (~rem[j0] + 1);
                        FLOOD(comp, rem);
                        double a = 0, x0 = 1e18, y0 = 1e18, x1 = -1e18, y1 = -1e18;
                        for (int j = 0; j < NY; ++j) {
                            if (!comp[j])
                                continue;
                            rem[j] &= ~comp[j];
                            const double H = ys[j + 1] - ys[j];
                            y0 = std::min(y0, ys[j]), y1 = std::max(y1, ys[j + 1]);
                            x0 = std::min(x0, xs[__builtin_ctzll(comp[j])]), x1 = std::max(x1, xs[64 - __builtin_clzll(comp[j])]);
                            for (uint64_t m = comp[j]; m; m &= m - 1) {
                                const int i = __builtin_ctzll(m);
                                a += (xs[i + 1] - xs[i]) * H;
                            }
                        }
                        if (a >= MINA && x1 - x0 >= c.gap && y1 - y0 >= c.gap)
                            ++sig;
                    }
            }
            return (sig ? 6.0 : 0.0) + 30.0 * enclA / BA + 10.0 * pocketA / BA;
        }

        // GUILLOTINE: every way to cut the windows into two groups laid side by
        // side or stacked, recursively, in any order -- the Pareto front of the
        // block shapes each group can take (capped). Windows of the same size are
        // interchangeable, so a group is a COUNT per distinct size (a multiset):
        // ten terminals of one size cost no more than one. Each root shape that
        // fits becomes a packing; inside a block a smaller part leans toward the
        // cluster centre, so the slack ends up outside. Returns false when the
        // set has too many distinct groups for the budget (nothing added).
        inline double aspectPenalty(const Ctx& c, const IR& B); // below (the root shapes are ranked with it)
        inline bool   guillotine(Ctx& c, int cap, long maxStates, long maxPairs, std::vector<std::vector<IR>>& pool) {
            const int N = c.N, G = c.G;
            if (N < 2)
                return true;
            // distinct sizes (types), their windows in input order
            std::vector<std::pair<int, int>> types;
            std::vector<std::vector<int>>    members;
            for (int i = 0; i < N; ++i) {
                size_t t = 0;
                while (t < types.size() && (types[t].first != c.W[i] || types[t].second != c.H[i]))
                    ++t;
                if (t == types.size())
                    types.push_back({c.W[i], c.H[i]}), members.emplace_back();
                members[t].push_back(i);
            }
            const int        T = static_cast<int>(types.size());
            std::vector<int> cnt(static_cast<size_t>(T)), stride(static_cast<size_t>(T));
            long             states = 1, pairs = 1;
            for (int t = 0; t < T; ++t) {
                cnt[t]    = static_cast<int>(members[t].size());
                stride[t] = static_cast<int>(states);
                states *= cnt[t] + 1;
                pairs *= static_cast<long>(cnt[t] + 1) * (cnt[t] + 2) / 2;
                if (states > maxStates || pairs > maxPairs)
                    return false;
            }
            if (pairs > c.prm.guillPairsWide) // a big search: a narrower front
                cap = std::min(cap, c.prm.guillCapWide);
            struct SCut {
                int     w = 0, h = 0;
                uint8_t kind = 0; // 0 one window, 1 side by side, 2 stacked
                int     sub = 0;  // the first part's state (the second: state - sub)
                uint8_t a = 0, b = 0;
                bool    line = true; // one long row (a column on a portrait area): every cut side by side
            };
            const uint8_t                  LINEKIND = c.areaW >= c.areaH ? 1 : 2;
            const int                      FULL = static_cast<int>(states) - 1;
            std::vector<std::vector<SCut>> F(static_cast<size_t>(states));
            std::vector<int>               cs(static_cast<size_t>(T)), ss(static_cast<size_t>(T));
            std::vector<SCut>              cand, front;
            std::vector<int>               slot(4096, -1);
            for (int code = 1; code <= FULL; ++code) {
                int total = 0, rem = code;
                for (int t = T - 1; t >= 0; --t)
                    cs[t] = rem / stride[t], rem %= stride[t], total += cs[t];
                if (total == 1) {
                    int t = 0;
                    while (!cs[t])
                        ++t;
                    F[static_cast<size_t>(code)] = {SCut{.w = types[t].first, .h = types[t].second}};
                    continue;
                }
                cand.clear();
                // every sub-multiset (odometer), each unordered pair once: sub <= rest
                std::fill(ss.begin(), ss.end(), 0);
                for (;;) {
                    int t = 0;
                    while (t < T && ss[t] == cs[t])
                        ss[t] = 0, ++t;
                    if (t == T)
                        break;
                    ++ss[t];
                    int sub = 0;
                    for (int u = 0; u < T; ++u)
                        sub += ss[u] * stride[u];
                    const int REST = code - sub;
                    if (sub == code || sub > REST)
                        continue;
                    const auto& FA = F[static_cast<size_t>(sub)];
                    const auto& FB = F[static_cast<size_t>(REST)];
                    for (size_t ai = 0; ai < FA.size(); ++ai)
                        for (size_t bi = 0; bi < FB.size(); ++bi) {
                            const SCut& A = FA[ai];
                            const SCut& B = FB[bi];
                            const SCut  SIDE{.w = A.w + G + B.w, .h = std::max(A.h, B.h), .kind = 1, .sub = sub, .a = static_cast<uint8_t>(ai), .b = static_cast<uint8_t>(bi), .line = LINEKIND == 1 && A.line && B.line};
                            const SCut  STACK{.w = std::max(A.w, B.w), .h = A.h + G + B.h, .kind = 2, .sub = sub, .a = static_cast<uint8_t>(ai), .b = static_cast<uint8_t>(bi), .line = LINEKIND == 2 && A.line && B.line};
                            if (SIDE.w <= c.AW && SIDE.h <= c.AH)
                                cand.push_back(SIDE);
                            if (STACK.w <= c.AW && STACK.h <= c.AH)
                                cand.push_back(STACK);
                        }
                }
                // the Pareto front (narrower AND shorter wins): a sort for a few
                // candidates; for many, one slot per width (the shortest of that width),
                // read in width order -- the same front without the big sort
                front.clear();
                if (cand.size() <= 384) {
                    std::sort(cand.begin(), cand.end(), [](const SCut& x, const SCut& y) { return x.w < y.w || (x.w == y.w && x.h < y.h); });
                    for (const SCut& C : cand)
                        if (front.empty() || C.h < front.back().h)
                            front.push_back(C);
                } else {
                    const int SLOTS = static_cast<int>(slot.size());
                    for (size_t q = 0; q < cand.size(); ++q) {
                        const SCut& C = cand[q];
                        const int   K = c.AW < SLOTS ? C.w : std::min(SLOTS - 1, static_cast<int>(static_cast<long long>(C.w) * SLOTS / (c.AW + 1)));
                        int&        B = slot[static_cast<size_t>(K)];
                        if (B < 0 || C.h < cand[static_cast<size_t>(B)].h || (C.h == cand[static_cast<size_t>(B)].h && C.w < cand[static_cast<size_t>(B)].w))
                            B = static_cast<int>(q);
                    }
                    for (int& B : slot) {
                        if (B >= 0 && (front.empty() || cand[static_cast<size_t>(B)].h < front.back().h))
                            front.push_back(cand[static_cast<size_t>(B)]);
                        B = -1;
                    }
                }
                if (code == FULL) { // the root: every point is a whole layout -- keep the best few by share and fill
                    std::stable_sort(front.begin(), front.end(), [&](const SCut& x, const SCut& y) {
                        const auto KEY = [&](const SCut& q) {
                            return 5.0 * std::max(q.w / c.areaW, q.h / c.areaH) - 2.0 * c.area0 / (static_cast<double>(q.w) * q.h) + (q.line ? aspectPenalty(c, IR{0, 0, q.w, q.h}) : 0.0);
                        };
                        return KEY(x) < KEY(y);
                    });
                    if (static_cast<int>(front.size()) > c.prm.guillRoots)
                        front.resize(static_cast<size_t>(c.prm.guillRoots));
                } else if (static_cast<int>(front.size()) > cap) {
                    // an even spread along the front
                    std::vector<SCut> kept;
                    for (int k = 0; k < cap; ++k)
                        kept.push_back(front[static_cast<size_t>(k) * (front.size() - 1) / static_cast<size_t>(cap - 1)]);
                    front.swap(kept);
                }
                F[static_cast<size_t>(code)] = front;
            }
            std::vector<size_t> next(static_cast<size_t>(T));
            for (size_t root = 0; root < F[static_cast<size_t>(FULL)].size(); ++root) {
                const SCut&     RT = F[static_cast<size_t>(FULL)][root];
                std::vector<IR> out(static_cast<size_t>(N));
                std::fill(next.begin(), next.end(), 0);
                const auto PLACE = [&](const auto& self, int code, size_t idx, int x, int y, int roomW, int roomH) -> void {
                    const SCut& SH = F[static_cast<size_t>(code)][idx];
                    // lean toward the cluster centre within the room
                    const int OX = std::clamp((RT.w - SH.w) / 2, x, x + roomW - SH.w);
                    const int OY = std::clamp((RT.h - SH.h) / 2, y, y + roomH - SH.h);
                    if (SH.kind == 0) { // the next window of this size, in input order
                        int t = 0;
                        while (code / stride[t] % (cnt[t] + 1) == 0)
                            ++t;
                        const int I = members[t][next[t]++];
                        out[static_cast<size_t>(I)] = {OX, OY, c.W[I], c.H[I]};
                        return;
                    }
                    const SCut& A = F[static_cast<size_t>(SH.sub)][SH.a];
                    if (SH.kind == 1) {
                        self(self, SH.sub, SH.a, OX, OY, A.w, SH.h);
                        self(self, code - SH.sub, SH.b, OX + A.w + G, OY, SH.w - A.w - G, SH.h);
                    } else {
                        self(self, SH.sub, SH.a, OX, OY, SH.w, A.h);
                        self(self, code - SH.sub, SH.b, OX, OY + A.h + G, SH.w, SH.h - A.h - G);
                    }
                };
                PLACE(PLACE, FULL, root, 0, 0, RT.w, RT.h);
                pool.push_back(std::move(out));
            }
            return true;
        }

        // GROUPED, NOT A LONG ROW (user, 2026-10-15: three windows on 4K came out
        // as one row, "spread out horizontally"; the user's pick: "rows, prefer
        // squarer"). Screen share alone always favours a strip along the
        // screen's long side, so a cluster of 3+ windows that is MORE elongated
        // than the screen itself (past `aspectTol` in log aspect; 0 = any wider
        // than the screen's own shape) pays weight x the excess in log aspect.
        // At 8, a grouped cluster wins unless it is clearly looser (the bench's
        // own scale: 5 per unit of screen share, 2 per unit of fill). One-sided: a squarer cluster never pays (a 2x2
        // grid of terminals is the ideal, not a fault), and a pair is exempt
        // (user: "a pair stays side by side").
        inline double aspectPenalty(const Ctx& c, const IR& B) {
            if (c.prm.aspectWeight <= 0 || c.N < 3 || B.w <= 0 || B.h <= 0)
                return 0;
            const double CLUSTER = static_cast<double>(B.w) / B.h, SCREEN = c.areaW / c.areaH;
            const double E       = SCREEN >= 1.0 ? std::log(CLUSTER / SCREEN) : std::log(SCREEN / CLUSTER);
            return c.prm.aspectWeight * std::max(0.0, E - c.prm.aspectTol);
        }
        // ...and only a STRIP pays it: every window side by side with every other
        // (their x spans disjoint; y spans on a portrait area) -- one long row.
        // A block of rows that happens to be wide (a 4x2 grid of terminals, a big
        // window beside a 2x2 of small ones) is a well-grouped cluster, not this.
        inline double stripPenalty(const Ctx& c, const std::vector<IR>& r, const IR& B) {
            if (c.prm.aspectWeight <= 0 || c.N < 3 || r.size() < 3)
                return 0;
            const bool LAND = c.areaW >= c.areaH;
            for (size_t i = 0; i < r.size(); ++i)
                for (size_t k = i + 1; k < r.size(); ++k)
                    if ((LAND ? sepX(r[i], r[k]) : sepY(r[i], r[k])) < 0)
                        return 0;
            return aspectPenalty(c, B);
        }

        // The part of the judgement that depends on the bounding box alone (screen
        // share, fill, centring): a lower bound of the full score.
        inline Judged boxOnly(const Ctx& c, const IR& B, double filled) {
            Judged       j;
            const double BA = std::max(1.0, static_cast<double>(B.w) * B.h);
            j.fill          = filled / BA;
            j.S             = std::max(B.w / c.areaW, B.h / c.areaH);
            j.comps         = 1;
            const double CX = B.x + B.w / 2.0 - (c.AX + c.AW / 2.0), CY = B.y + B.h / 2.0 - (c.AY + c.AH / 2.0);
            j.score         = 5.0 * j.S + 2.0 * (1.0 - j.fill) + 2.0 * std::hypot(CX, CY) / (std::min(c.areaW, c.areaH) / 2.0); // (no strip penalty: a lower bound)
            return j;
        }

        inline Judged judge(Ctx& c, const std::vector<IR>& r) {
            Judged    j;
            const int N = c.N;
            const IR  B = bbox(r);
            double    filled = 0;
            for (const IR& R : r)
                filled += static_cast<double>(R.w) * R.h;
            const double BA = std::max(1.0, static_cast<double>(B.w) * B.h);
            j.fill          = filled / BA;
            j.S             = std::max(B.w / c.areaW, B.h / c.areaH);
            // one cluster: windows within gap+2 across a shared span
            auto& par = c.par;
            par.resize(static_cast<size_t>(N));
            std::iota(par.begin(), par.end(), 0);
            const auto FIND = [&](int x) {
                while (par[x] != x)
                    x = par[x] = par[par[x]];
                return x;
            };
            const double TOL = c.gap + 2.0;
            for (int i = 0; i < N; ++i)
                for (int k = i + 1; k < N; ++k) {
                    const int SX = sepX(r[i], r[k]), SY = sepY(r[i], r[k]);
                    if ((SX <= TOL && SY < 0) || (SY <= TOL && SX < 0))
                        par[FIND(i)] = FIND(k);
                }
            int comps = 0;
            for (int i = 0; i < N; ++i)
                comps += FIND(i) == i;
            j.comps     = comps;
            double conn = comps > 1 ? std::min(60.0, 25.0 + 5.0 * (comps - 2)) : 0.0;
            // holes and pockets on the windows inflated by gap/2 + 1
            const double D  = c.gap / 2.0 + 1.0;
            auto&        xs = c.xs;
            auto&        ys = c.ys;
            xs.clear(), ys.clear();
            for (const IR& R : r) {
                xs.push_back(R.x - D), xs.push_back(R.x + R.w + D);
                ys.push_back(R.y - D), ys.push_back(R.y + R.h + D);
            }
            std::sort(xs.begin(), xs.end());
            xs.erase(std::unique(xs.begin(), xs.end()), xs.end());
            std::sort(ys.begin(), ys.end());
            ys.erase(std::unique(ys.begin(), ys.end()), ys.end());
            const int NX = static_cast<int>(xs.size()) - 1, NY = static_cast<int>(ys.size()) - 1;
            double    hole = 0;
            if (NX > 0 && NY > 0 && NX <= 64 && NY <= 64 && !c.slowHoles)
                hole = holesFast(c, r, D, NX, NY, BA);
            else if (NX > 0 && NY > 0) {
                auto& occ = c.occ;
                occ.assign(static_cast<size_t>(NX * NY), 0);
                for (const IR& R : r) {
                    const int I0 = static_cast<int>(std::lower_bound(xs.begin(), xs.end(), R.x - D) - xs.begin());
                    const int I1 = static_cast<int>(std::lower_bound(xs.begin(), xs.end(), R.x + R.w + D) - xs.begin());
                    const int J0 = static_cast<int>(std::lower_bound(ys.begin(), ys.end(), R.y - D) - ys.begin());
                    const int J1 = static_cast<int>(std::lower_bound(ys.begin(), ys.end(), R.y + R.h + D) - ys.begin());
                    for (int jj = J0; jj < J1; ++jj)
                        std::fill(occ.begin() + jj * NX + I0, occ.begin() + jj * NX + I1, 1);
                }
                auto& out = c.out;
                auto& st  = c.st;
                out.assign(occ.size(), 0);
                st.clear();
                for (int jj = 0; jj < NY; ++jj)
                    for (int i = 0; i < NX; ++i)
                        if ((i == 0 || jj == 0 || i == NX - 1 || jj == NY - 1) && !occ[jj * NX + i]) {
                            out[jj * NX + i] = 1;
                            st.push_back(jj * NX + i);
                        }
                while (!st.empty()) {
                    const int K = st.back();
                    st.pop_back();
                    const int I = K % NX, J = K / NX;
                    const int NB[4][2] = {{I + 1, J}, {I - 1, J}, {I, J + 1}, {I, J - 1}};
                    for (const auto& Q : NB) {
                        if (Q[0] < 0 || Q[1] < 0 || Q[0] >= NX || Q[1] >= NY)
                            continue;
                        const int KK = Q[1] * NX + Q[0];
                        if (!occ[KK] && !out[KK])
                            out[KK] = 1, st.push_back(KK);
                    }
                }
                auto& pk = c.pk;
                pk.assign(occ.size(), 0);
                double encl = 0, pocket = 0;
                bool   anyEncl = false;
                for (int jj = 0; jj < NY; ++jj) {
                    int first = -1, last = -1;
                    for (int i = 0; i < NX; ++i)
                        if (occ[jj * NX + i]) {
                            if (first < 0)
                                first = i;
                            last = i;
                        }
                    for (int i = first + 1; i < last; ++i)
                        if (!occ[jj * NX + i])
                            pk[jj * NX + i] = 1;
                }
                for (int i = 0; i < NX; ++i) {
                    int first = -1, last = -1;
                    for (int jj = 0; jj < NY; ++jj)
                        if (occ[jj * NX + i]) {
                            if (first < 0)
                                first = jj;
                            last = jj;
                        }
                    for (int jj = first + 1; jj < last; ++jj)
                        if (!occ[jj * NX + i])
                            pk[jj * NX + i] = 1;
                }
                for (int jj = 0; jj < NY; ++jj)
                    for (int i = 0; i < NX; ++i) {
                        const int    K  = jj * NX + i;
                        const double CA = (xs[i + 1] - xs[i]) * (ys[jj + 1] - ys[jj]);
                        if (!occ[K] && !out[K]) {
                            encl += CA, anyEncl = true;
                            pk[K] = 1;
                        }
                        if (pk[K])
                            pocket += CA;
                    }
                int sig = 0;
                if (anyEncl) { // significant enclosed holes: area >= (2 gap)^2, both sides >= gap
                    auto& seen = c.seen;
                    seen.assign(occ.size(), 0);
                    const double MINA = 4 * c.gap * c.gap;
                    for (int s = 0; s < NX * NY && !sig; ++s) {
                        if (occ[s] || out[s] || seen[s])
                            continue;
                        double a = 0, x0 = 1e18, y0 = 1e18, x1 = -1e18, y1 = -1e18;
                        st.assign(1, s);
                        seen[s] = 1;
                        while (!st.empty()) {
                            const int K = st.back();
                            st.pop_back();
                            const int I = K % NX, J = K / NX;
                            a += (xs[I + 1] - xs[I]) * (ys[J + 1] - ys[J]);
                            x0 = std::min(x0, xs[I]), x1 = std::max(x1, xs[I + 1]), y0 = std::min(y0, ys[J]), y1 = std::max(y1, ys[J + 1]);
                            const int NB[4][2] = {{I + 1, J}, {I - 1, J}, {I, J + 1}, {I, J - 1}};
                            for (const auto& Q : NB) {
                                if (Q[0] < 0 || Q[1] < 0 || Q[0] >= NX || Q[1] >= NY)
                                    continue;
                                const int KK = Q[1] * NX + Q[0];
                                if (!occ[KK] && !out[KK] && !seen[KK])
                                    seen[KK] = 1, st.push_back(KK);
                            }
                        }
                        if (a >= MINA && x1 - x0 >= c.gap && y1 - y0 >= c.gap)
                            ++sig;
                    }
                }
                hole = (sig ? 6.0 : 0.0) + 30.0 * encl / BA + 10.0 * pocket / BA;
            }
            j.hole          = hole;
            const double CX = B.x + B.w / 2.0 - (c.AX + c.AW / 2.0), CY = B.y + B.h / 2.0 - (c.AY + c.AH / 2.0);
            const double CE = 2.0 * std::hypot(CX, CY) / (std::min(c.areaW, c.areaH) / 2.0);
            j.score         = conn + hole + 5.0 * j.S + 2.0 * (1.0 - j.fill) + CE + stripPenalty(c, r, B);
            return j;
        }

        // ------------------------------------------------------- placement
        // Shift a box-relative packing so its bounding box is centred in the area.
        inline void centre(const Ctx& c, std::vector<IR>& r) {
            const IR  B  = bbox(r);
            const int DX = c.AX + (c.AW - B.w) / 2 - B.x, DY = c.AY + (c.AH - B.h) / 2 - B.y;
            for (IR& R : r)
                R.x += DX, R.y += DY;
        }

        // Slide every window toward the cluster centre (mode 0), axis by axis,
        // until it is blocked by a neighbour (>= G apart) or lines up with the
        // centre -- inner windows first, repeated until nothing moves. Modes 1-4
        // slide everything toward one side of the bounding box instead (left,
        // right, top, bottom), which can push a trapped gap out to the edge.
        inline bool compact(const Ctx& c, std::vector<IR>& r, int mode = 0) {
            const int        N = c.N, G = c.G;
            std::vector<int> ord(static_cast<size_t>(N));
            bool             any = false;
            for (int it = 0; it < 12; ++it) {
                const IR B = bbox(r);
                // twice the target point
                const int CX2 = mode == 1 ? 2 * B.x - 4 * B.w : mode == 2 ? 2 * (B.x + B.w) + 4 * B.w : 2 * B.x + B.w;
                const int CY2 = mode == 3 ? 2 * B.y - 4 * B.h : mode == 4 ? 2 * (B.y + B.h) + 4 * B.h : 2 * B.y + B.h;
                std::iota(ord.begin(), ord.end(), 0);
                const auto KEY = [&](int a) {
                    if (mode == 1)
                        return r[a].x;
                    if (mode == 2)
                        return -(r[a].x + r[a].w);
                    if (mode == 3)
                        return r[a].y;
                    if (mode == 4)
                        return -(r[a].y + r[a].h);
                    return std::abs(2 * r[a].x + r[a].w - CX2) + std::abs(2 * r[a].y + r[a].h - CY2);
                };
                std::stable_sort(ord.begin(), ord.end(), [&](int a, int b) { return KEY(a) < KEY(b); });
                bool moved = false;
                for (const int i : ord) {
                    for (int axis = 0; axis < 2; ++axis) {
                        if ((mode == 1 || mode == 2) && axis == 1)
                            continue;
                        if ((mode == 3 || mode == 4) && axis == 0)
                            continue;
                        IR& A  = r[i];
                        int D2 = axis == 0 ? CX2 - (2 * A.x + A.w) : CY2 - (2 * A.y + A.h);
                        if (std::abs(D2) < 2)
                            continue;
                        int lim = std::abs(D2) / 2;
                        // never past the bounding box
                        if (axis == 0)
                            lim = std::min(lim, D2 > 0 ? B.x + B.w - (A.x + A.w) : A.x - B.x);
                        else
                            lim = std::min(lim, D2 > 0 ? B.y + B.h - (A.y + A.h) : A.y - B.y);
                        for (int k = 0; k < N && lim > 0; ++k) {
                            if (k == i)
                                continue;
                            const IR& Q = r[k];
                            if (axis == 0) {
                                if (sepY(A, Q) >= G)
                                    continue;
                                if (D2 > 0 && Q.x >= A.x + A.w)
                                    lim = std::min(lim, Q.x - (A.x + A.w) - G);
                                else if (D2 < 0 && Q.x + Q.w <= A.x)
                                    lim = std::min(lim, A.x - (Q.x + Q.w) - G);
                            } else {
                                if (sepX(A, Q) >= G)
                                    continue;
                                if (D2 > 0 && Q.y >= A.y + A.h)
                                    lim = std::min(lim, Q.y - (A.y + A.h) - G);
                                else if (D2 < 0 && Q.y + Q.h <= A.y)
                                    lim = std::min(lim, A.y - (Q.y + Q.h) - G);
                            }
                        }
                        if (lim > 0) {
                            (axis == 0 ? A.x : A.y) += D2 > 0 ? lim : -lim;
                            moved = any = true;
                        }
                    }
                }
                if (!moved)
                    break;
            }
            return any;
        }

        inline void mirror(std::vector<IR>& r, bool fx, bool fy) {
            const IR B = bbox(r);
            for (IR& R : r) {
                if (fx)
                    R.x = B.x + (B.x + B.w) - (R.x + R.w);
                if (fy)
                    R.y = B.y + (B.y + B.h) - (R.y + R.h);
            }
        }

        // The mirror image that puts the biggest window (then the next) left of /
        // above the cluster centre: mirrors score the same, and a fixed choice keeps
        // the cluster from flipping when a window opens or closes.
        inline void canonical(std::vector<IR>& r) {
            const IR     B = bbox(r);
            const size_t N = r.size();
            double       sx = 0, sy = 0; // earlier windows weigh more
            for (size_t i = 0; i < N; ++i) {
                const double WT = static_cast<double>(N - i);
                sx += WT * (2 * r[i].x + r[i].w - (2 * B.x + B.w));
                sy += WT * (2 * r[i].y + r[i].h - (2 * B.y + B.h));
            }
            mirror(r, sx > 0, sy > 0);
        }

        // How far the layout's reading order (top-left corners, row by row) is
        // from the windows' own order: inversions / all pairs, 0..1. The caller
        // keeps its windows in the previous layout's reading order, so a layout
        // that reads in that order is the one that moves the least.
        inline double disorder(const std::vector<IR>& r) {
            const size_t N = r.size();
            if (N < 2)
                return 0;
            long inv = 0;
            for (size_t i = 0; i < N; ++i)
                for (size_t k = i + 1; k < N; ++k)
                    if (r[k].y < r[i].y || (r[k].y == r[i].y && r[k].x < r[i].x))
                        ++inv;
            return static_cast<double>(inv) / (static_cast<double>(N) * (N - 1) / 2);
        }

        // Inside the area, every pair at least G apart (a last guard before output).
        inline bool valid(const Ctx& c, const std::vector<IR>& r) {
            const int N = c.N;
            for (const IR& R : r)
                if (R.x < c.AX || R.y < c.AY || R.x + R.w > c.AX + c.AW || R.y + R.h > c.AY + c.AH)
                    return false;
            for (int i = 0; i < N; ++i)
                for (int k = i + 1; k < N; ++k)
                    if (sepX(r[i], r[k]) < c.G && sepY(r[i], r[k]) < c.G)
                        return false;
            return true;
        }

        inline uint64_t hashOf(const std::vector<IR>& r) {
            uint64_t h = 1469598103934665603ULL;
            for (const IR& R : r)
                h = (h ^ static_cast<uint64_t>(R.x * 131071 + R.y)) * 1099511628211ULL;
            return h;
        }

        inline void shuffle(std::vector<int>& o, uint64_t& seed) {
            for (size_t i = o.size() - 1; i > 0; --i) { // Fisher-Yates on a splitmix64 stream
                seed += 0x9E3779B97F4A7C15ULL;
                uint64_t z = seed;
                z          = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
                z          = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
                z ^= z >> 31;
                std::swap(o[i], o[static_cast<size_t>(z % static_cast<uint64_t>(i + 1))]);
            }
        }

        struct Cand {
            std::vector<IR> r;
            Judged          j;
            double          s = 0; // the objective (score, + drop terms for a drop)
            bool            polished = false;
        };

        // RELOCATE: move single windows to better spots inside the cluster's
        // bounding box -- every spot where the window lines up with or touches a
        // neighbour (or the box edge) on both axes -- while the objective improves.
        // This is what closes the slivers a packer leaves where one window
        // straddles two neighbours of different depth.
        template <class F>
        inline void relocate(Ctx& c, Cand& cur, F&& objective, int& budget) {
            const int        N = c.N, G = c.G;
            std::vector<int> cx, cy, lane;
            auto&            r = cur.r;
            double           filled = 0;
            for (const IR& R : r)
                filled += static_cast<double>(R.w) * R.h;
            for (int pass = 0; pass < 4 && budget > 0 && c.posBudget > 0; ++pass) {
                bool improved = false;
                for (int i = 0; i < N && budget > 0 && c.posBudget > 0; ++i) {
                    const IR B    = bbox(r);
                    const IR SAVE = r[i];
                    // the bounding box of the others
                    IR BO{INT_MAX / 2, INT_MAX / 2, 0, 0};
                    {
                        int x1 = INT_MIN / 2, y1 = INT_MIN / 2;
                        for (int k = 0; k < N; ++k)
                            if (k != i)
                                BO.x = std::min(BO.x, r[k].x), BO.y = std::min(BO.y, r[k].y), x1 = std::max(x1, r[k].x + r[k].w), y1 = std::max(y1, r[k].y + r[k].h);
                        BO.w = x1 - BO.x, BO.h = y1 - BO.y;
                    }
                    const int Wi = SAVE.w, Hi = SAVE.h;
                    cx.clear(), cy.clear();
                    cx.push_back(B.x), cx.push_back(B.x + B.w - Wi);
                    cy.push_back(B.y), cy.push_back(B.y + B.h - Hi);
                    for (int k = 0; k < N; ++k) {
                        if (k == i)
                            continue;
                        const IR& Q = r[k];
                        for (const int X : {Q.x + Q.w + G, Q.x - Wi - G, Q.x, Q.x + Q.w - Wi})
                            if (X >= B.x && X + Wi <= B.x + B.w)
                                cx.push_back(X);
                        for (const int Y : {Q.y + Q.h + G, Q.y - Hi - G, Q.y, Q.y + Q.h - Hi})
                            if (Y >= B.y && Y + Hi <= B.y + B.h)
                                cy.push_back(Y);
                    }
                    std::sort(cx.begin(), cx.end());
                    cx.erase(std::unique(cx.begin(), cx.end()), cx.end());
                    std::sort(cy.begin(), cy.end());
                    cy.erase(std::unique(cy.begin(), cy.end()), cy.end());
                    double bestS = cur.s;
                    IR     bestR = SAVE;
                    Judged bestJ = cur.j;
                    for (const int X : cx) {
                        // the windows this column of positions could collide with
                        lane.clear();
                        const IR AX{X, 0, Wi, Hi};
                        for (int k = 0; k < N; ++k)
                            if (k != i && sepX(AX, r[k]) < G)
                                lane.push_back(k);
                        for (const int Y : cy) {
                            if (X == SAVE.x && Y == SAVE.y)
                                continue;
                            const IR A{X, Y, Wi, Hi};
                            --c.posBudget;
                            bool     ok = true;
                            for (const int k : lane)
                                if (sepY(A, r[k]) < G) {
                                    ok = false;
                                    break;
                                }
                            if (!ok)
                                continue;
                            r[i] = A;
                            {
                                const IR NB{std::min(BO.x, X), std::min(BO.y, Y), 0, 0};
                                const IR BB{NB.x, NB.y, std::max(BO.x + BO.w, X + Wi) - NB.x, std::max(BO.y + BO.h, Y + Hi) - NB.y};
                                if (objective(r, boxOnly(c, BB, filled)) >= bestS - 1e-9)
                                    continue; // cannot beat the best even with no hole or pocket
                            }
                            const Judged J = judge(c, r);
                            const double S = objective(r, J);
                            --budget;
                            if (S < bestS - 1e-9)
                                bestS = S, bestR = A, bestJ = J;
                        }
                    }
                    r[i] = bestR;
                    if (bestS < cur.s - 1e-9) {
                        cur.s = bestS, cur.j = bestJ;
                        improved = true;
                    }
                }
                if (!improved)
                    break;
            }
            // re-centre (the box may have shrunk)
            centre(c, r);
            cur.j = judge(c, r);
            cur.s = objective(r, cur.j);
        }

        // SWAP: two windows trade places. Same size: free (the cluster does not
        // change, only who sits where -- the drop point, the reading order).
        // Different sizes: each takes the other's spot anchored at one of its four
        // corners, if that is still a valid layout.
        template <class F>
        inline void swaps(Ctx& c, Cand& cur, F&& objective, int& budget) {
            const int N = c.N, G = c.G;
            auto&     r = cur.r;
            double    filled = 0;
            for (const IR& R : r)
                filled += static_cast<double>(R.w) * R.h;
            const auto FREE = [&](const IR& A, int skip1, int skip2) {
                for (int k = 0; k < N; ++k)
                    if (k != skip1 && k != skip2 && sepX(A, r[k]) < G && sepY(A, r[k]) < G)
                        return false;
                return true;
            };
            for (int pass = 0; pass < 2; ++pass) {
                bool improved = false;
                for (int i = 0; i < N; ++i)
                    for (int k = i + 1; k < N; ++k) {
                        const IR A = r[i], B = r[k];
                        if (A.w == B.w && A.h == B.h) {
                            r[i].x = B.x, r[i].y = B.y, r[k].x = A.x, r[k].y = A.y;
                            const double S = objective(r, cur.j);
                            if (S < cur.s - 1e-9)
                                cur.s = S, improved = true;
                            else
                                r[i] = A, r[k] = B;
                            continue;
                        }
                        if (budget <= 0)
                            continue;
                        double bestS = cur.s;
                        IR     bA = A, bB = B;
                        Judged bJ = cur.j;
                        for (int anchor = 0; anchor < 4; ++anchor) {
                            const bool RX = anchor & 1, BY = anchor & 2;
                            const IR   NA{RX ? B.x + B.w - A.w : B.x, BY ? B.y + B.h - A.h : B.y, A.w, A.h};
                            const IR   NB{RX ? A.x + A.w - B.w : A.x, BY ? A.y + A.h - B.h : A.y, B.w, B.h};
                            if (sepX(NA, NB) < G && sepY(NA, NB) < G)
                                continue;
                            if (!FREE(NA, i, k) || !FREE(NB, i, k))
                                continue;
                            r[i] = NA, r[k] = NB;
                            const IR BB = bbox(r);
                            if (BB.w <= c.AW && BB.h <= c.AH && objective(r, boxOnly(c, BB, filled)) < bestS - 1e-9) {
                                const Judged J = judge(c, r);
                                --budget;
                                const double S = objective(r, J);
                                if (S < bestS - 1e-9)
                                    bestS = S, bA = NA, bB = NB, bJ = J;
                            }
                            r[i] = A, r[k] = B;
                        }
                        if (bestS < cur.s - 1e-9) {
                            r[i] = bA, r[k] = bB;
                            cur.s = bestS, cur.j = bJ;
                            improved = true;
                        }
                    }
                if (!improved)
                    break;
            }
        }

        // The top few candidates by the objective get polished -- relocation,
        // then every compaction (centre, four sides), again while it helps -- and
        // the best wins.
        template <class F>
        inline Cand polish(Ctx& c, std::vector<Cand>& cands, F&& objective, int top, int& budget) {
            std::vector<size_t> idx(cands.size());
            std::iota(idx.begin(), idx.end(), size_t{0});
            std::stable_sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return cands[a].s < cands[b].s; });
            Cand best = cands[idx[0]];
            int  done = 0;
            for (size_t k = 0; k < idx.size() && done < top; ++k) {
                Cand& SRC = cands[idx[k]];
                if (SRC.polished)
                    continue;
                SRC.polished = true;
                ++done;
                Cand C = SRC;
                for (int round = 0; round < 3; ++round) {
                    if (budget > 0)
                        relocate(c, C, objective, budget);
                    {
                        const double BEFORE = C.s;
                        swaps(c, C, objective, budget);
                        if (C.s < BEFORE - 1e-9) { // a swap can free a better spot: relocate again
                            centre(c, C.r);
                            C.j = judge(c, C.r);
                            C.s = objective(C.r, C.j);
                            if (budget > 0)
                                relocate(c, C, objective, budget);
                        }
                    }
                    bool better = false;
                    for (int mode = 0; mode < 5; ++mode) {
                        Cand D = C;
                        if (!compact(c, D.r, mode))
                            continue;
                        centre(c, D.r);
                        D.j = judge(c, D.r);
                        D.s = objective(D.r, D.j);
                        if (D.s < C.s - 1e-9)
                            C = std::move(D), better = true;
                    }
                    if (!better)
                        break;
                }
                C.polished = true;
                if (C.s < best.s - 1e-9 && valid(c, C.r)) // the guard: a polish never ships an invalid layout
                    best = std::move(C);
            }
            return best;
        }

    } // namespace BoxPack

    // fitOnly: "does anything pack?" for a resize's limit (dozens of calls a
    // step): the same generators' first packing of the whole area, unjudged,
    // centred. Whenever it answers yes, the full pack() of the same set answers
    // too (its pool holds the same packings); it may say no where the full
    // search (strips, the Beam fallback) still finds a fit -- a limit a few px
    // short, never one that does not hold.
    inline std::optional<std::vector<SRect>> packImpl(const std::vector<SRect>& sizes, const SRect& area, double gap, const PackHint* hint, int fitOnly) {
        using namespace BoxPack;
        const size_t NS = sizes.size();
        if (NS == 0)
            return std::vector<SRect>{};
        // nonsense in, nothing out (the desktop tiles)
        if (!std::isfinite(area.x) || !std::isfinite(area.y) || !std::isfinite(area.w) || !std::isfinite(area.h) || area.w < 1 || area.h < 1 || !std::isfinite(gap) ||
            std::abs(area.x) > 1e7 || std::abs(area.y) > 1e7 || area.w > 1e6 || area.h > 1e6)
            return std::nullopt;
        for (const SRect& S : sizes)
            if (!std::isfinite(S.w) || !std::isfinite(S.h) || S.w <= 0 || S.h <= 0)
                return std::nullopt;
        if (hint && (!std::isfinite(hint->x) || !std::isfinite(hint->y)))
            hint = nullptr;
        gap = std::max(0.0, gap);
        Ctx c;
        c.N     = static_cast<int>(NS);
        c.gap   = gap;
        c.G     = static_cast<int>(std::ceil(gap - 1e-9));
        if (NS > 20) // past the budgeted sizes: a lighter search
            c.prm.shufBig = 2, c.prm.shapesBig = 2, c.prm.polishTop = 2, c.prm.judgeBig = 400, c.prm.judge2Big = 600, c.prm.polish2Top = 3, c.prm.posBig = 150000, c.prm.stripOrdersBig = 4, c.prm.judgePoolBig = 30;
        c.AX    = static_cast<int>(std::ceil(area.x - 1e-9));
        c.AY    = static_cast<int>(std::ceil(area.y - 1e-9));
        c.AW    = static_cast<int>(std::floor(area.x + area.w + 1e-9)) - c.AX;
        c.AH    = static_cast<int>(std::floor(area.y + area.h + 1e-9)) - c.AY;
        c.areaW = std::max(1.0, area.w), c.areaH = std::max(1.0, area.h);
        const int N = c.N, G = c.G;
        double    inflated = 0, area0 = 0;
        for (const SRect& S : sizes) {
            const int Wi = static_cast<int>(std::ceil(S.w - 1e-6)), Hi = static_cast<int>(std::ceil(S.h - 1e-6));
            c.W.push_back(Wi), c.H.push_back(Hi);
            c.maxW = std::max(c.maxW, Wi), c.maxH = std::max(c.maxH, Hi);
            inflated += static_cast<double>(Wi + G) * (Hi + G);
            area0 += static_cast<double>(Wi) * Hi;
        }
        // proven impossible: too big, the inflated-area bound, a pair that can neither sit side by side nor stack
        if (c.maxW > c.AW || c.maxH > c.AH || inflated > static_cast<double>(c.AW + G) * (c.AH + G))
            return std::nullopt;
        for (int i = 0; i < N; ++i)
            for (int k = i + 1; k < N; ++k)
                if (c.W[i] + G + c.W[k] > c.AW && c.H[i] + G + c.H[k] > c.AH)
                    return std::nullopt;
        c.posBudget = N <= 8 ? c.prm.posSmall : c.prm.posBig;
        c.area0     = area0;
        const auto OUT = [&](const std::vector<IR>& r) {
            std::vector<SRect> o(NS);
            for (size_t i = 0; i < NS; ++i)
                o[i] = SRect{static_cast<double>(r[i].x), static_cast<double>(r[i].y), sizes[i].w, sizes[i].h};
            return o;
        };
        if (N == 1) {
            std::vector<IR> r{{0, 0, c.W[0], c.H[0]}};
            centre(c, r);
            return OUT(r);
        }

        // ------------------------------------------------ window orders
        std::vector<std::vector<int>> orders;
        const auto                    PUSH = [&](const std::vector<int>& o) {
            for (const auto& O : orders)
                if (O == o)
                    return;
            orders.push_back(o);
        };
        {
            std::vector<int> base(static_cast<size_t>(N));
            std::iota(base.begin(), base.end(), 0);
            const auto SORTED = [&](auto key) {
                auto o = base;
                std::stable_sort(o.begin(), o.end(), [&](int a, int b) { return key(a) > key(b); });
                PUSH(o);
            };
            SORTED([&](int i) { return static_cast<double>(c.W[i]) * c.H[i]; });
            SORTED([&](int i) { return c.H[i] * 4096.0 + c.W[i]; });
            SORTED([&](int i) { return c.W[i] * 4096.0 + c.H[i]; });
            SORTED([&](int i) { return std::max(c.W[i], c.H[i]) * 4096.0 + std::min(c.W[i], c.H[i]); });
            SORTED([&](int i) { return static_cast<double>(c.W[i] + c.H[i]); });
            SORTED([&](int i) { return -static_cast<double>(i); }); // input order
        }
        // deterministic shuffles of the area order (seeded from the sizes)
        uint64_t seed = 0x9E3779B97F4A7C15ULL;
        for (int i = 0; i < N; ++i)
            seed = (seed ^ static_cast<uint64_t>(c.W[i] * 7919 + c.H[i])) * 1099511628211ULL;
        std::vector<int> shuf = orders[0];
        const auto       SHUFFLES = [&](int k) {
            if (N <= 3)
                return;
            for (int t = 0; t < k; ++t) {
                shuffle(shuf, seed);
                PUSH(shuf);
            }
        };
        SHUFFLES(N <= 8 ? c.prm.shufSmall : c.prm.shufBig);

        const EHeur HEURS[] = {EHeur::BSSF, EHeur::BL, EHeur::CP, EHeur::BAF};
        if (fitOnly) {
            std::vector<IR> r, scratch;
            bool            ok = false;
            for (size_t o = 0; o < orders.size() && !ok; ++o)
                for (const EHeur HE : HEURS)
                    if (maxRects(c, orders[o].data(), c.AW, c.AH, HE, scratch)) {
                        r = scratch, ok = true;
                        break;
                    }
            if (!ok && N <= c.prm.guillMax && fitOnly == 1) {
                std::vector<std::vector<IR>> gp;
                guillotine(c, N <= 5 ? 16 : N == 6 ? c.prm.guillCap6 : N == 7 ? c.prm.guillCap7 : N == 8 ? c.prm.guillCap8 : c.prm.guillCapBig, c.prm.guillStates, c.prm.guillPairs, gp);
                if (!gp.empty())
                    r = gp.front(), ok = true;
            }
            if (!ok && fitOnly == 1) { // the same deterministic shuffles the full search falls back to (2, quick: MaxRects alone, no group search, no shuffles)
                const int TRIES = N <= 8 ? 400 : N <= 14 ? 160 : 90;
                for (int t = 0; t < TRIES && !ok; ++t) {
                    shuffle(shuf, seed);
                    for (const EHeur HE : HEURS)
                        if (maxRects(c, shuf.data(), c.AW, c.AH, HE, scratch)) {
                            r = scratch, ok = true;
                            break;
                        }
                }
            }
            if (!ok)
                return std::nullopt;
            centre(c, r);
            return OUT(r);
        }
        // box shapes at screen share s: (s kx AW) x (s ky AH)
        const std::array<std::pair<double, double>, 5> SHAPES = {{{1.0, 1.0}, {0.88, 1.0}, {1.0, 0.88}, {0.76, 1.0}, {1.0, 0.76}}};
        const double S0 = std::max({std::sqrt(inflated / (static_cast<double>(c.AW + G) * (c.AH + G))), static_cast<double>(c.maxW) / c.AW, static_cast<double>(c.maxH) / c.AH});
        const auto   BOX = [&](double s, double kx, double ky) {
            const int BW = std::clamp(static_cast<int>(std::ceil(s * kx * c.AW)), c.maxW, c.AW);
            const int BH = std::clamp(static_cast<int>(std::ceil(s * ky * c.AH)), c.maxH, c.AH);
            return std::pair<int, int>{BW, BH};
        };

        std::vector<std::vector<IR>> pool; // box-relative packings
        std::vector<IR>              tmp;
        const int                    STEPS = N <= 8 ? 8 : 7;
        // the smallest box for one order x heuristic x shape. Once some packing
        // reached share sBest, a combination that cannot pack within sBest + margin
        // is not worth its binary search (it would lose by 5 x margin anyway).
        double     sBest  = 1e9;
        const auto SEARCH = [&](const int* order, EHeur heur, double kx, double ky, int pinIdx, auto pinFor) {
            double          lo = S0, hi = 1.0 / std::min(kx, ky);
            std::vector<IR> best;
            if (pinIdx < 0 && sBest + c.prm.margin < hi)
                hi = std::max(lo, sBest + c.prm.margin);
            {
                const auto [BW, BH] = BOX(hi, kx, ky);
                if (!maxRects(c, order, BW, BH, heur, tmp, pinIdx, pinFor(BW, BH)))
                    return;
                best = tmp;
            }
            for (int it = 0; it < STEPS && hi - lo > 0.002; ++it) {
                const double MID    = it == 0 ? std::max(lo, std::min(hi, lo + 0.02)) : (lo + hi) / 2;
                const auto [BW, BH] = BOX(MID, kx, ky);
                if (maxRects(c, order, BW, BH, heur, tmp, pinIdx, pinFor(BW, BH)))
                    hi = MID, best = tmp;
                else
                    lo = MID;
            }
            if (pinIdx < 0)
                sBest = std::min(sBest, hi);
            pool.push_back(std::move(best));
        };
        const auto NOPIN    = [](int, int) { return IR{}; };
        size_t     searched = 0, stripped = 0;
        // every order not yet searched: the smallest box per heuristic and shape,
        // then strips (fixed width filled top-down, fixed height filled left to right)
        const auto GENERATE = [&](int nShapes, size_t stripOrders) {
            for (; searched < orders.size(); ++searched) {
                const int* O = orders[searched].data();
                for (const EHeur HE : HEURS) {
                    if (sBest > 1.0 && !maxRects(c, O, c.AW, c.AH, HE, tmp))
                        continue;
                    for (int s = 0; s < nShapes; ++s)
                        SEARCH(O, HE, SHAPES[static_cast<size_t>(s)].first, SHAPES[static_cast<size_t>(s)].second, -1, NOPIN);
                }
            }
            const int K  = c.prm.stripSteps;
            const int W0 = std::max(c.maxW, static_cast<int>(S0 * c.AW)), H0 = std::max(c.maxH, static_cast<int>(S0 * c.AH));
            for (; stripped < std::min(orders.size(), stripOrders); ++stripped)
                for (int k = 0; k <= K; ++k) {
                    const int* O  = orders[stripped].data();
                    const int  BW = W0 + static_cast<int>(static_cast<long long>(c.AW - W0) * k / K);
                    if (maxRects(c, O, BW, c.AH, EHeur::BL, tmp))
                        pool.push_back(tmp);
                    const int BH = H0 + static_cast<int>(static_cast<long long>(c.AH - H0) * k / K);
                    if (maxRects(c, O, c.AW, BH, EHeur::LB, tmp))
                        pool.push_back(tmp);
                }
        };
        const bool SMALL = N <= 8;
        if (N <= c.prm.guillMax) // the group search, when the set has few enough distinct groups
            guillotine(c, N <= 5 ? 16 : N == 6 ? c.prm.guillCap6 : N == 7 ? c.prm.guillCap7 : N == 8 ? c.prm.guillCap8 : c.prm.guillCapBig, c.prm.guillStates, c.prm.guillPairs, pool);
        GENERATE(SMALL ? c.prm.shapesSmall : c.prm.shapesBig, static_cast<size_t>(SMALL ? c.prm.stripOrdersSmall : c.prm.stripOrdersBig));
        // barely fits: more orders (deterministic shuffles) until one packs the area
        if (pool.empty()) {
            const int TRIES = SMALL ? 400 : N <= 14 ? 160 : 90;
            for (int t = 0; t < TRIES && pool.empty(); ++t) {
                shuffle(shuf, seed);
                for (const EHeur HE : HEURS)
                    if (maxRects(c, shuf.data(), c.AW, c.AH, HE, tmp)) {
                        SEARCH(shuf.data(), HE, 1.0, 1.0, -1, NOPIN);
                        PUSH(shuf);
                        searched = orders.size();
                        break;
                    }
            }
        }
        if (pool.empty())
            return std::nullopt;

        // ------------------------------------------------ judge the candidates
        std::vector<Cand>     cands;
        std::vector<uint64_t> seen;
        const auto            ADD = [&](std::vector<IR> r) {
            const uint64_t H = hashOf(r);
            if (std::find(seen.begin(), seen.end(), H) != seen.end())
                return;
            seen.push_back(H);
            Cand C{std::move(r), {}, 0};
            C.j = judge(c, C.r);
            C.s = C.j.score + (c.prm.orderWeight > 0 ? c.prm.orderWeight * disorder(C.r) : 0.0);
            cands.push_back(std::move(C));
        };
        // only the most promising packings (by screen share and fill) are judged in full
        const auto ADDPOOL = [&]() {
            std::vector<std::pair<double, size_t>> key;
            for (size_t k = 0; k < pool.size(); ++k) {
                const IR B = bbox(pool[k]);
                key.push_back({5.0 * std::max(B.w / c.areaW, B.h / c.areaH) + 2.0 * (1.0 - area0 / (static_cast<double>(B.w) * B.h)) + stripPenalty(c, pool[k], B), k});
            }
            std::stable_sort(key.begin(), key.end());
            const size_t M = std::min(key.size(), static_cast<size_t>(SMALL ? c.prm.judgePoolSmall : c.prm.judgePoolBig));
            for (size_t q = 0; q < M; ++q) {
                auto& P = pool[key[q].second];
                centre(c, P);
                ADD(P);
                if (compact(c, P)) {
                    centre(c, P);
                    ADD(P);
                }
            }
            pool.clear();
        };
        ADDPOOL();
        const double ORDERW = c.prm.orderWeight;
        const auto   PLAIN  = [&](const std::vector<IR>& r, const Judged& j) { return j.score + (ORDERW > 0 ? ORDERW * disorder(r) : 0.0); };
        int        budget = SMALL ? c.prm.judgeSmall : c.prm.judgeBig;
        Cand       best   = polish(c, cands, PLAIN, c.prm.polishTop, budget);
        // still a hole, a split or a sizeable pocket: a second, harder look
        // (more orders and shapes, a longer polish) -- only for the hard sets
        if (best.j.hole > c.prm.hardHole || best.j.comps > 1) {
            SHUFFLES(SMALL ? c.prm.shuf2Small : c.prm.shuf2Big);
            GENERATE(5, static_cast<size_t>(SMALL ? c.prm.strip2Small : c.prm.strip2Big));
            ADDPOOL();
            cands.push_back(best);
            budget = SMALL ? c.prm.judge2Small : c.prm.judge2Big;
            Cand B2 = polish(c, cands, PLAIN, c.prm.polish2Top, budget);
            if (B2.s < best.s - 1e-9)
                best = std::move(B2);
        }
        if (!hint || hint->idx >= NS) {
            canonical(best.r);
            if (ORDERW > 0) { // the mirror image that reads most like the windows' order
                std::vector<IR> keep = best.r;
                double          d0   = disorder(keep);
                for (int m = 1; m < 4; ++m) {
                    std::vector<IR> t = keep;
                    mirror(t, m & 1, m & 2);
                    if (const double D = disorder(t); D < d0 - 1e-12)
                        d0 = D, best.r = std::move(t);
                }
            }
            // EVERY WINDOW LEANS TOWARD THE MIDDLE (user, 2026-10-14: "all bunched
            // in the center of the screen"). The reading-order tie-break above can
            // leave a window at the far end of its room (a tall window top-aligned
            // beside a stack, the space below it empty): each one slides toward the
            // cluster centre within its room -- kept only when the cluster is judged
            // no worse (no new hole, pocket or split, the same or a smaller box).
            {
                std::vector<IR> lean = best.r;
                if (compact(c, lean, 0)) {
                    centre(c, lean);
                    const Judged J0 = judge(c, best.r), J1 = judge(c, lean);
                    if (valid(c, lean) && J1.comps <= J0.comps && J1.score <= J0.score + 1e-9)
                        best.r = std::move(lean);
                }
            }
            return OUT(best.r);
        }

        // ------------------------------------------------ a Super+drag drop
        const int    DI = static_cast<int>(hint->idx);
        const double PX = hint->x, PY = hint->y;
        const double FILL0 = best.j.fill; // the same set without the drop
        const double HOLE0 = best.j.hole; // its holes + pockets part of the score
        // which side of the cluster: against a tight centred cluster of these windows
        char side = 'M';
        {
            const double A  = inflated / 0.85, RT = std::sqrt(c.areaW / c.areaH);
            double       hw = std::sqrt(A * RT), hh = A / hw;
            hw              = std::min(std::max(hw, static_cast<double>(c.maxW)), c.areaW) / 2;
            hh              = std::min(std::max(hh, static_cast<double>(c.maxH)), c.areaH) / 2;
            const double U = (PX - (area.x + area.w / 2)) / hw, V = (PY - (area.y + area.h / 2)) / hh;
            if (std::max(std::abs(U), std::abs(V)) >= 0.4)
                side = std::abs(U) >= std::abs(V) ? (U > 0 ? 'R' : 'L') : (V > 0 ? 'B' : 'T');
        }
        const double DIAG      = std::hypot(c.areaW, c.areaH);
        // A DROP IN THE MIDDLE (user, 2026-10-16: "does not let me drop a window
        // in the center"): no side to honour, so the point itself is the ask --
        // pulled 8x harder than a side drop's point. Never into a gap: any growth
        // of the holes + pockets over the same set without the drop costs 8x its
        // score, so a window set down mid-cluster finds a seam where the others
        // close around it, or keeps to its tight place (an empty bay walled in
        // by windows on three sides reads as a hole even with a corridor out).
        const double PULL      = side == 'M' ? 16.0 : 2.0;
        const double GAPGROWTH = side == 'M' ? 8.0 : 0.0;
        const auto   DROPSCORE = [&](const std::vector<IR>& r, const Judged& j) {
            const IR&    Q  = r[static_cast<size_t>(DI)];
            const IR     B  = bbox(r);
            const double QX = Q.x + Q.w / 2.0, QY = Q.y + Q.h / 2.0, BX = B.x + B.w / 2.0, BY = B.y + B.h / 2.0;
            bool         ok = true;
            if (side == 'L')
                ok = QX < BX;
            else if (side == 'R')
                ok = QX > BX;
            else if (side == 'T')
                ok = QY < BY;
            else if (side == 'B')
                ok = QY > BY;
            return j.score + (ok ? 0.0 : 3.0) + PULL * std::hypot(QX - PX, QY - PY) / DIAG + GAPGROWTH * std::max(0.0, j.hole - HOLE0) + 3.0 * std::max(0.0, FILL0 - j.fill) +
                (ORDERW > 0 ? ORDERW * disorder(r) : 0.0);
        };
        // pinned: the dropped window in the free region on its side of the box, the rest packed around it
        {
            const int  NO  = std::min<int>(static_cast<int>(orders.size()), c.prm.dropOrders);
            const auto PIN = [&](int BW, int BH) {
                const int Wd = c.W[DI], Hd = c.H[DI];
                // the drop point in box coordinates (box centred in the area)
                const double OX = c.AX + (c.AW - BW) / 2.0, OY = c.AY + (c.AH - BH) / 2.0;
                const int    X  = std::clamp(static_cast<int>(std::lround(PX - OX - Wd / 2.0)), 0, std::max(0, BW - Wd));
                const int    Y  = std::clamp(static_cast<int>(std::lround(PY - OY - Hd / 2.0)), 0, std::max(0, BH - Hd));
                switch (side) {
                    case 'L': return IR{0, Y, Wd, Hd};
                    case 'R': return IR{BW - Wd, Y, Wd, Hd};
                    case 'T': return IR{X, 0, Wd, Hd};
                    case 'B': return IR{X, BH - Hd, Wd, Hd};
                    default: return IR{X, Y, Wd, Hd};
                }
            };
            for (int o = 0; o < NO; ++o)
                for (const EHeur HE : HEURS)
                    SEARCH(orders[static_cast<size_t>(o)].data(), HE, 1.0, 1.0, DI, PIN);
            ADDPOOL();
        }
        // every candidate in its four mirror images (the polished plain layout too)
        {
            cands.push_back(best);
            const size_t NC = cands.size();
            for (size_t k = 0; k < NC; ++k)
                for (int m = 1; m < 4; ++m) {
                    Cand C = cands[k];
                    mirror(C.r, m & 1, m & 2);
                    cands.push_back(std::move(C));
                }
        }
        for (auto& C : cands)
            C.s = DROPSCORE(C.r, C.j), C.polished = false;
        budget = SMALL ? c.prm.judgeSmall : c.prm.judgeBig;
        Cand D = polish(c, cands, DROPSCORE, c.prm.polishTop, budget);
        return OUT(D.r);
    }

    inline std::optional<std::vector<SRect>> pack(const std::vector<SRect>& sizes, const SRect& area, double gap, const PackHint* hint) {
        return packImpl(sizes, area, gap, hint, 0);
    }
    inline std::optional<std::vector<SRect>> fitLayout(const std::vector<SRect>& sizes, const SRect& area, double gap, bool quick = false) {
        return packImpl(sizes, area, gap, nullptr, quick ? 2 : 1);
    }

} // namespace ScrollOverview::MosaicPack::Box

// ==========================================================================
// PART 2 -- Beam: the fit fallback ("beamcontact"), used only when Box finds nothing
// ==========================================================================
// BEAMCONTACT -- the mosaic cluster, grown from the centre one window at a time.
//
// Every window keeps its own size; they gather into ONE cluster with only the
// theme gap between neighbours, centred in the area. Nothing fits -> nullopt
// (the desktop tiles).
//
// GROWTH. The cluster starts with one window. Each next window goes into a
// spot TOUCHING the cluster: flush against a placed window at exactly the gap,
// on any of its four sides, and along that side either pushed to the end of a
// free stretch (against the next window: that is how a notch gets filled) or
// lined up with a nearby placed edge (so rows and columns come out flush).
// A beam of partial clusters goes on at every step, scored by what an organic
// cluster is:
//   * where it is heading: the windows still to come first fill the usable room
//     inside its box, the rest grows the box the cheapest (screen-shaped) way;
//     that box's screen share and fill are the estimate of the final cluster;
//   * the PERIMETER EXCESS of the union of the gap-inflated windows over its
//     bounding box: zero exactly when every row and every column through the
//     cluster is one unbroken run (no notch between windows, no hole, no
//     stray), and it grows with the depth of every notch -- so contact with
//     neighbours is rewarded and a spot that walls off a hole is penalised.
//     It costs a sum over the neighbours per spot (incremental).
// The beam keeps only distinct shapes (whichever window is which among equal
// sizes, however mirrored) and only a few of one box size, so one shape and
// its alignment variants cannot crowd the others out.
//
// SELECTION. Four placement orders (biggest area, tallest, widest, longest
// side) are grown; every finished cluster is judged exactly -- enclosed holes,
// pockets, components, screen share, fill, as the bench measures them -- the
// best is polished (each window lifted out and put back in the best spot
// touching the rest, kept only if the judge improves) and centred.
//
// TIGHT SETS. When nothing grows from the centre, the same orders are grown
// again with a beam 2x (then, for 9..16 windows, 8x) wider; when that fails
// too, the cluster is grown inside the area itself from its top-left corner
// (the walls count as neighbours, a window still has to touch the cluster),
// with more orders. Only then is the answer nullopt.
//
// A DROP (hint): the side of the cluster it was dropped on is read against the
// cluster the same set packs into without the drop (a corner drop asks for
// both sides). The cluster is grown again with the dropped window placed last
// and, separately, first, pulled to the drop point all along. Of every cluster
// seen, the one with the least judge + 3 per side it misses + 2 * distance to
// the drop point / diagonal + 3 * the fill lost against the drop-free cluster
// wins: the drop is honoured only within that compactness slack.
//
// Contract: one rect per input window in input order, w/h as given, integer
// x/y, all inside `area`, any two at least `gap` apart; deterministic (no
// randomness; the extra orders for tight sets are fixed-seed shuffles).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <optional>
#include <vector>

namespace ScrollOverview::MosaicPack::Beam {

    struct SRect {
        double x = 0, y = 0, w = 0, h = 0;
    };
    struct PackHint {
        size_t idx = 0;
        double x = 0, y = 0;
    };

    namespace BeamContact {

        // Tuned on /tmp/packer/bench (see the report): the weights mirror the
        // bench's own (share 5, fill 2, side 3, distance 2).
        inline constexpr double W_SHARE = 5, W_FILL = 2, W_EXCESS = 2, W_SIDE = 3, W_DIST = 2;
        inline constexpr double USABLE     = 0.9; // how much of the room inside a partial cluster's box later windows can use
        inline constexpr double BOTH_SIDES = 0.7; // a drop this diagonal (minor/major axis) asks for both sides
        inline constexpr double NEAR_ALIGN = 2;   // line up with placed edges within this many window sizes
        inline constexpr size_t BEAM_SMALL = 16, BEAM = 8; // beam width for <= 8 windows, for <= 20
        inline constexpr size_t SAME_BOX_MIN = 2, SAME_BOX_DIV = 3; // partial clusters of one box size: max(2, beam/3)
        inline constexpr size_t POLISH_TRIES = 3;                   // spots tried per window when polishing
        // nothing grew from the centre: the same orders with a beam 2x wider, then (9..16 windows, where the
        // budget allows it) 8x wider, before growing inside the area
        inline constexpr size_t RETRY_MUL = 2, WIDE_MUL = 8, WIDE_MIN_N = 9, WIDE_MAX_N = 16;

        constexpr double EPS = 1e-6;

        // ------------------------------------------------------------ exact judge
        // The bench's own measure of a finished cluster (holes and pockets on the
        // windows inflated by gap/2+1, components, share, fill), lower = better.
        struct SEval {
            double score = 0, S = 0, fill = 0, encl = 0, pocket = 0;
            int    holes = 0, comps = 1;
        };

        inline int componentsOf(const std::vector<SRect>& rs, double gap) {
            const size_t     N = rs.size();
            std::vector<int> p(N);
            std::iota(p.begin(), p.end(), 0);
            const auto FIND = [&](int x) {
                while (p[x] != x)
                    x = p[x] = p[p[x]];
                return x;
            };
            for (size_t i = 0; i < N; ++i)
                for (size_t j = i + 1; j < N; ++j) {
                    const double SX = std::max(rs[j].x - (rs[i].x + rs[i].w), rs[i].x - (rs[j].x + rs[j].w));
                    const double SY = std::max(rs[j].y - (rs[i].y + rs[i].h), rs[i].y - (rs[j].y + rs[j].h));
                    if ((SX <= gap + 2 && SY < -0.5) || (SY <= gap + 2 && SX < -0.5))
                        p[FIND(static_cast<int>(i))] = FIND(static_cast<int>(j));
                }
            int c = 0;
            for (size_t i = 0; i < N; ++i)
                c += FIND(static_cast<int>(i)) == static_cast<int>(i);
            return c;
        }

        inline SEval judge(const std::vector<SRect>& rs, double aw, double ah, double gap) {
            SEval        e;
            const double D = gap / 2.0 + 1.0;
            double       x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300, filled = 0;
            for (const auto& R : rs) {
                x0 = std::min(x0, R.x), y0 = std::min(y0, R.y), x1 = std::max(x1, R.x + R.w), y1 = std::max(y1, R.y + R.h);
                filled += R.w * R.h;
            }
            const double BW = x1 - x0, BH = y1 - y0, BA = std::max(1.0, BW * BH);
            e.S    = std::max(BW / aw, BH / ah);
            e.fill = filled / BA;
            e.comps = componentsOf(rs, gap);
            std::vector<double> xs, ys;
            xs.reserve(rs.size() * 2), ys.reserve(rs.size() * 2);
            for (const auto& R : rs) {
                xs.push_back(R.x - D), xs.push_back(R.x + R.w + D);
                ys.push_back(R.y - D), ys.push_back(R.y + R.h + D);
            }
            const auto UNIQ = [](std::vector<double>& v) {
                std::sort(v.begin(), v.end());
                v.erase(std::unique(v.begin(), v.end(), [](double a, double b) { return std::abs(a - b) < 1e-6; }), v.end());
            };
            UNIQ(xs), UNIQ(ys);
            const int NX = static_cast<int>(xs.size()) - 1, NY = static_cast<int>(ys.size()) - 1;
            if (NX > 0 && NY > 0) {
                std::vector<char> occ(static_cast<size_t>(NX * NY), 0);
                const auto IDX = [](const std::vector<double>& v, double t) {
                    return static_cast<int>(std::lower_bound(v.begin(), v.end(), t - 1e-6) - v.begin());
                };
                for (const auto& R : rs) {
                    const int I0 = IDX(xs, R.x - D), I1 = IDX(xs, R.x + R.w + D), J0 = IDX(ys, R.y - D), J1 = IDX(ys, R.y + R.h + D);
                    for (int j = J0; j < J1; ++j)
                        for (int i = I0; i < I1; ++i)
                            occ[static_cast<size_t>(j * NX + i)] = 1;
                }
                // outside: free cells reachable from the border
                std::vector<char> out(occ.size(), 0);
                std::vector<int>  st;
                for (int j = 0; j < NY; ++j)
                    for (int i = 0; i < NX; ++i)
                        if ((i == 0 || j == 0 || i == NX - 1 || j == NY - 1) && !occ[static_cast<size_t>(j * NX + i)]) {
                            out[static_cast<size_t>(j * NX + i)] = 1;
                            st.push_back(j * NX + i);
                        }
                while (!st.empty()) {
                    const int K = st.back();
                    st.pop_back();
                    const int I = K % NX, J = K / NX;
                    const int NB[4][2] = {{I + 1, J}, {I - 1, J}, {I, J + 1}, {I, J - 1}};
                    for (const auto& Q : NB) {
                        if (Q[0] < 0 || Q[1] < 0 || Q[0] >= NX || Q[1] >= NY)
                            continue;
                        const size_t KK = static_cast<size_t>(Q[1] * NX + Q[0]);
                        if (!occ[KK] && !out[KK]) {
                            out[KK] = 1;
                            st.push_back(Q[1] * NX + Q[0]);
                        }
                    }
                }
                std::vector<char> pk(occ.size(), 0);
                for (int j = 0; j < NY; ++j) {
                    int a = -1, b = -1;
                    for (int i = 0; i < NX; ++i)
                        if (occ[static_cast<size_t>(j * NX + i)]) {
                            if (a < 0)
                                a = i;
                            b = i;
                        }
                    for (int i = a + 1; i < b; ++i)
                        if (!occ[static_cast<size_t>(j * NX + i)])
                            pk[static_cast<size_t>(j * NX + i)] = 1;
                }
                for (int i = 0; i < NX; ++i) {
                    int a = -1, b = -1;
                    for (int j = 0; j < NY; ++j)
                        if (occ[static_cast<size_t>(j * NX + i)]) {
                            if (a < 0)
                                a = j;
                            b = j;
                        }
                    for (int j = a + 1; j < b; ++j)
                        if (!occ[static_cast<size_t>(j * NX + i)])
                            pk[static_cast<size_t>(j * NX + i)] = 1;
                }
                // enclosed components: total, and whether any is significant
                std::vector<char> seen(occ.size(), 0);
                const double      MINA = 4 * gap * gap;
                for (int s = 0; s < NX * NY; ++s) {
                    const size_t S0 = static_cast<size_t>(s);
                    if (pk[S0] || (!occ[S0] && !out[S0]))
                        e.pocket += (xs[s % NX + 1] - xs[s % NX]) * (ys[s / NX + 1] - ys[s / NX]);
                    if (occ[S0] || out[S0] || seen[S0])
                        continue;
                    double area = 0, cx0 = 1e300, cy0 = 1e300, cx1 = -1e300, cy1 = -1e300;
                    st.assign(1, s);
                    seen[S0] = 1;
                    while (!st.empty()) {
                        const int K = st.back();
                        st.pop_back();
                        const int I = K % NX, J = K / NX;
                        area += (xs[I + 1] - xs[I]) * (ys[J + 1] - ys[J]);
                        cx0 = std::min(cx0, xs[I]), cy0 = std::min(cy0, ys[J]), cx1 = std::max(cx1, xs[I + 1]), cy1 = std::max(cy1, ys[J + 1]);
                        const int NB[4][2] = {{I + 1, J}, {I - 1, J}, {I, J + 1}, {I, J - 1}};
                        for (const auto& Q : NB) {
                            if (Q[0] < 0 || Q[1] < 0 || Q[0] >= NX || Q[1] >= NY)
                                continue;
                            const size_t KK = static_cast<size_t>(Q[1] * NX + Q[0]);
                            if (!occ[KK] && !out[KK] && !seen[KK]) {
                                seen[KK] = 1;
                                st.push_back(Q[1] * NX + Q[0]);
                            }
                        }
                    }
                    e.encl += area;
                    if (area >= MINA && cx1 - cx0 >= gap && cy1 - cy0 >= gap)
                        ++e.holes;
                }
            }
            e.score = (e.comps > 1 ? std::min(60.0, 25.0 + 5.0 * (e.comps - 2)) : 0.0) + (e.holes > 0 ? 6.0 : 0.0) + 30.0 * e.encl / BA + 10.0 * e.pocket / BA + 5.0 * e.S +
                2.0 * (1.0 - e.fill);
            return e;
        }

        // ------------------------------------------------------------ the beam
        // A partial cluster: the placed items -- the area's four walls first when
        // the cluster is grown inside the area itself (anchored), then the windows
        // in placement order.
        struct SNode {
            std::vector<SRect> it;
            double             x0 = 0, y0 = 0, x1 = 0, y1 = 0, perim = 0, area = 0, infl = 0, J = 0;
        };
        struct SChild {
            uint32_t parent = 0;
            double   x = 0, y = 0, x0 = 0, y0 = 0, x1 = 0, y1 = 0, perim = 0, J = 0;
        };

        struct SCtx {
            const std::vector<SRect>* sizes = nullptr;
            double                    aw = 0, ah = 0, g = 0, diag = 1;
            double                    totalArea = 0, totalInfl = 0; // every window's area, and gap-inflated
            bool                      anchored  = false;
            size_t                    walls     = 0;
            // a drop: window dropIdx should end with its centre at (tx, ty) relative to the cluster's centre,
            // on side sx (-1 left, +1 right, 0 either) and sy (-1 above, +1 below, 0 either) of it
            bool   drop    = false;
            size_t dropIdx = 0;
            double tx = 0, ty = 0;
            int    sx = 0, sy = 0;
        };

        // What window (x, y, w, h) shares with the placed items, measured on the
        // gap/2-inflated rects (up to 2 px of rounding slack counts as touching):
        // the union's perimeter drops by twice this.
        inline double shared(const std::vector<SRect>& items, double x, double y, double w, double h, double g) {
            double sh = 0;
            for (const auto& Q : items) {
                const double SX = std::max(Q.x - (x + w), x - (Q.x + Q.w)), SY = std::max(Q.y - (y + h), y - (Q.y + Q.h));
                if (SX >= g - EPS && SX <= g + 2 + EPS && SY < 0) {
                    const double OV = std::min(y + h, Q.y + Q.h) - std::max(y, Q.y) + g;
                    if (OV > 0)
                        sh += OV;
                } else if (SY >= g - EPS && SY <= g + 2 + EPS && SX < 0) {
                    const double OV = std::min(x + w, Q.x + Q.w) - std::max(x, Q.x) + g;
                    if (OV > 0)
                        sh += OV;
                }
            }
            return sh;
        }

        // A partial cluster judged by where it is heading (see the top). Lower =
        // better. Anchored (inside the area): as much contact as possible.
        inline double stateJ(const SCtx& C, double x0, double y0, double x1, double y1, double perim, double infl) {
            if (C.anchored)
                return perim / (C.aw + C.ah);
            const double BW = x1 - x0, BH = y1 - y0, g = C.g;
            const double EX = std::max(0.0, perim - 2 * (BW + BH + 2 * g));
            double       FW = BW, FH = BH; // the estimated final box
            double       more = (C.totalInfl - infl) - std::max(0.0, (BW + g) * (BH + g) - infl) * USABLE;
            if (more > 0) {
                // grow the relatively smaller side up to the larger one, then both at the screen's shape
                if (FW / C.aw < FH / C.ah) {
                    const double ADD = std::min(more, (FH / C.ah * C.aw - FW) * (FH + g));
                    FW += ADD / (FH + g), more -= ADD;
                } else {
                    const double ADD = std::min(more, (FW / C.aw * C.ah - FH) * (FW + g));
                    FH += ADD / (FW + g), more -= ADD;
                }
                if (more > 0) {
                    const double K = std::sqrt(((FW + g) * (FH + g) + more) / ((FW + g) * (FH + g)));
                    FW = (FW + g) * K - g, FH = (FH + g) * K - g;
                }
            }
            const double S = std::max(FW / C.aw, FH / C.ah);
            const double F = std::min(1.0, C.totalArea / std::max(1.0, FW * FH));
            return W_SHARE * S + W_FILL * (1 - F) + W_EXCESS * EX / (BW + BH + 2 * g);
        }

        // The sides of (cx, cy) -- a window's centre relative to the cluster's
        // centre -- that the drop asked for and it misses.
        inline int sidesMissed(const SCtx& C, double cx, double cy) {
            return (C.sx * cx < 0 || (C.sx && cx == 0)) + (C.sy * cy < 0 || (C.sy && cy == 0));
        }

        // The dropped window's pull on a partial cluster: its centre against the
        // drop point, both relative to the cluster's centre (it ends up centred).
        inline double dropPull(const SCtx& C, const SRect& Q, double x0, double y0, double x1, double y1) {
            const double CX = Q.x + Q.w / 2 - (x0 + x1) / 2, CY = Q.y + Q.h / 2 - (y0 + y1) / 2;
            return W_DIST * std::hypot(CX - C.tx, CY - C.ty) / C.diag + W_SIDE * sidesMissed(C, CX, CY);
        }

        // Every spot for window (w, h) touching the cluster: flush against a placed
        // item on one of its four sides (at the gap), slid along that side to the
        // ends of each free stretch (= against the next window: a notch), to the
        // middle of the item and to every nearby placed edge it can line up with.
        // The box stays within the area (anchored: the window stays inside it).
        template <class F>
        inline void spots(const SNode& n, double w, double h, const SCtx& C, std::vector<std::pair<double, double>>& forb, std::vector<double>& cand, F&& emit) {
            const double g = C.g;
            const size_t K = n.it.size();
            for (size_t p = 0; p < K; ++p) {
                if (C.anchored && p < C.walls && (K > C.walls || (p != 0 && p != 2)))
                    continue; // inside the area: the first window in the top-left corner, then flush against a window
                const SRect& P = n.it[p];
                for (int side = 0; side < 4; ++side) {
                    const bool HORIZ = side < 2; // beside P (left/right): slide along y
                    const double fixed = side == 0 ? std::ceil(P.x + P.w + g - EPS) // right of P
                        : side == 1                ? std::floor(P.x - g - w + EPS)  // left of P
                        : side == 2                ? std::ceil(P.y + P.h + g - EPS) // below P
                                                   : std::floor(P.y - g - h + EPS); // above P
                    const double FL = HORIZ ? w : h, SL = HORIZ ? h : w;
                    if (C.anchored) {
                        if (fixed < -EPS || fixed + FL > (HORIZ ? C.aw : C.ah) + EPS)
                            continue;
                    } else if (n.x1 > n.x0) {
                        const double A0 = HORIZ ? n.x0 : n.y0, A1 = HORIZ ? n.x1 : n.y1, AL = HORIZ ? C.aw : C.ah;
                        if (std::max(A1, fixed + FL) - std::min(A0, fixed) > AL + EPS)
                            continue;
                    }
                    // slide range: touch P by a little at least, keep the box within the area
                    const double PS = HORIZ ? P.y : P.x, PL = HORIZ ? P.h : P.w;
                    const double MINOV = std::min({20.0, PL / 4, SL / 4});
                    double       lo = PS - SL + MINOV, hi = PS + PL - MINOV;
                    if (C.anchored)
                        lo = std::max(lo, 0.0), hi = std::min(hi, (HORIZ ? C.ah : C.aw) - SL);
                    else if (n.x1 > n.x0) {
                        const double B0 = HORIZ ? n.y0 : n.x0, B1 = HORIZ ? n.y1 : n.x1, BL = HORIZ ? C.ah : C.aw;
                        lo = std::max(lo, B1 - BL), hi = std::min(hi, B0 + BL - SL);
                    }
                    lo = std::ceil(lo - EPS), hi = std::floor(hi + EPS);
                    if (lo > hi)
                        continue;
                    // obstacles: placed items closer than the gap across the fixed axis -> open intervals of slide values
                    forb.clear();
                    for (size_t t = 0; t < K; ++t) {
                        if (t == p)
                            continue;
                        const SRect& Q  = n.it[t];
                        const double QF = HORIZ ? Q.x : Q.y, QFL = HORIZ ? Q.w : Q.h;
                        const double QS = HORIZ ? Q.y : Q.x, QSL = HORIZ ? Q.h : Q.w;
                        if (QF < fixed + FL + g - EPS && QF + QFL > fixed - g + EPS)
                            forb.push_back({QS - g - SL, QS + QSL + g});
                    }
                    cand.clear();
                    for (size_t t = C.walls; t < K; ++t) {
                        const SRect& Q  = n.it[t];
                        const double QF = HORIZ ? Q.x : Q.y, QFL = HORIZ ? Q.w : Q.h, R = NEAR_ALIGN * FL + g;
                        if (QF > fixed + FL + R || QF + QFL < fixed - R)
                            continue;
                        const double QS = HORIZ ? Q.y : Q.x, QSL = HORIZ ? Q.h : Q.w;
                        cand.push_back(std::round(QS));
                        cand.push_back(std::round(QS + QSL - SL));
                    }
                    if (p >= C.walls)
                        cand.push_back(std::round(PS + (PL - SL) / 2));
                    const auto EMIT = [&](double s) {
                        if (HORIZ)
                            emit(fixed, s);
                        else
                            emit(s, fixed);
                    };
                    double cur = lo;
                    while (cur <= hi) {
                        for (bool moved = true; moved;) { // past every forbidden interval covering cur
                            moved = false;
                            for (const auto& FB : forb)
                                if (FB.first < cur - EPS && FB.second > cur + EPS) {
                                    cur   = std::ceil(FB.second - EPS);
                                    moved = true;
                                }
                        }
                        if (cur > hi)
                            break;
                        double end = hi; // the free stretch ends where the next forbidden interval starts
                        for (const auto& FB : forb)
                            if (FB.first >= cur - EPS)
                                end = std::min(end, std::floor(FB.first + EPS));
                        if (end < cur) {
                            cur += 1;
                            continue;
                        }
                        EMIT(cur);
                        if (end > cur)
                            EMIT(end);
                        for (const double V : cand)
                            if (V > cur && V < end)
                                EMIT(V);
                        cur = end + 1;
                    }
                }
            }
        }

        // A partial cluster's shape, whichever window is which (equal sizes swap
        // freely) and however it is mirrored: equal keys grow the same clusters.
        inline uint64_t mix64(uint64_t z) {
            z += 0x9E3779B97F4A7C15ULL;
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
            return z ^ (z >> 31);
        }
        inline uint64_t keyOf(const SNode& n, size_t from, const SRect& add, double x0, double y0, double x1, double y1, bool mirror) {
            uint64_t   k[4] = {0, 0, 0, 0};
            const auto ONE  = [&](const SRect& R) {
                const int64_t W = std::llround(R.w * 4), H = std::llround(R.h * 4);
                const int64_t X[2] = {std::llround(R.x - x0), std::llround(x1 - R.x - R.w)}, Y[2] = {std::llround(R.y - y0), std::llround(y1 - R.y - R.h)};
                for (int m = 0; m < (mirror ? 4 : 1); ++m)
                    k[m] += mix64(static_cast<uint64_t>(X[m & 1]) * 0x100000001B3ULL ^ mix64(static_cast<uint64_t>(Y[m >> 1]) ^ mix64(static_cast<uint64_t>(W) * 31 + static_cast<uint64_t>(H))));
            };
            for (size_t t = from; t < n.it.size(); ++t)
                ONE(n.it[t]);
            ONE(add);
            return mirror ? std::min({k[0], k[1], k[2], k[3]}) : k[0];
        }

        // The area's walls as items just outside it (at the gap): flush against
        // one is flush against the area's edge.
        inline std::vector<SRect> wallsOf(const SCtx& C) {
            const double T = 10, g = C.g;
            return {SRect{-g - T, -g - T, T, C.ah + 2 * g + 2 * T}, SRect{C.aw + g, -g - T, T, C.ah + 2 * g + 2 * T}, SRect{-g - T, -g - T, C.aw + 2 * g + 2 * T, T},
                    SRect{-g - T, C.ah + g, C.aw + 2 * g + 2 * T, T}};
        }

        // One placement order grown as a beam of width B. Returns the finished
        // clusters (positions by window index, in the cluster's own frame).
        inline std::vector<std::vector<SRect>> grow(const std::vector<size_t>& order, const SCtx& C, size_t B) {
            const auto&        S = *C.sizes;
            const size_t       N = order.size();
            const double       g = C.g;
            std::vector<SNode> beam(1);
            size_t             k0 = 0;
            if (C.anchored) {
                beam[0].it = wallsOf(C);
                beam[0].x1 = C.aw, beam[0].y1 = C.ah;
            } else {
                SNode&       n = beam[0];
                const SRect& F = S[order[0]];
                n.it.push_back(SRect{0, 0, F.w, F.h});
                n.x1 = F.w, n.y1 = F.h;
                n.perim = 2 * (F.w + g + F.h + g);
                n.area  = F.w * F.h;
                n.infl  = (F.w + g) * (F.h + g);
                k0      = 1;
            }
            const size_t                           SAME = std::max(SAME_BOX_MIN, B / SAME_BOX_DIV);
            std::vector<SChild>                    kids;
            std::vector<std::pair<double, double>> forb, pts;
            std::vector<double>                    cand;
            std::vector<uint32_t>                  idx;
            std::vector<uint64_t>                  keys;
            for (size_t k = k0; k < N; ++k) {
                const SRect& W       = S[order[k]];
                const bool   DROPPED = C.drop && order[k] == C.dropIdx;
                long         dropAt  = -1; // the dropped window, once placed, keeps pulling
                for (size_t t = 0; C.drop && t < k; ++t)
                    if (order[t] == C.dropIdx)
                        dropAt = static_cast<long>(C.walls + t);
                kids.clear();
                for (uint32_t pi = 0; pi < beam.size(); ++pi) {
                    const SNode& n = beam[pi];
                    pts.clear();
                    spots(n, W.w, W.h, C, forb, cand, [&](double x, double y) { pts.push_back({x, y}); });
                    std::sort(pts.begin(), pts.end());
                    pts.erase(std::unique(pts.begin(), pts.end()), pts.end());
                    for (const auto& [x, y] : pts) {
                        SChild c;
                        c.parent = pi, c.x = x, c.y = y;
                        if (C.anchored)
                            c.x0 = 0, c.y0 = 0, c.x1 = C.aw, c.y1 = C.ah;
                        else
                            c.x0 = std::min(n.x0, x), c.y0 = std::min(n.y0, y), c.x1 = std::max(n.x1, x + W.w), c.y1 = std::max(n.y1, y + W.h);
                        c.perim = n.perim + 2 * (W.w + g + W.h + g) - 2 * shared(n.it, x, y, W.w, W.h, g);
                        c.J     = stateJ(C, c.x0, c.y0, c.x1, c.y1, c.perim, n.infl + (W.w + g) * (W.h + g));
                        if (DROPPED)
                            c.J += dropPull(C, SRect{x, y, W.w, W.h}, c.x0, c.y0, c.x1, c.y1);
                        else if (dropAt >= 0)
                            c.J += dropPull(C, n.it[static_cast<size_t>(dropAt)], c.x0, c.y0, c.x1, c.y1);
                        kids.push_back(c);
                    }
                }
                if (kids.empty())
                    return {};
                // the best distinct children, at most SAME of one box size
                idx.resize(kids.size());
                std::iota(idx.begin(), idx.end(), 0u);
                const auto BYJ  = [&](uint32_t a, uint32_t b) { return kids[a].J < kids[b].J || (kids[a].J == kids[b].J && a < b); };
                size_t     done = std::min(idx.size(), 3 * B);
                if (done < idx.size())
                    std::nth_element(idx.begin(), idx.begin() + static_cast<std::ptrdiff_t>(done), idx.end(), BYJ);
                std::sort(idx.begin(), idx.begin() + static_cast<std::ptrdiff_t>(done), BYJ);
                std::vector<SNode> next;
                keys.clear();
                for (size_t q = 0; q < idx.size() && next.size() < B; ++q) {
                    if (q == done) { // not enough distinct ones among the best: order the rest too
                        std::sort(idx.begin() + static_cast<std::ptrdiff_t>(done), idx.end(), BYJ);
                        done = idx.size();
                    }
                    const SChild& c = kids[idx[q]];
                    const SNode&  n = beam[c.parent];
                    if (!C.anchored) {
                        size_t same = 0;
                        for (const auto& m : next)
                            same += std::abs((m.x1 - m.x0) - (c.x1 - c.x0)) < 0.5 && std::abs((m.y1 - m.y0) - (c.y1 - c.y0)) < 0.5;
                        if (same >= SAME)
                            continue;
                    }
                    const uint64_t KEY = keyOf(n, C.walls, SRect{c.x, c.y, W.w, W.h}, c.x0, c.y0, c.x1, c.y1, !C.anchored);
                    if (std::find(keys.begin(), keys.end(), KEY) != keys.end())
                        continue;
                    keys.push_back(KEY);
                    SNode m;
                    m.it.reserve(C.walls + N);
                    m.it = n.it;
                    m.it.push_back(SRect{c.x, c.y, W.w, W.h});
                    m.x0 = c.x0, m.y0 = c.y0, m.x1 = c.x1, m.y1 = c.y1, m.perim = c.perim, m.J = c.J;
                    m.area = n.area + W.w * W.h, m.infl = n.infl + (W.w + g) * (W.h + g);
                    next.push_back(std::move(m));
                }
                beam = std::move(next);
            }
            std::vector<std::vector<SRect>> outs;
            for (const auto& n : beam) {
                std::vector<SRect> r(N);
                for (size_t t = 0; t < N; ++t)
                    r[order[t]] = n.it[C.walls + t];
                outs.push_back(std::move(r));
            }
            return outs;
        }

        struct SBox {
            double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
        };
        inline SBox boxOf(const std::vector<SRect>& r) {
            SBox b;
            for (const auto& R : r)
                b.x0 = std::min(b.x0, R.x), b.y0 = std::min(b.y0, R.y), b.x1 = std::max(b.x1, R.x + R.w), b.y1 = std::max(b.y1, R.y + R.h);
            return b;
        }

        // The cluster moved (by whole pixels) so its box is centred in the area.
        inline std::vector<SRect> centred(std::vector<SRect> r, const SRect& area) {
            const SBox   B  = boxOf(r);
            const double DX = std::clamp(std::round(area.x + (area.w - (B.x1 - B.x0)) / 2 - B.x0), std::ceil(area.x - B.x0 - EPS), std::floor(area.x + area.w - B.x1 + EPS));
            const double DY = std::clamp(std::round(area.y + (area.h - (B.y1 - B.y0)) / 2 - B.y0), std::ceil(area.y - B.y0 - EPS), std::floor(area.y + area.h - B.y1 + EPS));
            for (auto& R : r)
                R.x += DX, R.y += DY;
            return r;
        }

        // Placement orders: biggest area, tallest, widest, longest side first
        // (ties: input order). `many` (a tight set): also by half-perimeter,
        // shortest side, as given, and six fixed-seed shuffles of the area order.
        inline std::vector<std::vector<size_t>> orders(const std::vector<SRect>& S, bool many) {
            const size_t        N = S.size();
            std::vector<size_t> base(N);
            std::iota(base.begin(), base.end(), size_t{0});
            std::vector<std::vector<size_t>> out;
            const auto ADD = [&](std::vector<size_t> o) {
                if (std::find(out.begin(), out.end(), o) == out.end())
                    out.push_back(std::move(o));
            };
            const auto BY = [&](auto key) {
                auto o = base;
                std::stable_sort(o.begin(), o.end(), [&](size_t a, size_t b) { return key(S[a]) > key(S[b]) + 1e-9; });
                ADD(std::move(o));
            };
            BY([](const SRect& s) { return s.w * s.h; });
            BY([](const SRect& s) { return s.h; });
            BY([](const SRect& s) { return s.w; });
            BY([](const SRect& s) { return std::max(s.w, s.h); });
            if (many) {
                BY([](const SRect& s) { return s.w + s.h; });
                BY([](const SRect& s) { return std::min(s.w, s.h); });
                ADD(base);
                auto     o = out[0];
                uint64_t r = 0x9E3779B97F4A7C15ULL;
                for (int k = 0; k < 6 && N > 3; ++k) {
                    for (size_t i = N - 1; i > 0; --i) {
                        r ^= r << 13, r ^= r >> 7, r ^= r << 17;
                        std::swap(o[i], o[r % (i + 1)]);
                    }
                    ADD(o);
                }
            }
            return out;
        }

        // POLISH: lift each window out of a finished cluster and put it back in
        // one of the best spots touching the rest (the growth's own spots, ranked
        // by the growth's score), kept only when `cost` (the exact judge, plus the
        // drop's terms for a drop) improves. One sweep.
        template <class FCost>
        inline void polish(std::vector<SRect>& r, double& best, const SCtx& C0, const SRect& area, FCost&& cost) {
            const size_t N = r.size();
            const double g = C0.g;
            SCtx         C = C0;
            C.drop         = false;
            std::vector<std::pair<double, double>> forb, pts;
            std::vector<double>                    cand;
            std::vector<std::pair<double, size_t>> ranked;
            for (size_t i = 0; i < N; ++i) {
                SNode n;
                for (size_t t = 0; t < N; ++t)
                    if (t != i) {
                        n.perim += 2 * (r[t].w + g + r[t].h + g) - 2 * shared(n.it, r[t].x, r[t].y, r[t].w, r[t].h, g);
                        n.it.push_back(r[t]);
                        n.infl += (r[t].w + g) * (r[t].h + g);
                    }
                const SBox BX = boxOf(n.it);
                n.x0 = BX.x0, n.y0 = BX.y0, n.x1 = BX.x1, n.y1 = BX.y1;
                const SRect& W = r[i];
                pts.clear();
                spots(n, W.w, W.h, C, forb, cand, [&](double x, double y) { pts.push_back({x, y}); });
                std::sort(pts.begin(), pts.end());
                pts.erase(std::unique(pts.begin(), pts.end()), pts.end());
                ranked.clear();
                for (size_t k = 0; k < pts.size(); ++k) {
                    const auto [x, y] = pts[k];
                    if (x == W.x && y == W.y)
                        continue;
                    const double PER = n.perim + 2 * (W.w + g + W.h + g) - 2 * shared(n.it, x, y, W.w, W.h, g);
                    ranked.push_back({stateJ(C, std::min(n.x0, x), std::min(n.y0, y), std::max(n.x1, x + W.w), std::max(n.y1, y + W.h), PER, C.totalInfl), k});
                }
                const size_t T = std::min(POLISH_TRIES, ranked.size());
                std::partial_sort(ranked.begin(), ranked.begin() + static_cast<std::ptrdiff_t>(T), ranked.end());
                for (size_t q = 0; q < T; ++q) {
                    std::vector<SRect> t = r;
                    t[i].x = pts[ranked[q].second].first, t[i].y = pts[ranked[q].second].second;
                    t = centred(t, area);
                    if (const double V = cost(t); V < best - 1e-9) {
                        best = V, r = std::move(t);
                        break;
                    }
                }
            }
        }

        struct SCandidate {
            std::vector<SRect> r;
            SEval              e;
        };
    }

    inline std::optional<std::vector<SRect>> pack(const std::vector<SRect>& sizes, const SRect& area, double gap, const PackHint* hint) {
        using namespace BeamContact;
        const size_t N = sizes.size();
        if (N == 0)
            return std::vector<SRect>{};
        if (!(area.w > 0) || !(area.h > 0) || !std::isfinite(gap))
            return std::nullopt;
        // what provably cannot fit: a window bigger than the area, more (gap-inflated) window area than the
        // area, two windows that can neither sit side by side nor stack, or windows taller than half the area
        // (they cannot stack, so they need disjoint columns) wider than the area -- and the same across
        double inflated = 0, tallW = -gap, wideH = -gap;
        for (const auto& s : sizes) {
            if (!std::isfinite(s.w) || !std::isfinite(s.h) || s.w > area.w + EPS || s.h > area.h + EPS)
                return std::nullopt;
            inflated += (s.w + gap) * (s.h + gap);
            if (2 * s.h + gap > area.h + EPS)
                tallW += s.w + gap;
            if (2 * s.w + gap > area.w + EPS)
                wideH += s.h + gap;
        }
        if (inflated > (area.w + gap) * (area.h + gap) + EPS || tallW > area.w + EPS || wideH > area.h + EPS)
            return std::nullopt;
        for (size_t i = 0; i < N; ++i)
            for (size_t j = i + 1; j < N; ++j)
                if (sizes[i].w + gap + sizes[j].w > area.w + EPS && sizes[i].h + gap + sizes[j].h > area.h + EPS)
                    return std::nullopt;
        if (N == 1)
            return centred({SRect{0, 0, sizes[0].w, sizes[0].h}}, area);

        SCtx C;
        C.sizes = &sizes, C.aw = area.w, C.ah = area.h, C.g = gap, C.diag = std::hypot(area.w, area.h);
        C.totalInfl = inflated;
        for (const auto& s : sizes)
            C.totalArea += s.w * s.h;
        // the beam: wide for a handful of windows, narrower for many (past 20 there is no budget: keep it bounded)
        const size_t B = N <= 8 ? BEAM_SMALL : N <= 20 ? BEAM : std::max<size_t>(3, 160 / N);

        std::vector<SCandidate> cands;
        const auto              ADD = [&](const std::vector<SRect>& r) {
            SCandidate c;
            c.r = centred(r, area);
            c.e = judge(c.r, area.w, area.h, gap);
            cands.push_back(std::move(c));
        };
        for (const auto& O : orders(sizes, false))
            for (const auto& R : grow(O, C, B))
                ADD(R);
        // nothing grew from the centre: wider beams (barely-fitting sets need them), as wide as the budget allows
        for (const size_t MUL : {RETRY_MUL, N >= WIDE_MIN_N && N <= WIDE_MAX_N ? WIDE_MUL : size_t{0}})
            if (cands.empty() && MUL > 1)
                for (const auto& O : orders(sizes, false))
                    for (const auto& R : grow(O, C, B * MUL))
                        ADD(R);
        if (cands.empty()) { // still nothing: grow inside the area, from its corner
            SCtx A     = C;
            A.anchored = true, A.walls = 4;
            for (const auto& O : orders(sizes, N <= 20))
                for (const auto& R : grow(O, A, B))
                    ADD(R);
        }
        if (cands.empty())
            return std::nullopt;
        const auto BEST = [&](auto cost) {
            size_t bi = 0;
            double bs = 1e300;
            for (size_t i = 0; i < cands.size(); ++i)
                if (const double T = cost(cands[i]); T < bs - 1e-9)
                    bs = T, bi = i;
            return bi;
        };
        size_t B0 = BEST([](const SCandidate& c) { return c.e.score; });
        {
            double v = cands[B0].e.score;
            polish(cands[B0].r, v, C, area, [&](const std::vector<SRect>& r) { return judge(r, area.w, area.h, gap).score; });
            cands[B0].e = judge(cands[B0].r, area.w, area.h, gap);
        }
        if (!hint || hint->idx >= N)
            return cands[B0].r;

        // ---- a drop: which side of the drop-free cluster it landed on
        const double F0  = cands[B0].e.fill;
        const SBox   P0  = boxOf(cands[B0].r);
        const double ACX = area.x + area.w / 2, ACY = area.y + area.h / 2;
        const double U = (hint->x - ACX) / std::max(1.0, (P0.x1 - P0.x0) / 2), V = (hint->y - ACY) / std::max(1.0, (P0.y1 - P0.y0) / 2);
        const double TOP = std::max(std::abs(U), std::abs(V));
        C.drop = true, C.dropIdx = hint->idx, C.tx = hint->x - ACX, C.ty = hint->y - ACY;
        if (TOP >= 0.4) { // in its middle: no side
            if (std::abs(U) >= BOTH_SIDES * TOP)
                C.sx = U > 0 ? 1 : -1;
            if (std::abs(V) >= BOTH_SIDES * TOP)
                C.sy = V > 0 ? 1 : -1;
        }
        // grown again with the dropped window placed last, and grown around it
        auto O = orders(sizes, false)[0];
        O.erase(std::find(O.begin(), O.end(), hint->idx));
        O.push_back(hint->idx);
        for (const auto& R : grow(O, C, B))
            ADD(R);
        std::rotate(O.rbegin(), O.rbegin() + 1, O.rend());
        for (const auto& R : grow(O, C, B))
            ADD(R);
        const auto DROPCOST = [&](const std::vector<SRect>& r, const SEval& e) {
            const SBox   BX = boxOf(r);
            const SRect& Q  = r[hint->idx];
            const double QX = Q.x + Q.w / 2, QY = Q.y + Q.h / 2;
            return e.score + W_SIDE * sidesMissed(C, QX - (BX.x0 + BX.x1) / 2, QY - (BX.y0 + BX.y1) / 2) + W_DIST * std::hypot(QX - hint->x, QY - hint->y) / C.diag +
                3.0 * std::max(0.0, F0 - e.fill);
        };
        const size_t BD = BEST([&](const SCandidate& c) { return DROPCOST(c.r, c.e); });
        double       v  = DROPCOST(cands[BD].r, cands[BD].e);
        polish(cands[BD].r, v, C, area, [&](const std::vector<SRect>& r) { return DROPCOST(r, judge(r, area.w, area.h, gap)); });
        return cands[BD].r;
    }

}

// ==========================================================================
// PART 3 -- the public API: pack(), fitLayout(), packEased(), fitsEased(), limitFor()
// ==========================================================================

namespace ScrollOverview::MosaicPack {

#ifndef MOSAICPACK_HAVE_SRECT // MosaicPack.hpp defines SRect itself and sets this
    struct SRect {
        double x = 0, y = 0, w = 0, h = 0;
    };
#endif
    struct PackHint {
        size_t idx = 0;   // the window that was Super+dragged
        double x = 0, y = 0; // where it was let go, global logical px
    };

    namespace Final {
        // The contract, exactly: no rounding slack anywhere.
        inline bool contractOk(const std::vector<SRect>& sizes, const SRect& area, double gap, const std::vector<SRect>& out) {
            if (out.size() != sizes.size())
                return false;
            const double EPS = 1e-9;
            for (size_t i = 0; i < out.size(); ++i) {
                const SRect& R = out[i];
                if (!std::isfinite(R.x) || !std::isfinite(R.y) || R.x != std::round(R.x) || R.y != std::round(R.y))
                    return false;
                if (R.w != sizes[i].w || R.h != sizes[i].h)
                    return false;
                if (R.x < area.x - EPS || R.y < area.y - EPS || R.x + R.w > area.x + area.w + EPS || R.y + R.h > area.y + area.h + EPS)
                    return false;
            }
            for (size_t i = 0; i < out.size(); ++i)
                for (size_t j = i + 1; j < out.size(); ++j) {
                    const SRect& A  = out[i];
                    const SRect& B  = out[j];
                    const double SX = std::max(B.x - (A.x + A.w), A.x - (B.x + B.w));
                    const double SY = std::max(B.y - (A.y + A.h), A.y - (B.y + B.h));
                    if (std::max(SX, SY) < gap - EPS)
                        return false;
                }
            return true;
        }
        // A layout that overhangs a fractional area edge by less than a pixel
        // (centring on a non-integer area): moved by whole pixels back inside.
        inline void nudgeInside(const SRect& area, std::vector<SRect>& out) {
            if (out.empty())
                return;
            double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
            for (const SRect& R : out)
                x0 = std::min(x0, R.x), y0 = std::min(y0, R.y), x1 = std::max(x1, R.x + R.w), y1 = std::max(y1, R.y + R.h);
            double dx = 0, dy = 0;
            if (x0 < area.x)
                dx = std::ceil(area.x - x0);
            else if (x1 > area.x + area.w)
                dx = -std::ceil(x1 - (area.x + area.w));
            if (y0 < area.y)
                dy = std::ceil(area.y - y0);
            else if (y1 > area.y + area.h)
                dy = -std::ceil(y1 - (area.y + area.h));
            for (SRect& R : out)
                R.x += dx, R.y += dy;
        }
        template <class NS_SRECT, class NS_HINT, class F>
        inline std::optional<std::vector<SRect>> run(F&& packer, const std::vector<SRect>& sizes, const SRect& area, double gap, const PackHint* hint) {
            std::vector<NS_SRECT> in;
            in.reserve(sizes.size());
            for (const SRect& S : sizes) {
                NS_SRECT r{};
                r.w = S.w, r.h = S.h;
                in.push_back(r);
            }
            NS_SRECT A{};
            A.x = area.x, A.y = area.y, A.w = area.w, A.h = area.h;
            NS_HINT H{};
            if (hint)
                H.idx = hint->idx, H.x = hint->x, H.y = hint->y;
            auto R = packer(in, A, gap, hint ? &H : nullptr);
            if (!R)
                return std::nullopt;
            std::vector<SRect> out;
            out.reserve(R->size());
            for (const auto& Q : *R) {
                SRect s{};
                s.x = Q.x, s.y = Q.y, s.w = Q.w, s.h = Q.h;
                out.push_back(s);
            }
            return out;
        }
    }

    inline std::optional<std::vector<SRect>> pack(const std::vector<SRect>& sizes, const SRect& area, double gap, const PackHint* hint) {
        if (sizes.empty())
            return std::vector<SRect>{};
        if (!std::isfinite(area.x) || !std::isfinite(area.y) || !std::isfinite(area.w) || !std::isfinite(area.h) || area.w < 1 || area.h < 1 || !std::isfinite(gap))
            return std::nullopt;
        for (const SRect& S : sizes)
            if (!std::isfinite(S.w) || !std::isfinite(S.h) || S.w <= 0 || S.h <= 0 || S.w > area.w || S.h > area.h)
                return std::nullopt;
        gap = std::max(0.0, gap);
        if (hint && (hint->idx >= sizes.size() || !std::isfinite(hint->x) || !std::isfinite(hint->y)))
            hint = nullptr; // a drop we cannot read is no drop
        // 1. the winner
        if (auto R = Final::run<Box::SRect, Box::PackHint>([](auto&&... a) { return Box::pack(a...); }, sizes, area, gap, hint); R && Final::contractOk(sizes, area, gap, *R))
            return R;
        // 2. the fit fallback: a different search, for the tight sets the first one misses
        if (auto R = Final::run<Beam::SRect, Beam::PackHint>([](auto&&... a) { return Beam::pack(a...); }, sizes, area, gap, hint)) {
            if (!Final::contractOk(sizes, area, gap, *R))
                Final::nudgeInside(area, *R);
            if (Final::contractOk(sizes, area, gap, *R))
                return R;
        }
        return std::nullopt;
    }

    // THE FAST FIT TEST. A resize bisects its limit with dozens of packs a
    // pointer step; this is "does anything fit, and one arrangement that does"
    // -- Box's own generators, unjudged, about 0.05-1 ms. Whenever it answers,
    // pack() answers too (never a limit the layout cannot hold); it can miss a
    // barely-fitting set pack() would still place (a limit a few px short).
    // `quick`: MaxRects over the sorted and shuffled orders alone -- no group
    // search, no last-resort shuffles -- what a resize step uses, so a probe
    // that does not fit costs about what one that fits does (still a subset of
    // what pack() tries: quick-yes => pack-yes).
    inline std::optional<std::vector<SRect>> fitLayout(const std::vector<SRect>& sizes, const SRect& area, double gap, bool quick = false) {
        if (sizes.empty())
            return std::vector<SRect>{};
        if (!std::isfinite(area.x) || !std::isfinite(area.y) || !std::isfinite(area.w) || !std::isfinite(area.h) || area.w < 1 || area.h < 1 || !std::isfinite(gap))
            return std::nullopt;
        for (const SRect& S : sizes)
            if (!std::isfinite(S.w) || !std::isfinite(S.h) || S.w <= 0 || S.h <= 0 || S.w > area.w || S.h > area.h)
                return std::nullopt;
        gap    = std::max(0.0, gap);
        auto R = Final::run<Box::SRect, Box::PackHint>([quick](const auto& z, const auto& a, double g, const auto*) { return Box::fitLayout(z, a, g, quick); }, sizes, area, gap, nullptr);
        if (R && Final::contractOk(sizes, area, gap, *R))
            return R;
        return std::nullopt;
    }

    // THE EASED MARGIN (user, 2026-10-15): `loose` is the work area with the
    // outer gap at the screen's edges eased down to the inner gap (Mosaic.cpp
    // makes it). It is a fallback for the FIT only: the full margin when the
    // windows fit in it, the eased one when only it holds them.
    inline std::optional<std::vector<SRect>> packEased(const std::vector<SRect>& sizes, const SRect& area, const std::optional<SRect>& loose, double gap, const PackHint* hint = nullptr) {
        if (auto M = pack(sizes, area, gap, hint))
            return M;
        if (loose)
            return pack(sizes, *loose, gap, hint);
        return std::nullopt;
    }
    inline bool fitsEased(const std::vector<SRect>& sizes, const SRect& area, const std::optional<SRect>& loose, double gap) {
        return fitLayout(sizes, area, gap).has_value() || (loose && fitLayout(sizes, *loose, gap).has_value());
    }

    // THE CALCULATED LIMIT (user, 2026-10-14), as before: while one window is
    // resized every other keeps its size, and the resized one may grow only as
    // far as SOME mosaic arrangement of all of them still fits. First along the
    // line start -> want, then each axis alone; `reached`: `want` itself fits.
    // Every probe is the fast fit test (fitLayout), so a step at 20 windows is
    // tens of fit tests, not tens of full packs.
    struct SLimit {
        double             w = 0, h = 0;
        bool               reached = false;
        std::vector<SRect> placed = {}; // an arrangement at that size (empty if even the start does not fit)
    };
    inline constexpr int LIMIT_STEPS = 12; // 12 halvings pin a 4000 px span to 1 px
    inline SLimit limitFor(const std::vector<SRect>& sizes, size_t idx, double wantW, double wantH, const SRect& area, double gap, double minSide,
                           const std::optional<SRect>& loose = std::nullopt, bool quick = true) {
        SLimit out;
        if (idx >= sizes.size())
            return out;
        std::vector<SRect> trial = sizes;
        const auto FITS = [&](double w, double h) -> std::optional<std::vector<SRect>> {
            trial[idx].w = std::round(w);
            trial[idx].h = std::round(h);
            if (auto P = fitLayout(trial, area, gap, quick))
                return P;
            if (loose)
                return fitLayout(trial, *loose, gap, quick);
            return std::nullopt;
        };
        wantW = std::max(minSide, wantW);
        wantH = std::max(minSide, wantH);
        if (auto P = FITS(wantW, wantH))
            return SLimit{.w = std::round(wantW), .h = std::round(wantH), .reached = true, .placed = std::move(*P)};
        const double W0 = sizes[idx].w, H0 = sizes[idx].h;
        auto         at = FITS(W0, H0);
        double       w = W0, h = H0;
        if (!at) {
            // even where it started does not fit (the area shrank under it):
            // shrinking toward `want` is always allowed, growing is not
            const double SW = std::min(W0, wantW), SH = std::min(H0, wantH);
            if (auto P = FITS(SW, SH))
                return SLimit{.w = std::round(SW), .h = std::round(SH), .reached = false, .placed = std::move(*P)};
            return SLimit{.w = std::round(W0), .h = std::round(H0)};
        }
        double lo = 0.0, hi = 1.0;
        for (int i = 0; i < LIMIT_STEPS; ++i) {
            const double MID = (lo + hi) / 2.0;
            if (auto P = FITS(W0 + (wantW - W0) * MID, H0 + (wantH - H0) * MID)) {
                lo = MID;
                at = std::move(P);
            } else
                hi = MID;
        }
        w = W0 + (wantW - W0) * lo;
        h = H0 + (wantH - H0) * lo;
        const auto ALONG = [&](bool horizontal) {
            const double FROM = horizontal ? w : h, TO = horizontal ? wantW : wantH;
            if (std::abs(TO - FROM) < 1)
                return;
            double a = 0.0, b = 1.0;
            for (int i = 0; i < LIMIT_STEPS; ++i) {
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
        return SLimit{.w = std::round(w), .h = std::round(h), .reached = false, .placed = at ? std::move(*at) : std::vector<SRect>{}};
    }

} // namespace ScrollOverview::MosaicPack
