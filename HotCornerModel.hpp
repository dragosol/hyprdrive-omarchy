#pragma once

// HOT CORNER, the pure half (user, 2026-10-13, asked for on r/omarchy: "have the
// menu pop out if you hover on a corner ... I think GNOME does something
// similar"). Which screen corner is armed, whether a point is in it, and where to
// probe for a neighbouring monitor -- no Hyprland headers, so
// scripts/hot_corner_test.cpp runs it on the host. HotCorner.cpp is the live half.

#include <array>
#include <string_view>

namespace ScrollOverview::HotCorner {

    enum class ECorner { TopLeft, TopRight, BottomLeft, BottomRight };

    // hot_corner:corner. Anything unknown is GNOME's corner, the top left.
    constexpr ECorner cornerFromName(std::string_view name) {
        if (name == "top-right")
            return ECorner::TopRight;
        if (name == "bottom-left")
            return ECorner::BottomLeft;
        if (name == "bottom-right")
            return ECorner::BottomRight;
        return ECorner::TopLeft;
    }

    struct SRect {
        double x = 0, y = 0, w = 0, h = 0;
    };

    // The pointer is clamped to the output, so "in the corner" is a few logical
    // px from both edges at once, never past them.
    inline constexpr double EDGE_PX = 2.0;

    constexpr bool inCorner(double px, double py, const SRect& box, ECorner corner, double edge = EDGE_PX) {
        if (box.w <= 0 || box.h <= 0 || px < box.x || py < box.y || px > box.x + box.w || py > box.y + box.h)
            return false;
        const bool LEFT   = px <= box.x + edge;
        const bool RIGHT  = px >= box.x + box.w - 1 - edge;
        const bool TOP    = py <= box.y + edge;
        const bool BOTTOM = py >= box.y + box.h - 1 - edge;
        switch (corner) {
            case ECorner::TopLeft: return TOP && LEFT;
            case ECorner::TopRight: return TOP && RIGHT;
            case ECorner::BottomLeft: return BOTTOM && LEFT;
            case ECorner::BottomRight: return BOTTOM && RIGHT;
        }
        return false;
    }

    struct SPoint {
        double x = 0, y = 0;
    };

    // Just beyond the corner: sideways, up/down, and diagonally. A corner only
    // counts when no other monitor contains any of these -- between two
    // monitors the pointer crosses over, it never rests in a "corner" there.
    constexpr std::array<SPoint, 3> probes(const SRect& box, ECorner corner, double gap = 4.0) {
        const bool   LEFT = corner == ECorner::TopLeft || corner == ECorner::BottomLeft;
        const bool   TOP  = corner == ECorner::TopLeft || corner == ECorner::TopRight;
        const double CX   = LEFT ? box.x : box.x + box.w - 1;
        const double CY   = TOP ? box.y : box.y + box.h - 1;
        const double OX   = LEFT ? box.x - gap : box.x + box.w + gap;
        const double OY   = TOP ? box.y - gap : box.y + box.h + gap;
        return {SPoint{OX, CY}, SPoint{CX, OY}, SPoint{OX, OY}};
    }
}
