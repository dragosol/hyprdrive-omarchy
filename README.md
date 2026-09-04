# Hyprdrive for Omarchy

A mission-control overview and a full dock for [Hyprland](https://hyprland.org),
forked to sit on [Omarchy](https://omarchy.org/) without a settings app and
without a second theme pipeline.

> Private development. Built against Hyprland **0.56.2** (Omarchy 4).

This is a fork of [Hyprdrive](https://github.com/dragosol/Hyprdrive). Upstream
is the original plugin; this repo is the Omarchy adaptation.

## What it does

**Overview** (tap Super):

- Mission-control grid of real compositor-rendered windows.
- Type-to-search: installed apps, open windows, and GNOME search providers.
- Workspace strip. Drag a tile onto a strip cell and the window teleports there.
- Scroll-wheel workspace stepping, trackpad follow, keyboard navigation.

**Dock**:

- Dash-to-Dock behaviour counted over the desktop you are on: launch, focus,
  cycle, whole-app minimize/restore, favorites, running dots, badges, autohide.
- Minimize/restore flies into the icon. Hover an icon for live window previews.
- Stacks: pin a folder, open it as a fan or grid, Space for Nautilus quick look.

`SUPER+SPACE` stays the Omarchy menu. `SUPER+1..0` stay workspace focus. A Super
*tap* (press and release, no other key) toggles the overview.

## Theming

Appearance is Hyprland's. The plugin does not ship colours, rounding, or blur of
its own. Unset plugin keys (alpha 0 / `-1`) read the compositor's live values,
and every surface goes through the accessors in `Config.hpp`:

| what you see | Hyprland key | accessor |
| --- | --- | --- |
| selection, hover, running dots | `general:col.active_border` | `highlightColor()` |
| overview cards, dock pill corners | `decoration:rounding` | `themeRounding()` |
| dock / overview frost | `decoration:blur:*` | `-1` follow |
| workspace-card shadows | `decoration:shadow:*` | `-1` follow |
| dock pill hue | `misc:background_color` | `dockBackgroundColor()` |

Omarchy already writes the border colour from the theme accent and reloads
Hyprland on `omarchy theme set`. Squared windows (`decoration:rounding = 0`)
stay squared; a theme that rounds windows rounds the overview and the dock too.
Blur off means frost off — that is inheritance, not a missing feature.

Do not pin `plugin:hyprdrive:col_highlight` (or dock colours, or rounding) in a
fragment unless you want that value frozen across theme changes.

## Build and install

```bash
cd ~/Projects/hyprdrive-omarchy
make all
make install
hyprctl plugin load ~/.local/lib/hyprdrive/hyprdrive.so
hyprctl reload
hyprctl configerrors
```

`make install` copies `hyprdrive.so` to `~/.local/lib/hyprdrive/`, lays fragments
in `~/.config/hypr/hyprdrive/`, and adds `require("hypr.hyprdrive")` to
`~/.config/hypr/hyprland.lua` if it is not already there.

`hl.plugin.load` is parse-time only and a no-op on `hyprctl reload`, so the first
session needs the explicit `hyprctl plugin load` (or a compositor restart). After
that the fragment keeps it loaded across reboots.

If the compositor has `hyprdrive.so` mapped, delete it before rebuilding:

```bash
rm -f hyprdrive.so ~/.local/lib/hyprdrive/hyprdrive.so
make all && make install
```

An undefined symbol is not a link error (`-shared` never complains). `make all`
runs `scripts/symcheck.sh` for that reason. Never ship a build whose symcheck
failed.

## Configuration

All keys: **[docs/CONFIG.md](docs/CONFIG.md)** (generated from `Config.cpp`).

Fragments in `~/.config/hypr/hyprdrive/` load in filename order. Each has a
machine-owned half and a raw-snippet block; hand edits go below `BEGIN
raw-snippet` so they survive reinstall.

```
~/.config/hypr/hyprdrive/
    00-plugin.lua     loads hyprdrive.so, defines hyprdrive_loaded()
    10-theme.lua      documents inheritance; pins nothing
    20-layout.lua     scale, layout, workspace gap; dock.hot_keys = false
    25-rules.lua      float+center the Nautilus previewer
    30-keybinds.lua   overview click submap only
```

Lua's last assignment wins, so a line in the raw block overrides the generated
one above it.

### Dispatchers

| dispatcher | argument | what it does |
| --- | --- | --- |
| `overview` | `toggle`/`on`/`off`/`select` | Open, close, or commit the overview |
| `navigate` | `left`, `right`, `up`, `down` | Move the selection |
| `window` | `select`, `close`, `click` | Act on the window under the cursor |
| `minimize` | `window`, `restore` | Minimize into the dock, or restore |
| `hotkey` | `overlay`, or `1`–`10` | Dock slot (unbound here; Super+digit is workspaces) |
| `stack` | `pin`/`unpin`/`toggle` `<path>` | Pin a file or folder as a dock stack |

```lua
hl.bind("SUPER + SHIFT + D", hl.plugin.hyprdrive.overview("toggle"))
hl.bind("SUPER + H", hl.plugin.hyprdrive.minimize("window"))
```

Tap Super is not a bind (`tap_super`, default true).

## What this fork dropped

- The Quickshell settings app (and its schema, slideshow, and monitor countdown)
- The matugen / illogical-impulse theme-sync path unit
- Super+1..0 dock hotkeys, which collided with Omarchy workspaces

Upstream Hyprdrive remains at `dragosol/Hyprdrive`.
