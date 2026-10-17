#pragma once

#include "globals.hpp"

#include <cstdint>
#include <string_view>

// THE BORDER FADE (user, 2026-10-16): when focus moves to a window, Hyprland
// does not snap its border to col.active_border -- it fades it over the
// `border` animation (Omarchy and fedora: 5.39 = 539 ms on easeOutQuint). Every
// ring in hyprdrive that goes from the resting (inactive) colour to the lit
// (active) one when hovered or selected fades the same way, on the same leaf,
// so the chrome and the windows change state at one speed.
//
// Only the PROGRESS is kept, never a colour: both ends are passed in on every
// frame, read from the theme as they are now, so a theme switch -- even one in
// the middle of a fade -- lands at once. The duration and curve come off the
// animation tree, re-read when a config reload bumps Config::themeGeneration();
// `border` disabled (or speed 0) means the ring snaps, as a window's border would.
//
// A key is one ring: a window, a plate, a cell. A key seen for the first time
// starts RESTING, so one that is lit fades in -- the overview does not redraw
// an idle tile, so its key is forgotten (keys not drawn for a while are) and
// the hover that brings it back must still fade.
namespace ScrollOverview::BorderFade {
    uint64_t key(const void* p, uint64_t salt = 0);
    uint64_t key(std::string_view tag, uint64_t salt = 0);

    // 0 = resting, 1 = lit, eased. Drawn on `monitor`; it asks for no frames
    // itself -- see animatingOn.
    float mix(const PHLMONITOR& monitor, uint64_t key, bool lit);

    // resting -> lit by mix(), component-wise in straight RGBA.
    CHyprColor color(const PHLMONITOR& monitor, uint64_t key, bool lit, const CHyprColor& resting, const CHyprColor& active);

    // A fade drawn on `monitor` is still in flight. The frames it needs are
    // asked for by the pumps that already exist, never from inside the pass
    // (damage asked for during the pass is cleared with the pass, and the
    // overview throttles plain damage): Dock::needsFramesReason on the
    // desktop, the overview's reflow pacer while one is open.
    bool animatingOn(const PHLMONITOR& monitor);

    // Mid-fade? (`key` drawn and not yet at its target.)
    bool fading(uint64_t key);

    // Plugin exit.
    void clear();
}
