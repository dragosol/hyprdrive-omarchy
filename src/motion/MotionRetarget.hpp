#pragma once

// A TURN THAT KEEPS ITS SPEED: the velocity-matched retarget (inventory B6).
//
// The sites that ease from/to over a curve and re-aim mid-way (a flight
// reversed, the grid reflowing again, the canvas pan re-aimed, the dock's
// glides, a border fade turned) started the new leg from the value as drawn
// but at the CURVE'S start slope: the house DECEL leaves at 6.25x its average
// speed, so a re-aim kicked, and a reversal snapped from full speed one way to
// full speed the other.
//
// THE NEW LEG, ONLY WHEN THE OLD ONE WAS STILL MOVING, is the site's own curve
// plus a correction that dies out before the end:
//
//     value(u) = from + (to - from) * E(u) + kick * K(u),   K(u) = u (1 - u)^2
//
// K(0) = 0 and K'(0) = 1, so the kick sets the start slope to the velocity the
// old leg had at the turn; K(1) = K'(1) = 0, so the leg still lands exactly on
// `to`, at the curve's own end slope, at the curve's own duration. An
// uninterrupted leg has kick == 0 and is the site's old expression, to the bit:
// a site evaluates `plain` its old way and adds the kick only when it is not 0.
//
// ZERO BOUNCE WINS OVER CONTINUITY (the house rule). The kick is clamped so the
// leg never passes `to`, and -- when the old velocity already pointed at the
// target -- never moves back past `from`. A reversal (the old velocity pointing
// away) may carry on a little the old way before it comes back: that is the
// speed it had, not a bounce. A component whose target is where it already is
// cannot move at all without passing it, so it stops there (kick 0).
// tests/unit/motion_math_test.cpp holds all of this, per site curve.
//
// Pure (<cmath>, <algorithm> only); every value is in the site's own units, u
// is the leg's progress 0..1 (its eased-before progress, not the eased one).

#include <algorithm>
#include <cmath>

namespace Hyprdrive::Motion {

    // The correction's shape and its slope.
    inline double kickBasis(double u) {
        const double V = 1.0 - u;
        return u * V * V;
    }
    inline double kickBasisSlope(double u) {
        const double V = 1.0 - u;
        return V * V - 2.0 * u * V;
    }

    // dE/du of a curve at u, by central difference (one-sided at the ends).
    template <typename FCurve>
    inline double curveSlope(FCurve curve, double u) {
        constexpr double H  = 1e-3;
        const double     LO = std::max(0.0, u - H), HI = std::min(1.0, u + H);
        if (HI <= LO)
            return 0.0;
        return (static_cast<double>(curve(static_cast<float>(HI))) - static_cast<double>(curve(static_cast<float>(LO)))) / (HI - LO);
    }

    // What one curve allows, measured once per retarget: its start slope, and
    // the range of the NORMALISED kick (kick / (to - from)) that keeps
    // E(u) + k K(u) inside [0, 1] -- `hi` for "never past the target", `lo`
    // for "never back past the start".
    struct SKickRoom {
        double slope0 = 0.0;
        double lo     = -1e300;
        double hi     = 1e300;
    };
    template <typename FCurve>
    inline SKickRoom kickRoom(FCurve curve) {
        SKickRoom room;
        room.slope0 = curveSlope(curve, 0.0);
        constexpr int N = 96;
        for (int i = 1; i < N; ++i) {
            const double U = static_cast<double>(i) / N;
            const double K = kickBasis(U);
            const double E = curve(static_cast<float>(U));
            room.hi        = std::min(room.hi, (1.0 - E) / K);
            room.lo        = std::max(room.lo, -E / K);
        }
        return room;
    }

    // The kick for one component of a new leg: `delta` = to - from, `velPerU`
    // = the old leg's velocity at the turn, in the site's units per unit of the
    // NEW leg's u (velocity per ms x the new leg's ms).
    inline double matchKick(double delta, double velPerU, const SKickRoom& room) {
        if (delta == 0.0 || !std::isfinite(delta) || !std::isfinite(velPerU))
            return 0.0;
        const double TOWARD = velPerU / delta; // > 0: already heading for the target
        double       k      = TOWARD - room.slope0;
        // "Never back past the start" only binds when the old velocity already
        // pointed at the target, and gives way to "never past the target".
        if (TOWARD >= 0.0)
            k = std::max(k, room.lo);
        k = std::min(k, room.hi);
        return std::isfinite(k) ? k * delta : 0.0;
    }

    // A leg's velocity (units per unit u) at u: the curve's part and the kick's.
    inline double legSlope(double delta, double curveSlopeAtU, double kick, double u) {
        return delta * curveSlopeAtU + kick * kickBasisSlope(u);
    }

    // The value with its kick: `plain` is the site's own from + delta * E(u).
    inline double withKick(double plain, double kick, double u) {
        return kick == 0.0 ? plain : plain + kick * kickBasis(u);
    }
}
