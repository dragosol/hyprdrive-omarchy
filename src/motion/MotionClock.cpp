// See MotionClock.hpp: the live half of the animation clock.
#include "MotionClock.hpp"

#include "Config.hpp"

#include <algorithm>

namespace Hyprdrive::Motion::Clock {
    namespace {
        // The single hottest read in the plugin: every eased thing on screen
        // asks it at least once a frame. A cached handle (a double indirection,
        // Config.hpp valueRef), no string lookup, read live: the panel's speed
        // slider reaches the next frame without a reload.
        float liveScale() {
            // Before registration a plugin: read aborts Hyprland (Config.hpp).
            if (!Config::g_pluginKeysRegistered)
                return 1.F;
            static auto& SCALE = Config::valueRef<Hyprlang::FLOAT>("plugin:hyprdrive:anim_scale");
            return std::max(0.F, sc<float>(*SCALE));
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
