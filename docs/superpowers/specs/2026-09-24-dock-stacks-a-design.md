# Dock Stacks, sub-project A: the stack experience

Date: 2026-09-24
Status: approved in conversation, awaiting written-spec review

## Context

hyprdrive already has dock stacks (`DockStacks.cpp/.hpp`, "V11 V4"): a folder
pinned to the dock as a cell, opening into a fan or a grid, with per-stack view
and sort, a Space-bar Sushi preview, a context menu (view, sort, open, unpin),
drag of an item out of an open stack onto another stack, and pinning through a
dispatcher and a Nautilus script. The goal is macOS 27-style stacks.

The work is split into three sub-projects, each with its own spec, plan and
build:

- **A — the stack experience** (this spec): fan and Space preview, the
  searchable overflow pane, the dock cell, the context menu, stack order and
  the default Downloads stack.
- **B — dropping into the dock**: files dragged from Nautilus / the desktop /
  a browser onto a stack (moved into the folder) or onto Trash (trashed), and a
  folder dragged onto the dock to pin it, with the dock parting ("Moses") to
  show where it will land.
- **C — icons**: high-fidelity empty-stack and Trash artwork of our own,
  inspired by macOS but not copied.

A is built first; B and C follow.

### Correction recorded for B

`DockStacks.hpp` states that a plugin cannot receive a drag from another
client. That is too strong. Seeing a drag is public
(`PROTO::data->dndActive()` plus the pointer — the scratchpad exit zone uses
it). Receiving it needs the drag's data source, which lives in the private
`CWLDataDeviceProtocol::m_dnd.currentSource`; once reached (a compile-time
private-member access, common in plugins), `IDataSource::mimes()` and
`send(mime, fd)` are public, so the plugin can read `text/uri-list`
asynchronously, cancel the drag, and act itself. B starts with a spike that
drops a file from Nautilus onto the Trash cell to prove the ordering (the
client must still write the list after the cancel). The header comment is
corrected as part of B.

## Principles

- **Theming is adopted, never hard-coded.** Every plate is drawn through the
  shared `OverviewRender::themePlate`: frosted only if Hyprland blur is on,
  tinted with the Omarchy theme's dock colour, Hyprland's rounding and
  rounding power; text in the theme's foreground / muted / heading / accent.
- **The compositor never waits on the disk or a client.** Folder reads, thumbs
  and file operations run off the compositor thread or through GIO async.
- **Nothing previews on hover alone.** Sushi opens only on Space.
- **Everything a module arms, it removes at unload** (timers, watchers,
  threads, Sushi) — unload must stay crash-free.

## Structure

`DockStacks.cpp` (1330 lines) stays the owner: the store, pin/unpin, open and
close, hit-testing, and hand-off. New units, each with one job:

| Unit | Job | Depends on |
|---|---|---|
| `StackModel` | Read a folder into entries; sort (Name, Date Added, Date Modified, Kind); filter by a search string; worker thread + generation counter | GIO, std only; **no Hyprland** (host-testable) |
| `StackFan` | Fan layout and the *fits* decision (arc against monitor height and the top bar) | layout geometry |
| `StackPane` | Searchable grid: layout, scrolling, selection, header | `StackModel`, `TextInput` |
| `TextInput` | Typing logic (keys, Backspace, compose, repeat) moved out of the overview search so both use one copy | xkb |
| `StackIcon` | The dock cell: Stack pile or Folder, cached texture | thumbnails, `StackModel` |

The Sushi hand-off stays in `DockStacks.cpp`, extended with a "previewing
item" that the fan and pane draw enlarged.

## Dock order and the default stack

- Dock order becomes: apps | Scratchpad | **stacks** (in the user's order) |
  removable drives | Trash (last). Stacks used to come after Trash.
- Stacks can be dragged to reorder among themselves, using the dock's existing
  gap ("part the water") animation. The order is stored.
- On first run with no store, `~/Downloads` is pinned and a `seeded` flag is
  written. Removing Downloads keeps it removed; the flag prevents re-seeding.
- "Unpin from Dock" is renamed **Remove from Dock**; it never touches the
  folder.

## The fan

- A click on a stack cell asks `StackFan` whether the fan fits: the fan is
  the existing vertical, slightly curving, tilting column of rows; it fits
  when every row stays below the top bar with a comfortable margin (one row
  height) on the stack's monitor, and the stack's view is not "Always Grid".
  Otherwise the pane opens. The fixed `FAN_MAX = 12` cap is removed.
- Rows follow the stack's sort, first item nearest the dock. Sort keys:
  **Name** (natural, case-insensitive); **Date Added** = the file's birth
  time (`statx` btime), falling back to its change time where the filesystem
  has none — Linux keeps no "added to this folder" date; **Date Modified**
  (mtime); **Kind** = GIO content type, then name. Newest first for the date
  keys.
- **Pile → fan:** the 4 items drawn on the dock cell lift off the pile,
  straighten, and settle into the first 4 rows while the rest of the fan fans
  out behind them; closing reverses it, the rows falling back onto the pile.
- Hover gives the existing subtle row highlight; nothing opens.
- **Space** on the hovered row opens Sushi beside the fan (on the side with
  more room, level with the item). The previewed row is drawn enlarged
  ~1.15× with an accent ring for as long as its preview is up. Space on
  another row moves the preview there; Space on the same row closes it.
  Closing the fan closes Sushi.
- Keyboard: arrows move the highlight, Space previews, Enter opens, Esc closes
  the preview first, then the fan.
- Dragging an item out of the fan is unchanged.
- Sushi is a normal client; "beside the fan" is a move of the Sushi window when it
  maps (Hyprland's own close gesture moves windows with the public
  `window->move()`). If the placement is ignored it opens
  centred — acceptable.

## The pane

- Opens instead of the fan when the fan does not fit, and always for a stack
  set to Always Grid.
- A theme plate grown out of the stack's icon (the existing grid entrance):
  up to 6 columns, at most ~60% of the monitor width and ~55% of its height.
- **Search field** at the top, focused on open: typing filters by filename
  (case-insensitive substring), Backspace edits, Esc clears. Uses
  `TextInput`, so repeat and compose behave as in the overview search. The
  keyboard is captured while the pane is open the same way the overview
  captures it.
- Cells: 48 px icon (a thumbnail for images/videos when the thumbnail cache
  already has one), filename wrapped to two lines. Wheel/touchpad scroll with
  eased stops; the selection is kept in view.
- Header: the stack's label, "12 of 340" (matches of total), and the sort key,
  clickable to change it.
- Keyboard: arrows move the selection, Space previews (same enlarge + ring as
  the fan), Enter opens, Esc clears the search, then closes.
- Dragging a cell out works as from the fan.
- Closes on a click outside, Esc with an empty search, or a click on the stack
  icon. The pane holds the dock revealed, as the popup does today.
- **Large folders:** the 500-entry cap is replaced by a background read of up
  to 5 000 entries. The pane shows "Loading…" until the first listing lands,
  and "showing first 5 000" past the limit.

## The dock cell

- **Display as Stack** (default): the 4 newest items as a slightly messy pile
  — each at its own small angle and offset, the newest on top — using real
  thumbnails for images/videos and themed icons otherwise, inside a thin
  rounded box outline in the theme's muted colour.
- **Display as Folder:** the folder's themed icon.
- **Empty stack:** the theme's folder icon inside the same outline, until C
  supplies the empty-stack artwork.
- An inotify watch on the folder, on the compositor's event loop (Hyprland
  never iterates GLib's main context, so a GFileMonitor would never fire),
  refreshes the cell within ~1 s of a
  change. The pile is drawn from cached per-item textures (no per-frame
  decode or file access).

## The context menu

In the app menus' Omarchy style (glyph column, hints, separators, danger
colour), grouped:

1. **Recent items** — the 5 newest, glyph + relative-time hint ("2h ago");
   click opens.
2. **Sort by** Name / Date Added / Date Modified / Kind (checked).
3. **Display as** Stack / Folder (checked).
4. **View as** Automatic / Always Grid (checked).
5. **Open in Files**, **Show in Files** (parent folder, this one selected).
6. **Rename in Dock…** — the menu becomes a one-line text field
   (`TextInput`); changes the dock label only, never the folder.
7. **Move Newest to Trash**, **Empty Folder…** (danger colour). Empty
   Folder… turns the menu in place into "Move 42 items to Trash?" with
   Cancel / Move — the same in-place swap Get Info uses. Both trash, never
   delete.
8. **Remove from Dock.**

## Data

- The stacks store becomes **v2**: per stack `path`, `order`, `sort`
  (`name|added|modified|kind`), `display` (`stack|folder`), `view`
  (`auto|grid`), `label` (empty = basename); plus a global `seeded` flag.
- v1 files are read and upgraded on the next write (`grid` → `view=grid`,
  `sortByTime` → `sort=modified`).
- Writes are atomic: temp file + rename.

## Errors

- A folder that no longer exists keeps its cell, dimmed, with a "Folder not
  found" hint; a click shakes the cell; Remove from Dock still works.
- File operations use GIO. A failure shakes the stack's cell and shows the
  error briefly in its tooltip. Empty Folder… trashes item by item; a failure
  stops it and reports how many were moved.
- No Sushi installed: Space does nothing and the pane header hints "Install
  sushi for previews".

## Testing

- New host test suite `scripts/stack_model_test.cpp` (added to
  `host_tests.sh`): each sort key; search filtering (case, unicode, empty);
  v1 → v2 store upgrade; the Downloads seed rule (first run, after removal);
  the fan-fit decision across monitor heights 720–2160 with and without a top
  bar.
- Live checks with the computer tool and screenshots: fan, pile → fan, Space
  preview with the enlarged row, pane with search, menu (confirm and rename
  views).
- `symcheck`, `keycheck` and the full `make all` before every commit.

## Out of scope for A

- Receiving drops from other apps, drop-to-Trash and drop-to-pin (B).
- New empty-stack and Trash artwork (C).
- A List view.
