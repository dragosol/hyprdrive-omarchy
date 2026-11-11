#!/usr/bin/env bash
# blocks -- Omarchy's per-monitor workspace blocks: no renumbering fight, each strip/row is its monitor's block.
# Protects: the owner's report 2026-11-11 (XPS: desktops jumping between 1-4
# and 21-24, "20 empty desktops" on the Samsung, which owns block 2) and the
# decision "per monitor workspace blocks - adopt but don't keep empty
# workspaces generated on config switch" + "no limit" (a full block overflows).
# The rig has no Omarchy: tests/rig/headless/blocks.lua runs the emulation
# omarchy-blocks.lua (renumbers with hl.dsp.workspace.change_id, as Omarchy 4.5
# does) and loads Hyprdrive through the real 00-plugin.lua publisher.
# Asserts:
#   A. two outputs, HEADLESS-1 = block 0, HEADLESS-2 = block 1; windows on 1 2
#      (output 1) and 11 12 13 (output 2). Twice: a scale change on output 1
#      then `hyprctl reload` -> no populated desktop outside its output's
#      block, no empty unshown desktop, ZERO changeworkspaceid events (no
#      fight), Hyprdrive renumbers nothing. Output 2's row reads 11=1 12=2 13=3;
#      its overview creates nothing below 11 and its spare is 14; Super+scroll
#      on it walks 11..14 only. Fullscreen with a second window on 11: the app
#      gets 12, the rest of block 1 moves up (13 14), block 0 untouched.
#   B. the 11th desktop: 11..20 all populated (two windows on 19); fullscreen
#      on 19 -> the app takes 20 and the old 20 goes to block 1's overflow
#      (1001001), never to 21; a reload: zero events (a full block + overflow
#      is a fixed point); out of fullscreen -> 20 comes back, no overflow left.
#   C. the owner's laptop: one output owning block 2 (block 0's panel
#      unplugged); windows on 21..24: scale change + reload twice -> nothing
#      moves, zero events, no desktop created below 21 by the overview,
#      Super+scroll walks 21..25.
#   D. the state the old build left: windows on 1..4 on that output. The first
#      reload merges them to 21..24 with records (4 events, Omarchy's); then
#      scale + reload twice -> zero events, 21..24 pinned and kept.
# Control: --so 3423fe4 (/home/dragos/Projects/hyprdrive-native/hyprdrive.so,
# before blocks): the scale change compacts to one global row and every reload
# renumbers back -- A and C fail (changeworkspaceid > 0, desktops outside their
# block, empties below 21).
source "$(dirname "$0")/lib.sh"
gate_begin blk
control_build "3423fe4, e.g. /home/dragos/Projects/hyprdrive-native/hyprdrive.so"

RIG=$ROOT/tests/rig/headless
seed() { # seed TEXT -- Omarchy's monitor-workspaces before the start
    SU bash -c "mkdir -p '$ST/state/omarchy' && chmod 755 '$ST' '$ST/state' '$ST/state/omarchy' && rm -f '$ST/state/omarchy/'* && printf '$1' > '$ST/state/omarchy/monitor-workspaces'"
}
blocks_start() {
    rig_start --config "$RIG/blocks.lua" --env "HD_BLOCKS_LUA=$RIG/omarchy-blocks.lua" --env "HD_PLUGIN_LUA=$ROOT/config/hyprdrive/00-plugin.lua" \
        --env "HYPRDRIVE_SO=$SO" --env "XDG_STATE_HOME=$ST/state"
    trace_on
}
events_start() { : > "$GOUT/events.log"; SU socat -u "UNIX-CONNECT:$RT/hypr/$SIG/.socket2.sock" - > "$GOUT/events.log" 2>/dev/null & _bg_pids="${_bg_pids:-} $!"; sleep 0.3; }
ncw() { grep -c '^changeworkspaceid>>' "$GOUT/events.log"; }              # changeworkspaceid so far
cwlist() { grep '^changeworkspaceid>>' "$GOUT/events.log" | sed 's/changeworkspaceid>>//' | tr '\n' ' '; }
ncompact() { refused | grep -c 'no empty desktops between'; }               # Hyprdrive's compactions so far
on() { J workspaces | jq -r "[.[] | select(.monitor == \"$1\" and .id > 0) | .id] | sort | map(tostring) | join(\" \")"; }
popon() { J workspaces | jq -r "[.[] | select(.monitor == \"$1\" and .id > 0 and .windows > 0) | .id] | sort | map(tostring) | join(\" \")"; }
outside() { # outside MON FIRST LAST -- populated desktops on MON outside FIRST..LAST (overflow ids allowed: >= 1000000)
    J workspaces | jq -r "[.[] | select(.monitor == \"$1\" and .id > 0 and .windows > 0 and (.id < $2 or (.id > $3 and .id < 1000000))) | .id] | map(tostring) | join(\" \")"; }
empties() { # empty desktops no monitor shows
    local shownids; shownids=$(J monitors | jq -c '[.[] | .activeWorkspace.id]')
    J workspaces | jq -r --argjson s "$shownids" '[.[] | select(.id > 0 and .windows == 0 and ((.id as $i | $s | index($i)) | not)) | .id] | map(tostring) | join(" ")'; }
scale() { ev "hl.monitor({ output = \"HEADLESS-1\", mode = \"1920x1080@60\", position = \"0x0\", scale = $1 })"; }
reload() { hq reload >/dev/null; }
focus_mon() { ev "hl.dispatch(hl.dsp.focus({ monitor = \"$1\" }))"; sleep 0.2; }
wheel() { hq dispatch "hl.plugin.hyprdrive.desktop(\"$1\")" >/dev/null 2>&1; sleep 0.45; }
rowsline() { dbg rows | awk -v m="$1:" '$1 == m { getline; print; exit }' | sed 's/^ *members: *//'; }
settled() { sleep 0.3; settle 4 'ids'; }
# one "config switch" round: a display change, then a reload; prints the
# changeworkspaceid count it caused
round() {
    local c0; c0=$(ncw)
    scale "$1"; sleep 1.2; settled
    reload; sleep 1.2; settled
    echo $(( $(ncw) - c0 ))
}

# ---------------------------------------------------------------- A
seed '0\tHEADLESS-1\tHEADLESS-1\n1\tHEADLESS-2\tHEADLESS-2\n'
blocks_start
focus_mon HEADLESS-1; go 1; kw a1; go 2; kw a2
$R mon2 >/dev/null; sleep 1   # (after 1 and 2 exist: Hyprland gives a new output the first free id)
focus_mon HEADLESS-2; go 11; kw b11; go 12; kw b12; go 13; kw b13; settled
events_start
C0=$(ncompact)
check "A: start 1 2 | 11 12 13" "$(is "$(popon HEADLESS-1)|$(popon HEADLESS-2)" '1 2|11 12 13')" "$(popon HEADLESS-1)|$(popon HEADLESS-2)"
N1=$(round 1.25); IDS1="$(popon HEADLESS-1)|$(popon HEADLESS-2)"
N2=$(round 1); IDS2="$(popon HEADLESS-1)|$(popon HEADLESS-2)"
echo "      A rounds: changeworkspaceid $N1 then $N2 [$(cwlist)]; ids $IDS1 -> $IDS2"
check "A: round 1 (scale + reload): zero changeworkspaceid" "$(is "$N1" 0)" "$N1: $(cwlist)"
check "A: round 2: zero changeworkspaceid (no fight)" "$(is "$N2" 0)" "$N2: $(cwlist)"
check "A: ids kept: 1 2 | 11 12 13" "$(is "$IDS2" '1 2|11 12 13')" "$IDS1 -> $IDS2"
check "A: Hyprdrive renumbered nothing" "$(is "$(( $(ncompact) - C0 ))" 0)" "$(refused | tail -2 | tr '\n' ';')"
check "A: nothing outside its block" "$(is "$(outside HEADLESS-1 1 10)|$(outside HEADLESS-2 11 20)" '|')" "$(outside HEADLESS-1 1 10)|$(outside HEADLESS-2 11 20)"
check "A: no empty unshown desktops" "$(is "$(empties)" '')" "$(empties)"
if [ "$CONTROL" != 1 ]; then
    check "A: output 2's row reads 11=1 12=2 13=3" "$(is "$(rowsline HEADLESS-2)" '11=1 12=2 13=3')" "$(rowsline HEADLESS-2)"
fi
focus_mon HEADLESS-2; go 11; sleep 0.5
$R overview >/dev/null; sleep 1.5
OV2=$(on HEADLESS-2)
check "A: overview on output 2 creates nothing below 11; spare 14" "$(is "$OV2" '11 12 13 14')" "$OV2"
$R overview >/dev/null; sleep 1.2; settled
focus_mon HEADLESS-2; go 11; sleep 0.5
WALK=""; for _ in 1 2 3 4 5; do wheel next; WALK="$WALK $(shown HEADLESS-2)"; done
for _ in 1 2 3 4 5; do wheel prev; WALK="$WALK $(shown HEADLESS-2)"; done
check "A: Super+scroll on output 2 walks 11..14 only" "$(is "$WALK" ' 12 13 14 14 14 13 12 11 11 11')" "$WALK"
settled
# fullscreen in block 1: a second window on 11, the app goes fullscreen
go 11; kw b11b; settled
A11=$(addr_of b11); focus "$A11"; fs "$A11" set; sleep 1.5; settled
check "A: fullscreen in block 1: app on 12, 12 13 -> 13 14" "$(is "$(win b11 .workspace.id) $(popon HEADLESS-2)" '12 11 12 13 14')" "app on $(win b11 .workspace.id), $(popon HEADLESS-2)"
check "A: ... block 0 untouched" "$(is "$(popon HEADLESS-1)" '1 2')" "$(popon HEADLESS-1)"
fs "$A11" unset; sleep 1.5; settled
check "A: out of fullscreen: home 11, 11 12 13 again" "$(is "$(win b11 .workspace.id) $(popon HEADLESS-2)" '11 11 12 13')" "app on $(win b11 .workspace.id), $(popon HEADLESS-2)"
check "A: no changeworkspaceid all along" "$(is "$(ncw)" 0)" "$(cwlist)"
rig_stop

# ---------------------------------------------------------------- B (11th desktop)
if [ "$CONTROL" != 1 ]; then
seed '0\tHEADLESS-1\tHEADLESS-1\n1\tHEADLESS-2\tHEADLESS-2\n'
blocks_start
focus_mon HEADLESS-1; go 1; kw a1
$R mon2 >/dev/null; sleep 1
focus_mon HEADLESS-2
for n in 11 12 13 14 15 16 17 18 19 20; do go $n; kw "b$n" 500 400; done
go 19; kw b19b 500 400; settled
events_start
A19=$(addr_of b19); focus "$A19"; fs "$A19" set; sleep 1.5; settled
check "B: the 11th: app on 20, the old 20 in block 1's overflow" "$(is "$(win b19 .workspace.id) $(win b20 .workspace.id)" '20 1001001')" "app on $(win b19 .workspace.id), b20 on $(win b20 .workspace.id); $(popon HEADLESS-2)"
check "B: never 21 (block 2's)" "$(is "$(J workspaces | jq '[.[] | select(.id == 21)] | length')" 0)" "$(ids)"
check "B: block 0 untouched" "$(is "$(popon HEADLESS-1)" '1')" "$(popon HEADLESS-1)"
c0=$(ncw); reload; sleep 1.5; settled
check "B: a reload with a full block + overflow: zero events" "$(is "$(( $(ncw) - c0 ))" 0)" "$(cwlist)"
check "B: ... and the overflow stays" "$(is "$(win b20 .workspace.id)" 1001001)" "b20 on $(win b20 .workspace.id)"
fs "$A19" unset; sleep 2; settled
check "B: out of fullscreen: 19 home, 20 back from overflow" "$(is "$(win b19 .workspace.id) $(win b20 .workspace.id) $(popon HEADLESS-2)" '19 20 11 12 13 14 15 16 17 18 19 20')" "b19 $(win b19 .workspace.id) b20 $(win b20 .workspace.id); $(popon HEADLESS-2)"
check "B: no changeworkspaceid all along" "$(is "$(ncw)" 0)" "$(cwlist)"
rig_stop
fi

# ---------------------------------------------------------------- C (the owner's laptop)
seed '0\t\tPANEL\n2\tHEADLESS-1\tHEADLESS-1\n'
blocks_start
focus_mon HEADLESS-1; for n in 21 22 23 24; do go $n; kw "c$n"; done; settled
events_start
C0=$(ncompact)
N1=$(round 1.25); IDS1=$(popon HEADLESS-1)
N2=$(round 1); IDS2=$(popon HEADLESS-1)
echo "      C rounds: changeworkspaceid $N1 then $N2 [$(cwlist)]; ids $IDS1 -> $IDS2"
check "C: block 2: two config switches, zero changeworkspaceid" "$(is "$N1 $N2" '0 0')" "$N1 $N2: $(cwlist)"
check "C: 21 22 23 24 kept" "$(is "$IDS2" '21 22 23 24')" "$IDS1 -> $IDS2"
check "C: Hyprdrive renumbered nothing" "$(is "$(( $(ncompact) - C0 ))" 0)" "$(refused | tail -2 | tr '\n' ';')"
if [ "$CONTROL" != 1 ]; then
    check "C: labels 1..4" "$(is "$(rowsline HEADLESS-1)" '21=1 22=2 23=3 24=4')" "$(rowsline HEADLESS-1)"
fi
go 21; sleep 0.4
$R overview >/dev/null; sleep 1.5
OV=$(on HEADLESS-1)
check "C: the overview creates no desktop below 21 (no 20 empties); spare 25" "$(is "$OV" '21 22 23 24 25')" "$OV"
$R overview >/dev/null; sleep 1.2; settled
go 21; sleep 0.4
WALK=""; for _ in 1 2 3 4 5; do wheel next; WALK="$WALK $(shown HEADLESS-1)"; done
check "C: Super+scroll walks 21..25" "$(is "$WALK" ' 22 23 24 25 25')" "$WALK"
rig_stop

# ---------------------------------------------------------------- D (what the old build left)
if [ "$CONTROL" != 1 ]; then
seed '0\t\tPANEL\n2\tHEADLESS-1\tHEADLESS-1\n'
blocks_start
focus_mon HEADLESS-1; for n in 1 2 3 4; do go $n; kw "d$n"; done; settled
events_start
reload; sleep 1.5; settled
check "D: the first reload merges 1..4 to 21..24 (Omarchy's 4 events)" "$(is "$(ncw) $(popon HEADLESS-1)" '4 21 22 23 24')" "$(cwlist) / $(popon HEADLESS-1)"
check "D: Hyprdrive pins the guests" "$(is "$(dbg rows | grep -c 'pinned (merged): 21 22 23 24')" 1)" "$(dbg rows | grep pinned)"
c0=$(ncw)
N1=$(round 1.25); N2=$(round 1)
check "D: then two config switches: zero events" "$(is "$N1 $N2" '0 0')" "$N1 $N2: $(cwlist)"
check "D: 21..24 kept, nothing empty left" "$(is "$(popon HEADLESS-1)|$(empties)" '21 22 23 24|')" "$(popon HEADLESS-1)|$(empties)"
fi

gate_end
