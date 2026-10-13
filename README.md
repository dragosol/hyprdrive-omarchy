# Hyprdrive for Omarchy

A mission-control overview, a full dock with stacks, and Mosaic, a hybrid
layout, for [Hyprland](https://hyprland.org) on [Omarchy](https://omarchy.org/).
Everything is set from a settings panel in Omarchy's own bar, and it follows
your Omarchy theme instead of shipping one of its own.

![Tap Super: every window on every desktop, in one overview](https://hyprdrive.shastasia.com/readme/overview.gif)

[Website](https://hyprdrive.shastasia.com) ·
[Demo film on YouTube](https://www.youtube.com/watch?v=RRkIViQvAOY) ·
[Releases](https://github.com/dragosol/hyprdrive-omarchy/releases)

> Built against Hyprland **0.56.2** (Omarchy 4).

## What it does

**Overview** (tap Super):

- Mission-control grid of real compositor-rendered windows.
- Type-to-search: installed apps, open windows, and GNOME search providers.

  ![Just start typing: apps, windows and files in one search](https://hyprdrive.shastasia.com/readme/search.gif)

- Workspace strip. Drag a tile onto a strip cell and the window teleports there,
  including an empty desktop, to give it one of its own.

  ![Drag a window to a new desktop](https://hyprdrive.shastasia.com/readme/new-desktop.gif)

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
- Minimize/restore flies into the icon, motion-blurred (`dock:minimize_motion_blur`). Hover an icon for live window previews.
- Windows the layout moves or resizes (retiles, the scrolling tape, opening, the scratchpad) are motion-blurred too (`layout_motion_blur`).
- With autohide off, tiled windows under the dock shorten to clear it (`dock:windows_make_room`).
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

  ![A Downloads stack fanning out, with a Space preview](https://hyprdrive.shastasia.com/readme/stacks.gif)

**Mosaic** (experimental): a third layout beside Dwindle and Scrolling, after
GNOME's planned Mosaic. Windows keep their natural size and gather in a centred
cluster that re-arranges itself as windows open and close; when they no longer
fit, the desktop tiles them, and it goes back to the mosaic once they fit again.
Turn it on in the panel's Mosaic section, then Super+L cycles Dwindle ->
Scrolling -> Mosaic. Drag a window's corner to resize it, and an app closed at
a size you like reopens at that size. Best on large displays.

**Dock icons** (settings panel -> Icons): give any app on the dock a different
icon. Pick an app, then search your icon theme, folders of your own (a
downloaded icon pack, say) or, if you switch it on, the
[selfh.st](https://selfh.st/icons) collection of about 3,000 app logos. The
picture is copied into `~/.local/share/hyprdrive/icons`, so tidying the folder
later does not take the dock's icon with it. Reset, or Restore defaults for
every app, puts the app's own icon back. The Trash has its own group with one
icon per state -- empty or full, on a dark dock or a light one -- and the dock
shows whichever matches. The choices are kept in
`~/.local/state/hyprdrive/icons`, one `desktop-id<TAB>icon` per line.

**Settings panel** (`dragos.hyprdrive`, in Omarchy's bar): every option in one
place -- one animation curve and speed for everything, per-animation motion
blur, a trackpad gesture editor, dock and overview options, Mosaic, and
performance and AC/battery profiles. "Send feedback" at the bottom opens a short
GitHub form with your version filled in. The same keys are plain Lua fragments
in `~/.config/hypr/hyprdrive/` if you would rather edit files.

**Search tools** (opt-in, see below):

- `define <word>` turns the results panel into a dictionary (Wiktionary).
- `tr <lang> <text>` / `tr en:fr <text>` / `translate <text>` turns it into a
  translator (MyMemory, or your own LibreTranslate). Enter copies a result.

`SUPER+SPACE` stays the Omarchy menu. `SUPER+1..0` stay workspace focus. A Super
*tap* (press and release, no other key) toggles the overview.

## Theming

![Change the Omarchy theme and Hyprdrive follows](https://hyprdrive.shastasia.com/readme/themes.gif)

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

## Install

Install the **latest release**, not the tip of the branch: the branch can be
ahead of the last release with work that has not been released yet. Each release
is a `vX.Y.Z` tag ([Releases](https://github.com/dragosol/hyprdrive-omarchy/releases)),
and the commands below check out the newest one.

The plugin is compiled against the Hyprland on your machine (the headers come
with the `hyprland` package), so it is built locally rather than downloaded.

**Needs:** Omarchy on Hyprland 0.56, and the build tools (`base-devel`, `pkgconf`;
everything else it links against ships with Omarchy). Optional: `ffmpeg` for the
search easter egg, GNOME Sushi for Space-to-preview in dock stacks.

### From the Omarchy plugin marketplace

The marketplace installs the settings panel; the compositor plugin is then built
from that same checkout -- this is the manual step.

```bash
omarchy plugin add https://github.com/dragosol/hyprdrive-omarchy
cd ~/.config/omarchy/plugins/dragos.hyprdrive
make all
make install
hyprctl plugin load ~/.local/lib/hyprdrive/hyprdrive.so
hyprctl reload
```

Enable the panel with `omarchy plugin enable dragos.hyprdrive` (or answer yes when
`plugin add` asks). To update: `omarchy plugin update dragos.hyprdrive`, then run
`make all && make install` in that folder again and log out and back in.

### From source

```bash
git clone https://github.com/dragosol/hyprdrive-omarchy.git ~/Projects/hyprdrive-omarchy
cd ~/Projects/hyprdrive-omarchy
git checkout "$(git tag --list 'v*' --sort=-v:refname | head -1)"   # the latest release
make all
make install
hyprctl plugin load ~/.local/lib/hyprdrive/hyprdrive.so
hyprctl reload
hyprctl configerrors
```

### Update to the latest release

```bash
cd ~/Projects/hyprdrive-omarchy
git fetch --tags
git checkout "$(git tag --list 'v*' --sort=-v:refname | head -1)"
rm -f hyprdrive.so ~/.local/lib/hyprdrive/hyprdrive.so
make all && make install
omarchy restart shell
```

Then log out and back in to load the new plugin (or, with your hands off the
trackpad, `hyprctl plugin unload ~/.local/lib/hyprdrive/hyprdrive.so && hyprctl
plugin load ~/.local/lib/hyprdrive/hyprdrive.so`). `omarchy restart shell` makes
the Hyprdrive settings panel pick up rows a release added; the shell's own plugin
reload keeps the old ones.

After a Hyprland update the plugin may refuse to load with `[he] Version mismatch`.
Rebuild from scratch: `make clean && make all && make install`.

## Removal

From the checkout you installed from (for a marketplace install,
`~/.config/omarchy/plugins/dragos.hyprdrive`):

```bash
make uninstall
omarchy plugin remove dragos.hyprdrive   # marketplace installs only
```

`make uninstall` removes the plugin (`~/.local/lib/hyprdrive`), the loader and the
`require("hypr.hyprdrive")` line it added to `~/.config/hypr/hyprland.lua` (a backup
is kept beside it), and a settings panel `make install` copied. The fragments in
`~/.config/hypr/hyprdrive/` are moved aside rather than deleted, because they hold
your own raw-snippet edits. Your saved state (`~/.local/state/hyprdrive`: favourites,
stacks, Mosaic sizes) is kept; delete it by hand to start fresh. Log out and back in
to unload the running plugin.

## The marketplace card

`preview.png` is built with the marketplace publishing skill's generator, from
the overview frame of the Hyprdrive film (`C00-v3t1` at 6.5 s, full 2880×1800) and the panel icon:

```bash
make_preview.py --shot overview.png --icon omarchy-plugin/hyprdrive-icon.png \
  --name Hypr --name2 drive --accent '#7aa2f7' \
  --tagline 'Mission control,|a real dock and|Mosaic for Omarchy.' \
  --features 'overview · dock|stacks · mosaic' --shot-width 980 --out preview.png
```

## Building from a checkout

For development, on whatever is checked out:

```bash
make all
make install
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

Hyprdrive makes **no network requests by default.** Three features go online,
and each is off until you turn it on. The two search tools send what you type to
a third party:

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

**selfh.st icons** (settings panel -> Icons, off by default) searches the
[selfh.st/icons](https://selfh.st/icons) collection for a dock app's new icon.
While it is on, the panel downloads the collection's index from
`cdn.jsdelivr.net` once a day and searches it on your machine, so your search
text is never sent. Previews of the matching logos load from the same CDN,
which can therefore see which logos were shown. Picking one downloads that one
SVG or PNG, refused if it is not a picture or an SVG points outside itself.
Nothing about your apps is sent. The collection is CC BY 4.0.

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

- Upstream's own Quickshell settings app (and its schema, slideshow, and monitor
  countdown). Settings live in the Omarchy panel above instead.
- The matugen / illogical-impulse theme-sync path unit
- Super+1..0 dock hotkeys, which collided with Omarchy workspaces

Upstream Hyprdrive remains at `dragosol/Hyprdrive`.
