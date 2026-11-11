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
    // Remove the empty desktops between, per row, once things settle -- as
    // after a display change, and on the same timer (a burst restarts it):
    // a config reload (where Omarchy reconciles its blocks), an overflow
    // desktop with room in its block (Rows.cpp).
    void compactSoon(const char* why);
    // The same, now, for `monitor`'s row alone (the overview strip's Clean Up
    // button, OverviewStripCleanup.cpp) -- with the overview up, which the
    // timed one waits out. Refused while a desktop slides, a swipe is under
    // way or the monitor's block is still a guess. True: something moved
    // (the fullscreen book follows, as after a display change).
    bool compactRow(const PHLMONITOR& monitor, const char* why);
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
    // The dock's "Always Open in" Empty Workspace + Fullscreen moves a new
    // window off `origin` to an empty desktop: closed there, fullscreen, the
    // view goes back to `origin` (FullscreenDesktop.cpp, SOrigin).
    void launchedFrom(const PHLWINDOW& w, const PHLWORKSPACE& origin);
    // PLACEMENT'S BESIDE (Placement.cpp decides; this does it). The fullscreen
    // app this book keeps on `ws` (a trip's or a settled one), or nullptr.
    PHLWINDOW fullscreenAppOn(const PHLWORKSPACE& ws);
    // At window.openEarly: `w`, opening over that app, goes to the next place
    // in the monitor's row (made if it is taken, and owed back). `prompted`:
    // the user asked for it -- the view goes there; else it goes quietly.
    // The desktop it went to, or nullptr (it stays: Hyprland's behaviour).
    PHLWORKSPACE beside(const PHLWINDOW& w, bool prompted);
    // At window.openEarly, any other window: one that OPENS on a beside
    // desktop (a dialog's own dialog, the next one right after) is that
    // link's and keeps it alive; one MOVED there makes it a desktop of its own.
    void openedOn(const PHLWINDOW& w);
}
