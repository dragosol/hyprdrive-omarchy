// ShowDesktopMath.hpp: where each window goes to be out of view, and the
// spring that takes it there and back, reversible at any point (user,
// 2026-11-10: "moves all windows in the active workspace completely out of view
// with an animation, reversable at any point of course").
#include "ShowDesktopMath.hpp"
#include <cstdio>
#include <string>
#include <vector>

using namespace ScrollOverview::ShowDesktopMath;

static int checks = 0, failed = 0;
static void expect(bool ok, const std::string& what) {
    ++checks;
    if (!ok) {
        ++failed;
        std::printf("FAIL %s\n", what.c_str());
    }
}

// After the move, is `w` entirely off `s`, at least `m` past its edge?
static bool clear(SRect w, SVec d, SRect s, double m) {
    w.x += d.x;
    w.y += d.y;
    return w.x >= s.x + s.w + m - 1e-9 || w.x + w.w <= s.x - m + 1e-9 || w.y >= s.y + s.h + m - 1e-9 || w.y + w.h <= s.y - m + 1e-9;
}

int main() {
    const SRect  S{0, 0, 1800, 1125};
    const double M = 48;

    // Where they go: outward, the shorter way.
    struct SCase {
        const char* what;
        SRect       w;
        int         dir; // 0 left, 1 right, 2 up, 3 down, -1 stays
    };
    const std::vector<SCase> CASES = {
        {"the left half of a split goes left", {0, 0, 900, 1125}, 0},
        {"the right half goes right", {900, 0, 900, 1125}, 1},
        {"the top-right quarter goes up", {900, 0, 900, 562}, 2},
        {"the bottom-left quarter goes down", {0, 563, 900, 562}, 3},
        {"a window filling the screen goes up", {0, 0, 1800, 1125}, 2},
        {"a small dialog in the middle goes up", {800, 450, 200, 225}, 2},
        {"a tall window right of the middle goes right", {1000, 50, 300, 1000}, 1},
        {"a wide window low on the screen goes down", {100, 900, 1600, 200}, 3},
        {"a window past the left edge (a canvas) stays", {-1200, 100, 900, 600}, -1},
        {"a window below the screen stays", {100, 1200, 400, 300}, -1},
        {"a window of no size stays", {100, 100, 0, 0}, -1},
    };
    for (const auto& C : CASES) {
        const SVec D   = outOf(C.w, S, M);
        const int  DIR = D.x < 0 ? 0 : D.x > 0 ? 1 : D.y < 0 ? 2 : D.y > 0 ? 3 : -1;
        expect(DIR == C.dir, std::string(C.what) + " (went " + std::to_string(DIR) + ")");
        if (C.dir >= 0)
            expect(clear(C.w, D, S, M), std::string(C.what) + ": completely out of view, the margin too");
        expect((D.x == 0) || (D.y == 0), std::string(C.what) + ": straight, one axis");
    }
    // The shortest of the clearing moves: no other straight move is shorter.
    for (const auto& C : CASES) {
        if (C.dir < 0)
            continue;
        const SVec   D   = outOf(C.w, S, M);
        const double LEN = std::abs(D.x) + std::abs(D.y);
        const double ALL[4] = {C.w.x + C.w.w - S.x + M, S.x + S.w - C.w.x + M, C.w.y + C.w.h - S.y + M, S.y + S.h - C.w.y + M};
        double       best   = ALL[0];
        for (double a : ALL)
            best = std::min(best, a);
        expect(std::abs(LEN - best) < 1e-9, std::string(C.what) + ": the shortest way out");
    }
    // A second screen to the right: its own edges, not the first one's.
    const SRect S2{1800, 0, 2560, 1440};
    const SVec  D2 = outOf({1850, 300, 400, 800}, S2, M);
    expect(D2.x < 0 && clear({1850, 300, 400, 800}, D2, S2, M), "on a second screen: left off ITS left edge");

    // The spring, out and back.
    for (const double FPS : {60.0, 144.0}) {
        SSpring sp;
        double  t = 0, prev = 0, at95 = -1;
        bool    monotonic = true, within = true;
        while (!sp.settled(1.0) && t < 3) {
            sp.step(1 / FPS, 1.0, OUT_OMEGA);
            t += 1 / FPS;
            monotonic = monotonic && sp.x >= prev - 1e-12;
            within    = within && sp.x >= 0 && sp.x <= 1;
            prev      = sp.x;
            if (at95 < 0 && sp.x >= 0.95)
                at95 = t;
        }
        const std::string F = " at " + std::to_string((int)FPS) + " Hz";
        expect(monotonic && within, "out: steadily on, never past the edge" + F);
        expect(at95 > 0.18 && at95 < 0.32, "out: 95% of the way in ~240 ms" + F + " (" + std::to_string(at95) + ")");
        expect(t < 0.6, "out: settled within 0.6 s" + F + " (" + std::to_string(t) + ")");

        sp.snap(1);
        t = 0, prev = 1, at95 = -1;
        bool down = true;
        while (!sp.settled(0.0) && t < 3) {
            sp.step(1 / FPS, 0.0, IN_OMEGA);
            t += 1 / FPS;
            down = down && sp.x <= prev + 1e-12 && sp.x >= 0;
            prev = sp.x;
            if (at95 < 0 && sp.x <= 0.05)
                at95 = t;
        }
        expect(down, "back: steadily home, never past its place" + F);
        expect(at95 > 0.24 && at95 < 0.38, "back: 95% of the way in ~300 ms, slower than out" + F + " (" + std::to_string(at95) + ")");
    }

    // Turned round half way out, at speed: no jump, the speed carried, home
    // without passing it.
    {
        SSpring sp;
        for (int i = 0; i < 6; ++i)
            sp.step(1 / 60.0, 1.0, OUT_OMEGA);
        const double X0 = sp.x, V0 = sp.v;
        expect(X0 > 0.2 && X0 < 0.8 && V0 > 1, "mid-way out and moving (" + std::to_string(X0) + ", " + std::to_string(V0) + "/s)");
        SSpring turned = sp;
        turned.step(1 / 1000.0, 0.0, IN_OMEGA);
        expect(std::abs(turned.x - X0) < 0.01, "a turn does not jump");
        expect(turned.v > 0, "a turn keeps the speed it had: still going out for a moment");
        double peak = X0, prev = X0;
        bool   neverUnder = true, smooth = true;
        for (int i = 0; i < 300 && !sp.settled(0.0); ++i) {
            sp.step(1 / 60.0, 0.0, IN_OMEGA);
            peak       = std::max(peak, sp.x);
            neverUnder = neverUnder && sp.x >= 0;
            smooth     = smooth && std::abs(sp.x - prev) < 0.1;
            prev       = sp.x;
        }
        expect(peak > X0 && peak < 1, "it eases out of its run before coming back, still on screen's side of out");
        expect(neverUnder && sp.settled(0.0), "home without passing its place");
        expect(smooth, "no frame moves more than a tenth of the way");
    }

    // A stalled frame lands where short ones would have (exact steps).
    {
        SSpring a, b;
        a.step(0.05, 1.0, OUT_OMEGA);
        for (int i = 0; i < 50; ++i)
            b.step(0.001, 1.0, OUT_OMEGA);
        expect(std::abs(a.x - b.x) < 1e-9 && std::abs(a.v - b.v) < 1e-6, "one 50 ms step == fifty 1 ms steps");
    }

    std::printf("show_desktop_test: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
