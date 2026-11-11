#pragma once

// THE CRITICALLY DAMPED SPRING, stepped in closed form: one definition for the
// two that were written out separately (layout/ShowDesktopMath.hpp's slide and
// desktops/DesktopStripModel.hpp's row). Pure (<cmath> only) -- both of those
// headers are pure and host-tested.
//
// Exact for any dt: a late frame lands where a train of short ones would have,
// and a new goal mid-flight keeps the position AND the velocity. It never
// overshoots from rest. What a site does around the step -- clamping the
// position, deciding when it has settled, what it does with dt <= 0 beyond
// leaving the state alone -- stays at the site; this is only the step.
// tests/unit/motion_math_test.cpp holds it to both old copies, trace for trace.

#include <cmath>

namespace Hyprdrive::Motion {

    // `dt` seconds of (x, v) toward `goal` at angular frequency `omega` (1/s).
    // From rest, 95% of the way takes ~4.74/omega seconds, 99% ~6.64/omega.
    // dt <= 0 leaves both untouched.
    inline void criticalStep(double& x, double& v, double goal, double omega, double dt) {
        if (dt <= 0)
            return;
        const double X = x - goal;
        const double B = v + omega * X;
        const double E = std::exp(-omega * dt);
        x              = goal + (X + B * dt) * E;
        v              = (v - omega * B * dt) * E;
    }
}
