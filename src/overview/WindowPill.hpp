#pragma once

#include "DockInput.hpp" // SClaim -- the pill answers on the dock's ladder, not a third one

// V11_SPEC V2 (user, 2026-08-17). Hold SUPER and every window grows a pill:
// three buttons -- minimize, maximize, close -- on a frosted capsule STRADDLING
// the window's top-right corner, "not entirely over the window... but not
// entirely outside of it". Release SUPER and it leaves.
//
// ONE module for BOTH contexts, because it is one surface: the same capsule,
// the same three actions, the same reveal. Only two things differ, and both are
// arguments rather than a second implementation -- the BOX it hangs off (a real
// window's on-screen rect on the desktop, a tile's grid slot in the overview)
// and the AUDIENCE (every visible window of the active desktop out there, the
// hovered tile alone in here, where it stands in for X55's close-X).
//
// IT OWNS NO INPUT PATH. Both hooks ask it through DockInput::SClaim, and the
// answer is deliberately narrow: a BUTTON claims, the capsule around the buttons
// does not. That is the whole of what keeps Hyprland's own
// `SUPER + mouse:272 -> window.drag` bindm working with the pill on screen. The
// plugin's button listener runs BEFORE the compositor's bind processing
// (CInputManager::onMouseButton emits the bus event first and returns on
// info.cancelled), so a claim is a veto over the bind and a pass-through leaves
// the drag exactly as it was before this existed.
namespace ScrollOverview::WindowPill {

enum class EButton : uint8_t {
    None = 0, // on the capsule, but not on a button -- NOT a claim
    Minimize,
    Maximize,
    Close,
};

// SUPER's edges, fed from the raw key stream in main.cpp. Raw keycodes rather
// than xkb, for the reason isGuardModifierKeycode already records: xkb was
// measured answering logo=0 for the second key of a SUPER chord in this VM.
void  setSuperHeld(bool held);
bool  superHeld();

// The reveal, 0 (gone) to 1 (out). EVERYTHING the pill draws and hit-tests
// rides this one number, so the thing on screen and the thing you can click can
// never disagree about whether the pill is there.
float progress();

// The ease is still running, so the caller owes another frame. Dock::needsFrames
// asks this: the pill animates on a desktop where nothing else is moving.
bool  animating();

// The capsule, monitor-local RENDER px, straddling `frame`'s top-right corner.
// Clamped into the monitor HERE, once, so the draw and the hit test cannot
// disagree about where it ended up.
CBox    box(PHLMONITOR monitor, const CBox& frame, float scale);

// The button a monitor-local point is on. None both for "on the capsule between
// buttons" and for "nowhere near"; the caller does not need to tell those apart,
// because neither one is a claim.
EButton buttonAt(PHLMONITOR monitor, const CBox& frame, float scale, const Vector2D& point);

// Draw it over `frame`. `alpha` is the caller's own fade -- the overview passes
// its open progress -- and the reveal ease is multiplied in here.
void    render(PHLMONITOR monitor, const CBox& frame, float scale, float alpha = 1.F);

// Run a button on a window. The one place the three actions live, so the desktop
// and the overview cannot drift into doing different things with the same glyph.
void    activate(PHLMONITOR monitor, const PHLWINDOW& window, EButton button);

// ---- the plain desktop ----------------------------------------------------

// The desktop's primary button, in one call: the pill's own buttons, and the
// double-click that toggles a window between floating and tiled. Everything
// else is passed through untouched -- see the note at the top about the bindm.
DockInput::SClaim handleDesktop(PHLMONITOR monitor, const Vector2D& local, uint32_t button, bool pressed);

// Draw a pill on every visible window of `monitor`'s active desktop. Called from
// the RENDER_POST_WINDOWS stage beside the dock, so the pills sit above every
// window and below the bar -- and, being one stage for all of them, above every
// OTHER window too, which is what stops a pill being buried by the window in
// front of the one it belongs to.
void renderDesktop(PHLMONITOR monitor);

}
