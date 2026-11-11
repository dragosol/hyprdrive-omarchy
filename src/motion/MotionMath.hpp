#pragma once

// THE SHARED MOTION ARITHMETIC: one definition for maths that used to be
// copied into each module that animates. Pure (hyprutils maths types, no
// compositor, no config, no clock), so tests/unit/motion_math_test.cpp holds
// every function here to the copies it replaced, bit for bit.
//
// THE RULE FOR WHAT MAY LIVE HERE: a function joins only if it returns the
// SAME BITS as every copy it replaces, at every call site. Where two copies
// spell the same maths differently (a product against a pow, float against
// double) and so round differently, both spellings stay, under two names --
// merging them would be a behaviour change, and those are decided separately.

#include <hyprutils/math/Box.hpp>
#include <hyprutils/math/Vector2D.hpp>

namespace Hyprdrive::Motion {

    // --- lerp -----------------------------------------------------------------
    // A box: every edge in double, t widened from float. Was dock/Dock.cpp's and
    // motion/FlightController.cpp's lerpBox (byte-identical) and
    // overview/OverviewGeometry.cpp's (the same expression, spelled out).
    inline Hyprutils::Math::CBox lerp(const Hyprutils::Math::CBox& from, const Hyprutils::Math::CBox& to, float t) {
        return Hyprutils::Math::CBox{
            from.x + (to.x - from.x) * t,
            from.y + (to.y - from.y) * t,
            from.w + (to.w - from.w) * t,
            from.h + (to.h - from.h) * t,
        };
    }

    // A float, in float. Was OverviewGeometry.cpp's hyprlerp.
    inline float lerp(float from, float to, float perc) {
        return (to - from) * perc + from;
    }

    // A point, through FLOAT: each coordinate is narrowed to float, lerped in
    // float and widened back -- not a double lerp, which rounds differently.
    // Was OverviewGeometry.cpp's hyprlerp(Vector2D).
    inline Hyprutils::Math::Vector2D lerp(const Hyprutils::Math::Vector2D& from, const Hyprutils::Math::Vector2D& to, float perc) {
        return Hyprutils::Math::Vector2D{lerp(static_cast<float>(from.x), static_cast<float>(to.x), perc), lerp(static_cast<float>(from.y), static_cast<float>(to.y), perc)};
    }
}
