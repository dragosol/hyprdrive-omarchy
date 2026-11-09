// MosaicPack: the tight mosaic (MosaicPackFinal.hpp's pack), the dwindle
// tiles, the calculated limit, and the size each window arrives with.
#include "../src/MosaicPack.hpp"
#include "../src/MosaicCanvas.hpp"

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
    // DWINDLE PARITY (user, 2026-10-23): what Hyprland's DwindleAlgorithm does
    // with the same moves, on a 1600x900 area.
    {
        const SRect     A{0, 0, 1600, 900};
        const STreeOpts KEEP{.preserve = true};
        const auto      AT = [&](const CDwindleTree& t, long long id, const STreeOpts& o) { return *t.cellOf(id, A, o); };
        // A drop splits the tile under the pointer, on the pointer's half.
        CDwindleTree t;
        t.buildBalanced({1, 2});
        long long under = t.leafAt(1300, 200, A);
        expect(under == 2, "the pointer over the right tile finds it");
        // 2's tile is 800x900, taller than wide: dwindle splits it one over the other.
        t.insertAt(3, under, true, 0.5, -1, A);
        expect(AT(t, 3, {}).x == 800 && AT(t, 3, {}).y == 0 && AT(t, 3, {}).h == 450 && AT(t, 2, {}).y == 450, "dropped on the top half of 2: above it");
        // The tile a window was picked out of, its window put back, is unchanged.
        t.remove(3);
        expect(AT(t, 2, {}).x == 800 && AT(t, 2, {}).w == 800, "picked out again: its sibling has the space back");
        // swapwindow: the windows trade tiles, the tiles stay.
        t.buildWeighted({1, 2, 3}, {4, 1, 1});
        const SRect BIG = AT(t, 1, {}), SMALL = AT(t, 3, {});
        t.swapIds(1, 3);
        expect(AT(t, 3, {}).w == BIG.w && AT(t, 3, {}).h == BIG.h && AT(t, 1, {}).y == SMALL.y, "the big tile and a small one trade windows");
        // preserve_split: a split keeps its direction when its space changes shape.
        CDwindleTree p;
        p.buildBalanced({1, 2});
        (void)p.layout(A, KEEP);                       // fixed side by side
        const SRect TALL{0, 0, 600, 900};
        const auto  C = p.layout(TALL, KEEP);
        expect(C[0].second.w == 300 && C[0].second.h == 900, "preserve_split: still side by side in a tall space");
        const auto D = p.layout(TALL, {});
        expect(D[0].second.w == 600 && D[0].second.h == 450, "without it: across the longer side");
        // togglesplit flips it, kept.
        p.toggleSplit(1, A, KEEP);
        expect(AT(p, 1, KEEP).w == 1600 && AT(p, 1, KEEP).h == 450, "togglesplit: one over the other");
        p.swapSplit(1);
        expect(AT(p, 1, KEEP).y == 450, "swapsplit: the two trade sides");
        // smart_resizing: dragging a tile's right edge moves only the split to its right.
        CDwindleTree r;
        r.buildBalanced({1, 2, 3}); // 1 | (2 over 3)
        r.resizeDwindle(1, 100, 0, {.right = true}, true, A, KEEP);
        expect(AT(r, 1, KEEP).w == 900 && AT(r, 2, KEEP).x == 900, "smart resize: the right edge grows the left tile by the pointer's travel");
        const SRect TWO = AT(r, 2, KEEP);
        r.resizeDwindle(3, 0, -60, {.top = true}, true, A, KEEP);
        expect(AT(r, 3, KEEP).h == 450 + 60 && AT(r, 2, KEEP).h == TWO.h - 60 && AT(r, 1, KEEP).w == 900, "smart resize: a top edge moves the split above, nothing else");
        // Not smart: the parent and the first ancestor across, both axes.
        r.resizeDwindle(2, -100, 40, {}, false, A, KEEP);
        expect(AT(r, 1, KEEP).w == 800 && AT(r, 2, KEEP).h == TWO.h - 60 + 40, "plain resize: both axes move");
        // movetoroot: the tile becomes a child of the root.
        CDwindleTree m;
        m.buildBalanced({1, 2, 3}); // 1 | (2 over 3)
        expect(m.moveToRoot(3, true) && AT(m, 3, {}).h == 900, "movetoroot: 3 now spans the height");
        // moving straight at a lone sibling lands on its far side (dwindle's override).
        CDwindleTree s;
        s.buildBalanced({1, 2});
        expect(s.towardLoneSibling(2, 3, A) && !s.towardLoneSibling(2, 1, A) && s.towardLoneSibling(1, 1, A), "toward a lone sibling: left from the right tile");
        // splitratio in dwindle's 0..2 units.
        s.splitRatio(1, 1.5, true);
        expect(AT(s, 1, {}).w == 1200, "splitratio 1.5 exact: the first tile takes three quarters");
        s.splitRatio(1, 5, false);
        expect(AT(s, 1, {}).w == 1520, "splitratio is clamped like dwindle's (0.1..1.9)");
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

    // ---- THE INFINITE CANVAS (MosaicCanvas.hpp) ----
    {
        const SRect  SCREEN{0, 0, 1920, 1040};
        const double G = 10;
        std::vector<SRect> MANY;
        for (int i = 0; i < 9; ++i)
            MANY.push_back({0, 0, 900, 600});
        for (const bool TWOD : {false, true}) {
            const auto C = packCanvas(MANY, SCREEN, std::nullopt, G, TWOD);
            bool       noOverlap = true, sizesKept = true, onCanvas = true;
            for (size_t i = 0; i < C.rects.size(); ++i) {
                sizesKept = sizesKept && C.rects[i].w == 900 && C.rects[i].h == 600;
                onCanvas  = onCanvas && C.rects[i].x >= -0.5 && C.rects[i].y >= -0.5 && C.rects[i].x + C.rects[i].w <= C.w + 0.5 && C.rects[i].y + C.rects[i].h <= C.h + 0.5;
                for (size_t j = i + 1; j < C.rects.size(); ++j)
                    noOverlap = noOverlap && !overlaps(C.rects[i], C.rects[j]) && sep(C.rects[i], C.rects[j]) >= G - 0.5;
            }
            expect(C.screensX * C.screensY > 1, TWOD ? "canvas 2D: nine big windows take more than one screen" : "canvas: nine big windows take more than one screen");
            expect(sizesKept, "canvas: every window keeps its size");
            expect(noOverlap, "canvas: no two windows overlap, gap kept");
            expect(onCanvas, "canvas: everything inside the canvas bounds");
            if (TWOD)
                expect(C.h > SCREEN.h && C.w > SCREEN.w, "canvas 2D: grows both ways");
            else
                expect(C.h <= SCREEN.h + 0.5 && C.w > SCREEN.w, "canvas horizontal: one screen tall, wider than the screen");
        }
        // THE TWO DIRECTIONS DIFFER (user, 2026-10-27: "switching between
        // mosaic canvas directions ... doesn't do anything, I'm stuck in
        // horizontal"): a desktop a little past one screen used to take one
        // screen more along x in both. All directions keeps the screen's shape.
        {
            const SRect        XPS{12, 34, 2280, 1394};
            std::vector<SRect> SIX = {{0, 0, 1037, 664}, {0, 0, 751, 504}, {0, 0, 751, 504}, {0, 0, 751, 504}, {0, 0, 751, 504}, {0, 0, 690, 519}};
            for (int i = 0; i < 8; ++i)
                SIX.push_back({0, 0, 900, 600});
            const auto H = packCanvas(SIX, XPS, std::nullopt, G, false);
            const auto A = packCanvas(SIX, XPS, std::nullopt, G, true);
            expect(A.h > XPS.h && A.w < H.w && A.w / A.h < H.w / H.h, "canvas: all directions spreads down as well as across, horizontal stays one strip");
        }
        // A DROP IN THE MIDDLE OF THE CLUSTER LANDS IN ITS MIDDLE (user,
        // 2026-10-27): the big window used to stay on the cluster's edge.
        {
            const SRect              AR{12, 12, 1896, 1056};
            const std::vector<SRect> EIGHT = {{0, 0, 622, 416}, {0, 0, 622, 416}, {0, 0, 571, 428}, {0, 0, 571, 428},
                                              {0, 0, 571, 428}, {0, 0, 571, 428}, {0, 0, 860, 549}, {0, 0, 622, 416}};
            const auto P = packCanvas(EIGHT, AR, std::nullopt, G, true);
            bool       centred = true;
            for (const size_t I : {size_t{6}, size_t{0}, size_t{2}}) {
                const SCanvasDrop D{.idx = I, .x = P.w / 2, .y = P.h / 2, .oldW = P.w, .oldH = P.h};
                const auto        Q = packCanvas(EIGHT, AR, std::nullopt, G, true, &D);
                const auto&       R = Q.rects[I];
                centred = centred && std::abs(R.x + R.w / 2 - Q.w / 2) < R.w / 2 && std::abs(R.y + R.h / 2 - Q.h / 2) < R.h / 2;
            }
            expect(centred, "canvas: a window dropped on the cluster's centre covers the new cluster's centre");
        }
        // A window bigger than the screen is shown screen sized.
        const auto BIG = packCanvas({{0, 0, 2500, 1500}, {0, 0, 800, 600}}, SCREEN, std::nullopt, G, false);
        expect(BIG.rects[0].w <= SCREEN.w && BIG.rects[0].h <= SCREEN.h, "canvas: an oversized window is clamped to the screen");
        // The view.
        const auto V = canvasView(4000, 1000, SCREEN);
        expect(V.maxX == 4000 - 1920 && V.maxY == 0 && V.originY == 20, "canvas view: pans along x only, centred in y");
        expect(revealAxis(0, 3000, 500, 1920, V.maxX, G) == 3510 - 1920, "reveal: scrolls just far enough right");
        expect(revealAxis(1500, 100, 500, 1920, V.maxX, G) == 90, "reveal: scrolls just far enough left");
        expect(revealAxis(500, 700, 500, 1920, V.maxX, G) == 500, "reveal: a window in view does not move it");

        // ONE TIGHT CLUSTER (user, 2026-10-26: no spaces between windows
        // other than the gap): every window has a neighbour one gap away.
        for (const bool TWOD : {false, true}) {
            const auto C     = packCanvas(MANY, SCREEN, std::nullopt, G, TWOD);
            bool       tight = true;
            for (size_t i = 0; i < C.rects.size(); ++i) {
                double nearest = 1e18;
                for (size_t j = 0; j < C.rects.size(); ++j)
                    if (i != j)
                        nearest = std::min(nearest, sep(C.rects[i], C.rects[j]));
                tight = tight && nearest <= G + 0.5;
            }
            expect(tight, TWOD ? "canvas 2D: every window sits one gap from a neighbour" : "canvas: every window sits one gap from a neighbour");
        }
        {
            // A drop lands at the point it was let go (the plain mosaic's PackHint).
            const auto FIRST = packCanvas(MANY, SCREEN, std::nullopt, G, false);
            const SCanvasDrop D{.idx = 0, .x = FIRST.w - 200, .y = FIRST.h / 2, .oldW = FIRST.w, .oldH = FIRST.h};
            const auto AFTER = packCanvas(MANY, SCREEN, std::nullopt, G, false, &D);
            const auto& R    = AFTER.rects[0];
            expect(R.x + R.w / 2 > AFTER.w / 2, "canvas drop: dropped at the far end, it lands in the far half");
            const auto T0 = std::chrono::steady_clock::now();
            std::vector<SRect> twelve(12, SRect{0, 0, 900, 600});
            (void)packCanvas(twelve, SCREEN, std::nullopt, G, true);
            const double MS = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - T0).count();
            std::printf("  canvas of 12 (all directions): %.2f ms\n", MS);
            expect(MS < 40.0, "canvas of 12 packs under 40 ms");
        }
        // THE TIGHT CANVAS (user, 2026-11-02, a screenshot: "this clustering is
        // still broken ... those spaces mean the separated windows are not
        // visible and completely offscreen"). Past one screen the canvas is
        // compacted: no window can slide closer to its neighbours, no space is
        // shut in between windows, and the gap is kept.
        {
            // Windows LOOSE between neighbours: along an axis it has a
            // neighbour on some side but touches none (more than the gap
            // away on every side that has one). Facing only the outside on
            // both sides is not loose: sliding would not bring it closer to anyone.
            const auto SLACK = [&](const std::vector<SRect>& r) {
                int loose = 0;
                for (size_t i = 0; i < r.size(); ++i)
                    for (int axis = 0; axis < 2; ++axis) {
                        const SRect& A   = r[i];
                        const double LO = axis ? A.y : A.x, HI = LO + (axis ? A.h : A.w);
                        double       lowFree = 1e18, highFree = 1e18;
                        for (size_t j = 0; j < r.size(); ++j) {
                            if (j == i)
                                continue;
                            const SRect& B     = r[j];
                            const bool   SHARE = axis ? (A.x < B.x + B.w + G - 0.5 && B.x < A.x + A.w + G - 0.5) : (A.y < B.y + B.h + G - 0.5 && B.y < A.y + A.h + G - 0.5);
                            if (!SHARE)
                                continue;
                            const double BLO = axis ? B.y : B.x, BHI = BLO + (axis ? B.h : B.w);
                            if (BHI <= LO + 0.5)
                                lowFree = std::min(lowFree, LO - BHI - G);
                            else if (BLO >= HI - 0.5)
                                highFree = std::min(highFree, BLO - HI - G);
                        }
                        if (std::min(lowFree, highFree) > 0.5 && std::min(lowFree, highFree) < 1e17)
                            ++loose;
                    }
                return sc_double(loose);
            };
            const auto VALID = [&](const SCanvasPack& C, const std::vector<SRect>& sizes, const SRect& area) {
                bool ok = C.rects.size() == sizes.size();
                for (size_t i = 0; ok && i < C.rects.size(); ++i) {
                    ok = ok && C.rects[i].w == std::min(sizes[i].w, area.w) && C.rects[i].h == std::min(sizes[i].h, area.h);
                    ok = ok && C.rects[i].x >= -0.5 && C.rects[i].y >= -0.5 && C.rects[i].x + C.rects[i].w <= C.w + 0.5 && C.rects[i].y + C.rects[i].h <= C.h + 0.5;
                    for (size_t j = i + 1; j < C.rects.size(); ++j)
                        ok = ok && !overlaps(C.rects[i], C.rects[j]) && sep(C.rects[i], C.rects[j]) >= G - 0.5;
                }
                return ok;
            };
            // The XPS desktop of the screenshot: Zen, two terminals, Dorion,
            // Pear Messages (each +2 for the border), the work area under the bar.
            const SRect              XPSAREA{10, 32, 1780, 1083};
            const std::vector<SRect> FIVE = {{0, 0, 1616, 967}, {0, 0, 755, 508}, {0, 0, 755, 508}, {0, 0, 952, 834}, {0, 0, 1114, 938}};
            const auto               C    = packCanvas(FIVE, XPSAREA, SRect{5, 27, 1790, 1093}, G, true);
            expect(VALID(C, FIVE, XPSAREA), "tight canvas (XPS desktop): sizes kept, no overlap, gap kept");
            expect(SLACK(C.rects) < 0.5, "tight canvas (XPS desktop): no window loose between neighbours");
            expect(enclosedArea(C.rects, G) < 1, "tight canvas (XPS desktop): no space shut in between windows");
            expect(C.w * C.h <= 2740.0 * 1782.0 + 1, "tight canvas (XPS desktop): a box no bigger than the old 2740x1782");
            // Random desktops past one screen.
            unsigned        seed  = 20261102;
            const auto      RNG   = [&seed] { return seed = seed * 1103515245u + 12345u, (seed >> 8); };
            const std::vector<std::pair<double, double>> APPS{{755, 508}, {1616, 967}, {952, 834}, {1114, 938}, {1302, 926}, {902, 602}, {622, 702}, {1039, 666}};
            bool  allValid = true, noSlack = true;
            int   tested   = 0;
            for (int t = 0; t < 150; ++t) {
                std::vector<SRect> sizes;
                const int          N = 3 + RNG() % 9;
                for (int i = 0; i < N; ++i) {
                    const auto [w, h] = APPS[RNG() % APPS.size()];
                    sizes.push_back({0, 0, w, h});
                }
                const auto R = packCanvas(sizes, XPSAREA, std::nullopt, G, t % 3 != 0);
                if (R.screensX * R.screensY <= 1)
                    continue;
                ++tested;
                allValid = allValid && VALID(R, sizes, XPSAREA);
                noSlack  = noSlack && SLACK(R.rects) < 0.5;
            }
            std::printf("  tight canvas: %d random desktops past one screen\n", tested);
            expect(tested > 50 && allValid, "tight canvas (random): sizes kept, no overlap, gap kept");
            expect(noSlack, "tight canvas (random): no window loose between neighbours");

            // BOUNDED (omacom/omarchy-plugin-marketplace#13685). enclosedArea
            // marks each window's own cells now; the per-cell scan of every
            // window it replaced is the reference, on skyline packings (which
            // leave pockets) and on random overlapping rectangles.
            {
                const auto REF = [](const std::vector<SRect>& r, double gap) {
                    if (r.size() < 3)
                        return 0.0;
                    std::vector<double> xs{0}, ys{0};
                    for (const auto& R : r) {
                        xs.push_back(R.x);
                        xs.push_back(R.x + R.w + gap);
                        ys.push_back(R.y);
                        ys.push_back(R.y + R.h + gap);
                    }
                    std::ranges::sort(xs);
                    std::ranges::sort(ys);
                    xs.erase(std::unique(xs.begin(), xs.end(), [](double a, double b) { return std::abs(a - b) < 0.5; }), xs.end());
                    ys.erase(std::unique(ys.begin(), ys.end(), [](double a, double b) { return std::abs(a - b) < 0.5; }), ys.end());
                    const size_t NX = xs.size() - 1, NY = ys.size() - 1;
                    std::vector<int> cell(NX * NY, 0);
                    for (size_t cy = 0; cy < NY; ++cy)
                        for (size_t cx = 0; cx < NX; ++cx)
                            for (const auto& R : r)
                                if ((xs[cx] + xs[cx + 1]) / 2 > R.x && (xs[cx] + xs[cx + 1]) / 2 < R.x + R.w + gap && (ys[cy] + ys[cy + 1]) / 2 > R.y &&
                                    (ys[cy] + ys[cy + 1]) / 2 < R.y + R.h + gap) {
                                    cell[cy * NX + cx] = 1;
                                    break;
                                }
                    std::vector<size_t> stack;
                    for (size_t cy = 0; cy < NY; ++cy)
                        for (size_t cx = 0; cx < NX; ++cx)
                            if ((cx == 0 || cy == 0 || cx == NX - 1 || cy == NY - 1) && !cell[cy * NX + cx])
                                cell[cy * NX + cx] = 2, stack.push_back(cy * NX + cx);
                    while (!stack.empty()) {
                        const size_t I = stack.back(), CX = I % NX, CY = I / NX;
                        stack.pop_back();
                        for (const auto& [dx, dy] : {std::pair{-1, 0}, {1, 0}, {0, -1}, {0, 1}}) {
                            const long X = static_cast<long>(CX) + dx, Y = static_cast<long>(CY) + dy;
                            if (X >= 0 && Y >= 0 && X < static_cast<long>(NX) && Y < static_cast<long>(NY) && !cell[Y * NX + X])
                                cell[Y * NX + X] = 2, stack.push_back(Y * NX + X);
                        }
                    }
                    double a = 0;
                    for (size_t cy = 0; cy < NY; ++cy)
                        for (size_t cx = 0; cx < NX; ++cx)
                            if (!cell[cy * NX + cx])
                                a += (xs[cx + 1] - xs[cx]) * (ys[cy + 1] - ys[cy]);
                    return a;
                };
                int  layouts = 0, pockets = 0;
                bool same    = true;
                for (int t = 0; t < 200; ++t) {
                    std::vector<SRect> r;
                    const int          N = 3 + RNG() % 14;
                    if (t % 2 == 0) {
                        std::vector<SRect> sizes;
                        for (int i = 0; i < N; ++i)
                            sizes.push_back({0, 0, 60.0 + RNG() % 900, 40.0 + RNG() % 700});
                        std::vector<size_t> order(sizes.size());
                        std::iota(order.begin(), order.end(), size_t{0});
                        r = skyline(sizes, order, 1200.0 + RNG() % 2000, G);
                        if (r.empty())
                            continue;
                    } else
                        for (int i = 0; i < N; ++i)
                            r.push_back({sc_double(RNG() % 3000) - 500, sc_double(RNG() % 2000) - 300, 30.0 + RNG() % 900, 30.0 + RNG() % 700});
                    const double A = enclosedArea(r, G), B = REF(r, G);
                    ++layouts;
                    pockets += B > 1;
                    same = same && std::abs(A - B) < 1e-6;
                }
                std::printf("  enclosed space: %d layouts against the reference, %d with pockets\n", layouts, pockets);
                expect(layouts > 150 && pockets > 20 && same, "bounded: enclosedArea equals the per-cell reference");
            }
            // Past CANVAS_SEARCH_MAX windows the canvas is shelved, in linear
            // time, and still a valid canvas; at the limit the full search
            // stays well inside a frame budget (generous: a loaded machine).
            {
                bool   shelvedValid = true, columnsFit = true;
                double worstSearch = 0, worstShelf = 0;
                for (const size_t N : {CANVAS_SEARCH_MAX, CANVAS_SEARCH_MAX + 1, size_t{200}, size_t{2000}})
                    for (const bool TWOD : {true, false}) {
                        std::vector<SRect> sizes;
                        for (size_t i = 0; i < N; ++i)
                            sizes.push_back({0, 0, 40.0 + RNG() % 1500, 30.0 + RNG() % 1000});
                        const auto   T0 = std::chrono::steady_clock::now();
                        const auto   C  = packCanvas(sizes, XPSAREA, std::nullopt, G, TWOD);
                        const double MS = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - T0).count();
                        if (N <= CANVAS_SEARCH_MAX)
                            worstSearch = std::max(worstSearch, MS);
                        else {
                            worstShelf   = std::max(worstShelf, MS);
                            shelvedValid = shelvedValid && (N > 200 || VALID(C, sizes, XPSAREA)) && C.rects.size() == N;
                            columnsFit   = columnsFit && (TWOD || C.h <= XPSAREA.h + 0.5);
                        }
                    }
                std::printf("  bounded: %zu windows searched in %.1f ms at worst; 33..2000 shelved in %.2f ms at worst\n", CANVAS_SEARCH_MAX, worstSearch, worstShelf);
                expect(shelvedValid, "bounded: a shelved canvas keeps sizes, no overlap, gap kept");
                expect(columnsFit, "bounded: horizontal shelves are a screen tall");
                expect(worstSearch < 250, "bounded: the search at its window limit stays well under a quarter second");
                expect(worstShelf < 50, "bounded: 2000 windows shelve in under 50 ms");
            }
            // A re-layout with the same inputs reuses the packing; any change re-packs.
            {
                SCanvasMemo memo;
                const auto  A = packCanvasMemo(memo, FIVE, XPSAREA, SRect{5, 27, 1790, 1093}, G, true);
                const auto  B = packCanvasMemo(memo, FIVE, XPSAREA, SRect{5, 27, 1790, 1093}, G, true);
                auto        moved = FIVE;
                moved[2].w += 40;
                const auto C     = packCanvasMemo(memo, moved, XPSAREA, SRect{5, 27, 1790, 1093}, G, true);
                const auto FRESH = packCanvas(moved, XPSAREA, SRect{5, 27, 1790, 1093}, G, true);
                const auto EQ    = [](const SCanvasPack& a, const SCanvasPack& b) {
                    bool ok = a.w == b.w && a.h == b.h && a.rects.size() == b.rects.size();
                    for (size_t i = 0; ok && i < a.rects.size(); ++i)
                        ok = a.rects[i].x == b.rects[i].x && a.rects[i].y == b.rects[i].y && a.rects[i].w == b.rects[i].w && a.rects[i].h == b.rects[i].h;
                    return ok;
                };
                expect(EQ(A, B) && EQ(A, packCanvas(FIVE, XPSAREA, SRect{5, 27, 1790, 1093}, G, true)), "memo: the same inputs give the same canvas");
                expect(EQ(C, FRESH) && !EQ(A, C), "memo: a changed size packs again");
                const auto D = packCanvasMemo(memo, moved, XPSAREA, std::nullopt, G, true);
                expect(EQ(D, packCanvas(moved, XPSAREA, std::nullopt, G, true)), "memo: a changed eased area packs again");
            }
        }

        // SMART FOCUS PAN.
        {
            const double VW = 1920, MAX = 6000;
            // Alone: centred.
            expect(std::abs(smartRevealAxis(VW, MAX, {3000, 600}, std::nullopt, {}, G, 48) - (3300 - 960)) < 0.5, "smart pan: a lone window is centred");
            // From a neighbour that fits with it: the two centred together.
            const double PAIR = smartRevealAxis(VW, MAX, {3000, 600}, SSpan{2390, 600}, {}, G, 48);
            expect(std::abs(PAIR - ((2390 + 3600) / 2.0 - 960)) < 0.5, "smart pan: the window focus came from stays in view");
            // From one too far to fit: leans toward it, the new one still whole.
            const double FAR = smartRevealAxis(VW, MAX, {3000, 600}, SSpan{500, 600}, {}, G, 48);
            expect(std::abs(FAR - (3000 - G)) < 0.5, "smart pan: leans toward a far window, the focused one whole");
            // Neighbours peek in.
            const double PEEKS = smartRevealAxis(VW, MAX, {3000, 1500}, std::nullopt, {{1490, 1500}, {4510, 900}}, G, 48);
            expect(PEEKS <= 2990 - 48 + 0.5 || PEEKS + VW >= 4510 + 48 - 0.5, "smart pan: a neighbour's edge shows where there is room");
            expect(PEEKS <= 3000 - G + 0.5 && PEEKS + VW >= 4500 + G - 0.5, "smart pan: the focused window stays whole");
        }

        // PUSH THROUGH THE EDGE, PRESSURE FIRST (SEdgePressure): one count per
        // axis, events 8 ms apart as a mouse sends them.
        {
            using namespace std::chrono;
            const auto T0   = steady_clock::time_point{} + hours(1);
            const auto HOLD = milliseconds(250); // Mosaic.cpp's EDGE_PAN_HOLD
            const int  ME = 0, OTHER = 0;
            const auto AT = [&](int i) { return T0 + milliseconds(8 * i); };
            // Straight at the right edge: it gives way at `need` px, not before.
            SEdgePressure S;
            int           n = 0;
            while (n < 100 && !S.press(0, &ME, 1.0, 4.0, 60.0, AT(n), HOLD))
                ++n;
            expect(n == 14, "edge push: a straight push gives way once it has pushed `need` px");
            // INTO A CORNER (2026-11-09): both axes lose motion in every event,
            // x then y as pushThrough asks them. Each gives way on its own
            // count -- the bottom's at 1.5x, as pushPressure asks.
            SEdgePressure C;
            int           gaveX = -1, gaveY = -1;
            for (int i = 0; i < 100 && (gaveX < 0 || gaveY < 0); ++i) {
                if (C.press(0, &ME, 1.0, 3.0, 60.0, AT(i), HOLD) && gaveX < 0)
                    gaveX = i;
                if (C.press(1, &ME, 1.0, 3.0, 90.0, AT(i), HOLD) && gaveY < 0)
                    gaveY = i;
            }
            expect(gaveX == 19 && gaveY == 29, "edge push: a push into a corner gives way on both axes");
            // A pause of HOLD, a turn, or another canvas: the count starts again.
            SEdgePressure P;
            P.press(0, &ME, 1.0, 50.0, 60.0, T0, HOLD);
            expect(!P.press(0, &ME, 1.0, 20.0, 60.0, T0 + HOLD, HOLD), "edge push: a pause starts the count again");
            SEdgePressure U;
            U.press(0, &ME, 1.0, 50.0, 60.0, T0, HOLD);
            expect(!U.press(0, &ME, -1.0, 20.0, 60.0, AT(1), HOLD), "edge push: turning starts the count again");
            SEdgePressure O;
            O.press(1, &ME, 1.0, 50.0, 60.0, T0, HOLD);
            expect(!O.press(1, &OTHER, 1.0, 20.0, 60.0, AT(1), HOLD), "edge push: another canvas starts the count again");
            // Nor does pushing the other edge in between start this one again.
            SEdgePressure B;
            B.press(0, &ME, 1.0, 50.0, 60.0, T0, HOLD);
            B.press(1, &ME, 1.0, 5.0, 90.0, AT(1), HOLD);
            expect(B.press(0, &ME, 1.0, 10.0, 60.0, AT(2), HOLD), "edge push: the other axis does not reset this one's count");
        }
    }

    std::printf("mosaic_pack_test: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
