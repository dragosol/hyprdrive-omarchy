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
namespace ScrollOverview::DesktopOrder {
    // Desktops get new numbers: (desktop, its new number). Numbers no other
    // ordinary desktop keeps. Special and named desktops are never touched.
    void renumber(const std::vector<std::pair<PHLWORKSPACE, WORKSPACEID>>& moves);
    // Every ordinary desktop numbered `from` or more moves `delta` places
    // (+1: room at `from`; -1: closing the gap below `from`).
    void shift(WORKSPACEID from, int delta);
    // An ordinary, numbered desktop (not special, not named).
    bool ordinary(const PHLWORKSPACE& ws);
}
