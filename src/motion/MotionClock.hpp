#pragma once

// THE TIME BETWEEN TWO CLOCK READS, one spelling for the whole plugin. Pure
// (<chrono> only), so the pure headers that take `now` as an argument
// (SearchLetters, SearchSession) and their host tests can use it too.
//
// Two clocks exist and this header is neither of them: it only measures.
//   real -- Config::realElapsedMs(since): wall-clock ms since `since`.
//   anim -- Config::animElapsedMs(since): the same, divided by anim_scale.
// A site that already holds `now` (one read per frame, shared by every value it
// draws) measures with msBetween(now, then) instead of reading the clock again;
// which of the two clocks a site runs on is the site's decision, and nothing
// here changes it.
//
// The representation is the caller's: float ms where the old read was
// duration<float, std::milli>, double where it was duration<double, std::milli>
// -- they round differently, so a site keeps the one it had.
// tests/unit/motion_math_test.cpp holds both to the expressions they replaced.

#include <chrono>

namespace Hyprdrive::Motion {

    // Milliseconds from `then` to `now`, in Rep.
    template <typename Rep = float, typename TP>
    inline Rep msBetween(const TP& now, const TP& then) {
        return std::chrono::duration<Rep, std::milli>(now - then).count();
    }

    // Seconds from `then` to `now`, in Rep.
    template <typename Rep = float, typename TP>
    inline Rep secondsBetween(const TP& now, const TP& then) {
        return std::chrono::duration<Rep>(now - then).count();
    }
}
