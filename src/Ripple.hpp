#pragma once

#include "globals.hpp"

// THE HOT CORNER'S RIPPLE (user, 2026-10-13: "a ripple effect with realistic
// distortions (same as effect from my videos) whenever I hit the corner").
//
// The film's "jump ring" (hyprdrive-film site-fx-shorts/src/film/edge.ts, the
// ripple-chroma family's drop ring) ported to the compositor: one long wavelet
// radiates out from the corner across the screen and REFRACTS what is under it
// -- nothing is drawn on top, the picture itself bends -- with the film's wet-
// glass RGB split. Same constants: 640 px/s, a 30 px wavelength under a 16 px
// envelope, gone in 1 s (fade 0.28 s; the film took 0.55), its front wobbling a little; in logical px, so
// it looks the same at any scale.
//
// Drawn last in the frame (RENDER_LAST_MOMENT): the composed frame is copied
// into a buffer of its own -- only the ring's band -- and drawn back displaced,
// so it bends the desktop, the overview, the bar and the dock alike. While one
// is live its monitor is fully damaged every frame (a partial redraw would bend
// the previous frame's bend), and once more after it ends.
namespace ScrollOverview::Ripple {
    void init();
    // After the PLUGIN_EXIT pass purge: the GL program and buffer go with it.
    void shutdown();
    // Launch one from `originGlobal` (global logical px, usually a screen
    // corner) on `monitor`. A second one on the same monitor restarts it.
    void trigger(const PHLMONITOR& monitor, const Vector2D& originGlobal);
    // Pass element type name, for the PLUGIN_EXIT purge.
    inline constexpr const char* PASS_NAME = "CRipplePassElement";
}
