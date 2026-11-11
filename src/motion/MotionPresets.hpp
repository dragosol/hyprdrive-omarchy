#pragma once

// THE CURVE PRESETS behind plugin:hyprdrive:animation:curve (see Motion.hpp),
// and their bakes. Pure -- hyprutils' CBezierCurve, no compositor, no config --
// so tests/unit/motion_math_test.cpp can hold the baked table to the bakes it
// replaced.
//
// Every preset x role is baked ONCE, into one table (bakedPresets()). Before,
// the same numbers were baked three or four times over: the active preset into
// Motion.cpp's live slots (again on every preset change), the house pair again
// for houseFixed, and easeNamed's single-slot cache, which re-baked whenever two
// names alternated. A CBezierCurve's answer depends only on its two control
// points, so reading the shared bake returns the same bits as each of those.

#include <hyprutils/animation/BezierCurve.hpp>
#include <hyprutils/math/Vector2D.hpp>

#include <array>
#include <cstddef>
#include <string_view>

namespace Hyprdrive::Motion::Presets {

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
    // Serpantinum (ilyamiro/serpantinum) is a FAMILY, not one curve
    // (user, 2026-09-30: "you picked the wrong curve, it has several"):
    // its Hyprland config runs windows, layers and workspaces on myBezier;
    // its Quickshell eases panels with OutCubic, long travel with OutQuint,
    // intros with OutExpo, pops with OutBack (overshoot ~1.2), and exits
    // with InCubic. Each is a preset here, paired with Serpantinum's exit
    // and pop. The Qt easings are fitted as cubic-beziers (least squares
    // over 51 points; worst residual ~3e-5).
    inline constexpr SCurve SERP_EXIT{0.33F, 0.F, 0.67F, 0.F};      // InCubic
    inline constexpr SCurve SERP_POP{0.33F, 1.39F, 0.66F, 1.F};     // OutBack, overshoot 1.2
    inline constexpr std::array<SPreset, 9> PRESETS{{
        {"hyprdrive", {0.16F, 1.F, 0.3F, 1.F}, {0.7F, 0.F, 0.84F, 0.F}, {0.34F, 1.56F, 0.64F, 1.F}},
        // myBezier, compositors/hyprland/config/settings.lua -- used in
        // and out alike, so the exit is the same curve.
        {"serpantinum", {0.05F, 0.9F, 0.1F, 1.05F}, {0.05F, 0.9F, 0.1F, 1.05F}, SERP_POP},
        {"serpantinum_glide", {0.22F, 1.09F, 0.46F, 1.F}, SERP_EXIT, SERP_POP},  // OutQuint
        {"serpantinum_soft", {0.33F, 1.F, 0.66F, 1.F}, SERP_EXIT, SERP_POP},     // OutCubic
        {"serpantinum_intro", {0.15F, 1.04F, 0.35F, 0.99F}, SERP_EXIT, SERP_POP}, // OutExpo
        {"serpantinum_pop", SERP_POP, SERP_EXIT, SERP_POP},                        // OutBack 1.2
        // Quick off the mark: OutCirc / InCirc.
        {"swift", {0.F, 0.55F, 0.45F, 1.F}, {0.55F, 0.F, 1.F, 0.45F}, {0.F, 0.55F, 0.45F, 1.F}},
        // Everything lands with a little overshoot.
        {"springy", {0.34F, 1.30F, 0.64F, 1.F}, {0.36F, 0.F, 0.66F, -0.3F}, {0.34F, 1.70F, 0.64F, 1.F}},
        {"linear", {0.F, 0.F, 1.F, 1.F}, {0.F, 0.F, 1.F, 1.F}, {0.F, 0.F, 1.F, 1.F}},
    }};

    inline const SPreset& presetNamed(std::string_view name) {
        for (const auto& P : PRESETS)
            if (P.name == name)
                return P;
        return PRESETS[0];
    }

    // A preset's row in PRESETS (and in bakedPresets()).
    inline size_t indexOf(const SPreset& p) {
        return static_cast<size_t>(&p - PRESETS.data());
    }

    inline void bake(Hyprutils::Animation::CBezierCurve& curve, const SCurve& c) {
        curve.setup(std::array<Hyprutils::Math::Vector2D, 2>{Hyprutils::Math::Vector2D{c.x1, c.y1}, Hyprutils::Math::Vector2D{c.x2, c.y2}});
    }

    // [preset][role]: DECEL, ACCEL, POP (Motion::ERole's order).
    using SBakedTable = std::array<std::array<Hyprutils::Animation::CBezierCurve, 3>, PRESETS.size()>;

    // Baked on first use, once: the table is immutable after that, so every
    // reader shares it.
    inline const SBakedTable& bakedPresets() {
        static const SBakedTable TABLE = [] {
            SBakedTable t;
            for (size_t i = 0; i < PRESETS.size(); ++i) {
                bake(t[i][0], PRESETS[i].decel);
                bake(t[i][1], PRESETS[i].accel);
                bake(t[i][2], PRESETS[i].pop);
            }
            return t;
        }();
        return TABLE;
    }
}
