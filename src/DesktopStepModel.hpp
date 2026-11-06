#pragma once

// SUPER+SCROLL THROUGH DESKTOPS, AND DESKTOP-CHANGE ANIMATIONS BY CAUSE -- the
// pure half (user, 2026-10-15). No Hyprland headers, so
// scripts/desktop_step_test.cpp runs it on the host. DesktopSwitch.cpp is the
// live half.

#include <algorithm>
#include <cstdint>
#include <cstdlib>

namespace ScrollOverview::DesktopStep {

    // The wheel walks desktops by NUMBER, so the empty ones between are visited
    // too (Hyprland's `r+1`, not Omarchy's `e+1`), and it stops at the ends
    // instead of wrapping: 1 at the bottom, at the top the larger of 10 and the
    // highest desktop that exists on this monitor -- Omarchy's Super+1..0 range,
    // or further when the user has gone past it.
    inline constexpr int FIRST       = 1;
    inline constexpr int DEFAULT_TOP = 10;

    constexpr int top(int highestOnMonitor) {
        return std::max(DEFAULT_TOP, highestOnMonitor);
    }

    // One step from `current` in `dir` (+1 next, -1 previous). `elsewhere(id)`
    // says a desktop lives on ANOTHER monitor: stepping onto it would pull the
    // focus over there, so it is skipped as if it were not on this monitor's
    // number line at all. Returns `current` when there is nowhere to go. A
    // current desktop outside the range (a named desktop, a negative id) steps
    // to the nearest end.
    template <typename FElsewhere>
    constexpr int target(int current, int dir, int highestOnMonitor, FElsewhere&& elsewhere) {
        const int TOP = top(highestOnMonitor);
        if (dir == 0)
            return current;
        int id = current < FIRST ? (dir > 0 ? FIRST - 1 : FIRST) : current > TOP ? (dir < 0 ? TOP + 1 : TOP) : current;
        for (;;) {
            id += dir > 0 ? 1 : -1;
            if (id < FIRST || id > TOP)
                return current;
            if (!elsewhere(id))
                return id;
        }
    }

    // ONE STEP PER NOTCH. A wheel reports in 1/120ths of a notch (value120); a
    // classic wheel sends 120 per click, a high-resolution one a stream of
    // fractions, and a free-spinning one far more. Fractions add up here and
    // only a whole notch counts, so a hi-res wheel or a nudge does not fly
    // through five desktops. A change of direction, or a pause longer than
    // IDLE_MS, starts over: a half notch left behind a second ago is not the
    // first half of this one.
    struct SNotchAccumulator {
        static constexpr int      NOTCH   = 120;
        static constexpr uint32_t IDLE_MS = 1000;

        int      acc    = 0;
        uint32_t lastMs = 0;
        bool     seen   = false;

        // -> whole notches completed by this event, signed (0 = a fraction).
        constexpr int feed(int value120, uint32_t timeMs) {
            if (value120 == 0)
                return 0;
            if (seen && (timeMs < lastMs || timeMs - lastMs > IDLE_MS))
                acc = 0;
            seen   = true;
            lastMs = timeMs;
            if ((acc > 0 && value120 < 0) || (acc < 0 && value120 > 0))
                acc = 0;
            acc += value120;
            const int N = acc / NOTCH; // toward zero
            acc -= N * NOTCH;
            return N;
        }

        constexpr void reset() {
            acc  = 0;
            seen = false;
        }
    };

    // WHY THE DESKTOP CHANGED, as far as an animation switch cares.
    enum class ECause : uint8_t {
        Other,    // a swipe, the overview, hyprctl dispatch workspace, a script
        Keyboard, // a key or scroll BIND (or hyprdrive's own desktop dispatcher)
        Focus,    // focus moved to a window on another desktop
    };

    // A bind outranks focus: Super+1 for the dock's first app is a keypress
    // even though what it does is focus a window.
    constexpr ECause classify(bool ownDispatcher, bool inKeybind, bool inFocus) {
        if (ownDispatcher || inKeybind)
            return ECause::Keyboard;
        if (inFocus)
            return ECause::Focus;
        return ECause::Other;
    }

    // Whether this change must land in one frame. Only a cause whose switch is
    // OFF snaps; Other is never touched.
    constexpr bool snaps(ECause cause, bool animateKeyboard, bool animateFocus) {
        switch (cause) {
            case ECause::Keyboard: return !animateKeyboard;
            case ECause::Focus: return !animateFocus;
            default: return false;
        }
    }
}
