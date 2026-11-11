// XStateRequest.hpp: which half of an X11 _NET_WM_STATE request Hyprland's
// maximize toggle must not see, and the guard that stops a window whose
// messages keep flipping it.
#include "XStateRequest.hpp"
#include <cstdio>
#include <string>

using namespace Hyprdrive::XStateRequest;

static int  checks = 0, failed = 0;
static void expect(bool ok, const std::string& what) {
    ++checks;
    if (!ok) {
        ++failed;
        std::printf("FAIL %s\n", what.c_str());
    }
}

static const char* name(eMode m) {
    return m == eMode::NONE ? "none" : m == eMode::MAXIMIZED ? "maximized" : "fullscreen";
}

int main() {
    const CStormGuard QUIET;

    // Maximize alone: action x current mode.
    struct {
        uint32_t action;
        eMode    current;
        bool     strip;
    } const MAX[] = {
        {ADD, eMode::NONE, false},          {ADD, eMode::MAXIMIZED, true}, {ADD, eMode::FULLSCREEN, false},   {REMOVE, eMode::NONE, true},        {REMOVE, eMode::MAXIMIZED, false},
        {REMOVE, eMode::FULLSCREEN, false}, {TOGGLE, eMode::NONE, false},  {TOGGLE, eMode::MAXIMIZED, false}, {TOGGLE, eMode::FULLSCREEN, false}, {7, eMode::NONE, true},
        {7, eMode::MAXIMIZED, true},
    };
    for (const auto& C : MAX) {
        const auto V = decide({.action = C.action, .maximize = true}, C.current, QUIET, 0);
        expect(V.stripMaximize == C.strip && !V.stripFullscreen && !V.blocked, std::string("maximize action ") + std::to_string(C.action) + " on " + name(C.current));
    }

    // Fullscreen alone: Hyprland already does add/remove/toggle right, never stripped.
    for (uint32_t a : {REMOVE, ADD, TOGGLE})
        for (eMode m : {eMode::NONE, eMode::MAXIMIZED, eMode::FULLSCREEN}) {
            const auto V = decide({.action = a, .fullscreen = true}, m, QUIET, 0);
            expect(!V.stripMaximize && !V.stripFullscreen && !V.blocked, std::string("fullscreen action ") + std::to_string(a) + " on " + name(m));
        }

    // Neither atom (DEMANDS_ATTENTION, HIDDEN...): nothing to judge.
    expect(!decide({.action = ADD}, eMode::MAXIMIZED, QUIET, 0).stripMaximize, "attention only");

    // FULLSCREEN + MAXIMIZED in one message: the maximize half is judged
    // against the mode the fullscreen half leaves.
    expect(afterFullscreen({.action = ADD, .fullscreen = true}, eMode::NONE) == eMode::FULLSCREEN, "add fs -> fs");
    expect(afterFullscreen({.action = REMOVE, .fullscreen = true}, eMode::FULLSCREEN) == eMode::NONE, "remove fs on fs -> none");
    expect(afterFullscreen({.action = REMOVE, .fullscreen = true}, eMode::MAXIMIZED) == eMode::MAXIMIZED, "remove fs on max -> max");
    expect(afterFullscreen({.action = TOGGLE, .fullscreen = true}, eMode::MAXIMIZED) == eMode::FULLSCREEN, "toggle fs on max -> fs");
    expect(afterFullscreen({.action = TOGGLE, .fullscreen = true}, eMode::FULLSCREEN) == eMode::NONE, "toggle fs on fs -> none");
    // add both on a maximized window: fullscreen goes through, maximize is left to Hyprland (it ignores it there)
    auto V = decide({.action = ADD, .maximize = true, .fullscreen = true}, eMode::MAXIMIZED, QUIET, 0);
    expect(!V.stripFullscreen && !V.stripMaximize, "add fs+max on max: both through");
    // remove both on a fullscreen window: fullscreen off, then "remove maximized" on a plain window is a no-op
    V = decide({.action = REMOVE, .maximize = true, .fullscreen = true}, eMode::FULLSCREEN, QUIET, 0);
    expect(!V.stripFullscreen && V.stripMaximize, "remove fs+max on fs: fs through, max stripped");
    // remove both on a maximized window: real un-maximize
    V = decide({.action = REMOVE, .maximize = true, .fullscreen = true}, eMode::MAXIMIZED, QUIET, 0);
    expect(!V.stripFullscreen && !V.stripMaximize, "remove fs+max on max: both through");
    // remove both on a plain window: both no-ops, maximize stripped
    V = decide({.action = REMOVE, .maximize = true, .fullscreen = true}, eMode::NONE, QUIET, 0);
    expect(!V.stripFullscreen && V.stripMaximize, "remove fs+max on none: max stripped");
    // toggle both on a fullscreen window: out of fullscreen, then a real toggle
    V = decide({.action = TOGGLE, .maximize = true, .fullscreen = true}, eMode::FULLSCREEN, QUIET, 0);
    expect(!V.stripFullscreen && !V.stripMaximize, "toggle fs+max on fs: both through");

    // The storm guard.
    {
        CStormGuard g;
        int64_t     t = 10000;
        for (int i = 0; i < CStormGuard::FLIPS; ++i)
            expect(!g.noteFlip(t + i * 100), "flip " + std::to_string(i + 1) + " within a second: no block yet");
        expect(!g.blocked(t + 599), "six flips: still allowed");
        expect(g.noteFlip(t + 600), "seventh flip within a second starts the block");
        expect(g.blocked(t + 601), "blocked right after");
        V = decide({.action = ADD, .maximize = true}, eMode::NONE, g, t + 700);
        expect(V.blocked && V.stripMaximize && !V.stripFullscreen, "blocked: maximize stripped even when it is a change");
        V = decide({.action = ADD, .fullscreen = true}, eMode::NONE, g, t + 700);
        expect(V.blocked && V.stripFullscreen && !V.stripMaximize, "blocked: fullscreen stripped");
        V = decide({.action = ADD}, eMode::NONE, g, t + 700);
        expect(!V.blocked && !V.stripFullscreen && !V.stripMaximize, "blocked: a message naming neither passes");
        expect(!g.noteFlip(t + 800), "a flip while blocked does not start another block");
        expect(g.blocked(t + 600 + CStormGuard::BLOCK - 1), "still blocked just before the end");
        expect(!g.blocked(t + 600 + CStormGuard::BLOCK), "block over");
        V = decide({.action = ADD, .maximize = true}, eMode::NONE, g, t + 600 + CStormGuard::BLOCK);
        expect(!V.blocked && !V.stripMaximize, "after the block: requests through again");
        expect(g.idle(t + 600 + CStormGuard::BLOCK + CStormGuard::WINDOW), "idle once quiet");
    }
    {
        // Slow flips (one every 200 ms = 5/s) never block.
        CStormGuard g;
        bool        any = false;
        for (int i = 0; i < 100; ++i)
            any |= g.noteFlip(i * 200);
        expect(!any && !g.blocked(100 * 200), "5 flips a second for 20 s: never blocked");
    }
    {
        // Seven flips spread over just over a second: the oldest has aged out.
        CStormGuard g;
        bool        any = false;
        for (int i = 0; i < 7; ++i)
            any |= g.noteFlip(i * 167);
        expect(!any, "7 flips over 1002 ms: not a storm");
    }
    {
        // ~15 flips a second (the WoW report): blocked within the first half second.
        CStormGuard g;
        int         at = -1;
        for (int i = 0; i < 15 && at < 0; ++i)
            if (g.noteFlip(i * 66))
                at = i;
        expect(at == CStormGuard::FLIPS, "15/s: blocked on the seventh flip");
    }

    std::printf("%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
