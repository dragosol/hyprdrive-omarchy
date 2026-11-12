#!/usr/bin/env bash
# tooltip_flash -- the dock tooltip fades in and out as one surface: its body never shows the border colour.
# Protects: the dock's app-name tooltip (and every label pill: they share
# OverviewRender::labelPill / ringedPlate) -- owner, 2026-11-11: "some of the
# tooltips are flashing their border color before the body of the tooltip goes
# back to its default black background color". The ring was a filled rect
# under the 90%-black body, both at the fade's alpha, so mid-fade the body held
# up to ~2.8x the accent it settles with.
# Filmed: anim_scale 8 (the 150 ms entrance takes 1.2 s), a saturated accent
# (general:col.active_border rgb(ff2000), border_size 4), grim crops as fast
# as the rig gives them (~50/s). Per frame the plate is found as the pill-shaped
# blob of accent-tinted pixels and its body (the middle third, text excluded)
# is measured as accent chroma R - (G+B)/2. Only the band above the hovered
# icon's cell is searched: on Omarchy that cell is accent-tinted at rest.
# Asserts:
#   1. the plate was filmed: some frame shows it, and it settles (chroma of the
#      settled body > 10, i.e. the 10% accent the design keeps)
#   2. entrance: no frame's body has more accent than both the first and the
#      settled frame (+8 of 255)
#   3. exit: the same for the fade-out
# Control (--control --control-so <a4d6892's build, the ring rect under the
# body>): 2 and 3 fail (body chroma ~64 against a settled ~24). Recorded
# 2026-11-11 on a4d6892: control FAILS as it must. Omarchy: control = the
# parent of "a ringed plate fades as one surface" (entrance peak 64.4 against
# a settled 24.1, exit 64.9); the fixed build peaks at 24.1 against 23.1.
source "$(dirname "$0")/lib.sh"
gate_begin tfl
control_build "a build that draws the label ring under the body (a4d6892)"
rig_start
cfg '{ general = { border_size = 4, col = { active_border = "rgb(ff2000)" } } }'
cfg '{plugin={hyprdrive={dock={autohide=false}}}}'
cfg '{plugin={hyprdrive={anim_scale=8}}}'
kw appa; kw appb; sleep 0.8
ICON=$(dbg dockmenu | grep -o 'icon appa [-0-9,]* [0-9x]*' | awk '{print $3, $4}')
[ -n "$ICON" ] || { check "dockmenu reports appa's icon" 0 "$(dbg dockmenu | tr '\n' ' ')"; gate_end; }
set -- $(echo "$ICON" | tr ',x' '  '); IX=$(( $1 + $3 / 2 )); IY=$(( $2 + $4 / 2 ))
# the band above the icon the plate (and the hover pane under it) occupies
GX=$(( IX - 150 )); GY=$(( IY - 300 )); [ $GY -lt 0 ] && GY=0
# Rows from CUT down are the hovered icon's cell (the icon less its 6 px hover
# halo, and 2 px to spare). Omarchy tints that cell in the accent (22%, X56)
# from the first hovered frame: a pill-shaped accent blob the plate finder
# took for the tooltip while the plate itself was still too faint to find.
CUT=$(( $2 - 8 - GY ))
$R ptr move 960 300 >/dev/null; sleep 1.5

# film NAME MS STEP -- STEP in the background, grim crops into $ST/burst/NAME_<ms>.png
film() {
    SU mkdir -p "$ST/burst"; SU chmod 755 "$ST/burst"
    ( $R ptr $3 >/dev/null 2>&1 ) & local bg=$!
    SU env XDG_RUNTIME_DIR="$RT" WAYLAND_DISPLAY="$(SU cat "$ST/wl")" bash -c '
        t0=$(date +%s%N); end=$(( t0 + '"$2"'000000 ))
        while :; do now=$(date +%s%N); [ $now -lt $end ] || break
            grim -g "'"$GX,$GY 300x300"'" "'"$ST"'/burst/'"$1"'_$(printf %05d $(( (now - t0) / 1000000 ))).png"; done'
    wait $bg
}
# body NAME -- "first peak last frames": the body's accent chroma over the burst
body() {
    python3 -P -c '
import glob, sys, numpy as np
from PIL import Image
from scipy import ndimage
vals = []
for f in sorted(glob.glob(sys.argv[1] + "_*.png")):
    a = np.asarray(Image.open(f).convert("RGB"), float)[: int(sys.argv[2])]
    ch = a[:, :, 0] - (a[:, :, 1] + a[:, :, 2]) / 2
    lab, n = ndimage.label(ch > 12)
    best = None
    for i, sl in enumerate(ndimage.find_objects(lab)):
        h, w = sl[0].stop - sl[0].start, sl[1].stop - sl[1].start
        if 24 <= h <= 90 and h < w < 400 and (best is None or w * h > best[2]):
            best = (sl, i + 1, w * h)
    if best is None:
        vals.append(None); continue
    (ys, xs), h = best[0], best[0][0].stop - best[0][0].start
    inner = a[ys.start + h // 3: ys.stop - h // 3, xs.start + h // 2: xs.stop - h // 2].reshape(-1, 3)
    inner = inner[inner.mean(axis=1) < 90]          # not the text
    vals.append(float((inner[:, 0] - (inner[:, 1] + inner[:, 2]) / 2).mean()) if len(inner) else None)
seen = [v for v in vals if v is not None]
first = vals[0] if vals and vals[0] is not None else 0.0
last = vals[-1] if vals and vals[-1] is not None else 0.0
print("%.1f %.1f %.1f %d" % (first, max(seen) if seen else 0.0, last, len(vals)))
' "$ST/burst/$1" "$CUT"
}

film in 2400 "move $IX $IY"; sleep 1
read -r F0 P0 L0 N0 <<< "$(body in)"
film out 2400 "move 960 300"; sleep 0.5
read -r F1 P1 L1 N1 <<< "$(body out)"
echo "      entrance: first $F0 peak $P0 settled $L0 ($N0 frames); exit: settled $F1 peak $P1 last $L1 ($N1 frames)"
gt() { python3 -I -c "import sys; print(1 if float(sys.argv[1]) > float(sys.argv[2]) else 0)" "$1" "$2"; }
check "1: the tooltip was filmed and settles with its 10% accent body" "$(gt "$L0" 10)" "settled body chroma $L0 over $N0 frames"
H0=$(python3 -I -c "import sys; print(max(float(sys.argv[1]), float(sys.argv[2])) + 8)" "$F0" "$L0")
H1=$(python3 -I -c "import sys; print(max(float(sys.argv[1]), float(sys.argv[2])) + 8)" "$F1" "$L1")
check "2: entrance: the body never holds more accent than at its ends" "$(gt "$H0" "$P0")" "peak $P0 > $H0 (ends $F0 / $L0)"
check "3: exit: the body never holds more accent than at its ends" "$(gt "$H1" "$P1")" "peak $P1 > $H1 (ends $F1 / $L1)"
gate_end
