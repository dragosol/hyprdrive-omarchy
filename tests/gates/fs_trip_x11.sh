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
# The fixture also maps a helper window of its own app (same class, same pid)
# 60 ms after each time it leaves fullscreen, for 350 ms: Battle.net's helper
# windows, which voided the home desktop's hold and swapped the launcher.
# Asserts, after 5 round trips at a human pace and 10 quick ones (0.3 s):
#   1. every window of desktop 1 is at its pre-trip box, the made desktop gone
#   2. the client was only ever configured to its tile or to the screen
#   3. one configure each way: at most 2 per round trip (+ the first one)
#   4. never a blank frame: in screenshot bursts over trip 1 and the quick
#      ones, no frame shows the fixture's white (its frame before it redraws
#      at a new size; the only white in the rig) -- the trip draws its last
#      good frame until then (TripFrame.hpp)
#   5. the recording's quick toggles (6 toggles 0.2-0.4 s apart, then off),
#      three times, the helper coming and going: every window back each time
#   6. no OTHER window is ever bigger than its own tile during the trips:
#      sampled every ~50 ms (its box), and drawn (in the bursts, the largest
#      blob of its own colour: ka blue, kb purple, the helper green) -- the
#      recording's small green window seen far larger than itself as home
#      slid back in; the neighbours travel with home (travelOthers)
# Control: --control-so <a4d6892, the build recorded by the owner>: check 2
# and 3 fail (1906x1066 and 896x616 between two fullscreens), 4 and 5 too.
# Recorded 2026-11-11: FAILS as it must. The build before TripFrame (2d66790)
# fails 4 (white frames) and 5 (the launcher swapped with kb).
source "$(dirname "$0")/lib.sh"
gate_begin fsx
control_build "a4d6892, e.g. a copy of the hyprdrive-native tree's build"
cc -O2 -o "$GOUT/slow_redraw" "$ROOT/tests/rig/xstate/slow_redraw.c" $(pkg-config --cflags --libs xcb) || skip "cannot build slow_redraw (xcb headers?)"
chmod 755 "$GOUT" "$GOUT/slow_redraw"
xdisplay() { hlog 4000 | grep -o 'DISPLAY: :[0-9]*' | tail -1 | grep -o ':[0-9]*$'; }

rig_start; cfg '{general={layout="mosaic"}}'; trace_on
# kitty fixtures in colours of their own (check 6 finds them in the frames; a
# grey foreground keeps check 4's white the fixture's alone)
kc() {
    $R win kitty -o remember_window_size=no -o initial_window_width="$2" -o initial_window_height="$3" -o background="$4" -o foreground=#a0a0a0 --class "$1" >/dev/null
    wait_for 5 "[ -n \"\$(addr_of $1)\" ]" || echo "warn: $1 never mapped"
}
go 1; kc ka 700 450 '#2050c0'
wait_for 6 '[ -n "$(xdisplay)" ]' || { $R win env DISPLAY=:99 true >/dev/null 2>&1; wait_for 6 '[ -n "$(xdisplay)" ]'; }
D=$(xdisplay); [ -n "$D" ] || skip "XWayland never announced a display"
$R win env DISPLAY="$D" sh -c "'$GOUT/slow_redraw' --lag 150 --flash --insist --child-unfs 60 --child-life 350 > '$ST/fix.log' 2>&1" >/dev/null
wait_for 6 '[ -n "$(addr_of slow_redraw.exe)" ]' || { echo "the fixture never mapped:"; hlog 40 | grep -iE "xwayland|xwm" | tail -5; }
kc kb 600 400 '#a040c0'
$R ptr move 700 1000 >/dev/null; settle 4 geom; G0=$(geom); IDS0=$(ids)
# Every window's box (class, title, fullscreen, size), ~20 times a second.
sampler_start boxes "J clients | jq -c '[.[] | [.class, .title, .fullscreen, .size]]'"
# (Its first sizes, as it opened, are the layout's business: counted from here.)
N0=$(SU cat "$ST/fix.log" | awk '$2 == "size"' | wc -l)
F=$(addr_of slow_redraw.exe)
check "start: the three windows on 1" "$(is "$IDS0" 1:3)" "$IDS0"
# Screenshot bursts (half size), on the rig's own output.
burst() { SU mkdir -p "$ST/$1"; SU env XDG_RUNTIME_DIR="$RT" WAYLAND_DISPLAY="$(SU cat "$ST/wl")" bash -c "for i in \$(seq $2); do grim -t jpeg -q 80 -s 0.5 -o HEADLESS-1 $ST/$1/\$(date +%s%3N).jpg; done" & BP=$!; }
for k in 1 2 3 4 5; do
    [ $k = 1 ] && burst trip1 90
    focus "$F"; fs "$F" set; sleep 1.6; fs "$F" unset; sleep 1.8
    [ $k = 1 ] && wait $BP
done
burst quick 80
for k in $(seq 10); do focus "$F"; fs "$F" set; sleep 0.3; fs "$F" unset; sleep 0.3; done
wait $BP
sleep 2; settle 4 geom
check "1: every window back at its box after 15 round trips" "$(is "$(geom)" "$G0")" "$(geom) -- was $G0"
check "1: the made desktop is gone" "$(is "$(ids)" "$IDS0")" "$(ids)"
SIZES=$(SU cat "$ST/fix.log" | awk '$2 == "size" {print $3}' | tail -n +$((N0 + 1)))
TILE=$(echo "$G0" | tr '|' '\n' | awk '$1 == "slow_redraw.exe" {print $4}')
SCREEN=$(J monitors | jq -r '.[0] | "\(.width)x\(.height)"')
odd=$(echo "$SIZES" | grep -vxE "$TILE|$SCREEN" | sort | uniq -c | tr '\n' ' ')
check "2: the client is only ever configured to its tile ($TILE) or the screen ($SCREEN)" "$(is "$odd" "")" "other sizes: $odd"
n=$(echo "$SIZES" | grep -c .)
check "3: one configure each way (<= 30 for 15 round trips)" "$(le "$n" 30)" "$n configures"
rm -rf "$GOUT/frames"; mkdir -p "$GOUT/frames"; SU tar -C "$ST" -cf - trip1 quick | tar -C "$GOUT/frames" -xf -
white=$(python3 - "$GOUT/frames" <<'PY' 2>&1
import sys, glob, numpy as np
from PIL import Image
bad = []
for f in sorted(glob.glob(sys.argv[1] + '/*/*.jpg')):
    w = (np.asarray(Image.open(f).convert('RGB')) > 235).all(2).mean()
    if w > 0.002: bad.append(f"{f.split('/')[-2]}/{f.split('/')[-1][:-4]}:{w:.2f}")
print(len(bad), ' '.join(bad[:6]))
PY
)
nframes=$(ls "$GOUT"/frames/*/*.jpg 2>/dev/null | wc -l)
check "4: never a blank frame ($nframes frames over trip 1 and the quick trips)" "$( [ "$nframes" -ge 100 ] && is "${white%% *}" 0 || echo 0)" "white frames: $white"
# 5. The recording's quick toggles, three times.
for r in 1 2 3; do
    a=set; for d in 0.3 0.2 0.4 0.25 0.35 0.2; do focus "$F"; fs "$F" $a; sleep $d; [ $a = set ] && a=unset || a=set; done
    fs "$F" unset; sleep 2.5; settle 4 geom
    check "5: quick toggles $r: every window back" "$(is "$(geom)" "$G0")" "$(geom) -- was $G0"
done
# 6. The others: never bigger than their own tile (+4 px), sampled or drawn.
sampler_stop boxes >/dev/null
TILES=$(echo "$G0" | tr '|' '\n' | awk '$1 == "ka" || $1 == "kb" {print $1, $4}')
big=$(python3 - "$GOUT/boxes.samp" "$TILES" <<'PY' 2>&1
import sys, json
tiles = {l.split()[0]: tuple(map(int, l.split()[1].split('x'))) for l in sys.argv[2].splitlines() if l.strip()}
tiles['helper'] = (360, 240)
bad = []
for line in open(sys.argv[1]):
    try: cl = json.loads(line)
    except Exception: continue
    for c, title, fs, (w, h) in cl:
        k = 'helper' if title == 'slow redraw helper' else c
        if k in tiles and not fs and (w > tiles[k][0] + 4 or h > tiles[k][1] + 4): bad.append(f"{k}:{w}x{h}")
print(len(bad), ' '.join(sorted(set(bad))[:6]))
PY
)
nb=$(wc -l < "$GOUT/boxes.samp")
check "6: sampled ($nb samples), no other window bigger than its tile" "$( [ "$nb" -ge 100 ] && is "${big%% *}" 0 || echo 0)" "too big: $big"
drawn=$(python3 - "$GOUT/frames" "$TILES" <<'PY' 2>&1
import sys, glob, numpy as np
from PIL import Image
from scipy import ndimage
tiles = {l.split()[0]: tuple(map(int, l.split()[1].split('x'))) for l in sys.argv[2].splitlines() if l.strip()}
tiles['helper'] = (360, 240)
cols = {'ka': (0x20, 0x50, 0xc0), 'kb': (0xa0, 0x40, 0xc0), 'helper': (0x22, 0xaa, 0x44)}
bad = []; seen = {k: 0 for k in cols}
for f in sorted(glob.glob(sys.argv[1] + '/*/*.jpg')):
    a = np.asarray(Image.open(f).convert('RGB')).astype(int)
    for k, (r, g, b) in cols.items():
        m = (abs(a[..., 0] - r) < 30) & (abs(a[..., 1] - g) < 30) & (abs(a[..., 2] - b) < 30)
        m = ndimage.binary_opening(m)
        lab, n = ndimage.label(m)
        if not n: continue
        seen[k] += 1
        big = int(np.argmax(ndimage.sum(m, lab, range(1, n + 1)))) + 1
        ys, xs = np.where(lab == big)
        w, h = (xs.max() - xs.min() + 1) * 2, (ys.max() - ys.min() + 1) * 2   # half-size frames
        if w > tiles[k][0] + 8 or h > tiles[k][1] + 8: bad.append(f"{f.split('/')[-2]}/{f.split('/')[-1][:-4]}:{k}:{w}x{h}")
print(len(bad), min(seen.values()), 'frames showing', seen, ' '.join(bad[:6]))
PY
)
seen=$(echo "$drawn" | awk '{print $2}')
check "6: drawn, no other window bigger than its tile (each seen in $seen+ frames)" "$( [ "${seen:-0}" -ge 10 ] 2>/dev/null && is "${drawn%% *}" 0 || echo 0)" "$drawn"
trace > "$GOUT/trace.txt" 2>/dev/null
check "5: the helper came and went" "$(le 10 "$(SU cat "$ST/fix.log" | grep -c 'child mapped')")" "$(SU cat "$ST/fix.log" | grep -c 'child mapped') helper maps"
gate_end
