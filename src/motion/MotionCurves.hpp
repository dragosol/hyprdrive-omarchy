#pragma once

// THE NAMED EASING CURVES, one definition each, for the sites that used to
// spell their own. Pure (<cmath> only), so the pure headers (SearchLetters) and
// their host tests can use it.
//
// EXACT, NOT "THE SAME MATHS". Each function is the spelling its sites had, and
// tests/unit/motion_math_test.cpp holds it to every copy it replaced, bit for
// bit. That is why two spellings of one curve can sit here under two names:
// 1 - U*U*U and 1 - pow(U, 3) are the same cubic and do not round the same, so
// a site keeps the one it had (outCubic vs outCubicPow). Folding them together
// is a behaviour change, decided elsewhere.
//
// t is NOT clamped. A site that clamped keeps its clamp at the call, written as
// it was; a site that did not clamp still does not.

#include <cmath>

namespace Hyprdrive::Motion {

    // Ease-out quadratic, 1 - (1-t)^2, as products (Clutter's EASE_OUT_QUAD).
    inline float outQuad(float t) {
        const float INV = 1.F - t;
        return 1.F - INV * INV;
    }

    // Ease-out cubic, 1 - (1-t)^3, as products.
    inline float outCubic(float t) {
        const float U = 1.F - t;
        return 1.F - U * U * U;
    }

    // Ease-out cubic through pow -- the stacks' Qt port and the double sites.
    inline float outCubicPow(float t) {
        return 1.F - std::pow(1.F - t, 3.F);
    }
    inline double outCubicPow(double t) {
        return 1.0 - std::pow(1.0 - t, 3.0);
    }

    // Ease-out quintic, 1 - (1-t)^5, as products.
    inline float outQuint(float t) {
        const float INV = 1.F - t;
        return 1.F - INV * INV * INV * INV * INV;
    }

    // Ease-out quintic through pow (the stacks' Qt port).
    inline float outQuintPow(float t) {
        return 1.F - std::pow(1.F - t, 5.F);
    }

    // Qt's Easing.OutBack with overshoot s, in Qt's own grouping. (The dock's
    // spawnEase is the same curve spelled 1 + C3 U^3 + C1 U^2, which rounds
    // differently, so it is not this.)
    inline float outBack(float t, float s) {
        t -= 1.F;
        return t * t * ((s + 1.F) * t + s) + 1.F;
    }

    // Smoothstep, t^2 (3 - 2t), in float or in double.
    inline float smoothstep(float t) {
        return t * t * (3.F - 2.F * t);
    }
    inline double smoothstep(double t) {
        return t * t * (3.0 - 2.0 * t);
    }
}
