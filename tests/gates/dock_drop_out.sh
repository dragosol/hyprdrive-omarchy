#!/usr/bin/env bash
# dock_drop_out -- a file pulled out of a dock stack and dropped on a client ends the drag (H8, the owner's 11-13 report).
# Protects (DockDrop.cpp H8, endDroppedOut): Hyprland's dropDrag() sends the
# drop and leaves the drag standing until the target answers
# wl_data_offer.finish; kitty never does. Without H8 the drag stood for good:
# the file's icon stayed on the cursor, and every later click went nowhere
# (Hyprland refuses pointer focus during a drag) until a plugin reload aborted
# it (owner: "it just holds the file on my cursor and never drops it and its
# stuck that way ... seems to have fixed itself in omarchy after i did
# hyprdrive reload").
# The drop target is a kitty whose shell is `cat` into a file, so a drop that
# lands is seen as the path it pastes; the click probe is tests/gates/lockclient.py,
# which logs every button it receives.
# Asserts:
#   1. the drag out of the stack lands: kitty pastes the file's path
#   2. after the drop, a click on another window reaches it (the drag ended)
#   3. ... and the next one too
# Control (--control --control-so <a build without H8: the Omarchy tree before
# its port, e.g. ce5261f>): 2 and 3 fail; 1 passes (the drop itself lands).
# The rig's stack: the first non-app cell after the running apps (70 px pitch at
# scale 1), whose fan's first item sits 82 px above the icon centre -- the
# hyprtest user's dock. Should the drop not land (1 fails), the fan did not open
# where expected: fix the coordinates, not the plugin.
source "$(dirname "$0")/lib.sh"
control_build "a build without H8 (Omarchy ce5261f)"
gate_begin ddo
EV=$GOUT/ev; SU rm -rf "$EV" "$GOUT/py" 2>/dev/null; mkdir -p "$EV" "$GOUT/py"; chmod 777 "$EV" "$GOUT/py"
cp "$GATES_DIR/lockclient.py" "$GOUT/"; chmod 644 "$GOUT/lockclient.py"
SU python3 -m pywayland.scanner -i /usr/share/wayland/wayland.xml \
    /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml \
    /usr/share/wayland-protocols/unstable/pointer-constraints/pointer-constraints-unstable-v1.xml \
    /usr/share/wayland-protocols/unstable/relative-pointer/relative-pointer-unstable-v1.xml \
    -o "$GOUT/py/lcproto" >/dev/null 2>&1 || skip "pywayland scanner failed for hyprtest"
rig_start

D=$EV/dropped.txt
$R win kitty -o remember_window_size=no --class tgt sh -c "stty -icanon -echo; exec cat > $D" >/dev/null
wait_for 6 "[ -n \"\$(addr_of tgt)\" ]" || echo "warn: tgt never mapped"
L=$EV/probe.log
ev "hl.dispatch(hl.dsp.exec_cmd(\"env PYTHONPATH=$GOUT/py python3 $GOUT/lockclient.py $L probe none\"))"
wait_for 6 "[ -n \"\$(addr_of probe)\" ]" || echo "warn: probe never mapped"
sleep 1; settle 3 geom
centre() { J clients | jq -r ".[] | select(.class == \"$1\") | \"\(.at[0]+.size[0]/2|floor) \(.at[1]+.size[1]/3|floor)\""; }
read TX TY <<<"$(centre tgt)"; read PX PY <<<"$(centre probe)"

# the dock out, and where its stack is
$R ptr seq "move 960 1079" "rel 0 20 15 16" "wait 1500" >/dev/null
LASTX=$(dbg dockmenu | sed -n 's/^ icon [^ ]* \([0-9]*\),\([0-9]*\) \([0-9]*\)x.*/\1 \3/p' | awk '{c=$1+$2/2; if (c>m) m=c} END {print int(m)}')
# an older build's report lists no icons: the rig's dock with these two apps running
[ "${LASTX:-0}" -gt 0 ] 2>/dev/null || LASTX=1100
SX=$((LASTX + 70)); SY=1035; echo "stack cell at $SX,$SY (last app icon centre $LASTX)"
# one device: open the fan, press its first item, pull it onto kitty, let go
$R ptr seq "move $SX 1079" "rel 0 20 15 16" "wait 1200" "move $SX $SY" "wait 600" "click" "wait 1500" \
    "move $SX $((SY - 82)) 5" "wait 400" "down" "wait 300" "move $((SX - 170)) 800 10" "move $TX $TY 20" "wait 1500" "up" "wait 300" >/dev/null
sleep 2.5   # past H8's settle (1 s)
check "1: the drag out of the stack lands on kitty (its path pasted)" "$(grep -q / "$D" 2>/dev/null && echo 1)" "pasted: [$(head -c 120 "$D" 2>/dev/null)]"
B0=$(grep -c '^button 272 1' "$L")
$R ptr tap "$PX" "$PY" left >/dev/null; sleep 1
check "2: after the drop, a click on another window reaches it" "$(is "$(grep -c '^button 272 1' "$L")" $((B0 + 1)))" "presses $B0 -> $(grep -c '^button 272 1' "$L")"
$R ptr tap "$PX" "$PY" left >/dev/null; sleep 1
check "3: ... and the next one" "$(is "$(grep -c '^button 272 1' "$L")" $((B0 + 2)))" "presses $B0 -> $(grep -c '^button 272 1' "$L")"
# closed here, not by the rig's stop (pywayland segfaults on a dropped connection)
close_win "$(addr_of probe)"; sleep 1
gate_end
