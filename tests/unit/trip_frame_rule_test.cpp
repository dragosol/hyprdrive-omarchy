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
    for (auto r : {ERelease::Hold, ERelease::Drawn, ERelease::Settled, ERelease::Unchanged, ERelease::Timeout})
        CHECK(why(r)[0] != '?');

    std::printf("trip_frame_rule_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
