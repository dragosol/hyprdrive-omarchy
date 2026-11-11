#pragma once

#include "globals.hpp"

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
    // Every ordinary desktop numbered `from` or more moves `delta` places
    // (+1: room at `from`; -1: closing the gap below `from`). Same check.
    bool shift(WORKSPACEID from, int delta);
    // Two ordinary desktops on one number: the fuller (else the older) keeps
    // it, each other one gets the next free number above the highest. Logged.
    // Returns whether anything moved.
    bool heal(const char* when);
    // Told after every applied renumber (FullscreenDesktop keeps the places it
    // owes by number). nullptr: nobody.
    void onRenumbered(void (*hook)());
    // An ordinary, numbered desktop (not special, not named).
    bool ordinary(const PHLWORKSPACE& ws);
}
