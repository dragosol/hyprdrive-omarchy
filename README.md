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

  Hold Super over the strip and, when there are empty desktops between the
  others, a **Clean Up Empty Desktops** button appears under it, by the
  pointer. Click it and the desktops with windows close ranks on that monitor
  (`strip_cleanup_button`, on by default).

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
- **Window order** (`overview_tile_order`): tiles go by last used (the
  default), by the order the windows were opened, or laid out to match where
  the windows sit on the desktop. The order holds still while the overview is
  open.
- **Keys act on what you point at:** Super+F, Super+T and the other window
  shortcuts act on the hovered tile, and on a hovered window preview in the
  dock.
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
- **Out of the way of fullscreen apps:** by default the dock keeps away from
  fullscreen games and the screensaver, and it never takes the keyboard or the
  mouse from a game. Settings > Dock > Visibility has "Hide dock over all
  fullscreen windows" (`dock:hide_over_fullscreen`, off) and "Keep it away
  from games and the screensaver" (`dock:hide_over_games`, on). For one app,
  right-click its icon > Get Info > Hide Dock in Fullscreen.
- **Open maximized or fullscreen:** an app's right-click menu can open it
  maximized, fullscreen, or always in fullscreen. Window previews have their
  own right-click menu and a close button.
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
GNOME's planned Mosaic. Windows keep their own size and gather in a cluster in
the middle of the screen, every window leaning toward the centre. Opening a
window never resizes the others: only the newcomer may come in smaller. When
the windows no longer fit, the desktop tiles them dwindle-style (a new window
splits the focused one), and it goes back to the mosaic -- every window at the
size it had -- once they fit again, by closing windows or shrinking one.
Turn it on in the panel's Mosaic section, then Super+L cycles Dwindle ->
Scrolling -> Mosaic, and Super+arrows move the focus between mosaic windows.
Drag a window's corner to resize it: the others hold still while you drag,
and the window grows only as far as an arrangement can fit everyone at their
size -- push past that and the desktop darkens; let go there to tile. A window
that opens floating joins the mosaic (dialogs stay afloat), and an app closed
at a size you like reopens at that size. Best on large displays.

**Infinite canvas** (off by default; panel -> Mosaic): instead of tiling when
the windows no longer fit, they keep their sizes and spill past the screen,
still one tight mosaic cluster, onto a canvas you pan across. Turning it off
puts the desktop straight back in the mosaic (or its tiles). Focusing a window
off screen, or clicking one, pans it into view on its own curve (the house curve
by default, no bounce; panel -> Animation), centred, keeping the window you came from and a sliver of its
neighbours in view where there is room; the pointer moves with the canvas, so it
stays on the same window. All directions (the default) grows the canvas down
as well as across; Horizontal is one strip: swipe sideways
with three fingers, or Super+scroll, to pan, and past the first or last window
the same gesture switches desktops. Holding Super and moving the pointer
either zooms out to every window (the default) or pans (All directions: its
place on the screen is your place on the canvas). Zoomed out, hovering a window
focuses it and letting go of Super zooms back in on it. Zoomed out, drag a
window to move it, drag its corner or edge or right-drag anywhere on it to
resize it, and your screenshot key (Print, or a trackpad press bound to it)
shoots the zoomed-out view. With that choice,
hovering a window on the canvas also pans it into view.
Super+drag drops a window where you let go; push against the screen's edge while
dragging to pan. The overview's desktop strip shows the whole canvas zoomed out.

**Desktops and fullscreen**:

- **Fullscreen gets its own desktop:** a window that goes fullscreen from a
  desktop it shares moves to a desktop of its own beside it, and comes home
  when it leaves fullscreen or closes. Moving a window onto a desktop with a
  fullscreen app takes that app out of fullscreen. A plugin reload keeps
  fullscreen windows fullscreen.
- **No stray empty desktops:** when monitors, scale or layout change, the empty
  desktops in between are removed and the later ones move down.
- **Wine, Proton and Steam** windows show their real name and icon.

**Hot corner** (off by default; panel -> Overview): rest the pointer in a screen
corner to open the overview, and again to close it, with an optional ripple.
**Hold to swap** (panel -> Overview): in the overview, drag a window over another
and hold it there; let go and the two trade places.

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
make install
hyprctl plugin load ~/.local/lib/hyprdrive/hyprdrive.so
hyprctl reload
```

Enable the panel with `omarchy plugin enable dragos.hyprdrive` (or answer yes when
`plugin add` asks). To update: `omarchy plugin update dragos.hyprdrive`, then run
`make clean && make install` in that folder again and log out and back in.

### From source

```bash
git clone https://github.com/dragosol/hyprdrive-omarchy.git ~/Projects/hyprdrive-omarchy
cd ~/Projects/hyprdrive-omarchy
git checkout "$(git tag --list 'v*' --sort=-v:refname | head -1)"   # the latest release
make install
hyprctl plugin load ~/.local/lib/hyprdrive/hyprdrive.so
hyprctl reload
hyprctl configerrors
```

### Update to the latest release

`hyprdrive update` does all of this for you. It says which version the
marketplace approved and whether a newer one is still waiting for approval, and
asks before changing anything: `hyprdrive update --approved` installs the
approved version, `hyprdrive update --github` the newest release without
waiting. The settings panel shows **Update available!** beside the version when
there is one. Click it for the same choices; it can copy the command for the
approved version or install the newest one in your terminal.

By hand:

```bash
cd ~/Projects/hyprdrive-omarchy
git fetch --tags
git checkout "$(git tag --list 'v*' --sort=-v:refname | head -1)"
make clean && make install   # builds, checks and installs; the running plugin is untouched
hyprctl plugin unload ~/.local/lib/hyprdrive/hyprdrive.so
hyprctl plugin load ~/.local/lib/hyprdrive/hyprdrive.so
hyprctl reload
hyprctl configerrors
```

Build first, then reload (keep your hands off the trackpad for that second):
`make install` replaces the plugin file without disturbing the copy that is
running, so nothing needs unloading or deleting beforehand, and a failed build
leaves you with the plugin you had. If Hyprland itself was updated since you
logged in, stop after `make install` and log out and back in instead: no plugin
can be loaded into a Hyprland whose binary was replaced.

The Omarchy shell keeps running the settings panel it loaded when it started:
its own plugin reload does not pick up changed files
([omacom/omarchy#6981](https://github.com/omacom/omarchy/issues/6981)). So
`make install` restarts the shell when the panel changed, and `hyprdrive update`
does after reloading the plugin; the bar disappears for a moment. Neither
restarts it while the screen is locked: run `omarchy restart shell` later, as
after `omarchy plugin update`. A panel from this release on also says itself when
it is out of date, with a button for the restart.

After a Hyprland update the plugin may refuse to load with `[he] Version mismatch`.
Rebuild from scratch: `make clean && make install`.

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
make_preview.py --shot overview.png --icon ui/hyprdrive-icon.png \
  --name Hypr --name2 drive --accent '#7aa2f7' \
  --tagline 'Mission control,|a real dock and|Mosaic for Omarchy.' \
  --features 'overview · dock|stacks · mosaic' --shot-width 980 --out preview.png
```

## Building from a checkout

For development, on whatever is checked out:

```bash
make install
```

`make install` copies `hyprdrive.so` and `agent-run` (the helper that types an
agent prompt in, see Network use) to `~/.local/lib/hyprdrive/`, lays fragments
in `~/.config/hypr/hyprdrive/`, and adds `require("hypr.hyprdrive")` to
`~/.config/hypr/hyprland.lua` if it is not already there.

`hl.plugin.load` is parse-time only and a no-op on `hyprctl reload`, so the first
session needs the explicit `hyprctl plugin load` (or a compositor restart). After
that the fragment keeps it loaded across reboots.

Rebuilding while the plugin runs is safe: the linker and `make install` both
write a new file rather than overwriting the one the compositor has mapped, so
neither copy needs deleting first -- and deleting the installed one before a
build that then fails leaves no plugin to load at the next login.

An undefined symbol is not a link error (`-shared` never complains). `make all`
runs `scripts/symcheck.sh` for that reason. Never ship a build whose symcheck
failed.

## Network use

Apart from the settings panel's **update check** (below), Hyprdrive makes **no
network requests by default.** Three features go online, and each is off until
you turn it on. The two search tools send what you type to
a third party:

| key | sends | to |
| --- | --- | --- |
| `search:dictionary = true` | the word after `define` | `en.wiktionary.org` |
| `search:translate = true` | the text after `tr` | `api.mymemory.translated.net`, or `search:translate_endpoint` |

Set `search:translate_endpoint` to a self-hosted
[LibreTranslate](https://github.com/LibreTranslate/LibreTranslate) URL to keep
translations on your own machine. Requests run off the compositor thread via
`curl` and `jq`. Your text reaches them on standard input, never as a
command-line argument (which every local account can read in
`/proc/<pid>/cmdline`).

**Agent prompts** (`/claude fix the tests` in the search) stay off command
lines too: the prompt goes to a private file (mode 0600, in
`$XDG_RUNTIME_DIR/hyprdrive`), and `agent-run` starts the agent without it,
deletes the file, and types the prompt into the agent once its screen is up.

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

**The update check.** While the settings panel is open, at most once every 6
hours, `hyprdrive update --check` asks GitHub which release is newest
(`api.github.com/repos/dragosol/hyprdrive-omarchy/releases/latest`), which
version the marketplace published (its `registry.json` on
raw.githubusercontent.com: the exact commit the marketplace verified, asked
with the last ETag so an unchanged registry is not downloaded again), and
whether a newer version is under review (a search of the marketplace's open
issues, used only to show its link). The approved version comes only from the
registry, never from issue text. Installing it reads the registry again in full
first; nothing is ever installed from a cached answer. It sends
nothing about you or your machine, follows no redirects, waits at most 6
seconds and caps each answer's size. `hyprdrive update --never-check` turns it
off; `hyprdrive update --allow-check` turns it back on. Installing an update
also runs `git fetch` on your checkout.

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
    30-keybinds.lua   overview click submap; 3-finger swipe up; Super+scroll desktops
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
| `hotkey` | `overlay`, or `1`–`10` | Dock slot (unbound here; Super+digit is workspaces, unless the panel's Keybinds section turns dock hotkeys on) |
| `desktop` | `next`, `prev` | One desktop along the focused monitor's row (`binds:scroll_desktops_stop_at_empty` / `_populated_only`; no wrap); with the overview open, its focused desktop card |
| `show_desktop` | `toggle`, `on`, `off` | Slide the desktop's windows out of view and back (Super+D) |
| `stack` | `pin`/`unpin`/`toggle`/`open` `<path>`, `view auto\|fan\|grid <path>`, `sort name\|added\|modified\|kind <path>`, `clear` | Pin and configure dock stacks |

```lua
hl.bind("SUPER + SHIFT + D", hl.plugin.hyprdrive.overview("toggle"))
hl.bind("SUPER + H", hl.plugin.hyprdrive.minimize("window"))
```

Super+scroll is `desktop` on `SUPER + mouse_down` / `mouse_up`, in place of
Omarchy's `e+1` / `e-1` (which skip empty desktops) (`binds:scroll_desktops`,
`binds:scroll_desktops_mods`). The wheel moves the row of desktops itself: one
spring carries it, so quick notches add up without a stop, scrolling back turns
it round where it is, and several desktops slide past on a fast spin, motion
blur and all. `binds:scroll_desktops_smooth`, `_ticks`, `_sticky`,
`_stop_at_empty` and `_populated_only` shape it; `animation:desktop_row_bounce`
and `animation:desktop_jump_through` are the row's two animation switches. Once
the settings panel's Keybinds section has written 40-panel.lua it owns this bind
and 30-keybinds.lua's default steps aside.

Super+D is `show_desktop`, on by default (the panel's Keybinds section turns it
off or moves it): every window on the desktop slides off the screen, each by its
nearest edge, and a second press brings them back -- at any point, even
halfway, where they turn round on the same spring. Nothing is retiled, so they
come back to the places they left; switching desktops, opening the overview or
a new window, or picking one of them in the dock brings them back too.

Super+T, Omarchy's floating toggle, tiles a window that cannot be resized -- a
Wine login, a game in a window -- at exactly its own size, centred in its tile,
and floats it again on the next press. Dwindle used to refuse such a window and
Mosaic stretched it, which a Wine program answers with a storm of resizes
(Battle.net's login flickering white). A tile smaller than the window lets it
overflow rather than squeeze it, kept on screen while its tile is. Only that
window's tile is touched: Hyprland's `misc:size_limits_tiled` would do the
overflow but also pulls every tiled window onto the screen, which piles up the
infinite canvas, so Hyprdrive leaves it off.

Tap Super is not a bind (`tap_super`, default true). Three-finger swipe up on
the trackpad is the same overview, registered as `hl.plugin.hyprdrive.gesture`
so it follows the fingers; three-finger left/right stay workspace switching.

## What this fork dropped

- Upstream's own Quickshell settings app (and its schema, slideshow, and monitor
  countdown). Settings live in the Omarchy panel above instead.
- The matugen / illogical-impulse theme-sync path unit
- Super+1..0 dock hotkeys, which collided with Omarchy workspaces

Upstream Hyprdrive remains at `dragosol/Hyprdrive`.
