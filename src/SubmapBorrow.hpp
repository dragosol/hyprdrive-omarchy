#pragma once

// Which of the user's binds stay live while the overview holds its submap
// (scrollOverview.cpp, borrowGlobalPassthroughBinds). Pure, so the rule is
// host-tested: scripts/submap_borrow_test.cpp.
//
// The submap exists for its click binds only -- inside it the global
// SUPER+mouse:272 drag bind cannot eat a click -- but Hyprland then matches
// only that submap's binds, so every keyboard bind went dead with the
// overview open. Two kinds are borrowed back for as long as it is up:
//
//   - the desktop's keys: Print/Sys_Req and the XF86 family (volume,
//     brightness, media) -- Omarchy's screenshot and media binds;
//   - CHORDS, a key with Super/Ctrl/Alt held (O5/B19): the README's own
//     SUPER+TAB toggle opened the overview and could never close it, SUPER+H
//     never minimized, a SUPER+S scratchpad toggle was inert. A chord is never
//     query text; the keys the overview takes for itself (SUPER+W on a tile,
//     Mosaic's SUPER+arrows, a stack popup's keys) are cancelled at the key
//     event, before Hyprland matches any bind.
//
// Never a mouse bind (the submap's own job), a catch-all, a plain or
// Shift-only key (the search field's), or a bind ON a modifier key -- a
// SUPER_L bind is the tap the plugin reads itself.

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>

namespace ScrollOverview::SubmapBorrow {

// Hyprland's HL_MODIFIER_* bits (IKeyboard.hpp, xkb's modifier order);
// scrollOverview.cpp static_asserts they still agree.
inline constexpr uint32_t MOD_SHIFT = 1u << 0;
inline constexpr uint32_t MOD_CTRL  = 1u << 2;
inline constexpr uint32_t MOD_ALT   = 1u << 3;
inline constexpr uint32_t MOD_META  = 1u << 6;
inline constexpr uint32_t CHORD_MODS = MOD_META | MOD_CTRL | MOD_ALT;

inline std::string lowered(std::string_view in) {
    std::string out{in};
    std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

inline bool isDesktopKey(std::string_view key) {
    const auto K = lowered(key);
    return K.starts_with("xf86") || K == "print" || K == "sys_req";
}

inline bool isChord(std::string_view key, uint32_t modmask, bool mouse, bool catchAll) {
    if (mouse || catchAll || key.empty() || (modmask & CHORD_MODS) == 0)
        return false;

    const auto K = lowered(key);
    if (K.starts_with("mouse"))
        return false;
    for (const std::string_view MOD : {"super", "control", "ctrl", "alt", "shift", "meta", "hyper"})
        if (K.starts_with(MOD))
            return false;
    return true;
}

inline bool borrows(std::string_view key, uint32_t modmask, bool mouse, bool catchAll) {
    return isDesktopKey(key) || isChord(key, modmask, mouse, catchAll);
}

}
