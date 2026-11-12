#!/usr/bin/env bash
# fs_trip_x11 -- a SLOW X11 client's fullscreen trip: one size each way, and
# everything back where it was.
# Protects (user, 2026-11-11: "battle.net struggling exiting in and out of
# fullscreen - animation is buggy"; TripSizes.hpp): the trip's move
# (moveWindowToWorkspace) took the window out of fullscreen and back, so an X11
# client was configured to fullscreen, its tile on the new desktop, its mosaic
# size and fullscreen again within 3 ms, and coming home to the trip desktop's
# lone tile and then its own size; a client that redraws late (Wine/CEF) drew
# each of them, white first, while the trip carried it.
# Fixture: tests/rig/xstate/slow_redraw.c (class slow_redraw.exe, 900x620,
# min 640x480, repaints 150 ms after a resize, white at once, insists on its
# own size), and kitty ka 700x450, kb 600x400, all on desktop 1, Mosaic.
# Asserts, after 5 round trips at a human pace and 10 quick ones (0.3 s):
#   1. every window of desktop 1 is at its pre-trip box, the made desktop gone
#   2. the client was only ever configured to its tile or to the screen
#   3. one configure each way: at most 2 per round trip (+ the first one)
# Control: --control-so <a4d6892, the build recorded by the owner>: check 2
# and 3 fail (1906x1066 and 896x616 between two fullscreens). Recorded
# 2026-11-11: FAILS as it must.
source "$(dirname "$0")/lib.sh"
gate_begin fsx
control_build "a4d6892, e.g. a copy of the hyprdrive-native tree's build"
cc -O2 -o "$GOUT/slow_redraw" "$ROOT/tests/rig/xstate/slow_redraw.c" $(pkg-config --cflags --libs xcb) || skip "cannot build slow_redraw (xcb headers?)"
chmod 755 "$GOUT" "$GOUT/slow_redraw"
xdisplay() { hlog 4000 | grep -o 'DISPLAY: :[0-9]*' | tail -1 | grep -o ':[0-9]*$'; }

rig_start; cfg '{general={layout="mosaic"}}'; trace_on
go 1; kw ka 700 450
wait_for 6 '[ -n "$(xdisplay)" ]' || { $R win env DISPLAY=:99 true >/dev/null 2>&1; wait_for 6 '[ -n "$(xdisplay)" ]'; }
D=$(xdisplay); [ -n "$D" ] || skip "XWayland never announced a display"
$R win env DISPLAY="$D" sh -c "'$GOUT/slow_redraw' --lag 150 --flash --insist > '$ST/fix.log' 2>&1" >/dev/null
wait_for 6 '[ -n "$(addr_of slow_redraw.exe)" ]' || { echo "the fixture never mapped:"; hlog 40 | grep -iE "xwayland|xwm" | tail -5; }
kw kb 600 400
$R ptr move 700 1000 >/dev/null; settle 4 geom; G0=$(geom); IDS0=$(ids)
F=$(addr_of slow_redraw.exe)
check "start: the three windows on 1" "$(is "$IDS0" 1:3)" "$IDS0"
for k in 1 2 3 4 5; do focus "$F"; fs "$F" set; sleep 1.6; fs "$F" unset; sleep 1.8; done
for k in $(seq 10); do focus "$F"; fs "$F" set; sleep 0.3; fs "$F" unset; sleep 0.3; done
sleep 2; settle 4 geom
check "1: every window back at its box after 15 round trips" "$(is "$(geom)" "$G0")" "$(geom) -- was $G0"
check "1: the made desktop is gone" "$(is "$(ids)" "$IDS0")" "$(ids)"
SIZES=$(SU cat "$ST/fix.log" | awk '$2 == "size" {print $3}')
TILE=$(echo "$G0" | tr '|' '\n' | awk '$1 == "slow_redraw.exe" {print $4}')
SCREEN=$(J monitors | jq -r '.[0] | "\(.width)x\(.height)"')
odd=$(echo "$SIZES" | grep -vxE "$TILE|$SCREEN" | sort | uniq -c | tr '\n' ' ')
check "2: the client is only ever configured to its tile ($TILE) or the screen ($SCREEN)" "$(is "$odd" "")" "other sizes: $odd"
n=$(echo "$SIZES" | grep -c .)
check "3: one configure each way (<= 31 for 15 round trips)" "$(le "$n" 31)" "$n configures"
gate_end
