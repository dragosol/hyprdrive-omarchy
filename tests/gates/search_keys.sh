#!/usr/bin/env bash
# search_keys -- type-to-search in the overview: rows and the results pane appear, arrows and Tab walk the zones, Backspace, Escape twice, no match, the long-query field, the strip label, a fresh session after close + reopen.
# Protects (designed behaviours, Overview/strip/dock "Search", and the search
# session contract in src/search/SearchSession.hpp) -- state read from the
# read-only dump `hyprdrive:debug search` (search.lib), not from pixels.
# Fixtures (one output): gsalpha (has a desktop entry) on desktop 2, gsbeta (has
# none) on desktop 1 and focused; the overview opened with the dispatcher.
# Asserts:
#   1. typing "gsalpha": the query and an active search; the running app's row
#      "Gsalpha Fixture" in column 0, auto-selected (zone result); the pane laid
#      out on the monitor (queried, label = query, row hits, a card), the band
#      eased open (>= 100 logical px), the pill docked up (dock 1, the pill
#      higher than at rest); the filter: one window matches (gsalpha), and it
#      is the one tile of the main grid -- promoted there from desktop 2 (its
#      drawn tile moves up from its own card's into the grid)
#   2. arrows: Up off the column top -> the search field; Down -> back on the
#      first row (explicit, browsing); Right -> a column-2 row; Left -> column 0
#   3. Tab: results -> tiles (the visible tile, gsalpha, keyboard-owned) ->
#      desktops -> results
#   4. Escape: the first clears the filter and the pane (overview still open,
#      gsbeta's tile back, band eases to 0, the placeholder back); the second
#      closes the overview; between them gsalpha's tile is back on its own card
#   5. Backspace: "gsal" -> "gsa" -> ... -> "" (search inactive, no rows, the
#      pane down); one more on the empty field changes nothing and closes nothing
#   6. no match ("zqxjvw"): no window matches (no tiles in the grid, the
#      matches count 0), no column-0 row, the pane still up with the rig's web
#      row; `empty` stays 0 (a typed query always has the web row: today's
#      behaviour, see docs/search-refactor.md)
#   7. a long query: the live field grows past its 210 px rest width and the
#      pill with it (PondFluid field, SearchSession.hpp SSearchFieldMotion)
#   8. the strip label: hovering desktop 1's strip cell with results up puts the
#      label under the plate and slides the grid down (gridNudgeL, gridShiftL >
#      0); with no query it lifts the pill instead (pillNudgeWant > 0, the pill
#      higher); off the strip both go back to 0
#   9. close (dispatcher toggle) with a query and an explicit selection, reopen:
#      a fresh session -- no query, zone tile, no selection, no rows, the band
#      at 0, the placeholder
# Control: builds with ONE product line disabled, tests/gates/search_controls/
# (search.lib says how to build one). Recorded 2026-11-17 on ba365d7 (1.9.68.1
# + the dump), the checks that FAIL -- the named one first, then what follows
# from the state it leaves:
#   esc_clear 4 (then 5, 6, 7)   backspace_active 5      band_target 1 (then 2, 3, 8)
#   up_to_search 2 (then 3, 4)   tab_to_tiles 3 (then 4) first_select 1
#   filter_off 1 (then 3, 6)     field_fixed 7 (then 8)  strip_nudge 8
#   session_reset 9
source "$(dirname "$0")/lib.sh"
gate_begin sk
control_build "a build from tests/gates/search_controls"
source "$(dirname "$0")/search.lib"
search_start
go 2; kw gsalpha 700 450; go 1; kw gsbeta 700 450; sleep 0.5
focus "$(addr_of gsbeta)"; $R ptr move 300 600 >/dev/null; settle 3 geom
M=HEADLESS-1

$R overview >/dev/null
until_s 4 '[ "$(sk overviews)" = 1 ] && [ "$(mk $M drawable)" = 1 ]'; settle_s $M
REST_Y=$(box $M pill 2); REST_W=$(box $M pill 3); DA0=$(drawn_y gsalpha)
check "0: the overview opens with a fresh search (no query, zone tile, placeholder)" \
    "$(is "$(sk overviews) $(sq)|$(sk active) $(sk zone) $(sk sel) $(mk $M queried) $(mk $M placeholder) $(mk $M hits)" "1 |0 tile -1 0 1 0")" "$(line) || $(mline $M)"

# 1. typing
$R type gsalpha >/dev/null
until_s 4 '[ "$(sq)" = gsalpha ] && [ "$(mk $M hits)" -ge 1 ] 2>/dev/null'; settle_s $M
check "1: the query is typed and the search is active" "$(is "$(sq) $(sk active) $(sk pane)" "gsalpha 1 1")" "$(line)"
check "1: the running app's row is listed in column 0" "$(is "$(hasrow 'Gsalpha Fixture' 0)" 1)" "$(echo "$SN" | grep '^  row' | tr '\n' ' ')"
check "1: ...and auto-selected (zone result)" "$(is "$(sk zone)|$(selrow)" "result|Gsalpha Fixture")" "$(line)"
check "1: the pane is laid out: queried, label = the query, row hits, a card" \
    "$(is "$(mk $M queried) $(mk $M placeholder) $(mlabel $M) $([ "$(mk $M hits)" -ge 1 ] && echo h) $([ "$(mk $M cards)" -ge 1 ] && echo c)" "1 0 gsalpha h c")" "$(mline $M)"
check "1: the results band eased open (>= 100 px) and the pill docked up" \
    "$(is "$(fgt "$(cur $M band)" 99.9) $(cur $M dock) $(flt "$(box $M pill 2)" $((REST_Y - 20)))" "1 1.00 1")" "band $(mk $M band) dock $(mk $M dock) pill $(mk $M pill) (rest y $REST_Y)"
wait_for 3 '[ "$(flt "$(drawn_y gsalpha)" $((DA0 - 100)))" = 1 ]'
check "1: the filter: one window matches, the grid's one tile, gsalpha promoted into it" "$(is "$(mk $M matches) $(mk $M gridTiles) $(flt "$(drawn_y gsalpha)" $((DA0 - 100)))" "1 1 1")" "matches $(mk $M matches) gridTiles $(mk $M gridTiles) gsalpha drawn y $DA0 -> $(drawn_y gsalpha)"

# 2. arrows
$R tap Up >/dev/null; until_s 2 '[ "$(sk zone)" = search ]'
check "2: Up off the column top: the search field" "$(is "$(sk zone) $(sk sel)" "search -1")" "$(line)"
$R tap Down >/dev/null; until_s 2 '[ "$(sk zone)" = result ]'
check "2: Down: back on the first row, explicit, browsing" "$(is "$(sk zone) $(sk explicit) $(sk intent)|$(selrow)" "result 1 browsing|Gsalpha Fixture")" "$(line)"
C2=$(rowsin 2)
if [ "$C2" -ge 1 ]; then
    $R tap Right >/dev/null; until_s 2 '[ "$(sk sel)" != 0 ]'
    S=$(sk sel); C=$(echo "$SN" | grep "^  row $S " | awk '{print $3}')
    check "2: Right: a column-2 row" "$(is "$(sk zone) $C" "result c2")" "$(line)"
    $R tap Left >/dev/null; until_s 2 '[ "$(sk sel)" = 0 ]'
    check "2: Left: back on column 0" "$(is "$(sk zone) $(sk sel)" "result 0")" "$(line)"
else
    subskip "2: Right/Left" "the rig offered no column-2 row"
fi

# 3. Tab
$R tap Tab >/dev/null; until_s 2 '[ "$(sk zone)" = tile ]'; snap
check "3: Tab from the results: the tiles, on the visible tile, keyboard-owned" "$(is "$(sk zone) $(mk $M tile) $(sk kbTile)" "tile gsalpha 1")" "$(line) || $(mline $M | cut -c1-90)"
$R tap Tab >/dev/null; until_s 2 '[ "$(sk zone)" = desktop ]'
check "3: Tab again: the desktops" "$(is "$(sk zone)" desktop)" "$(line)"
$R tap Tab >/dev/null; until_s 2 '[ "$(sk zone)" = result ]'
check "3: Tab again: back in the results" "$(is "$(sk zone)|$(selrow)" "result|Gsalpha Fixture")" "$(line)"

# 4. Escape twice
$R tap Escape >/dev/null; until_s 2 '[ -z "$(sq)" ]'; settle_s $M
check "4: the first Escape clears the filter, the overview stays" "$(is "$(sk overviews) $(sq)|$(sk active) $(sk pane) $(sk zone) $(sk rows)" "1 |0 0 tile 0")" "$(line)"
check "4: ...the pane goes: band eased to 0, the placeholder, no hits" "$(is "$(cur $M band) $(mk $M queried) $(mk $M placeholder) $(mk $M hits)" "0.0 0 1 0")" "$(mline $M)"
wait_for 3 '[ "$(drawn_y gsalpha)" -ge $((DA0 - 3)) ] && [ "$(drawn_y gsalpha)" -le $((DA0 + 3)) ]'
check "4: ...gsalpha's tile back on its own card" "$(is "$(( $(drawn_y gsalpha) - DA0 ))" 0)" "gsalpha drawn y $(drawn_y gsalpha), at open $DA0"
$R tap Escape >/dev/null; until_s 3 '[ "$(sk overviews)" = 0 ]'
check "4: the second Escape closes the overview" "$(is "$(sk overviews)" 0)" "$(line)"

# 5. Backspace
sleep 0.8; $R overview >/dev/null; until_s 4 '[ "$(sk overviews)" = 1 ] && [ "$(mk $M drawable)" = 1 ]'
$R type gsal >/dev/null; until_s 3 '[ "$(sq)" = gsal ]'
seq5=
for want in gsa gs g ""; do
    $R tap BackSpace >/dev/null; until_s 2 "[ \"\$(sq)\" = \"$want\" ]"; seq5="$seq5 '$(sq)'"
done
settle_s $M
check "5: Backspace walks the query down one codepoint at a time" "$(is "$seq5" " 'gsa' 'gs' 'g' ''")" "$seq5"
check "5: ...to an inactive search with no rows and no pane" "$(is "$(sk active) $(sk pane) $(sk rows) $(mk $M queried) $(goal $M band)" "0 0 0 0 0.0")" "$(line) || band $(mk $M band)"
$R tap BackSpace >/dev/null; sleep 0.5; snap
check "5: Backspace on the empty field: nothing changes, nothing closes" "$(is "$(sk overviews) $(sq)|$(sk active) $(sk zone)" "1 |0 tile")" "$(line)"

# 6. no match
$R type zqxjvw >/dev/null; until_s 4 '[ "$(sq)" = zqxjvw ] && [ "$(mk $M hits)" -ge 1 ] 2>/dev/null'; settle_s $M
check "6: no match: no window matches (no grid tiles, matches 0)" "$(is "$(mk $M matches) $(mk $M gridTiles)" "0 0")" "$(mline $M | cut -c1-120)"
check "6: ...no column-0 row, the pane still up with the web row" "$(is "$(rowsin 0) $(sk pane) $(hasrow 'Search Google' 2) $(mk $M queried)" "0 1 1 1")" "$(echo "$SN" | grep '^  row' | tr '\n' ' ')"
check "6: ...empty stays 0 (a typed query always lists the web row)" "$(is "$(mk $M empty)" 0)" "$(mline $M | cut -c1-200)"
$R tap Escape >/dev/null; until_s 2 '[ -z "$(sq)" ]'; settle_s $M

# 7. the live field
REST_F=$(mk $M field)
$R type "gsalpha and then a much longer query typed" >/dev/null
until_s 5 '[ "$(sq)" = "gsalpha and then a much longer query typed" ]'; settle_s $M 5
check "7: a long query: the field grows past its rest width (210)" "$(fgt "$(mk $M field)" 300)" "field $REST_F -> $(mk $M field), fieldW $(mk $M fieldW)"
check "7: ...and the pill with it" "$(fgt "$(box $M pill 3)" $((REST_W + 80)))" "pill w $REST_W -> $(box $M pill 3)"
$R tap Escape >/dev/null; until_s 2 '[ -z "$(sq)" ]'; settle_s $M

# 8. the strip label
cell1() {  # the centre of desktop 1's strip cell (the first of `images` cells on the plate)
    local P I; P=$(dbg stripclean | grep -m1 -o 'plate=[0-9]*,[0-9]* [0-9]*x[0-9]*' | tr '=,x' '   '); I=$(dbg stripclean | grep -m1 -o 'images=[0-9]*' | cut -d= -f2)
    set -- $P; echo $(( $2 + $4 / (2 * ${I:-3}) )) $(( $3 + $5 / 2 ))
}
$R type gsalpha >/dev/null; until_s 4 '[ "$(mk $M hits)" -ge 1 ] 2>/dev/null'; settle_s $M
set -- $(cell1); $R ptr move "$1" "$2" >/dev/null
until_s 3 '[ "$(fgt "$(mk $M gridShiftL)" 5)" = 1 ]'; sleep 0.5; snap
check "8: hovering desktop 1's strip cell with results up: the label goes under the plate, the grid slides down" \
    "$(is "$(fgt "$(mk $M gridNudgeL)" 5) $(fgt "$(mk $M gridShiftL)" 5)" "1 1")" "at $1,$2: gridNudgeL $(mk $M gridNudgeL) gridShiftL $(mk $M gridShiftL)"
# a path, not one jump: a single motion event straight off the strip leaves
# the label up until the next motion (seen in the rig; docs/search-refactor.md)
$R ptr path 20 "$1" "$2" 600 500 300 600 >/dev/null; until_s 3 '[ "$(mk $M gridShiftL)" = 0.0 ]'
check "8: ...off the strip: both back to 0" "$(is "$(mk $M gridNudgeL) $(mk $M gridShiftL)" "0.0 0.0")" "gridNudgeL $(mk $M gridNudgeL) gridShiftL $(mk $M gridShiftL)"
$R tap Escape >/dev/null; until_s 2 '[ -z "$(sq)" ]'; settle_s $M
set -- $(cell1); $R ptr move "$1" "$2" >/dev/null
until_s 3 '[ "$(fgt "$(mk $M pillNudgeWant)" 5)" = 1 ]'; sleep 0.6; snap
check "8: hovering it with no query: the label lifts the pill instead" \
    "$(is "$(fgt "$(mk $M pillNudgeWant)" 5) $(flt "$(box $M pill 2)" $((REST_Y - 5))) $(mk $M gridNudgeL)" "1 1 0.0")" "at $1,$2: pillNudgeWant $(mk $M pillNudgeWant) pill $(mk $M pill) (rest y $REST_Y) gridNudgeL $(mk $M gridNudgeL)"
$R ptr move 300 600 >/dev/null; sleep 0.6

# 9. close + reopen
$R type gsalpha >/dev/null; until_s 4 '[ "$(mk $M hits)" -ge 1 ] 2>/dev/null'
$R tap Up >/dev/null; $R tap Down >/dev/null; until_s 2 '[ "$(sk explicit)" = 1 ]'
$R overview >/dev/null; until_s 3 '[ "$(sk overviews)" = 0 ]'; sleep 0.8
$R overview >/dev/null; until_s 4 '[ "$(sk overviews)" = 1 ] && [ "$(mk $M drawable)" = 1 ]'; settle_s $M
check "9: close with a query, reopen: a fresh session" "$(is "$(sq)|$(sk active) $(sk zone) $(sk sel) $(sk explicit) $(sk rows)" "|0 tile -1 0 0")" "$(line)"
check "9: ...and a fresh pane: band 0, the placeholder, no hits" "$(is "$(cur $M band) $(mk $M queried) $(mk $M placeholder) $(mk $M hits)" "0.0 0 1 0")" "$(mline $M)"
$R overview >/dev/null; sleep 0.8
gate_end
