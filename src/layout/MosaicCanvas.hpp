#pragma once

// THE INFINITE CANVAS (user, 2026-10-26: "an additional option in the mosaic
// mode, toggled off by default (when off, windows going past the screen size
// go into the current tiled mode)"). With mosaic:infinite_canvas on, a desktop
// whose windows no longer fit the screen does not tile: the windows keep their
// mosaic sizes and the desktop grows past the screen into a canvas the view
// pans across. Pure geometry, host-tested (tests/unit/mosaic_pack_test.cpp).
//
// The canvas is ONE mosaic cluster (user, 2026-10-26: "per my clustering
// rules on native windows shouldn't organize in rows ... there should be no
// spaces between windows as much as possible ... they can organize in rows but
// without spacing between them other than the hyprland padding"): the same
// packer as the plain mosaic (pack()), any order, every window at its own size
// with only the inner gap between neighbours -- in an area as many screens
// wide (horizontal) or wide and tall (all directions, landscape steps) as it
// takes to hold everyone. The canvas is that cluster's bounds.

#include "MosaicPack.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <numeric>
#include <optional>
#include <vector>

namespace Hyprdrive::MosaicPack {

    struct SCanvasPack {
        std::vector<SRect> rects;  // per window, in the input order; canvas coordinates (0,0 = top left)
        double             w = 0, h = 0;
        size_t             screensX = 0, screensY = 0; // the area it took, in screens
    };

    // A window let go on the canvas lands where it was let go, the rest packed
    // around it (PackHint, as a drop in the plain mosaic): `x`/`y` in the
    // coordinates of the canvas it was dropped on, `oldW`/`oldH` that canvas's size.
    struct SCanvasDrop {
        size_t idx = 0;
        double x = 0, y = 0;
        double oldW = 0, oldH = 0;
    };

    // CLOSE THE SLACK (user, 2026-10-29: "clustering is supposed to have them
    // as close together as possible"): the packer can leave a window short of
    // its neighbour -- Pear 132 px off the terminal beside it, every other gap
    // the inner one, against the cluster's edge instead. Each window slides
    // toward the cluster's centre, one axis at a time, until the inner gap to
    // whatever is in its way (or it reaches the centre): the layout stays, the
    // gaps close. `keep` (a drop) stays where it was let go.
    inline void closeSlack(std::vector<SRect>& r, double gap, size_t keep = SIZE_MAX) {
        if (r.size() < 2)
            return;
        for (int round = 0; round < 8; ++round) {
            double minX = 1e18, minY = 1e18, maxX = -1e18, maxY = -1e18;
            for (const auto& R : r) {
                minX = std::min(minX, R.x);
                minY = std::min(minY, R.y);
                maxX = std::max(maxX, R.x + R.w);
                maxY = std::max(maxY, R.y + R.h);
            }
            const double CX = (minX + maxX) / 2.0, CY = (minY + maxY) / 2.0;
            bool         moved = false;
            for (size_t i = 0; i < r.size(); ++i) {
                if (i == keep)
                    continue;
                for (int axis = 0; axis < 2; ++axis) {
                    SRect&       A   = r[i];
                    const double POS = axis ? A.y : A.x, LEN = axis ? A.h : A.w, MID = POS + LEN / 2.0, C = axis ? CY : CX;
                    const double DIR = C < MID ? -1.0 : 1.0;
                    double       room = std::abs(C - MID); // never past the centre
                    for (size_t j = 0; j < r.size(); ++j) {
                        if (j == i)
                            continue;
                        const SRect& B = r[j];
                        // In the way only if they share the OTHER axis.
                        const bool SHARE = axis ? (A.x < B.x + B.w && B.x < A.x + A.w) : (A.y < B.y + B.h && B.y < A.y + A.h);
                        if (!SHARE)
                            continue;
                        const double BPOS = axis ? B.y : B.x, BLEN = axis ? B.h : B.w;
                        const double FREE = DIR < 0 ? POS - (BPOS + BLEN) - gap : BPOS - (POS + LEN) - gap;
                        if (FREE >= -0.5)
                            room = std::min(room, std::max(0.0, FREE));
                        else if (DIR < 0 ? BPOS + BLEN <= POS + 0.5 : BPOS >= POS + LEN - 0.5)
                            room = 0; // already closer than the gap: stay
                    }
                    if (room >= 1.0) {
                        (axis ? A.y : A.x) += DIR * std::floor(room);
                        moved = true;
                    }
                }
            }
            if (!moved)
                break;
        }
    }

    // ---- THE TIGHT CANVAS (user, 2026-11-02, a screenshot: "this clustering
    // is still broken ... sometimes those spaces mean the separated windows are
    // not visible and completely offscreen and it's awkward to navigate to
    // them"). The plain mosaic's packer centres: a column shorter than the one
    // beside it sat in the middle of it, space above and below -- and closing
    // the slack toward the cluster's centre never moved a column already
    // centred. On a canvas nothing is gained by centring and every space
    // pushes a window further off screen. So a canvas past one screen is:
    //   * GRAVITY-COMPACTED: every window pulled up, then left, until it meets
    //     a neighbour one gap away or the cluster's edge -- no space between
    //     windows; what no arrangement fills ends up on the cluster's outside;
    //   * the TIGHTEST of many arrangements: skyline packings at many widths
    //     and window orders, and the mosaic packer's own, each compacted,
    //     scored on the box (its area, its shape against the screen's) and,
    //     heavily, on any space enclosed between windows. The windows' own
    //     order wins a near-tie: a window opening does not reshuffle the rest.

    // BOUNDED (marketplace review, omacom/omarchy-plugin-marketplace#13685:
    // the search ran on the compositor thread with no limit -- 1.6 s for 96
    // windows). The search below is for a desktop's worth of windows: it
    // takes at most CANVAS_SEARCH_MAX of them and MAX_WIDTHS strip widths
    // per window order. A desktop with more (packCanvas) is shelf-packed,
    // in linear time.
    constexpr size_t CANVAS_SEARCH_MAX = 32;
    constexpr size_t MAX_WIDTHS        = 64;

    // Pull every window up, then left, until it meets a neighbour (one gap
    // away) or the top / left edge; again until nothing moves.
    inline void gravity(std::vector<SRect>& r, double gap, bool upFirst = true) {
        if (r.empty())
            return;
        double minX = 1e18, minY = 1e18;
        for (const auto& R : r) {
            minX = std::min(minX, R.x);
            minY = std::min(minY, R.y);
        }
        for (auto& R : r) {
            R.x -= minX;
            R.y -= minY;
        }
        std::vector<size_t> order(r.size());
        for (int round = 0; round < 12; ++round) {
            bool moved = false;
            for (int pass = 0; pass < 2; ++pass) { // up first, then left (or the other way)
                const int axis = upFirst ? 1 - pass : pass;
                std::iota(order.begin(), order.end(), size_t{0});
                std::ranges::sort(order, [&](size_t a, size_t b) { return axis ? r[a].y < r[b].y : r[a].x < r[b].x; });
                for (const size_t I : order) {
                    SRect& A   = r[I];
                    double lim = 0; // the edge
                    for (size_t j = 0; j < r.size(); ++j) {
                        if (j == I)
                            continue;
                        const SRect& B = r[j];
                        // In the way if they come within a gap of each other on the OTHER axis.
                        const bool SHARE = axis ? (A.x < B.x + B.w + gap - 0.5 && B.x < A.x + A.w + gap - 0.5) : (A.y < B.y + B.h + gap - 0.5 && B.y < A.y + A.h + gap - 0.5);
                        if (!SHARE)
                            continue;
                        const double BEND = axis ? B.y + B.h : B.x + B.w;
                        const double APOS = axis ? A.y : A.x;
                        if (BEND <= APOS + 0.5)
                            lim = std::max(lim, BEND + gap);
                    }
                    double& POS = axis ? A.y : A.x;
                    if (POS - lim >= 0.5) {
                        POS   = lim;
                        moved = true;
                    }
                }
            }
            if (!moved)
                break;
        }
    }

    // Empty space enclosed between windows (not open to the cluster's
    // outside), in px^2. Each window counts with the gap past its right and
    // bottom edges, so neighbours one gap apart close off what lies between.
    inline double enclosedArea(const std::vector<SRect>& r, double gap) {
        if (r.size() < 3)
            return 0;
        std::vector<double> xs{}, ys{};
        double              maxX = 0, maxY = 0;
        for (const auto& R : r) {
            xs.push_back(R.x);
            xs.push_back(R.x + R.w + gap);
            ys.push_back(R.y);
            ys.push_back(R.y + R.h + gap);
            maxX = std::max(maxX, R.x + R.w + gap);
            maxY = std::max(maxY, R.y + R.h + gap);
        }
        xs.push_back(0);
        ys.push_back(0);
        std::ranges::sort(xs);
        std::ranges::sort(ys);
        xs.erase(std::unique(xs.begin(), xs.end(), [](double a, double b) { return std::abs(a - b) < 0.5; }), xs.end());
        ys.erase(std::unique(ys.begin(), ys.end(), [](double a, double b) { return std::abs(a - b) < 0.5; }), ys.end());
        const size_t NX = xs.size() - 1, NY = ys.size() - 1;
        if (NX == 0 || NY == 0)
            return 0;
        // cell state: 1 covered, 2 reached from outside. Each window marks
        // only the cells under it, found by bisection: the grid's lines include
        // its edges, so that is its own cells plus one either side to test --
        // O(cells) for a layout without overlaps, not cells x windows.
        std::vector<uint8_t> cell(NX * NY, 0);
        for (const auto& R : r) {
            const double R1 = R.x + R.w + gap, B1 = R.y + R.h + gap;
            const size_t X0 = static_cast<size_t>(std::max<std::ptrdiff_t>(0, (std::lower_bound(xs.begin(), xs.end(), R.x - 1.0) - xs.begin()) - 1));
            const size_t X1 = std::min(NX, static_cast<size_t>(std::upper_bound(xs.begin(), xs.end(), R1 + 1.0) - xs.begin()));
            const size_t Y0 = static_cast<size_t>(std::max<std::ptrdiff_t>(0, (std::lower_bound(ys.begin(), ys.end(), R.y - 1.0) - ys.begin()) - 1));
            const size_t Y1 = std::min(NY, static_cast<size_t>(std::upper_bound(ys.begin(), ys.end(), B1 + 1.0) - ys.begin()));
            for (size_t cy = Y0; cy < Y1; ++cy) {
                const double MY = (ys[cy] + ys[cy + 1]) / 2;
                if (!(MY > R.y && MY < B1))
                    continue;
                for (size_t cx = X0; cx < X1; ++cx) {
                    const double MX = (xs[cx] + xs[cx + 1]) / 2;
                    if (MX > R.x && MX < R1)
                        cell[cy * NX + cx] = 1;
                }
            }
        }
        std::vector<size_t> stack;
        for (size_t cy = 0; cy < NY; ++cy)
            for (size_t cx = 0; cx < NX; ++cx)
                if ((cx == 0 || cy == 0 || cx == NX - 1 || cy == NY - 1) && cell[cy * NX + cx] == 0) {
                    cell[cy * NX + cx] = 2;
                    stack.push_back(cy * NX + cx);
                }
        while (!stack.empty()) {
            const size_t I = stack.back();
            stack.pop_back();
            const size_t CX = I % NX, CY = I / NX;
            const auto   VISIT = [&](size_t x, size_t y) {
                if (cell[y * NX + x] == 0) {
                    cell[y * NX + x] = 2;
                    stack.push_back(y * NX + x);
                }
            };
            if (CX > 0)
                VISIT(CX - 1, CY);
            if (CX + 1 < NX)
                VISIT(CX + 1, CY);
            if (CY > 0)
                VISIT(CX, CY - 1);
            if (CY + 1 < NY)
                VISIT(CX, CY + 1);
        }
        double area = 0;
        for (size_t cy = 0; cy < NY; ++cy)
            for (size_t cx = 0; cx < NX; ++cx)
                if (cell[cy * NX + cx] == 0)
                    area += (xs[cx + 1] - xs[cx]) * (ys[cy + 1] - ys[cy]);
        (void)maxX;
        (void)maxY;
        return area;
    }

    // Skyline packing into a strip `width` wide: each window, in `order`, at
    // the highest place along the top skyline where it fits (then the
    // leftmost). Windows count with the gap past their right and bottom edges.
    inline std::vector<SRect> skyline(const std::vector<SRect>& s, const std::vector<size_t>& order, double width, double gap) {
        struct SSeg {
            double x, w, y;
        };
        std::vector<SSeg>  sky{{0, width + gap, 0}};
        std::vector<SRect> out(s.size());
        for (const size_t I : order) {
            const double W = s[I].w + gap, H = s[I].h + gap;
            double       bestY = 1e18, bestX = 0;
            for (size_t k = 0; k < sky.size(); ++k) {
                const double X = sky[k].x;
                if (X + W > width + gap + 0.5)
                    break;
                double top = 0, run = 0;
                for (size_t m = k; m < sky.size() && run < W - 0.5; ++m) {
                    top = std::max(top, sky[m].y);
                    run += sky[m].w;
                }
                if (run < W - 0.5)
                    continue;
                if (top < bestY - 0.5 || (std::abs(top - bestY) < 0.5 && X < bestX)) {
                    bestY = top;
                    bestX = X;
                }
            }
            if (bestY > 1e17)
                return {}; // wider than the strip
            out[I] = SRect{bestX, bestY, s[I].w, s[I].h};
            // Raise the skyline under it.
            std::vector<SSeg> next;
            const double      L = bestX, R = bestX + W;
            for (const auto& G : sky) {
                const double GL = G.x, GR = G.x + G.w;
                if (GR <= L + 0.25 || GL >= R - 0.25) {
                    next.push_back(G);
                    continue;
                }
                if (GL < L)
                    next.push_back({GL, L - GL, G.y});
                if (GR > R)
                    next.push_back({R, GR - R, G.y});
            }
            next.push_back({L, W, bestY + H});
            std::ranges::sort(next, [](const SSeg& a, const SSeg& b) { return a.x < b.x; });
            // Merge neighbours at the same height.
            sky.clear();
            for (const auto& G : next) {
                if (!sky.empty() && std::abs(sky.back().y - G.y) < 0.25 && std::abs(sky.back().x + sky.back().w - G.x) < 0.25)
                    sky.back().w += G.w;
                else
                    sky.push_back(G);
            }
        }
        return out;
    }

    // A layout's bounding box and its score (lower is tighter): the box's
    // area and a penalty for a shape unlike the screen's. Cheap -- every
    // candidate is screened on it.
    inline double boxScore(const std::vector<SRect>& r, double screenAspect, double* boxW = nullptr, double* boxH = nullptr) {
        double minX = 1e18, minY = 1e18, maxX = -1e18, maxY = -1e18;
        for (const auto& R : r) {
            minX = std::min(minX, R.x);
            minY = std::min(minY, R.y);
            maxX = std::max(maxX, R.x + R.w);
            maxY = std::max(maxY, R.y + R.h);
        }
        const double W = maxX - minX, H = maxY - minY;
        if (boxW)
            *boxW = W;
        if (boxH)
            *boxH = H;
        if (W < 1 || H < 1)
            return 1e30;
        const double SHAPE = std::abs(std::log((W / H) / std::max(0.1, screenAspect)));
        return W * H * (1.0 + 0.35 * SHAPE);
    }
    // The full score: the box's, and a heavy penalty for space enclosed
    // between windows.
    inline double canvasScore(const std::vector<SRect>& r, double gap, double screenAspect, double* boxW = nullptr, double* boxH = nullptr) {
        const double BOX = boxScore(r, screenAspect, boxW, boxH);
        return BOX >= 1e30 ? BOX : BOX + 30.0 * enclosedArea(r, gap);
    }

    // HUG THE NEIGHBOUR (user, 2026-11-02: Dorion, narrower than Pear above
    // it, sat against the canvas's left edge with 174 px of space between it
    // and Zen). A window whose one side faces nothing but the outside, and
    // whose other side has a neighbour further than the gap, slides over to
    // it: the space moves to the cluster's outside. It never opens a gap
    // between windows -- the side it leaves faces nothing.
    inline void hugNeighbours(std::vector<SRect>& r, double gap) {
        for (int round = 0; round < 8; ++round) {
            bool moved = false;
            for (size_t i = 0; i < r.size(); ++i) {
                for (int axis = 0; axis < 2; ++axis) {
                    SRect&       A   = r[i];
                    const double LO = axis ? A.y : A.x, HI = LO + (axis ? A.h : A.w);
                    double       lowFree = 1e18, highFree = 1e18; // to the nearest window on each side, minus the gap
                    for (size_t j = 0; j < r.size(); ++j) {
                        if (j == i)
                            continue;
                        const SRect& B     = r[j];
                        const bool   SHARE = axis ? (A.x < B.x + B.w + gap - 0.5 && B.x < A.x + A.w + gap - 0.5) : (A.y < B.y + B.h + gap - 0.5 && B.y < A.y + A.h + gap - 0.5);
                        if (!SHARE)
                            continue;
                        const double BLO = axis ? B.y : B.x, BHI = BLO + (axis ? B.h : B.w);
                        if (BHI <= LO + 0.5)
                            lowFree = std::min(lowFree, LO - BHI - gap);
                        else if (BLO >= HI - 0.5)
                            highFree = std::min(highFree, BLO - HI - gap);
                    }
                    double& POS = axis ? A.y : A.x;
                    if (lowFree > 1e17 && highFree < 1e17 && highFree >= 0.5) {
                        POS += highFree;
                        moved = true;
                    } else if (highFree > 1e17 && lowFree < 1e17 && lowFree >= 0.5) {
                        POS -= lowFree;
                        moved = true;
                    }
                }
            }
            if (!moved)
                break;
        }
    }

    // Gravity toward one corner of four (bit 0: right, bit 1: down): mirrored
    // into the top-left one and back.
    inline void gravityToward(std::vector<SRect>& r, double gap, int corner, bool upFirst = true) {
        const auto FLIP = [&] {
            for (auto& R : r) {
                if (corner & 1)
                    R.x = -R.x - R.w;
                if (corner & 2)
                    R.y = -R.y - R.h;
            }
        };
        FLIP();
        gravity(r, gap, upFirst);
        FLIP();
        hugNeighbours(r, gap);
    }

    // The best corner to compact `r` toward (fewest enclosed pockets, then
    // the smaller box), `r` compacted that way.
    inline double compactBest(std::vector<SRect>& r, double gap, const std::function<double(const std::vector<SRect>&)>& score) {
        std::vector<SRect> best;
        double             bestScore = 1e30;
        for (int v = 0; v < 8; ++v) {
            auto c = r;
            gravityToward(c, gap, v & 3, v < 4);
            const double SC = score(c);
            if (SC < bestScore) {
                bestScore = SC;
                best      = std::move(c);
            }
        }
        r = std::move(best);
        return bestScore;
    }

    // The tightest compacted arrangement of `s` (sizes already clamped):
    // `seed` (the mosaic packer's) against skyline packings. `widthCap`: a
    // horizontal canvas packs transposed, its height the screen's.
    inline std::vector<SRect> tightestCanvas(const std::vector<SRect>& s, std::vector<SRect> seed, double gap, double screenAspect, bool horizontal, double screenH) {
        const size_t N = s.size();
        // Horizontal: work transposed (a strip of the screen's height, growing right).
        const auto T = [](const SRect& R) { return SRect{R.y, R.x, R.h, R.w}; };
        std::vector<SRect> sz = s;
        if (horizontal) {
            for (auto& R : sz)
                R = T(R);
            for (auto& R : seed)
                R = T(R);
        }
        const double ASPECT = horizontal ? 1.0 / std::max(0.01, screenAspect) : screenAspect;
        const auto   SCORE  = [&](const std::vector<SRect>& r) {
            double W = 0, H = 0;
            double sc = canvasScore(r, gap, ASPECT, &W, &H);
            if (horizontal && W > screenH + 0.5) // transposed: the strip must stay a screen tall
                sc = 1e30;
            return sc;
        };

        std::vector<SRect> best = seed;
        double             bestScore = seed.empty() ? 1e30 : compactBest(best, gap, SCORE);
        // The windows' own order, then tall-first, wide-first, big-first.
        std::vector<std::vector<size_t>> orders(4, std::vector<size_t>(N));
        for (auto& O : orders)
            std::iota(O.begin(), O.end(), size_t{0});
        std::ranges::stable_sort(orders[1], [&](size_t a, size_t b) { return sz[a].h > sz[b].h; });
        std::ranges::stable_sort(orders[2], [&](size_t a, size_t b) { return sz[a].w > sz[b].w; });
        std::ranges::stable_sort(orders[3], [&](size_t a, size_t b) { return sz[a].w * sz[a].h > sz[b].w * sz[b].h; });
        double maxW = 0, sumW = 0;
        for (const auto& R : sz) {
            maxW = std::max(maxW, R.w);
            sumW += R.w + gap;
        }
        std::vector<double> widths;
        if (horizontal)
            widths.push_back(screenH);
        else {
            for (const auto& O : orders) { // every prefix of every order
                double run = 0;
                for (const size_t I : O) {
                    run += sz[I].w + (run > 0 ? gap : 0);
                    if (run >= maxW - 0.5)
                        widths.push_back(run);
                }
            }
            for (int k = 0; k <= 24; ++k) // and evenly between the widest window and a single row
                widths.push_back(maxW + (sumW - maxW) * k / 24.0);
            // Each width once, and at most MAX_WIDTHS of them, spread evenly
            // over the range: the prefixes grow with the window count, the
            // search must not (BOUNDED, packCanvas).
            std::ranges::sort(widths);
            widths.erase(std::unique(widths.begin(), widths.end(), [](double a, double b) { return std::abs(a - b) < 0.5; }), widths.end());
            if (widths.size() > MAX_WIDTHS) {
                std::vector<double> few;
                for (size_t k = 0; k < MAX_WIDTHS; ++k)
                    few.push_back(widths[k * (widths.size() - 1) / (MAX_WIDTHS - 1)]);
                widths = std::move(few);
            }
        }
        // Every skyline, judged first on its box alone (cheap); only the most
        // promising few are compacted every way and judged on their pockets.
        struct SCand {
            std::vector<SRect> r;
            double             box;
            bool               own;
        };
        std::vector<SCand> cands;
        for (size_t o = 0; o < orders.size(); ++o) {
            for (const double W : widths) {
                auto r = skyline(sz, orders[o], W, gap);
                if (r.empty())
                    continue;
                double       bw = 0, bh = 0;
                const double BOX = boxScore(r, ASPECT, &bw, &bh);
                if (horizontal && bw > screenH + 0.5)
                    continue;
                cands.push_back({std::move(r), BOX, o == 0});
            }
        }
        std::ranges::sort(cands, [](const SCand& a, const SCand& b) { return a.box < b.box; });
        constexpr size_t   KEEP = 12;
        std::vector<SRect> ownBest;
        double             ownScore = 1e30;
        size_t             ownSeen  = 0;
        for (size_t k = 0; k < cands.size(); ++k) {
            const bool TOP = k < KEEP, OWN = cands[k].own && ownSeen < 2;
            if (!TOP && !OWN)
                continue;
            ownSeen += cands[k].own;
            auto         r  = cands[k].r;
            const double SC = compactBest(r, gap, SCORE);
            if (SC < bestScore) {
                bestScore = SC;
                best      = r;
            }
            if (cands[k].own && SC < ownScore) {
                ownScore = SC;
                ownBest  = r;
            }
        }
        // A near-tie goes to the windows' own order (less churn).
        if (!ownBest.empty() && ownScore <= bestScore * 1.03)
            best = ownBest;
        if (horizontal)
            for (auto& R : best)
                R = T(R);
        return best;
    }

    // More windows than the search takes (CANVAS_SEARCH_MAX): rows -- or,
    // horizontal, columns a screen tall -- in the windows' own order, each
    // window at the next place along, in one pass. All directions: a few row
    // lengths around the one a screen-shaped whole would have, the closest to
    // the screen's shape kept (boxScore) -- a fixed number of linear passes.
    inline std::vector<SRect> shelfRows(const std::vector<SRect>& s, double gap, bool horizontal, double len) {
        std::vector<SRect> out;
        double             along = 0, across = 0, thick = 0; // along the shelf, the shelf's offset, its thickness
        for (const auto& R : s) {
            const double A = horizontal ? R.h : R.w, T = horizontal ? R.w : R.h;
            if (along > 0 && along + A > len + 0.5) {
                across += thick + gap;
                along = thick = 0;
            }
            out.push_back(horizontal ? SRect{across, along, R.w, R.h} : SRect{along, across, R.w, R.h});
            along += A + gap;
            thick = std::max(thick, T);
        }
        return out;
    }
    inline std::vector<SRect> shelfCanvas(const std::vector<SRect>& s, double gap, double screenAspect, bool horizontal, double screenH) {
        if (horizontal)
            return shelfRows(s, gap, true, screenH);
        double sumA = 0, maxW = 0;
        for (const auto& R : s) {
            sumA += (R.w + gap) * (R.h + gap);
            maxW = std::max(maxW, R.w);
        }
        const double       LEN = std::sqrt(sumA * std::max(0.1, screenAspect));
        std::vector<SRect> best;
        double             bestScore = 1e30;
        for (const double K : {1.0, 1.15, 1.3, 1.5, 1.75}) {
            auto         r  = shelfRows(s, gap, false, std::max(maxW, LEN * K));
            const double SC = boxScore(r, screenAspect);
            if (SC < bestScore) {
                bestScore = SC;
                best      = std::move(r);
            }
        }
        return best;
    }

    // `area` is the screen's work area (the view), `loose` the eased one.
    inline SCanvasPack packCanvas(const std::vector<SRect>& sizes, const SRect& area, const std::optional<SRect>& loose, double gap, bool twoD,
                                  const SCanvasDrop* drop = nullptr) {
        SCanvasPack out;
        out.rects.resize(sizes.size());
        if (sizes.empty() || area.w < 1 || area.h < 1)
            return out;
        // A window bigger than the screen on its own is shown at most screen
        // sized (its mosaic size is untouched: Mosaic.cpp keeps `natural`).
        std::vector<SRect> s;
        for (const auto& R : sizes)
            s.push_back(SRect{0, 0, std::min(R.w, area.w), std::min(R.h, area.h)});

        // The smallest area that holds everyone: one screen, then a screen
        // more along x (horizontal); ALL DIRECTIONS grows the screen's own
        // shape a quarter at a time on both axes (1, 1.25, 1.5 ... screens
        // each way), so the cluster spills down as much as across (user,
        // 2026-10-27: "switching between mosaic canvas directions ... doesn't
        // do anything, I'm stuck in horizontal" -- a screen more along x came
        // first in both, and most desktops never needed a second step).
        std::optional<std::vector<SRect>> placed;
        SRect                             big = area;
        size_t                            cx = 1, cy = 1;
        // More windows than the search takes: shelves (BOUNDED, above).
        const bool SHELF = s.size() > CANVAS_SEARCH_MAX;
        if (SHELF) {
            placed = shelfCanvas(s, gap, area.w / area.h, !twoD, area.h);
            double W = 0, H = 0;
            for (const auto& R : *placed) {
                W = std::max(W, R.x + R.w);
                H = std::max(H, R.y + R.h);
            }
            cx = std::max<size_t>(1, static_cast<size_t>(std::ceil(W / area.w - 1e-9)));
            cy = std::max<size_t>(1, static_cast<size_t>(std::ceil(H / area.h - 1e-9)));
        }
        for (size_t step = 0; !SHELF && step <= 4 * sizes.size() + 4 && !placed; ++step) {
            if (twoD) {
                const double K = 1.0 + 0.25 * static_cast<double>(step);
                big            = SRect{area.x, area.y, area.w * K, area.h * K};
                cx = cy = static_cast<size_t>(std::ceil(K - 1e-9));
            } else
                big = SRect{area.x, area.y, area.w * static_cast<double>(cx) + gap * static_cast<double>(cx - 1),
                            area.h * static_cast<double>(cy) + gap * static_cast<double>(cy - 1)};
            std::optional<PackHint> hint;
            if (drop && drop->idx < s.size())
                hint = PackHint{.idx = drop->idx, .x = big.x + (big.w - drop->oldW) / 2.0 + drop->x, .y = big.y + (big.h - drop->oldH) / 2.0 + drop->y};
            const bool ONE = step == 0;
            if (ONE ? fitsEased(s, area, loose, gap) : fitLayout(s, big, gap, true).has_value())
                placed = ONE ? packEased(s, area, loose, gap, hint ? &*hint : nullptr) : pack(s, big, gap, hint ? &*hint : nullptr);
            if (placed)
                break;
            if (!twoD)
                ++cx;
        }
        if (!placed) {
            // Nothing packs (cannot happen short of a broken size): a row.
            placed = std::vector<SRect>{};
            double x = area.x;
            for (const auto& R : s) {
                placed->push_back({x, area.y, R.w, R.h});
                x += R.w + gap;
            }
        }
        // Past one screen, no drop: the tightest compacted arrangement (THE
        // TIGHT CANVAS). One screen, or a drop, as before: the mosaic's own,
        // its slack closed toward the centre.
        const bool ONESCREEN = !SHELF && cx == 1 && cy == 1 && fitsEased(s, area, loose, gap);
        if (!SHELF && !drop && !ONESCREEN)
            *placed = tightestCanvas(s, *placed, gap, area.w / area.h, !twoD, area.h);
        else if (!SHELF)
            closeSlack(*placed, gap, drop ? drop->idx : SIZE_MAX);
        double minX = 1e18, minY = 1e18, maxX = -1e18, maxY = -1e18;
        for (const auto& R : *placed) {
            minX = std::min(minX, R.x);
            minY = std::min(minY, R.y);
            maxX = std::max(maxX, R.x + R.w);
            maxY = std::max(maxY, R.y + R.h);
        }
        for (size_t i = 0; i < placed->size(); ++i) {
            const auto& R = (*placed)[i];
            out.rects[i]  = SRect{std::round(R.x - minX), std::round(R.y - minY), R.w, R.h};
        }
        out.w        = std::round(maxX - minX);
        out.h        = std::round(maxY - minY);
        out.screensX = cx;
        out.screensY = cy;
        return out;
    }

    // The last packing and what it was asked. Hyprland re-lays out a desktop
    // on many events that change nothing the packing reads (focus, a layer
    // opening, a fullscreen toggle and back); those reuse it instead of
    // searching again (BOUNDED, above). packCanvas is a pure function of these.
    struct SCanvasMemo {
        std::vector<SRect>         sizes;
        SRect                      area;
        std::optional<SRect>       loose;
        double                     gap  = 0;
        bool                       twoD = false;
        std::optional<SCanvasDrop> drop;
        SCanvasPack                out;
        bool                       valid = false;
    };
    inline SCanvasPack packCanvasMemo(SCanvasMemo& memo, const std::vector<SRect>& sizes, const SRect& area, const std::optional<SRect>& loose, double gap, bool twoD,
                                      const SCanvasDrop* drop = nullptr) {
        const auto SAME = [](const SRect& a, const SRect& b) { return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h; };
        const bool HIT  = memo.valid && memo.gap == gap && memo.twoD == twoD && SAME(memo.area, area) && memo.loose.has_value() == loose.has_value() &&
            (!loose || SAME(*memo.loose, *loose)) && memo.drop.has_value() == (drop != nullptr) &&
            (!drop || (memo.drop->idx == drop->idx && memo.drop->x == drop->x && memo.drop->y == drop->y && memo.drop->oldW == drop->oldW && memo.drop->oldH == drop->oldH)) &&
            std::ranges::equal(memo.sizes, sizes, SAME);
        if (!HIT) {
            memo.sizes = sizes;
            memo.area  = area;
            memo.loose = loose;
            memo.gap   = gap;
            memo.twoD  = twoD;
            memo.drop  = drop ? std::optional<SCanvasDrop>{*drop} : std::nullopt;
            memo.out   = packCanvas(sizes, area, loose, gap, twoD, drop);
            memo.valid = true;
        }
        return memo.out;
    }

    // Where the canvas sits on screen for a view offset, and the offsets the
    // view may take. A canvas narrower (shorter) than the view is centred on
    // that axis and does not pan along it.
    struct SCanvasView {
        double originX = 0, originY = 0;   // screen position of canvas (0,0) at offset 0
        double maxX = 0, maxY = 0;         // offsets run 0..max
    };
    inline SCanvasView canvasView(double canvasW, double canvasH, const SRect& area) {
        SCanvasView v;
        v.maxX    = std::max(0.0, canvasW - area.w);
        v.maxY    = std::max(0.0, canvasH - area.h);
        v.originX = canvasW <= area.w ? area.x + (area.w - canvasW) / 2.0 : area.x;
        v.originY = canvasH <= area.h ? area.y + (area.h - canvasH) / 2.0 : area.y;
        return v;
    }

    // The smallest move of the view (offset, clamped to 0..max) that shows
    // `r` (canvas coordinates) whole, with `margin` around it where possible.
    inline double revealAxis(double offset, double rStart, double rLen, double viewLen, double maxOffset, double margin) {
        const double LO = rStart - margin, HI = rStart + rLen + margin;
        if (HI - LO >= viewLen)
            offset = rStart + rLen / 2.0 - viewLen / 2.0; // bigger than the view: centre it
        else if (LO < offset)
            offset = LO;
        else if (HI > offset + viewLen)
            offset = HI - viewLen;
        return std::clamp(offset, 0.0, maxOffset);
    }

    // SMART FOCUS PAN (user, 2026-10-26: "windows can center on canvas when
    // focused ... the pan tries to (in order of priority, 1. fit the newly
    // focused window on screen, 2. if there is sufficient space, show the
    // adjacent window user focused from, 3. try and show at least edges of
    // other windows in proximity of the newly focused one)"). One axis of the
    // view's offset, canvas coordinates:
    //   1. `r` whole, `margin` around it, centred if nothing else asks;
    //   2. with `from` (the window focus came from) too, the two centred
    //      together if both fit; if not, the view leans toward it as far as
    //      `r` allows;
    //   3. at least `peek` px of the nearest neighbour on each side of `r`
    //      (from `near`: the windows beside it on this axis), where 1 and 2
    //      leave room.
    struct SSpan {
        double start = 0, len = 0;
        double end() const {
            return start + len;
        }
    };
    inline double smartRevealAxis(double viewLen, double maxOffset, const SSpan& r, const std::optional<SSpan>& from, const std::vector<SSpan>& near, double margin,
                                  double peek) {
        const auto CLAMP = [&](double o) { return std::clamp(o, 0.0, maxOffset); };
        if (r.len + 2 * margin >= viewLen)
            return CLAMP(r.start + r.len / 2.0 - viewLen / 2.0); // bigger than the view: centred
        double lo     = r.end() + margin - viewLen; // the offsets that show r whole
        double hi     = r.start - margin;
        double target = r.start + r.len / 2.0 - viewLen / 2.0;
        if (from) {
            const double US = std::min(r.start, from->start), UE = std::max(r.end(), from->end());
            if (UE - US + 2 * margin <= viewLen) {
                lo     = std::max(lo, UE + margin - viewLen);
                hi     = std::min(hi, US - margin);
                target = (US + UE) / 2.0 - viewLen / 2.0;
            } else if (from->end() <= r.start + 0.5 || from->start >= r.end() - 0.5)
                target = from->start < r.start ? hi : lo; // lean toward it
        }
        // 3. The nearest neighbour each side, a sliver of it.
        std::optional<double> leftEnd, rightStart;
        for (const auto& N : near) {
            if (N.end() <= r.start + 0.5 && (!leftEnd || N.end() > *leftEnd))
                leftEnd = N.end();
            if (N.start >= r.end() - 0.5 && (!rightStart || N.start < *rightStart))
                rightStart = N.start;
        }
        double plo = lo, phi = hi;
        if (leftEnd && *leftEnd - peek >= lo)
            phi = std::min(phi, *leftEnd - peek);
        if (rightStart && *rightStart + peek - viewLen <= hi)
            plo = std::max(plo, *rightStart + peek - viewLen);
        if (plo <= phi)
            target = std::clamp(target, plo, phi);
        return CLAMP(std::clamp(target, std::min(lo, hi), std::max(lo, hi)));
    }

    // PRESSURE FIRST (Mosaic.cpp pushThrough): a push through the screen's
    // edge starts only once the hand has pushed `need` px past it in one go.
    // A pause of `hold`, a turn, or another canvas starts the count again.
    // ONE COUNT PER AXIS (2026-11-09): a push into a corner loses motion on
    // both axes in every event, and the one count the two used to share was
    // started again by each axis in turn -- it never reached `need`, so a
    // corner never gave way (canvas_directions "all" is the default).
    struct SEdgePressure {
        struct SCount {
            const void*                           who  = nullptr;
            double                                sign = 0, sum = 0;
            std::chrono::steady_clock::time_point at{};
        };
        SCount count[2]; // x, y
        // `lost` px pushed past the edge on `axis` (0 = x, 1 = y) toward
        // `sign`, on `who`'s canvas: true when the push gives way, and that
        // axis counts from zero again.
        bool press(int axis, const void* who, double sign, double lost, double need, std::chrono::steady_clock::time_point now,
                   std::chrono::steady_clock::duration hold) {
            auto& C = count[axis ? 1 : 0];
            if (C.who != who || C.sign != sign || now - C.at >= hold)
                C = {who, sign, 0.0, now};
            C.sum += std::abs(lost);
            C.at = now;
            if (C.sum < need)
                return false;
            C = {};
            return true;
        }
    };
}
