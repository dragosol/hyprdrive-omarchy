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
#      Fullscreen on 20 itself (the block's last, with company) stays on 20:
#      the book's home+1 is block 2's 21, never taken (part B: overflow).
#   C. the owner's laptop: one output owning block 2 (block 0's panel
#      unplugged); windows on 21..24: scale change + reload twice -> nothing
#      moves, zero events, no desktop created below 21 by the overview,
#      Super+scroll walks 21..25.
#   D. the state the old build left: windows on 1..4 on that output. The first
#      reload merges them to 21..24 with records (4 events, Omarchy's); then
#      scale + reload twice -> zero events, 21..24 pinned and kept.
#   E. Omarchy meets the overflow (an overflow id is a guest to its reconcile:
#      merge() pulls it into a block with room, origin identity nil, no record):
#      a slot frees and the config reloads before Hyprdrive compacts -> Omarchy
#      pulls it in, no Lua error, next reloads quiet; Hyprdrive compacting first
#      -> the same, quiet; an empty overflow desktop on view at a reload -> no
#      renumber (Omarchy's own_views puts the monitor back on its home).
#   With GATE_OMARCHY_MW=/abs/monitor-workspaces.lua (a copy of Omarchy 4.5's
#   own script; never committed) the REAL script runs instead of the emulation
#   (blocks.lua), and each case also checks it was the one loaded and that none
#   of its hl.on / hl.timer callbacks raised.
# Control: --so 3423fe4 (/home/dragos/Projects/hyprdrive-native/hyprdrive.so,
# before blocks): the scale change compacts to one global row and every reload
# renumbers back -- A and C fail (changeworkspaceid > 0, desktops outside their
# block, empties below 21).
source "$(dirname "$0")/lib.sh"
gate_begin blk
control_build "3423fe4, e.g. /home/dragos/Projects/hyprdrive-native/hyprdrive.so"

RIG=$ROOT/tests/rig/headless
seed() { # seed TEXT -- Omarchy's monitor-workspaces before the start
    SU bash -c "mkdir -p '$ST/state/omarchy' && chmod 755 '$ST' '$ST/state' '$ST/state/omarchy' && rm -f '$ST/state/omarchy/'* '$ST/state/lua-errors.log' '$ST/state/omarchy-real.loaded' && printf '$1' > '$ST/state/omarchy/monitor-workspaces'"
}
# GATE_OMARCHY_MW=/abs/monitor-workspaces.lua: run Omarchy's REAL script (a
# copy from an Omarchy 4.5 machine; never committed) instead of the emulation.
# Its callbacks' errors go to $ST/state/lua-errors.log (blocks.lua).
blocks_start() {
    local mw=(--env "HD_BLOCKS_LUA=$RIG/omarchy-blocks.lua")
    [ -n "${GATE_OMARCHY_MW:-}" ] && mw=(--env "HD_OMARCHY_MW=$GATE_OMARCHY_MW" --env "HD_LUA_ERRLOG=$ST/state/lua-errors.log")
    rig_start --config "$RIG/blocks.lua" "${mw[@]}" --env "HD_PLUGIN_LUA=$ROOT/config/hyprdrive/00-plugin.lua" \
        --env "HYPRDRIVE_SO=$SO" --env "XDG_STATE_HOME=$ST/state"
    trace_on
}
# lua_ok CASE -- no error in Omarchy's callbacks (the real script) so far
lua_ok() {
    [ -n "${GATE_OMARCHY_MW:-}" ] || return 0
    local e; e=$(SU cat "$ST/state/lua-errors.log" 2>/dev/null | head -3 | tr '\n' ' ')
    check "$1: Omarchy's real script is the one loaded" "$(SU grep -c ' o.workspaces$' "$ST/state/omarchy-real.loaded" 2>/dev/null || echo 0)" "$(SU cat "$ST/state/omarchy-real.loaded" 2>&1)"
    check "$1: no error in Omarchy's Lua callbacks" "$(is "$e" '')" "$e"
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
lua_ok A
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
# part A's known gap: fullscreen on the block's LAST desktop with company. The
# fullscreen book still counts home+1 = 21 (block 2's first id): that place is
# never taken -- the app goes fullscreen where it is (part B: the overflow).
go 20; kw b20b 500 400; settled
A20=$(addr_of b20); focus "$A20"; fs "$A20" set; sleep 1.5; settled
check "B: fullscreen on the block's last desktop: stays on 20, fullscreen, no 21" "$(is "$(win b20 .workspace.id) $(win b20 .fullscreen) $(J workspaces | jq '[.[] | select(.id == 21)] | length')" '20 2 0')" "b20 on $(win b20 .workspace.id) fs $(win b20 .fullscreen); $(ids)"
fs "$A20" unset; sleep 1; settled
lua_ok B
rig_stop
fi

# ---------------------------------------------------------------- E (overflow meets Omarchy)
# An overflow id (1000000 + block*1000 + n) is in no block Omarchy knows, so
# its reconcile treats a populated one as a GUEST: merge() renumbers it into
# the block when the block has room (append_slot), and its origin identity is
# identity_of_block(100100) = nil -> no merged record. E1: a slot frees and
# the config reloads BEFORE Hyprdrive's compaction -> Omarchy pulls the
# overflow in itself; must not error, and the next reload is quiet. E2: the
# same, Hyprdrive's compaction first -> it pulls the overflow in; reloads quiet.
# E3: an EMPTY overflow desktop on view (the spare of a full block) at a
# reload: Omarchy's own_views() puts the monitor back on its home (a view
# change, not a renumber).
if [ "$CONTROL" != 1 ]; then
full_block() {
    seed '0\tHEADLESS-1\tHEADLESS-1\n1\tHEADLESS-2\tHEADLESS-2\n'
    blocks_start
    focus_mon HEADLESS-1; go 1; kw a1
    $R mon2 >/dev/null; sleep 1
    focus_mon HEADLESS-2
    for n in 11 12 13 14 15 16 17 18 19 20; do go $n; kw "e$n" 500 400; done
    go 19; kw e19b 500 400; settled
    A19=$(addr_of e19); focus "$A19"; fs "$A19" set; sleep 1.5; settled
}
full_block
events_start
check "E1: setup: e20 in the overflow" "$(is "$(win e20 .workspace.id)" 1001001)" "e20 on $(win e20 .workspace.id); $(popon HEADLESS-2)"
close_win "$(addr_of e15)"; sleep 0.15; reload; sleep 1.5; settled
E1="$(cwlist)"; E1N=$(ncw)
echo "      E1 (reload first): changeworkspaceid [$E1]; $(popon HEADLESS-2); e20 on $(win e20 .workspace.id)"
check "E1: the overflow is back in block 1, nothing outside it" "$(is "$(outside HEADLESS-2 11 20)|$(J workspaces | jq '[.[] | select(.id >= 1000000 and .windows > 0)] | length')" '|0')" "$(ids)"
c0=$(ncw); reload; sleep 1.5; settled; reload; sleep 1.5; settled
check "E1: then two reloads: zero events" "$(is "$(( $(ncw) - c0 ))" 0)" "$(cwlist)"
check "E1: no empty unshown desktops" "$(is "$(empties)" '')" "$(empties)"
lua_ok E1
rig_stop
full_block
events_start
close_win "$(addr_of e15)"; sleep 3; settled
echo "      E2 (Hyprdrive first): changeworkspaceid [$(cwlist)]; $(popon HEADLESS-2); e20 on $(win e20 .workspace.id)"
check "E2: Hyprdrive compacted: 11..20, no overflow" "$(is "$(popon HEADLESS-2)" '11 12 13 14 15 16 17 18 19 20')" "$(ids)"
c0=$(ncw); reload; sleep 1.5; settled; reload; sleep 1.5; settled
check "E2: then two reloads: zero events" "$(is "$(( $(ncw) - c0 ))" 0)" "$(cwlist)"
lua_ok E2
fs "$A19" unset; sleep 1.5; settled
# E3: show an empty desktop in block 1's overflow range
go 1001009; sleep 0.5
SH0=$(shown HEADLESS-2)
c0=$(ncw); reload; sleep 1.5; settled
echo "      E3: showing $SH0 (empty) at a reload -> shows $(shown HEADLESS-2); events [$(cwlist)]"
check "E3: a reload with an empty overflow desktop on view renumbers nothing" "$(is "$(( $(ncw) - c0 ))" 0)" "$(cwlist)"
check "E3: no empty unshown desktops" "$(is "$(empties)" '')" "$(empties)"
lua_ok E3
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
lua_ok C
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
go 21; sleep 0.4
$R overview >/dev/null; sleep 1.5
OVD=$(on HEADLESS-1)
check "D: its strip: 21..24 and the spare 25, no empties below 21" "$(is "$OVD" '21 22 23 24 25')" "$OVD"
$R overview >/dev/null; sleep 1.2; settled
lua_ok D
fi

gate_end
