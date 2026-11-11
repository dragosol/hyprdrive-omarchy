#pragma once

// FULLSCREEN THROUGH A LAYOUT SWAP, PURE (user, 2026-11-11: `hyprdrive reload`
// took every fullscreen window out of fullscreen on a mosaic desktop -- a
// running game dropped to windowed; on dwindle it stayed).
//
// Hyprland keeps a tiled window's fullscreen modes in the fullscreen handler
// OWNED BY THE DESKTOP'S TILED ALGORITHM (IModeAlgorithm::
// m_defaultFullscreenHandler, its m_fsTargets). Removing or adding a layout
// (CWorkspaceAlgoMatcher::unregisterAlgo / registerTiledAlgo ->
// updateWorkspaceLayouts -> CAlgorithm::updateTiledAlgo) gives each desktop on
// that layout a NEW algorithm object and drops the old one with its handler:
// - unload, mosaic -> dwindle: the modes are simply forgotten (nothing is
//   sent; the window is no longer fullscreen to Hyprland, and the next
//   recalculate tiles it);
// - load, dwindle -> mosaic: CDwindleAlgorithm::removeTarget calls
//   setFullscreenMode(NONE) on a fullscreen window -- a real exit, sent to the
//   client.
// Mosaic.cpp records the modes before the swap and writes them straight into
// the new handler after it (and, at load, moves them out of dwindle's into the
// desktop's floating handler first, so dwindle's removeTarget has nothing to
// drop while its recalculate still sees a fullscreen desktop), with the
// handlers' own setters: no event, no configure, nothing the client sees.
// Kept by window address and desktop id, and only ever matched against the
// live windows.

#include <cstdint>
#include <vector>

namespace Hyprdrive::FullscreenCarry {
    // Fullscreen::eFullscreenMode's values: 0 none, 1 maximized, 2 fullscreen.
    constexpr int NONE = 0, MAXIMIZED = 1, FULLSCREEN = 2;

    struct SMode {
        int  internal = NONE;
        int  client   = NONE;
        bool operator==(const SMode&) const = default;
    };

    struct SCarried {
        std::uintptr_t win = 0; // the window's address: matched, never dereferenced
        long long      ws  = 0; // its desktop's id when recorded
        SMode          mode;
    };

    constexpr bool validMode(int m) {
        return m >= NONE && m <= FULLSCREEN;
    }
    // Worth carrying: well formed, and fullscreen or maximized in either mode.
    constexpr bool worth(SMode m) {
        return validMode(m.internal) && validMode(m.client) && (m.internal != NONE || m.client != NONE);
    }

    // The record for this live window: same address AND same desktop. A window
    // that is not the one recorded (address 0, another desktop) has none.
    inline const SCarried* find(const std::vector<SCarried>& carried, std::uintptr_t win, long long ws) {
        if (!win)
            return nullptr;
        for (const auto& C : carried)
            if (C.win == win && C.ws == ws && worth(C.mode))
                return &C;
        return nullptr;
    }

    // Write it back only where the handler now says something else.
    constexpr bool needsSeed(SMode carried, SMode live) {
        return worth(carried) && !(carried == live);
    }
}
