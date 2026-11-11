#pragma once

// WHICH DESKTOP IDS EACH MONITOR'S STRIP MAY MATERIALISE -- as a pure function.
//
// The overview keeps two GNOME invariants by creating REAL workspaces (E2 and
// X3 in Overview.cpp): the highest occupied desktop is followed by one
// empty one, and the holes below it are filled so desktop N is the Nth cell.
// Both were written for one monitor and did id arithmetic on that monitor
// alone: `highest occupied + 1`, `every missing id below it`.
//
// With a second monitor that arithmetic hands out ids the OTHER monitor owns
// (user, 2026-09-14: DP-1's spare desktop was one that exists on eDP-1).
// Hyprland's CWorkspaceStateTracker::create does not check for an existing id
// either -- it makes a SECOND workspace with the same number -- and a workspace
// rule binding that id to another output silently creates it THERE instead.
//
// So the rule lives here, where it can be tested without a compositor
// (tests/unit/workspace_plan_test.cpp, run by `make`):
//
//   1. An id that exists ANYWHERE is never created again. Only its own monitor
//      may use it.
//   2. An id a workspace rule binds to another connected monitor is never
//      created on this one.
//   3. A monitor's spare is the lowest EXISTING empty desktop above its highest
//      occupied one, if it already has one -- reused, so the answer is stable
//      from one rebuild to the next and nothing is created twice. Otherwise it
//      is the lowest id above its highest occupied one that rules 1-2 allow.
//   4. Monitors are planned in ascending monitor id, and every spare planned
//      counts as taken for the monitors after it. Every overview computes the
//      whole plan and applies only its own row, so the result does not depend
//      on which monitor's overview happens to rebuild first.
//   6. A PHANTOM -- an empty desktop an overview created to fill a hole (`held`)
//      -- still exists, so no spare may take its id, but it is not a neighbour:
//      it is planned again as a gap every rebuild it still qualifies, so the
//      overview keeps paying for it instead of dropping and re-creating it.
//   5. A hole is filled on a monitor only when the nearest existing desktops on
//      BOTH sides of it are that monitor's (or there is nothing below it). With
//      one monitor that is every hole, exactly as before. With two, an id
//      between the laptop's 3 and the external's 4 belongs to neither, and is
//      left alone rather than claimed by whichever overview looked first.
//   7. ROWS (Row.hpp, Omarchy's per-monitor blocks): each monitor plans on its
//      own row's places -- holes from the row's first place, never below it
//      (no "20 empty desktops below 21"), the spare in the row (a full block's
//      spare is its first overflow place), neighbours only within the row, and
//      desktops outside the row (visitors) are nobody's to plan around. No
//      `rows` (blocks off): every monitor has the implicit row, slot == id, and
//      rules 1-6 are exactly what they were.

#include "Row.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <vector>

namespace Hyprdrive::WorkspacePlan {

using ID = int64_t;

struct SWorkspace {
    ID     id      = 0; // > 0: named/special workspaces never enter the plan
    ID     monitor = -1;
    size_t windows = 0;
    bool   held    = false; // a phantom an overview materialised to fill a hole (rule 6)
};

struct SRow {
    std::optional<ID> spare;    // the trailing empty, or nothing when the monitor has no windows
    bool              existing = false; // the spare already exists on this monitor
    std::vector<ID>   gaps;     // ids to materialise (or keep holding) below the highest occupied
};

// The monitor a workspace rule binds `id` to, when that monitor is connected;
// nullopt when unbound. Hyprland: workspaceRuleMgr()->getBoundMonitorForWS.
using BoundMonitor = std::function<std::optional<ID>(ID)>;

// ponytail: X3's ceiling (v1.1 item 0), see ensureNoWorkspaceGaps in Overview.cpp.
inline constexpr ID GAP_CEILING = 32;

inline std::map<ID, SRow> plan(std::vector<ID> monitors, const std::vector<SWorkspace>& workspaces, const BoundMonitor& bound = {},
                               const std::map<ID, Hyprdrive::Row>& rows = {}) {
    const auto rowOf = [&](ID monitor) {
        const auto IT = rows.find(monitor);
        return IT == rows.end() ? Hyprdrive::Row{} : IT->second;
    };
    std::sort(monitors.begin(), monitors.end());
    monitors.erase(std::unique(monitors.begin(), monitors.end()), monitors.end());

    std::map<ID, ID> owner; // every existing positive id that is not a phantom -> its monitor
    std::set<ID>     taken; // every existing positive id, phantoms included
    std::map<ID, ID> phantoms;
    for (const auto& W : workspaces) {
        if (W.id <= 0)
            continue;
        taken.insert(W.id);
        if (W.held && W.windows == 0)
            phantoms.emplace(W.id, W.monitor);
        else if (!owner.contains(W.id))
            owner[W.id] = W.monitor;
    }

    const auto boundElsewhere = [&](ID id, ID monitor) {
        if (!bound)
            return false;
        const auto B = bound(id);
        return B.has_value() && *B != monitor;
    };

    std::map<ID, SRow> out;

    for (const ID MONITOR : monitors) {
        auto&                row = out[MONITOR];
        const Hyprdrive::Row R   = rowOf(MONITOR);

        // (places on the row; implicit row: place == id)
        ID maxOccupied = 0;
        for (const auto& W : workspaces) {
            if (W.id > 0 && W.monitor == MONITOR && W.windows > 0 && R.contains(W.id))
                maxOccupied = std::max<ID>(maxOccupied, R.slot(W.id));
        }

        if (maxOccupied <= 0)
            continue;

        // Rule 3, reuse. Lowest, so a second stray empty (an active desktop
        // the user jumped to, say) is not preferred over the nearer one.
        std::optional<ID> reuse;
        for (const auto& W : workspaces) {
            if (W.monitor == MONITOR && W.windows == 0 && R.contains(W.id) && R.slot(W.id) > maxOccupied && (!reuse || R.slot(W.id) < R.slot(*reuse)))
                reuse = W.id;
        }

        if (reuse) {
            row.spare    = reuse;
            row.existing = true;
        } else {
            ID place = maxOccupied + 1;
            while (R.idOf(place) > 0 && (taken.contains(R.idOf(place)) || boundElsewhere(R.idOf(place), MONITOR)))
                ++place;
            if (R.idOf(place) > 0) {
                row.spare = R.idOf(place);
                taken.insert(*row.spare); // rule 4
            }
        }
    }

    // Rule 5, after every spare is known: a spare planned for one monitor is a
    // neighbour the holes of another must respect, exactly as if it existed.
    std::map<ID, ID> neighbours = owner;
    for (const auto& [MONITOR, ROW] : out) {
        if (ROW.spare && !neighbours.contains(*ROW.spare))
            neighbours[*ROW.spare] = MONITOR;
    }

    for (const ID MONITOR : monitors) {
        auto&                row = out[MONITOR];
        const Hyprdrive::Row R   = rowOf(MONITOR);

        // Rule 7: this row's neighbours, by place. (Implicit row: by id, as before.)
        std::map<ID, ID> near;
        for (const auto& [ID_, MON] : neighbours)
            if (R.contains(ID_))
                near[R.slot(ID_)] = MON;

        ID maxOccupied = 0, maxID = 0;
        for (const auto& W : workspaces) {
            if (W.id <= 0 || W.monitor != MONITOR || !R.contains(W.id))
                continue;
            maxID = std::max<ID>(maxID, R.slot(W.id));
            if (W.windows > 0)
                maxOccupied = std::max<ID>(maxOccupied, R.slot(W.id));
        }

        // With no windows at all, the highest desktop that exists: jumping to an
        // empty desktop 5 still shows five cells (the pre-plan behaviour).
        // (The ceiling is the unbounded row's; a block's row ends by itself.)
        const ID TOP    = maxOccupied > 0 ? maxOccupied : maxID;
        const ID FILLTO = R.bounded() ? TOP : std::min(TOP, GAP_CEILING + 1);
        for (ID place = 1; place < FILLTO; ++place) {
            const ID id = R.idOf(place);
            if (near.contains(place) || boundElsewhere(id, MONITOR))
                continue;

            // Someone else's phantom: theirs to keep or release, never ours to
            // plan on top of.
            if (const auto PH = phantoms.find(id); PH != phantoms.end() && PH->second != MONITOR)
                continue;
            // (an id held by a desktop of no row here -- a visitor elsewhere -- is never planned)
            if (taken.contains(id) && !phantoms.contains(id))
                continue;

            const auto ABOVE = near.upper_bound(place); // first existing place > place
            if (ABOVE == near.end() || ABOVE->second != MONITOR)
                continue;

            const auto BELOW = near.lower_bound(place); // == ABOVE here; the one before it is below
            if (BELOW != near.begin() && std::prev(BELOW)->second != MONITOR)
                continue;

            row.gaps.push_back(id);
        }
    }

    return out;
}

} // namespace Hyprdrive::WorkspacePlan
