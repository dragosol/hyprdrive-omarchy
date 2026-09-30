// See Motion.hpp.
#include "Motion.hpp"

#include "Config.hpp"
#include "globals.hpp"

#include <hyprland/src/animation/AnimationManager.hpp>
#include <hyprutils/animation/BezierCurve.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <string_view>

namespace ScrollOverview::Motion {
    namespace {
        using Hyprutils::Math::Vector2D;

        // Two control points per curve, the CSS cubic-bezier() convention
        // (x1, y1, x2, y2), so every preset can be checked against easings.net.
        struct SCurve {
            float x1, y1, x2, y2;
        };
        struct SPreset {
            std::string_view name;
            SCurve           decel, accel, pop;
        };

        // "hyprdrive" is the house pair (hyprdriveOpen / hyprdriveClose, the
        // rice's appleDecel / appleAccel) -- the numbers the constructor has
        // always registered. Its POP is only read when another preset is on:
        // with the house preset every hand-rolled site keeps its own ease.
        constexpr std::array<SPreset, 6> PRESETS{{
            {"hyprdrive", {0.16F, 1.F, 0.3F, 1.F}, {0.7F, 0.F, 0.84F, 0.F}, {0.34F, 1.56F, 0.64F, 1.F}},
            // ilyamiro/serpantinum: OutQuint travel, InQuint exit, OutBack pop
            // (the dock stacks already run on these numbers).
            {"serpantinum", {0.22F, 1.F, 0.36F, 1.F}, {0.64F, 0.F, 0.78F, 0.F}, {0.34F, 1.40F, 0.64F, 1.F}},
            // Softer: OutCubic / InCubic, a gentle pop.
            {"smooth", {0.33F, 1.F, 0.68F, 1.F}, {0.32F, 0.F, 0.67F, 0.F}, {0.33F, 1.25F, 0.68F, 1.F}},
            // Quick off the mark: OutCirc / InCirc.
            {"swift", {0.F, 0.55F, 0.45F, 1.F}, {0.55F, 0.F, 1.F, 0.45F}, {0.F, 0.55F, 0.45F, 1.F}},
            // Everything lands with a little overshoot.
            {"springy", {0.34F, 1.30F, 0.64F, 1.F}, {0.36F, 0.F, 0.66F, -0.3F}, {0.34F, 1.70F, 0.64F, 1.F}},
            {"linear", {0.F, 0.F, 1.F, 1.F}, {0.F, 0.F, 1.F, 1.F}, {0.F, 0.F, 1.F, 1.F}},
        }};

        const SPreset& presetNamed(std::string_view name) {
            for (const auto& P : PRESETS)
                if (P.name == name)
                    return P;
            return PRESETS[0];
        }

        struct SLive {
            const SPreset*                                preset = nullptr;
            std::array<Hyprutils::Animation::CBezierCurve, 3> curves;
            Hyprutils::Animation::CBezierCurve            houseDecel, houseAccel;
            bool                                          houseBaked = false;
            // Hyprland's registry was pointed at this preset (0 = never).
            const SPreset* synced = nullptr;
        };
        SLive g_live;

        void bake(Hyprutils::Animation::CBezierCurve& curve, const SCurve& c) {
            curve.setup(std::array<Vector2D, 2>{Vector2D{c.x1, c.y1}, Vector2D{c.x2, c.y2}});
        }

        // The preset config says now. Read through the cached handle: this runs
        // for every eased value of every frame. Before the plugin's keys are
        // registered a read would abort Hyprland (Config.hpp), so it is the
        // house preset until then.
        const SPreset& current() {
            const SPreset* want = &PRESETS[0];
            if (Config::g_pluginKeysRegistered) {
                static auto& VAL = Config::valueRef<Hyprlang::STRING>("plugin:hyprdrive:animation:curve");
                if (const char* S = *VAL; S && std::strcmp(S, g_live.preset ? g_live.preset->name.data() : "") == 0)
                    want = g_live.preset;
                else if (S)
                    want = &presetNamed(S);
            }
            if (want != g_live.preset) {
                g_live.preset = want;
                bake(g_live.curves[0], want->decel);
                bake(g_live.curves[1], want->accel);
                bake(g_live.curves[2], want->pop);
            }
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
        return g_live.curves[sc<size_t>(role)].getYForPoint(T);
    }

    float houseFixed(bool decel, float t) {
        if (!g_live.houseBaked) {
            bake(g_live.houseDecel, PRESETS[0].decel);
            bake(g_live.houseAccel, PRESETS[0].accel);
            g_live.houseBaked = true;
        }
        return (decel ? g_live.houseDecel : g_live.houseAccel).getYForPoint(std::clamp(t, 0.F, 1.F));
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
