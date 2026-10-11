// MosaicPack: the mosaic's rows, its centring, its fit, and the tile fallback.
#include "../MosaicPack.hpp"

#include <cstdio>

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
    return r.x >= area.x && r.y >= area.y && r.x + r.w <= area.x + area.w && r.y + r.h <= area.y + area.h;
}

int main() {
    const SRect  AREA{0, 0, 1600, 900};
    const double GAP = 10;

    // One window: its own size, dead centre.
    {
        const auto M = mosaic({{0, 0, 600, 400}}, AREA, GAP);
        expect(M && (*M)[0].w == 600 && (*M)[0].h == 400 && (*M)[0].x == 500 && (*M)[0].y == 250, "one window keeps its size and is centred");
    }
    // The clip's three: landscape, portrait, wide. They keep their sizes, never
    // overlap, sit inside the area, and the cluster is centred.
    {
        const std::vector<SRect> S = {{0, 0, 480, 340}, {0, 0, 320, 480}, {0, 0, 640, 320}};
        const auto               M = mosaic(S, AREA, GAP);
        expect(M.has_value(), "three fit");
        if (M) {
            bool sizes = true, noOverlap = true, in = true;
            double x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
            for (size_t i = 0; i < S.size(); ++i) {
                sizes &= (*M)[i].w == S[i].w && (*M)[i].h == S[i].h;
                in &= inside((*M)[i], AREA);
                for (size_t j = i + 1; j < S.size(); ++j)
                    noOverlap &= !overlaps((*M)[i], (*M)[j]);
                x0 = std::min(x0, (*M)[i].x), y0 = std::min(y0, (*M)[i].y);
                x1 = std::max(x1, (*M)[i].x + (*M)[i].w), y1 = std::max(y1, (*M)[i].y + (*M)[i].h);
            }
            expect(sizes, "natural sizes kept");
            expect(noOverlap, "no two windows overlap");
            expect(in, "all inside the work area");
            expect(std::abs((x0 + x1) / 2 - 800) <= 1 && std::abs((y0 + y1) / 2 - 450) <= 1, "the cluster is centred");
        }
    }
    // Gaps: two in a row are exactly `gap` apart.
    {
        const auto M = mosaic({{0, 0, 400, 300}, {0, 0, 400, 300}}, AREA, GAP);
        expect(M && std::abs((*M)[1].x - ((*M)[0].x + 400) - GAP) < 1 && (*M)[0].y == (*M)[1].y, "neighbours sit one gap apart on one row");
    }
    // Too big or too many: no mosaic (the layout tiles).
    expect(!mosaic({{0, 0, 1700, 300}}, AREA, GAP), "a window wider than the area cannot be a mosaic");
    {
        std::vector<SRect> many(12, SRect{0, 0, 700, 500});
        expect(!mosaic(many, AREA, GAP), "twelve big windows do not fit");
    }
    // Wrapping to rows when one row would not fit.
    {
        std::vector<SRect> four(4, SRect{0, 0, 700, 400});
        const auto         M = mosaic(four, AREA, GAP);
        expect(M && (*M)[0].y != (*M)[3].y, "four wide windows wrap to two rows");
    }
    // A little give: two foots (700x500) overflow a 1376 px desktop at natural
    // size, and fit at 95%; seven do not fit even at the 75% floor.
    {
        const SRect LAPTOP{0, 0, 1376, 814};
        double      scale = 0;
        std::vector<SRect> two(2, SRect{0, 0, 700, 500});
        const auto M = mosaicScaled(two, LAPTOP, GAP, 0.75, &scale);
        expect(!mosaic(two, LAPTOP, GAP) && M && scale < 1.0 && scale >= 0.75 && (*M)[0].w == (*M)[1].w, "two foots shrink evenly into one row");
        std::vector<SRect> seven(7, SRect{0, 0, 700, 500});
        expect(!mosaicScaled(seven, LAPTOP, GAP, 0.75), "seven foots tile");
    }
    // Tiles: cover the area exactly, no overlaps, last row spreads out.
    {
        for (size_t n : {1u, 2u, 3u, 5u, 7u, 12u}) {
            const auto T = tiles(n, AREA);
            double     sum = 0;
            bool       ok  = T.size() == n;
            for (size_t i = 0; i < T.size(); ++i) {
                sum += T[i].w * T[i].h;
                ok &= inside(T[i], AREA);
                for (size_t j = i + 1; j < T.size(); ++j)
                    ok &= !overlaps(T[i], T[j]);
            }
            expect(ok && std::abs(sum - AREA.w * AREA.h) < 1, "tiles cover the area exactly, without overlap");
        }
    }

    std::printf("mosaic_pack_test: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
