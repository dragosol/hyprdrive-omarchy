// MosaicPack: the tight mosaic, the dwindle tiles, and the calculated limit.
#include "../MosaicPack.hpp"

#include <cstdio>
#include <utility>

using namespace ScrollOverview::MosaicPack;

static int checks = 0, failed = 0;
static void expect(bool ok, const char* what) {
    ++checks;
    if (!ok) {
        ++failed;
        std::printf("FAIL %s\n", what);
    }
}
static bool overlaps(const SRect& a, const SRect& b) {
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}
static bool inside(const SRect& r, const SRect& area) {
    return r.x >= area.x - 0.5 && r.y >= area.y - 0.5 && r.x + r.w <= area.x + area.w + 0.5 && r.y + r.h <= area.y + area.h + 0.5;
}
// Neighbours keep at least `gap` between them: pad each by half and they must not overlap.
static bool gapsKept(const std::vector<SRect>& rs, double gap) {
    for (size_t i = 0; i < rs.size(); ++i)
        for (size_t j = i + 1; j < rs.size(); ++j) {
            const SRect A{rs[i].x - gap / 2 + 0.6, rs[i].y - gap / 2 + 0.6, rs[i].w + gap - 1.2, rs[i].h + gap - 1.2};
            const SRect B{rs[j].x - gap / 2 + 0.6, rs[j].y - gap / 2 + 0.6, rs[j].w + gap - 1.2, rs[j].h + gap - 1.2};
            if (overlaps(A, B))
                return false;
        }
    return true;
}
static SRect bbox(const std::vector<SRect>& rs) {
    double x0 = 1e18, y0 = 1e18, x1 = -1e18, y1 = -1e18;
    for (const auto& R : rs) {
        x0 = std::min(x0, R.x);
        y0 = std::min(y0, R.y);
        x1 = std::max(x1, R.x + R.w);
        y1 = std::max(y1, R.y + R.h);
    }
    return SRect{x0, y0, x1 - x0, y1 - y0};
}

int main() {
    const SRect  AREA{0, 0, 1600, 900};
    const SRect  LAPTOP{0, 0, 1376, 820};
    const double GAP = 10;

    // One window: its own size, dead centre.
    {
        const auto M = mosaic({{0, 0, 600, 400}}, AREA, GAP);
        expect(M && (*M)[0].w == 600 && (*M)[0].h == 400 && (*M)[0].x == 500 && (*M)[0].y == 250, "one window keeps its size and is centred");
    }
    // NOBODY IS RESIZED, nothing overlaps, the theme gap is kept, all on screen.
    {
        const std::vector<SRect> MIX = {{0, 0, 600, 400}, {0, 0, 400, 240}, {0, 0, 400, 240}, {0, 0, 500, 380}, {0, 0, 300, 420}};
        const auto               M   = mosaic(MIX, AREA, GAP);
        bool                     ok  = M.has_value();
        for (size_t i = 0; ok && i < MIX.size(); ++i)
            ok &= (*M)[i].w == MIX[i].w && (*M)[i].h == MIX[i].h && inside((*M)[i], AREA);
        expect(ok, "every window keeps its exact size, on screen");
        expect(M && gapsKept(*M, GAP), "neighbours keep the theme gap, no overlap");
    }
    // BUNCHED IN THE CENTRE: the user's four (btop, Files, two terminals).
    {
        const std::vector<SRect> FOUR = {{0, 0, 940, 600}, {0, 0, 1040, 580}, {0, 0, 580, 550}, {0, 0, 580, 550}};
        const SRect              BIG{0, 0, 2500, 1480};
        const auto               M = mosaic(FOUR, BIG, GAP);
        const auto               B = bbox(*M);
        expect(M && std::abs((B.x + B.w / 2) - 1250) <= 1 && std::abs((B.y + B.h / 2) - 740) <= 1, "the cluster is centred on the screen");
        // Two rows: the top pair leans down, the bottom pair hangs up: the gap
        // between them is exactly the theme gap, the terminals side by side.
        const double TOPBOTTOM = std::max((*M)[0].y + (*M)[0].h, (*M)[1].y + (*M)[1].h);
        expect(std::abs((*M)[2].y - TOPBOTTOM - GAP) <= 1 && (*M)[2].y == (*M)[3].y, "rows close in on the middle: one gap between them");
        expect(std::abs((*M)[0].y + (*M)[0].h - ((*M)[1].y + (*M)[1].h)) <= 1, "a short window in the top row sits on the row's bottom edge");
        expect(std::abs((*M)[3].x - ((*M)[2].x + 580 + GAP)) <= 1, "the bottom pair sits together, one gap apart");
    }
    // Too much: nothing, and the layout tiles. Never shrunk to fit.
    {
        std::vector<SRect> many(9, SRect{0, 0, 700, 500});
        expect(!mosaic(many, LAPTOP, GAP).has_value(), "nine big windows do not fit: no mosaic (they are never shrunk)");
        expect(!mosaic({{0, 0, 2000, 400}}, AREA, GAP).has_value(), "a window wider than the area does not fit");
    }

    // DWINDLE TREE: dwindle's own behaviour.
    {
        const auto CELLS = [&](const CDwindleTree& t) {
            std::vector<SRect> out;
            for (const auto& [ID, R] : t.layout(LAPTOP))
                out.push_back(R);
            return out;
        };
        const auto COVERS = [&](const std::vector<SRect>& c) {
            double sum = 0;
            bool   ok  = true;
            for (size_t i = 0; i < c.size(); ++i) {
                sum += c[i].w * c[i].h;
                ok &= inside(c[i], LAPTOP) && c[i].w >= 1 && c[i].h >= 1;
                for (size_t j = i + 1; j < c.size(); ++j)
                    ok &= !overlaps(c[i], c[j]);
            }
            return ok && std::abs(sum - LAPTOP.w * LAPTOP.h) < 1;
        };
        CDwindleTree t;
        t.buildBalanced({1, 2});
        auto c = CELLS(t);
        expect(c.size() == 2 && c[0].w == 688 && c[0].h == 820 && c[1].x == 688, "two windows: side by side, even");
        t.buildBalanced({1, 2, 3});
        c = CELLS(t);
        expect(c[0].x == 0 && c[0].h == 820 && c[1].x == c[2].x && c[1].y < c[2].y, "three: one left, two stacked right");
        t.buildBalanced({1, 2, 3, 4});
        c = CELLS(t);
        expect(c[0].x == c[1].x && c[2].x == c[3].x && c[0].y == c[2].y && c[0].x < c[2].x, "four: a 2x2 grid");
        // Twelve at once: usable, no slivers.
        std::vector<long long> twelve;
        for (long long i = 0; i < 12; ++i)
            twelve.push_back(i);
        t.buildBalanced(twelve);
        c         = CELLS(t);
        bool usable = COVERS(c);
        for (const auto& R : c)
            usable &= R.w >= LAPTOP.w / 6 && R.h >= LAPTOP.h / 6;
        expect(usable, "twelve windows at once: covered, no slivers");
        // A new window splits the FOCUSED tile in half, like dwindle.
        t.buildBalanced({1, 2});
        t.insert(3, 1);
        c = CELLS(t);
        expect(COVERS(c) && c[2].x == 688 && c[2].w == 688, "a new window splits the focused tile, the other is untouched");
        // A window leaving: its sibling takes the space back.
        t.remove(3);
        c = CELLS(t);
        expect(c.size() == 2 && c[0].w == 688 && c[0].h == 820, "a window leaving gives its space back to its sibling");
        // Resizing a tile moves the split above it.
        t.buildBalanced({1, 2, 3});
        t.resize(1, 900, 820, LAPTOP);
        expect(std::abs(CELLS(t)[0].w - 900) <= 1, "resizing a tile moves its split");
        t.resize(3, 600, 300, LAPTOP);
        expect(std::abs(CELLS(t)[2].h - 300) <= 1, "the second of a pair moves the split from its side");
        expect(COVERS(CELLS(t)), "still covers the area after resizes");
    }

    // THE CALCULATED LIMIT: others never shrink, the resized window stops where
    // no arrangement fits.
    {
        const std::vector<SRect> THREE = {{0, 0, 600, 380}, {0, 0, 500, 380}, {0, 0, 500, 380}};
        const auto               SMALL = limitFor(THREE, 1, 560, 400, LAPTOP, GAP, 120);
        expect(SMALL.reached && SMALL.w == 560 && SMALL.h == 400 && SMALL.placed.size() == 3, "a grow that fits is granted whole");
        expect(SMALL.placed[0].w == 600 && SMALL.placed[2].w == 500, "the others keep their sizes in the limit's arrangement");
        const auto BIG = limitFor(THREE, 1, 4000, 4000, LAPTOP, GAP, 120);
        expect(!BIG.reached && BIG.w < 4000 && BIG.h < 4000 && BIG.w >= 500 && BIG.h >= 380, "a grow past the limit stops at it");
        std::vector<SRect> check = THREE;
        check[1].w               = BIG.w;
        check[1].h               = BIG.h;
        expect(mosaic(check, LAPTOP, GAP).has_value() && BIG.placed.size() == 3, "the limit itself has an arrangement");
        for (const auto& [WW, WH] : std::vector<std::pair<double, double>>{{4000, 500}, {700, 4000}, {4000, 4000}, {1200, 420}}) {
            const auto L    = limitFor(THREE, 1, WW, WH, LAPTOP, GAP, 120);
            auto       wide = THREE, tall = THREE;
            wide[1]         = SRect{0, 0, L.w + 12, L.h};
            tall[1]         = SRect{0, 0, L.w, L.h + 12};
            const bool WMAX = L.w >= WW - 1 || !mosaic(wide, LAPTOP, GAP);
            const bool HMAX = L.h >= WH - 1 || !mosaic(tall, LAPTOP, GAP);
            expect(WMAX && HMAX, "the limit cannot grow further on either axis");
        }
        const auto SHRINK = limitFor(THREE, 1, 300, 200, LAPTOP, GAP, 120);
        expect(SHRINK.reached && SHRINK.w == 300 && SHRINK.h == 200, "shrinking is always granted");
        expect(limitFor(THREE, 1, 50, 50, LAPTOP, GAP, 120).w == 120, "never below the minimum side");
    }

    std::printf("mosaic_pack_test: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
