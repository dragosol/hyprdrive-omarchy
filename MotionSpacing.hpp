#pragma once

// MOTION BLUR WITHOUT ARTIFACTS (user, 2026-10-14: "how can we prevent
// artifacting from ever happening again on motion blur?"). Hyprland's motion
// blur shader stacks N sharp copies of the window between its previous box and
// its current one. When the boxes are far apart, the copies are too: 16 copies
// over a 600 px jump land 37 px apart, and what should be a smear reads as a
// striped stack of ghost windows, with hard rectangles where copies of a
// resizing window end (the Mosaic resize recording).
//
// The rule that makes that impossible: consecutive copies are never more than
// MAX_GAP device pixels apart, measured on every EDGE of the box -- so a resize
// counts as well as a move. More samples when the motion is long; when even the
// shader's maximum is not enough, the trail is SHORTENED (the previous box drawn
// toward the current one) instead of spread thin. The configured sample count
// stays the minimum. Pure: scripts/motion_spacing_test.cpp holds it to the rule.

#include <algorithm>
#include <cmath>

namespace ScrollOverview::MotionSpacing {

    inline constexpr double MAX_GAP     = 2.0; // device px between consecutive copies
    inline constexpr int    MAX_SAMPLES = 64;  // the shader's MOTION_BLUR_MAX_SAMPLES

    // (named so it cannot collide with the plugin's sc<> helper)
    inline int sc_int(double v) {
        return static_cast<int>(v);
    }

    struct SBox {
        double x = 0, y = 0, w = 0, h = 0;
    };

    struct SPlan {
        int    samples = 0;
        double keep    = 1.0; // how much of the trail to draw: prev' = cur + (prev - cur) * keep
        SBox   previous;      // the previous box to hand the shader
    };

    // The longest distance any edge travels, in device px.
    inline double travel(const SBox& prev, const SBox& cur, double scale) {
        const double S = std::max(scale, 0.01);
        const double L = std::abs(prev.x - cur.x), T = std::abs(prev.y - cur.y);
        const double R = std::abs((prev.x + prev.w) - (cur.x + cur.w)), B = std::abs((prev.y + prev.h) - (cur.y + cur.h));
        return std::max(std::max(L, R), std::max(T, B)) * S;
    }

    // `prev`, `cur` in the same (logical) units; `scale` turns them into device
    // px; `minSamples` is the configured count; `maxSamples` a caller's cost
    // ceiling (a workspace swipe blurs two desktops of windows at once) -- a
    // lower ceiling only shortens the trail, it never spreads the copies.
    inline SPlan plan(const SBox& prev, const SBox& cur, double scale, int minSamples, int maxSamples = MAX_SAMPLES) {
        SPlan        out;
        const double D    = travel(prev, cur, scale);
        const int    CAP  = std::clamp(maxSamples, 2, MAX_SAMPLES);
        const int    BASE = std::clamp(minSamples, 2, CAP);
        // N copies span N-1 gaps... the shader places them at t = i/N, i < N, so
        // the gap is D / N and the last copy sits one gap short of `prev`.
        const int NEED = sc_int(std::ceil(D / MAX_GAP));
        out.samples    = std::clamp(std::max(BASE, NEED), 2, CAP);
        out.keep       = D <= out.samples * MAX_GAP ? 1.0 : (out.samples * MAX_GAP) / D;
        out.previous   = SBox{cur.x + (prev.x - cur.x) * out.keep, cur.y + (prev.y - cur.y) * out.keep, cur.w + (prev.w - cur.w) * out.keep,
                              cur.h + (prev.h - cur.h) * out.keep};
        return out;
    }

}
