// Host test for FullscreenCarry.hpp: what is kept of a window's fullscreen
// modes through a layout swap (a plugin reload), and what it is matched to.
#include "FullscreenCarry.hpp"

#include <cstdio>
#include <vector>

using namespace Hyprdrive::FullscreenCarry;

static int g_checks = 0, g_failed = 0;
#define CHECK(c)                                                                                                                                                                   \
    do {                                                                                                                                                                           \
        ++g_checks;                                                                                                                                                                \
        if (!(c)) {                                                                                                                                                                \
            ++g_failed;                                                                                                                                                            \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c);                                                                                                               \
        }                                                                                                                                                                          \
    } while (0)

static void worthKeeping() {
    // fullscreen, maximized, a client-only mode (fakefullscreen) and both
    static_assert(worth({.internal = FULLSCREEN, .client = FULLSCREEN}));
    static_assert(worth({.internal = MAXIMIZED, .client = MAXIMIZED}));
    static_assert(worth({.internal = NONE, .client = FULLSCREEN}));
    static_assert(worth({.internal = MAXIMIZED, .client = FULLSCREEN}));
    // a plain window is not carried
    static_assert(!worth({}));
    // malformed: never written into a handler
    static_assert(!worth({.internal = 3, .client = NONE}));
    static_assert(!worth({.internal = -1, .client = FULLSCREEN}));
    static_assert(!worth({.internal = FULLSCREEN, .client = 7}));
    static_assert(!validMode(3) && !validMode(-1) && validMode(NONE) && validMode(FULLSCREEN));
}

static void matching() {
    const std::vector<SCarried> CARRIED = {
        {.win = 0x1000, .ws = 1, .mode = {.internal = FULLSCREEN, .client = FULLSCREEN}},
        {.win = 0x2000, .ws = 4, .mode = {.internal = MAXIMIZED, .client = MAXIMIZED}},
        {.win = 0x3000, .ws = 2, .mode = {.internal = 9, .client = 9}}, // malformed
        {.win = 0, .ws = 5, .mode = {.internal = FULLSCREEN, .client = FULLSCREEN}},
    };
    // the same window on the same desktop
    const auto* A = find(CARRIED, 0x1000, 1);
    CHECK(A && A->mode.internal == FULLSCREEN && A->mode.client == FULLSCREEN);
    const auto* B = find(CARRIED, 0x2000, 4);
    CHECK(B && B->mode.internal == MAXIMIZED);
    // same address, another desktop: not the window recorded
    CHECK(!find(CARRIED, 0x1000, 2));
    // an address nobody recorded
    CHECK(!find(CARRIED, 0x4000, 1));
    // a malformed record is skipped, address 0 never matches
    CHECK(!find(CARRIED, 0x3000, 2));
    CHECK(!find(CARRIED, 0, 5));
    // nothing recorded: nothing matches
    CHECK(!find({}, 0x1000, 1));
}

static void seeding() {
    constexpr SMode FS{.internal = FULLSCREEN, .client = FULLSCREEN};
    constexpr SMode MAX{.internal = MAXIMIZED, .client = MAXIMIZED};
    // the new handler forgot it (the swap): written back
    static_assert(needsSeed(FS, {}));
    static_assert(needsSeed(MAX, {}));
    // it already says the same (a desktop that stayed on its layout): untouched
    static_assert(!needsSeed(FS, FS));
    static_assert(!needsSeed(MAX, MAX));
    // the user changed it meanwhile in the same mode family: the record wins
    // only while it differs (re-seeding the same value is a no-op anyway)
    static_assert(needsSeed(FS, MAX));
    // nothing worth carrying is never written
    static_assert(!needsSeed({}, FS));
    static_assert(!needsSeed({.internal = 5, .client = 0}, {}));
    CHECK(true);
}

// A reload, end to end on the records: unload records mosaic's handler,
// the new (dwindle) handler starts empty and is seeded; load takes them out of
// dwindle's (the clear), mosaic's starts empty and is seeded. Every window
// comes out with the modes it went in with.
static void reloadRoundTrip() {
    struct SWin {
        std::uintptr_t addr;
        long long      ws;
        SMode          mode;
    };
    std::vector<SWin> wins = {
        {0x10, 1, {.internal = FULLSCREEN, .client = FULLSCREEN}}, // lone fullscreen
        {0x20, 2, {.internal = FULLSCREEN, .client = FULLSCREEN}}, // a trip's desktop
        {0x30, 3, {.internal = MAXIMIZED, .client = MAXIMIZED}},   // maximized
        {0x40, 4, {.internal = FULLSCREEN, .client = FULLSCREEN}}, // XWayland
        {0x50, 1, {}},                                             // plain, beside nothing
    };
    const auto before = wins;
    for (int pass = 0; pass < 2; ++pass) { // unload, then load
        std::vector<SCarried> rec;
        for (const auto& W : wins)
            if (worth(W.mode))
                rec.push_back({.win = W.addr, .ws = W.ws, .mode = W.mode});
        for (auto& W : wins)
            W.mode = {}; // the swap: a new handler knows nobody
        for (auto& W : wins)
            if (const auto* C = find(rec, W.addr, W.ws); C && needsSeed(C->mode, W.mode))
                W.mode = C->mode;
    }
    for (size_t i = 0; i < wins.size(); ++i)
        CHECK(wins[i].mode == before[i].mode);
}

int main() {
    worthKeeping();
    matching();
    seeding();
    reloadRoundTrip();
    std::printf("fullscreen_carry_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
