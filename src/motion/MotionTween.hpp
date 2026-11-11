#pragma once

// A VALUE THAT TURNS FROM WHERE IT IS: from, to and the moment it set off. The
// sites that ease a progress toward a target and, when the target changes
// mid-way, start the new leg from the value as drawn (not from an end) used to
// hold the three fields loose and write the lerp and the retarget each time.
// Pure (no clock read, no curve), so tests/unit/motion_math_test.cpp replays
// recorded retarget sequences through it against each site's old code.
//
// What it deliberately does NOT own, because the sites differ and must keep
// what they had: the clock (real, anim-scaled, or a `now` the caller holds),
// the duration and its clamp, the curve and which direction picks it, the end
// snap, and when to ask for frames. A site computes the eased progress its way
// and hands it to at(). Holds progress, never colours.

namespace Hyprdrive::Motion {

    template <typename TP>
    struct STween {
        float from  = 0.F;
        float to    = 0.F;
        TP    start = {};

        // The value at eased progress `e`, from + (to - from) * e.
        float at(float e) const {
            return from + ((to - from) * e);
        }

        // Heading up (an entrance, by the house rule) rather than down.
        bool rising() const {
            return to > from;
        }

        // A new leg toward `target`, setting off at `now` from `current` -- the
        // value as drawn at the turn, which the caller reads BEFORE `now` when
        // the two come from the clock, as every site did.
        void retarget(float current, float target, const TP& now) {
            from  = current;
            to    = target;
            start = now;
        }
    };
}
