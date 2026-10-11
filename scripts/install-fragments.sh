#!/usr/bin/env bash
# Install Hyprdrive on Omarchy: the .so, the Lua fragments, and a require()
# from ~/.config/hypr/hyprland.lua.
#
# Appearance is not installed. The plugin reads Hyprland's live theme values
# (borders, rounding, blur, shadows); Omarchy already writes those on
# `omarchy theme set` and reloads the compositor.
#
#   scripts/install-fragments.sh            live install
#   scripts/install-fragments.sh DIR        fragments only, for tests
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEST=${1:-$HOME/.config/hypr/hyprdrive}
SRC=$ROOT/config/hyprdrive
LOADER_SRC=$ROOT/config/hyprdrive.lua
LOADER_DEST=$HOME/.config/hypr/hyprdrive.lua
HYPRLAND_LUA=$HOME/.config/hypr/hyprland.lua
LIBDIR=$HOME/.local/lib/hyprdrive

[ -d "$SRC" ] || { echo "install-fragments: no $SRC" >&2; exit 1; }
mkdir -p "$DEST"

for f in "$SRC"/*.lua; do
    python3 "$ROOT/scripts/fragment.py" write "$DEST/$(basename "$f")" < "$f"
done

echo "fragments installed in $DEST"

if [ $# -gt 0 ]; then
    echo "loader, plugin copy, and hyprland.lua skipped (explicit DEST)"
    exit 0
fi

install -Dm644 "$LOADER_SRC" "$LOADER_DEST"
echo "loader: $LOADER_DEST"

if [ -f "$HYPRLAND_LUA" ]; then
    if grep -q 'require("hypr.hyprdrive")' "$HYPRLAND_LUA" || grep -q "require('hypr.hyprdrive')" "$HYPRLAND_LUA"; then
        echo "hyprland.lua already requires hypr.hyprdrive"
    else
        cat >> "$HYPRLAND_LUA" <<'LUA'

-- Hyprdrive (overview + dock). Fragments live in hypr/hyprdrive/.
require("hypr.hyprdrive")
LUA
        echo "added require(\"hypr.hyprdrive\") to $HYPRLAND_LUA"
    fi
else
    echo "install-fragments: no $HYPRLAND_LUA -- add: require(\"hypr.hyprdrive\")" >&2
fi

if [ -f "$ROOT/hyprdrive.so" ]; then
    install -Dm755 "$ROOT/hyprdrive.so" "$LIBDIR/hyprdrive.so"
    echo "plugin: $LIBDIR/hyprdrive.so"
else
    echo "install-fragments: no hyprdrive.so -- run make all first, then re-run this" >&2
fi

PLUGIN_SRC=$ROOT/omarchy-plugin
PLUGIN_DEST=${XDG_CONFIG_HOME:-$HOME/.config}/omarchy/plugins/dragos.hyprdrive
# INSTALLED FROM THE MARKETPLACE: `omarchy plugin add` cloned this very
# repository into $PLUGIN_DEST, and its root manifest.json points the shell at
# omarchy-plugin/Panel.qml. The panel is already installed -- it IS this
# checkout -- and copying omarchy-plugin/ over the clone's root would dirty it
# and break `omarchy plugin update`.
if [ -d "$PLUGIN_DEST" ] && [ "$(cd "$PLUGIN_DEST" && pwd -P)" = "$(cd "$ROOT" && pwd -P)" ]; then
    echo "omarchy plugin: $PLUGIN_DEST is this checkout (marketplace install) -- panel already in place"
elif [ -d "$PLUGIN_SRC" ]; then
    # The Omarchy shell watches this directory with inotify and re-initialises
    # EVERY installed plugin whenever a loadable source (manifest.json, *.qml,
    # *.js) is written. Three of the files below are loadable, and they land
    # within ~2ms of each other, well inside the shell's 150ms reload debounce
    # -- so the shell reloads against a half-copied plugin, and that reload
    # spawns ~500 processes (measured). Pause the watcher across the copy and
    # fire exactly one reload once everything is in place.
    # See omacom/omarchy#12601 and #11923.
    RELOAD_PAUSE=$(command -v omarchy-plugin-reload-pause || true)
    if [ -n "$RELOAD_PAUSE" ]; then
        "$RELOAD_PAUSE" on >/dev/null
        trap '"$RELOAD_PAUSE" off >/dev/null 2>&1 || true' EXIT INT TERM
    fi

    mkdir -p "$PLUGIN_DEST/bin"
    # ONE MANIFEST: the marketplace refuses a repository with a second
    # manifest.json near the top, so the copied panel's is generated from the
    # root one, with the entry point moved to where Panel.qml lands here.
    python3 - "$ROOT/manifest.json" "$PLUGIN_DEST/manifest.json" <<'PY'
import json, sys
m = json.load(open(sys.argv[1]))
m["entryPoints"] = {k: v.split("/")[-1] for k, v in m.get("entryPoints", {}).items()}
open(sys.argv[2], "w").write(json.dumps(m, indent=2, ensure_ascii=False) + "\n")
PY
    install -Dm644 "$PLUGIN_SRC/Panel.qml" "$PLUGIN_DEST/Panel.qml"
    install -Dm644 "$PLUGIN_SRC/HyprdrivePanel.qml" "$PLUGIN_DEST/HyprdrivePanel.qml"
    install -Dm644 "$PLUGIN_SRC/Schema.js" "$PLUGIN_DEST/Schema.js"
    install -Dm644 "$PLUGIN_SRC/Model.js" "$PLUGIN_DEST/Model.js"
    install -Dm644 "$PLUGIN_SRC/README.md" "$PLUGIN_DEST/README.md"
    install -Dm644 "$PLUGIN_SRC/hyprdrive-icon.png" "$PLUGIN_DEST/hyprdrive-icon.png"
    install -Dm755 "$PLUGIN_SRC/bin/hdpanel" "$PLUGIN_DEST/bin/hdpanel"
    # Super+L's layout cycle while the panel's Mosaic switch is on; 40-panel.lua
    # binds it by this path (hdpanel's LAYOUT_CYCLE, beside hdpanel itself).
    install -Dm755 "$PLUGIN_SRC/bin/hd-layout-cycle" "$PLUGIN_DEST/bin/hd-layout-cycle"
    echo "omarchy plugin: $PLUGIN_DEST"

    if [ -n "$RELOAD_PAUSE" ]; then
        "$RELOAD_PAUSE" off >/dev/null
        trap - EXIT INT TERM
        # Everything is on disk now: one deliberate reload instead of a
        # mid-copy one. Skipped if the shell is not running.
        if pgrep -x quickshell >/dev/null 2>&1; then
            touch "$PLUGIN_DEST/manifest.json"
            echo "shell reload: 1 (watcher paused during copy)"
        fi
    fi
fi
