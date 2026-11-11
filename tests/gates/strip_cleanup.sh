#!/usr/bin/env bash
# strip_cleanup -- Super held over the overview strip: a Clean Up button under it removes the empty desktops between.
# Protects: the owner's request 2026-11-11 ("if I hold super over the workspace
# strip it puts a button under the strip near my cursor to clean up empty
# desktops" ... "where's my 'clear empty desktops button'?" ... "move the
# windows on later desktops back to the next available desktops").
# Asserts (one window each on 1 2 5 6, 1 shown, the overview open):
#   1. Super up, the pointer on the strip: no button
#   2. Super held, the pointer on the strip: the button is up, just under the
#      strip's plate, centred near the pointer (clamped to the plate)
#   3. the pointer moved along the strip: the button follows it
#   4. the pointer down from the strip onto the button, Super still held: it stays
#   5. Super released: it goes
#   6. back on the strip, down onto the button, a click (Super held): populated desktops 1 2 3 4, each window on
#      its old desktop's rank, 1 still shown, no window moved or resized, the
#      overview still open, the strip has no cell for the removed ones
#   7. afterwards Super held over the strip again: no button (nothing to clean)
#   B. a fullscreen trip in flight: a second window on 1 goes fullscreen (it
#      takes 2, the others move along: 3 6 7), the overview opens on it, a
#      click -> 1 2 3 4 5 with the app alone on 2, still fullscreen and shown;
#      it leaves fullscreen -> home on 1, the room it took given back: 1 2 3 4
#   C. Omarchy's blocks (the rig's emulation, as the blocks gate runs it): output
#      1 = block 0 with windows on 1 3, output 2 = block 1 with 11 12 15; the
#      button on output 2's strip -> 11 12 13 there, output 1's 1 3 untouched
#      (one row only); a reload then: zero changeworkspaceid (no fight), 11 12 13 kept
# Control (--control): overview:strip_cleanup_button off -- 2, 3, 4, 6, B, C fail.
source "$(dirname "$0")/lib.sh"
gate_begin scl
rig_start
[ "$CONTROL" = 1 ] && cfg '{plugin={hyprdrive={strip_cleanup_button=false}}}'
for n in 1 2 5 6; do go $n; kw "w$n" 900 600; done
go 1; sleep 0.6
check "start: populated 1 2 5 6" "$(is "$(pop)" '1 2 5 6')" "$(pop)"
G0=$(geom | sed 's/ [0-9]* / /g') # classes + boxes, without the desktop number

sc() { dbg stripclean | grep -o 'stripclean:.*' | head -1; }
field() { sc | grep -o "$1=[^ ]*" | head -1 | cut -d= -f2; }
boxf() { sc | grep -o "$1=[-0-9]*,[-0-9]* [0-9]*x[0-9]*" | head -1 | sed "s/$1=//; s/[,x]/ /g"; }

$R overview >/dev/null; sleep 1.2
set -- $(boxf plate)
[ $# -eq 4 ] || { check "debug stripclean reports the strip plate" 0 "$(sc)"; gate_end; }
PX=$1; PY=$2; PW=$3; PH=$4
CY=$(( PY + PH / 2 ))
X1=$(( PX + PW / 2 ))
$R ptr move "$X1" "$CY" >/dev/null; sleep 0.5
check "1: Super up over the strip: no button" "$(is "$(field want)" 0)" "$(sc)"

$R down Super >/dev/null; sleep 0.6
set -- $(boxf box); BX=${1:-0}; BY=${2:-0}; BW=${3:-0}; BH=${4:-0}
check "2: Super held over the strip: the button is up" "$(is "$(field want)" 1)" "$(sc)"
check "2: ... just under the strip's plate" "$([ "$BW" -gt 40 ] && [ "$BY" -ge $(( PY + PH )) ] && [ "$BY" -le $(( PY + PH + 30 )) ] && echo 1)" "plate $PX,$PY ${PW}x$PH, button $BX,$BY ${BW}x$BH"
check "2: ... centred near the pointer" "$([ $(( BX + BW / 2 - X1 )) -le 3 ] && [ $(( X1 - BX - BW / 2 )) -le 3 ] && echo 1)" "pointer x $X1, button $BX..$(( BX + BW ))"

X2=$(( PX + PW / 4 )); [ $(( X2 - PX )) -lt $(( BW / 2 )) ] && X2=$(( PX + BW / 2 + 10 ))
$R ptr path 20 "$X1" "$CY" "$X2" "$CY" >/dev/null; sleep 0.8
set -- $(boxf box); BX=${1:-0}; BW=${3:-0}
check "3: the pointer along the strip: the button follows it" "$([ "$BW" -gt 40 ] && [ $(( BX + BW / 2 - X2 )) -le 3 ] && [ $(( X2 - BX - BW / 2 )) -le 3 ] && echo 1)" "pointer x $X2, button $BX..$(( BX + BW )), $(sc)"

set -- $(boxf box); BX=${1:-0}; BY=${2:-0}; BW=${3:-0}; BH=${4:-0}
TX=$(( BX + BW / 2 )); TY=$(( BY + BH / 2 ))
$R ptr path 15 "$X2" "$CY" "$TX" "$TY" >/dev/null; sleep 0.6
check "4: down onto the button, Super held: it stays" "$([ "$(field want)" = 1 ] && [ "$BW" -gt 40 ] && echo 1)" "$(sc)"
shot "$GOUT/button.png"

$R up Super >/dev/null; sleep 0.6
check "5: Super released: it goes" "$(is "$(field want)" 0)" "$(sc)"

# back onto the strip with Super held, down onto the button, click
$R ptr move "$X2" "$CY" >/dev/null; $R down Super >/dev/null; sleep 0.6
set -- $(boxf box); TX=$(( ${1:-0} + ${3:-0} / 2 )); TY=$(( ${2:-0} + ${4:-0} / 2 ))
$R ptr path 15 "$X2" "$CY" "$TX" "$TY" >/dev/null; sleep 0.4
shot "$GOUT/hover.png"
$R ptr click left >/dev/null; sleep 1.5
$R up Super >/dev/null; sleep 0.3
check "6: click: populated desktops 1 2 3 4" "$(is "$(pop)" '1 2 3 4')" "$(ids)"
check "6: ... each window on its old desktop's rank" "$(is "$(win w1 .workspace.id)$(win w2 .workspace.id)$(win w5 .workspace.id)$(win w6 .workspace.id)" 1234)" "$(geom)"
check "6: ... desktop 1 still shown" "$(is "$(shown)" 1)" "shown $(shown)"
check "6: ... no window moved or resized" "$(is "$(geom | sed 's/ [0-9]* / /g')" "$G0")" "$(geom)"
check "6: ... the overview is still open" "$([ -n "$(field images)" ] && echo 1)" "$(sc)"
check "6: ... the strip has 1..4 and the spare (5 cells)" "$(is "$(field images)" 5)" "$(sc), $(ids)"
shot "$GOUT/after.png"

$R ptr move "$X1" "$CY" >/dev/null; $R down Super >/dev/null; sleep 0.6
check "7: Super held over the strip again: no button (nothing to clean)" "$(is "$(field want)$(field removable)" 00)" "$(sc)"
$R up Super >/dev/null
$R overview >/dev/null; sleep 0.8
check "after: populated 1 2 3 4 with the overview closed" "$(is "$(pop)" '1 2 3 4')" "$(ids)"
rig_stop

# B. with a fullscreen trip in flight
rig_start; trace_on
[ "$CONTROL" = 1 ] && cfg '{plugin={hyprdrive={strip_cleanup_button=false}}}'
for n in 1 2 5 6; do go $n; kw "w$n" 900 600; done
go 1; kw w1b 900 600; A=$(addr_of w1b); focus "$A"; fs "$A" set; sleep 1.5
check "B: trip: the app alone on 2, the rest moved along" "$(is "$(pop) $(win w1b .workspace.id)" '1 2 3 6 7 2')" "$(ids)"
$R overview >/dev/null; sleep 1.2
set -- $(boxf plate); PX=${1:-0}; PY=${2:-0}; PW=${3:-0}; PH=${4:-0}; CY=$(( PY + PH / 2 )); X1=$(( PX + PW / 2 ))
$R ptr move "$X1" "$CY" >/dev/null; $R down Super >/dev/null; sleep 0.6
set -- $(boxf box); TX=$(( ${1:-0} + ${3:-0} / 2 )); TY=$(( ${2:-0} + ${4:-0} / 2 ))
$R ptr path 15 "$X1" "$CY" "$TX" "$TY" >/dev/null; sleep 0.4
$R ptr click left >/dev/null; sleep 1.5; $R up Super >/dev/null
check "B: click with the trip in flight: 1 2 3 4 5, the app alone on 2" "$(is "$(pop) $(win w1b .workspace.id)" '1 2 3 4 5 2')" "$(ids)"
check "B: ... still fullscreen, still shown" "$(is "$(win w1b .fullscreen) $(shown)" '2 2')" "fullscreen $(win w1b .fullscreen), shown $(shown)"
$R overview >/dev/null; sleep 1
fs "$A" unset; wait_for 4 "[ \"\$(pop)\" = '1 2 3 4' ]"; sleep 0.3
check "B: out of fullscreen: home on 1, the room given back: 1 2 3 4" "$(is "$(pop) $(win w1b .workspace.id)" '1 2 3 4 1')" "$(ids)"
check "B: ... each window on its rank" "$(is "$(win w2 .workspace.id)$(win w5 .workspace.id)$(win w6 .workspace.id)" 234)" "$(geom)"
rig_stop

# C. Omarchy's blocks: only the row of the strip the button is on
RIG=$ROOT/tests/rig/headless
SU bash -c "mkdir -p '$ST/state/omarchy' && chmod 755 '$ST' '$ST/state' '$ST/state/omarchy' && rm -f '$ST/state/omarchy/'* && printf '0\tHEADLESS-1\tHEADLESS-1\n1\tHEADLESS-2\tHEADLESS-2\n' > '$ST/state/omarchy/monitor-workspaces'"
rig_start --config "$RIG/blocks.lua" --env "HD_BLOCKS_LUA=$RIG/omarchy-blocks.lua" --env "HD_PLUGIN_LUA=$ROOT/config/hyprdrive/00-plugin.lua" \
    --env "HYPRDRIVE_SO=$SO" --env "XDG_STATE_HOME=$ST/state"
[ "$CONTROL" = 1 ] && cfg '{plugin={hyprdrive={strip_cleanup_button=false}}}'
popon() { J workspaces | jq -r "[.[] | select(.monitor == \"$1\" and .id > 0 and .windows > 0) | .id] | sort | map(tostring) | join(\" \")"; }
focus_mon() { ev "hl.dispatch(hl.dsp.focus({ monitor = \"$1\" }))"; sleep 0.2; }
focus_mon HEADLESS-1; go 1; kw a1 900 600; go 3; kw a3 900 600
$R mon2 >/dev/null; sleep 1
focus_mon HEADLESS-2; go 11; kw b11 900 600; go 12; kw b12 900 600; go 15; kw b15 900 600; go 11; sleep 0.8
check "C: start 1 3 | 11 12 15" "$(is "$(popon HEADLESS-1)|$(popon HEADLESS-2)" '1 3|11 12 15')" "$(popon HEADLESS-1)|$(popon HEADLESS-2)"
$R overview >/dev/null; sleep 1.5
sc2() { dbg stripclean | grep -o 'HEADLESS-2: stripclean:.*' | head -1; }
box2() { sc2 | grep -o "$1=[-0-9]*,[-0-9]* [0-9]*x[0-9]*" | head -1 | sed "s/$1=//; s/[,x]/ /g"; }
set -- $(box2 plate); PX=$(( ${1:-0} + 1920 )); PY=${2:-0}; PW=${3:-0}; PH=${4:-0}; CY=$(( PY + PH / 2 )); X1=$(( PX + PW / 2 ))
$R ptr move "$X1" "$CY" >/dev/null; $R down Super >/dev/null; sleep 0.6
set -- $(box2 box); TX=$(( ${1:-0} + 1920 + ${3:-0} / 2 )); TY=$(( ${2:-0} + ${4:-0} / 2 ))
$R ptr path 15 "$X1" "$CY" "$TX" "$TY" >/dev/null; sleep 0.4
: > "$GOUT/events.log"; SU socat -u "UNIX-CONNECT:$RT/hypr/$SIG/.socket2.sock" - > "$GOUT/events.log" 2>/dev/null & _bg_pids="${_bg_pids:-} $!"; sleep 0.3
$R ptr click left >/dev/null; sleep 1.5; $R up Super >/dev/null
check "C: click on output 2's button: 11 12 13 there, 1 3 untouched" "$(is "$(popon HEADLESS-1)|$(popon HEADLESS-2)" '1 3|11 12 13')" "$(popon HEADLESS-1)|$(popon HEADLESS-2), $(sc2)"
$R overview >/dev/null; sleep 1.2
C0=$(grep -c '^changeworkspaceid>>' "$GOUT/events.log")
hq reload >/dev/null; sleep 2
N=$(( $(grep -c '^changeworkspaceid>>' "$GOUT/events.log") - C0 ))
check "C: a reload after it: zero changeworkspaceid (a fixed point of Omarchy's reconcile)" "$(is "$N" 0)" "$N: $(grep '^changeworkspaceid>>' "$GOUT/events.log" | tail -4 | tr '\n' ' ')"
# (the reload's own compaction then closes output 1's hole too: Rows.cpp, by design)
check "C: ... and output 2 keeps 11 12 13" "$(is "$(popon HEADLESS-2)" '11 12 13')" "$(popon HEADLESS-1)|$(popon HEADLESS-2)"
gate_end
