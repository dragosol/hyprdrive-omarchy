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
