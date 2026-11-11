#pragma once

// A RINGED PLATE FADES AS ONE SURFACE. The label pill (dock tooltip, window
// peeks, tile captions, strip and Scratchpad labels, Mosaic titles) and the
// overview tile's close button used to draw their ring as a FILLED rounded
// rect one border wider than the plate and the plate's own fill on top, both
// at the fade's alpha. Settled (alpha 1) that is right: the ring is opaque and
// the fill's 90% black leaves 10% of the accent in the body. Mid-fade it is
// not: the body is then fill-at-alpha over accent-at-alpha over the backdrop,
// which holds up to 28% accent (alpha 5/9) against the settled 10% -- every
// entrance and exit showed the body in the border colour for a moment and
// then let it go dark (owner, 2026-11-11).
//
// The fix draws the body with the colour it SETTLES at (`settledBody`, fully
// opaque) times the fade's alpha, and the ring only outside it (a border pass,
// not a rect under the body). At any alpha the body is then the backdrop
// mixed with its settled colour -- a plain opacity fade -- and at alpha 1 the
// pixels are the old ones.
//
// Pure (no Hyprland types) so tests/unit/plate_color_test.cpp composites both
// the old and the new drawing over a backdrop at every alpha.

namespace Hyprdrive::PlateColor {

    struct SRGBA {
        float r = 0.F, g = 0.F, b = 0.F, a = 1.F;
    };

    // `top` (straight alpha) composited over the opaque `under`.
    inline SRGBA over(const SRGBA& top, const SRGBA& under) {
        const float A = top.a;
        return {top.r * A + under.r * (1.F - A), top.g * A + under.g * (1.F - A), top.b * A + under.b * (1.F - A), 1.F};
    }

    // The opaque colour a plate's body settles at: its fill (with the fill's
    // own alpha, e.g. black at 0.9) over its OPAQUE ring colour.
    inline SRGBA settledBody(const SRGBA& fill, const SRGBA& ring) {
        return over(fill, SRGBA{ring.r, ring.g, ring.b, 1.F});
    }

    // What the body shows over `backdrop` at fade `alpha` when drawn as
    // settledBody at that alpha (the fix): a plain opacity fade.
    inline SRGBA bodyAt(const SRGBA& fill, const SRGBA& ring, const SRGBA& backdrop, float alpha) {
        SRGBA body = settledBody(fill, ring);
        body.a     = alpha;
        return over(body, backdrop);
    }

    // What the body showed before the fix: the ring rect at `alpha` under the
    // fill at fill.a * alpha. Kept for the test's control.
    inline SRGBA bodyAtRingUnder(const SRGBA& fill, const SRGBA& ring, const SRGBA& backdrop, float alpha) {
        const SRGBA UNDER = over(SRGBA{ring.r, ring.g, ring.b, alpha}, backdrop);
        return over(SRGBA{fill.r, fill.g, fill.b, fill.a * alpha}, UNDER);
    }
}
