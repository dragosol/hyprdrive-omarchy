// Host test for GridSolve.hpp: the overview grid's order and the one-solve-
// per-drawn-frame rule (#9 "a hovered tile jumps right with motion off", #7
// "tiles shake with motion on").
//
// THE FORCED REPRODUCTION. Two windows side by side, 621x670 logical each (the
// reporter's 1920x1080 at 1.5, Omarchy's gaps), in the rig's grid area. The
// tile pass moves a window's size onto its tile while it draws it; a query for
// the OTHER tile from inside that draw solved the grid with the drawn window
// at tile size. The CONTROL is that plain re-solve: it must move the first
// tile right by half the drawn one's shrink (the jump, 116 device px on the
// rig at scale 1.5 after the card's 0.82) and the last tile left. With the
// frame's solve held, every query of the frame answers the same slots.
#include "GridSolve.hpp"

#include <cmath>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

using namespace Hyprdrive::GridSolve;

static int g_checks = 0, g_failed = 0;
#define CHECK(c)                                                                                                                                                                   \
    do {                                                                                                                                                                           \
        ++g_checks;                                                                                                                                                                \
        if (!(c)) {                                                                                                                                                                \
            ++g_failed;                                                                                                                                                            \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c);                                                                                                               \
        }                                                                                                                                                                          \
    } while (0)

static double cx(const CBox& b) {
    return b.x + b.w / 2.0;
}

// The memo procedure of GridLayout.cpp overviewGridLayout, reduced to what this
// test varies: one grid (same members, chrome, gap), a sizes key, the frame.
struct SMemo {
    std::vector<Vector2D> sizes;
    std::vector<CBox>     slots;
    uint64_t              frame = 0;
};

static const std::vector<CBox>& query(std::vector<SMemo>& memos, const std::vector<Vector2D>& sizes, const CBox& usable, float gap, bool inDraw, uint64_t frame) {
    for (auto& m : memos)
        if (m.sizes == sizes || frameSolveHolds(inDraw, m.frame, frame, true))
            return m.slots;
    memos.push_back({sizes, {}, inDraw ? frame : 0});
    solve(sizes, usable, gap, memos.back().slots);
    return memos.back().slots;
}

int main() {
    // 1. ORDER: the creation order, whatever the window list's order -- every
    //    raise (a window moved to the end of Hyprland's list) gives one list.
    {
        using W = std::pair<char, uint64_t>; // name, creation sequence
        const std::vector<std::vector<W>> LISTS = {
            {{'a', 2}, {'b', 3}, {'c', 4}}, // as created
            {{'b', 3}, {'c', 4}, {'a', 2}}, // a raised
            {{'a', 2}, {'c', 4}, {'b', 3}}, // b raised
            {{'c', 4}, {'b', 3}, {'a', 2}}, // reversed
        };
        for (auto list : LISTS) {
            order(list, 0, list.size(), [](const W& w) { return w.second; });
            std::string got;
            for (const auto& w : list)
                got += w.first;
            CHECK(got == "abc");
        }
        // The promoted block keeps its place after the desktop's own windows.
        std::vector<W> mixed = {{'b', 9}, {'a', 5}, {'y', 8}, {'x', 1}};
        order(mixed, 0, 2, [](const W& w) { return w.second; });
        order(mixed, 2, mixed.size(), [](const W& w) { return w.second; });
        std::string got;
        for (const auto& w : mixed)
            got += w.first;
        CHECK(got == "abxy");
        // Out-of-range asks are ignored.
        order(mixed, 3, 2, [](const W& w) { return w.second; });
        order(mixed, 0, 9, [](const W& w) { return w.second; });
        CHECK(mixed[0].first == 'a');
    }

    // 2. THE RULE: held only inside the drawn frame, only for its own frame.
    CHECK(frameSolveHolds(true, 7, 7, true));
    CHECK(!frameSolveHolds(false, 7, 7, true)); // a hit test between frames solves from what it reads
    CHECK(!frameSolveHolds(true, 6, 7, true));  // a new frame looks again
    CHECK(!frameSolveHolds(true, 0, 0, true));  // a solve made outside a drawn frame holds nothing
    CHECK(!frameSolveHolds(true, 7, 7, false)); // other members: another grid

    // 3. THE FORCED REPRODUCTION.
    const CBox                  USABLE = {50.0, 288.0, 1180.0, 382.0};
    const float                 GAP    = 40.F;
    const std::vector<Vector2D> REST   = {{621.0, 670.0}, {621.0, 670.0}};
    std::vector<CBox>           restSlots;
    solve(REST, USABLE, GAP, restSlots);
    CHECK(restSlots.size() == 2);
    // The second window drawn: its size is its tile's while it is drawn -- the
    // slot on the card, which the overview draws at 0.82 (the rig's scale).
    const double                CARD     = 0.82;
    const std::vector<Vector2D> DRAWING2 = {REST[0], {restSlots[1].w * CARD, restSlots[1].h * CARD}};
    const std::vector<Vector2D> DRAWING1 = {{restSlots[0].w * CARD, restSlots[0].h * CARD}, REST[1]};
    std::vector<CBox>           c2, c1;
    solve(DRAWING2, USABLE, GAP, c2);
    solve(DRAWING1, USABLE, GAP, c1);
    const double JUMP1 = cx(c2[0]) - cx(restSlots[0]); // the first tile, asked while the second is drawn
    const double JUMP2 = cx(c1[1]) - cx(restSlots[1]); // the last tile, asked while the first is drawn
    // The control: the plain re-solve moves the asked tile by half the drawn
    // one's shrink, right for the first, left for the last -- 94.4 logical on
    // this grid, x0.82 card x1.5 = 116 device px, the reporter's jump.
    CHECK(JUMP1 > 90.0 && JUMP1 < 99.0);
    CHECK(JUMP2 < -90.0 && JUMP2 > -99.0);
    CHECK(std::abs(c2[0].w - restSlots[0].w) < 0.01); // ...at the same size, as reported
    std::printf("control: first tile %+.1f, last tile %+.1f logical px (rest %.1f / %.1f)\n", JUMP1, JUMP2, cx(restSlots[0]), cx(restSlots[1]));

    // Frames as the overview draws them, motion on or off: the frame's first
    // query solves, the tile pass then asks again from inside each draw.
    for (uint64_t frame = 1; frame <= 120; ++frame) {
        std::vector<SMemo> memos;
        const auto&        FIRST = query(memos, REST, USABLE, GAP, true, frame);
        const auto         A     = FIRST; // the boxes the frame starts drawing with
        const auto&        MID2  = query(memos, DRAWING2, USABLE, GAP, true, frame);
        const auto&        MID1  = query(memos, DRAWING1, USABLE, GAP, true, frame);
        for (size_t i = 0; i < 2; ++i) {
            CHECK(MID2[i] == A[i]);
            CHECK(MID1[i] == A[i]);
        }
        CHECK(memos.size() == 1); // one solve in the frame
        // Between frames (a hit test), the read is what it is: no hold.
        const auto& BETWEEN = query(memos, DRAWING2, USABLE, GAP, false, frame);
        CHECK(std::abs(cx(BETWEEN[0]) - cx(A[0])) > 90.0);
    }
    // A new frame takes new sizes (a real resize) -- the hold is one frame long.
    {
        std::vector<SMemo>          memos;
        const std::vector<Vector2D> RESIZED = {{521.0, 670.0}, {621.0, 670.0}};
        const auto                  F1      = query(memos, REST, USABLE, GAP, true, 1);
        const auto                  F2      = query(memos, RESIZED, USABLE, GAP, true, 2);
        CHECK(!(F1[0] == F2[0]));
    }

    std::printf("grid_solve_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
