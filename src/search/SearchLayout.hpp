#pragma once

// What the search render step publishes for one monitor: the layout, its row
// hits, and the result drag aimed at them.
// Cut from SearchSession.hpp by pure code motion.

#include "globals.hpp"
#include "SearchField.hpp"

#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/helpers/time/Time.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// OVERVIEW_CPP_SPEC §13.4 G12. A result row as it was last DRAWN. Same contract
// as the dock's own menu rows: a row's box is only known inside the render pass,
// so the hit-test answers against the last frame -- which is the frame the user
// was looking at when they pressed.
struct SSearchRowHit {
    CBox        box;
    std::string desktopId; // "" for a provider row: those are not draggable
    std::string icon;
    // X62. Which result this box belongs to. The hit-test existed only to
    // reject drops before; answering the mouse means the hit has to say WHICH
    // result was hit, in the same index space the keyboard selection uses.
    int         index    = -1;
    int         column   = 0;
    int         rowInCol = 0;
};

// G12. A result drag: press, HOLD, then move. The hold is the gate and the
// distance is not, so a click can never turn into a drag no matter how far the
// hand wobbles inside it.
struct SResultDrag {
    std::string     desktopId;
    std::string     icon;
    Vector2D        start;
    Time::steady_tp startedAt;
    bool            live = false;
    int             index = -1; // X62: the row the PRESS landed on
};

inline constexpr float G12_HOLD_MS = 200.F;

// What the search render step PUBLISHED on the frame it last drew.
//
// Everything here is an output of layout, consumed by somebody who is not the
// draw code: the grid and the strip read the two heights to place themselves,
// and BOTH input devices read the same `hits` -- the keyboard walk in
// moveSelection() and the pointer hit-test in searchRowAt() -- so they cannot
// disagree about where a row is.
//
// FRAME ORDER, which is a contract and not an accident: the search step runs
// AFTER the tiles and the strip in the same frame, so the heights they read are
// the ones published on the PREVIOUS frame. `bandH` is additionally re-published
// by the band animation's update callback between frames, which is what keeps a
// hit-test taken mid-ease honest. Do not "fix" the lag by moving the publish
// earlier: that shifts the grid's climb one frame ahead of the pill it is
// following, which is a pixel change to X50's travel.
struct SSearchLayout {
    // X21/X23/X60: the results band, EASED. The panel is part of the layout
    // stack, not an overlay -- a growing list pushes the strip down and shrinks
    // the grid instead of covering them.
    float bandH = 0.F;

    // User, 2026-08-06: "place search results higher so that there isn't an
    // awkward space between search bar and search results". The row is the full
    // 64px while the pill sits in it, and shrinks as the pill docks away under
    // the bar, so the results, the strip and the tiles all climb into the space
    // the pill left. Written from the pill's ACTUAL landed foot rather than from
    // the dock progress, because the floor is the pill itself: collapsing the
    // row outright puts the panel's head ABOVE the docked pill's foot and the
    // two overlap.
    float rowL = OVERVIEW_SEARCH_ROWHL;

    // THE STRIP PLATE'S FOOT, measured from the top of its reserved band.
    //
    // The band is a RESERVATION (strip_height); the plate drawn in it is only
    // as tall as the cells came out, and StripLayout centres those cells in the
    // band whenever the row had to shrink to fit the monitor's width. Read the
    // band instead and the grid is placed against a floor the strip never
    // reached -- leftover reservation that renders as a hole between the strip
    // and the windows, growing with the desktop count (5 desktops on a 1440px
    // monitor shrink by 0.4%, 8 shrink by 38%).
    //
    // Published as a HEIGHT, not an absolute y, on purpose: an absolute would
    // have to be read against the same frame's search row and results band, and
    // the publish contract above hands those over one frame late. A height only
    // moves when the cells do.
    float stripPlateL = 0.F;

    // How many window previews the grid was showing when this was published.
    // The strip's leftover ceiling and the results row budget are both RATIOS
    // against this: with no previews on screen there is nothing to protect and
    // both are free to take the room.
    size_t gridTiles = 0;

    // LOGICAL px the grid is pushed down by so a desktop label drawn UNDER the
    // strip (results panel open) has the same gap to the windows as to the
    // plate (user, 2026-09-23). Written by the strip label each frame, read by
    // overviewStripStackL; the grid's own reflow animation does the easing.
    float gridNudgeL = 0.F;
    // ...and how far the grid is actually moved this frame: gridNudgeL eased.
    // The grid is TRANSLATED by it, not re-laid-out around it (user, 2026-09-24:
    // "instead of squeezing down ... just gently move the entire set down out of
    // the way, just like the search bar moves out of the way").
    float gridShiftL = 0.F;

    // G12/X62: the rows and grid cells as drawn, in draw order.
    std::vector<SSearchRowHit> hits;

    // ---- the rest is THIS frame's geometry, handed straight to the draw pass --
    //
    // Rendered px unless a name ends in L. Nothing here is re-derived while
    // drawing: layoutSearch() decides every box once, so a "just nudge it in the
    // renderer" edit cannot move the pixels away from the boxes the hit-test and
    // the keyboard walk are answering against.

    // The pill survived its own cull -- there is a frame to paint. False leaves
    // the two heights above still valid (they carry from the last frame that
    // did get this far), which is what the culled-pill path has always done.
    bool  drawable = false;
    // Whose frame this layout is. The live field's springs are per monitor, and
    // the draw that measures the text has to hand the width back to the right one.
    MONITORID monitorId = MONITOR_INVALID;

    float fade  = 0.F; // the overview's open progress; every element rides it
    float scale = 1.F; // pill.width / its logical width: the card transform, MEASURED
    // decoration:rounding / rounding_power, same source as workspace cards.
    int   rounding      = 0;
    float roundingPower = 2.F;
    bool  queried = false, emptyResult = false;
    // Nothing typed: the pill shows the muted placeholder. NOT !queried -- a
    // submenu or the root browse has the results up with an empty query.
    bool  placeholder = true;

    CBox  pill, badge;
    // The pill BEFORE the strip label's nudge, and how far it may be lifted
    // (to just under the bar). The label sizes its gaps off these, never off
    // `pill`: `pill` already carries the lift, and measuring the lift against
    // itself is a loop -- once the ceiling capped it, the wanted lift grew by a
    // frame's worth every frame (measured: 331 -> 347 -> 365 px and climbing).
    CBox  pillRest;
    float pillMaxUp = 0.F;
    float fieldW = 0.F; // the text field, rendered px
    float gap    = 0.F; // badge -> field, rendered px
    // The field's left edge with NO pixel snapping, in the same rendered px.
    // The pill is centred, so every pixel the field grows moves this edge half a
    // pixel -- and the snapped edge therefore flipped a whole pixel every other
    // frame while the field expanded, carrying the entire glyph run with it.
    // The chrome still snaps (a blurry pill edge is worse than a still one);
    // the TYPE is positioned from here and each glyph snaps on its own, so the
    // run flows instead of shivering as one block (user, 2026-09-19).
    float fieldXExact = 0.F;

    // What the field says: the placeholder, the query, or X20's "no matches"
    // suffix. Decided with the rest of the layout so the draw pass has no
    // opinion about the query at all.
    std::string label;

    // X61/X62: the selection AS THE PANEL RESOLVED IT. The ranking can shrink
    // under a selection made against a longer list; the clamp belongs to the
    // frame that noticed, and is written back by the publish step.
    int   selection = -1;

    // One drawn result -- a G5 grid cell or a list row, they differ only in
    // which boxes the card put them at.
    struct SItem {
        size_t      hit = 0; // index into hits: this item's box, in draw order
        std::string name, description, icon, iconFont;
        bool        selected = false;
    };

    // X49: one rounded card per provider.
    struct SCard {
        CBox               box;
        // X64/user 2026-08-06: a card fades in by how much room the band has
        // actually made for it, so the reveal pays for its own overhang and
        // needs no timer and no second animation to keep in step with the band.
        float              alpha = 1.F;
        bool               grid  = false; // G5: apps are a GRID of cells, providers a LIST of rows
        std::string        title, icon;   // G6's header; empty on a grid card
        size_t             more  = 0;     // G6's "%d more"
        std::string        emptyHint;     // shown when items is empty ("Searching…", "No files")
        std::vector<SItem> items;
    };
    struct SInspector {
        CBox        box;
        float       alpha = 1.F;
        bool        visible = false;
        std::string title;
        std::string subtitle;
        std::string icon;
        std::string category;
        std::string detail1;
        std::string detail2;
        std::string detail3;
        std::string shortcutHint;
        std::string shortcutHint2;
        std::string status;
        bool        canQuickLook   = false;
        bool        running        = false;
        bool        canNewInstance = false;
    } inspector;

    // THE TOOL PANE: a `define` / `tr` query draws this across the whole
    // container instead of cards and an inspector.
    struct SToolPane {
        bool        active = false;
        uint8_t     kind   = 0; // 1 dictionary, 2 translator
        CBox        header;
        std::string title, subtitle;
        struct SEntry {
            size_t      hit = 0; // into hits
            std::string tag, text, example;
            bool        status   = false;
            bool        selected = false;
        };
        std::vector<SEntry> entries;
    } tool;

    CBox              containerBox;
    std::vector<CBox> dividers;
    std::vector<SCard> cards;
};
