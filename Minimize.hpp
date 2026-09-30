#pragma once

#include "globals.hpp"

#include <vector>

// OVERVIEW_CPP_SPEC §13.9 X24. The compositor half of hyprgnomedock's
// minimize: the dock surface does not exist yet, and this is what its icon
// clicks will drive once it does.
namespace ScrollOverview::Minimize {

bool                   isMinimized(PHLWINDOW window);
// Whether this workspace IS the stash. Scratchpad.hpp needs the question asked
// in exactly one place: every OTHER special workspace is a scratchpad, and
// confusing the two would have the dock offering to "hide" the stash, or have a
// restore drag a window out of the user's scratchpad.
bool                   isStash(const PHLWORKSPACE& workspace);

// Re-adopt windows found on the stash with no ledger entry. The ledger lives in
// plugin memory, so a plugin RELOAD wipes it while the compositor keeps every
// stashed window exactly where it was -- stranded on special:minimized, which is
// never shown, with the dock reporting "nothing is minimized" (measured
// 2026-09-12). Called once at load.
void                   adoptStashedWindows();
// `handoffFocus` false keeps the window focused after it is stashed -- the dock
// flight wants that, so the flying proxy renders the window BRIGHT (Zen and
// other focus-themed clients darken the instant they lose focus, and a minimize
// that flickers dark mid-flight is the bug this fixes; the restore flight
// already focuses first). The flight hands focus off at its landing instead,
// via handoffMinimizedFocus. A non-flight minimize passes true and hands off
// immediately (X33: a stashed window must not keep the keyboard).
bool                   minimize(PHLWINDOW window, bool handoffFocus = true);

// A WHOLE-APP GESTURE (a dock click that minimizes every window of an app).
// Held around the loop, it records each window's box before the first of them
// moves; minimize() then pins each one at ITS recorded box instead of the one
// the layout re-tiled it to after its siblings left -- which is what told the
// last terminal out that it was full screen (user, 2026-09-30).
class CGestureBoxes {
  public:
    explicit CGestureBoxes(const std::vector<PHLWINDOW>& windows);
    ~CGestureBoxes();
    CGestureBoxes(const CGestureBoxes&)            = delete;
    CGestureBoxes& operator=(const CGestureBoxes&) = delete;
};
// X33 focus handoff, deferred: give the keyboard to a window on the workspace
// the stashed window came from (or reset it). Called when a minimize FLIGHT
// lands, so the window stays focused/bright for the whole animation.
void                   handoffMinimizedFocus(PHLWINDOW window);
bool                   restore(PHLWINDOW window);
std::vector<PHLWINDOW> minimized();

// Y9/Y10. The desktop a stashed window came FROM. A minimized window's own
// m_workspace is the stash, so nothing else can answer this -- the overview
// needs it to place the window's tile, and the flight needs it to know which
// way an off-screen desktop lies. WORKSPACE_INVALID when not minimized.
WORKSPACEID            originalWorkspace(PHLWINDOW window);

// Z6. Recreate-if-reaped and SWITCH to the desktop a stashed window came from,
// so a cross-workspace restore flight lands where the user is looking instead
// of flying off the edge and snapping desktops at touchdown.
bool                   showOriginalWorkspace(PHLWINDOW window);

// Z4/first-maximize. The GLOBAL LOGICAL box the window had when it was
// minimized. The restore flight aims here: reading the stashed window itself
// gives the stash workspace's retile, which is not where the window is going.
CBox                   originalBox(PHLWINDOW window);

}
