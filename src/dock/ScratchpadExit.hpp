#pragma once

#include "globals.hpp"

// THE WAY OUT OF THE SCRATCHPAD MID-DRAG (user, 2026-09-22: "if I start
// dragging something from scratchpad (e.g. a file) I should have an omarchy
// style popup zone that says something like 'drag here to exit scratchpad'
// which lets me easily drag the file out of scratchpad and wherever i want on
// the active desktop").
//
// A scratchpad covers the desktop, so a file dragged out of a window on it has
// nowhere to be dropped: everything the user can see is the scratchpad. While a
// Wayland drag is in flight over a shown scratchpad, a plate appears at the top
// of the screen; carrying the drag onto it hides the scratchpad. The drag itself
// is untouched -- it is the client's -- so it carries on over the desktop that
// was underneath and drops wherever it is released.
namespace ScrollOverview::ScratchpadExit {

    // Draw the zone on `monitor` if a drag is live over its shown scratchpad.
    // Desktop only: called from the dock's render hook, which the overview
    // bypasses.
    void render(PHLMONITOR monitor);

    // The pointer moved over `monitor` (null when it is off every monitor or an
    // overview owns it). Keeps the zone repainting through the drag and hides
    // the scratchpad when the drag reaches it.
    void onPointerMove(PHLMONITOR monitor);

    // Plugin unload: remove the drift timer (it would fire into unloaded code)
    // and free the stripe textures. Needs the GL context current.
    void shutdown();

}
