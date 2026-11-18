#pragma once

// A TRIP NEVER SHOWS AN UNDRAWN PICTURE (user, 2026-11-11: "battle.net
// struggling exiting in and out of fullscreen - animation is buggy"; the
// recording: the launcher's box white or black for a few frames as the toggle
// starts and again as it lands, and the launcher small on a bare desktop).
//
// A trip sends its window one size each way (TripSizes.hpp), but a client
// that redraws late (Wine, CEF) shows its OLD buffer at the new size for a
// while -- too small in a fullscreen box, too big in its tile -- and then,
// at the new size, a blank first frame (CEF: white) before its real one. So
// from the moment the trip changes its size, the window is drawn from a
// snapshot of its last good frame, fitted into the travelling box
// (TripFrame.cpp), until the client has presented a real frame at the new
// size. This is the pure decision: when is the live window good again?
//
// Decided by what the client COMMITS, not by what kind of window it is:
//   - it was never sent another size (a trip that kept its size): at once,
//     once that is clear (UNCHANGED_MS) -- and only clear when no size is
//     still waiting to be sent (ConfigureHold). "Sent" is what the client
//     was TOLD (m_pendingReportedSize), not Hyprland's "reported" size: for
//     an X11 window that one only follows on the client's next commit, so a
//     client that redraws late (no blank frame first) still had its old size
//     100 ms in, and the hold let go of the old buffer stretched into the
//     travelling box (10 of 11 holds, slow_redraw without --flash);
//   - its SECOND FRAME at the size it was sent: the first may be the blank
//     one (the X server's background, CEF's white), the second is drawn. A
//     frame is a commit that repaints (nearly) all of the window, and the
//     second counts only MIN_GAP_MS after the first: XWayland re-commits the
//     same blank picture, fully damaged, when something else happens to the
//     window (measured on the rig: 60 ms after the white frame, as the
//     launcher's helper window mapped and the desktop re-arranged; counting
//     it released the hold on the white frame). Nor does a commit that
//     came after Hyprland only MOVED the window since its last commit (its
//     size the same): XWayland re-commits the picture it has when an X11
//     window is moved -- 120 ms after the white frame when the helper mapped
//     later, past MIN_GAP_MS, and the hold let go on white again (fs_trip_x11
//     check 4, 1 frame in 170). A client's real frame comes
//     its redraw time after the blank one (CEF: ~150 ms); one that draws
//     faster than MIN_GAP_MS waits for its next frame or SETTLE_MS. A heuristic
//     on purpose: commits do not say what is in them, and reading the buffer
//     back each frame costs a GPU stall.
//     A NATIVE WAYLAND window has no X server painting a background for it,
//     and nothing re-commits its picture behind its back: its frames are its
//     own. So there the second frame counts whenever it comes (only not in
//     the same commit as the first: a synced subsurface commits with its
//     parent, SAME_COMMIT_MS), and a frame may come from any surface of the
//     window, a subsurface too, at the new size and repainting all of that
//     surface -- Firefox draws its page in a subsurface and commits its main
//     surface once per resize, so the main surface alone said "one frame,
//     then still" and the picture stood frozen for SETTLE_MS (user,
//     2026-11-18: "if a video is playing it should still play");
//   - a client that commits once and then stays still (a plain X11 client
//     redrawing once): SETTLE_MS after that one commit -- longer than the
//     ~150 ms a CEF launcher takes from its white frame to its real one;
//   - a client that never draws at all still appears: TIMEOUT_MS after its
//     box stopped changing size (from the snapshot when it never did), and
//     never later than CAP_MS after the snapshot.
// All in real time: it is the client's pace, not an animation (the speed
// setting does not slow the client down).
//
// THE PICTURE STRETCHES ALL THE WAY (user, 2026-11-18: "Zen snaps jankily
// partway through the transition ... make the snapshot LIVE, like if a video
// is playing it should still play, then crossfade into the new aspect ratio
// and size"). The rule above says when the live picture is GOOD; it is not
// when the box is done. Released at the client's pace (Firefox: 275 ms) while
// the trip's box still had a fifth of its way to go (it lands ~1.15 s in),
// the live window was drawn by Hyprland, which crops a buffer bigger than its
// box and smears one smaller -- the snap. So while a trip's box changes size,
// the held window is drawn stretched into it, in three phases:
//   A. its OLD frames, live (liveNow): the client has not committed another
//      size yet, and its picture is known good;
//   B. from its first commit at another size until the rule above releases
//      it: the snapshot, re-taken from its last old frame (the only frozen
//      moment: what keeps the blank-frame protection);
//   C. released: the live window fades in over the snapshot (fadeMs), still
//      stretched into the box until it lands (over).
// CAP_MS after the snapshot it is released whatever it did, live at once
// (no fade), and still stretched until its box lands -- at a slowed speed
// setting the box can travel longer than that.
// A hold whose box never changed size (a trip that kept its size, a slide
// that only moves it) ends the moment the rule releases it, as before.
namespace Hyprdrive::TripFrameRule {
    constexpr double UNCHANGED_MS = 100.0;
    constexpr double SETTLE_MS    = 250.0;
    constexpr double TIMEOUT_MS   = 400.0;
    constexpr double CAP_MS       = 2000.0; // never frozen longer than this: live from then on, no fade
    constexpr double LAST_MS      = 4000.0; // nor drawn by the hold at all (a box that never lands)
    constexpr double FULL_REPAINT = 0.5; // of the window's area: a frame, not a touch-up
    constexpr double MIN_GAP_MS   = 100.0; // X11 only (above)
    constexpr double SAME_COMMIT_MS = 1.0; // Wayland: another commit, not the same one seen twice
    constexpr double FADE_MIN_MS  = 150.0; // the fade in, animation ms (the speed setting
    constexpr double FADE_MAX_MS  = 250.0; // stretches it, as the trip's own motion)

    struct SState {
        bool   resized = false; // it was sent a size other than the snapshot's
        int    drawn   = 0;     // commits at that size since
        double firstMs = -1.0;  // when the first of them came (ms since the snapshot)
    };

    // The client was sent a size: `differs` from the snapshot's. A new size
    // starts the count again (a quick toggle back mid-hold).
    inline void sent(SState& s, bool differs, bool changedSinceLast) {
        if (!differs) {
            s = {};
            return;
        }
        if (!s.resized || changedSinceLast)
            s = {.resized = true};
    }
    // It committed a FRAME (a full repaint, FULL_REPAINT), `atSent`: at the
    // size it was sent; `movedOnly`: since its last commit it was moved, not
    // resized (the commit is the X server's, not the client's); `wayland`:
    // a native Wayland window (no MIN_GAP_MS, above).
    inline void committed(SState& s, double ms, bool atSent, bool movedOnly = false, bool wayland = false) {
        if (!s.resized || !atSent || movedOnly)
            return;
        if (s.drawn >= 1 && ms - s.firstMs < (wayland ? SAME_COMMIT_MS : MIN_GAP_MS))
            return; // the same blank picture again, re-committed (or, Wayland, the same commit)
        if (s.drawn++ == 0)
            s.firstMs = ms;
    }

    enum class ERelease { Hold, Drawn, Settled, Unchanged, Timeout };
    inline const char* why(ERelease r) {
        switch (r) {
            case ERelease::Hold: return "held";
            case ERelease::Drawn: return "its second frame at the new size";
            case ERelease::Settled: return "one frame at the new size, then still";
            case ERelease::Unchanged: return "its size did not change";
            case ERelease::Timeout: return "it did not draw in time";
        }
        return "?";
    }
    // `ms`: real time since the snapshot; `waiting`: a size is still to be
    // sent to it (the trip's own, held back by its gate); `landedMs`: when
    // its box last stopped changing size (ms since the snapshot), < 0 while
    // it still changes -- 0, a box that never changed: the timeout counts
    // from the snapshot, as it did.
    inline ERelease decide(const SState& s, double ms, bool waiting = false, double landedMs = 0.0) {
        if (s.resized && s.drawn >= 2)
            return ERelease::Drawn;
        if (s.resized && s.drawn == 1 && ms - s.firstMs >= SETTLE_MS)
            return ERelease::Settled;
        if (ms >= CAP_MS || (landedMs >= 0 && ms - landedMs >= TIMEOUT_MS))
            return ERelease::Timeout;
        if (!s.resized && !waiting && ms >= UNCHANGED_MS)
            return ERelease::Unchanged;
        return ERelease::Hold;
    }

    // Phase A: its live picture is shown before the rule releases it --
    // its size changes (it was sent another, or its box changes), it has not
    // committed at another size than its old one (`fresh`), and its picture
    // is a drawn one (`good`: not a hold taken again before it had drawn).
    inline bool liveNow(const SState& s, bool boxResized, bool fresh, bool good) {
        return (s.resized || boxResized) && !fresh && good;
    }
    // Phase C's length: the travel it has left (animation ms), in
    // [FADE_MIN_MS, FADE_MAX_MS]; landed already, FADE_MIN_MS at rest.
    inline double fadeMs(bool landed, double leftMs) {
        if (landed)
            return FADE_MIN_MS;
        return leftMs < FADE_MIN_MS ? FADE_MIN_MS : leftMs > FADE_MAX_MS ? FADE_MAX_MS : leftMs;
    }
    // The hold ends (Hyprland draws the window again): once released, at
    // once if its box never changed size (as before), or once the box has
    // landed (`resizing` false) and the live picture is all that is drawn
    // (`live` 1); LAST_MS after the snapshot, whatever.
    inline bool over(ERelease r, double ms, bool boxResized, bool resizing, double live) {
        if (ms >= LAST_MS)
            return true;
        if (r == ERelease::Hold)
            return false;
        if (!boxResized)
            return true;
        return !resizing && live >= 1.0;
    }
}
