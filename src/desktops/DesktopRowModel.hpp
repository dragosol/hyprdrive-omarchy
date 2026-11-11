#pragma once

// THE ROW OF DESKTOPS the wheel walks and the desktop strip draws (user,
// 2026-11-02) -- the pure half: no Hyprland headers, so
// tests/unit/desktop_row_test.cpp runs it on the host. DesktopStrip.cpp moves
// along it; DesktopSwitch.cpp gathers the facts.
//
//   stopAtEmpty    the row ends at the FIRST EMPTY desktop past the last one
//                  with windows, as the overview's strip does: a window there
//                  makes the next empty one reachable. Off: it ends at desktop
//                  10 or the highest that exists, whichever is larger (the
//                  wheel's range before 1.9.45).
//   populatedOnly  only desktops with windows, plus that first empty one --
//                  and the desktop shown, empty or not, so the wheel can always
//                  leave it. Off: every desktop number in between too.
//
// Desktops on another monitor are never in this monitor's row: stepping onto
// one would pull the focus across.

#include <algorithm>
#include <vector>

namespace Hyprdrive::DesktopRow {

    inline constexpr int FIRST       = 1;
    inline constexpr int DEFAULT_TOP = 10;

    struct SFacts {
        std::vector<int> populated; // desktops on this monitor with windows
        std::vector<int> existing;  // desktops that exist on this monitor, empty ones included
        std::vector<int> elsewhere; // desktops on other monitors
        int              current       = FIRST;
        bool             stopAtEmpty   = true;
        bool             populatedOnly = false;
    };

    inline bool has(const std::vector<int>& v, int id) {
        return std::ranges::find(v, id) != v.end();
    }

    // The first desktop past the last one with windows that is not on another
    // monitor: where the row ends with stopAtEmpty.
    inline int firstEmpty(const SFacts& f) {
        int id = FIRST;
        for (const int P : f.populated)
            id = std::max(id, P + 1);
        while (has(f.elsewhere, id))
            ++id;
        return id;
    }

    // The row, ascending desktop ids.
    inline std::vector<int> row(const SFacts& f) {
        const int EMPTY = firstEmpty(f);
        int       top   = EMPTY;
        if (!f.stopAtEmpty) {
            top = std::max(top, DEFAULT_TOP);
            for (const int E : f.existing)
                top = std::max(top, E);
        }
        std::vector<int> out;
        const int        LAST = std::max(top, f.current);
        for (int id = FIRST; id <= LAST; ++id) {
            if (has(f.elsewhere, id))
                continue;
            if (id > top && id != f.current)
                continue;
            if (f.populatedOnly && !has(f.populated, id) && id != EMPTY && id != f.current)
                continue;
            out.push_back(id);
        }
        return out;
    }

    // One step from `id` along `row` in `dir` (+1 / -1): `id` itself when the
    // row ends there. An id outside the row (a named desktop) steps to the
    // nearest one in that direction.
    inline int neighbour(const std::vector<int>& row, int id, int dir) {
        if (dir > 0) {
            for (const int R : row)
                if (R > id)
                    return R;
        } else if (dir < 0) {
            for (auto it = row.rbegin(); it != row.rend(); ++it)
                if (*it < id)
                    return *it;
        }
        return id;
    }
}
