#pragma once

#include "globals.hpp"

// A FULLSCREEN APP GETS ITS OWN DESKTOP (user, 2026-11-05: "if I open a
// fullscreen app or go into fullscreen, it should move the app into the next
// workspace and bump every window in other workspaces until I exit. it's kinda
// how macos does it").
//
// general:fullscreen_desktop --
//   next   (default) the desktop right after the one it was on, made for it:
//          the desktops after that move along a place (DesktopOrder.hpp) and
//          come back when it is done
//   free   the first free desktop after the one it was on; nothing moves
//   stay   where it is, as Hyprland has it
// An app alone on its desktop keeps that desktop in any case: it is already
// its own.
//
// Real fullscreen only (not maximize). One mechanic (FullscreenDesktop.cpp,
// "ONE MECHANIC"): the free place after it is used (a missing number counts --
// that is what an empty desktop is), else room is made and owed back; leaving
// fullscreen sends it home, sending it elsewhere leaves it there, closing it
// ends it -- and whatever was owed is paid back. The desktops slide on the
// strip's spring (DesktopStrip::glideTo): changing your mind halfway turns the
// slide round where it is.
namespace Hyprdrive::FullscreenDesktop {
    void init();
    void shutdown();
    // render.pre, after the desktop strip's: a window on its way to or from a
    // fullscreen desktop sets off from where it was seen.
    void preRender(PHLMONITOR monitor);
    // A window on that journey: drawn wherever its stored place is (Hyprland's
    // culling, which does not count the strip's slide, would skip it).
    bool travelling(const PHLWINDOW& w, const PHLMONITOR& monitor);
    // Desktop `ws` belongs to a fullscreen app (the overview's cards say so).
    bool isFullscreenDesktop(const PHLWORKSPACE& ws);
    // `ws` was a fullscreen app's desktop a moment ago (its trip just ended):
    // the overview shows it going back under the desktop the app returns to.
    bool justLeft(const PHLWORKSPACE& ws);
    // An app went fullscreen onto `ws` a moment ago (its trip just started):
    // only then does the overview show the card coming out from under. Merely
    // scrolling to a fullscreen app's desktop is a plain pan.
    bool justEntered(const PHLWORKSPACE& ws);
    // `w` is being carried, right now, out of its home to its fullscreen
    // desktop (`home` false) or back home at the end of that trip (`home`
    // true): Mosaic puts it back where it stood (MosaicTripHold.hpp).
    bool carrying(const PHLWINDOW& w, bool home);
}
