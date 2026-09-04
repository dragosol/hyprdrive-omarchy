#pragma once

#include "globals.hpp"

// OVERVIEW_CPP_SPEC 13.5 X7 (hover blow-up, press shrink) and X8 (app icon on
// the tile), plus the user's 2026-08-02 amendment: a close button on the
// TOP RIGHT of the hovered tile.
//
// State lives here rather than on CScrollOverview because there is exactly one
// cursor: hover is a property of the session, not of a monitor's overview.
namespace ScrollOverview::TileChrome {

// The cursor moved. Pass nullptr when it is over no tile.
// `lingerOnLeave` is the post-hover hold that keeps the close button
// reachable in empty space. False when the pointer has gone to the dock:
// that hold would keep the title over the pill.
void setHovered(const PHLWINDOW& window, bool lingerOnLeave = true);

// The primary button went down on / came up from the hovered tile. Drives X7's
// "shrink just a little bit as pressed feedback".
void setPressed(const PHLWINDOW& window, bool down);

// The overview closed. Drops every ease so the next open starts cold instead of
// replaying the state the user left behind.
void forget();

// The tile the cursor is on, or null. The caller needs it to keep the hover
// alive while the cursor sits on the close button, which pokes a few px past
// the unscaled hit area the hover test uses.
PHLWINDOW hovered();


// An ease is still running, so the caller owes another frame.
bool animating();

// The grid slot, grown by the hover ease and shrunk by the press ease, about its
// own centre. Render AND the close-button hit-test both go through this: a
// button placed off the unscaled box lands somewhere the tile is not.
CBox scaledBox(const PHLWINDOW& window, const CBox& base);

// The close button's circle, or an empty box when this tile has no button.
// Deliberately inset rather than straddling the corner: a button hanging off the
// tile would leave the tile's hit area, un-hover it, and delete itself under the
// cursor.
CBox closeButtonBox(const PHLWINDOW& window, const CBox& tileBox, float monitorScale);

// The close button's HIT area: the same geometry, but independent of the
// visibility ramp and with slop. Use this for hover keep-alive and for clicks;
// closeButtonBox() is what gets DRAWN.
//
// V2: WHILE SUPER IS HELD THIS IS THE PILL'S BOX, not the close-X's, because
// while SUPER is held the pill is what the tile is wearing. One function, so
// the hover keep-alive (the tile must not shrink away as the cursor reaches for
// a button that overhangs it) and the click hit-test cannot end up asking about
// two different shapes -- which is the exact defect X55 fixed for the close-X.
CBox closeButtonHitBox(const PHLWINDOW& window, const CBox& tileBox, float monitorScale);

// App icon (X8) + close button, drawn over an already-rendered tile.
// Hover captions are queued here and flushed by renderCaptions() AFTER the
// dock, so a title sitting under a bottom-of-card window is not buried.
//
// `alpha` is the OVERVIEW's own open/close progress. The chrome is not part of
// the tile texture, so nothing else fades it: without this the icons hold full
// opacity for the whole transition and pop out at the end of it, which is very
// visible when the overview is toggled quickly.
void render(PHLMONITOR monitor, const PHLWINDOW& window, const CBox& tileBox, float monitorScale, float alpha = 1.F);

// Window-title pills queued during render(), drawn last so they sit above the
// dock. Safe to call with nothing queued.
void renderCaptions(PHLMONITOR monitor);

}
