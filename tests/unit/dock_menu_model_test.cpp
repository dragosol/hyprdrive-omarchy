// Host test for DockMenuModel.hpp: the "Always Open in" id lists (parse,
// toggle, write), the open-fullscreen decision, and the segmented rows' boxes.
#include "DockMenuModel.hpp"

#include <cmath>
#include <cstdio>
#include <limits>
#include <sstream>

using namespace Hyprdrive::Dock::MenuModel;

static int g_checks = 0, g_failed = 0;
#define CHECK(c)                                                                                                                                                                   \
    do {                                                                                                                                                                           \
        ++g_checks;                                                                                                                                                                \
        if (!(c)) {                                                                                                                                                                \
            ++g_failed;                                                                                                                                                            \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c);                                                                                                               \
        }                                                                                                                                                                          \
    } while (0)

int main() {
    // The id list: the file the empty-desktop list has always been.
    {
        std::istringstream in("kitty.desktop\r\n\n  \nzen.desktop \t\nfirefox.desktop");
        const auto         IDS = parseIdLines(in);
        CHECK(IDS.size() == 3);
        CHECK(IDS[0] == "kitty.desktop" && IDS[1] == "zen.desktop" && IDS[2] == "firefox.desktop");
        std::istringstream empty("");
        CHECK(parseIdLines(empty).empty());
    }
    {
        std::vector<std::string> ids{"a", "b"};
        auto                     on = toggledId(ids, "c");
        CHECK((on == std::vector<std::string>{"a", "b", "c"}));
        auto off = toggledId(on, "a");
        CHECK((off == std::vector<std::string>{"b", "c"}));
        CHECK(toggledId(off, "") == off); // an app with no id changes nothing
        CHECK(toggledId(toggledId(off, "x"), "x") == off);
        // Round trip through the file format.
        std::istringstream in(serializeIds(off));
        CHECK(parseIdLines(in) == off);
        CHECK(serializeIds({}).empty());
    }

    // Open fullscreen: the app's main windows, once.
    {
        const SOpenedWindow MAIN{.listed = true};
        CHECK(shouldOpenFullscreen(MAIN));
        CHECK(!shouldOpenFullscreen({.listed = false}));
        CHECK(!shouldOpenFullscreen({.listed = true, .floating = true}));
        CHECK(!shouldOpenFullscreen({.listed = true, .child = true}));
        CHECK(!shouldOpenFullscreen({.listed = true, .modal = true}));
        CHECK(!shouldOpenFullscreen({.listed = true, .special = true}));
        CHECK(!shouldOpenFullscreen({.listed = true, .applied = true})); // left fullscreen: never again
    }

    // The split row: equal halves, tiled without a seam, the gap between.
    {
        const auto S = splitSpan(107.0, 300.0, 2, 4.0);
        CHECK(S.size() == 2);
        CHECK(S[0].x == 107.0 && S[1].x + S[1].w == 407.0);
        CHECK(S[1].x - (S[0].x + S[0].w) == 4.0);
        CHECK(std::abs(S[0].w - S[1].w) <= 1.0);
        const auto ODD = splitSpan(0.5, 201.0, 2, 3.0);
        CHECK(std::abs(ODD[0].w - ODD[1].w) <= 1.0);
        for (const auto& P : ODD)
            CHECK(P.x == std::round(P.x) && P.w == std::round(P.w));
        // Guarded: too narrow or not finite draws nothing.
        const auto TINY = splitSpan(0.0, 5.0, 2, 4.0);
        CHECK(TINY.size() == 2 && TINY[0].w == 0 && TINY[1].w == 0);
        CHECK(splitSpan(std::numeric_limits<double>::quiet_NaN(), 100.0, 2, 4.0).empty());
        CHECK(splitSpan(0.0, 100.0, 0, 4.0).empty());
    }

    // The chips: right-aligned, in order, the gap between.
    {
        const auto C = chipsEndingAt(400.0, {120.0, 80.0}, 4.0);
        CHECK(C.size() == 2);
        CHECK(C[1].x + C[1].w == 400.0 && C[1].w == 80.0);
        CHECK(C[0].x + C[0].w == C[1].x - 4.0 && C[0].w == 120.0);
        const auto BAD = chipsEndingAt(400.0, {1.0, std::numeric_limits<double>::infinity()}, 4.0);
        CHECK(BAD[0].w == 0 && BAD[1].w == 0);
    }

    std::printf("dock_menu_model_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
