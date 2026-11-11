# The source, by area

One compiled plugin (`hyprdrive.so`). Each folder is one subsystem; every
`#include` names a file bare, so a file can move between folders without an edit.

| folder | what lives there |
| --- | --- |
| `core/` | plugin entry (`main.cpp`), the settings registry (`Config`), names kept for old configs (`Compat`), small shared helpers |
| `overview/` | the overview: its window grid, cards, hit-testing, input, rendering and frame policy |
| `search/` | type-to-search: the result sources (apps, windows, files, tools, GNOME providers), the session state and the letter animation |
| `dock/` | the dock: items, layout and autohide, menu, input, live previews, minimize and scratchpad flights |
| `stacks/` | dock folders: fan and grid stacks, the file reader, thumbnails, drives and Trash |
| `apps/` | what an app is: desktop entries, icons, games and Wine windows |
| `layout/` | window management: Mosaic and its packer, own-size tiles, fullscreen desktops, show desktop |
| `desktops/` | switching and reordering desktops, and the strip that shows them |
| `motion/` | flights between two places, motion blur, the shared curves, border fades |
| `input/` | trackpad gestures: raw fingers, swipe models, the gesture driver |
| `platform/` | what differs between a plain Hyprland desktop and Omarchy: detection, theme sources, fonts, the settings app |
| `debug/` | the `hyprdrive:debug` instruments: profiler, synthetic gestures, log paths |

A header that ends in `Model`, `Math`, `Layout`, `Plan` or `Decision` is pure
(no compositor) and has a host test in `tests/unit/`.

Every Hyprland function hook is created through `core/FunctionHooks`
(`required()` for the ones the plugin cannot load without, `firstMatch()` for
the ones that only disable a feature when missing), which records each one in
creation order -- `hyprctl dispatch 'hl.plugin.hyprdrive.debug("hooks")'`
lists them. It refuses a second hook on an address already hooked. Only the
find-and-create step is shared: each module still decides when it hooks, calls
`hook()`, keeps its pointer and trampoline, and unhooks on its own schedule.

**A new animation that needs frames.** Anything that moves with no pointer
motion and no client commit left to repaint it (a fade, a spring settling, a
flight) has to say so, or the desktop stops drawing mid-way. A module outside
`dock/` adds one line to the `SOURCES` table in `core/FramePump.cpp`: the reason
name `hyprdrive:debug frames` prints, its predicate, and the slot in
`Dock::needsFramesReason`'s chain it is asked at. The dock's own terms stay in
that function. Nothing asks for a frame by damaging from inside the render pass:
`FramePump::request` (the dock's `pumpFrame`) posts it for after the pass and
coalesces it to one per output per event-loop iteration.
