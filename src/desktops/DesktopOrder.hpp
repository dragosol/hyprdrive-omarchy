#pragma once

#include "globals.hpp"
#include "Row.hpp"

#include <optional>
#include <string>
#include <utility>
#include <vector>

// DESKTOPS CHANGE PLACES (user, 2026-11-05): a fullscreen app takes the desktop
// after yours and the ones after it move along a place (FullscreenDesktop.hpp);
// a desktop dragged along the overview's strip takes its new place.
//
// A desktop's place IS its number -- Super+N, the bar, the strip, the overview's
// cards all count them -- so moving one renumbers it. In place: the workspace
// keeps its windows, its layout and its arrangement (moving the windows to
// another number would rebuild every layout from scratch), only m_id (and a
// numeric name) change. Hyprland finds workspaces by comparing m_id, never
// through a map; only the monitors' private "previous workspace" stacks keep
// old numbers. Bars hear it as the
// desktops going and coming back (destroy/create IPC events) plus the shown
// desktop again; the overview rebuilds.
namespace Hyprdrive::DesktopOrder {
    // Desktops get new numbers: (desktop, its new number). The whole plan is
    // checked first (DesktopPlan::check): one that would leave two ordinary
    // desktops on one number, or one below 1, is refused whole -- nothing is
    // written -- and logged. Returns whether it was applied. Special and named
    // desktops are never touched.
    bool renumber(const std::vector<std::pair<PHLWORKSPACE, WORKSPACEID>>& moves);
    // Every desktop of `row` (a monitor's, Rows::of) at the place of `from`
    // or after moves `delta` places along the row (+1: room at `from`; -1:
    // closing the gap below `from`). A block's 10th desktop +1 goes to the
    // row's overflow, never to the next block. A `from` outside a bounded row
    // is refused. Blocks off: every ordinary desktop numbered `from` or more.
    // Same check.
    bool shift(const Row& row, WORKSPACEID from, int delta);
    // A place `at` of `row` paid back while its empty desktop `e` is still
    // there: the row's desktops after it come back a place and `e` goes to
    // the row's top (DesktopPlan::payMoves). Nothing after it: no moves,
    // true. Same check.
    bool payBack(const Row& row, WORKSPACEID at, const PHLWORKSPACE& e);
    // Two ordinary desktops on one number: the fuller (else the older) keeps
    // it, each other one gets the first free place in its own row (blocks off:
    // the next free number above the highest). Logged.
    // Returns whether anything moved.
    bool heal(const char* when);
    // NO EMPTY DESKTOPS (a display change, FullscreenDesktop.cpp): in each
    // monitor's row, the desktops holding windows take its first places in
    // their order, then each shown empty one, then any other empty one
    // (DesktopPlan::compactMoves), Omarchy's guests included; visitors and
    // provisional rows stay where they are. Blocks off: 1..N over every desktop. Logged
    // with `why`, before -> after. Returns the row -- (old number, new number)
    // of every desktop holding windows -- when it renumbered; nothing when
    // already compact or refused (logged). `only`: that one row and no other
    // (the strip's Clean Up button: the monitor it is on); nullptr: every row.
    std::optional<std::vector<std::pair<WORKSPACEID, WORKSPACEID>>> compact(const std::string& why, const Row* only = nullptr);
    // Told after every applied renumber (FullscreenDesktop keeps the places it
    // owes by number). nullptr: nobody.
    void onRenumbered(void (*hook)());
    // An ordinary, numbered desktop (not special, not named).
    bool ordinary(const PHLWORKSPACE& ws);
}
