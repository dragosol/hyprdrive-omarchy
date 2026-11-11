// See Motion.hpp.
#include "Motion.hpp"
#include "MotionPresets.hpp"

#include "Config.hpp"
#include "globals.hpp"

#include <hyprland/src/animation/AnimationManager.hpp>
#include <hyprutils/animation/BezierCurve.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <string_view>

namespace Hyprdrive::Motion {
    namespace {
        using Hyprutils::Math::Vector2D;

        using namespace Presets;

        struct SLive {
            const SPreset* preset = nullptr;
            // Hyprland's registry was pointed at this preset (0 = never).
            const SPreset* synced = nullptr;
        };
        SLive g_live;

        // The baked curve for `role` of preset `p` (MotionPresets.hpp).
        const Hyprutils::Animation::CBezierCurve& curveOf(const SPreset& p, ERole role) {
            return bakedPresets()[indexOf(p)][sc<size_t>(role)];
        }

        // The preset config says now. Read through the cached handle: this runs
        // for every eased value of every frame. Before the plugin's keys are
        // registered a read would abort Hyprland (Config.hpp), so it is the
        // house preset until then.
        const SPreset& current() {
            const SPreset* want = &PRESETS[0];
            if (Config::g_pluginKeysRegistered) {
                // std::string, as every plugin string key is stored (Config.hpp
                // getValue): read as a const char* it was garbage, never matched
                // a preset, and every preset silently fell back to the house one.
                static auto& VAL = Config::valueRef<std::string>("plugin:hyprdrive:animation:curve");
                const std::string& S = *VAL;
                want = g_live.preset && S == g_live.preset->name ? g_live.preset : &presetNamed(S);
            }
            g_live.preset = want;
            return *g_live.preset;
        }
    }

    bool housePreset() {
        return &current() == &PRESETS[0];
    }

    float ease(ERole role, float t) {
        const float T = std::clamp(t, 0.F, 1.F);
        current();
        // A preset changed live (the panel writes hl.config, no reload): the
        // overview's own animated variables read the NAMED beziers, so point
        // those at it too.
        if (g_live.synced != g_live.preset)
            sync();
        return curveOf(*g_live.preset, role).getYForPoint(T);
    }

    float easeNamed(const std::string& preset, ERole role, float t) {
        // The shared bake: a pan reads this every frame, and two names in
        // turn no longer re-bake each other out of a single cache slot.
        return curveOf(presetNamed(preset), role).getYForPoint(std::clamp(t, 0.F, 1.F));
    }

    float houseFixed(bool decel, float t) {
        return curveOf(PRESETS[0], decel ? ERole::DECEL : ERole::ACCEL).getYForPoint(std::clamp(t, 0.F, 1.F));
    }

    void sync() {
        const auto& P = current();
        if (!Animation::mgr())
            return;
        // addBezierWithName replaces a curve of the same name, and the animated
        // variables look their bezier up by name at every step, so an overview
        // already open follows too. Re-adding also restores a curve a config
        // reload dropped.
        Animation::mgr()->addBezierWithName(HOUSE_DECEL_BEZIER, Vector2D{P.decel.x1, P.decel.y1}, Vector2D{P.decel.x2, P.decel.y2});
        Animation::mgr()->addBezierWithName(HOUSE_ACCEL_BEZIER, Vector2D{P.accel.x1, P.accel.y1}, Vector2D{P.accel.x2, P.accel.y2});
        g_live.synced = &P;
    }
}
