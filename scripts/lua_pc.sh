#!/bin/sh
# Which pkg-config Lua to build against: the one Hyprland itself is linked to.
#
# A Lua header bakes constants into the plugin -- LUA_REGISTRYINDEX is
# -1001000 in 5.4 and -1073742823 in 5.5 -- that must match the liblua the
# compositor runs, or the first lua_rawgeti(L, LUA_REGISTRYINDEX, ...) reads a
# wild stack slot and Hyprland segfaults (dragosol/Hyprdrive#1: current Arch
# has `lua` 5.5, Hyprland's, AND `lua54` 5.4, which libinput pulls in, and the
# old "lua5.4 whenever it exists" pick crashed `hyprctl hyprdrive-binds`).
# Fedora ships one lua.pc (5.4) and its Hyprland links 5.4.
#
# HYPRLAND_BIN overrides which Hyprland to ask. Prints a pkg-config module name.
HYPR=${HYPRLAND_BIN:-$(command -v Hyprland 2>/dev/null)}
want=""
if [ -n "$HYPR" ] && [ -r "$HYPR" ]; then
    # Its own NEEDED entry, not a transitive one: liblua.so.5.5,
    # liblua-5.4.so, liblua5.4.so.5.4 ... -> 5.5 / 5.4
    lib=$(readelf -d "$HYPR" 2>/dev/null | sed -n 's/.*NEEDED.*\[\(liblua[^]]*\)\].*/\1/p' | head -n 1)
    want=$(printf '%s\n' "$lib" | grep -o '5\.[0-9]' | head -n 1)
fi
for m in lua lua5.5 lua-5.5 lua55 lua5.4 lua-5.4 lua54; do
    pkg-config --exists "$m" 2>/dev/null || continue
    v=$(pkg-config --modversion "$m" 2>/dev/null | cut -d. -f1,2)
    if [ -z "$want" ] || [ "$v" = "$want" ]; then
        echo "$m"
        exit 0
    fi
done
# Nothing matches it (or no Hyprland to ask): the default Lua.
echo lua
