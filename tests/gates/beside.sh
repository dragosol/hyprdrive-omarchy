#!/usr/bin/env bash
# beside -- a window that opens over a fullscreen app goes beside it; asked for, the view follows and closing it goes back to the app.
# Protects (owner, 2026-11-11): "if a floating window shows up over a
# fullscreen one, it should be moved to the next desktop (new one if next is
# not empty) and focused ... if the window is like a file selection prompt or
# something intentional and temporary, upon closing the window, user should be
# sent back to the fullscreen app"; "if a window opens unprompted over
# something fullscreen it should go to next desktop and not be focused";
# a second one joins the first one's desktop; happens regardless of
# general:open_tiled; in the monitor's row (FullscreenDesktop.cpp links,
# FullscreenBook::link).
# Asserts, from 1:{a,b} 2:{c}, a fullscreen (a trip: a on 2, c bumped to 3):
#   - PROMPTED (launched through the compositor: Intent's launch grant) X11
#     dialog: on the desktop right after a's, shown, focused; a stays
#     fullscreen; a second prompted one (from the app again) joins it; both
#     closed: the view is back on a, a focused, the desktops as before
#   - UNPROMPTED (mapped behind the compositor's back, no click before): on
#     the next desktop, the view stays on a, a keeps the focus; closed:
#     nothing moves, the desktops as before
#   - closed DURING the glide: back on a, the desktops as before
#   - a SETTLED fullscreen app (alone on its desktop) too
#   - 30 open/close cycles leave no desktop behind
#   - with Omarchy's blocks (per-monitor rows, tests/gates/blocks.sh) the new
#     desktop is in the monitor's row
# Control (--control --control-so a4d6892): the dialog stayed on a's desktop
# (floats were skipped) and there was no way back. Recorded 2026-11-11 on
# a4d6892: control FAILS as it must.
source "$(dirname "$0")/lib.sh"
gate_begin bes
control_build "a4d6892, e.g. /home/dragos/Projects/hyprdrive-native/hyprdrive.so"
cc -O2 -o "$GOUT/x_place" "$ROOT/tests/rig/xstate/x_place.c" $(pkg-config --cflags --libs xcb) || skip "cannot build x_place (xcb headers?)"
chmod 755 "$GOUT" "$GOUT/x_place"
xdisplay() { hlog 4000 | grep -o 'DISPLAY: :[0-9]*' | tail -1 | grep -o ':[0-9]*$'; }
active() { J activewindow | jq -r .class; }
asked() { # asked KIND CLASS -- through the compositor (a launch: the user asked)
    ev "hl.dispatch(hl.dsp.exec_cmd(\"env DISPLAY=$D $GOUT/x_place $1 $2 60\"))"
    wait_for 5 "[ -n \"\$(addr_of $2)\" ]" || echo "warn: $2 never mapped"
}
unasked() { $R win env DISPLAY="$D" "$GOUT/x_place" "$1" "$2" 60 >/dev/null; wait_for 5 "[ -n \"\$(addr_of $2)\" ]" || echo "warn: $2 never mapped"; }
xclose() { close_win "$(addr_of "$1")"; }

rig_start; trace_on
go 1; kw a; kw b; go 2; kw c; go 1; sleep 0.4
wait_for 6 '[ -n "$(xdisplay)" ]' || { $R win env DISPLAY=:99 true >/dev/null 2>&1; wait_for 6 '[ -n "$(xdisplay)" ]'; }
D=$(xdisplay); [ -n "$D" ] || skip "XWayland never announced a display"
A=$(addr_of a); focus "$A"; fs "$A" set; sleep 1.2; AW=$(win a .workspace.id); START=$(ids)
check "the app went to 2" "$(is "$AW" 2)" "a on $AW ($START)"

# PROMPTED
asked dialog pick1; sleep 0.9; settle 3 ids
P1=$(win pick1 .workspace.id)
check "prompted: the dialog opens on the desktop right after the app's" "$(is "$P1" $((AW + 1)))" "app on $AW, dialog on $P1, $(ids)"
check "prompted: shown" "$(is "$(shown)" "$P1")" "shown $(shown)"
check "prompted: focused" "$(is "$(active)" pick1)" "active $(active)"
check "the app stays fullscreen on its desktop" "$(is "$(win a .fullscreen)/$(win a .workspace.id)" "2/$AW")" "$(win a .fullscreen)/$(win a .workspace.id)"
go "$AW"; sleep 0.9 # back on the app, the first one still open
asked dialog pick2; sleep 0.9; settle 3 ids
check "a second one joins the first one's desktop, shown" "$(is "$(win pick2 .workspace.id)/$(shown)" "$P1/$P1")" "pick2 on $(win pick2 .workspace.id), shown $(shown), $(ids)"
xclose pick2; sleep 0.8
check "one of two closed: still there" "$(is "$(shown)" "$P1")" "shown $(shown)"
xclose pick1; wait_for 4 "[ \"\$(shown)\" = $AW ]"; sleep 0.8; settle 3 ids
check "closed: back on the fullscreen app" "$(is "$(shown)/$(active)" "$AW/a")" "shown $(shown), active $(active)"
check "closed: the desktop made for it is gone" "$(is "$(ids)" "$START")" "$(ids) (was $START)"

# UNPROMPTED
sleep 1.6 # no click / Enter just before
unasked dialog quiet1; sleep 0.9; settle 3 ids
Q1=$(win quiet1 .workspace.id)
check "unprompted: on the next desktop" "$(is "$Q1" $((AW + 1)))" "quiet1 on $Q1"
check "unprompted: the view stays on the app, focus kept" "$(is "$(shown)/$(active)" "$AW/a")" "shown $(shown), active $(active)"
check "unprompted: the trace says it went quietly" "$(ge "$(trace | grep -c 'unasked: not shown')" 1)" ""
xclose quiet1; sleep 1.2; settle 3 ids
check "unprompted, closed: nothing moves" "$(is "$(shown)/$(ids)" "$AW/$START")" "shown $(shown), $(ids)"

# CLOSED DURING THE GLIDE
ev "hl.dispatch(hl.dsp.exec_cmd(\"env DISPLAY=$D $GOUT/x_place dialog glide 60\"))"
wait_for 5 '[ -n "$(addr_of glide)" ]'; xclose glide
wait_for 4 "[ \"\$(shown)\" = $AW ]"; sleep 1; settle 3 ids
check "closed during the glide: back on the app, desktops as before" "$(is "$(shown)/$(ids)" "$AW/$START")" "shown $(shown), $(ids)"

# 30 CYCLES
for i in $(seq 1 30); do
    asked dialog cyc$i; sleep 0.25; xclose cyc$i; sleep 0.25
done
wait_for 6 "[ \"\$(ids)\" = '$START' ]"; sleep 0.5
check "30 open/close cycles: no desktop left behind" "$(is "$(ids)/$(shown)" "$START/$AW")" "$(ids), shown $(shown)"

# A SETTLED APP (alone on its desktop), a tiled window this time
fs "$A" unset; sleep 1.2; go 5; kw solo; S=$(addr_of solo); fs "$S" set; sleep 1; SW=$(win solo .workspace.id); START2=$(ids)
kexec tilednew; sleep 0.9; settle 3 ids
check "settled app: the new window goes beside it, shown" "$(is "$(win tilednew .workspace.id)/$(shown)" "$((SW + 1))/$((SW + 1))")" "solo on $SW, new on $(win tilednew .workspace.id), shown $(shown)"
close_win "$(addr_of tilednew)"; wait_for 4 "[ \"\$(shown)\" = $SW ]"; sleep 0.8; settle 3 ids
check "settled app: closed, back on it" "$(is "$(shown)/$(ids)" "$SW/$START2")" "shown $(shown), $(ids) (was $START2)"
gate_end
