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
#   4. the picture, filmed (user, 2026-11-18, Hyprland 0.57: "Firefox's box
#      mostly black, a cropped sliver of the old picture at its right edge"):
#      a round trip of b, the window on the RIGHT (its box travels 967 <-> 0
#      px, so a picture cropped at the box's own x is a band that wide), at
#      ~15 frames a second: no frame has a hole -- a black or empty area at
#      least 48x48 px (the gaps between windows are 12-14 px) -- and enough
#      frames fell inside the trips to tell. On 0.57 a moving window's
#      elements are drawn onto a canvas of the window's own size (its
#      FlightBlur frost), with the canvas's damage, and the hold's elements
#      matched their monitor-px box against it: only the part right of the
#      box's x was drawn, the band growing as the box moved right. Checks 1-3
#      read the trace and never saw it.
#   5. b's holds in that trip end on its frames ("its second frame at the new
#      size", within SETTLE_MS), not on the fallback for a client that drew
#      once and stood still: on 0.57 Hyprland's own commit handler takes a
#      surface's damage before the hold's, every commit read 0% repainted and
#      every hold waited out the 250 ms settle (Firefox's picture frozen for a
#      quarter of a second).
# Control: --control-so <this commit with src/layout/TripFrame.cpp,
# TripFrameRule.hpp and FullscreenDesktop.cpp's resizing() reverted to
# 8c358b3, its two trace lines kept: "travel landed" and the "box WxH of WxH"
# of "live again">: checks 1 and 2 fail (released at ~275 ms with the box at
# 1/5 of its way). Recorded 2026-11-18: FAILS as it must.
# Control for 4 and 5 (Hyprland 0.57 line): --control-so <this commit with
# TripFrame.cpp's draw() matching the layer's box against the damage as it
# is, without the canvas's render modifier> fails 4; <... with the repaint
# read at the commit event only, not from the pending state at precommit>
# fails 5. abb9a22 (both) fails 4 and 5. Recorded 2026-11-18: FAIL as they must.
source "$(dirname "$0")/lib.sh"
gate_begin fss
control_build "this commit with the stretch reverted and its trace lines kept (see the header)"
rig_start; trace_on
# kitty fixtures in colours of their own (check 4: black is a hole, not a background)
kc() {
    $R win kitty -o cursor_blink_interval=0 -o remember_window_size=no -o initial_window_width="$2" -o initial_window_height="$3" -o background="$4" --class "$1" >/dev/null
    wait_for 5 "[ -n \"\$(addr_of $1)\" ]" || echo "warn: $1 never mapped"
}
go 1; kc a 900 600 '#2050c0'; kc b 700 450 '#a040c0'
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
# 4. b's round trip, filmed on the rig's own output (half size, so 24 px there)
burst() { SU mkdir -p "$ST/$1"; SU env XDG_RUNTIME_DIR="$RT" WAYLAND_DISPLAY="$(SU cat "$ST/wl")" bash -c "for i in \$(seq $2); do grim -t jpeg -q 85 -s 0.5 -o HEADLESS-1 $ST/$1/\$(date +%s%3N).jpg; done" & BP=$!; }
B=$(addr_of b)
n2=$(trace | wc -l)
for d in set unset; do
    burst "film-$d" 25; sleep 0.2
    echo "$(date +%s%3N)" > "$GOUT/t-$d"; focus "$B"; fs "$B" $d
    wait $BP; sleep 0.8
done
settle 4 geom
rm -rf "$GOUT/frames"; mkdir -p "$GOUT/frames"; SU tar -C "$ST" -cf - film-set film-unset | tar -C "$GOUT/frames" -xf -
holes=$(python3 - "$GOUT" <<'PY' 2>&1
import sys, glob, os, numpy as np
from PIL import Image
g = sys.argv[1]; K = 24; inside = 0; bad = []
for d in ('set', 'unset'):
    t0 = int(open(f'{g}/t-{d}').read())
    for f in sorted(glob.glob(f'{g}/frames/film-{d}/*.jpg')):
        t = int(os.path.basename(f)[:-4]) - t0
        inside += 100 <= t <= 1000
        m = (np.asarray(Image.open(f).convert('RGB')) < 12).all(2).astype(np.int32)
        S = np.pad(m.cumsum(0).cumsum(1), ((1, 0), (1, 0)))
        win = S[K:, K:] - S[:-K, K:] - S[K:, :-K] + S[:-K, :-K]   # black pixels in each KxK square
        ys, xs = np.nonzero(win == K * K)
        if len(xs): bad.append(f"{d}{t:+d}ms:x{2 * xs.min()}-{2 * (xs.max() + K)}")
print(inside, len(bad), ' '.join(bad[:8]))
PY
)
check "4: b's round trip filmed: frames inside both trips (>= 16 between +100 and +1000 ms)" "$(ge "${holes%% *}" 16)" "$holes"
h=${holes#* }
check "4: ... no frame has a black/empty hole of 48x48 px or more (where: ms after the toggle, x span)" "$(is "${h%% *}" 0)" "$h"
# 5. why b's holds of that trip let go: "<ms>|<why>" per hold
why=$(trace | tail -n +"$((n2 + 1))" | sed -nE 's/^[0-9]+ tripframe b released after ([0-9]+) ms: ([^;]*).*/\1|\2/p' | tr '\n' ';')
check "5: b's 2 holds let go on its second frame at the new size, within 250 ms (ms|why)" \
    "$(echo "$why" | tr ';' '\n' | awk -F'|' 'NF == 2 { n++; if ($2 == "its second frame at the new size" && $1 < 250) ok++ } END { print (n == 2 && ok == 2) ? 1 : 0 }')" "$why"
trace > "$GOUT/trace.txt" 2>/dev/null
gate_end
