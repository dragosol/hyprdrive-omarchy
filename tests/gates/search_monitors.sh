#!/usr/bin/env bash
# search_monitors -- with two outputs, one query drives both overviews: each lays the results out on its own output, the keyboard stays with one, Escape twice clears both and closes both.
# Protects the per-monitor split of the search state (src/search/SearchSession.hpp:
# query and navigation are per SESSION, one keyboard; the published layout and
# the live field are PER MONITOR) -- the split the refactor must keep exactly.
# State from `hyprdrive:debug search` (search.lib).
# Fixtures: HEADLESS-2 added at 1920x0 (hl.sh mon2) before anything opens;
# gsalpha on desktop 2 (HEADLESS-2), gsbeta on desktop 1 (HEADLESS-1), gsbeta
# focused; the overview opened with the dispatcher on both outputs.
# Asserts:
#   1. two overviews, the keyboard with exactly one (HEADLESS-1)
#   2. "gsalpha": one query, one selection; BOTH outputs show the pane --
#      queried, the label, the same number of row hits, a band eased open
#   3. the filter per output: the match is on HEADLESS-2's desktop, so its grid
#      has the one tile and HEADLESS-1's has none
#   4. Right/Left on the keyboard's output move the ONE selection both outputs draw
#      (each output's published selection follows)
#   5. Escape: both panes go (both outputs back to the placeholder, bands 0),
#      both overviews stay; Escape again: both close
# Control: builds with ONE product line disabled, tests/gates/search_controls/
# (search.lib says how). Recorded 2026-11-17 on ba365d7, the checks that FAIL:
#   other_monitor_blank 2 (then 3, 4)   band_target 2 (then 4)   esc_clear 5
source "$(dirname "$0")/lib.sh"
gate_begin sm
control_build "a build from tests/gates/search_controls"
source "$(dirname "$0")/search.lib"
search_start
$R mon2 >/dev/null; wait_for 3 '[ "$(J monitors | jq length)" = 2 ]'
go 2; kw gsalpha 700 450; go 1; kw gsbeta 700 450; sleep 0.5
focus "$(addr_of gsbeta)"; $R ptr move 300 600 >/dev/null; settle 3 geom
A=HEADLESS-1; B=HEADLESS-2

$R overview >/dev/null
until_s 4 '[ "$(sk overviews)" = 2 ] && [ "$(mk $A drawable)$(mk $B drawable)" = 11 ]'; settle_s $A
check "1: two overviews, the keyboard with HEADLESS-1 only" "$(is "$(sk overviews) $(mk $A keys)$(mk $B keys)" "2 10")" "$(line) || keys $(mk $A keys)/$(mk $B keys)"

$R type gsalpha >/dev/null
until_s 4 '[ "$(sq)" = gsalpha ] && [ "$(mk $A hits)" -ge 1 ] && [ "$(mk $B hits)" -ge 1 ] 2>/dev/null'; settle_s $A; settle_s $B
check "2: one query, one selection" "$(is "$(sq) $(sk zone)|$(selrow)" "gsalpha result|Gsalpha Fixture")" "$(line)"
for O in $A $B; do
    check "2: $O shows the pane: queried, the label, row hits, the band open" \
        "$(is "$(mk $O queried) $(mlabel $O) $(mk $O hits) $(fgt "$(cur $O band)" 99.9)" "1 gsalpha $(mk $A hits) 1")" "$(mline $O | cut -c1-260)"
done
check "3: the filter per output: HEADLESS-2's grid has the match, HEADLESS-1's none" "$(is "$(mk $B gridTiles) $(mk $A gridTiles)" "1 0")" "gridTiles $B $(mk $B gridTiles), $A $(mk $A gridTiles)"

C2=$(rowsin 2)
if [ "$C2" -ge 1 ]; then
    $R tap Right >/dev/null; until_s 2 '[ "$(sk sel)" != 0 ] && [ "$(mk $B sel)" = "$(sk sel)" ]'
    check "4: Right on the keyboard's output: both outputs draw the new selection" "$(is "$(mk $A sel) $(mk $B sel)" "$(sk sel) $(sk sel)")" "session sel $(sk sel), $A $(mk $A sel), $B $(mk $B sel)"
    $R tap Left >/dev/null; until_s 2 '[ "$(sk sel)" = 0 ] && [ "$(mk $B sel)" = 0 ]'
    check "4: Left: both back on row 0" "$(is "$(mk $A sel) $(mk $B sel)" "0 0")" "session sel $(sk sel), $A $(mk $A sel), $B $(mk $B sel)"
else
    subskip "4: Right/Left" "the rig offered no column-2 row"
fi

$R tap Escape >/dev/null; until_s 2 '[ -z "$(sq)" ]'; settle_s $A; settle_s $B
check "5: Escape: both panes go, both overviews stay" \
    "$(is "$(sk overviews) $(mk $A queried)$(mk $B queried) $(mk $A placeholder)$(mk $B placeholder) $(cur $A band) $(cur $B band)" "2 00 11 0.0 0.0")" "$(line) || $(mline $A | cut -c1-120) || $(mline $B | cut -c1-120)"
$R tap Escape >/dev/null; until_s 3 '[ "$(sk overviews)" = 0 ]'
check "5: Escape again: both close" "$(is "$(sk overviews)" 0)" "$(line)"
gate_end
