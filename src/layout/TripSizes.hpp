#pragma once

// A FULLSCREEN TRIP SENDS ITS WINDOW ONE SIZE EACH WAY (user, 2026-11-11:
// "battle.net struggling exiting in and out of fullscreen - animation is
// buggy"; the recording showed white and black frames, the launcher small on
// its new desktop and then snapping to fullscreen).
//
// A trip (FullscreenDesktop.cpp) is decided a pass after the fullscreen change,
// and its move goes through Hyprland's moveWindowToWorkspace, which takes a
// fullscreen window out of fullscreen for the move and puts it back after.
// Measured on the rig with an X11 client (tests/rig/xstate/slow_redraw.c): in
// 3 ms it was configured to fullscreen (the toggle), its mosaic tile on the new
// desktop, its own mosaic size and fullscreen again; coming home, to the
// lone-window tile on the trip's desktop, then its own size. A client that
// redraws late -- Wine, CEF -- draws each of those after the compositor has
// moved on: its buffer at the in-between sizes shows in the travelling box
// (the launcher small on a bare desktop), white first at each new size.
//
// So between the fullscreen change and the end of the trip's move, the
// window's sizes are held (ConfigureHold's gate) and it is sent the final one
// once. This is the pure decision: do `w`'s sizes wait for a move that is
// about to happen?
namespace Hyprdrive::TripSizes {
    struct SState {
        bool mapped      = false;
        bool carrying    = false; // the trip is moving it right now
        bool onTrip      = false; // it has a trip
        bool onTripSpace = false; // ...and is on that trip's desktop
        bool fullscreen  = false;
        bool ordinary    = false; // on an ordinary desktop (no special one)
        int  others      = 0;     // other windows on its desktop
        bool settled     = false; // fullscreen, deliberately left where it is
    };

    inline bool wait(const SState& s) {
        if (!s.mapped)
            return false;
        if (s.carrying)
            return true;
        // On a trip: out of fullscreen on its desktop, it goes home next pass.
        if (s.onTrip)
            return s.onTripSpace && !s.fullscreen;
        // Fullscreen among others, no trip yet: it leaves next pass (or is
        // settled there, and the pass releases it).
        return s.fullscreen && s.ordinary && s.others > 0 && !s.settled;
    }
}
