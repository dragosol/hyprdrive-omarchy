#pragma once

#include "globals.hpp"
#include "Row.hpp"

#include <optional>
#include <string>
#include <vector>

struct lua_State; // lua.h's, at global scope

// EACH MONITOR'S ROW OF DESKTOPS, live (Row.hpp has the what and why).
//
// Whether blocks are on, and how many slots a block has, comes from Lua and
// nowhere else: 00-plugin.lua calls hl.plugin.hyprdrive.workspace_blocks(n)
// with Omarchy's `slots` when its monitor-workspaces module is loaded, 0
// otherwise (at parse, on hyprland.start and on config.reloaded, so the order
// fragments are sourced in does not matter). Omarchy's state files on disk
// decide nothing on their own: a machine with the files but no Omarchy
// running them (fedora's test user) keeps the implicit row.
//
// Which block each monitor has: Omarchy's $XDG_STATE_HOME/omarchy/
// monitor-workspaces (RowsFile.hpp), re-read when it is written (inotify on
// the compositor's loop, no polling), on monitor.added / monitor.removed and on
// config.reloaded. Omarchy writes it before it renumbers anything into a
// block, so it is never behind the ids. A monitor not in it yet gets the block
// of the desktop it shows, PROVISIONAL: no compaction there until the file
// says.
//
// The guests Omarchy merged (monitor-workspaces-merged, same watcher) are
// PINNED: Hyprdrive never renumbers them (DesktopPlan::check), or Omarchy's
// restore() would send the wrong desktop back to their monitor.
//
// This file is the only place that knows Omarchy exists; everything else asks
// for a Row.
namespace Hyprdrive::Rows {
    void init();
    void shutdown();

    // Blocks are on: some monitor's row is bounded.
    bool blocks();
    // The monitor's row (blocks off, or no monitor: the implicit row).
    Row of(const PHLMONITOR& monitor);
    // The row of the monitor the desktop is on.
    Row ofWorkspace(const PHLWORKSPACE& ws);
    // The row of the connected monitor whose row holds `id` (blocks off, or
    // nobody's: the implicit row).
    Row containing(WORKSPACEID id);
    // What a strip cell says for this desktop: its place in its monitor's row;
    // a visitor (outside it), Omarchy's own slotOf; blocks off, the number.
    std::string label(const PHLWORKSPACE& ws);
    // The ordinary desktops in `row` (on a monitor with that row, numbered in
    // it), by place.
    std::vector<PHLWORKSPACE> members(const Row& row);
    // The lowest id in the monitor's row with no desktop, or with an empty one
    // on this monitor. (Rows never fill up: overflow.)
    WORKSPACEID firstEmpty(const PHLMONITOR& monitor);
    // An Omarchy guest: a merged record's id.
    bool pinned(WORKSPACEID id);

    // hl.plugin.hyprdrive.workspace_blocks(slots): 0 = off.
    int luaWorkspaceBlocks(lua_State* L);
    // `hyprdrive:debug rows`.
    std::string debugReport();
}
