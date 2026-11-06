#pragma once

#include <cstdint>
#include <hyprland/src/helpers/math/Math.hpp>

// Feed Hyprland's trackpad gesture manager a swipe of our own (GestureDrive.cpp).
namespace ScrollOverview::GestureDrive {
    void begin(uint32_t timeMs, uint32_t fingers);
    // ignoreMods: pick the gesture even with a modifier held (a Super+drag).
    void update(uint32_t timeMs, uint32_t fingers, const Vector2D& delta, bool ignoreMods);
    void end(uint32_t timeMs, bool cancelled);
    bool active();
}
