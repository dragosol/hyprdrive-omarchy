// Host test for DockMenuModel.hpp: the "Always Open in" id lists (parse,
// toggle, write), the open-fullscreen decision, the segmented rows' boxes, the
// window rows' four buttons, and a preview's own menu.
#include "DockMenuModel.hpp"

#include <cmath>
#include <cstdio>
#include <algorithm>
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


    // A window row's four buttons (Move Here, Move to Empty, Scratchpad, the
    // new Close): touching, equal, the last ending at the row's right.
    {
        const auto B = rowButtons(500.0, 30.0, 4);
        CHECK(B.size() == 4);
        CHECK(B[3].x + B[3].w == 500.0);
        for (size_t i = 0; i < B.size(); ++i) {
            CHECK(B[i].w == 30.0);
            if (i > 0)
                CHECK(B[i].x == B[i - 1].x + B[i - 1].w);
        }
        // The first three sit where the three-button row had them, one button left.
        const auto THREE = rowButtons(470.0, 30.0, 3);
        for (size_t i = 0; i < 3; ++i)
            CHECK(THREE[i].x == B[i].x);
        const auto NAN_ = rowButtons(std::numeric_limits<double>::quiet_NaN(), 30.0, 4);
        CHECK(NAN_.size() == 4 && std::ranges::all_of(NAN_, [](const SSpan& s) { return s.w == 0; }));
        CHECK(rowButtons(500.0, 1.0, 4)[0].w == 0); // under 2 px: no button
    }

    // A preview's own menu: the rows for the window's state.
    {
        using W        = EWinRow;
        const auto HAS = [](const std::vector<W>& r, W k) { return std::ranges::find(r, k) != r.end(); };
        // On the current desktop, out: no Move Here; size row; Minimize; Close last.
        const auto HERE = windowMenuRows({});
        CHECK(HERE.front() == W::Header && HERE.back() == W::Close);
        CHECK(!HAS(HERE, W::MoveHere) && HAS(HERE, W::MoveEmptyWorkspace) && HAS(HERE, W::MoveScratchpad));
        CHECK(HAS(HERE, W::Size) && HAS(HERE, W::Minimize) && !HAS(HERE, W::Show));
        // Close nearest the pointer (the menu hangs above the pane), then
        // Minimize, then the size row.
        CHECK(HERE[HERE.size() - 3] == W::Minimize && HERE[HERE.size() - 4] == W::Size);
        // Elsewhere: Move to This Workspace, first of the moves.
        const auto AWAY = windowMenuRows({.elsewhere = true});
        CHECK(HAS(AWAY, W::MoveHere) && AWAY[2] == W::MoveHere);
        // Minimized: Show instead, no size row, no Move Here (Show brings it back).
        const auto MIN = windowMenuRows({.elsewhere = true, .minimized = true});
        CHECK(HAS(MIN, W::Show) && !HAS(MIN, W::Minimize) && !HAS(MIN, W::Size) && !HAS(MIN, W::MoveHere));
        // On a scratchpad: no Move to Scratchpad and no size row.
        const auto PAD = windowMenuRows({.elsewhere = true, .scratchpad = true});
        CHECK(!HAS(PAD, W::MoveScratchpad) && !HAS(PAD, W::Size) && HAS(PAD, W::MoveHere) && HAS(PAD, W::Close));
        // Never two separators in a row, never one first or last.
        for (const auto& R : {HERE, AWAY, MIN, PAD}) {
            CHECK(R.front() != W::Separator && R.back() != W::Separator);
            for (size_t i = 1; i < R.size(); ++i)
                CHECK(!(R[i] == W::Separator && R[i - 1] == W::Separator));
        }
    }

    std::printf("dock_menu_model_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
