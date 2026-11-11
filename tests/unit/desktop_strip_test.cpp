// Host test for DesktopStripModel.hpp: the spring and what the wheel asks of it.
#include "DesktopStripModel.hpp"

#include <cmath>
#include <cstdio>

using namespace Hyprdrive::StripModel;

static int g_checks = 0, g_failed = 0;
#define CHECK(c)                                                                                                                                                                   \
    do {                                                                                                                                                                           \
        ++g_checks;                                                                                                                                                                \
        if (!(c)) {                                                                                                                                                                \
            ++g_failed;                                                                                                                                                            \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c);                                                                                                               \
        }                                                                                                                                                                          \
    } while (0)

static bool near(double a, double b, double eps = 1e-6) {
    return std::abs(a - b) < eps;
}

int main() {
    // ---- the spring
    {
        // Closed form: one big step == many small ones.
        SSpring a{.pos = 0, .vel = 3}, b = a;
        a.step(4, 13, 0.2);
        for (int i = 0; i < 200; ++i)
            b.step(4, 13, 0.001);
        CHECK(near(a.pos, b.pos, 1e-6) && near(a.vel, b.vel, 1e-5));

        // From rest it never overshoots (critically damped) and settles.
        SSpring s;
        double  maxPos = 0;
        for (int i = 0; i < 120; ++i) {
            s.step(1, 13, 1.0 / 120);
            maxPos = std::max(maxPos, s.pos);
        }
        CHECK(maxPos <= 1.0 + 1e-9);
        CHECK(s.settled(1, 1e-3));

        // A second step while the first is under way: speed carries on, no
        // stop -- the velocity never drops to zero on the way.
        SSpring c;
        double  minVel = 1e9;
        for (int i = 0; i < 12; ++i)
            c.step(1, 13, 1.0 / 120);
        for (int i = 0; i < 12; ++i) {
            c.step(2, 13, 1.0 / 120);
            minVel = std::min(minVel, c.vel);
        }
        CHECK(minVel > 0.5);

        // The bounce kick peaks at the asked distance.
        SSpring k{.pos = 0, .vel = kickFor(BOUNCE, 13)};
        double  peak = 0;
        for (int i = 0; i < 240; ++i) {
            k.step(0, 13, 1.0 / 1200);
            peak = std::max(peak, k.pos);
        }
        CHECK(near(peak, BOUNCE, 2e-4));
    }

    // ---- ticks mode
    {
        SSettings s;
        SDrive    d;
        d.reset(5, 1);
        // Every notch is a desktop, as fast as they come.
        CHECK(d.feed(120, 1000, s) && d.target == 2);
        CHECK(d.feed(120, 1010, s) && d.target == 3);
        CHECK(d.goal(s) == 3);
        // Back: a reverse turns it round at once.
        CHECK(d.feed(-120, 1020, s) && d.target == 2);

        // A high-resolution wheel: fractions show as a peek, a whole notch steps.
        d.reset(5, 1);
        CHECK(!d.feed(60, 2000, s) && d.target == 1);
        CHECK(near(d.goal(s), 1 + PEEK * 0.5));
        CHECK(d.holding(s));
        CHECK(d.feed(60, 2010, s) && d.target == 2 && d.goal(s) == 2);
        // ... and a fraction left hanging is let go after PRESSURE_MS.
        d.feed(30, 2020, s);
        CHECK(d.goal(s) > 2);
        d.tick(2020 + PRESSURE_MS, s);
        CHECK(d.goal(s) == 2 && !d.holding(s));

        // Two notches per desktop.
        s.ticks = 2;
        d.reset(5, 0);
        CHECK(!d.feed(120, 3000, s) && near(d.goal(s), PEEK * 0.5));
        CHECK(d.feed(120, 3010, s) && d.target == 1);
        CHECK(!d.feed(120, 3020, s));
        CHECK(d.feed(120, 3030, s) && d.target == 2);
        // Four notches in one event (a fast spin): two desktops.
        CHECK(d.feed(480, 3040, s) && d.target == 4);
        s.ticks = 1;

        // The ends: no step, one bounce per push, nothing banked against it.
        d.reset(3, 2);
        CHECK(!d.feed(120, 4000, s) && d.target == 2 && d.kick && d.kickDir == 1);
        d.kick = false;
        d.feed(120, 4010, s);
        CHECK(!d.kick); // the same push
        CHECK(d.feed(-120, 4020, s) && d.target == 1); // straight back, nothing to undo
        // Bounce off: no kick.
        s.bounce = false;
        d.reset(3, 0);
        d.feed(-120, 5000, s);
        CHECK(!d.kick && d.target == 0);
        s.bounce = true;

        // Sticky: the first desktop of a burst takes `ticks`, each further
        // one STICKY_EXTRA more; a pause ends the burst.
        s.sticky = true;
        d.reset(6, 0);
        CHECK(d.feed(120, 6000, s) && d.target == 1);
        CHECK(!d.feed(120, 6010, s) && !d.feed(120, 6020, s)); // resisting
        CHECK(d.goal(s) > 1 && d.goal(s) < 1 + PEEK);          // ... and showing it
        CHECK(d.feed(120, 6030, s) && d.target == 2);
        CHECK(d.feed(120, 6030 + BURST_MS, s) && d.target == 3); // a new burst: one notch again
        // The pressure let go mid-burst: banked notches gone, the burst goes on.
        CHECK(!d.feed(120, 7000, s));
        CHECK(!d.feed(120, 7000 + PRESSURE_MS, s) && d.acc == 120);
        s.sticky = false;
    }

    // ---- smooth mode
    {
        SSettings s{.smooth = true};
        SDrive    d;
        d.reset(5, 1);
        // Half a desktop per notch, followed continuously.
        CHECK(!d.feed(60, 1000, s) && near(d.goal(s), 1.25));
        CHECK(d.holding(s));
        CHECK(d.feed(60, 1016, s) && d.target == 2 && near(d.goal(s), 1.5)); // past half way: that one is shown
        // Still for SNAP_MS: it settles on the nearest, thrown by its speed.
        d.tick(1016 + SNAP_MS, s);
        CHECK(!d.holding(s) && d.target == 2 && d.goal(s) == 2);
        // A slow nudge settles back.
        d.feed(40, 2000, s);
        d.tick(2000 + SNAP_MS, s);
        CHECK(d.target == 2 && d.goal(s) == 2);
        // Two quick notches are one desktop, never thrown to a second.
        d.reset(5, 1);
        d.feed(120, 2500, s);
        d.feed(120, 2560, s);
        CHECK(d.target == 2);
        d.tick(2560 + SNAP_MS, s);
        CHECK(d.target == 2);
        // One notch, half way: on to the next (the push decides a tie).
        d.reset(5, 1);
        d.feed(120, 2800, s);
        d.tick(2800 + SNAP_MS, s);
        CHECK(d.target == 2);
        // A quick flick a third of the way is thrown on to the next.
        d.feed(40, 3000, s);
        d.feed(40, 3008, s);
        CHECK(d.target == 2);
        CHECK(d.tick(3008 + SNAP_MS, s) && d.target == 3);

        // The rubber band past an end, and back on release.
        d.reset(3, 2);
        d.feed(240, 4000, s);
        CHECK(d.target == 2 && d.goal(s) > 2 && d.goal(s) < 2 + RUBBER);
        d.tick(4000 + SNAP_MS, s);
        CHECK(d.goal(s) == 2);
        // Without bounce: a hard stop, and straight back out of it.
        s.bounce = false;
        d.reset(3, 2);
        d.feed(240, 5000, s);
        CHECK(d.goal(s) == 2);
        d.feed(-120, 5010, s);
        CHECK(near(d.goal(s), 1.5));
        s.bounce = true;

        // Sticky: after the first desktop, a dead zone at each one reached.
        s.sticky = true;
        d.reset(6, 0);
        d.feed(240, 6000, s); // to 1.0: the first desktop, no zone before it
        CHECK(d.target == 1 && near(d.drag, 1.0));
        d.feed(120, 6010, s); // 0.5 on: free (zone only at a desktop reached after the first)
        CHECK(near(d.drag, 1.5) && d.target == 2);
        d.feed(120, 6020, s); // reaches 2.0, zone set
        CHECK(near(d.drag, 2.0) && d.zone > 0);
        d.feed(48, 6030, s); // 0.2 into the zone: moves 0.04
        CHECK(near(d.drag, 2.0 + 0.2 * STICKY_GIVE, 1e-9));
        d.feed(96, 6040, s); // through the rest (0.2 more) and 0.2 beyond
        CHECK(near(d.drag, 2.0 + STICKY_ZONE * STICKY_GIVE + 0.2, 1e-9) && d.zone == 0);
        // Settling from inside a zone never throws on.
        d.reset(6, 0);
        d.feed(240, 7000, s);
        d.feed(240, 7008, s); // reaches 2: zone
        d.feed(24, 7016, s);  // in the zone, fast
        CHECK(d.zone > 0);
        d.tick(7016 + SNAP_MS, s);
        CHECK(d.target == 2);
    }

    std::printf("desktop_strip_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
