// See MotionClock.hpp: the live half of the animation clock.
#include "MotionClock.hpp"

#include "Config.hpp"

#include <algorithm>

namespace Hyprdrive::Motion::Clock {
    namespace {
        // The single hottest read in the plugin: every eased thing on screen
        // asks it at least once a frame. Two cached handles (a double
        // indirection each, Config.hpp valueRef), no string lookup, read live:
        // the panel's speed slider and an `animations:enabled` change reach the
        // next frame without a reload.
        //
        // Hyprland's animations:enabled = 0 folds in as scale 0 (owner,
        // 2026-11-10: "snap everything when animations are off"): every
        // hand-made animation then snaps, as Hyprland's own variables do.
        float liveScale() {
            // Before registration a plugin: read aborts Hyprland (Config.hpp).
            if (!Config::g_pluginKeysRegistered)
                return 1.F;
            static auto& ENABLED = Config::valueRef<Hyprlang::INT>("animations:enabled");
            static auto& SCALE   = Config::valueRef<Hyprlang::FLOAT>("plugin:hyprdrive:anim_scale");
            return effectiveScale(sc<float>(*SCALE), *ENABLED != 0);
        }

        // At load: from here on every Clock::scale() is the config's.
        [[maybe_unused]] const bool g_sourced = (g_scaleSource = &liveScale, true);
    }

    float realMs(const TP& since) {
        return msBetween(Time::steadyNow(), since);
    }

    float animMs(const TP& since) {
        return scaleMs(realMs(since), scale());
    }
}
