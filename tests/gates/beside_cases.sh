#!/usr/bin/env bash
# beside_cases -- Beside in a monitor's row, over a game, a dialog's own dialog, the app closing first, and under fullscreen_desktop = stay.
# Protects (owner, 2026-11-11): "if a floating window shows up over a
# fullscreen one, it should be moved to the next desktop ... upon closing the
# window, user should be sent back to the fullscreen app"; "unprompted ... not
# be focused"; the per-monitor rows (Beside is in the app's monitor's row, a
# block's 10th desktop is followed by the row's overflow); beside.sh has the
# plain cases and the cycles, this one the rest (split for the runner's time
# limit).
# Asserts:
#   A. per-monitor rows (Omarchy's blocks, blocks.lib): a prompted window over
#      a fullscreen app on HEADLESS-1 goes to the next desktop in HEADLESS-1's
#      row; nothing in HEADLESS-2's row moves; closed: back on the app, the
#      desktops as before. From the block's 10th desktop it goes to the row's
#      overflow, never to 11 (HEADLESS-2's block) -- a full block: one with
#      room pulls its overflow in (blocks_overflow.sh).
#   B. a GAME (an X11 `.exe` class, FullscreenGuard::isGame): a prompted dialog
#      goes beside, focused; closed, the game has the focus again and is still
#      fullscreen, not minimized (FullscreenGuard); an unprompted one leaves
#      the view and the focus on the game.
#   C. a prompted dialog that opens a second window (launched from its
#      desktop): the second one joins it; the view stays until BOTH close,
#      then goes back to the app.
#   D. the fullscreen app closes first: the dialog stays where it is; closed
#      too, no desktop is left behind.
#   E. fullscreen_desktop = "stay": no Beside, the dialog opens on the app's
#      desktop, nothing moves.
# Control (--control --control-so a4d6892): B/C/D fail (the dialog stayed
# over the app: floats were never moved beside).
source "$(dirname "$0")/lib.sh"
gate_begin bsc
control_build "a4d6892, e.g. /home/dragos/Projects/hyprdrive-native/hyprdrive.so"
source "$(dirname "$0")/blocks.lib"
cc -O2 -o "$GOUT/x_place" "$ROOT/tests/rig/xstate/x_place.c" $(pkg-config --cflags --libs xcb) || skip "cannot build x_place (xcb headers?)"
chmod 755 "$GOUT" "$GOUT/x_place"
xdisplay() { hlog 4000 | grep -o 'DISPLAY: :[0-9]*' | tail -1 | grep -o ':[0-9]*$'; }
xup() {
    wait_for 6 '[ -n "$(xdisplay)" ]' || { $R win env DISPLAY=:99 true >/dev/null 2>&1; wait_for 6 '[ -n "$(xdisplay)" ]'; }
    D=$(xdisplay); [ -n "$D" ] || skip "XWayland never announced a display"
}
active() { J activewindow | jq -r .class; }
asked() { # asked KIND CLASS -- launched through the compositor (the user asked)
    ev "hl.dispatch(hl.dsp.exec_cmd(\"env DISPLAY=$D $GOUT/x_place $1 $2 60\"))"
    wait_for 5 "[ -n \"\$(addr_of $2)\" ]" || echo "warn: $2 never mapped"
}
unasked() { $R win env DISPLAY="$D" "$GOUT/x_place" "$1" "$2" 60 >/dev/null; wait_for 5 "[ -n \"\$(addr_of $2)\" ]" || echo "warn: $2 never mapped"; }
xclose() { close_win "$(addr_of "$1")"; }
monof() { J clients | jq -r ".[] | select(.class == \"$1\") | .monitor" | head -1; }
wsmon() { J workspaces | jq -r ".[] | select(.id == $1) | .monitor"; }

# ---------------------------------------------------------------- A (rows)
seed '0\tHEADLESS-1\tHEADLESS-1\n1\tHEADLESS-2\tHEADLESS-2\n'
blocks_start
focus_mon HEADLESS-1; go 1; kw a; kw b; go 2; kw c
$R mon2 >/dev/null; sleep 1
focus_mon HEADLESS-2; go 11; kw m; go 12; kw n
focus_mon HEADLESS-1; go 1; sleep 0.4
A=$(addr_of a); focus "$A"; fs "$A" set; sleep 1.2; settled
AW=$(win a .workspace.id); START=$(ids); TWO=$(popon HEADLESS-2); MN="$(win m .workspace.id)/$(win n .workspace.id)"
check "A: setup: the app went to 2 on HEADLESS-1" "$(is "$AW/$(wsmon "$AW")" 2/HEADLESS-1)" "a on $AW ($START)"
kexec rowpick; sleep 0.9; settle 3 ids
RP=$(win rowpick .workspace.id)
check "A: the window goes to the next desktop of HEADLESS-1's row, shown" "$(is "$RP/$(wsmon "$RP")/$(shown HEADLESS-1)" "$((AW + 1))/HEADLESS-1/$((AW + 1))")" "rowpick on $RP ($(wsmon "$RP")), shown $(shown HEADLESS-1), $(ids)"
check "A: nothing in HEADLESS-2's row moved" "$(is "$(popon HEADLESS-2)|$(win m .workspace.id)/$(win n .workspace.id)" "$TWO|$MN")" "$(popon HEADLESS-2), m/n $(win m .workspace.id)/$(win n .workspace.id)"
close_win "$(addr_of rowpick)"; wait_for 4 "[ \"\$(shown HEADLESS-1)\" = $AW ]"; sleep 0.8; settle 3 ids
check "A: closed: back on the app, the desktops as before" "$(is "$(shown HEADLESS-1)/$(active)/$(ids)" "$AW/a/$START")" "shown $(shown HEADLESS-1), active $(active), $(ids) (was $START)"
# A2: the app fullscreen on the block's 10th desktop -> the next place is the row's overflow
fs "$A" unset; sleep 1.2; settled
for n in 3 4 5 6 7 8; do go $n; kw "f$n" 500 400; done # a full block (one with room pulls its overflow in)
go 9; kw g; kw h; go 10; kw i; go 9; sleep 0.4
G=$(addr_of g); focus "$G"; fs "$G" set; sleep 1.2; settled
GW=$(win g .workspace.id); START2=$(ids)
kexec lastpick; sleep 0.9; settle 3 ids
LP=$(win lastpick .workspace.id)
check "A2: from the block's 10th desktop: the row's overflow, on HEADLESS-1" "$(is "$GW|$(ge "$LP" 1000000)|$(wsmon "$LP")" "10|1|HEADLESS-1")" "g on $GW, lastpick on $LP ($(wsmon "$LP")), $(ids)"
check "A2: nothing in HEADLESS-2's row moved" "$(is "$(popon HEADLESS-2)|$(win m .workspace.id)/$(win n .workspace.id)" "$TWO|$MN")" "$(popon HEADLESS-2)"
close_win "$(addr_of lastpick)"; wait_for 4 "[ \"\$(shown HEADLESS-1)\" = $GW ]"; sleep 0.8; settle 3 ids
check "A2: closed: back on the app, the desktops as before" "$(is "$(shown HEADLESS-1)/$(ids)" "$GW/$START2")" "shown $(shown HEADLESS-1), $(ids) (was $START2)"
rig_stop

# ---------------------------------------------------------------- B (a game)
rig_start; trace_on
go 1; kw b; sleep 0.4; xup
unasked normal game.exe; GA=$(addr_of game.exe); focus "$GA"; fs "$GA" set; sleep 1.2; settle 3 ids
GW=$(win game.exe .workspace.id); START=$(ids)
check "B: setup: the game is fullscreen on a desktop of its own" "$(is "$(win game.exe .fullscreen)/$GW" "2/2")" "$(win game.exe .fullscreen)/$GW ($START)"
asked dialog gpick; sleep 0.9; settle 3 ids
check "B: prompted over a game: beside, shown, focused" "$(is "$(win gpick .workspace.id)/$(shown)/$(active)" "$((GW + 1))/$((GW + 1))/gpick")" "gpick on $(win gpick .workspace.id), shown $(shown), active $(active)"
check "B: the game stays fullscreen on its desktop" "$(is "$(win game.exe .fullscreen)/$(win game.exe .workspace.id)" "2/$GW")" "$(win game.exe .fullscreen)/$(win game.exe .workspace.id)"
xclose gpick; wait_for 4 "[ \"\$(shown)\" = $GW ]"; sleep 0.8; settle 3 ids
check "B: closed: back on the game, focused, still fullscreen, not minimized" "$(is "$(shown)/$(active)/$(win game.exe .fullscreen)/$(win game.exe .hidden)" "$GW/game.exe/2/false")" "shown $(shown), active $(active), fs $(win game.exe .fullscreen), hidden $(win game.exe .hidden)"
check "B: closed: the desktops as before" "$(is "$(ids)" "$START")" "$(ids) (was $START)"
sleep 1.6
unasked dialog gquiet; sleep 0.9; settle 3 ids
check "B: unprompted over a game: beside, the view and the focus stay on the game" "$(is "$(win gquiet .workspace.id)/$(shown)/$(active)/$(win game.exe .fullscreen)" "$((GW + 1))/$GW/game.exe/2")" "gquiet on $(win gquiet .workspace.id), shown $(shown), active $(active)"
xclose gquiet; sleep 1.2; settle 3 ids
check "B: unprompted, closed: nothing moves" "$(is "$(shown)/$(ids)" "$GW/$START")" "shown $(shown), $(ids)"

# ---------------------------------------------------------------- C (a dialog's own dialog)
asked dialog outer; sleep 0.9; settle 3 ids
OW=$(win outer .workspace.id)
asked dialog inner; sleep 0.9; settle 3 ids # launched with the view on outer's desktop
check "C: the second window joins the first one's desktop" "$(is "$(win inner .workspace.id)/$(shown)" "$OW/$OW")" "outer on $OW, inner on $(win inner .workspace.id), shown $(shown)"
xclose outer; sleep 1; settle 3 ids
check "C: the first one closed: the view stays (the second is still there)" "$(is "$(shown)" "$OW")" "shown $(shown)"
xclose inner; wait_for 4 "[ \"\$(shown)\" = $GW ]"; sleep 0.8; settle 3 ids
check "C: both closed: back on the game, the desktops as before" "$(is "$(shown)/$(active)/$(ids)" "$GW/game.exe/$START")" "shown $(shown), active $(active), $(ids)"

# ---------------------------------------------------------------- D (the app closes first)
asked dialog orphan; sleep 0.9; settle 3 ids
DW=$(win orphan .workspace.id)
close_win "$GA"; sleep 1.2; settle 3 ids
check "D: the app closed first: the dialog stays, shown" "$(is "$(shown)" "$(win orphan .workspace.id)")" "shown $(shown), orphan on $(win orphan .workspace.id) (was $DW), $(ids)"
xclose orphan; sleep 1; go 1; sleep 1.2; settle 3 ids
check "D: then the dialog: no desktop left behind" "$(is "$(ids)" "1:1")" "$(ids)"

# ---------------------------------------------------------------- E (stay)
cfg '{plugin={hyprdrive={general={fullscreen_desktop="stay"}}}}'
kw s2; S2=$(addr_of s2); focus "$S2"; fs "$S2" set; sleep 1.2; settle 3 ids; START=$(ids)
asked dialog stayed; sleep 0.9; settle 3 ids
check "E: stay: no Beside -- the dialog opens on the app's desktop, nothing moves" "$(is "$(win stayed .workspace.id)/$(ids)" "1/1:3")" "stayed on $(win stayed .workspace.id), $(ids) (was $START)"
check "E: stay: the trace has no beside for it" "$(is "$(trace | grep -c 'stayed.*opened over fullscreen')" 0)" ""
gate_end
