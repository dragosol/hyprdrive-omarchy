// MosaicPack: the tight mosaic (MosaicPackFinal.hpp's pack), the dwindle
// tiles, the calculated limit, and the size each window arrives with.
#include "../MosaicPack.hpp"

#include <chrono>
#include <cstdio>
#include <numeric>
#include <utility>

using namespace ScrollOverview::MosaicPack;

static int  checks = 0, failed = 0;
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
static double sep(const SRect& a, const SRect& b) {
    return std::max(std::max(b.x - (a.x + a.w), a.x - (b.x + b.w)), std::max(b.y - (a.y + a.h), a.y - (b.y + b.h)));
}
// The contract: count, exact sizes, whole-pixel positions, on screen, the gap between every pair.
static bool contract(const std::vector<SRect>& sizes, const std::optional<std::vector<SRect>>& M, const SRect& area, double gap) {
    if (!M || M->size() != sizes.size())
        return false;
    for (size_t i = 0; i < sizes.size(); ++i) {
        const SRect& R = (*M)[i];
        if (R.w != sizes[i].w || R.h != sizes[i].h || R.x != std::round(R.x) || R.y != std::round(R.y) || !inside(R, area))
            return false;
        for (size_t j = i + 1; j < sizes.size(); ++j)
            if (sep(R, (*M)[j]) < gap)
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
// ONE cluster: windows within gap + 2 px of each other across a shared span are neighbours.
static bool oneCluster(const std::vector<SRect>& rs, double gap) {
    std::vector<size_t> p(rs.size());
    std::iota(p.begin(), p.end(), size_t{0});
    const auto FIND = [&](size_t x) {
        while (p[x] != x)
            x = p[x] = p[p[x]];
        return x;
    };
    for (size_t i = 0; i < rs.size(); ++i)
        for (size_t j = i + 1; j < rs.size(); ++j) {
            const SRect &A = rs[i], &B = rs[j];
            const double SX = std::max(B.x - (A.x + A.w), A.x - (B.x + B.w)), SY = std::max(B.y - (A.y + A.h), A.y - (B.y + B.h));
            if ((SX <= gap + 2 && SY < -0.5) || (SY <= gap + 2 && SX < -0.5))
                p[FIND(i)] = FIND(j);
        }
    size_t roots = 0;
    for (size_t i = 0; i < rs.size(); ++i)
        roots += FIND(i) == i;
    return roots == 1;
}
static bool centred(const std::vector<SRect>& rs, const SRect& area) {
    const SRect B = bbox(rs);
    return std::abs(B.x + B.w / 2 - (area.x + area.w / 2)) <= 1 && std::abs(B.y + B.h / 2 - (area.y + area.h / 2)) <= 1;
}
static bool oneRow(const std::vector<SRect>& rs) { // every pair side by side
    for (size_t i = 0; i < rs.size(); ++i)
        for (size_t j = i + 1; j < rs.size(); ++j)
            if (std::max(rs[j].x - (rs[i].x + rs[i].w), rs[i].x - (rs[j].x + rs[j].w)) < 0)
                return false;
    return true;
}

int main() {
    const SRect  AREA{0, 0, 1600, 900};
    const SRect  LAPTOP{0, 0, 1376, 820};
    const SRect  UHD{0, 0, 3820, 2096};
    const SRect  K4{0, 44, 3840, 2116};
    const double GAP = 10;

    // One window: its own size, dead centre.
    {
        const auto M = pack({{0, 0, 600, 400}}, AREA, GAP, nullptr);
        expect(M && (*M)[0].w == 600 && (*M)[0].h == 400 && (*M)[0].x == 500 && (*M)[0].y == 250, "one window keeps its size and is centred");
    }
    // NOBODY IS RESIZED, nothing overlaps, the theme gap is kept, all on screen.
    {
        const std::vector<SRect> MIX = {{0, 0, 600, 400}, {0, 0, 400, 240}, {0, 0, 400, 240}, {0, 0, 500, 380}, {0, 0, 300, 420}};
        const auto               M   = pack(MIX, AREA, GAP, nullptr);
        expect(contract(MIX, M, AREA, GAP), "every window keeps its exact size, on screen, the gap kept");
        expect(M && oneCluster(*M, GAP) && centred(*M, AREA), "one cluster, centred");
        // fractional sizes (a few real clients): whole-pixel positions, sizes as given
        const std::vector<SRect> FRAC = {{0, 0, 613.016, 851.16}, {0, 0, 641.527, 600}, {0, 0, 500, 380}};
        const SRect              WIDE{0.5, 33.25, 2499.5, 1480.75}; // a fractional work area too
        expect(contract(FRAC, pack(FRAC, WIDE, 10.5, nullptr), WIDE, 10.5), "fractional sizes, gap and area: the contract holds exactly");
    }
    // BUNCHED IN THE CENTRE: the user's four (btop, Files, two terminals).
    {
        const std::vector<SRect> FOUR = {{0, 0, 940, 600}, {0, 0, 1040, 580}, {0, 0, 580, 550}, {0, 0, 580, 550}};
        const SRect              BIG{0, 0, 2500, 1480};
        const auto               M = pack(FOUR, BIG, GAP, nullptr);
        expect(contract(FOUR, M, BIG, GAP) && oneCluster(*M, GAP) && centred(*M, BIG), "the four: one cluster, centred");
        expect(M && bbox(*M).w <= 1640 && bbox(*M).h <= 1200, "the four: two rows, about 1630x1190");
    }
    // GROUPED, NOT ONE LONG ROW (user, 2026-10-15): Claude, the settings app and
    // Zen on fedora's 4K came out as one row 3290 px wide.
    {
        const SRect              LOOSE{-5, -5, 3830, 2106};
        const std::vector<SRect> THREE = {{0, 0, 1266, 1121}, {0, 0, 945, 938}, {0, 0, 1081, 968}};
        const auto               M     = packEased(THREE, UHD, LOOSE, GAP);
        expect(contract(THREE, M, UHD, GAP), "the user's three fit the full margin");
        expect(M && !oneRow(*M) && bbox(*M).w < 2400, "the user's three: grouped, not a 3300 px row");
        // three terminals: two over one, not a 2700 px strip
        const std::vector<SRect> TERMS = {{0, 0, 866, 776}, {0, 0, 958, 767}, {0, 0, 866, 776}};
        const auto               T     = pack(TERMS, SRect{10, 54, 3820, 2096}, GAP, nullptr);
        expect(T && !oneRow(*T) && oneCluster(*T, GAP), "three terminals: grouped");
        // only when nothing fits the full margin does the eased one hold them
        const std::vector<SRect> TALL = {{0, 0, 1906, 1050}, {0, 0, 1906, 1050}};
        expect(!pack(TALL, UHD, GAP, nullptr) && packEased(TALL, UHD, LOOSE, GAP), "the eased margin holds what the full one cannot");
    }
    // A PAIR STAYS SIDE BY SIDE (user, 2026-10-15).
    {
        const std::vector<SRect> PAIR = {{0, 0, 1280, 800}, {0, 0, 1280, 800}};
        const auto               M    = pack(PAIR, UHD, GAP, nullptr);
        expect(M && (*M)[0].y == (*M)[1].y && oneCluster(*M, GAP), "two same-size windows sit side by side on 4K");
    }
    // DROP TO REARRANGE (user, 2026-10-15/16): it lands where it was dropped.
    {
        const SRect              STRIP{0, 0, 3000, 500}; // one row only: two rows need 610
        const std::vector<SRect> ROW = {{0, 0, 400, 300}, {0, 0, 400, 300}, {0, 0, 400, 300}, {0, 0, 400, 300}};
        const auto               AT  = [&](double x, double y) {
            const PackHint H{.idx = 3, .x = x, .y = y};
            return pack(ROW, STRIP, GAP, &H);
        };
        const auto BETWEEN = AT(1295, 250), PAST = AT(2600, 250), LEFT = AT(100, 100);
        expect(BETWEEN && PAST && LEFT && contract(ROW, BETWEEN, STRIP, GAP), "strip drops fit");
        const auto RANK = [](const std::vector<SRect>& r) { // how many windows sit left of the dropped one
            int k = 0;
            for (size_t i = 0; i < 3; ++i)
                k += r[i].x < r[3].x;
            return k;
        };
        expect(BETWEEN && RANK(*BETWEEN) == 1, "dropped in the gap between A and B: between them");
        expect(PAST && RANK(*PAST) == 3, "dropped past C: last");
        expect(LEFT && RANK(*LEFT) == 0, "dropped left of A: first");
        // On a 2D cluster: on the side it was dropped, still one cluster.
        const SRect              FHD{0, 0, 1900, 1060};
        const std::vector<SRect> MIX = {{0, 0, 600, 400}, {0, 0, 500, 380}, {0, 0, 300, 420}, {0, 0, 400, 240}};
        for (const double PX : {100.0, 950.0, 1800.0}) {
            const PackHint H{.idx = 3, .x = PX, .y = 530};
            const auto     M = pack(MIX, FHD, GAP, &H);
            bool           ok = contract(MIX, M, FHD, GAP) && oneCluster(*M, GAP);
            if (ok && PX != 950.0) {
                const SRect  B = bbox(*M);
                const double C = (*M)[3].x + (*M)[3].w / 2;
                ok             = PX < 950 ? C < B.x + B.w / 2 : C > B.x + B.w / 2;
            }
            expect(ok, "a drop on a 2D cluster lands on its side, one cluster");
        }
        std::vector<SRect> many(9, SRect{0, 0, 700, 500});
        const PackHint     H{.idx = 8, .x = 600, .y = 400};
        expect(!pack(many, LAPTOP, GAP, &H), "no fit: a drop changes nothing (the desktop tiles)");
        // a tiny window dropped right of the cluster stays right of it (user, 2026-10-16, a recording)
        const std::vector<SRect> TINY4 = {{0, 0, 1059, 819}, {0, 0, 956, 765}, {0, 0, 1427, 1118}, {0, 0, 300, 200}};
        const PackHint           HT{.idx = 3, .x = 3500, .y = 1100};
        const auto               T = pack(TINY4, K4, 12, &HT);
        expect(contract(TINY4, T, K4, 12) && (*T)[3].x + 150 > bbox(*T).x + bbox(*T).w / 2, "a tiny window dropped right of the cluster stays on its right");
    }
    // COLUMNS WHEN ROWS FAIL (user, 2026-10-15, the XPS): Nautilus, ghostty, Nautilus.
    {
        const SRect              XPS{0, 0, 1780, 1083};
        const std::vector<SRect> TRIO = {{0, 0, 619, 465}, {0, 0, 992, 680}, {0, 0, 694, 523}};
        const auto               M    = pack(TRIO, XPS, GAP, nullptr);
        expect(contract(TRIO, M, XPS, GAP) && oneCluster(*M, GAP), "the XPS trio fits, one cluster");
        const auto& A       = (*M)[0];
        const auto& G       = (*M)[1];
        const auto& N       = (*M)[2];
        const bool  STACKED = A.y + A.h <= N.y || N.y + N.h <= A.y;
        const bool  BESIDE  = (G.x >= std::max(A.x + A.w, N.x + N.w)) || (G.x + G.w <= std::min(A.x, N.x));
        expect(M && STACKED && BESIDE, "the two Nautilus stacked, ghostty beside them");
    }
    // GROUPED (user, 2026-10-15, fedora 4K): Zen beside ghostty over Nautilus, not a 3735 px row.
    {
        const std::vector<SRect> TRIO = {{0, 0, 1733, 1532}, {0, 0, 1061, 821}, {0, 0, 921, 556}};
        const auto               M    = pack(TRIO, UHD, GAP, nullptr);
        expect(contract(TRIO, M, UHD, GAP) && bbox(*M).w < 2900 && bbox(*M).h <= 1532, "Zen beside ghostty over Nautilus (about 2804x1532)");
    }
    // NESTED (user, 2026-10-15: "move beyond rows+columns"): fits only as A | (B over (C|D)).
    {
        const SRect              BOX{0, 0, 1000, 700};
        const std::vector<SRect> FOUR = {{0, 0, 500, 690}, {0, 0, 480, 300}, {0, 0, 230, 380}, {0, 0, 240, 380}};
        expect(contract(FOUR, pack(FOUR, BOX, GAP, nullptr), BOX, GAP), "nested groups: all four at their own sizes");
    }
    // TILES LIKE THE MOSAIC (user, 2026-10-15).
    {
        CDwindleTree t;
        t.buildWeighted({1, 2}, {3.0, 1.0});
        const auto C = t.layout(SRect{0, 0, 1600, 900});
        expect(C.size() == 2 && std::abs(C[0].second.w - 1200) <= 1 && std::abs(C[1].second.w - 400) <= 1, "two tiles at 3:1 by area");
        t.buildWeighted({1, 2, 3}, {2.0, 1.0, 1.0});
        const auto D = t.layout(SRect{0, 0, 1600, 900});
        expect(D.size() == 3 && std::abs(D[0].second.w - 800) <= 1 && std::abs(D[1].second.h - 450) <= 1, "2:1:1 -> the big one half, the two small ones share the rest");
        t.buildWeighted({1, 2}, {100.0, 1.0});
        expect(std::abs(t.layout(SRect{0, 0, 1600, 900})[1].second.w - 240) <= 1, "never a sliver: at most 85:15");
        CDwindleTree u;
        u.insert(1, -1);
        u.insert(2, 1, [](long long id) { return id == 1 ? 3.0 : 1.0; });
        const auto E = u.layout(SRect{0, 0, 1600, 900});
        expect(E.size() == 2 && std::abs(E[0].second.w - 1200) <= 1, "a window opening in the tiles splits by area too");
    }
    // Too much: nothing, and the layout tiles. Never shrunk to fit.
    {
        std::vector<SRect> many(9, SRect{0, 0, 700, 500});
        expect(!pack(many, LAPTOP, GAP, nullptr) && !fitLayout(many, LAPTOP, GAP), "nine big windows do not fit: no mosaic (they are never shrunk)");
        expect(!pack({{0, 0, 2000, 400}}, AREA, GAP, nullptr), "a window wider than the area does not fit");
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
        std::vector<long long> twelve;
        for (long long i = 0; i < 12; ++i)
            twelve.push_back(i);
        t.buildBalanced(twelve);
        c           = CELLS(t);
        bool usable = COVERS(c);
        for (const auto& R : c)
            usable &= R.w >= LAPTOP.w / 6 && R.h >= LAPTOP.h / 6;
        expect(usable, "twelve windows at once: covered, no slivers");
        t.buildBalanced({1, 2});
        t.insert(3, 1);
        c = CELLS(t);
        expect(COVERS(c) && c[2].x == 688 && c[2].w == 688, "a new window splits the focused tile, the other is untouched");
        t.remove(3);
        c = CELLS(t);
        expect(c.size() == 2 && c[0].w == 688 && c[0].h == 820, "a window leaving gives its space back to its sibling");
        t.buildBalanced({1, 2, 3});
        t.resize(1, 900, 820, LAPTOP);
        expect(std::abs(CELLS(t)[0].w - 900) <= 1, "resizing a tile moves its split");
        t.resize(3, 600, 300, LAPTOP);
        expect(std::abs(CELLS(t)[2].h - 300) <= 1, "the second of a pair moves the split from its side");
        expect(COVERS(CELLS(t)), "still covers the area after resizes");
    }
    // THE CALCULATED LIMIT: others never shrink; the resized window stops where
    // the fast fit test finds no arrangement -- and pack() can place the limit.
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
        expect(pack(check, LAPTOP, GAP, nullptr).has_value() && BIG.placed.size() == 3, "the limit itself is placed by pack()");
        for (const auto& [WW, WH] : std::vector<std::pair<double, double>>{{4000, 500}, {700, 4000}, {4000, 4000}, {1200, 420}}) {
            const auto L    = limitFor(THREE, 1, WW, WH, LAPTOP, GAP, 120);
            auto       wide = THREE, tall = THREE, at = THREE;
            wide[1]         = SRect{0, 0, L.w + 12, L.h};
            tall[1]         = SRect{0, 0, L.w, L.h + 12};
            at[1]           = SRect{0, 0, L.w, L.h};
            const bool WMAX = L.w >= WW - 1 || !fitLayout(wide, LAPTOP, GAP, true);
            const bool HMAX = L.h >= WH - 1 || !fitLayout(tall, LAPTOP, GAP, true);
            expect(WMAX && HMAX, "the limit cannot grow further on either axis (the fast fit test)");
            expect(pack(at, LAPTOP, GAP, nullptr).has_value(), "and the layout can always place the limit");
        }
        const auto SHRINK = limitFor(THREE, 1, 300, 200, LAPTOP, GAP, 120);
        expect(SHRINK.reached && SHRINK.w == 300 && SHRINK.h == 200, "shrinking is always granted");
        expect(limitFor(THREE, 1, 50, 50, LAPTOP, GAP, 120).w == 120, "never below the minimum side");
    }
    // THE WAY BACK FROM THE TILES (2026-10-15): [three] + [one] fits 1900x1060.
    {
        const SRect              FHD{0, 0, 1900, 1060};
        const std::vector<SRect> FOUR = {{0, 0, 900, 600}, {0, 0, 380, 636}, {0, 0, 595, 442}, {0, 0, 604, 374}};
        expect(contract(FOUR, pack(FOUR, FHD, GAP, nullptr), FHD, GAP), "four windows that fit only as 3+1 come back as a mosaic, sizes kept");
        auto big = FOUR;
        big[3]   = SRect{0, 0, 1100, 750};
        expect(!pack(big, FHD, GAP, nullptr), "the 1100x750 window that sent it to the tiles still does not fit");
    }
    // A WINDOW'S OWN MOSAIC SIZE WINS (2026-10-15).
    {
        const SRect              FHD{0, 0, 1900, 1060};
        const std::vector<SSize> PURE  = {{900, 600}, {380, 636}, {595, 442}};
        const std::vector<SSize> TILES = {{941, 1056}, {941, 616}, {941, 521}};
        std::vector<SRect>       before, after;
        for (size_t i = 0; i < PURE.size(); ++i) {
            SNaturalInputs in{.desired = TILES[i], .fallback = {1140, 636}, .fresh = true};
            before.push_back({0, 0, chooseNatural(in, 120).size.w, chooseNatural(in, 120).size.h});
            in.kept      = PURE[i];
            const auto N = chooseNatural(in, 120);
            expect(N.kept && N.size.w == PURE[i].w && N.size.h == PURE[i].h, "a kept size wins over the current tile");
            after.push_back({0, 0, N.size.w, N.size.h});
        }
        expect(!pack(before, FHD, GAP, nullptr) && pack(after, FHD, GAP, nullptr), "the toggle comes back as the mosaic it left, not stuck in the tiles");
        SNaturalInputs in{.app = SSize{700, 500}, .desired = SSize{800, 600}, .floating = {640, 480}, .fallback = {1140, 636}, .fresh = true};
        expect(chooseNatural(in, 120).size.w == 700 && !chooseNatural(in, 120).kept, "an app's closing size beats the client's own");
        in.kept = SSize{900, 600};
        expect(chooseNatural(in, 120).size.w == 900, "the window's own beats its app's");
        in.kept = SSize{50, 600};
        expect(chooseNatural(in, 120).size.w == 700, "a kept size under the minimum is ignored");
        in.kept.reset();
        in.app.reset();
        expect(chooseNatural(in, 120).size.w == 800, "a new window: the client's own size");
        in.fresh = false;
        expect(chooseNatural(in, 120).size.w == 640, "moved in: its floating size over its old tile");
        in.floating = {};
        in.desired.reset();
        expect(chooseNatural(in, 120).size.w == 1140, "nothing: the fallback");
    }
    // TRUSTED: only the app's or the user's own size may become the app's size on close.
    {
        SNaturalInputs in{.desired = SSize{800, 600}, .floating = SSize{640, 480}, .fallback = SSize{1140, 640}, .fresh = true};
        expect(chooseNatural(in, 120).trusted, "a new window's own size is trusted");
        in.fresh = false;
        expect(!chooseNatural(in, 120).trusted, "moved in from elsewhere: its floating size is not");
        in.floating = SSize{0, 0};
        expect(chooseNatural(in, 120).size.w == 800 && !chooseNatural(in, 120).trusted, "moved in: its old tile is not");
        in.app = SSize{700, 500};
        expect(chooseNatural(in, 120).trusted, "the app's stored size is");
        in.kept        = SSize{900, 650};
        in.keptTrusted = false;
        expect(chooseNatural(in, 120).kept && !chooseNatural(in, 120).trusted, "a kept size keeps its own trust (untrusted)");
        in.keptTrusted = true;
        expect(chooseNatural(in, 120).trusted, "a kept size keeps its own trust (trusted)");
        SNaturalInputs none{.fallback = SSize{1140, 640}};
        expect(!chooseNatural(none, 120).trusted, "the fallback is not");
    }
    // ANY ORDER, ORGANICALLY (user, 2026-10-16): five on 4K, not four columns 3746 px wide.
    {
        const std::vector<SRect> FIVE = {{0, 0, 972, 1260}, {0, 0, 1116, 1530}, {0, 0, 870, 862}, {0, 0, 946, 1039}, {0, 0, 730, 720}};
        const auto               M    = pack(FIVE, K4, 12, nullptr);
        expect(contract(FIVE, M, K4, 12) && oneCluster(*M, 12) && bbox(*M).w < 3200, "five on 4K: one cluster, not a 3746 px line");
        if (M)
            std::printf("  five on 4K: cluster %.0fx%.0f\n", bbox(*M).w, bbox(*M).h);
    }
    // GROWN, NOT STACKED (user, 2026-10-16: fifteen windows came out 2655x1949 full of holes).
    {
        const std::vector<SRect> FIFTEEN = {{0, 0, 1059, 819}, {0, 0, 864, 774}, {0, 0, 864, 774}, {0, 0, 947, 688}, {0, 0, 348, 259},
                                            {0, 0, 348, 259},  {0, 0, 864, 774}, {0, 0, 348, 259}, {0, 0, 348, 259}, {0, 0, 348, 259},
                                            {0, 0, 348, 259},  {0, 0, 666, 509}, {0, 0, 348, 259}, {0, 0, 348, 259}, {0, 0, 348, 259}};
        const auto               M = pack(FIFTEEN, K4, GAP, nullptr);
        double                   filled = 0;
        for (const auto& S : FIFTEEN)
            filled += S.w * S.h;
        const double COVER = M ? filled / (bbox(*M).w * bbox(*M).h) : 0;
        if (M)
            std::printf("  fifteen: cluster %.0fx%.0f, %.0f%% covered\n", bbox(*M).w, bbox(*M).h, COVER * 100);
        expect(contract(FIFTEEN, M, K4, GAP) && oneCluster(*M, GAP) && COVER >= 0.86, "fifteen mixed windows: one tight cluster");
    }
    // DETERMINISTIC: the same windows in the same order, the same cluster, every pass.
    {
        const std::vector<SRect> SIX = {{0, 0, 300, 200}, {0, 0, 337, 253}, {0, 0, 374, 306}, {0, 0, 411, 200}, {0, 0, 448, 253}, {0, 0, 300, 306}};
        const auto               A = pack(SIX, AREA, GAP, nullptr), B = pack(SIX, AREA, GAP, nullptr);
        bool                     same = A && B;
        for (size_t i = 0; same && i < SIX.size(); ++i)
            same = (*A)[i].x == (*B)[i].x && (*A)[i].y == (*B)[i].y;
        expect(same, "repeat calls agree");
    }
    // FAST ENOUGH: a pack of 20 under the 25 ms budget; a resize step (limitFor) a few ms.
    {
        std::vector<SRect> TWENTY;
        for (size_t i = 0; i < 20; ++i)
            TWENTY.push_back({0, 0, 300.0 + 37.0 * static_cast<double>(i % 5), 200.0 + 53.0 * static_cast<double>(i % 3)});
        auto T0 = std::chrono::steady_clock::now();
        for (int k = 0; k < 5; ++k)
            (void)pack(TWENTY, UHD, GAP, nullptr);
        const double MS = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - T0).count() / 5.0;
        T0               = std::chrono::steady_clock::now();
        const auto   L   = limitFor(TWENTY, 3, 3000, 1900, UHD, GAP, 120, SRect{-5, -5, 3830, 2106});
        const double LMS = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - T0).count();
        std::printf("  pack of 20: %.2f ms, a resize step at 20: %.2f ms (limit %.0fx%.0f%s)\n", MS, LMS, L.w, L.h, L.reached ? " reached" : "");
        expect(MS < 25.0 && LMS < 25.0, "pack of 20 under 25 ms, a resize step under 25 ms");
    }

    std::printf("mosaic_pack_test: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
