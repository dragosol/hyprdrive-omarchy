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
// plus a short correction at its start:
//
//     value(u) = from + (to - from) * E(u) + kick * K(u),
//     K(u)     = u (1 - u/w)^2  for u < w,  0 after.
//
// K(0) = 0 and K'(0) = 1, so the kick sets the start slope toward the velocity
// the old leg had at the turn; K(w) = K'(w) = 0, so from u = w on the leg IS
// the plain retarget, to the bit: it arrives exactly when and where the plain
// one does. An uninterrupted leg has kick == 0 and is the site's old
// expression, to the bit: a site evaluates `plain` its old way and adds the
// kick only when it is not 0.
//
// THE WINDOW w is short: TURN_MS of the leg's own (unscaled) time, at most
// MAX_WINDOW of it, and over before the plain leg is within `arrive` of its
// target. A correction spread over the whole leg (the first version, u(1-u)^2)
// lagged the leg all the way in: a slow old speed against the curve's steep
// start drove the kick strongly negative, the leg flattened ~80% of the way,
// stepped back and arrived late (rig, 2026-11-11: grid reflow 1336 ms vs 606).
//
// WHAT THE KICK MAY DO (clamped; k = 0, the plain retarget, always qualifies):
//  - never pass the target, nor overshoot more than the plain curve does;
//  - heading for the target already: never slower than FLOOR of the plain
//    leg's speed at that moment -- no stall, no step back;
//  - a reversal (the old velocity pointing away) may carry on the old way --
//    the speed it had, not a bounce -- by at most REVERSAL_ROOM of the new leg
//    and at most the site's `maxCarry` units, and has turned by w/3 (K' = 0
//    there, so the leg moves at the plain curve's own speed).
// tests/unit/motion_math_test.cpp holds all of this, per site curve.
//
// USED BY the infinite-canvas pan (L1), the border fade (B1), the
// minimize/restore flight reversed mid-air (F1), the dock's arrangement glide
// (D2) and its hover preview panel (D9). NOT by the
// overview grid reflow (O1), which takes the plain retarget again: a query
// typed and erased re-aims every tile inside the window, each leg then starts
// behind the plain one, and the grid settled ~50 ms later at scale 2 (rig,
// 2026-11-11) -- later than the plain retarget is not allowed.
//
// Pure (<cmath>, <algorithm> only); every value is in the site's own units, u
// is the leg's progress 0..1 (its eased-before progress, not the eased one).

#include <algorithm>
#include <array>
#include <cmath>

namespace Hyprdrive::Motion {

    // One component's correction: its size (site units) and its window (of u).
    struct SKick {
        double kick = 0.0;
        double w    = 1.0;
        bool   on() const {
            return kick != 0.0;
        }
    };

    // The correction's shape and its slope, over the window w.
    inline double kickBasis(double u, double w) {
        if (!(u > 0.0) || !(u < w))
            return 0.0;
        const double V = 1.0 - u / w;
        return u * V * V;
    }
    inline double kickBasisSlope(double u, double w) {
        if (u < 0.0 || !(u < w))
            return 0.0;
        const double S = u / w, V = 1.0 - S;
        return V * V - 2.0 * S * V;
    }

    // dE/du of a curve at u, by central difference (one-sided at the ends).
    template <typename FCurve>
    inline double curveSlope(FCurve curve, double u) {
        constexpr double H  = 2.5e-4;
        const double     LO = std::max(0.0, u - H), HI = std::min(1.0, u + H);
        if (HI <= LO)
            return 0.0;
        return (static_cast<double>(curve(static_cast<float>(HI))) - static_cast<double>(curve(static_cast<float>(LO)))) / (HI - LO);
    }

    // How long the correction lives, in the leg's own (unscaled) ms: a
    // reversal has turned by a third of it (50 ms at 1x).
    inline constexpr double TURN_MS = 150.0;
    // ...and never more than this much of the leg.
    inline constexpr double MAX_WINDOW = 0.5;
    // Heading for the target, a kicked leg never runs slower than this much
    // of the plain leg's speed at the same moment.
    inline constexpr double FLOOR = 0.4;
    // A reversal, once turned, never runs slower than this much of the plain
    // leg's speed again (up to w/3; past it, at least the plain speed).
    inline constexpr double TURNED = 0.1;
    // How far (of the new leg's distance) a reversal may carry on the old way.
    // Full velocity continuity would cancel the curve's own start slope too (the
    // house DECEL leaves at 6.25x), which on a long leg ran the view most of the
    // way back to the OLD target before turning (rig, canvas pan): the turn
    // looked ignored. The site's `maxCarry` bounds it in its units as well.
    inline constexpr double REVERSAL_ROOM = 0.10;

    // A NUDGE IS NOT A RE-AIM. A target that moved less than this (site
    // units: px) since the old leg was aimed takes the plain retarget: a tile
    // whose slot settles a pixel at a time was re-aimed every few frames, and
    // matching the old speed each time compounded into a crawl (rig,
    // 2026-11-11: a grid reflow 330 ms late at scale 2), where the plain
    // restart reaches it on time.
    inline constexpr double REAIM_MIN_PX = 8.0;
    inline bool isReaim(double oldTo, double newTo, double minUnits = REAIM_MIN_PX) {
        return std::abs(newTo - oldTo) >= minUnits;
    }

    // ONE MATCH PER BURST. A turn while the old leg's own correction is still
    // under way takes the plain retarget: each matched leg may run a little
    // behind the plain one inside its window, and turns in quick succession
    // (a search query typed and erased) stacked those lags into a late
    // arrival. Turned once, a leg is plain from its window on.
    inline bool inWindow(const SKick& k, double u) {
        return k.on() && u < k.w;
    }

    // What a site allows its new leg, in its own units.
    struct SKickLimits {
        double legMs    = 0.0; // the new leg's unscaled duration; <= 0: MAX_WINDOW
        double maxCarry = 0.0; // a reversal carries on the old way at most this far
        double arrive   = 0.5; // the correction is over before the plain leg is this close
    };

    // The kick for one component of a new leg: `delta` = to - from, `velPerU`
    // = the old leg's velocity at the turn, in the site's units per unit of the
    // NEW leg's u (velocity per ms x the new leg's ms).
    template <typename FCurve>
    inline SKick matchKick(FCurve curve, double delta, double velPerU, const SKickLimits& lim) {
        if (delta == 0.0 || !std::isfinite(delta) || !std::isfinite(velPerU))
            return {};
        const auto   E      = [&](double u) { return static_cast<double>(curve(static_cast<float>(u))); };
        const double D      = std::abs(delta);
        const double TOWARD = velPerU / delta; // > 0: already heading for the target
        double       w      = MAX_WINDOW;
        if (lim.legMs > 0.0 && std::isfinite(lim.legMs))
            w = std::min(w, TURN_MS / lim.legMs);
        // Over before the plain leg arrives: from there on it is the plain leg.
        constexpr int N = 96;
        for (int i = 1; i <= N; ++i) {
            const double U = w * i / N;
            if (D * std::abs(1.0 - E(U)) <= lim.arrive) {
                w = w * (i - 1) / N;
                break;
            }
        }
        if (!(w > 1e-3))
            return {};
        // Sampled densest at the start, where the house curves bend hardest.
        double KS[N], R[N]; // K'(u), and E'(u) / K'(u) where K' > 0
        double lo = -1e300, hi = 1e300;
        const double CARRY = std::clamp(std::isfinite(lim.maxCarry) ? lim.maxCarry / D : 0.0, 0.0, REVERSAL_ROOM);
        for (int i = 0; i < N; ++i) {
            const double Q  = static_cast<double>(i) / N;
            const double U  = w * Q * Q;
            const double K  = kickBasis(U, w);
            const double KD = kickBasisSlope(U, w);
            const double EU = E(U);
            const double ES = std::max(0.0, curveSlope(curve, U));
            KS[i] = KD;
            R[i]  = KD > 0.0 ? ES / KD : 1e300;
            if (K > 0.0) {
                hi = std::min(hi, std::max(0.0, 1.0 - EU) / K); // never past the target (nor past the plain overshoot)
                lo = std::max(lo, -(std::max(EU, 0.0) + (TOWARD >= 0.0 ? 0.0 : CARRY)) / K);
            }
            if (TOWARD >= 0.0) { // no stall: at least FLOOR of the plain speed
                if (KD > 0.0)
                    lo = std::max(lo, -(1.0 - FLOOR) * ES / KD);
                else if (KD < 0.0)
                    hi = std::min(hi, (1.0 - FLOOR) * ES / -KD);
            }
        }
        double k = std::clamp(TOWARD - curveSlope(curve, 0.0), std::min(lo, 0.0), std::max(hi, 0.0));
        if (TOWARD < 0.0) // a reversal only ever slows the new way's start
            k = std::min(k, 0.0);
        // A reversal turns ONCE: the leg's speed E' + k K' changes sign at
        // most once, from the old way to the new. Once it is back on the new
        // way at TURNED of the plain speed, it stays there at every later point
        // (k >= -(1 - TURNED) E'/K'; those can only need a smaller |k|, so this
        // settles in a few rounds). The margin keeps the baked tables' grain
        // from wobbling the leg back at the turn.
        for (int round = 0; k < 0.0 && round < 8; ++round) {
            int first = N;
            for (int i = 0; i < N; ++i)
                if (KS[i] <= 0.0 || k >= -(1.0 - TURNED) * R[i]) {
                    first = i;
                    break;
                }
            double need = k;
            for (int i = first + 1; i < N; ++i)
                if (KS[i] > 0.0)
                    need = std::max(need, -(1.0 - TURNED) * R[i]);
            if (need <= k)
                break;
            k = std::min(need, 0.0);
        }
        if (!std::isfinite(k) || k == 0.0)
            return {};
        return {.kick = k * delta, .w = w};
    }

    // A fade (0..1) turned at `from` toward from + delta: how far it may carry
    // on the old way and stay inside 0..1, at most `cap`.
    inline double fadeCarry(double from, double delta, double cap = 0.1) {
        return std::clamp(delta > 0.0 ? from : 1.0 - from, 0.0, cap);
    }

    // A leg's velocity (units per unit u) at u: the curve's part and the kick's.
    inline double legSlope(double delta, double curveSlopeAtU, const SKick& kick, double u) {
        return delta * curveSlopeAtU + kick.kick * kickBasisSlope(u, kick.w);
    }

    // The value with its kick: `plain` is the site's own from + delta * E(u).
    inline double withKick(double plain, const SKick& kick, double u) {
        return kick.kick == 0.0 ? plain : plain + kick.kick * kickBasis(u, kick.w);
    }

    // A BOX's turn (x, y, width, height; any type with those members): the
    // old leg oldFrom -> oldTo with its own kick, at its progress u, re-aimed
    // from `cur` (the box as drawn) to `to`. `perU` turns the old leg's
    // velocity per old u into per new u (new ms / old ms). Off -- the plain
    // retarget -- when the old leg is over, its own correction is still under
    // way (one match per burst), or no edge moved by a real re-aim.
    using SBoxKick = std::array<SKick, 4>;
    inline bool boxKicked(const SBoxKick& k) {
        return k[0].on() || k[1].on() || k[2].on() || k[3].on();
    }
    // ...the old leg on `oldCurve`, the new one on `newCurve` (a panel that
    // slid decelerating and now dismisses accelerating).
    // `oncePerBurst` false: a site whose re-aims come in a burst of one event
    // (the dock: a window closing bumps its items two or three times in one
    // frame) may match again inside the last correction, from the velocity
    // that correction had -- the burst is one turn, not a stream.
    template <typename FOld, typename FNew, typename TBox>
    inline SBoxKick matchBoxKick2(FOld oldCurve, FNew newCurve, const TBox& oldFrom, const TBox& oldTo, const SBoxKick& oldKick, double u, double perU, const TBox& cur,
                                  const TBox& to, const SKickLimits& lim, double minReaim = REAIM_MIN_PX, bool oncePerBurst = true) {
        if (!(u >= 0.0 && u < 1.0) || !std::isfinite(perU))
            return {};
        for (const auto& K : oldKick)
            if (oncePerBurst && inWindow(K, u))
                return {};
        const double OF[4] = {oldFrom.x, oldFrom.y, oldFrom.width, oldFrom.height}, OT[4] = {oldTo.x, oldTo.y, oldTo.width, oldTo.height};
        const double C[4] = {cur.x, cur.y, cur.width, cur.height}, T[4] = {to.x, to.y, to.width, to.height};
        bool         reaim = false;
        for (int i = 0; i < 4; ++i)
            reaim |= isReaim(OT[i], T[i], minReaim);
        if (!reaim)
            return {};
        const double S = curveSlope(oldCurve, u);
        SBoxKick     out{};
        for (int i = 0; i < 4; ++i)
            out[i] = matchKick(newCurve, T[i] - C[i], legSlope(OT[i] - OF[i], S, oldKick[i], u) * perU, lim);
        return out;
    }
    template <typename FCurve, typename TBox>
    inline SBoxKick matchBoxKick(FCurve curve, const TBox& oldFrom, const TBox& oldTo, const SBoxKick& oldKick, double u, double perU, const TBox& cur, const TBox& to,
                                 const SKickLimits& lim, double minReaim = REAIM_MIN_PX, bool oncePerBurst = true) {
        return matchBoxKick2(curve, curve, oldFrom, oldTo, oldKick, u, perU, cur, to, lim, minReaim, oncePerBurst);
    }
    // The box with its kick; `plain` untouched when there is none.
    template <typename TBox>
    inline TBox withBoxKick(TBox plain, const SBoxKick& k, double u) {
        if (!boxKicked(k))
            return plain;
        plain.x      = withKick(plain.x, k[0], u);
        plain.y      = withKick(plain.y, k[1], u);
        plain.width  = std::max(2.0, withKick(plain.width, k[2], u));
        plain.height = std::max(2.0, withKick(plain.height, k[3], u));
        return plain;
    }
}
