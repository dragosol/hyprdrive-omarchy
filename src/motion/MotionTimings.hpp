#pragma once

// SHARED MOTION TIMINGS: numbers that more than one module animates with, held
// once so the copies cannot drift. Each site keeps its own name for them (an
// alias of these), its own clock and its own curve; only the number is shared.

namespace Hyprdrive::Motion {

    // The house label fade: GNOME's dash label does not appear, it fades
    // (`_showLabel` over DASH_ITEM_LABEL_SHOW_TIME). In on the house entrance
    // curve, out on the exit curve ~25% quicker (the house rule). Used by the
    // dock tooltip and window peek (Dock.cpp TOOLTIP_FADE_MS / _OUT_MS), the
    // overview strip cell's label plate (STRIPTIP_FADE_MS / _OUT_MS) and the
    // canvas's Super-held window titles (MosaicGestures.cpp LABEL_FADE_MS /
    // _OUT_MS) -- three copies of the same two numbers before.
    inline constexpr float LABEL_FADE_IN_MS  = 150.F;
    inline constexpr float LABEL_FADE_OUT_MS = 110.F;

    // Y8's hop (Dock::hopLift): a landing / launch bounce, up on the entrance
    // curve for the first HOP_UP_SHARE of HOP_MS, down on the exit curve, to
    // HOP_HEIGHT of an icon. An app icon and a stack cell hop alike.
    inline constexpr float HOP_MS       = 420.F;
    inline constexpr float HOP_HEIGHT   = 0.34F; // of one icon height
    inline constexpr float HOP_UP_SHARE = 0.42F; // the hop is quicker up than down
}
