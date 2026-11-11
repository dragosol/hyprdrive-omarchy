#pragma once

// THE TIME BETWEEN TWO CLOCK READS, and THE ANIMATION CLOCK. The measuring half
// is pure (<chrono> only), so the pure headers that take `now` as an argument
// (SearchLetters, SearchSession) and their host tests can use it too.
//
// Two clocks:
//   real -- Clock::realMs(since): wall-clock ms since `since`. Input timeouts,
//           hold and long-press thresholds, hover delays before something
//           starts, debounce, polls, the profiler.
//   anim -- Clock::animMs(since): the same, run through the speed setting.
//           Every hand-made animation (owner, 2026-11-10: "speed setting
//           everywhere", "not exempt").
// A site that already holds `now` (one read per frame, shared by every value it
// draws) measures with msBetween(now, then) -- or animMsBetween(now, then) --
// instead of reading the clock again.
//
// The representation is the caller's: float ms where the old read was
// duration<float, std::milli>, double where it was duration<double, std::milli>
// -- they round differently, so a site keeps the one it had.
// tests/unit/motion_math_test.cpp holds both to the expressions they replaced.
//
// THE SPEED SETTING, ONE MEANING (plugin:hyprdrive:anim_scale; larger is
// slower). The effective scale is that key, clamped to >= 0, and 0 whenever
// Hyprland's animations:enabled is 0 (owner, 2026-11-10: "snap everything when
// animations are off"):
//   1   -- the shipped timings, bit for bit (x / 1.0f == x).
//   s>0 -- every animated millisecond is s real ones: elapsed / s.
//   0   -- SNAP: every animation is already at its end. Elapsed reads as
//          SNAP_MS (longer than any duration), and so does a frame step
//          (scaleDt): a progress that adds dt and clamps lands on its end. An
//          INTEGRATOR must test isSnapDt(dt) and put itself on its goal: fed
//          SNAP_MS in fixed substeps it would loop for ever, so every one
//          clamps its step after scaling or tests first.
// A stamp in the FUTURE is a delay that has not run out (TileChrome's linger):
// its negative elapsed is real time, never scaled and never snapped, so a
// delay keeps its length at every speed and an animation waits for it.

#include <chrono>
#include <cmath>

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

    // What an animation's elapsed time reads at scale 0: past every duration
    // in the plugin. The value Config::animElapsedMs always returned there.
    inline constexpr float SNAP_MS = 1e9F;

    // The effective scale from its two settings: anim_scale (clamped to >= 0)
    // and Hyprland's animations:enabled, which at 0 makes it 0.
    inline float effectiveScale(float animScale, bool animationsEnabled) {
        if (!animationsEnabled)
            return 0.F;
        return animScale > 0.F ? animScale : 0.F;
    }

    // Scale 0 (or NaN): every animation is at its end.
    inline bool snapsAt(float scale) {
        return !(scale > 0.F);
    }

    // Real elapsed ms -> animated elapsed ms at `scale` (the rules above).
    template <typename Rep>
    inline Rep scaleMs(Rep realMs, float scale) {
        if (realMs < Rep(0))
            return realMs;
        if (snapsAt(scale))
            return Rep(SNAP_MS);
        return realMs / Rep(scale);
    }

    // A real frame step (any unit) -> the animated step at `scale`; SNAP_MS
    // when the clock snaps (see above: an integrator tests isSnapDt).
    template <typename Rep>
    inline Rep scaleDt(Rep realDt, float scale) {
        if (snapsAt(scale))
            return Rep(SNAP_MS);
        return realDt / Rep(scale);
    }

    // The step a snapping clock hands out: put the value on its goal.
    template <typename Rep>
    inline bool isSnapDt(Rep dt) {
        return dt >= Rep(SNAP_MS);
    }

    // A duration in the clock's units -> the real time it takes at `scale`
    // (for a site that hands a length to somebody else's clock: a Hyprland
    // animated variable's speed, a timer). 0 when the clock snaps.
    template <typename Rep>
    inline Rep scaleDuration(Rep ms, float scale) {
        if (snapsAt(scale))
            return Rep(0);
        return ms * Rep(scale);
    }

    // A LOOP (a pulse, a sweep, a drift): its phase, 0..1, at real time
    // `realT` off a steady epoch, one period being `period` animated units. A loop has no end to
    // snap to, so a snapping clock HOLDS it at `restPhase` -- still, and asking
    // for no frames. A change of scale moves the phase (it is off the epoch).
    // In Rep, as the site computed it (a float loop stays float).
    template <typename Rep>
    inline Rep loopPhaseAt(Rep realT, Rep period, float scale, Rep restPhase) {
        if (snapsAt(scale))
            return restPhase;
        return std::fmod(realT / Rep(scale), period) / period;
    }

    // ---- the clock -----------------------------------------------------------
    namespace Clock {
        using TP = std::chrono::steady_clock::time_point;

        // Where the speed setting comes from. The plugin points it at the
        // live config (MotionClock.cpp, at load); a host test points it at its
        // own number; unset, the clock runs at 1. Kept a pointer so this
        // header stays pure and the pure headers that animate (SearchLetters,
        // ...) read the same clock as the plugin.
        inline float (*g_scaleSource)() = nullptr;

        // The effective speed setting (see the top of this file).
        inline float scale() {
            return g_scaleSource ? g_scaleSource() : 1.F;
        }
        // Animations run at all (scale() > 0). False = every one snaps.
        inline bool enabled() {
            return !snapsAt(scale());
        }

        // Wall-clock ms since `since`, and the same through the speed setting
        // (MotionClock.cpp: they read Hyprland's clock).
        float realMs(const TP& since);
        float animMs(const TP& since);

        // The same, for a site that holds `now`.
        template <typename Rep = float>
        inline Rep animMsBetween(const TP& now, const TP& then) {
            return scaleMs<Rep>(msBetween<Rep>(now, then), scale());
        }
        // Seconds, for a site that held secondsBetween.
        template <typename Rep = float>
        inline Rep animSecondsBetween(const TP& now, const TP& then) {
            return scaleMs<Rep>(secondsBetween<Rep>(now, then), scale());
        }
        // loopPhaseAt on the live scale, `realSeconds` off the steady epoch.
        template <typename Rep>
        inline Rep loopPhase(Rep realT, Rep period, Rep restPhase) {
            return loopPhaseAt<Rep>(realT, period, scale(), restPhase);
        }
        // A real step -> the animated step (SNAP_MS when snapping; see above).
        template <typename Rep>
        inline Rep animDt(Rep realDt) {
            return scaleDt<Rep>(realDt, scale());
        }
    }
}
