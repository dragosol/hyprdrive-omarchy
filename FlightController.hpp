#pragma once

// OVERVIEW_CPP_SPEC 13.12 X54 and the whole Z4-Z20 saga, lifted out of Dock.cpp.
// A "flight" is the animation of a window shrinking into its dock icon
// (minimize) or growing back out of it (restore).
//
// This module owns the flight STATE MACHINE: what flights exist, what phase
// each is in, when one is armed, reversed, landed or aborted, and the landing
// guarantees every one of those paths has to honour. It does NOT own a second
// render path -- the loop that advances flights still draws them INLINE, in the
// dock's own render pass, through Dock::drawFlightSurface. That is deliberate,
// and it is the whole reason this extraction is landing on the second attempt:
// the first one split the loop into a mutate-everything tick() plus a
// draw-everything consumer, which compiled, passed z4/z4c/z6/z7, and
// deterministically broke z5 -- three reversals in a row fired against flights
// that had never advanced a frame, because separating the mutation from the
// draw moved when the dock damages itself relative to when its flights advance.
// The state machine is the point; the tick/draw split was never the point.
//
// The phases replace what used to be sentinels (-1 alphas, empty boxes, a
// landedFrames counter that meant "flying" at -1), because every bug in the
// Z-series was a misreading of which state a flight was in.

#include "globals.hpp"

#include <hyprland/src/helpers/time/Time.hpp>  // Time::steady_tp: the frame clock a flight draw is stamped with

#include <optional>
#include <string>

namespace ScrollOverview::Dock {
struct SLayout;
}

namespace ScrollOverview::Flight {

// FROZEN AT CREATION (Z18: re-deriving it per frame flipped a flight between
// proxy and driven when the overview closed mid-flight).
//   MinimizeProxy  window -> icon, drawn as a proxy surface
//   RestoreProxy   icon -> window, proxy (overview path: the real window is hidden)
//   RestoreDriven  icon -> window, the REAL window driven natively (desktop path;
//                  Z17: no proxy, so no proxy/real hand-off seam)
enum class EFlightKind { MinimizeProxy, RestoreProxy, RestoreDriven };

// The lifetime, explicitly:
//   Armed          created; no frame drawn yet. A reversal here has nothing to
//                  be continuous WITH, which used to be spelled `lastAlpha < 0`
//                  and used to make reverse() bail -- the strand 5623364 killed.
//   Flying         at least one frame drawn; `drawn` is the seed a reversal
//                  continues from.
//   LandingBridge  the restore's held frames (Z4: restore-then-erase in one
//                  frame left a frame with neither proxy nor real window on
//                  screen). The proxy holds its landing box while the real
//                  window works its way back through moveWindowToWorkspace +
//                  retile + damage.
//   Done           landed or aborted, landing sanity run, about to be erased.
enum class EFlightPhase { Armed, Flying, LandingBridge, Done };

const char* phaseName(EFlightPhase phase);
const char* kindName(EFlightKind kind);

// X54 / Y8. Minimize / restore WITH the shrink-into-the-icon flight. Everything
// that minimizes must come through these -- the dispatcher went straight to
// Minimize:: once and a keybind minimize never animated at all. `bounceOnLand`
// hops the icon (Z10: at the DROP, which is why a drag asks for it and a click
// does not).
// `startBox` (monitor-local render px): where the window is SEEN when the
// minimize starts, if that is not its tile or real box -- a window dropped on
// the dock leaves from where it was let go, not from its old grid slot.
void minimize(PHLMONITOR monitor, PHLWINDOW window, bool bounceOnLand = false, std::optional<CBox> startBox = std::nullopt);
void restore(PHLMONITOR monitor, PHLWINDOW window);
// Re-aim every desktop restore flight at its window's CURRENT layout box: after
// Minimize::CRestoreSlots swaps windows into their own slots, the boxes the
// flights froze at click time are each other's.
void retargetRestores();


// Is the dock in the middle of anything? Autohide holds the pill out for a
// flight, and the render loop asks before deciding whether to keep asking for
// frames.
bool any();
// ...on this monitor's dock: a flight is drawn by, and holds out, exactly one.
bool anyOn(PHLMONITOR monitor);

// Does this window have a flight at all, and is that flight a RESTORE? Z5: a
// window mid-restore still reads as minimized (it stays stashed until landing),
// so the dock's click policy has to count it as visible or a spam of clicks
// re-restores forever instead of toggling.
bool hasFlight(const PHLWINDOW& window);
bool isRestoreFlight(const PHLWINDOW& window);

// Advance every flight one frame AND DRAW IT, inside the dock's render pass.
// `now` and `ownsPass` are the dock's render context, handed over for the
// duration of the loop so the drawing is bit-identical to when it lived there.
void renderFlights(PHLMONITOR monitor, const ScrollOverview::Dock::SLayout& layout, const Time::steady_tp& now, bool ownsPass);

// LAND EVERY FLIGHT WHERE IT STANDS, with the same guarantees a landing gives:
// unhidden, alpha 1, geometry at the target, focus handed off. One mechanism,
// not two -- P0's monitor-removed settle IS this call.
//
// A flight's boxes are monitor-local RENDER px of the output it started on and
// it only advances inside that output's dock render, so one left in the air
// across a monitor change is drawn against the wrong origin -- or, if its
// output was the one drawing it, never lands at all, and a restore that never
// lands leaves its window STASHED: hidden, unfocusable, gone. Same for a config
// reload that rebuilds the dock under it, and for PLUGIN_EXIT, which is the one
// path where a stranded hidden window outlives the code that could unhide it.
//
// `reason` is recorded in the transition log; the gates read it.
void abortAll(const std::string& reason);

}
