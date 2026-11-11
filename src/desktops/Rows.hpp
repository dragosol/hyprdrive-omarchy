#pragma once

#include "globals.hpp"
#include "Row.hpp"

#include <functional>
#include <optional>
#include <string>
#include <utility>
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
// ordinary members of the row they sit in: shifted, paid back, reordered and
// compacted like any other (user, 2026-11-11: pinned, they blocked the whole
// row). A record names a desktop by number; renumbered, it would describe
// another one and restore() would send the wrong desktop back. So a guest
// Hyprdrive renumbers is ADOPTED: Omarchy drops its record by its own rule
// (renumbering) -- it belongs to this monitor's row for good. A guest
// Hyprdrive never touched keeps its record, and restore() takes it home when
// its monitor really comes back. A record whose id has no desktop (stale) is
// ignored.
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
    // An Omarchy guest: a desktop under a merged record's id (stale records,
    // with no desktop, are not).
    bool guest(WORKSPACEID id);
    // DesktopOrder::renumber applies its checked plan through this: every
    // merged record under a number a desktop leaves (it describes that
    // desktop) or gets (a stale one) is dropped by Omarchy's own rule -- its
    // workspace.created callback, which drops merged[ws.id] -- called for the
    // desktop under its old number before `apply` and under its new one
    // after. Omarchy's table and file then hold only records that still
    // describe the desktop under their id, and its next reconcile finds
    // nothing to restore or merge for them. Blocks off: just `apply`.
    void renumbering(const std::vector<std::pair<PHLWORKSPACE, WORKSPACEID>>& moves, const std::function<void()>& apply);

    // hl.plugin.hyprdrive.workspace_blocks(slots): 0 = off.
    int luaWorkspaceBlocks(lua_State* L);
    // `hyprdrive:debug rows`.
    std::string debugReport();
}
