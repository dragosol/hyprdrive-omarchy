#pragma once
#include "globals.hpp"

// A TRIP NEVER SHOWS AN UNDRAWN PICTURE (TripFrameRule.hpp has the why and
// the release rule). FullscreenDesktop's trips call hold() right before they
// move the window -- while its old buffer is still the one on screen (the
// trip's sizes wait for that move: TripSizes.hpp) -- and from then on, until
// the client has drawn at its new size, the window's own surface is drawn
// from that snapshot, fitted into the box the trip moves it through: its
// border, shadow and place are Hyprland's as ever, only the picture inside
// is the last good one.
namespace Hyprdrive::TripFrame {
    void hold(const PHLWINDOW& w);
    bool holding(const PHLWINDOW& w);
    void init();
    void shutdown();
}
