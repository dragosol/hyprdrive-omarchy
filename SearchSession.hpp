#pragma once

// OVERVIEW_CPP_SPEC §5 F2/F5 + §13.2 F3 + §13.4 G1/G5/G6/G12 + §13.12 X48-X52.
//
// ONE object for the whole search state machine.
//
// Before this file the state was fourteen loose globals across two headers, and
// every open/close path hand-listed the subset it remembered: the dispatcher
// cleared six, the constructor cleared two, close() cleared two, and nothing
// cleared the rest. X61 and G12 both shipped a new piece of state and wired its
// reset into ONE of those sites -- that is the defect class this file exists to
// close. The fields are grouped, each group is reset by assigning a fresh
// group (`query = {}`), so a field added tomorrow is reset by construction and
// cannot be forgotten at any site.
//
// The three groups are not cosmetic; they are the three DIFFERENT lifetimes the
// state actually has, and mixing them is what makes a blanket reset unsafe:
//   * query/nav/pointer live for one SEARCH SESSION and are cleared when the
//     dispatcher starts a new one (reset()).
//   * layout is the render pipeline's per-frame OUTPUT, republished every frame
//     by the search render step. It is initialised for a brand-new overview
//     (resetLayout()) and must NOT be cleared by the dispatcher: the band and
//     the pill row are read by the grid, so zeroing them under a live overview
//     snaps a layout that is supposed to ease (measured as a one-frame jump of
//     the whole grid on toggle-close with results on screen).

#include "globals.hpp"

#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/helpers/time/Time.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// X61. Which ZONE the keyboard selection is in. The overview has three surfaces
// the user can land on and they keep their selection in three different places
// -- the strip in viewportCurrentWorkspace, the results in nav.selection, the
// tiles in closeOnWindow -- so the arrows need one explicit answer to "where am
// I" rather than inferring it from whichever of the three happens to be set.
// Return reads it to know what activating means, and the tile border reads it so
// a selection that has moved to the results does not leave a second highlight
// behind on the grid.
enum class EOverviewNavZone : uint8_t {
    Tile = 0,
    Result,
    Desktop,
    Search,
};

// X10: the search row above the strip. ponytail: constant 64 logical px
// (48px pill + 8px margins); make it a config knob if anyone ever asks.
inline constexpr float OVERVIEW_SEARCH_ROWHL = 64.F, OVERVIEW_SEARCH_DOCKPADL = 8.F;

// X2. The pill's own metrics, from the deprecated Quickshell overview's
// SearchBar.qml (ii/modules/ii/overview/): a 40px row + 4px margins = 48 high,
// 15px type, 22px glyph, 210px collapsed / 360px expanded. They are LOGICAL px:
// the draw pass measures the card transform off the landed box and scales every
// one of them by it, so the pill's contents follow the transform for free.
//
// The row height above and these belong together -- 4 + 40 + 4 is where the 48
// comes from, and 8 + 48 + 8 is where the 64 comes from -- which is why they
// sit here rather than with the result-card family in OverviewInternal.hpp.
inline constexpr float OVERVIEW_PILL_HL       = 48.F;  // the pill itself
inline constexpr float OVERVIEW_PILL_PADL     = 4.F;   // its leading margin, and the badge's inset
inline constexpr float OVERVIEW_PILL_BADGEL   = 40.F;  // the accent badge carrying the search glyph
inline constexpr float OVERVIEW_PILL_GAPL     = 12.F;  // badge -> text field
inline constexpr float OVERVIEW_PILL_TAILL    = 20.F;  // text field -> trailing edge
inline constexpr float OVERVIEW_PILL_FIELDL   = 210.F; // the field at rest
inline constexpr float OVERVIEW_PILL_FIELDQL  = 360.F; // ...and once a query is typed
inline constexpr float OVERVIEW_PILL_GLYPHPX  = 22.F;  // the Material Symbols glyph in the badge
inline constexpr float OVERVIEW_PILL_TEXTPX   = 15.F;  // the query / placeholder

// The full width the pill lays out to. Named because BOTH the layout and the
// scale it measures back out of the landed box divide by it, and a mismatch
// there silently rescales everything inside the pill.
inline constexpr float OVERVIEW_PILL_WL(float fieldWL) {
    return OVERVIEW_PILL_PADL + OVERVIEW_PILL_BADGEL + OVERVIEW_PILL_GAPL + fieldWL + OVERVIEW_PILL_TAILL;
}

inline float overviewPillTargetWL(bool queried) {
    return OVERVIEW_PILL_PADL + OVERVIEW_PILL_BADGEL + OVERVIEW_PILL_GAPL + (queried ? OVERVIEW_PILL_FIELDQL : OVERVIEW_PILL_FIELDL) + OVERVIEW_PILL_TAILL;
}

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

    float fade  = 0.F; // the overview's open progress; every element rides it
    float scale = 1.F; // pill.width / its logical width: the card transform, MEASURED
    // decoration:rounding / rounding_power, same source as workspace cards.
    int   rounding      = 0;
    float roundingPower = 2.F;
    bool  queried = false, emptyResult = false;

    CBox  pill, badge;
    float fieldW = 0.F; // the text field, rendered px
    float gap    = 0.F; // badge -> field, rendered px

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
        std::string name, description, icon;
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
        bool        canQuickLook = false;
    } inspector;

    std::vector<SCard> cards;
};

// THE search session. One instance, `g_search` below; there is exactly one
// keyboard and one cursor, so this is per-session state and not per-monitor.
struct SSearchSession {
    // ---- the query, and everything derived from it ----------------------
    struct SQuery {
        // OVERVIEW_CPP_SPEC §5 F2 type-to-filter query. Written by the keyboard
        // hook in main.cpp, read by the window gate.
        std::string text;

        // G1 (spec 13.11): "compose/dead keys must START a search" only means
        // something if a search can be ACTIVE while the query is still empty --
        // Compose alone types nothing. Every "is a search running" test reads
        // this, not text.empty(), which cannot represent that state.
        bool        active = false;

        // G6. How many results each provider offered, INCLUDING the ones the cap
        // and the trim never showed. Filled by overviewSearchRows(); read by the
        // section header to say "%d more".
        std::unordered_map<std::string, size_t> providerTotals;

        // X52 secondary. ~40 call sites consult the window gate, so one frame
        // asks the same question about the same window dozens of times. The
        // answer cannot change within a frame, so it is cached for the frame.
        // The key carries the QUERY as well as the frame counter, not a
        // generation number someone has to remember to bump.
        //
        // These were function-local statics of an inline function. Same single
        // instance, but now they are reset with the query that keys them instead
        // of relying on the key comparison alone.
        uint64_t                                         memoFrame = ~0ULL;
        std::string                                      memoQuery;
        std::unordered_map<Desktop::View::CWindow*, bool> memo;
    } query;

    // ---- where the keyboard selection is --------------------------------
    struct SNav {
        // OVERVIEW_CPP_SPEC §13.2 F3. Index into the ranked results, -1 meaning
        // "no result selected, the grid owns Return". Paired with the query: any
        // edit to the query resets it.
        int              selection = -1;

        // User, 2026-08-06: "make the first search result the first thing that
        // is selected, rather than the desktops". Typing ARMS that, but the
        // results are not known at the keystroke -- the providers answer over the
        // next frames -- so the keyboard hook sets this and the search render
        // step spends it once there is a result to select. Cleared by the spend,
        // so a deliberate Tab back to the grid (-1) is never overridden.
        bool             selectFirstPending = false;
        bool             explicitSelection  = false;

        EOverviewNavZone zone = EOverviewNavZone::Tile;

        // X74 (user, 2026-08-06): the ring is the POINTER's, except while the
        // keyboard is the thing actually driving. The tile zone is seeded to the
        // active window on every open, so keying the ring on the zone alone drew
        // a permanent ring around the active window that the user never asked
        // for. X61 is why this is a flag and not a deletion: arrowing between
        // tiles has to keep its visual. Set by an arrow landing on a tile,
        // cleared by the overview opening and by ANY pointer motion -- whichever
        // device moved last owns the ring.
        bool             keyboardTile = false;
    } nav;

    // ---- what the pointer is in the middle of ---------------------------
    struct SPointer {
        std::optional<SResultDrag> drag;
    } pointer;

    // ---- what the last drawn frame published ----------------------------
    SSearchLayout layout;

    // A NEW SEARCH SESSION. Everything a previous session could leak, cleared in
    // one call -- this is the whole point of the file. Deliberately NOT the
    // layout: see the note at the top of the header.
    void reset() {
        query = {};
        nav   = {};
        endInteraction();
    }

    // X32/G12. The overview can close under a held button (Escape, a keybind, a
    // window taking focus), and the release that would have ended the gesture
    // then never reaches the hook. Neither the drag nor the boxes it was aimed
    // at may survive into the next open as a phantom drop.
    void endInteraction() {
        pointer = {};
        layout.hits.clear();
    }

    // A brand-new overview starts with an unpushed band and a pill at rest: a
    // previous overview's last published layout must not open the next one
    // already docked.
    void resetLayout() { layout = {}; }
};

inline SSearchSession g_search;

// G12/X62. The result row under a point, or nullptr. The pointer's half of "one
// hit list, two devices"; moveSelection() walks the same vector.
inline const SSearchRowHit* searchRowAt(const Vector2D& point) {
    for (const auto& ROW : g_search.layout.hits) {
        if (ROW.box.containsPoint(point))
            return &ROW;
    }
    return nullptr;
}
