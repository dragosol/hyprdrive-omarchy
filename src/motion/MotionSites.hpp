#pragma once

// PER-SITE CURVES: the sites that keep a hand-rolled ease under the house
// preset and take the active preset's curve under any other (Motion.hpp: "sites
// ask housePreset() and keep their own function in that case"). Each used to
// write that branch itself; Motion::curve(site, t) is the branch, once, and
// this header is the table it reads -- which role a site takes from another
// preset, and what it draws under the house one.
//
// Pure (Motion.hpp's ERole and MotionCurves.hpp), so
// tests/unit/motion_math_test.cpp holds every site, under every preset, to the
// branch it replaced, bit for bit. Each house curve is the site's own, clamp
// and all: a site whose caller clamped keeps no clamp of its own here.

#include "Motion.hpp"
#include "MotionCurves.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace Hyprdrive::Motion {

    enum class ESite : uint8_t {
        // Y5 (motion/FlightController.cpp). GNOME's MINIMIZE_WINDOW_ANIMATION_MODE
        // is Clutter's EASE_OUT_EXPO, 1 - 2^(-10t) snapped to exactly 1 at the
        // end. Extracted from the host's windowManager.js, not recalled -- see
        // spec 13.14's reference-discipline note. Other presets: DECEL.
        FLIGHT,
        // X75 (dock/DockLayout.cpp, autohide). dash-to-dock eases the slider
        // with Clutter's EASE_OUT_QUAD in BOTH directions, so the hide does NOT
        // accelerate away the way the house exit curve does; matching the
        // reference means dropping the house asymmetry here, by instruction.
        // Other presets: DECEL, both ways.
        DOCK_REVEAL,
        // dock/DockLayout.cpp, icons gliding to a new arrangement: ease-out
        // cubic, fast away, soft landing. Other presets: DECEL.
        DOCK_ARRANGE,
        // dock/DockLayout.cpp, a new icon: ease-out back, a touch of overshoot
        // so it lands. Other presets: POP.
        DOCK_SPAWN,
        // Y6 (dock/Dock.cpp), GNOME's launch blow-up: ease-out quad (the
        // caller clamps). Other presets: DECEL.
        DOCK_LAUNCH_ZOOM,
        // X73 (overview/TileChrome.cpp), the tile hover: Clutter's EASE_OUT_QUAD,
        // what GNOME Shell puts on its overview and dash hover, the same both
        // ways (the caller clamps). Other presets: DECEL.
        TILE_HOVER,
    };

    // The role a site takes from any preset but the house one.
    inline ERole siteRole(ESite site) {
        return site == ESite::DOCK_SPAWN ? ERole::POP : ERole::DECEL;
    }

    // The site's own curve, under the house preset.
    inline float houseCurve(ESite site, float t) {
        switch (site) {
            case ESite::FLIGHT: return t >= 1.F ? 1.F : 1.F - std::pow(2.F, -10.F * t);
            case ESite::DOCK_REVEAL: return outQuad(std::clamp(t, 0.F, 1.F));
            case ESite::DOCK_ARRANGE: return outCubic(std::clamp(t, 0.F, 1.F));
            case ESite::DOCK_SPAWN: {
                t                  = std::clamp(t, 0.F, 1.F);
                constexpr float C1 = 1.75F; // 1.25 + two 0.25 steps: peak overshoot ~6% -> ~10%
                constexpr float C3 = C1 + 1.F;
                const float     U  = t - 1.F;
                return 1.F + C3 * U * U * U + C1 * U * U;
            }
            case ESite::DOCK_LAUNCH_ZOOM:
            case ESite::TILE_HOVER: return outQuad(t);
        }
        return t;
    }

    // The site's curve under the active preset (Motion.cpp): resolveCurve with
    // Motion::housePreset() and Motion::ease.
    float curve(ESite site, float t);

    // The branch every site wrote: `house` first (Motion::housePreset(), read
    // once, before anything else), then the site's own curve or the preset's
    // curve for the site's role. Motion::curve() is this with the live preset;
    // the test hands it each preset in turn.
    template <typename PresetEase>
    inline float resolveCurve(ESite site, float t, bool house, PresetEase&& presetEase) {
        if (!house)
            return presetEase(siteRole(site), t);
        return houseCurve(site, t);
    }
}
