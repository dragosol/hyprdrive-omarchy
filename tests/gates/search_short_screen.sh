#!/usr/bin/env bash
# search_short_screen -- on a short logical screen with a top bar, typing never docks the search pill downward, and hovering a strip desktop lifts the pill for its label.
# Protects layoutSearch's dock clamp and its uncapped strip-label lift
# (src/search/SearchView.cpp) and the label's gaps (stripCellLabelLive,
# src/overview/OverviewStrip.cpp). The case: fedora while the XPS streams it
# (xps-stream-display: DP-1 1920x1200 @ 1.0666667 = 1800x1125 logical) with a
# top bar. There the pill's rest row (the logical row inset by the overview
# scale only) lies ABOVE the docked line (bar + gaps_out +
# OVERVIEW_SEARCH_DOCKPADL): docking moved the pill DOWN to it, and the lift for
# a strip label, capped at that line, was 0 -- the label squeezed both its gaps
# to nothing. The owner (2026-11-19): docking never moves the pill below where
# it rests; the lift is whatever the label needs, even off the screen.
# State from `hyprdrive:debug search` (search.lib).
# Fixtures: HEADLESS-1 at MODE (default 1920x1200@60), scale SCL (default
# 1.0666667); a one-line kitty panel on the top edge; gsalpha on desktop 2,
# gsbeta on desktop 1, focused; overview scale 0.84, horizontal, the mosaic on.
# Asserts:
#   1. hovering desktop 1's strip cell with no query: the pill lifts for the
#      label (pillNudgeWant > 5 and the pill > 3 px higher than at rest)
#   2. the pointer off the strip: the pill goes back to its rest y
#   3. typing "gsalpha": the pill docks (dock 1.00) and its y is not below rest
# MODE=3840x2160@60 SCL=1 (fedora native) passes on every build.
# Control (--control --control-so <the build before this fix, a613c42>): 1 and 3
# fail at the default mode. Recorded 2026-11-19: control FAILS as it must.
source "$(dirname "$0")/lib.sh"
control_build "the build before the short-screen fix (a613c42)"
gate_begin ss
source "$GATES_DIR/search.lib"
cell1() { local P I; P=$(dbg stripclean | grep -m1 -o 'plate=[0-9]*,[0-9]* [0-9]*x[0-9]*' | tr '=,x' '   '); I=$(dbg stripclean | grep -m1 -o 'images=[0-9]*' | cut -d= -f2); set -- $P; echo $(( $2 + $4 / (2 * ${I:-3}) )) $(( $3 + $5 / 2 )); }
search_start
cfg "{plugin={hyprdrive={scale=0.84,layout=\"horizontal\",workspace_gap=40,anim_scale=1.053,mosaic={infinite_canvas=true,zoom_labels=\"hover\",default=true}}}}" >/dev/null
$R eval "hl.monitor({ output = 'HEADLESS-1', mode = '${MODE:-1920x1200@60}', position = '0x0', scale = ${SCL:-1.0666667} })" >/dev/null; sleep 1
$R win kitty +kitten panel --edge=top --lines=1 sleep 9999 >/dev/null; sleep 2
go 2; kw gsalpha 700 450; go 1; kw gsbeta 700 450; sleep 0.5
focus "$(addr_of gsbeta)"; $R ptr move 300 600 >/dev/null; settle 3 geom
M=HEADLESS-1

$R overview >/dev/null
until_s 4 '[ "$(sk overviews)" = 1 ] && [ "$(mk $M drawable)" = 1 ]'; settle_s $M
REST_Y=$(box $M pill 2)

set -- $(cell1); $R ptr move "$1" "$2" >/dev/null; sleep 1.2; snap
check "1: hovering desktop 1 with no query: the pill lifts for the label" \
    "$( [ "$(fgt "$(mk $M pillNudgeWant)" 5)" = 1 ] && [ "$(flt "$(box $M pill 2)" $((REST_Y - 3)))" = 1 ] && echo 1 || echo 0)" \
    "pillNudgeWant $(mk $M pillNudgeWant) pill y $REST_Y -> $(box $M pill 2) gridNudgeL $(mk $M gridNudgeL) ($(J monitors | jq -c '.[0] | [.width,.height,.scale,.reserved]'))"
$R ptr path 30 300 600 330 640 360 680 >/dev/null; sleep 1.2; snap   # a path: one lone move does not always reach the overview on the rig
check "2: off the strip, the pill goes back to rest" "$(is "$(box $M pill 2)" "$REST_Y")" "pill y $REST_Y -> $(box $M pill 2) pillNudgeWant $(mk $M pillNudgeWant)"

$R type gsalpha >/dev/null; until_s 4 '[ "$(mk $M hits)" -ge 1 ] 2>/dev/null'; settle_s $M
check "3: typing docks the pill up, never down" "$(is "$(cur $M dock) $([ "$(box $M pill 2)" -le "$REST_Y" ] && echo up)" "1.00 up")" \
    "dock $(mk $M dock) pill y $REST_Y -> $(box $M pill 2)"
$R tap Escape >/dev/null; $R tap Escape >/dev/null; sleep 0.8
gate_end
