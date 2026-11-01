#pragma once

#include "globals.hpp"

// THE CANVAS ZOOM'S DRAWING (Mosaic.cpp, holding Super on the canvas). A
// window drawn at another box -- smaller, elsewhere on the screen -- without
// its client being told: its size stays what the layout gave it, only the
// compositor's current (animated) position and size move, the way a restore
// flight draws a window (FlightController.cpp). Its own TU because it needs
// the animated variables' protected flag.
namespace ScrollOverview::MosaicDrive {
    // Draw `w` at `box` (global logical) from the next frame on.
    void drawAt(const PHLWINDOW& w, const CBox& box);
    // Where the layout has it (the animated variables' goals).
    CBox layoutBox(const PHLWINDOW& w);
    // Back to where the layout has it, at once.
    void release(const PHLWINDOW& w);
    // A window still growing in (Hyprland's popin) carried by a pan: placed
    // where its open animation draws it -- centred on its new goal at the
    // size it has grown to -- without ending that animation. False when it is
    // not opening (its size not animating, or a dock flight driving it): then
    // the caller warps it as usual.
    bool followPan(const PHLWINDOW& w);
}
