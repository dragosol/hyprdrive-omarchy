#pragma once

// The search's half of the overview's arrow, Tab and Return keys: what they do
// while the keyboard is in the results or the search field. The overview keeps
// its guards (modifiers, Super+W, one press per overview), the tile and desktop
// zones, and every effect on itself -- damage(), the tile hover, close() -- and
// asks these for the search's part, in the same order it always ran.

#include "SearchSession.hpp"

#include <cstddef>
#include <functional>
#include <vector>

namespace OvInternal {
    struct SSearchRow;
}

namespace Search {
    // What a key handed to the search did. `handled`: the overview stops there
    // (returns true / breaks); `damage`: and repaints first.
    struct SKeyOutcome {
        bool handled = false;
        bool damage  = false;
    };

    // moveSelection's search half, ahead of its tile walk: the Result zone's
    // column walk over the full row lists, its 2D walk over the drawn hits, and
    // Down from the search field. `monitor` is the overview's: its published
    // layout is read at the moments the walk reads it.
    SKeyOutcome onArrow(CSession& S, MONITORID monitor, bool LEFT, bool RIGHT, bool UP, bool DOWN);

    // Down in the search field (the pane up, the keyboard in none of Result,
    // Tile, Desktop): into the results, on the first drawn hit or else row 0.
    // False: not entered, moveSelection("down") takes it.
    bool downIntoResults(CSession& S, MONITORID monitor);

    // Tab onto the results, if there are any: Result zone, explicit, Browsing,
    // the selection kept when it is still a row (else 0). `rowsFirst` is the
    // Desktop branch's order (the rows are asked before the pane test); without
    // it the pane test comes first and the rows only when it holds.
    bool tabIntoResults(CSession& S, bool rowsFirst);

    // tabAcrossMonitors' search stop: the results when the pane has rows, else
    // the bare search field.
    void tabEnterSearch(CSession& S, const std::vector<OvInternal::SSearchRow>& RESULTS);

    // Return. `pickable`: the search takes this Return (something typed, a
    // submenu, or the root browse walked into; the keyboard not in Tile or
    // Desktop). `index`: the row it runs, -1 for none. matchingWindows is asked
    // only where the old ladder asked it.
    struct SReturnPick {
        bool pickable = false;
        int  index    = -1;
    };
    SReturnPick returnPick(CSession& S, const std::function<size_t()>& matchingWindows);
}
