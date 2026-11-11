#!/usr/bin/env bash
# blocks_overflow -- Omarchy's reconcile meets a block's overflow desktop: no error, no fight.
# Protects: the owner's "no limit" ruling for a full block (2026-11-11; an 11th
# desktop goes to the row's overflow) on Omarchy's per-monitor workspace
# blocks; split from blocks.sh (case E) to keep each gate inside the runner's
# time limit. The rig and the GATE_OMARCHY_MW opt-in are blocks.sh's.
# Asserts:
#   E. Omarchy meets the overflow (an overflow id is a guest to its reconcile:
#      merge() pulls it into a block with room, origin identity nil, no record):
#      a slot frees and the config reloads before Hyprdrive compacts -> Omarchy
#      pulls it in, no Lua error, next reloads quiet; Hyprdrive compacting first
#      -> the same, quiet; an empty overflow desktop on view at a reload -> no
#      renumber (Omarchy's own_views puts the monitor back on its home).
# Control: --so 3423fe4 (before blocks): no overflow at all -- E1's setup
# fails (the 11th desktop is 21).
source "$(dirname "$0")/lib.sh"
gate_begin bov
control_build "3423fe4, e.g. /home/dragos/Projects/hyprdrive-native/hyprdrive.so"
source "$(dirname "$0")/blocks.lib"

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

gate_end
