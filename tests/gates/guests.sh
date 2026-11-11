#!/usr/bin/env bash
# guests -- Omarchy's guests (desktops with a merged record) are ordinary members of their monitor's row; the panel on and off leaves no empty desktops.
# Protects: the owner's laptop, 2026-11-11: desktops Omarchy had merged
# ("origin = the panel") were pinned, so fullscreening in them and moving them
# "work differently or not at all", the compaction after a display change
# never ran, and "I just went to two monitors and back to one and it created
# two new empty desktops"; and the rule "when you clear desktops after a
# monitor config change, move the windows on later desktops back to the next
# available desktops" (populated desktops contiguous from the row's first).
# The rig has no Omarchy: blocks.lua runs the emulation (omarchy-blocks.lua,
# with restore() and the workspace.created rule) or, with
# GATE_OMARCHY_MW=/abs/monitor-workspaces.lua, Omarchy 4.5's real script
# (never committed; each case then also checks no callback of it raised).
# Asserts:
#   D. the owner's state: one output on block 2, the panel (block 0)
#      unplugged; windows on 1..4 merge to 21..24 with records. A stale
#      record (d3 closes): a display change closes the hole, 21 22 23, and no
#      record describes another desktop. A fullscreen trip from the 2nd
#      desktop makes room at 23 through the guests and comes back. A strip
#      reorder (Super+Shift+Right in the overview) swaps 21 and 22. Every
#      following reload renumbers nothing; every record still describes the
#      desktop it was written for (Hyprdrive had Omarchy drop the records of
#      the desktops it renumbered).
#   F. the panel on and off three times, windows on both: the panel's desktop
#      merges into block 2 when it goes (after an empty desktop the external
#      shows, on odd rounds: append_slot leaves a hole), and within ~1.6 s the
#      external's populated desktops are contiguous from 21, the shown one
#      still shown, no empties; when it comes back Omarchy's restore() takes
#      home the guests it still has records for, contiguous from 1. After
#      every step: every record a panel desktop's, the next reload quiet; the
#      strip after the first trip back: the populated cells and the spare.
# Control: --so a4d6892 (/home/dragos/Projects/hyprdrive-native/hyprdrive.so,
# guests pinned): D2 (the hole stays), D3 (the trip settles in place), D4
# (the reorder refused) and F (the guest past the hole kept there) fail.
source "$(dirname "$0")/lib.sh"
gate_begin gst
control_build "a4d6892, e.g. /home/dragos/Projects/hyprdrive-native/hyprdrive.so"
source "$(dirname "$0")/blocks.lib"

# ---------------------------------------------------------------- D (the owner's state)
seed '0\t\tPANEL\n2\tHEADLESS-1\tHEADLESS-1\n'
blocks_start
focus_mon HEADLESS-1; for n in 1 2 3 4; do go $n; kw "d$n"; done; settled
events_start
reload; sleep 1.5; settled
check "D: the first reload merges 1..4 to 21..24 with records" "$(is "$(popon HEADLESS-1)|$(recs_ok d exact)|$(recs | wc -w)" '21 22 23 24|1|4')" "$(ids) | $(recs)"
# Guests are members, never pinned (the owner's laptop, 2026-11-11: trips,
# paybacks, reorders and compaction all refused around them). A stale record
# first: d3's window closes (Hyprland drops 23, Omarchy's record stays); the
# next display change closes the hole (24 -> 23) and the record under 23 goes
# with it -- it must not describe d4.
close_win "$(addr_of d3)"; sleep 0.5; settled
c0=$(ncw); scale 1.25; sleep 1.5; settled
check "D2: a display change closes the hole: 21 22 23" "$(is "$(popon HEADLESS-1)|$(win d4 .workspace.id)" '21 22 23|23')" "$(ids)"
check "D2: ... no record describes another desktop" "$(recs_ok d exact)" "$(recs)"
check "D2: ... no empty unshown desktops" "$(is "$(empties)" '')" "$(empties)"
c0=$(ncw); reload; sleep 1.5; settled
check "D2: the next reload renumbers nothing" "$(is "$(( $(ncw) - c0 ))" 0)" "$(cwlist)"
# a fullscreen trip from the 2nd desktop: room at 23 (through the guests),
# and back
go 22; kw d2b; settled
AD2=$(addr_of d2); focus "$AD2"; fs "$AD2" set; sleep 1.5; settled
check "D3: fullscreen from the 2nd: the app on 23, 23 -> 24" "$(is "$(win d2 .workspace.id) $(win d4 .workspace.id) $(popon HEADLESS-1)" '23 24 21 22 23 24')" "d2 on $(win d2 .workspace.id) fs $(win d2 .fullscreen), d4 on $(win d4 .workspace.id); $(ids); $(refused | tail -1)"
check "D3: ... records consistent" "$(recs_ok d exact)" "$(recs)"
fs "$AD2" unset; sleep 2; settled
check "D3: out of fullscreen: back on 22, 21 22 23" "$(is "$(win d2 .workspace.id) $(win d4 .workspace.id) $(popon HEADLESS-1)" '22 23 21 22 23')" "d2 on $(win d2 .workspace.id), d4 on $(win d4 .workspace.id); $(ids); $(refused | tail -1)"
check "D3: ... records consistent" "$(recs_ok d exact)" "$(recs)"
c0=$(ncw); reload; sleep 1.5; settled
check "D3: the next reload renumbers nothing" "$(is "$(( $(ncw) - c0 ))" 0)" "$(cwlist)"
# a strip reorder: in the overview, Super+Shift+Right moves the shown 21 to 22
go 21; sleep 0.4
$R overview >/dev/null; sleep 1.5
$R tap Super+Shift+Right >/dev/null; sleep 1.2
$R overview >/dev/null; sleep 1.2; settled
check "D4: strip reorder: d1 on 22, d2 on 21" "$(is "$(win d1 .workspace.id) $(win d2 .workspace.id)" '22 21')" "d1 $(win d1 .workspace.id) d2 $(win d2 .workspace.id); $(ids); $(refused | tail -1)"
check "D4: ... records consistent" "$(recs_ok d exact)" "$(recs)"
c0=$(ncw); reload; sleep 1.5; settled; scale 1; sleep 1.5; settled
check "D4: the next reload and display change renumber nothing" "$(is "$(( $(ncw) - c0 ))|$(popon HEADLESS-1)" '0|21 22 23')" "$(cwlist) | $(ids)"
lua_ok D4
rig_stop


# ---------------------------------------------------------------- F (the panel on, off, on...)
# The owner, 2026-11-11: "I just went to two monitors and back to one and it
# created two new empty desktops". The external owns block 2 (21 22 23
# populated, no records), the panel block 0. Panel on: a window on its
# desktop. Panel off: Omarchy merges that desktop into block 2 with
# append_slot -- after the highest id in the block, so past an empty desktop
# the external shows (odd rounds) -- and writes a record "origin = the
# panel"; within ~1 s Hyprdrive's compaction leaves the external's populated
# desktops contiguous from 21 (the guest included: the old build pinned it
# and left the hole). Panel on again: Omarchy's restore() takes the guests it
# still has records for home; the panel's row is contiguous from 1 too.
# Three round trips; after each step: no empties, every record a panel
# desktop's, the next reload renumbers nothing.
seed '0\t\tHEADLESS-2\n2\tHEADLESS-1\tHEADLESS-1\n'
blocks_start
focus_mon HEADLESS-1; for n in 21 22 23; do go $n; kw "f$n"; done; settled
events_start
inv() { # inv LABEL
    check "$1: the external's desktops contiguous from 21" "$(contig HEADLESS-1 21)" "$(ids)"
    if has_mon HEADLESS-2; then check "$1: the panel's contiguous from 1" "$(contig HEADLESS-2 1)" "$(popon HEADLESS-2) | $(ids)"; fi
    check "$1: no empty unshown desktops" "$(is "$(empties)" '')" "$(empties) | $(ids)"
    check "$1: every record describes a panel desktop" "$(recs_ok o)" "$(recs)"
    local c0; c0=$(ncw); reload; sleep 1.5; settled
    check "$1: the next reload renumbers nothing" "$(is "$(( $(ncw) - c0 ))" 0)" "$(cwlist) | $(ids)"
    lua_ok "$1"
}
for r in 1 2 3; do
    $R mon2 >/dev/null; sleep 1.5; settled
    focus_mon HEADLESS-2; kw "o$r"; settled
    inv "F$r on"
    focus_mon HEADLESS-1
    TOP=$(popon HEADLESS-1 | awk '{print $NF}')
    if [ $((r % 2)) = 1 ]; then go $((TOP + 1)); SHOWNCLS=; else go "$TOP"; SHOWNCLS=$(J clients | jq -r "[.[] | select(.workspace.id == $TOP) | .class][0]"); fi
    sleep 0.4
    T0=$(date +%s%N)
    hq output remove HEADLESS-2 >/dev/null
    sleep 0.3
    wait_for 4 "[ \$(contig HEADLESS-1 21) = 1 ] && [ -z \"\$(empties)\" ] && ! has_mon HEADLESS-2"
    MS=$(( ($(date +%s%N) - T0) / 1000000 ))
    settled
    echo "      F$r off: contiguous after ${MS} ms; $(ids); records [$(recs)]"
    check "F$r off: contiguous within ~1 s" "$(le "$MS" 1600)" "${MS} ms; $(ids)"
    check "F$r off: every window kept ($((3 + r)))" "$(is "$(nclients)" $((3 + r)))" "$(nclients)"
    if [ -n "$SHOWNCLS" ]; then
        check "F$r off: the shown desktop still shown" "$(is "$(shown HEADLESS-1)" "$(win "$SHOWNCLS" .workspace.id)")" "shows $(shown HEADLESS-1), $SHOWNCLS on $(win "$SHOWNCLS" .workspace.id)"
    fi
    if [ "$r" = 1 ]; then
        go 21; sleep 0.4; $R overview >/dev/null; sleep 1.5
        OVF=$(on HEADLESS-1); POPF=$(popon HEADLESS-1); N=$(echo $POPF | wc -w)
        check "F1 off: its strip: the populated desktops + the spare" "$(is "$OVF" "$(seq -s' ' 21 $((21 + N)))")" "$OVF (populated $POPF)"
        $R overview >/dev/null; sleep 1.2; settled
    fi
    inv "F$r off"
done
rig_stop

gate_end
