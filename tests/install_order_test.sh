#!/usr/bin/env bash
# The installed plugin is whole before anything can load it (issue #14).
#
# install-fragments.sh adds the require line to hyprland.lua, and Hyprland
# reloads its config and loads the plugin the moment that file changes. On a
# first install the script used to add the line first and copy hyprdrive.so
# after, so Hyprland could open a half-written file and crash.
#
# Checked with a stand-in mv on PATH that looks at hyprland.lua at the moment
# the plugin is renamed into place:
#   1 the plugin reaches its path by a rename (never written in place)
#   2 hyprland.lua has no require line yet at that moment
#   3 afterwards the plugin is there, whole, and the require line is added
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d) || exit 1
trap 'rm -rf "$T"' EXIT
fails=0
check() { if [ "$1" = 0 ]; then :; else echo "FAIL: $2"; fails=$((fails + 1)); fi; }

mkdir -p "$T/root/scripts" "$T/root/config" "$T/home/.config/hypr" "$T/bin"
cp "$ROOT/scripts/install-fragments.sh" "$ROOT/scripts/fragment.py" "$T/root/scripts/"
[ -e "$ROOT/scripts/hyprdrive-integration" ] && cp "$ROOT/scripts/hyprdrive-integration" "$T/root/scripts/"
cp "$ROOT/manifest.json" "$T/root/" 2>/dev/null
cp "$ROOT/config/hyprdrive.lua" "$T/root/config/"
cp -r "$ROOT/config/hyprdrive" "$T/root/config/"
cp -r "$ROOT/ui" "$T/root/ui"
head -c 300000 /dev/urandom > "$T/root/hyprdrive.so"
echo '-- the user config' > "$T/home/.config/hypr/hyprland.lua"

cat > "$T/bin/mv" <<SH
#!/bin/sh
for a in "\$@"; do last=\$a; done
case \$last in
    */lib/hyprdrive/hyprdrive.so)
        echo renamed >> "$T/log"
        grep -q hyprdrive "$T/home/.config/hypr/hyprland.lua" && echo late >> "$T/log" ;;
esac
exec /usr/bin/mv "\$@"
SH
chmod +x "$T/bin/mv"

env -i HOME="$T/home" XDG_CONFIG_HOME="$T/home/.config" XDG_STATE_HOME="$T/home/.local/state" \
    PATH="$T/bin:/usr/bin:/bin" PREFIX="$T/home/.local" KEEP_DEBUG=1 HYPRDRIVE_NO_OMARCHY=1 \
    bash "$T/root/scripts/install-fragments.sh" > "$T/out" 2>&1
check $? "install-fragments.sh exits 0: $(tail -3 "$T/out")"

grep -q renamed "$T/log" 2>/dev/null
check $? "1 the plugin is renamed into place"
! grep -q late "$T/log" 2>/dev/null
check $? "2 hyprland.lua has no require line when the plugin lands"
cmp -s "$T/root/hyprdrive.so" "$T/home/.local/lib/hyprdrive/hyprdrive.so"
check $? "3 the installed plugin is the whole file"
grep -q 'require("hypr' "$T/home/.config/hypr/hyprland.lua"
check $? "3 the require line is added"
[ -z "$(ls -A "$T/home/.local/lib/hyprdrive" | grep '^\.hyprdrive\.so')" ]
check $? "3 no temporary file is left beside the plugin"

echo "install_order_test: 6 checks, $fails failed"
[ "$fails" = 0 ]
