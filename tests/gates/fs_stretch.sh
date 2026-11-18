#!/usr/bin/env bash
# fs_stretch -- a fast client's fullscreen trip: its picture stretches into the box until the box lands.
# Protects (user, 2026-11-18: "Zen snaps jankily partway through the
# transition"; TripFrameRule.hpp, "THE PICTURE STRETCHES"): the trip's hold
# let go of a fast client at its own pace (~275 ms) while the box still had a
# fifth of its way to go (it lands ~1.15 s in), and Hyprland drew the new
# buffer cropped / smeared in the still-growing box -- the snap. Now the hold
# draws the picture stretched (live, the snapshot only until the client has
# drawn, then a fade back to live) until the box has landed.
# Fixture: kitty a 900x600 and b 700x450 on desktop 1 (a fast Wayland client).
# Asserts, from the trace (fsdesk "travel landed", tripframe "live again"):
#   1. two round trips at the default speed: every hold of a lets go at or
#      after its box lands, and the box is at its goal size then
#   2. one round trip at anim_scale 2 (the box travels ~2.3 s, past the 2 s
#      cap on the frozen picture): the same
#   3. every window back at its box after the trips
# Control: --control-so <this commit with src/layout/TripFrame.cpp,
# TripFrameRule.hpp and FullscreenDesktop.cpp's resizing() reverted to
# 8c358b3, its two trace lines kept: "travel landed" and the "box WxH of WxH"
# of "live again">: checks 1 and 2 fail (released at ~275 ms with the box at
# 1/5 of its way). Recorded 2026-11-18: FAILS as it must.
source "$(dirname "$0")/lib.sh"
gate_begin fss
control_build "this commit with the stretch reverted and its trace lines kept (see the header)"
rig_start; trace_on
go 1; kw a 900 600; kw b 700 450
$R ptr move 700 1000 >/dev/null; settle 4 geom; G0=$(geom)
A=$(addr_of a)
check "start: a and b on 1" "$(is "$(ids)" 1:2)" "$(ids)"
trips() {  # trips N PAUSE -- N round trips of a
    local k; for k in $(seq "$1"); do focus "$A"; fs "$A" set; sleep "$2"; fs "$A" unset; sleep "$2"; done
}
# every trip of a in the trace from line $1 on: "<released at> <landed at> <box> <goal>" (ms)
holds() {
    trace | tail -n +"$1" | python3 -I -c '
import sys, re
held = None; land = None
for l in sys.stdin:
    m = re.match(r"(\d+) (\w+) a (.*)", l)
    if not m: continue
    t, mod, rest = int(m[1]), m[2], m[3]
    if mod == "tripframe" and rest.startswith("held"): held, land = t, None
    elif mod == "fsdesk" and rest.startswith("travel landed") and held is not None and land is None: land = t
    elif mod == "tripframe" and rest.startswith("live again") and held is not None:
        b = re.search(r"box (\d+)x(\d+) of (\d+)x(\d+)", rest)
        print(t - held, "-" if land is None else land - held, f"{b[1]}x{b[2]}" if b else "?", f"{b[3]}x{b[4]}" if b else "?")
        held = None'
}
# late: holds let go before their box landed, or with the box short of its goal
late() { echo "$1" | awk 'NF == 4 && ($2 == "-" || $1 < $2 || $3 != $4)' | tr '\n' ';'; }
n0=$(trace | wc -l)
trips 2 2.2
H1=$(holds $((n0 + 1)))
check "1: two round trips: 4 holds traced" "$(is "$(echo "$H1" | grep -c .)" 4)" "$(echo "$H1" | tr '\n' ';')"
check "1: every hold lets go at or after its box lands, the box at its goal (released/landed ms, box, goal)" "$(is "$(late "$H1")" "")" "$(echo "$H1" | tr '\n' ';')"
cfg '{plugin={hyprdrive={anim_scale=2}}}'
n1=$(trace | wc -l)
trips 1 3.6
H2=$(holds $((n1 + 1)))
cfg '{plugin={hyprdrive={anim_scale=1}}}'
check "2: anim_scale 2, a round trip: 2 holds traced" "$(is "$(echo "$H2" | grep -c .)" 2)" "$(echo "$H2" | tr '\n' ';')"
check "2: ... each lets go at or after its box lands (past the 2 s cap), the box at its goal" "$(is "$(late "$H2")" "")" "$(echo "$H2" | tr '\n' ';')"
sleep 1; settle 4 geom
check "3: every window back at its box" "$(is "$(geom)" "$G0")" "$(geom) -- was $G0"
trace > "$GOUT/trace.txt" 2>/dev/null
gate_end
