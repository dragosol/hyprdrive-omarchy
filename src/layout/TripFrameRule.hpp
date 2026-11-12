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
//     it released the hold on the white frame). A client's real frame comes
//     its redraw time after the blank one (CEF: ~150 ms); one that draws
//     faster than MIN_GAP_MS waits for its next frame or SETTLE_MS. A heuristic
//     on purpose: commits do not say what is in them, and reading the buffer
//     back each frame costs a GPU stall;
//   - a client that commits once and then stays still (a plain X11 client
//     redrawing once): SETTLE_MS after that one commit -- longer than the
//     ~150 ms a CEF launcher takes from its white frame to its real one;
//   - a client that never draws at all still appears: TIMEOUT_MS after the
//     snapshot, whatever it did.
// All in real time: it is the client's pace, not an animation (the speed
// setting does not slow the client down).
namespace Hyprdrive::TripFrameRule {
    constexpr double UNCHANGED_MS = 100.0;
    constexpr double SETTLE_MS    = 250.0;
    constexpr double TIMEOUT_MS   = 400.0;
    constexpr double FULL_REPAINT = 0.5; // of the window's area: a frame, not a touch-up
    constexpr double MIN_GAP_MS   = 100.0;

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
    // size it was sent.
    inline void committed(SState& s, double ms, bool atSent) {
        if (!s.resized || !atSent)
            return;
        if (s.drawn >= 1 && ms - s.firstMs < MIN_GAP_MS)
            return; // the same blank picture again, re-committed
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
    // sent to it (the trip's own, held back by its gate).
    inline ERelease decide(const SState& s, double ms, bool waiting = false) {
        if (s.resized && s.drawn >= 2)
            return ERelease::Drawn;
        if (s.resized && s.drawn == 1 && ms - s.firstMs >= SETTLE_MS)
            return ERelease::Settled;
        if (ms >= TIMEOUT_MS)
            return ERelease::Timeout;
        if (!s.resized && !waiting && ms >= UNCHANGED_MS)
            return ERelease::Unchanged;
        return ERelease::Hold;
    }
}
