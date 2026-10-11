#!/usr/bin/env bash
# Remove Hyprdrive: everything scripts/install-fragments.sh put in place.
#
#   - the plugin:        ~/.local/lib/hyprdrive/
#   - the fragments:     ~/.config/hypr/hyprdrive/  (moved aside, not deleted:
#                        their raw-snippet blocks are the user's own edits)
#   - the loader:        ~/.config/hypr/hyprdrive.lua
#   - the require block install-fragments appended to ~/.config/hypr/hyprland.lua
#   - the settings panel, when it was copied there by `make install`. A panel
#     the marketplace installed is a git checkout: `omarchy plugin remove
#     dragos.hyprdrive` removes that one, and this script says so.
#
# Left alone: ~/.local/state/hyprdrive (favourites, stacks, Mosaic sizes),
# so a reinstall picks up where it was. Delete it by hand to start fresh.
#
# The compositor keeps the plugin it already loaded until the session ends:
# log out and back in (or restart Hyprland) to finish. Unloading it live is
# not done here -- a live unload mid-gesture can take the session down.
set -eu
CONFIG=${XDG_CONFIG_HOME:-$HOME/.config}
LIBDIR=$HOME/.local/lib/hyprdrive
FRAGS=$CONFIG/hypr/hyprdrive
LOADER=$CONFIG/hypr/hyprdrive.lua
HYPRLAND_LUA=$CONFIG/hypr/hyprland.lua
PANEL=$CONFIG/omarchy/plugins/dragos.hyprdrive

if [ -d "$LIBDIR" ]; then rm -rf "$LIBDIR"; echo "removed $LIBDIR"; fi

if [ -d "$FRAGS" ]; then
    BACKUP="$FRAGS.removed-$(date +%Y%m%d-%H%M%S)"
    mv "$FRAGS" "$BACKUP"
    echo "fragments moved to $BACKUP (delete it once you are sure)"
fi

if [ -f "$LOADER" ]; then rm -f "$LOADER"; echo "removed $LOADER"; fi

if [ -f "$HYPRLAND_LUA" ] && grep -q 'require("hypr.hyprdrive")' "$HYPRLAND_LUA"; then
    # Exactly the block install-fragments appends: its comment and the require.
    python3 - "$HYPRLAND_LUA" <<'PY'
import sys, re
p = sys.argv[1]
s = open(p).read()
s2 = re.sub(r'\n?-- Hyprdrive \(overview \+ dock\)\. Fragments live in hypr/hyprdrive/\.\n', '\n', s)
s2 = re.sub(r'^require\("hypr\.hyprdrive"\)\n?', '', s2, flags=re.M)
if s2 != s:
    open(p + ".hyprdrive-bak", "w").write(s)
    open(p, "w").write(s2)
    print(f"removed require(\"hypr.hyprdrive\") from {p} (backup: {p}.hyprdrive-bak)")
PY
fi

if [ -d "$PANEL" ]; then
    if [ -d "$PANEL/.git" ]; then
        echo "settings panel installed from the marketplace: run  omarchy plugin remove dragos.hyprdrive"
    else
        rm -rf "$PANEL"
        echo "removed settings panel $PANEL"
    fi
fi

echo "done -- log out and back in to unload the running plugin"
