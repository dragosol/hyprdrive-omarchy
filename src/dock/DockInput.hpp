#pragma once

#include "Dock.hpp" // PHLMONITOR + Vector2D (via globals.hpp) and the ladder API

// One dock pointer protocol for BOTH the plain desktop and the overview. The two
// hooks used to paste the same menu>launch>panel>pill>cancel ladder and drifted
// apart every time one was touched (the "must not behave differently" comments
// that littered both). The ladder lives here now; each hook resolves its own
// monitor, pill-local point and button ROLE, then forwards. Every place the two
// modes genuinely differ is a parameter, not a second copy.
namespace ScrollOverview::DockInput {

// The role the caller's own button mapping assigned. Desktop hardcodes
// (right=Menu, middle=Launch, left=Main); the overview honours the left-handed
// config (resize-button=Menu, middle=Launch, main-button=Main).
enum class EButton { Menu, Launch, Main, Other };

// claimed    -> the caller swallows this event and returns; it was the dock's.
// needsFrame -> overview only: damage()+requestInputFrame() after a claim whose
//               visual result nothing else will ask a frame for (the drag
//               snap-back). Desktop draws from render.pre and ignores it.
struct SClaim {
    bool claimed    = false;
    bool needsFrame = false;
};

// The menu and the middle-launch: the parts the overview must decide BEFORE it
// offers a release to a dragged search result (they sit ahead of that in its
// button handler). `menuMissLetsThrough` is the desktop fail-safe -- a press that
// misses an open menu closes it and is LET THROUGH, so the same press still acts
// and can never be swallowed into a menu with no drawn geometry to hit-test
// (measured 2026-08-06). The overview always draws, so it swallows the miss.
SClaim handleMenu(PHLMONITOR monitor, const Vector2D& local, EButton role, bool pressed, bool windowDragActive, bool menuMissLetsThrough);

// The pill's own gestures, asked in order: the hover panel (it floats above the
// pill, so a press over a preview must not fall through to the tile) > the pill
// (press remembers, motion reorders, release decides drag-vs-click) > a drag
// released OFF the pill is a cancel. `windowDragActive` keeps the dock out of a
// window drag's release: that release belongs to the drag (X26), not the dock.
SClaim handlePill(PHLMONITOR monitor, const Vector2D& local, EButton role, bool pressed, bool windowDragActive);

}
