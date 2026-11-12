#!/usr/bin/env bash
# overview_tile_order -- the overview grid's tile order in each plugin:hyprdrive:overview_tile_order mode, and that it holds while the overview is up.
# Protects: the tile-order setting (GridSolve.hpp order/recentKey/positionOrder,
# GridLayout.cpp gridOrderTake). Four floating kitties a1..a4, opened in that
# order, each 500x300 at its own corner of the desktop (a1 bottom right, a2
# top left, a3 bottom left, a4 top right), then focused a1, a3, a2, a4.
# Asserts, read off where the tile pass DREW each tile (`hyprdrive:debug
# drawn`), in reading order (rows top to bottom, each left to right):
#   created  -> a1 a2 a3 a4
#   recent   -> a4 a2 a3 a1 (last focused first)
#   position -> a2 a4 / a3 a1 on two rows (b2 by x on one row: a2 a3 a4 a1)
# and, in every mode, with the overview up: hovering each tile moves no tile
# (on the rig a hover does not focus), nor does focusing a1 then a3 by
# dispatch (in recent that moves Hyprland's focus history); a window opened
# meanwhile (a5) is appended last and moves none of the others; closing it
# gives the first order back. Then, in recent:
# closed, a1 focused, reopened -> a1 comes first (the order is taken at open).
# Control: --control sets the mode to `created` for every case, so the recent
# and position orders are not met and the gate must FAIL.
source "$(dirname "$0")/lib.sh"
gate_begin tord

CORNER="960 1060"
# drawn_lines CLASSES... -> "class cx cy h" of where each tile was drawn
drawn_lines() {
    local c
    for c in "$@"; do dbg "drawn $c" | grep -o "drawn $c [0-9 -]*"; done | awk 'NF==6 {print $2, $3+$5/2, $4+$6/2, $6}'
}
# banded -> "row cx class": a new row starts when a centre is more than half
# a tile below the row's first
banded() { sort -k3,3n | awk '{ if (NR == 1 || $3 - y > h / 2) { row++; y = $3; h = $4 } print row, $2, $1 }'; }
# order CLASSES... -> the classes in reading order (rows top down, each left to right)
order() { drawn_lines "$@" | banded | sort -k1,1n -k2,2n | awk '{printf "%s%s", (NR > 1 ? " " : ""), $3} END {print ""}'; }
# rows CLASSES... -> how many rows the drawn tiles make
rows() { drawn_lines "$@" | banded | awk '{r = $1} END {print r + 0}'; }
place() { # place CLASS X Y -- float it and put its top left at X,Y
    local a; a=$(addr_of "$1")
    ev "hl.dispatch(hl.dsp.window.float({ window = [[address:$a]], action = \"enable\" }))"
    ev "hl.dispatch(hl.dsp.window.resize({ window = [[address:$a]], x = 500, y = 300, relative = false }))"
    ev "hl.dispatch(hl.dsp.window.move({ window = [[address:$a]], x = $2, y = $3, relative = false }))"
}

rig_start
ev 'hl.monitor({ output = "HEADLESS-1", mode = "1920x1080@60", position = "0x0", scale = 1 })'
cfg '{general={gaps_in=5,gaps_out=10,border_size=2,layout="dwindle"}}'
cfg '{plugin={hyprdrive={scale=0.82,layout="horizontal",workspace_gap=40,anim_scale=0}}}'
sleep 0.5
go 1
for c in a1 a2 a3 a4; do kw "$c" 500 300; done
place a1 1300 700; place a2 100 100; place a3 200 700; place a4 1200 100
sleep 0.5
for c in a1 a3 a2 a4; do focus "$(addr_of $c)"; sleep 0.2; done
sleep 0.5
echo "desktop: $(geom)"

opened() { $R ptr move $CORNER >/dev/null; sleep 0.3; $R overview >/dev/null; sleep 1.5; }
closed() { $R ptr move $CORNER >/dev/null; sleep 0.3; $R overview >/dev/null; sleep 1.2; }

for mode in created recent position; do
    set_mode=$mode; [ "$CONTROL" = 1 ] && set_mode=created
    cfg "{plugin={hyprdrive={overview_tile_order=\"$set_mode\"}}}"
    # the focus history the cases start from: a4 last, as set up
    for c in a1 a3 a2 a4; do focus "$(addr_of $c)"; sleep 0.15; done
    sleep 0.3
    opened
    got=$(order a1 a2 a3 a4); nrows=$(rows a1 a2 a3 a4)
    case $mode in
        created)  want="a1 a2 a3 a4" ;;
        recent)   want="a4 a2 a3 a1" ;;
        position) if [ "$nrows" = 1 ]; then want="a2 a3 a4 a1"; else want="a2 a4 a3 a1"; fi ;;
    esac
    check "$mode: the grid reads $want ($nrows rows)" "$(is "$got" "$want")" "got: $got"
    # hover each tile: no reorder
    now=$got
    for h in a4 a3 a2 a1; do
        b=$(dbg "drawn $h" | grep -o "drawn $h [0-9 -]*")
        set -- $b
        [ $# = 6 ] || continue
        $R ptr move $(( $3 + $5 / 2 )) $(( $4 + $6 / 2 )) >/dev/null; sleep 0.5
        now=$(order a1 a2 a3 a4)
        [ "$now" = "$got" ] || break
    done
    check "$mode: hovering every tile reorders nothing" "$(is "$now" "$got")" "after hovering $h: $now"
    # focus changes while it is up (in recent, the history moves): no reorder
    for f in a1 a3; do focus "$(addr_of $f)"; sleep 0.4; done
    check "$mode: focusing a1 then a3 while it is up reorders nothing" "$(is "$(order a1 a2 a3 a4)" "$got")" "got: $(order a1 a2 a3 a4), focused $(J activewindow | jq -r .class)"
    $R ptr move $CORNER >/dev/null; sleep 0.3
    # a window opened while the overview is up is appended; nothing else moves
    kw a5 500 300; sleep 1.2
    grown=$(order a1 a2 a3 a4 a5)
    without=$(echo "$grown" | tr ' ' '\n' | grep -v '^a5$' | tr '\n' ' ' | sed 's/ $//')
    last=${grown##* }
    check "$mode: a window opened meanwhile goes last, the rest keep their order" "$( [ "$last" = a5 ] && [ "$without" = "$got" ] && echo 1 || echo 0)" "got: $grown"
    close_win "$(addr_of a5)"; sleep 1.2
    check "$mode: closing it gives the order back" "$(is "$(order a1 a2 a3 a4)" "$got")" "got: $(order a1 a2 a3 a4)"
    closed
    if [ "$mode" = recent ]; then
        focus "$(addr_of a1)"; sleep 0.3
        opened
        again=$(order a1 a2 a3 a4)
        check "recent: a1 focused before the next open comes first" "$(is "${again%% *}" a1)" "got: $again"
        closed
    fi
done

gate_end
