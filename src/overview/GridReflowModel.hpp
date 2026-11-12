#pragma once

// THE GRID REFLOW, the pure half: the per-window ease that carries a tile to
// its grid slot, and the filter that decides which window size the grid lays
// a window out with. No compositor, no config, no clock of its own (the
// caller hands in "now" and how to measure elapsed time), so
// tests/unit/grid_reflow_test.cpp runs it on the host. GridLayout.cpp is the
// live half.
//
// WHY IT IS SPLIT INTO A STEP AND A PEEK (two public reports, 2026-11: "opened
// apps just shake weirdly" in the overview; "windows ... almost vibrating,
// random twitchy motion where they move to the side and get slightly bigger
// before going back"). A reporter's video, frame by frame: with the pointer
// still, one tile shrinks by 2-7 px for 1-5 frames and eases back over
// ~100-170 ms while its row neighbour slides 3-4 px, at irregular intervals.
// That is exactly what the old gridReflowBox produced when ONE box query saw
// the window ~15 px smaller: every query -- hit-testing, damage, commit
// handlers, tooltips, the drag -- could re-aim the window's one shared ease
// (from = where it is, to = the query's slot, start = now), the next frame
// re-aimed it back, and the house decel curve's steep start showed part of the
// jump on the very first frame: an instant jump, then an eased return.
//
// So the ease now has ONE owner. step() -- advance, seed, retarget -- is only
// ever run by the overview's own frame (GridLayout.hpp: SGridOwnerScope);
// every other caller gets peek(), which is const and answers "where is this
// tile drawn right now" without moving anything. A query can no longer start
// an ease, whatever the window's size happens to read at that instant.
//
// And the size the grid lays a window out with is filtered (SSizeTrack): a
// change of the animation's GOAL is taken by the frame only when it has moved
// by more than rounding noise AND stayed moved for SIZE_SETTLE_MS; from then
// on the grid follows the animated value to its end, as it always did. A
// client that resizes itself and back within a frame or two, a configure in
// flight and the animation out and back: none of them reach the grid.

#include "MotionMath.hpp"

#include <cmath>
#include <cstdint>

namespace Hyprdrive::GridReflow {

    using Hyprutils::Math::CBox;
    using Hyprutils::Math::Vector2D;

    // A slot that moved by no more than this (logical px, on any edge) is the
    // same slot. Unchanged from the old Geometry::boxesDiffer: the slots are
    // computed in float from the filtered sizes below, so they carry no
    // device-pixel rounding of their own; the noise lives in the INPUT, and
    // that is where it is filtered.
    inline constexpr double SLOT_NOISE_PX = 0.5;

    // A window size that changed by no more than this (logical px, per axis)
    // is the same size. One device pixel is 1/scale logical px, so this covers
    // the logical<->device rounding of a client's size at every scale >= 1
    // (1.25: 0.8 px, 1.6: 0.625 px). A tile laid out 1 px off its window's
    // aspect is invisible -- the texture is scaled into the slot either way.
    inline constexpr double SIZE_NOISE_PX = 1.0;

    // How long a new window size must persist before the grid takes it. Long
    // enough that a client resizing itself and back (the size flicker the
    // reports describe lasted under a frame) never reaches the layout; short
    // enough to read as "the tiles follow the window". A genuine resize
    // reflows this much later, with the reflow's own duration and curve.
    inline constexpr double SIZE_SETTLE_MS = 50.0;

    inline bool slotsDiffer(const CBox& a, const CBox& b) {
        return std::abs(a.x - b.x) > SLOT_NOISE_PX || std::abs(a.y - b.y) > SLOT_NOISE_PX || std::abs(a.w - b.w) > SLOT_NOISE_PX || std::abs(a.h - b.h) > SLOT_NOISE_PX;
    }

    inline bool sizesDiffer(const Vector2D& a, const Vector2D& b) {
        return std::abs(a.x - b.x) > SIZE_NOISE_PX || std::abs(a.y - b.y) > SIZE_NOISE_PX;
    }

    // ------------------------------------------------------------------ ease

    template <class TP>
    struct SEase {
        CBox     from;
        CBox     to;
        TP       start{};
        uint64_t lastFrame    = 0;
        bool     promotedLast = false;
    };

    // One box question. `frame` is the asking monitor's overview frame count,
    // `durationMs` the reflow (or release) duration, `exiting` picks the curve.
    struct SAsk {
        CBox        slot;
        const CBox* enterFrom  = nullptr;
        bool        promoted   = false;
        bool        exiting    = false;
        uint64_t    frame      = 0;
        float       durationMs = 0.F;
    };

    enum class EStep : uint8_t {
        SEEDED,     // first sight (or back after an absence): drawn at its start
        RETARGETED, // the slot moved: re-aimed from where the tile is
        MOVING,     // still easing toward an unchanged slot
        SETTLED,    // at its slot
    };

    // Where the ease has the tile at `elapsedMs` since its start.
    template <class TP, class Ease>
    CBox easeAt(const SEase<TP>& e, float elapsedMs, float durationMs, bool exiting, const Ease& ease) {
        const float T = ease(elapsedMs / durationMs, exiting);
        return Hyprdrive::Motion::lerp(e.from, e.to, T);
    }

    // Would the owner reseed this entry? `known` is "the entry belongs to this
    // window" (a recycled address must not inherit a dead window's ease).
    // Unknown; or done and not drawn last frame (it left the grid and came
    // back); or it just crossed onto the active card (search promotion).
    template <class TP>
    bool needsSeed(const SEase<TP>& e, bool known, const SAsk& ask, float elapsedMs) {
        if (!known)
            return true;
        const bool INFLIGHT = elapsedMs < ask.durationMs;
        const bool UNDRAWN  = !INFLIGHT && ask.frame - e.lastFrame > 1;
        const bool JOINED   = ask.promoted && !e.promotedLast;
        return UNDRAWN || JOINED;
    }

    // THE OWNER'S STEP: the only function that moves an ease. `elapsed(tp)` is
    // the animation-clock time since `tp` (the caller's anim_scale applies),
    // `ease(t, exiting)` the reflow curve. The arithmetic and its order are the
    // old gridReflowBox's, line for line; what changed is who may call it.
    template <class TP, class Elapsed, class Ease>
    EStep step(SEase<TP>& e, bool known, const SAsk& ask, const TP& now, const Elapsed& elapsed, const Ease& ease, CBox& out) {
        if (needsSeed(e, known, ask, known ? elapsed(e.start) : 0.F)) {
            const CBox START = ask.enterFrom ? *ask.enterFrom : ask.slot;
            e                = SEase<TP>{START, ask.slot, now, ask.frame, ask.promoted};
            out              = START;
            return EStep::SEEDED;
        }

        e.lastFrame    = ask.frame;
        e.promotedLast = ask.promoted;

        const float ELAPSED = elapsed(e.start);
        const CBox  CURRENT = easeAt(e, ELAPSED, ask.durationMs, ask.exiting, ease);
        out                 = CURRENT;

        if (slotsDiffer(ask.slot, e.to)) {
            e.from  = CURRENT; // retarget from where the tile actually is
            e.to    = ask.slot;
            e.start = now;
            return EStep::RETARGETED;
        }

        return ease(ELAPSED / ask.durationMs, ask.exiting) < 1.F ? EStep::MOVING : EStep::SETTLED;
    }

    // THE READ everyone else gets: where the tile is drawn right now. Const,
    // so no caller can re-aim anything. A slot that differs from the ease's
    // target is NOT followed here -- the owner decides on its next frame
    // whether that change is real. `e` is null when there is no entry.
    template <class TP, class Elapsed, class Ease>
    CBox peek(const SEase<TP>* e, bool known, const SAsk& ask, const Elapsed& elapsed, const Ease& ease) {
        if (!e || needsSeed(*e, known, ask, known ? elapsed(e->start) : 0.F))
            return ask.enterFrom ? *ask.enterFrom : ask.slot; // what the owner will seed
        return easeAt(*e, elapsed(e->start), ask.durationMs, ask.exiting, ease);
    }

    // ------------------------------------------------------------------ size

    // The size the grid lays one window out with.
    //
    // A REAL CHANGE KEEPS ITS OLD MOTION (owner, 2026-11-11: the timings are
    // intentional). Before the fix the grid read the size animation's current
    // value, so a resize moved the tiles for the whole of Hyprland's size
    // animation and then eased -- 765 ms on the rig for a -300x-200 resize --
    // and a window closing in the overview let the survivors grow with their
    // own retile. That motion is kept: once a change is ADOPTED the grid
    // FOLLOWS the animated value for the rest of that animation, exactly as it
    // used to, and holds the goal when it ends. What it no longer follows is a
    // change that never got adopted: the goal flicked away and back within
    // SIZE_SETTLE_MS, and the animation's way out and back again. Following
    // from the first sight of a new goal would follow exactly that -- the
    // twitch -- so a real change starts moving the tiles SIZE_SETTLE_MS late
    // and ends on the same frame as before (the animation's end, then the ease).
    template <class TP>
    struct SSizeTrack {
        Vector2D adopted;    // the goal the grid has taken
        Vector2D pending;    // a goal it has seen, not taken yet
        Vector2D input;      // what the grid lays the window out with
        TP       since{};
        bool     hasPending = false;
        bool     following  = false; // input = the animated value until it reaches the goal
        uint64_t lastFrame  = 0;
    };

    enum class ESize : uint8_t {
        FIRST,     // first sight (or back after an absence): taken as is
        STEADY,    // unchanged (within SIZE_NOISE_PX)
        PENDING,   // changed, not for long enough yet: the grid keeps the old size
        ADOPTED,   // changed and stayed changed: the grid takes it now
        FOLLOWING, // an adopted change still animating: the grid follows the value
        REJECTED,  // changed and came back before SIZE_SETTLE_MS: never reached the grid
    };

    // The owner's per-frame look at one window's size: its animation's `goal`
    // and current `value`. `realElapsedMs(tp)` is wall-clock ms since `tp`: a
    // filter on the input is not an animation, so anim_scale does not stretch
    // it. `immediate` takes a real change at once (the overview's own resize
    // handle: the user is the one changing it, every frame).
    template <class TP, class Elapsed>
    ESize observe(SSizeTrack<TP>& t, bool known, const Vector2D& goal, const Vector2D& value, uint64_t frame, const TP& now, const Elapsed& realElapsedMs,
                  bool immediate = false) {
        // Taking a change: follow its animation if there is one left.
        const auto TAKE = [&](ESize result) {
            t.adopted    = goal;
            t.hasPending = false;
            t.following  = value != goal;
            t.input      = t.following ? value : goal;
            return result;
        };

        if (!known || frame - t.lastFrame > 1) {
            // First sight: what the grid always read, the value (an overview
            // opened over a window mid-animation follows it, as before).
            t           = SSizeTrack<TP>{};
            t.lastFrame = frame;
            return TAKE(ESize::FIRST);
        }
        t.lastFrame = frame;

        if (t.following) {
            // Exactly the old input until the animation lands (Hyprland warps
            // the value onto the goal on its last tick), a goal moved again
            // mid-way included.
            return TAKE(ESize::FOLLOWING);
        }

        if (!sizesDiffer(goal, t.adopted)) {
            const bool HAD = t.hasPending;
            t.hasPending   = false;
            t.input        = t.adopted; // NOT the value: it may be animating back from a flicker
            return HAD ? ESize::REJECTED : ESize::STEADY;
        }

        if (immediate)
            return TAKE(ESize::ADOPTED);

        t.input = t.adopted;
        if (!t.hasPending) {
            t.hasPending = true;
            t.pending    = goal;
            t.since      = now;
            return ESize::PENDING;
        }

        t.pending = goal;
        if (realElapsedMs(t.since) >= SIZE_SETTLE_MS)
            return TAKE(ESize::ADOPTED);
        return ESize::PENDING;
    }

} // namespace Hyprdrive::GridReflow
