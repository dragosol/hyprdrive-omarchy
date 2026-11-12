// Host test for GridSolve.hpp: the overview grid's order and the one-solve-
// per-drawn-frame rule (#9 "a hovered tile jumps right with motion off", #7
// "tiles shake with motion on"), and the three tile orders the overview can
// take as it opens (plugin:hyprdrive:overview_tile_order): last used,
// creation order, and matching the windows' places on the desktop.
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

    // 4. #10: THE STRIP'S MINI. The strip draws the desktop's window as a mini
    //    and, for the length of that draw, the window's size is the mini's
    //    (161x86 logical here: the 242x130 device px tile 1.9.57 drew on the
    //    rig at scale 1.5, one window). A query for that window from inside
    //    its mini draw solved a one-window grid from the mini: the solver never
    //    scales up (its scale is capped at 1), so the slot came out mini-sized.
    //    During a driven swipe the strip is drawn BEFORE the tiles, so with
    //    motion off the reflow's one-call lag drew exactly that slot next.
    {
        const std::vector<Vector2D> ONE  = {{1256.0, 670.0}};
        const std::vector<Vector2D> MINI = {{161.0, 86.0}};
        std::vector<CBox>           rest, plain;
        solve(ONE, USABLE, GAP, rest);
        solve(MINI, USABLE, GAP, plain);
        CHECK(rest.size() == 1 && plain.size() == 1);
        // The control: the plain re-solve is the mini itself, a fraction of the tile.
        CHECK(std::abs(plain[0].w - 161.0) < 0.01 && std::abs(plain[0].h - 86.0) < 0.01);
        CHECK(rest[0].w > 4.0 * plain[0].w);
        std::printf("control #10: tile %.1fx%.1f, re-solved from the mini %.1fx%.1f logical px\n", rest[0].w, rest[0].h, plain[0].w, plain[0].h);
        for (uint64_t frame = 1; frame <= 60; ++frame) {
            std::vector<SMemo> memos;
            const auto         A   = query(memos, ONE, USABLE, GAP, true, frame);
            const auto&        MID = query(memos, MINI, USABLE, GAP, true, frame);
            CHECK(MID[0] == A[0]);
            CHECK(memos.size() == 1);
        }
        // Three windows, the middle one drawn as its mini.
        const std::vector<Vector2D> THREE  = {{621.0, 670.0}, {621.0, 670.0}, {621.0, 670.0}};
        const std::vector<Vector2D> THREEM = {{621.0, 670.0}, {80.0, 86.0}, {621.0, 670.0}};
        std::vector<CBox>           r3, p3;
        solve(THREE, USABLE, GAP, r3);
        solve(THREEM, USABLE, GAP, p3);
        CHECK(r3.size() == 3 && p3.size() == 3 && p3[1].w < r3[1].w / 2.0);
        std::vector<SMemo> memos;
        const auto         A3 = query(memos, THREE, USABLE, GAP, true, 9);
        const auto&        M3 = query(memos, THREEM, USABLE, GAP, true, 9);
        for (size_t i = 0; i < 3; ++i)
            CHECK(M3[i] == A3[i]);
    }

    // 4. LAST USED (recentKey): the rank the overview took as it opened, 0 the
    //    most recent, whatever order the window list is in; a window opened
    //    since (unknown) is appended in creation order.
    {
        struct W {
            char     name;
            bool     known;
            uint64_t rank, seq;
        };
        const auto KEY = [](const W& w) { return recentKey(w.known, w.rank, w.seq); };
        // a used last, then c, then b; d never focused (the history's front).
        std::vector<std::vector<W>> lists = {
            {{'a', true, 0, 1}, {'b', true, 2, 2}, {'c', true, 1, 3}, {'d', true, 3, 4}},
            {{'d', true, 3, 4}, {'c', true, 1, 3}, {'b', true, 2, 2}, {'a', true, 0, 1}},
            {{'b', true, 2, 2}, {'d', true, 3, 4}, {'a', true, 0, 1}, {'c', true, 1, 3}},
        };
        for (auto& list : lists) {
            order(list, 0, list.size(), KEY);
            std::string got;
            for (const auto& w : list)
                got += w.name;
            CHECK(got == "acbd");
        }
        // Opened while the overview is up: after every known one, oldest first,
        // and nothing known moves.
        std::vector<W> grown = {{'f', false, 0, 9}, {'a', true, 0, 1}, {'e', false, 0, 8}, {'b', true, 1, 2}};
        order(grown, 0, grown.size(), KEY);
        std::string got;
        for (const auto& w : grown)
            got += w.name;
        CHECK(got == "abef");
        // Hyprland's history rank ties (windows it never held): creation order.
        std::vector<W> tied = {{'y', true, 5, 7}, {'x', true, 5, 3}};
        order(tied, 0, tied.size(), KEY);
        CHECK(tied[0].name == 'x');
    }

    // 5. MATCH THE DESKTOP (positionOrder): an order, laid out by solve().
    {
        const CBox                  GRID = {0.0, 0.0, 1800.0, 900.0};
        const std::vector<Vector2D> FOUR(4, Vector2D{800.0, 450.0});
        const auto DEAL = [](const std::vector<Vector2D>& sizes, const std::vector<size_t>& perm) {
            std::vector<Vector2D> out;
            for (const size_t I : perm)
                out.push_back(sizes[I]);
            return out;
        };
        // Created a, b, c, d; on the desktop: a bottom right, b top left,
        // c bottom left, d top right.
        const std::vector<std::optional<Vector2D>> AT = {Vector2D{1400.0, 800.0}, Vector2D{300.0, 200.0}, Vector2D{400.0, 820.0}, Vector2D{1300.0, 180.0}};
        std::vector<size_t>                        perm;
        CHECK(positionOrder(FOUR, AT, GRID, 40.F, perm) == 2);
        CHECK((perm == std::vector<size_t>{1, 3, 2, 0})); // b d / c a
        // The rows it was dealt for are the rows the solve builds, and the
        // tiles are the plain solve's boxes (equal sizes: the same slots).
        CHECK(solveRows(DEAL(FOUR, perm), GRID, 40.F) == 2);
        std::vector<CBox> slots, plain;
        solve(DEAL(FOUR, perm), GRID, 40.F, slots);
        solve(FOUR, GRID, 40.F, plain);
        CHECK(slots.size() == 4 && plain.size() == 4);
        for (size_t k = 0; k < 4; ++k)
            CHECK(slots[k] == plain[k]);
        // b top left, a bottom right.
        CHECK(slots[0].x < slots[1].x && slots[0].y < slots[2].y);
        CHECK(slots[3].x > slots[2].x && slots[3].y > slots[1].y);

        // Already in reading order: the given order.
        const std::vector<std::optional<Vector2D>> READING = {Vector2D{300.0, 200.0}, Vector2D{1300.0, 180.0}, Vector2D{400.0, 820.0}, Vector2D{1400.0, 800.0}};
        positionOrder(FOUR, READING, GRID, 40.F, perm);
        CHECK((perm == std::vector<size_t>{0, 1, 2, 3}));

        // One row (wide grid): left to right by x alone, whatever the y.
        const CBox WIDE = {0.0, 0.0, 4000.0, 300.0};
        CHECK(positionOrder(FOUR, AT, WIDE, 40.F, perm) == 1);
        CHECK((perm == std::vector<size_t>{1, 2, 3, 0})); // b(300) c(400) d(1300) a(1400)

        // Fully overlapping windows: creation order.
        const std::vector<std::optional<Vector2D>> STACKED(4, Vector2D{960.0, 540.0});
        positionOrder(FOUR, STACKED, GRID, 40.F, perm);
        CHECK((perm == std::vector<size_t>{0, 1, 2, 3}));

        // No place on this desktop (opened since, or promoted by a search):
        // after every placed one, in the order given -- even when the row
        // they land in is read left to right.
        const std::vector<std::optional<Vector2D>> SOME = {std::nullopt, Vector2D{1300.0, 200.0}, std::nullopt, Vector2D{300.0, 200.0}};
        positionOrder(FOUR, SOME, WIDE, 40.F, perm);
        CHECK((perm == std::vector<size_t>{3, 1, 0, 2}));

        // Mixed sizes: the dealt rows are the solver's own choice for the
        // dealt order, and no other row count fits it better.
        const std::vector<Vector2D>                MIXED = {{1700.0, 400.0}, {400.0, 400.0}, {400.0, 400.0}};
        const std::vector<std::optional<Vector2D>> MAT   = {Vector2D{900.0, 900.0}, Vector2D{200.0, 100.0}, Vector2D{1600.0, 100.0}};
        const size_t                               MROWS = positionOrder(MIXED, MAT, GRID, 40.F, perm);
        CHECK(MROWS == 2);
        CHECK((perm == std::vector<size_t>{1, 2, 0})); // the two small ones on top, left to right
        float scale = 0.F;
        CHECK(solveRows(DEAL(MIXED, perm), GRID, 40.F, &scale) == MROWS);
        for (size_t r = 1; r <= 3; ++r)
            CHECK(rowsScale(DEAL(MIXED, perm), r, GRID, 40.F) <= scale);

        // A many-window desktop: every dealing it answers is self-consistent.
        std::vector<Vector2D>                many;
        std::vector<std::optional<Vector2D>> at;
        for (int i = 0; i < 9; ++i) {
            many.push_back({300.0 + 70.0 * (i % 4), 200.0 + 50.0 * (i % 3)});
            at.push_back(Vector2D{100.0 + 211.0 * ((i * 5) % 9), 80.0 + 97.0 * ((i * 7) % 9)});
        }
        const size_t MANYROWS = positionOrder(many, at, GRID, 40.F, perm);
        CHECK(MANYROWS > 0 && perm.size() == 9);
        CHECK(solveRows(DEAL(many, perm), GRID, 40.F) == MANYROWS);

        // Nothing to lay out, or a mismatched ask.
        CHECK(positionOrder({}, {}, GRID, 40.F, perm) == 0 && perm.empty());
        CHECK(positionOrder(FOUR, {}, GRID, 40.F, perm) == 0);
    }

    std::printf("grid_solve_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
