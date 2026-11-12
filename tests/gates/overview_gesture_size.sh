#!/usr/bin/env bash
# overview_gesture_size -- a swipe-opened overview draws its tiles at the size a key-opened one does, with Hyprdrive motion off and on.
# Protects: the fix for #10 ("if I turn off the animation ... the size can get
# very smaller even if I move it barely", 3-finger swipe up, Hyprdrive motion
# off). The strip draws every window of a desktop as a mini, and the tile pass
# a tile: for the length of that draw the window's size is the mini's / the
# tile's (Window.cpp renderOverviewWindow). A box query for the window from
# inside its own mini draw solved the grid with the window at MINI size --
# one window, the solver's scale capped at 1: a mini-sized slot -- and with
# motion off the reflow's one-call lag drew that slot in the next frame. Every
# frame of a driven swipe draws the strip again, so the tile stayed tiny for
# as long as the fingers were down (1920x1080 at 1.5, one window: 242 px wide
# instead of 880). GridSolve.hpp: the grid takes the sizes the frame observed
# before it drew, never a size a draw has imposed.
# The query on the reporter's machine is one the rig does not make, so this
# gate makes it: `hyprdrive:debug probe_draw self` asks for the drawn window's
# own box from inside every draw, strip minis included (docs/DEBUG.md).
# Rig: 1920x1080 at scale 1.5 with a 26 px top bar, gaps 5/10, border 2 (the
# reporter's Omarchy), the horizontal overview at 0.82, the 3-finger
# swipe up bound; the windows on desktop 1.
# Asserts, for motion off (anim_scale 0) and on (1), one and three windows,
# probe self and off (and once at scale 1.0), against the tile widths of a
# key-opened overview with the probe off (the reference):
#   - key-opened with the probe: every tile within 1 px of the reference;
#   - a swipe held at 20% of gesture_distance: motion off it is today's
#     snapped state, the desktop -- each window drawn at its own size; motion
#     on, every sample is drawn where the layout says (`drawn` == `tile`);
#   - held at 60%: motion off, the snapped open state -- every sample of every
#     tile within 1 px of the reference; motion on, `drawn` == `tile`;
#   - released: settled within 1 px of the reference; a full quick swipe too.
# The motion-off SNAP at 50% of travel (OverviewSwipe.cpp onSwipeUpdate) is
# today's design and is only recorded here, not judged.
# Control: --control --control-so <the release 1.9.57 (23bceff) with the
# probe_draw instrument (on|self)> -- with motion off, held at 60% and the
# probe on, the tile is drawn 242 px wide (reference 880; three windows 66-99
# vs 435) for as long as the swipe is held. Recorded 2026-11-12: see the commit.
source "$(dirname "$0")/lib.sh"
gate_begin ogs
control_build "1.9.57 (23bceff) + the probe_draw instrument with self"

CORNER="960 1060"
# sample MS FILE CLASSES... -- "<ms> drawn|tile <class> x y w h" lines, one hyprctl loop as hyprtest
sample() {
    local ms=$1 file=$2; shift 2
    SU env XDG_RUNTIME_DIR="$RT" HYPRLAND_INSTANCE_SIGNATURE="$SIG" sh -c '
        end=$(( $(date +%s%3N) + '"$ms"' ))
        while [ $(date +%s%3N) -lt $end ]; do t=$(date +%s%3N)
            for c in '"$*"'; do for k in drawn tile; do
                echo "$t $(hyprctl dispatch "hl.plugin.hyprdrive.debug(\"$k $c\")" 2>&1 | grep -o "$k $c [0-9 -]*")"
            done; done
        done' > "$file"
}
g() { $R gesture "$1" >/dev/null; }
# widths FILE KIND CLASS -> "min max" of that class's sampled widths
widths() { grep " $2 $3 " "$1" | awk 'NR==1{lo=$6;hi=$6} {if($6<lo)lo=$6; if($6>hi)hi=$6} END{print lo+0, hi+0}'; }
# offby FILE CLASS REF -> the largest |w - REF| over the drawn samples
offby() { grep " drawn $2 " "$1" | awk -v R="$3" '{d=$6-R; if(d<0)d=-d; if(d>m)m=d} END{print (NR?m+0:9999)}'; }
# drift FILE CLASS -> the largest |drawn w - tile w| over sample pairs
drift() { grep " $2 " "$1" | awk '$2=="drawn"{d=$6} $2=="tile"&&d!=""{x=$6-d; if(x<0)x=-x; if(x>m)m=x; n++} END{print (n?m+0:9999)}'; }
# own CLASS -> the window's own width in device px (what a closed overview draws)
own() { win "$1" .size[0] | awk -v S="$MSCALE" '{printf "%d", $1*S+0.5}'; }

setup() {
    MSCALE=$1
    ev "hl.monitor({ output = \"HEADLESS-1\", mode = \"1920x1080@60\", position = \"0x0\", scale = $MSCALE, reserved = { top = 26 } })"
    sleep 1
}

# ref TAG ANIM CLASSES... -- the reference: key-opened, probe off; sets REF[class]
declare -A REF
ref() {
    local tag=$1 anim=$2; shift 2
    cfg "{plugin={hyprdrive={anim_scale=$anim}}}"
    dbg probe_draw off >/dev/null
    $R overview >/dev/null; sleep 1.5
    sample 300 "$GOUT/$tag.ref" "$@"
    $R overview >/dev/null; sleep 1.2
    local c; for c in "$@"; do REF[$c]=$(widths "$GOUT/$tag.ref" drawn "$c" | awk '{print $2}'); done
}

# run_case TAG ANIM PROBE CLASSES...
run_case() {
    local tag=$1 anim=$2 probe=$3; shift 3
    local c bad
    ref "$tag" "$anim" "$@"
    dbg probe_draw "$probe" >/dev/null

    $R overview >/dev/null; sleep 1.5
    sample 300 "$GOUT/$tag.key" "$@"
    $R overview >/dev/null; sleep 1.2
    bad=""; for c in "$@"; do local d; d=$(offby "$GOUT/$tag.key" "$c" "${REF[$c]}"); [ "$d" -le 1 ] || bad="$bad $c:${d}px"; done
    check "$tag: key-opened, every tile at the reference (<= 1 px)" "$(is "$bad" "")" "off:$bad (ref ${REF[*]})"

    g 'swipe begin 3; swipe update 3 0 -10; wait 16; swipe update 3 0 -10; wait 16; swipe update 3 0 -10; wait 16; swipe update 3 0 -10'
    sleep 0.6; sample 400 "$GOUT/$tag.h20" "$@"
    bad=""
    for c in "$@"; do
        if [ "$anim" = 0 ]; then
            local o; o=$(own "$c"); local d; d=$(offby "$GOUT/$tag.h20" "$c" "$o"); [ "$d" -le 2 ] || bad="$bad $c:${d}px-from-own-$o"
        else
            local d; d=$(drift "$GOUT/$tag.h20" "$c"); [ "$d" -le 2 ] || bad="$bad $c:drawn-tile-${d}px"
        fi
    done
    check "$tag: held at 20%, $( [ "$anim" = 0 ] && echo "the desktop (snapped): each window at its own size" || echo "drawn where the layout says")" "$(is "$bad" "")" "$bad"

    g 'swipe update 3 0 -20; wait 16; swipe update 3 0 -20; wait 16; swipe update 3 0 -20; wait 16; swipe update 3 0 -20'
    sleep 0.6; sample 600 "$GOUT/$tag.h60" "$@"
    bad=""
    for c in "$@"; do
        local d
        if [ "$anim" = 0 ]; then d=$(offby "$GOUT/$tag.h60" "$c" "${REF[$c]}"); [ "$d" -le 1 ] || bad="$bad $c:$(widths "$GOUT/$tag.h60" drawn "$c")-vs-${REF[$c]}"
        else d=$(drift "$GOUT/$tag.h60" "$c"); [ "$d" -le 2 ] || bad="$bad $c:drawn-tile-${d}px"; fi
    done
    check "$tag: held at 60%, $( [ "$anim" = 0 ] && echo "open (snapped): every tile at the reference" || echo "drawn where the layout says")" "$(is "$bad" "")" "$bad"

    g 'swipe update 3 0 -40; wait 16; swipe update 3 0 -40; wait 16; swipe end'
    sleep 1.5; sample 300 "$GOUT/$tag.rel" "$@"
    bad=""; for c in "$@"; do local d; d=$(offby "$GOUT/$tag.rel" "$c" "${REF[$c]}"); [ "$d" -le 1 ] || bad="$bad $c:$(widths "$GOUT/$tag.rel" drawn "$c")-vs-${REF[$c]}"; done
    check "$tag: released, settled at the reference (<= 1 px)" "$(is "$bad" "")" "$bad"
    $R overview >/dev/null; sleep 1.2

    g 'swipe 3 up 300'; sleep 1.5; sample 300 "$GOUT/$tag.full" "$@"
    bad=""; for c in "$@"; do local d; d=$(offby "$GOUT/$tag.full" "$c" "${REF[$c]}"); [ "$d" -le 1 ] || bad="$bad $c:$(widths "$GOUT/$tag.full" drawn "$c")-vs-${REF[$c]}"; done
    check "$tag: a full swipe settles at the reference (<= 1 px)" "$(is "$bad" "")" "$bad"
    $R overview >/dev/null; sleep 1.2
    dbg probe_draw off >/dev/null
}

rig_start
# The synthetic gesture is the native tree's (docs/DEBUG.md `gesture`); a
# build without it (the Omarchy flavour) cannot be driven here.
dbg "gesture wait 1" | grep -q "expected:" && skip "this build has no synthetic gesture (hyprdrive:debug gesture)"
setup 1.5
cfg '{general={gaps_in=5,gaps_out=10,border_size=2,layout="dwindle"}}'
cfg '{plugin={hyprdrive={scale=0.82,layout="horizontal",workspace_gap=40}}}'
ev 'hl.plugin.hyprdrive.gesture({ fingers = 3, direction = "up" })'
sleep 0.5
go 1; kw a1
sleep 1
$R ptr move $CORNER >/dev/null

run_case off1 0 self a1
run_case on1 1 self a1
kw a2; kw a3; sleep 1
run_case off3 0 self a1 a2 a3
run_case on3 1 self a1 a2 a3
run_case plain3 0 off a1 a2 a3
setup 1.0
run_case off3s1 0 self a1 a2 a3

gate_end
