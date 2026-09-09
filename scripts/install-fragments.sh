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
if [ -d "$PLUGIN_SRC" ]; then
    mkdir -p "$PLUGIN_DEST/bin"
    install -Dm644 "$PLUGIN_SRC/manifest.json" "$PLUGIN_DEST/manifest.json"
    install -Dm644 "$PLUGIN_SRC/Panel.qml" "$PLUGIN_DEST/Panel.qml"
    install -Dm644 "$PLUGIN_SRC/Model.js" "$PLUGIN_DEST/Model.js"
    install -Dm644 "$PLUGIN_SRC/README.md" "$PLUGIN_DEST/README.md"
    install -Dm755 "$PLUGIN_SRC/bin/hdpanel" "$PLUGIN_DEST/bin/hdpanel"
    echo "omarchy plugin: $PLUGIN_DEST"
fi
