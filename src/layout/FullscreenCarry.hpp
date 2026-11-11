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
//
// ...AND THROUGH THE GAP BETWEEN UNLOAD AND LOAD (user, 2026-11-11, the
// laptop: `hyprdrive reload` took three fullscreen windows out of
// fullscreen, and one on a trip "went home"). The unload's carry lasts one
// call; the plugin is gone until the load. Hyprland's own config reload,
// scheduled by every `plugin unload` (PluginSystem.cpp), runs REFRESH_ALL,
// and CWorkspaceAlgoMatcher::updateWorkspaceLayouts then sees each desktop
// whose configured layout is "mosaic" -- missing -- on "dwindle", calls that a
// change, and swaps in a NEW dwindle: the old one's removeTarget takes the
// window out of fullscreen (sent to the client). Every refresh does it again
// (a renumber: CWorkspace::changeID, Omarchy's change_id; a monitor rule).
// Nothing the plugin leaves in Hyprland can stop that, so the unload hands
// the modes over (SHandover, in the environment, as FullscreenBook's trips)
// and the load puts back what the gap took: by window address AND desktop
// object -- a renumber or a move to another monitor keeps the object, so
// neither loses the record -- only for a window that is in no mode at all now
// (one fullscreen or maximized again, by its client or the user, is left as
// it is), and only right after the unload (HANDOVER_MS: a plugin left
// unloaded for longer has Hyprland's behaviour).

#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
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

    // ---- the gap between unload and load
    struct SHanded {
        std::uintptr_t win = 0; // the window's address
        std::uintptr_t ws  = 0; // its desktop's address (survives a renumber and a monitor move)
        SMode          mode;
        bool           operator==(const SHanded&) const = default;
    };
    struct SHandover {
        long long            savedMs = 0; // steady clock, ms: the same process reads it
        std::vector<SHanded> wins;
    };
    constexpr long long   HANDOVER_MS = 10000;
    constexpr const char* HANDOVER_TAG = "hdcarry1";

    inline std::string save(const SHandover& h) {
        std::ostringstream o;
        o << HANDOVER_TAG << ' ' << h.savedMs;
        for (const auto& W : h.wins)
            o << " W " << W.win << ' ' << W.ws << ' ' << W.mode.internal << ' ' << W.mode.client;
        return o.str();
    }
    // Anything malformed, or another build's format: nothing.
    inline std::optional<SHandover> load(std::string_view text) {
        std::istringstream in{std::string{text}};
        std::string        tag, kind;
        SHandover          out;
        if (!(in >> tag) || tag != HANDOVER_TAG || !(in >> out.savedMs))
            return std::nullopt;
        while (in >> kind) {
            SHanded W;
            if (kind != "W" || !(in >> W.win >> W.ws >> W.mode.internal >> W.mode.client))
                return std::nullopt;
            out.wins.push_back(W);
        }
        return out;
    }

    // Read at a load this soon after the unload (a clock that went back: no).
    constexpr bool fresh(long long savedMs, long long nowMs) {
        return nowMs >= savedMs && nowMs - savedMs <= HANDOVER_MS;
    }
    // The record for this live window on this live desktop (both addresses).
    inline const SHanded* findHanded(const SHandover& h, std::uintptr_t win, std::uintptr_t ws) {
        if (!win || !ws)
            return nullptr;
        for (const auto& W : h.wins)
            if (W.win == win && W.ws == ws && worth(W.mode))
                return &W;
        return nullptr;
    }
    // Put back at the load: it had a mode at the unload and has none now.
    constexpr bool lostInGap(SMode handed, SMode live) {
        return worth(handed) && live == SMode{};
    }
}
