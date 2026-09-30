#pragma once

// ONE MOTION LANGUAGE (user, 2026-09-30: "drop-downs for animation curves ...
// serpantinum curves, or hyprdrive's defaults ... globally, affecting
// EVERYTHING EXCEPT DOCK STACKS").
//
// plugin:hyprdrive:animation:curve picks a preset. Every preset is three
// curves, each with one job:
//   DECEL -- an entrance or a travel: most of the distance early, then settle
//            (the overview opening, a flight, a tile gliding to its new slot).
//   ACCEL -- an exit: leave slowly enough to read as motion, then get out of
//            the way (the overview closing, a tooltip fading out).
//   POP   -- something appearing in place: a small overshoot (a dock icon
//            spawning).
//
// "hyprdrive" is the default and is EXACTLY what shipped before this existed:
// the house curves for houseEase and the overview, and each hand-rolled ease
// (the flight's expo, the autohide's quad, ...) left as it was -- sites ask
// housePreset() and keep their own function in that case. Any other preset
// replaces all of them with its three curves.
//
// The dock stacks keep their own motion (dock:stack_motion) and use
// houseFixed(), today's house curves whatever the preset.

#include <cstdint>

namespace ScrollOverview::Motion {

    enum class ERole : uint8_t {
        DECEL,
        ACCEL,
        POP,
    };

    // The default preset is active: sites keep their hand-rolled eases.
    bool housePreset();

    // The active preset's curve for `role`, t clamped to 0..1.
    float ease(ERole role, float t);

    // The house curves, whatever the preset (the dock stacks).
    float houseFixed(bool decel, float t);

    // Points Hyprland's named house beziers -- which the overview's own
    // animated variables run on -- at the active preset. Cheap when nothing
    // changed; call after registerConfig, on every config reload (a reload can
    // drop plugin beziers) and whenever the preset may have been set live.
    void sync();
}
