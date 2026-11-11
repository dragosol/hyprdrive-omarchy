#!/usr/bin/env bash
# placement -- windows float only when the user floats them: on Mosaic always, everywhere with general:open_tiled.
# Protects (owner, 2026-11-11): "setup an enforcer rule to prevent windows ever
# being floating unless the user has intentionally toggled floating, and this
# rule should apply to any wine and proton windows too", and the rulings:
# option off by default, on Mosaic always; float RULES honoured; splash screens
# and palettes not tiled; the hyprdrive-float tag and the user's toggle are
# intent (src/layout/WindowPlacement.cpp, PlacementModel.hpp).
# Asserts (tests/rig/xstate/x_place.c clients through XWayland):
#   Mosaic desktop:
#   - an X11 dialog (_NET_WM_WINDOW_TYPE_DIALOG + transient-for) is tiled
#   - a fixed-size X11 window (min == max 400x300) is tiled at its own size
#   - a splash is left floating
#   - an override-redirect menu is left as Hyprland has it (floating)
#   - a kitty floated by a window rule floats
#   - Super+T on a tiled window floats it, and it stays floating 1.5 s later
#   - an X11 window the user floated stays floating across a remap (unmap/map)
#   dwindle desktop:
#   - option off: an X11 dialog floats (Hyprland's own placement)
#   - option on (general:open_tiled): an X11 dialog is tiled
# Control (--control --control-so a4d6892): the fixed-size window stayed
# floating, the splash was tiled and the remap forgot the float. Recorded
# 2026-11-11 on a4d6892: control FAILS as it must.
source "$(dirname "$0")/lib.sh"
gate_begin plc
control_build "a4d6892, e.g. /home/dragos/Projects/hyprdrive-native/hyprdrive.so"
cc -O2 -o "$GOUT/x_place" "$ROOT/tests/rig/xstate/x_place.c" $(pkg-config --cflags --libs xcb) || skip "cannot build x_place (xcb headers?)"
chmod 755 "$GOUT" "$GOUT/x_place"
xdisplay() { hlog 4000 | grep -o 'DISPLAY: :[0-9]*' | tail -1 | grep -o ':[0-9]*$'; }
fl() { J clients | jq -r ".[] | select(.class == \"$1\") | .floating" | head -1; }
sz() { J clients | jq -r ".[] | select(.class == \"$1\") | \"\(.size[0])x\(.size[1])\"" | head -1; }
xp() { # xp KIND CLASS [W H] -- an x_place window; waits until it is a client
    $R win env DISPLAY="$D" ${XENV:-} "$GOUT/x_place" "$1" "$2" 40 ${3:-} ${4:-} >/dev/null
    wait_for 5 "[ -n \"\$(addr_of $2)\" ]" || echo "warn: $2 never mapped"
}

rig_start; trace_on
cfg '{general={layout="mosaic"}}'
ev 'hl.bind("SUPER + T", hl.dsp.window.float({ action = "toggle" }))'
ev 'hl.window_rule({ match = { class = "^(rulefloat)$" }, float = true })'
go 1; kw kk
wait_for 6 '[ -n "$(xdisplay)" ]' || { $R win env DISPLAY=:99 true >/dev/null 2>&1; wait_for 6 '[ -n "$(xdisplay)" ]'; }
D=$(xdisplay); [ -n "$D" ] || skip "XWayland never announced a display"

xp dialog xdlg; sleep 0.6
check "Mosaic: an X11 dialog is tiled" "$(is "$(fl xdlg)" false)" "floating=$(fl xdlg)"
xp fixed xfix 400 300; sleep 0.8
check "Mosaic: a fixed-size window is tiled" "$(is "$(fl xfix)" false)" "floating=$(fl xfix)"
check "...at its own size" "$(is "$(sz xfix)" 400x300)" "size $(sz xfix)"
xp splash xspl; sleep 0.6
check "Mosaic: a splash is left floating" "$(is "$(fl xspl)" true)" "floating=$(fl xspl)"
$R win env DISPLAY="$D" "$GOUT/x_place" menu xmenu 40 >/dev/null; sleep 1
check "an override-redirect menu is left as it is (floating)" "$(is "$(fl xmenu)" true)" "floating=$(fl xmenu)"
kw rulefloat 500 400; sleep 0.6
check "a rule-floated window floats" "$(is "$(fl rulefloat)" true)" "floating=$(fl rulefloat)"
focus "$(addr_of kk)"; sleep 0.3; $R tap Super+T >/dev/null; sleep 1.5
check "Super+T floats a window, and it stays floating" "$(is "$(fl kk)" true)" "floating=$(fl kk)"
XENV=X_PLACE_REMAP=2.5 xp normal xremap; sleep 0.5
check "a plain X11 window is tiled" "$(is "$(fl xremap)" false)" "floating=$(fl xremap)"
focus "$(addr_of xremap)"; sleep 0.3; $R tap Super+T >/dev/null; sleep 0.4
F1=$(fl xremap); A1=$(addr_of xremap)
wait_for 6 "[ -n \"\$(addr_of xremap)\" ] && [ \"\$(addr_of xremap)\" != '$A1' ]"; sleep 0.8
check "the user's float survives a remap" "$(is "$F1/$(fl xremap)" true/true)" "before $F1, after $(fl xremap) (addr $A1 -> $(addr_of xremap))"

go 2; cfg '{general={layout="dwindle"}}'; sleep 0.3
xp dialog ddlg; sleep 0.6
check "dwindle, option off: an X11 dialog floats" "$(is "$(fl ddlg)" true)" "floating=$(fl ddlg)"
cfg '{plugin={hyprdrive={general={open_tiled=true}}}}'; sleep 0.3
xp dialog ddlg2; sleep 0.6
check "dwindle, option on: an X11 dialog is tiled" "$(is "$(fl ddlg2)" false)" "floating=$(fl ddlg2)"
check "the trace names the decisions" "$(ge "$(trace | grep -c 'placement .*overridden')" 2)" "$(trace | grep placement | tail -3 | cut -d' ' -f3- | tr '\n' ';')"
gate_end
