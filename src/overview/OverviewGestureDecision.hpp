#pragma once

// Whether a trackpad swipe bound to the overview OPENS it or CLOSES it, decided
// once in COverviewGesture::begin. Pure: no Hyprland headers, so
// tests/unit/overview_gesture_decision_test.cpp can try every direction pair.
// The enum mirrors Hyprland's eTrackpadGestureDirection value for value;
// OverviewGesture.cpp static_asserts each one, so a reordering upstream fails
// the build instead of flipping a gesture.

#include <cstdint>

namespace Hyprdrive::OverviewGestureDecision {

    enum class EDir : uint8_t {
        None = 0,
        Swipe,
        Left,
        Right,
        Up,
        Down,
        Vertical,
        Horizontal,
        Pinch,
        PinchOut,
        PinchIn,
    };

    // `bound` is the direction in the gesture binding, `taken` the one Hyprland
    // hands begin() (the way the fingers actually went). A binding to "down"
    // always closes. A "vertical" binding is both halves of an up/down pair, so
    // it closes when the fingers went down -- reading only the bound direction
    // made a vertical binding open on either direction and never close. Every
    // other binding opens (and an open overview under the fingers stays open).
    constexpr bool wantsClose(EDir bound, EDir taken) {
        return bound == EDir::Down || (bound == EDir::Vertical && taken == EDir::Down);
    }

    constexpr bool wantsOpen(EDir bound, EDir taken) {
        return !wantsClose(bound, taken);
    }
}
