# Dock Stacks, sub-project B: dropping into the dock

Date: 2026-09-24
Status: user pre-approved "make and execute plans B and C"; decisions below were made on their behalf and are listed so they can be revisited.
Builds on: `2026-09-24-dock-stacks-a-design.md`

## What the user asked for

- Drop files onto a stack's dock icon → they go into that folder.
- Drop files over the Trash icon → they go to the trash.
- Drag a folder onto the dock → the dock "splits open Moses-style" until a spot
  is chosen; dropping pins the folder there as a stack.

## The spike (done, 2026-09-24, nested Hyprland)

The dock is not a Wayland surface, so a drag from another app cannot target
it through the protocol. The spike proved the plugin can take such a drop:

1. While a drag hovers the dock, read the drag's source from the private
   `CWLDataDeviceProtocol::m_dnd.currentSource` (header widened for that one
   include) and call the public `IDataSource::send("text/uri-list", fd)`; read
   the pipe on the compositor event loop. A GTK4 source answered with both file
   URIs, complete, before the button was released.
2. On release over the dock, the plugin's button hook runs before Hyprland's
   own drop listener (registered later, at drag start), calls the public
   `abortDndIfPresent()`, and cancels the event. Hyprland's signal emitter skips
   the drop listener that the abort removed. The source saw a cancel with
   `delete=False`: nothing is lost on the source side.

So B needs no helper process.

## Decisions (made for the user; revisit freely)

- **Files onto a stack cell → move** when source and folder are on the same
  filesystem, **copy** when not (macOS semantics). Never overwrite: a name that
  exists becomes `name (2).ext`, `name (3).ext`, ….
- **Files onto Trash → trash** (`g_file_trash`), never delete.
- **A single folder onto the pill between cells → pin it** at that position.
  While it hovers, a faint placeholder cell for it is inserted there and the
  dock's existing arrangement animation slides the neighbours apart — the
  "Moses" gap. Anything else over a gap (files, several folders) shows no gap
  and drops nothing.
- **A folder onto a stack cell** moves into that folder (the cell wins over the
  gap: the gap is only between cells).
- **Onto an app icon:** out of scope for B (no "open with" yet). The drop is
  cancelled and nothing happens.
- **Only on the plain desktop.** Inside the overview the overview owns drags.
- **Feedback:** the target cell gets the accent ring and a small lift while
  hovered; a good drop bounces the cell; a failure shakes it and puts the error
  in its tooltip (A's `tooltipSuffix`). The autohidden dock reveals when a drag
  reaches the bottom edge, as it does for the pointer.

## Structure

| Unit | Job |
|---|---|
| `DockDropModel.hpp` (pure, host-tested) | `parseUriList` (CRLF, comments, percent-decoding, file:// only), `uniqueName` (`a (2).txt`), `resolveTarget` (cell kind + counts → action) |
| `DockDrop.cpp/.hpp` | the live part: prefetch on hover, target tracking, release takeover, placeholder index, damage |
| `StackReader` | gains a `moveOrCopyInto(dest, paths)` job next to `trash` |
| `Dock.cpp` | the placeholder item in `buildDockItems`, the hover ring/lift, reveal during a drag |
| `DockStacks` | `pinAt(path, index)` |

The spike file `DockDropSpike.cpp` is deleted; its technique lives on in
`DockDrop.cpp`.

## Safety

- Every new pass element goes through `Stacks::drawableBox`.
- The prefetch pipe's event source and fd are removed in PLUGIN_EXIT; a drag
  that ends (abort or drop elsewhere) drops the pending read.
- No file operation on the compositor thread.
- Tested in the nested Hyprland with the GTK4 drag source first; the live
  desktop only after that.

## Testing

- `scripts/dock_drop_test.cpp`: uri-list parsing (CRLF, `#` comments,
  `%20`/UTF-8 escapes, non-file URIs skipped, trailing newline), `uniqueName`
  (no extension, dotfiles, double extensions `a.tar.gz` → `a (2).tar.gz`),
  `resolveTarget` (stack/trash/gap/app/none × files/folder/mixed).
- `scripts/stack_reader_test.cpp`: `moveOrCopyInto` on a temp dir, including a
  name conflict.
- Nested Hyprland: drag one file onto a stack (moved), onto Trash (trashed —
  using files on a real filesystem under `~/.cache/hyprdrive-test`), a folder
  onto a gap (placeholder appears, drop pins it at that index), a folder onto
  an app icon (nothing happens, source unharmed).
