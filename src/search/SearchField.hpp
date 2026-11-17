#pragma once

// The pill's metrics and the live field's springs. Pure: no Hyprland state.
// Cut from SearchSession.hpp by pure code motion.

#include "MotionClock.hpp"

#include <hyprland/src/helpers/time/Time.hpp>

#include <algorithm>
#include <cmath>
#include <optional>

// X10: the search row above the strip. ponytail: constant 64 logical px
// (48px pill + 8px margins); make it a config knob if anyone ever asks.
inline constexpr float OVERVIEW_SEARCH_ROWHL = 64.F, OVERVIEW_SEARCH_DOCKPADL = 8.F;

// X2. The pill's own metrics, from the deprecated Quickshell overview's
// SearchBar.qml (ii/modules/ii/overview/): a 40px row + 4px margins = 48 high,
// 15px type, 22px glyph, 210px collapsed / 360px expanded. They are LOGICAL px:
// the draw pass measures the card transform off the landed box and scales every
// one of them by it, so the pill's contents follow the transform for free.
//
// The row height above and these belong together -- 4 + 40 + 4 is where the 48
// comes from, and 8 + 48 + 8 is where the 64 comes from -- which is why they
// sit here rather than with the result-card family in OverviewInternal.hpp.
inline constexpr float OVERVIEW_PILL_HL       = 48.F;  // the pill itself
inline constexpr float OVERVIEW_PILL_PADL     = 4.F;   // its leading margin, and the badge's inset
inline constexpr float OVERVIEW_PILL_BADGEL   = 40.F;  // the accent badge carrying the search glyph
inline constexpr float OVERVIEW_PILL_GAPL     = 12.F;  // badge -> text field
inline constexpr float OVERVIEW_PILL_TAILL    = 20.F;  // text field -> trailing edge
inline constexpr float OVERVIEW_PILL_FIELDL   = 210.F; // the field at rest
inline constexpr float OVERVIEW_PILL_FIELDQL  = 360.F; // ...and once a query is typed
inline constexpr float OVERVIEW_PILL_GLYPHPX  = 22.F;  // FA magnifying glass (Nerd Font U+F002)
inline constexpr float OVERVIEW_PILL_TEXTPX   = 15.F;  // the query / placeholder

// THE LIVE FIELD (user, 2026-09-12): the field grows with the query the way the
// PondFluid lock screen's password pill does, until the WHOLE pill reaches
// MAXFRAC of the monitor; past that the text scrolls so the end stays in view,
// and the left edge fades it out.
inline constexpr float OVERVIEW_PILL_MAXFRAC  = 0.75F; // the pill's cap, as a share of monitor width
inline constexpr float OVERVIEW_PILL_ENDROOML = 16.F;  // breathing room kept after the last glyph
inline constexpr float OVERVIEW_PILL_FADEL    = 56.F;  // how far in from the left edge scrolled text fades

// The full width the pill lays out to. Named because BOTH the layout and the
// scale it measures back out of the landed box divide by it, and a mismatch
// there silently rescales everything inside the pill.
inline constexpr float OVERVIEW_PILL_WL(float fieldWL) {
    return OVERVIEW_PILL_PADL + OVERVIEW_PILL_BADGEL + OVERVIEW_PILL_GAPL + fieldWL + OVERVIEW_PILL_TAILL;
}

inline float overviewPillTargetWL(bool queried) {
    return OVERVIEW_PILL_PADL + OVERVIEW_PILL_BADGEL + OVERVIEW_PILL_GAPL + (queried ? OVERVIEW_PILL_FIELDQL : OVERVIEW_PILL_FIELDL) + OVERVIEW_PILL_TAILL;
}

// Qt's SpringAnimation, ported rather than approximated, so the field moves
// exactly like the PondFluid pill it copies: fixed 16ms steps, velocity grows by
// (spring * distance - damping * velocity) / mass, and the value snaps home once
// both speed and distance are under epsilon. Returns whether it is still moving.
// `dtMs` is the time this frame actually covers. Qt's own step is written for a
// fixed 16ms tick -- velocity gains (spring * distance - damping * velocity) /
// mass per tick and the value advances by velocity * 0.016 -- so both halves are
// scaled by how many ticks this frame is worth. At exactly 16ms the arithmetic
// is unchanged; at 16.67ms (60Hz), 10ms (100Hz) or a late frame it is the same
// curve sampled where the frame is, which is what "adapt to the monitor's
// refresh rate" means for a spring (user, 2026-09-19).
inline bool qtSpringStep(float& value, float& velocity, float target, float spring, float damping, float mass, float epsilon, float dtMs) {
    // FRAME-RATE INDEPENDENCE (user, 2026-09-19: the search animations are
    // "still choppy"). This was ONE explicit Euler step whose size WAS the
    // frame time -- clamp(dt,1,64)/16 -- so a 16ms frame stepped 1.0, a 120Hz
    // frame 0.5, and a late 64ms frame took a SINGLE step of 4.0. Explicit
    // Euler error grows with the square of the step, so the one frame that was
    // already late is also the frame the spring overshot and snapped back:
    // a hitch got amplified into a visible lurch instead of being absorbed.
    //
    // Substepping at a fixed 4ms makes the motion identical at any frame rate
    // and through a stall. The per-substep formula is unchanged, so the tuned
    // PondFluid constants keep their meaning at the 16ms frame they were
    // tuned on.
    constexpr float STEPMS = 4.F;

    // A snapping animation clock (MotionClock.hpp): on the goal, at rest.
    if (Hyprdrive::Motion::isSnapDt(dtMs)) {
        value    = target;
        velocity = 0.F;
        return false;
    }

    for (float remaining = std::clamp(dtMs, 0.F, 64.F); remaining > 0.F;) {
        const float H     = std::min(STEPMS, remaining);
        const float TICKS = H / 16.F;
        remaining -= H;

        velocity += ((spring * (target - value) - damping * velocity) / mass) * TICKS;
        value += velocity * 0.016F * TICKS;
    }

    if (std::abs(velocity) < epsilon && std::abs(target - value) < epsilon) {
        value    = target;
        velocity = 0.F;
        return false;
    }

    return true;
}

// The field's animated width and scroll. Lives on the SESSION, not on the
// layout: layoutSearch() builds a fresh layout every frame, so state kept there
// would be wiped before it could spring anywhere.
struct SSearchFieldMotion {
    // PondFluid.qml: widthSpring / widthDamping, mass 1.1, epsilon 0.4 -- and
    // slideSpring / slideDamping for the text sliding under the cap.
    static constexpr float WIDTH_SPRING = 4.0F, WIDTH_DAMPING = 0.44F, WIDTH_MASS = 1.1F, WIDTH_EPSILON = 0.4F;
    static constexpr float LAND_TAU_MS = 70.F;
    static constexpr float SLIDE_SPRING = 4.6F, SLIDE_DAMPING = 0.30F, SLIDE_MASS = 1.0F, SLIDE_EPSILON = 0.4F;

    float widthL   = OVERVIEW_PILL_FIELDL; // the field, logical px
    float widthV   = 0.F;
    float scrollL  = 0.F;                  // how far the text is scrolled left, logical px
    float scrollV  = 0.F;
    float contentL = 0.F;                  // the text's width as last DRAWN, logical px

    // What this frame draws. Kept as its own pair so every reader takes the
    // same sample, and so the raw spring state stays the simulation's business.
    float                          drawWidthL = OVERVIEW_PILL_FIELDL, drawScrollL = 0.F;

    std::optional<Time::steady_tp> last;
    // The draw pass is the only place the text is measured, one frame behind
    // stepTo: the first frame of an overview steps with nothing measured yet,
    // so it lands on the bare rest width and the second frame -- the first with
    // the placeholder measured -- sprang out past its real width and back (user,
    // 2026-11-06: "remove the overshoot of the search bar when overview opens").
    // The field LANDS until that first measurement has been taken.
    bool                           measured = false, landedMeasured = false;
    // What the last stepTo answered, for the overview's idle gate: these two
    // springs are wall-clock, so nothing the compositor animates reports them.
    bool                           moving = false;

    // Returns whether either spring is still moving, so the caller keeps frames
    // coming until both have settled.
    // `land`: take the target as it is, without springing to it (the overview
    // opening or closing around a pill at rest -- see renderSearch).
    bool stepTo(float wantWidthL, float wantScrollL, const Time::steady_tp& now, bool land = false) {
        // The first frame of an overview lands at rest rather than springing in
        // from wherever the struct's defaults happen to be.
        if (!last || !landedMeasured) {
            landedMeasured = measured;
            last        = now;
            widthL      = wantWidthL;
            scrollL     = wantScrollL;
            widthV      = 0.F;
            scrollV     = 0.F;
            drawWidthL  = widthL;
            drawScrollL = scrollL;
            moving      = false;
            return false;
        }

        // The animation clock's step: the speed setting stretches both springs
        // and the landing; a snapping clock's step puts them on their goals
        // (qtSpringStep, and exp(-SNAP_MS / tau) = 0 for the landing).
        const float DTMS = Hyprdrive::Motion::Clock::animDt(Hyprdrive::Motion::msBetween(now, *last));
        last             = now;

        // Landing: a critically damped glide (no overshoot by construction) that
        // smooths the zoom's whole-px font steps instead of jumping with them.
        if (land) {
            const float K = 1.F - std::exp(-DTMS / LAND_TAU_MS);
            widthL += (wantWidthL - widthL) * K;
            scrollL += (wantScrollL - scrollL) * K;
            if (std::abs(wantWidthL - widthL) < WIDTH_EPSILON)
                widthL = wantWidthL;
            if (std::abs(wantScrollL - scrollL) < SLIDE_EPSILON)
                scrollL = wantScrollL;
            widthV = scrollV = 0.F;
            drawWidthL       = widthL;
            drawScrollL      = scrollL;
            moving           = widthL != wantWidthL || scrollL != wantScrollL;
            return moving;
        }

        qtSpringStep(widthL, widthV, wantWidthL, WIDTH_SPRING, WIDTH_DAMPING, WIDTH_MASS, WIDTH_EPSILON, DTMS);
        qtSpringStep(scrollL, scrollV, wantScrollL, SLIDE_SPRING, SLIDE_DAMPING, SLIDE_MASS, SLIDE_EPSILON, DTMS);

        drawWidthL  = widthL;
        drawScrollL = scrollL;

        moving = widthV != 0.F || scrollV != 0.F || widthL != wantWidthL || scrollL != wantScrollL;
        return moving;
    }

    bool busy() const {
        return moving;
    }
};
