#pragma once
// SHOW DESKTOP's arithmetic (ShowDesktop.hpp), kept apart from Hyprland so a
// host test can hold it (tests/unit/show_desktop_test.cpp): where each window
// goes to be out of view, and the spring that takes it there and back.
#include <algorithm>
#include <cmath>

#include "MotionSpring.hpp"

namespace Hyprdrive::ShowDesktopMath {
    struct SRect {
        double x = 0, y = 0, w = 0, h = 0;
    };
    struct SVec {
        double x = 0, y = 0;
    };

    // The shortest move that takes `win` -- its whole drawn box, border and
    // shadow included -- completely off `screen`, `margin` past the edge. On
    // each axis the shorter way out is always the side the window's centre
    // leans to, so every window leaves outward from the middle of the screen;
    // of the two axes the shorter trip wins, a tie going vertical (a window
    // filling the screen goes up, like a blind). One already off the screen,
    // or with no size, stays where it is.
    inline SVec outOf(const SRect& win, const SRect& screen, double margin) {
        if (win.w <= 0 || win.h <= 0 || win.x >= screen.x + screen.w || win.x + win.w <= screen.x || win.y >= screen.y + screen.h ||
            win.y + win.h <= screen.y)
            return {};
        const double LEFT  = win.x + win.w - screen.x + margin;
        const double RIGHT = screen.x + screen.w - win.x + margin;
        const double UP    = win.y + win.h - screen.y + margin;
        const double DOWN  = screen.y + screen.h - win.y + margin;
        if (std::min(LEFT, RIGHT) < std::min(UP, DOWN))
            return {LEFT <= RIGHT ? -LEFT : RIGHT, 0};
        return {0, UP <= DOWN ? -UP : DOWN};
    }

    // A critically damped spring on the slide's progress: 0 every window in its
    // place, 1 all of them out of view. Critically damped, it never overshoots
    // either end, and a new target mid-flight keeps the position AND the
    // velocity -- a second press turns the windows round where they are, easing
    // out of their run instead of snapping back (DesktopStrip's glide does the
    // same for the desktops). Stepped by the exact solution, so a long frame
    // lands where a train of short ones would have.
    struct SSpring {
        double x = 0; // progress
        double v = 0; // progress per second

        // `dt` seconds toward `target` at angular frequency `omega` (1/s): from
        // rest, 95% of the way takes ~4.74/omega seconds, 99% ~6.64/omega.
        void step(double dt, double target, double omega) {
            // (dt <= 0 skips the clamp below as well, as it always has.)
            if (dt <= 0)
                return;
            Hyprdrive::Motion::criticalStep(x, v, target, omega, dt);
            // Critically damped from rest never crosses its target; from a
            // reversal it may graze it by rounding. Progress stays 0..1.
            x = std::clamp(x, 0.0, 1.0);
        }
        void snap(double target) {
            x = target;
            v = 0;
        }
        // Close enough to call it there: a thousandth of the way and nearly still.
        bool settled(double target) const {
            return std::abs(x - target) < 1e-3 && std::abs(v) < 1e-2;
        }
    };

    // The frequencies: an exit 25% quicker than the return (Apple's rule, as
    // everywhere in Hyprdrive's motion: exits accelerate away, entrances
    // settle). 95% of the way in ~240 ms out, ~300 ms back.
    inline constexpr double OUT_OMEGA = 20.0;
    inline constexpr double IN_OMEGA  = 16.0;
}
