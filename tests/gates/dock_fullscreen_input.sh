#!/usr/bin/env bash
# dock_fullscreen_input -- a dock that is not drawn, or a held pointer, takes no input from a fullscreen window (issue #11).
# Protects (Dock::inputInert, main.cpp's pointer listeners and render.preChecks stand-down):
#   an opaque fullscreen window is drawn on Hyprland's solitary path, which
#   never reaches RENDER_POST_WINDOWS, so the dock is not on screen there --
#   and a pointer resting where the pill would be took the window's keyboard
#   and mouse (the seat), and a click there acted on an icon nobody could see
#   (reporter: Runescape Dragonwilds under Proton, keys dead, mouse stuck).
# The "game" is tests/gates/lockclient.py: an opaque toplevel that logs every
# key, button, pointer enter and relative motion it gets, and can take a
# pointer lock like a game in mouse-look.
# Asserts:
#   1. opaque fullscreen = solitary; pointer pushed into the bottom edge and
#      then on the (invisible) pill: a key reaches the game, a click reaches
#      the game, and the dock did nothing (same windows, none minimized)
#   2. composited fullscreen (a 90%-opaque kitty: not solitary) is unchanged:
#      the dock still reveals and is drawn over it
#   3. pointer resting on the dock, then a game opens fullscreen and LOCKS the
#      pointer without the pointer moving: its keys arrive at once, and
#      relative motion reaches it
# Control (--control --control-so <a build without Dock::inputInert, e.g.
# 32a3f63>): 1 fails (the key and the click never reach the game).
# NOT covered: XWayland's own lock emulation (Wine), a real GPU's direct
# scanout (the rig is headless; render.preChecks fires on both paths).
source "$(dirname "$0")/lib.sh"
control_build "a build without Dock::inputInert (32a3f63)"
gate_begin dfi
EV=$GOUT/ev; mkdir -p "$EV" "$GOUT/py"; chmod 777 "$EV" "$GOUT/py"
cp "$GATES_DIR/lockclient.py" "$GOUT/"; chmod 644 "$GOUT/lockclient.py"
SU python3 -m pywayland.scanner -i /usr/share/wayland/wayland.xml \
    /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml \
    /usr/share/wayland-protocols/unstable/pointer-constraints/pointer-constraints-unstable-v1.xml \
    /usr/share/wayland-protocols/unstable/relative-pointer/relative-pointer-unstable-v1.xml \
    -o "$GOUT/py/lcproto" >/dev/null 2>&1 || skip "pywayland scanner failed for hyprtest"
rig_start

# game LOG CLASS MODE -- the lock client, launched by the compositor (the pointer does not move)
game() {
    ev "hl.dispatch(hl.dsp.exec_cmd(\"env PYTHONPATH=$GOUT/py python3 $GOUT/lockclient.py $1 $2 $3\"))"
    wait_for 6 "[ -n \"\$(addr_of $2)\" ]" || echo "warn: $2 never mapped"
}
solitary() { J monitors | jq -r '.[] | select(.name == "HEADLESS-1") | .solitary'; }
n() { grep -c "^$2" "$1" 2>/dev/null || true; }
# the pointer pushed into the bottom edge (relative motion, as a mouse does), then onto the pill
to_pill() {
    $R ptr move 960 1079 >/dev/null; sleep 0.2
    $R ptr rel 0 20 15 16 >/dev/null; sleep 1.2
    $R ptr move 960 1040 >/dev/null; sleep 0.8
}

# ---- 1. solitary
L1=$EV/g1.log
game "$L1" game none
G=$(addr_of game); focus "$G"; sleep 0.3; fs "$G" set; sleep 1.5; settle 3 geom
wait_for 4 "[ \"\$(solitary)\" != 0 ] && [ -n \"\$(solitary)\" ]"
check "1: the opaque fullscreen game is on the solitary path" "$([ "$(solitary)" != 0 ] && [ -n "$(solitary)" ] && echo 1)" "solitary=$(solitary)"
GEOM0=$(geom)
to_pill
K0=$(n "$L1" "key 46 1"); B0=$(n "$L1" "button 272 1")
$R type c >/dev/null; sleep 0.5
check "1: pointer on the undrawn pill: a key reaches the game" "$(is "$(n "$L1" "key 46 1")" $((K0 + 1)))" "keys $K0 -> $(n "$L1" "key 46 1")"
$R ptr click left >/dev/null; sleep 1.2
check "1: ... and a click reaches the game" "$(is "$(n "$L1" "button 272 1")" $((B0 + 1)))" "presses $B0 -> $(n "$L1" "button 272 1")"
check "1: ... and the dock did nothing (same windows, same boxes)" "$(is "$(geom)" "$GEOM0")" "$(geom) was $GEOM0"
$R ptr move 960 540 >/dev/null; sleep 0.5
fs "$G" unset; sleep 1; close_win "$G"; sleep 1.2; go 1; sleep 0.8

# ---- 2. composited: today's dock, drawn over the fullscreen window
ev "hl.dispatch(hl.dsp.exec_cmd(\"kitty -o remember_window_size=no -o background=#ffffff -o background_opacity=0.9 --class glass\"))"
wait_for 6 "[ -n \"\$(addr_of glass)\" ]" || echo "warn: glass never mapped"
A=$(addr_of glass)
focus "$A"; sleep 0.3; fs "$A" set; sleep 1.5; settle 3 geom
SOL2=$(solitary)
$R ptr move 960 540 >/dev/null; sleep 1.2; shot "$EV/2a.png"
to_pill; shot "$EV/2b.png"
D=$(pixdiff "$EV/2a.png" "$EV/2b.png"); BLOB=${D#* }
check "2: a composited fullscreen window is not solitary" "$(is "$SOL2" 0)" "solitary=$SOL2"
check "2: ... and the dock still reveals over it, drawn" "$(ge "${BLOB:-0}" 20000)" "pixdiff $D"
$R ptr move 960 540 >/dev/null; sleep 0.5
fs "$A" unset; sleep 1; close_win "$A"; sleep 1.2; go 1; sleep 0.8

# ---- 3. pointer resting on the dock, then a game takes a pointer lock.
# ONE pointer device for the whole step (vptr seq, in the background): the
# rig's devices go away with each `ptr` call, and with no mouse left Hyprland
# refuses every pointer focus ("setPointerFocus without a valid mouse set") --
# no enter, so no relative motion, whatever the dock does.
L3=$EV/g3.log
$R ptr seq "move 960 1079" "rel 0 20 15 16" "wait 1200" "move 960 1040" "wait 9000" "rel 6 -4 10 16" "wait 1500" >/dev/null 2>&1 &
SEQ=$!; _bg_pids="${_bg_pids:-} $SEQ"
sleep 3
game "$L3" lockgame lock
G3=$(addr_of lockgame); fs "$G3" set
wait_for 4 "grep -q '^locked' $L3"
sleep 0.8
check "3: the game holds a pointer lock" "$(grep -q '^locked' "$L3" && echo 1)" "$(tr '\n' ' ' < "$L3" | cut -c1-200)"
K3=$(n "$L3" "key 37 1"); R3=$(n "$L3" rel)
$R type k >/dev/null; sleep 0.5
check "3: no motion yet, a key reaches the game" "$(is "$(n "$L3" "key 37 1")" $((K3 + 1)))" "keys $K3 -> $(n "$L3" "key 37 1")"
wait "$SEQ" 2>/dev/null
check "3: relative motion reaches the game" "$(ge "$(n "$L3" rel)" $((R3 + 5)))" "rel $R3 -> $(n "$L3" rel); $(uniq -c < "$L3" | tr '\n' ' ' | cut -c1-300)"
$R type k >/dev/null; sleep 0.5
check "3: ... and keys still do" "$(is "$(n "$L3" "key 37 1")" $((K3 + 2)))" "keys $K3 -> $(n "$L3" "key 37 1")"
# closed here, not by the rig's stop: pywayland segfaults on a connection the
# compositor drops under it, and that would read as a core of this instance
close_win "$G3"; sleep 1
gate_end
