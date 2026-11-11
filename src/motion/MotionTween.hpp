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

#include <algorithm>
#include <vector>

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

    // ONE OF MANY LIT, EACH TURNING FROM WHERE IT IS: the hover highlight of a
    // list (the search rows, the dock menu's rows and buttons). The key the
    // pointer is on heads to 1, every other key still showing heads to 0, and a
    // key that changes direction mid-way sets off from the level it has --
    // where the old two-slot cross-fade (`now`, `prev`, one shared clock)
    // restarted both on every change: back onto a row mid-fade popped it to 0
    // and the row it left to 1, and a third row still fading vanished.
    //
    // An uninterrupted move is that cross-fade exactly: the new key 0 -> 1 at
    // `0 + 1*e`, the old one 1 -> 0 at `1 + (-1)*e`, which are e and 1 - e to
    // the bit (and the same with rest and on swapped). Pure like STween: the caller supplies `eased(tween)`, the eased
    // progress on its own clock and curve, and `running(tween)` for frames.
    template <typename TP>
    struct SLitSet {
        static constexpr long NONE = -1;
        struct SEntry {
            long       key = NONE;
            STween<TP> fade;
        };
        std::vector<SEntry> entries;
        long                lit = NONE;
        // The level a key rests at (and every key not in `entries` reads), and
        // the one the lit key heads to. 0 and 1 for a highlight; a set whose
        // lit key HIDES something (the desktop strip's resting numbers under a
        // badge) is 1 and 0.
        float rest = 0.F;
        float on   = 1.F;

        template <typename FEased>
        float level(long key, FEased eased) const {
            for (const auto& E : entries)
                if (E.key == key)
                    return E.fade.at(eased(E.fade));
            return rest;
        }

        // The pointer is now on `key` (NONE: on nothing). No-op if unchanged.
        template <typename FEased>
        void moveTo(long key, const TP& now, FEased eased) {
            if (key == lit)
                return;
            // Keys that are back at rest are dropped (exactly: the end of
            // `from + (rest - from) * 1` is rest).
            std::erase_if(entries, [&](const SEntry& e) { return e.fade.to == rest && e.fade.at(eased(e.fade)) == rest; });
            for (auto& e : entries)
                if (e.key == lit && e.fade.to != rest)
                    e.fade.retarget(e.fade.at(eased(e.fade)), rest, now);
            lit = key;
            if (key == NONE)
                return;
            for (auto& e : entries)
                if (e.key == key) {
                    e.fade.retarget(e.fade.at(eased(e.fade)), on, now);
                    return;
                }
            entries.push_back(SEntry{.key = key, .fade = STween<TP>{.from = rest, .to = on, .start = now}});
        }

        template <typename FRunning>
        bool anyRunning(FRunning running) const {
            return std::ranges::any_of(entries, [&](const SEntry& e) { return running(e.fade); });
        }
    };
}
