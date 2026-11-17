#!/usr/bin/env bash
# search_activate -- Return and a click on a search result do the right thing: focus the running app's window, launch an app that is not running, pick the tile the keyboard is on.
# Protects (designed behaviours, Focus and attention: "Switching to a window
# from the dock previews or overview puts the cursor on it"; Search) and
# activateSearchResult's three outcomes (src/overview/OverviewInternal.hpp).
# State from `hyprdrive:debug search` (search.lib) and hyprctl.
# Fixtures (one output): gsalpha (a desktop entry, so a RUNNING app row) on
# desktop 2, gsbeta (no entry) on desktop 1; gslaunch has an entry and is never
# opened before step 3. Each step starts with gsbeta focused on desktop 1
# (step 4: gsalpha focused on desktop 2) and opens the overview afresh.
# Asserts:
#   1. "gsalpha", Return (the row is auto-selected): the overview closes, the
#      running window is focused and its desktop shown -- not a second copy
#   2. "gsalpha", the pointer onto the row: it is the hovered selection
#      (intent hovering); a click on it: the same as 1
#   3. "gslaunch", Return: a gslaunch window maps (launched), the overview closes
#   4. "gsbeta", Tab to the tiles, Return: the keyboard's tile (gsbeta) is
#      focused and its desktop shown -- Return in the tile zone is the tile,
#      not the selected result
# Control: builds with ONE product line disabled, tests/gates/search_controls/
# (search.lib says how). Recorded 2026-11-17 on ba365d7, the checks that FAIL:
#   pending_focus 1, 2    launch_off 3    click_index 2 (then 3: the overview
#   was left open)        tab_to_tiles 4
source "$(dirname "$0")/lib.sh"
gate_begin sa
control_build "a build from tests/gates/search_controls"
source "$(dirname "$0")/search.lib"
search_start
go 2; kw gsalpha 700 450; go 1; kw gsbeta 700 450; sleep 0.5
M=HEADLESS-1
park() { $R ptr move 300 600 >/dev/null; }
from() {  # from CLASS DESKTOP -- that window focused on its desktop, the overview open, QUERY typed
    go "$2"; focus "$(addr_of "$1")"; park; settle 3 geom
    $R overview >/dev/null; until_s 4 '[ "$(sk overviews)" = 1 ] && [ "$(mk $M drawable)" = 1 ]'
    $R type "$3" >/dev/null; until_s 4 "[ \"\$(sq)\" = $3 ] && [ \"\$(mk $M hits)\" -ge 1 ] 2>/dev/null"; settle_s $M
}
act() { J activewindow | jq -r .class; }

# 1. Return on the running app
from gsbeta 1 gsalpha
check "1: the running app's row is selected before Return" "$(is "$(sk zone)|$(selrow)" "result|Gsalpha Fixture")" "$(line)"
$R tap Return >/dev/null; until_s 3 '[ "$(sk overviews)" = 0 ]'; sleep 0.6
check "1: Return: the overview closes, gsalpha focused, desktop 2 shown, no second copy" \
    "$(is "$(sk overviews) $(act) $(shown) $(J clients | jq '[.[] | select(.class == "gsalpha")] | length')" "0 gsalpha 2 1")" \
    "overviews $(sk overviews), focus $(act), shown $(shown), clients $(J clients | jq -r '[.[].class] | join(",")')"

# 2. the pointer: hover, then click
from gsbeta 1 gsalpha
H=$(hit0 $M); IFS=, read -r HX HY HW HH <<< "$H"
if [ -n "$H" ]; then
    CX=$((HX + HW / 2)); CY=$((HY + HH / 2))
    $R ptr move $CX $CY >/dev/null; until_s 2 '[ "$(sk intent)" = hovering ]'
    check "2: the pointer on the row: the hovered selection" "$(is "$(sk zone) $(sk intent) $(sk hover)|$(selrow)" "result hovering 0|Gsalpha Fixture")" "$(line)"
    $R ptr tap $CX $CY >/dev/null; until_s 3 '[ "$(sk overviews)" = 0 ]'; sleep 0.6
    check "2: a click on it: the overview closes, gsalpha focused, desktop 2 shown" "$(is "$(sk overviews) $(act) $(shown)" "0 gsalpha 2")" "overviews $(sk overviews), focus $(act), shown $(shown), clicked $CX,$CY"
else
    check "2: the row has a hit box" 0 "$(mline $M)"
fi

# 3. Return on an app that is not running
from gsbeta 1 gslaunch
check "3: the not-running app's row is selected" "$(is "$(sk zone)|$(selrow)|$(hasrow 'Gslaunch Fixture' 0)" "result|Gslaunch Fixture|1")" "$(line) $(echo "$SN" | grep '^  row' | tr '\n' ' ')"
$R tap Return >/dev/null; until_s 3 '[ "$(sk overviews)" = 0 ]'
wait_for 6 '[ -n "$(addr_of gslaunch)" ]'
check "3: Return: the overview closes and gslaunch is launched" "$(is "$(sk overviews) $([ -n "$(addr_of gslaunch)" ] && echo up)" "0 up")" "overviews $(sk overviews), clients $(J clients | jq -r '[.[].class] | join(",")')"
[ -n "$(addr_of gslaunch)" ] && { close_win "$(addr_of gslaunch)"; wait_for 3 '[ -z "$(addr_of gslaunch)" ]'; }

# 4. Return in the tile zone
from gsalpha 2 gsbeta
$R tap Tab >/dev/null; until_s 2 '[ "$(sk zone)" = tile ]'; snap
check "4: Tab: the tiles, on gsbeta's" "$(is "$(sk zone) $(mk $M tile)" "tile gsbeta")" "$(line) || $(mline $M | cut -c1-90)"
$R tap Return >/dev/null; until_s 3 '[ "$(sk overviews)" = 0 ]'; sleep 0.6
check "4: Return: the overview closes, gsbeta focused, desktop 1 shown" "$(is "$(sk overviews) $(act) $(shown)" "0 gsbeta 1")" "overviews $(sk overviews), focus $(act), shown $(shown)"
gate_end
