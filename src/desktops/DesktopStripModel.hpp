#pragma once

// THE DESKTOP STRIP, the pure half (user, 2026-11-02: "put on a senior
// ux/product designer hat and refine the mechanics significantly"). No
// Hyprland headers: tests/unit/desktop_strip_test.cpp runs it on the host.
// DesktopStrip.cpp puts it on screen.
//
// The desktops of a monitor are cards in one row (DesktopRowModel.hpp), and the
// wheel moves the row itself: one critically damped spring carries it, so a
// second notch while the first is still sliding just pulls the spring further
// -- the row never stops, restarts its curve or jumps -- and a notch back turns
// it around where it is. Every position is in ROW INDEX units (desktop 0, 1,
// 2... of the row), never desktop ids.
//
//   SSpring  the row's position and speed, pulled toward a goal.
//   SDrive   what the wheel asks for: the desktop it lands on (target) and the
//            goal the spring is pulled to now (target, plus a peek toward the
//            next one while notches are pending, plus a rubber band past the
//            ends). Two modes:
//              ticks   `ticks` whole notches per desktop (1 = every notch); a
//                      notch short of a desktop shows as a peek.
//              smooth  the row follows the wheel continuously, SMOOTH_V120 per
//                      desktop, and settles on the nearest desktop -- thrown
//                      toward the next one by the wheel's speed -- once it has
//                      been still for SNAP_MS.
//            sticky: after the first desktop of a burst, every further one the
//            same way takes more push (STICKY_EXTRA notches, or a STICKY_ZONE
//            dead zone in smooth mode), shown as resistance; it is let go after
//            PRESSURE_MS still.

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "MotionSpring.hpp"

namespace Hyprdrive::StripModel {

    inline constexpr int      NOTCH        = 120;   // value120 per wheel notch
    inline constexpr int      SMOOTH_V120  = 240;   // smooth: two notches per desktop
    inline constexpr int      STICKY_EXTRA = 2;     // sticky: extra notches per further desktop
    inline constexpr double   STICKY_ZONE  = 0.4;   // sticky, smooth: the dead zone, in desktops
    inline constexpr double   STICKY_GIVE  = 0.2;   // ... of which the row shows this share (the resistance)
    inline constexpr double   PEEK         = 0.14;  // a pending step shows this far toward the next desktop, at most
    inline constexpr double   RUBBER       = 0.30;  // the furthest the row stretches past an end
    inline constexpr double   BOUNCE       = 0.05;  // a notch into an end throws the row this far past it
    inline constexpr uint32_t BURST_MS     = 600;   // a pause this long ends a burst
    inline constexpr uint32_t PRESSURE_MS  = 400;   // pending notches / sticky push let go after this still
    inline constexpr uint32_t SNAP_MS      = 140;   // smooth: settle after this still
    inline constexpr double   THROW_S      = 0.12;  // smooth: the settle looks this far ahead at the wheel's speed
    inline constexpr double   THROW_MAX    = 0.3;   // ... and never further (a wheel's notches are coarse: two never throw two desktops)

    struct SSettings {
        bool smooth = false; // binds:scroll_desktops_smooth
        int  ticks  = 1;     // binds:scroll_desktops_ticks (1..10)
        bool sticky = false; // binds:scroll_desktops_sticky
        bool bounce = true;  // animation:desktop_row_bounce
    };

    // A critically damped spring, stepped in closed form: exact for any dt, so
    // a late frame lands where it would have anyway.
    struct SSpring {
        double pos = 0, vel = 0;

        void step(double goal, double omega, double dt) {
            Hyprdrive::Motion::criticalStep(pos, vel, goal, omega, dt);
        }
        // At rest on `goal` to within `eps` (index units), and barely moving.
        bool settled(double goal, double eps) const {
            return std::abs(pos - goal) < eps && std::abs(vel) < eps * 20.0;
        }
    };

    // The rubber band: `over` desktops past an end shows as less, never more
    // than RUBBER (the curve iOS scroll views use).
    inline double rubber(double over) {
        if (over <= 0)
            return 0;
        return RUBBER * (1.0 - 1.0 / (over * 0.55 / RUBBER + 1.0));
    }

    // The outward speed (index units / s) that throws a resting spring of
    // stiffness `omega` `dist` past where it is: x(t) = v t e^-wt peaks at
    // v / (w e).
    inline double kickFor(double dist, double omega) {
        return dist * omega * std::exp(1.0);
    }

    struct SDrive {
        int  count  = 1; // desktops in the row
        int  target = 0; // the desktop it lands on: the one shown
        bool kick   = false; // a notch hit an end: the caller throws the spring (bounce)
        int  kickDir = 0;

        // ticks
        int acc = 0; // value120 banked toward the next desktop, signed
        // smooth
        double drag     = 0;     // where the wheel has taken the row (unclamped, before stickiness)
        double zone     = 0;     // sticky: dead zone left at the desktop just reached
        double inVel    = 0;     // the wheel's speed, index units / s
        bool   dragging = false; // smooth: following the wheel (not settled yet)
        // both
        uint32_t lastMs    = 0;
        bool     seen      = false;
        int      burstDir  = 0;
        int      burstSteps = 0; // desktops passed in this burst
        bool     wallHit   = false; // this push already bounced off an end

        void reset(int count_, int target_) {
            count      = std::max(1, count_);
            target     = std::clamp(target_, 0, count - 1);
            acc        = 0;
            drag       = target;
            zone       = 0;
            inVel      = 0;
            dragging   = false;
            seen       = false;
            burstDir   = 0;
            burstSteps = 0;
            wallHit    = false;
            kick       = false;
            kickDir    = 0;
        }

        // ticks mode: value120 per desktop now.
        static int need(const SSettings& s, int burstSteps) {
            return (std::clamp(s.ticks, 1, 10) + (s.sticky && burstSteps > 0 ? STICKY_EXTRA : 0)) * NOTCH;
        }

        // A pause, a reverse: a new burst.
        void newBurst(int dir) {
            burstDir   = dir;
            burstSteps = 0;
            wallHit    = false;
            zone       = 0;
        }

        // One wheel event: `v120` signed (+ = next), at `nowMs`. -> whether the
        // target changed.
        bool feed(int v120, uint32_t nowMs, const SSettings& s) {
            if (v120 == 0)
                return false;
            const int  DIR  = v120 > 0 ? 1 : -1;
            const bool IDLE = seen && (nowMs < lastMs || nowMs - lastMs >= BURST_MS);
            const uint32_t GAP = seen && nowMs >= lastMs ? nowMs - lastMs : 0;
            if (!seen || IDLE || DIR != burstDir)
                newBurst(DIR);
            else if (GAP >= PRESSURE_MS) {
                acc     = 0; // the pressure was let go
                wallHit = false;
            }
            seen      = true;
            lastMs    = nowMs;
            const int BEFORE = target;
            if (s.smooth)
                feedSmooth(v120, GAP, s);
            else
                feedTicks(v120, DIR, s);
            return target != BEFORE;
        }

        void feedTicks(int v120, int dir, const SSettings& s) {
            if ((acc > 0 && dir < 0) || (acc < 0 && dir > 0))
                acc = 0;
            acc += v120;
            for (;;) {
                const int NEED = need(s, burstSteps);
                if (std::abs(acc) < NEED)
                    break;
                const int NEXT = target + dir;
                if (NEXT < 0 || NEXT >= count) {
                    // An end: nothing banked against it, one throw per push.
                    acc = 0;
                    if (s.bounce && !wallHit) {
                        kick    = true;
                        kickDir = dir;
                    }
                    wallHit = true;
                    break;
                }
                acc -= dir * NEED;
                target = NEXT;
                ++burstSteps;
            }
        }

        void feedSmooth(int v120, uint32_t gapMs, const SSettings& s) {
            const bool FIRST = !dragging;
            if (FIRST) {
                drag     = target;
                zone     = 0;
                inVel    = 0;
                dragging = true;
            }
            double       d   = static_cast<double>(v120) / SMOOTH_V120;
            const int    DIR = d > 0 ? 1 : -1;
            const double DT  = std::clamp(static_cast<double>(gapMs) / 1000.0, 0.008, 0.1);
            // The wheel's speed, from the gaps between its events: the first
            // of a drag has none to go by.
            if (!FIRST)
                inVel = inVel * 0.5 + (d / DT) * 0.5;
            // Sticky: a desktop reached in this burst holds the row for a while.
            while (d != 0) {
                if (s.sticky && zone > 0) {
                    const double USE = std::min(std::abs(d), zone);
                    zone -= USE;
                    drag += DIR * USE * STICKY_GIVE;
                    d -= DIR * USE;
                    if (zone > 0)
                        break;
                    continue; // through: on from where the resistance left it

                }
                // The next whole desktop that way.
                const double NEXT = DIR > 0 ? std::floor(drag + 1e-9) + 1 : std::ceil(drag - 1e-9) - 1;
                const double TO   = drag + d;
                if ((DIR > 0 && TO < NEXT) || (DIR < 0 && TO > NEXT) || NEXT < 0 || NEXT > count - 1) {
                    drag = TO;
                    break;
                }
                d -= NEXT - drag;
                drag = NEXT;
                if (s.sticky && burstSteps > 0)
                    zone = STICKY_ZONE;
            }
            // Past an end: only into the rubber band, and only with one.
            drag           = s.bounce ? std::clamp(drag, -1.5, count - 1 + 1.5) : std::clamp(drag, 0.0, static_cast<double>(count - 1));
            const int NEAR = std::clamp(static_cast<int>(std::lround(drag)), 0, count - 1);
            if (NEAR != target) {
                target = NEAR;
                ++burstSteps;
            }
        }

        // Time passing with no wheel: the pressure let go, the smooth drag
        // settled. -> whether the target changed.
        bool tick(uint32_t nowMs, const SSettings& s) {
            if (!seen || nowMs < lastMs)
                return false;
            const uint32_t IDLE = nowMs - lastMs;
            if (IDLE >= PRESSURE_MS) {
                acc     = 0;
                zone    = 0;
                wallHit = false;
            }
            if (s.smooth && dragging && IDLE >= SNAP_MS) {
                dragging = false;
                // Thrown on by the wheel's speed -- not out of a sticky hold.
                const double THROW = zone > 0 ? 0.0 : std::clamp(inVel * THROW_S, -THROW_MAX, THROW_MAX);
                const int    TO    = std::clamp(static_cast<int>(std::lround(drag + THROW)), 0, count - 1);
                const int WAS  = target;
                target         = TO;
                drag           = TO;
                zone           = 0;
                return TO != WAS;
            }
            return false;
        }

        // Where the spring is pulled to now.
        double goal(const SSettings& s) const {
            if (s.smooth && dragging) {
                const double LAST = count - 1;
                if (drag < 0)
                    return s.bounce ? -rubber(-drag) : 0.0;
                if (drag > LAST)
                    return s.bounce ? LAST + rubber(drag - LAST) : LAST;
                return drag;
            }
            if (s.smooth || acc == 0)
                return target;
            // A step on its way: the row leans toward it.
            const double F    = std::min(1.0, std::abs(static_cast<double>(acc)) / need(s, burstSteps));
            const int    DIR  = acc > 0 ? 1 : -1;
            const int    NEXT = target + DIR;
            if (NEXT < 0 || NEXT >= count)
                return target; // nothing that way
            return target + DIR * PEEK * F;
        }

        // Still busy with input: the spring must not be called settled yet.
        bool holding(const SSettings& s) const {
            return (s.smooth && dragging) || acc != 0;
        }
    };
}
