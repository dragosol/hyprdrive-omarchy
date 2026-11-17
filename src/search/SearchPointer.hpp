#pragma once

// The search's half of the overview's pointer: hover over the pill and the
// result rows, the G12 press -> hold -> drag of a result, and the release that
// is a click on the row it was pressed on. The overview keeps its own guards
// (a window drag or resize in flight), the dock calls (drop index, pin, unpin)
// and its effects (damage(), requestInputFrame(), close()), and asks these for
// the search's part, in the same order it always ran. `monitor` is the
// overview's: its published layout is read where the old code read it.

#include "SearchSession.hpp"

#include <cstdint>
#include <optional>

namespace Search {
    // Motion. Search hover is motion-only: results appearing under a parked
    // cursor must not steal SPACE from the query. Moving onto the pill puts
    // SPACE back to typing even if a file is still highlighted for Enter.
    // Nothing while a result drag is live. True: the hover changed (repaint).
    bool onPointerMove(CSession& S, MONITORID monitor, const Vector2D& point);

    // G12. The hold has to pass BEFORE the distance is even looked at. True
    // while a result drag (of a desktop entry) is live: the overview aims the
    // dock's drop slot at the pointer.
    bool onPointerDragMove(CSession& S, const Vector2D& point, float dragThresholdSq);

    // A release with a result drag in hand: the drag, taken out of the session.
    std::optional<SResultDrag> takeDrag(CSession& S);

    // X62. A drag that never went live was a CLICK: the row index it activates
    // when released over the row it was pressed on, else -1 (the user changing
    // their mind).
    int clickedRow(CSession& S, MONITORID monitor, const Vector2D& point, const SResultDrag& drag);

    // The press landed on the pill or the pane: its release is the search's.
    bool takeUiPress(CSession& S);

    // A press over the search UI, which floats over the grid. Row: the press
    // selects it and starts a (not yet live) G12 drag. Pill: focuses the search
    // (with nothing typed that is the root browse) -- the overview plays the
    // pill's squeeze. Pane: claims the release. None: not the search's.
    enum class EPress : uint8_t {
        None = 0,
        Row,
        Pill,
        Pane,
    };
    EPress onPointerPress(CSession& S, MONITORID monitor, const Vector2D& point);
}
