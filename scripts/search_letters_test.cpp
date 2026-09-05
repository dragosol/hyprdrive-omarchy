// Host-side unit test for SearchLetters -- the overview search field's
// per-glyph bounce, ported from Serpantinum Input.qml. Needs no compositor:
// eases, UTF-8 splitting, the prefix/suffix diff, and the animator state
// machine are arithmetic on strings and timestamps.
//
//   g++ -std=c++2b -O2 -Wall -o /tmp/search_letters_test scripts/search_letters_test.cpp && /tmp/search_letters_test
//
// `make check` runs exactly that.

#include "../SearchLetters.hpp"

#include <cmath>
#include <cstdio>
#include <string>

using namespace SearchLetters;

static int g_fail = 0, g_run = 0;

static void ok(bool cond, const std::string& what) {
    ++g_run;
    if (!cond) {
        ++g_fail;
        std::printf("FAIL  %s\n", what.c_str());
    }
}

static bool near(float a, float b, float eps = 1e-4F) {
    return std::abs(a - b) <= eps;
}

int main() {
    ok(near(easeOutBack(0.F, 3.2F), 0.F), "easeOutBack(0)=0");
    ok(near(easeOutBack(1.F, 3.2F), 1.F), "easeOutBack(1)=1");
    // t=0.5, s=3.2: t'=-0.5; 0.25*((4.2)(-0.5)+3.2)+1 = 1.275
    ok(near(easeOutBack(0.5F, 3.2F), 1.275F), "easeOutBack(0.5, 3.2)=1.275");
    ok(easeOutBack(0.5F, 3.2F) > 1.F, "easeOutBack overshoots past 1");

    ok(near(easeInBack(0.F), 0.F), "easeInBack(0)=0");
    ok(near(easeInBack(1.F), 1.F), "easeInBack(1)=1");
    ok(easeInBack(0.5F) < 0.F, "easeInBack dips negative (the wind-up)");

    ok(near(easeOutCubic(0.F), 0.F), "easeOutCubic(0)=0");
    ok(near(easeOutCubic(1.F), 1.F), "easeOutCubic(1)=1");
    ok(near(easeOutCubic(0.5F), 0.875F), "easeOutCubic(0.5)=0.875");
    ok(near(easeInCubic(0.5F), 0.125F), "easeInCubic(0.5)=0.125");

    ok(codepoints("").empty(), "codepoints empty");
    ok(codepoints("ab").size() == 2 && codepoints("ab")[0] == "a" && codepoints("ab")[1] == "b", "codepoints ascii");
    {
        const auto CP = codepoints("é"); // U+00E9, two UTF-8 bytes
        ok(CP.size() == 1 && CP[0] == "é", "codepoints one composed char");
    }
    {
        const auto CP = codepoints("aéb");
        ok(CP.size() == 3 && CP[1] == "é", "codepoints mixed");
    }

    {
        const auto E = diff({"a", "b", "c"}, {"a", "b", "c", "d"});
        ok(E.prefix == 3 && E.suffix == 0 && E.deleteCount == 0 && E.inserted.size() == 1 && E.inserted[0] == "d",
           "diff append");
    }
    {
        const auto E = diff({"a", "b", "c", "d"}, {"a", "b", "c"});
        ok(E.prefix == 3 && E.suffix == 0 && E.deleteCount == 1 && E.inserted.empty(), "diff backspace");
    }
    {
        const auto E = diff({"a", "b", "c"}, std::vector<std::string>{});
        ok(E.prefix == 0 && E.deleteCount == 3 && E.inserted.empty(), "diff clear");
    }
    {
        const auto E = diff({"a", "b", "c"}, {"a", "x", "c"});
        ok(E.prefix == 1 && E.suffix == 1 && E.deleteCount == 1 && E.inserted.size() == 1 && E.inserted[0] == "x",
           "diff replace middle");
    }
    {
        const auto E = diff({"a", "c"}, {"a", "x", "c"});
        ok(E.prefix == 1 && E.suffix == 1 && E.deleteCount == 0 && E.inserted.size() == 1 && E.inserted[0] == "x",
           "diff insert middle");
    }

    const auto T0 = clock::now();
    {
        const auto P = entrancePose(0.F);
        ok(near(P.scale, ENTRANCE_SCALE0) && near(P.y, ENTRANCE_Y0) && near(P.opacity, 0.F), "entrance at 0: pop from below");
    }
    {
        const auto P = entrancePose(ENTRANCE_MS);
        ok(near(P.scale, 1.F) && near(P.y, 0.F) && near(P.opacity, 1.F), "entrance at end: rest");
    }
    {
        const auto P = entrancePose(ENTRANCE_OPACITY_MS);
        ok(near(P.opacity, 1.F), "entrance opacity finishes at 140ms");
        ok(P.scale > ENTRANCE_SCALE0 && P.scale != 1.F, "entrance scale still travelling at 140ms");
    }
    {
        const auto P = exitPose(0.F);
        ok(near(P.scale, 1.F) && near(P.y, 0.F) && near(P.opacity, 1.F), "exit at 0: rest");
    }
    {
        const auto P = exitPose(EXIT_MS);
        ok(near(P.scale, EXIT_SCALE) && near(P.y, EXIT_Y) && P.opacity <= 1e-5F, "exit at end: up and gone");
    }

    {
        SAnimator A;
        A.sync("ab", T0);
        ok(A.liveGlyphs().size() == 2, "sync type two letters");
        ok(A.busy(T0), "fresh letters are busy");
        const auto W = std::vector<float>{10.F, 11.F};
        const auto P = A.place(W, 1.F, 1.F, T0);
        ok(P.size() == 2, "place two live");
        ok(near(P[0].x, 0.F) && near(P[1].x, 11.F), "live letters pack left, 1px gap");
        ok(P[0].scale < 1.F && P[0].y > 0.F, "typed letter starts small and below");
        ok(!A.busy(T0 + std::chrono::milliseconds(static_cast<int>(ENTRANCE_MS) + 1)), "entrance settles");
    }

    {
        SAnimator A;
        A.sync("abc", T0);
        A.place({10.F, 10.F, 10.F}, 1.F, 1.F, T0);
        const auto T1 = T0 + std::chrono::milliseconds(500);
        A.sync("ab", T1);
        ok(A.liveGlyphs().size() == 2, "backspace drops live count");
        const auto P = A.place({10.F, 10.F}, 1.F, 1.F, T1);
        ok(P.size() == 3, "dying letter still drawn");
        size_t dying = 0;
        for (const auto& L : P)
            dying += L.dying ? 1 : 0;
        ok(dying == 1, "one dying on backspace");
        ok(A.busy(T1), "exit is busy");
        A.sync("ab", T1 + std::chrono::milliseconds(static_cast<int>(EXIT_MS) + 1));
        ok(A.liveGlyphs().size() == 2 && A.letters.size() == 2, "dying letter pruned after exit");
    }

    {
        SAnimator A;
        A.sync("abc", T0);
        A.place({10.F, 10.F, 10.F}, 1.F, 1.F, T0);
        const auto T1 = T0 + std::chrono::milliseconds(500);
        A.sync("", T1);
        ok(A.liveGlyphs().empty(), "clear drops live");
        ok(A.letters.size() == 3, "clear keeps three dying");
        ok(A.busy(T1), "mass-erase is busy");
        A.reset();
        ok(A.empty() && !A.busy(T1), "reset is instant");
    }

    {
        SAnimator A;
        A.sync("ac", T0);
        A.place({10.F, 10.F}, 1.F, 1.F, T0);
        const auto T1 = T0 + std::chrono::milliseconds(500);
        A.sync("axc", T1);
        const auto P = A.place({10.F, 12.F, 10.F}, 1.F, 1.F, T1);
        ok(P.size() == 3 && P[1].glyph == "x", "insert middle");
        ok(near(P[2].x, 11.F), "displaced letter has not jumped to rest on first frame");
        const auto P2 = A.place({10.F, 12.F, 10.F}, 1.F, 1.F, T1 + std::chrono::milliseconds(static_cast<int>(DISPLACE_MS) + 1));
        ok(P2.size() == 3 && near(P2[2].x, 24.F), "displaced letter rests at packed x");
        ok(near(A.liveEndX({10.F, 12.F, 10.F}, 1.F), 34.F), "liveEndX is last glyph's right edge");
    }

    if (g_fail) {
        std::printf("search_letters_test: %d/%d failed\n", g_fail, g_run);
        return 1;
    }
    std::printf("search_letters_test: %d checks OK\n", g_run);
    return 0;
}
