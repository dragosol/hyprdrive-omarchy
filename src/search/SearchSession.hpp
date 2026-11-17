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
#include "MotionClock.hpp"

#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/helpers/time/Time.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "SearchNav.hpp"

#include "SearchField.hpp"

#include "SearchLayout.hpp"

namespace Search {

// THE search session. One instance (SearchSession.cpp, reached through
// searchSession() below); there is exactly one keyboard and one cursor, so this
// is per-session state and not per-monitor.
struct CSession {
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

        // The dock's Show Windows (user, 2026-09-24): the query was put there
        // for the user, not typed -- it filters the windows and shows no results
        // panel. Any edit clears it; the first typed key REPLACES the query, the
        // way typing over a selected field does, and Backspace drops it whole.
        bool        windowsOnly = false;

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
        ESearchIntent    intent             = ESearchIntent::Typing;
        // Result index the pointer last MOVED onto. -1 when it is not over a
        // row. Updated only from motion, never from layout: results appearing
        // under a parked cursor must not steal SPACE from the query.
        int              hoverIndex         = -1;

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
        // Set by activateSearchResult when Enter should switch to a live window
        // rather than spawn. close() reads it the same way a tile click does.
        PHLWINDOWREF     pendingFocus;
    } nav;

    // ---- what the pointer is in the middle of ---------------------------
    struct SPointer {
        std::optional<SResultDrag> drag;
        // The press landed on the search pill or the results pane: its
        // RELEASE is the search's too. Left to the grid it ran the default
        // click -- select the window under the cursor and close -- so a click
        // on the pill that opened the results (search:browse_on_click) shut
        // the overview as the pane slid under the pointer.
        bool uiPress = false;
    } pointer;

    // ---- what the last drawn frame published ----------------------------
    // PER MONITOR. Every monitor's overview lays the search out in its own pixels
    // and publishes it every frame; with one shared copy the pointer's hit tests
    // on one monitor read whichever monitor had drawn last -- the other screen's
    // rows and pill -- and next frame's layout carried the other monitor's band.
    std::unordered_map<MONITORID, SSearchLayout> layouts;

    SSearchLayout& layoutOn(MONITORID monitor) {
        return layouts[monitor];
    }

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
        for (auto& [_, L] : layouts)
            L.hits.clear();
    }

    // A brand-new overview starts with an unpushed band and a pill at rest: a
    // previous overview's last published layout must not open the next one
    // already docked.
    void resetLayout(MONITORID monitor) {
        layouts.erase(monitor);
        fields.erase(monitor);
    }

    // The live field's springs -- see SSearchFieldMotion -- ONE PER MONITOR
    // (user, 2026-09-14: "glitching in the search results when I type longer
    // sentences"). Every monitor's overview draws its own pill, and they shared
    // one set: each frame the laptop aimed the springs at ITS cap (75% of 1440)
    // and ITS measurement of the text, the external at its own, so the field on
    // the external never grew and the scroll flipped between two targets -- the
    // text shivering sideways a few pixels a frame.
    std::unordered_map<MONITORID, SSearchFieldMotion> fields;

    SSearchFieldMotion& fieldOn(MONITORID monitor) {
        return fields[monitor];
    }
};

} // namespace Search

// The one session, plugin-wide: both outputs' overviews share it, exactly as
// they share the keyboard. Defined in SearchSession.cpp.
Search::CSession& searchSession();

// G12/X62. The result row under a point, or nullptr. The pointer's half of "one
// hit list, two devices"; moveSelection() walks the same vector.
inline const SSearchRowHit* searchRowAt(MONITORID monitor, const Vector2D& point) {
    auto& S = searchSession();
    for (const auto& ROW : S.layoutOn(monitor).hits) {
        if (ROW.box.containsPoint(point))
            return &ROW;
    }
    return nullptr;
}
