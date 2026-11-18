// Host test for TripFrameRule.hpp: when a travelling window's live picture is
// good again (until then it is drawn from its last good frame).
#include "TripFrameRule.hpp"

#include <cstdio>
#include <initializer_list>

using namespace Hyprdrive::TripFrameRule;

static int g_checks = 0, g_failed = 0;
#define CHECK(c)                                                                                                                                                                   \
    do {                                                                                                                                                                           \
        ++g_checks;                                                                                                                                                                \
        if (!(c)) {                                                                                                                                                                \
            ++g_failed;                                                                                                                                                            \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c);                                                                                                               \
        }                                                                                                                                                                          \
    } while (0)

int main() {
    {   // CEF (slow_redraw --flash --lag 150): white at once at the new size, drawn 150 ms later
        SState s;
        CHECK(decide(s, 0) == ERelease::Hold);
        sent(s, true, true);
        committed(s, 2, false); // the old buffer, once more, at the old size
        CHECK(decide(s, 3) == ERelease::Hold);
        committed(s, 5, true); // white
        CHECK(decide(s, 5) == ERelease::Hold);
        CHECK(decide(s, 150) == ERelease::Hold);
        committed(s, 155, true); // drawn
        CHECK(decide(s, 155) == ERelease::Drawn);
    }
    {   // XWayland re-commits the white frame 60 ms later (the helper window mapped): not a frame
        SState s;
        sent(s, true, true);
        committed(s, 1, true);   // white
        committed(s, 62, true);  // white again
        CHECK(s.drawn == 1);
        CHECK(decide(s, 62) == ERelease::Hold);
        committed(s, 152, true); // drawn
        CHECK(decide(s, 152) == ERelease::Drawn);
    }
    {   // ...or 102 ms later, past MIN_GAP_MS, because the helper mapped later
        // and the desktop re-arranged: the window was only MOVED since its
        // last commit, so the commit is the X server's, not a frame
        SState s;
        sent(s, true, true);
        committed(s, 21, true);                // white
        committed(s, 123, true, true);         // re-committed after a move
        CHECK(s.drawn == 1);
        CHECK(decide(s, 123) == ERelease::Hold);
        committed(s, 171, true);               // drawn
        CHECK(decide(s, 171) == ERelease::Drawn);
    }
    {   // a client that draws once, late, and then stays still
        SState s;
        sent(s, true, true);
        committed(s, 150, true);
        CHECK(decide(s, 300) == ERelease::Hold);
        CHECK(decide(s, 399) == ERelease::Hold);
        CHECK(decide(s, 400) == ERelease::Settled); // (and the timeout, the same moment)
    }
    {   // ...or once, early: it settles before the timeout
        SState s;
        sent(s, true, true);
        committed(s, 20, true);
        CHECK(decide(s, 200) == ERelease::Hold);
        CHECK(decide(s, 270) == ERelease::Settled);
    }
    {   // a client that never redraws still appears
        SState s;
        sent(s, true, true);
        CHECK(decide(s, 399) == ERelease::Hold);
        CHECK(decide(s, 400) == ERelease::Timeout);
    }
    {   // a trip that kept its size: the live window is the right picture
        SState s;
        sent(s, false, false);
        committed(s, 10, true);
        CHECK(decide(s, 50) == ERelease::Hold);
        CHECK(decide(s, 100) == ERelease::Unchanged);
    }
    {   // ...but not while its size is still waiting to be sent (the trip's gate)
        SState s;
        sent(s, false, false);
        CHECK(decide(s, 150, true) == ERelease::Hold);
        CHECK(decide(s, 399, true) == ERelease::Hold);
        CHECK(decide(s, 400, true) == ERelease::Timeout);
        // sent at last: a real resize, held for its frames
        sent(s, true, true);
        CHECK(decide(s, 200, false) == ERelease::Hold);
    }
    {   // a client that redraws late with no blank frame first (slow_redraw
        // without --flash): it was TOLD the new size at once, so the hold
        // waits for its frame -- it never counts as "unchanged" while it has
        // not committed (the reported size of an X11 window follows commits)
        SState s;
        sent(s, true, true); // told the new size at 1 ms
        CHECK(decide(s, 100) == ERelease::Hold);
        CHECK(decide(s, 149) == ERelease::Hold);
        committed(s, 150, true); // its one real frame
        CHECK(decide(s, 150) == ERelease::Hold);
        CHECK(decide(s, 400) == ERelease::Settled);
    }
    {   // a quick toggle back mid-hold: another size, the count starts again
        SState s;
        sent(s, true, true);
        committed(s, 10, true);
        sent(s, true, true);
        CHECK(s.drawn == 0);
        committed(s, 30, true);
        CHECK(decide(s, 40) == ERelease::Hold);
        committed(s, 160, true);
        CHECK(decide(s, 160) == ERelease::Drawn);
        // the same size again is not a new one
        SState t;
        sent(t, true, true);
        committed(t, 10, true);
        sent(t, true, false);
        CHECK(t.drawn == 1);
    }
    {   // commits before any size was sent do not count
        SState s;
        committed(s, 5, true);
        CHECK(s.drawn == 0);
    }
    {   // a native Wayland window: its second frame counts whenever it comes
        // (Firefox: its main surface at 17 ms, its page's subsurface at 25 ms)
        SState s;
        sent(s, true, true);
        committed(s, 17, true, false, true);
        CHECK(decide(s, 17) == ERelease::Hold);
        committed(s, 25, true, false, true);
        CHECK(decide(s, 25) == ERelease::Drawn);
        // ...but not twice in one commit (a synced subsurface commits with its parent)
        SState t;
        sent(t, true, true);
        committed(t, 17.0, true, false, true);
        committed(t, 17.2, true, false, true);
        CHECK(t.drawn == 1);
        CHECK(decide(t, 200) == ERelease::Hold);
        CHECK(decide(t, 267) == ERelease::Settled); // drew once, then still: SETTLE_MS, as before
        // X11 keeps MIN_GAP_MS: the same pace is the X server's re-commit, not a frame
        SState x;
        sent(x, true, true);
        committed(x, 17, true);
        committed(x, 25, true);
        CHECK(x.drawn == 1);
        CHECK(decide(x, 25) == ERelease::Hold);
        committed(x, 117, true);
        CHECK(decide(x, 117) == ERelease::Drawn);
        // and X11's moved-only re-commit never counts, past the gap or not
        SState m;
        sent(m, true, true);
        committed(m, 1, true);
        committed(m, 120, true, true);
        CHECK(m.drawn == 1);
        // a Wayland frame before a size was sent, or at another size, does not count either
        SState w;
        committed(w, 5, true, false, true);
        CHECK(w.drawn == 0);
        sent(w, true, true);
        committed(w, 10, false, false, true);
        CHECK(w.drawn == 0);
    }
    {   // what counts as a frame: Chromium (viewport onto its old buffer, partial damage)
        SState s;
        sent(s, true, true);
        CHECK(!frame(s, true, false, 0.17));     // before any frame at the size: a touch-up is not one
        CHECK(frame(s, true, true, 0.24));       // its picture newly at the size it was sent: the first
        committed(s, 7, frame(s, true, true, 0.24), false, true);
        CHECK(s.drawn == 1);
        CHECK(frame(s, true, false, 0.17));      // then any repaint there is the second
        CHECK(!frame(s, true, false, 0.0));      // ...not a commit that repainted nothing
        committed(s, 23, frame(s, true, false, 0.17), false, true);
        CHECK(decide(s, 23) == ERelease::Drawn);
        // X11 is untouched: only a full repaint is a frame, first or second
        SState x;
        sent(x, true, true);
        CHECK(!frame(x, false, true, 0.24));
        CHECK(frame(x, false, false, 0.6));
        committed(x, 5, frame(x, false, false, 0.6));
        CHECK(!frame(x, false, false, 0.17));
        CHECK(frame(x, false, false, 1.0));
    }
    {   // THE PICTURE STRETCHES: the timeout counts from the box's landing
        SState s;
        sent(s, true, true);
        CHECK(decide(s, 400, false, -1) == ERelease::Hold); // still travelling: not yet
        CHECK(decide(s, 1199, false, -1) == ERelease::Hold);
        CHECK(decide(s, 1559, false, 1160) == ERelease::Hold); // landed at 1160
        CHECK(decide(s, 1560, false, 1160) == ERelease::Timeout);
        // ...but never past the cap, landed or not
        CHECK(decide(s, CAP_MS, false, -1) == ERelease::Timeout);
        CHECK(decide(s, CAP_MS, false, 1900) == ERelease::Timeout);
        // the client's own frames release it mid-travel, as before (the fade starts there)
        committed(s, 20, true);
        CHECK(decide(s, 270, false, -1) == ERelease::Settled);
        committed(s, 130, true);
        CHECK(decide(s, 130, false, -1) == ERelease::Drawn);
    }
    {   // ...a box that never changed size: the timeout from the snapshot, as before
        SState s;
        sent(s, true, true);
        CHECK(decide(s, 399, false, 0) == ERelease::Hold);
        CHECK(decide(s, 400, false, 0) == ERelease::Timeout);
        SState u; // and a trip that kept its size releases as it did
        sent(u, false, false);
        CHECK(decide(u, 100, false, 0) == ERelease::Unchanged);
    }
    {   // the hold ends once the box has landed and the live picture is all there is
        CHECK(!over(ERelease::Hold, 500, true, true, 0));
        CHECK(!over(ERelease::Settled, 300, true, true, 1)); // released, still travelling: drawn stretched
        CHECK(!over(ERelease::Settled, 1200, true, false, 0.6)); // landed, still fading in
        CHECK(over(ERelease::Settled, 1200, true, false, 1));
        CHECK(!over(ERelease::Timeout, CAP_MS, true, true, 1)); // the cap: live, stretched until it lands
        CHECK(over(ERelease::Timeout, CAP_MS, true, false, 1));
        CHECK(over(ERelease::Hold, LAST_MS, true, true, 0));    // and never drawn by the hold past this
        // a box that never changed size (kept its size, or a slide that only
        // moves it): released means over, at once, as before
        CHECK(over(ERelease::Unchanged, 100, false, false, 0));
        CHECK(over(ERelease::Drawn, 150, false, false, 0));
        CHECK(!over(ERelease::Hold, 150, false, false, 0));
    }
    {   // phase A: live old frames while it has not committed another size, if they are drawn ones
        SState s;
        CHECK(!liveNow(s, false, false, true)); // nothing changes size: the snapshot, as before
        sent(s, true, true);
        CHECK(liveNow(s, false, false, true));
        CHECK(liveNow(s, true, false, true));
        CHECK(!liveNow(s, true, true, true));  // it committed at its new size: the snapshot
        CHECK(!liveNow(s, true, false, false)); // held again before it had drawn: the snapshot
        SState u;
        CHECK(liveNow(u, true, false, true));   // its box changes size (held again mid-trip)
    }
    {   // the fade: the travel's rest, in [150, 250] ms; landed, 150 at rest
        CHECK(fadeMs(true, 900) == FADE_MIN_MS);
        CHECK(fadeMs(false, 0) == FADE_MIN_MS);
        CHECK(fadeMs(false, 200) == 200);
        CHECK(fadeMs(false, 1e9) == FADE_MAX_MS);
    }
    for (auto r : {ERelease::Hold, ERelease::Drawn, ERelease::Settled, ERelease::Unchanged, ERelease::Timeout})
        CHECK(why(r)[0] != '?');

    std::printf("trip_frame_rule_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
