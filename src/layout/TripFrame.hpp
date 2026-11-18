#pragma once
#include "globals.hpp"

// A TRIP NEVER SHOWS AN UNDRAWN PICTURE (TripFrameRule.hpp has the why and
// the release rule). FullscreenDesktop's trips call hold() right before they
// move the window -- while its old buffer is still the one on screen (the
// trip's sizes wait for that move: TripSizes.hpp) -- and from then on, until
// the client has drawn at its new size, the window's own surface is drawn
// from that snapshot, fitted into the box the trip moves it through: its
// border, shadow and place are Hyprland's as ever, only the picture inside
// is the last good one. While its box changes size, the picture is drawn
// stretched into it all the way -- live while that is safe, the snapshot
// only between its first commit at the new size and its real frame, then a
// fade back to live (TripFrameRule.hpp, "THE PICTURE STRETCHES").
namespace Hyprdrive::TripFrame {
    void hold(const PHLWINDOW& w);
    bool holding(const PHLWINDOW& w);
    // render.pre, after the trip has placed its windows for this frame: the
    // holds looked at again, and a held window drawn live redrawn each frame.
    void preRender(const PHLMONITOR& monitor);
    void init();
    void shutdown();
}
