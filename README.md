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
- Several monitors act as one: the overview opens and closes on all of them,
  and Tab walks search -> each monitor's grid (or strip -> grid on each
  monitor when no results are up). Picking a window moves the cursor into it
  and releases a game's pointer lock.
- On a scrolling-layout desktop, the 3-finger swipe scrolls its windows first
  and carries on into the next/previous desktop past either end (4 fingers
  still jump desktops directly).
- Super/Alt/Ctrl+click a dock app that is not running (or "Open in Empty
  Workspace" in its menu) opens it on an empty desktop.
- **Smooth frames** (Hyprdrive panel -> Textures, `smooth_frames`): lets
  Hyprland render ahead when the GPU runs late, so animations drop a refresh
  instead of halving the frame rate. hyprdrive suggests it once if the overview
  keeps dropping frames.

**Dock**:

- Dash-to-Dock behaviour counted over the desktop you are on: launch, focus,
  cycle, whole-app minimize/restore, favorites, running dots, badges, autohide.
- Minimize/restore flies into the icon. Hover an icon for live window previews.
- Stacks, macOS style: a pinned folder shows its 4 newest files as a pile;
  click it and they fan out, or it opens as a searchable pane when the fan
  would not fit. Space previews the hovered item (GNOME Sushi), arrows and
  Enter work, and the right-click menu has recent items, sort, Display as
  Stack/Folder, View as Grid, Rename in Dock, Trash actions and Remove from
  Dock. Downloads is pinned on first run; drag stacks to reorder them. Drop
  files from any file manager onto a stack to move them in (copied across
  drives, never overwriting) or onto Trash to trash them; drag a folder onto
  the dock and it parts to make room -- let go to pin it there. Trash and
  empty stacks use hyprdrive's own artwork (`dock:builtin_icons = false` for
  the icon theme's). Files drag out of a fan into any app as a real drag;
  images, videos and PDFs show real previews; a download in progress shows a
  bar, and a stack bounces when something new arrives (a hidden dock peeks the
  pile up from the edge). The
  store is `~/.local/state/hyprdrive/stacks`; removing a stack never touches
  the folder.

**Search tools** (opt-in, see below):

- `define <word>` turns the results panel into a dictionary (Wiktionary).
- `tr <lang> <text>` / `tr en:fr <text>` / `translate <text>` turns it into a
  translator (MyMemory, or your own LibreTranslate). Enter copies a result.

`SUPER+SPACE` stays the Omarchy menu. `SUPER+1..0` stay workspace focus. A Super
*tap* (press and release, no other key) toggles the overview.

## Theming

Appearance is Hyprland's. The plugin does not ship colours, rounding, or blur of
its own. Unset plugin keys (alpha 0 / `-1`) read the compositor's live values,
and every surface goes through the accessors in `Config.hpp`:

| what you see | Hyprland key | accessor |
| --- | --- | --- |
| selection, hover, running dots | `general:col.active_border` | `highlightColor()` |
| overview cards | `decoration:rounding` | `themeRounding()` |
| dock pill and dock highlights | `plugin:hyprdrive:dock:corner_radius` (24) | `dockCornerRadius()` |
| dock / overview frost | `decoration:blur:*` | `-1` follow |
| workspace-card shadows | `decoration:shadow:*` | `-1` follow |
| dock / search / strip / results fill | Omarchy terminal background (`colors.toml`), else `misc:background_color` | `dockBackgroundColor()` |

Omarchy already writes the border colour from the theme accent and reloads
Hyprland on `omarchy theme set`. Squared windows (`decoration:rounding = 0`)
stay squared in the overview; the dock keeps its original Dash-to-Dock pill
radius of 24, including hover and selection highlights. Blur off means frost
off — that is inheritance, not a missing feature.

Do not pin `plugin:hyprdrive:col_highlight` (or dock colours) in a fragment
unless you want that value frozen across theme changes.

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

## Network use

Hyprdrive makes **no network requests by default.** The two search tools are
off until you turn them on, because they send what you type to a third party:

| key | sends | to |
| --- | --- | --- |
| `search:dictionary = true` | the word after `define` | `en.wiktionary.org` |
| `search:translate = true` | the text after `tr` | `api.mymemory.translated.net`, or `search:translate_endpoint` |

Set `search:translate_endpoint` to a self-hosted
[LibreTranslate](https://github.com/LibreTranslate/LibreTranslate) URL to keep
translations on your own machine. Requests run off the compositor thread via
`curl` and `jq`, with your text passed as an argument, never through a shell.

```lua
hl.config({ plugin = { hyprdrive = { search = { dictionary = true, translate = true } } } })
```

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
    30-keybinds.lua   overview click submap; 3-finger swipe up
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
| `stack` | `pin`/`unpin`/`toggle`/`open` `<path>`, `view auto\|fan\|grid <path>`, `sort name\|added\|modified\|kind <path>`, `clear` | Pin and configure dock stacks |

```lua
hl.bind("SUPER + SHIFT + D", hl.plugin.hyprdrive.overview("toggle"))
hl.bind("SUPER + H", hl.plugin.hyprdrive.minimize("window"))
```

Tap Super is not a bind (`tap_super`, default true). Three-finger swipe up on
the trackpad is the same overview, registered as `hl.plugin.hyprdrive.gesture`
so it follows the fingers; three-finger left/right stay workspace switching.

## What this fork dropped

- The Quickshell settings app (and its schema, slideshow, and monitor countdown)
- The matugen / illogical-impulse theme-sync path unit
- Super+1..0 dock hotkeys, which collided with Omarchy workspaces

Upstream Hyprdrive remains at `dragosol/Hyprdrive`.
