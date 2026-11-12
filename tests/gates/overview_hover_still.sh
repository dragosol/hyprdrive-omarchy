#!/usr/bin/env bash
# overview_hover_still -- hovering an overview tile only zooms it: no tile's centre moves, with Hyprdrive motion off and on.
# Protects: the fix for #9 ("hover a tile on the left and it moves right, over
# its neighbour; the last one just zooms", Hyprdrive motion off) and the same
# cause as #7's shake (motion on) -- GridSolve.hpp: the grid lays windows out in
# creation order and, inside the frame that draws, one solve answers every box
# query of that frame. The tile pass draws a window with its size moved onto
# its tile; a box query for the hovered tile from INSIDE that draw solved the
# grid with the drawn window shrunk, and every tile before it slid right by
# half the shrink (116 px at 1920x1080 scale 1.5: the reporter's number).
# The trigger on the reporter's machine is a mid-draw query the rig does not
# make, so this gate makes it: `hyprdrive:debug probe_draw on` asks for the
# hovered tile's box from inside every other tile's draw (docs/DEBUG.md).
# Rig: 1920x1080 at scale 1.5 with a 26 px top bar, gaps 5/10, border 2 (the
# reporter's Omarchy), the horizontal overview at 0.82; desktop 1 one window,
# desktop 3 the row under test (2 and 4 empty, as in his video).
# Asserts, in each tile order (created, recent, position), for motion off
# (anim_scale 0) and on (1), with 2 and with 3 windows in the row: the
# pointer rests on each tile's centre in turn for 1.2 s while
# `hyprdrive:debug drawn` (where the tile pass DREW each tile) is sampled
# ~60 Hz; every sample of every tile has its centre within 1 px of where it
# was drawn at rest, and the hovered tile is drawn larger than at rest (the
# hover zoom still happens). Then probe off, motion off: the same, once.
# Control: --control --control-so <the release 1.9.57 (23bceff) with only the
# probe instrument added> -- the tile before the last jumps +116 px with motion
# off (and the last -116 px with this probe, which asks during every other
# tile's draw); with motion on it wobbles while it settles. Recorded
# 2026-11-12: see the commit.
source "$(dirname "$0")/lib.sh"
gate_begin hvs
control_build "1.9.57 (23bceff) + the probe_draw instrument"

CORNER="960 1060"
# sample MS FILE CLASSES... -- "<ms> drawn <class> x y w h" lines, one hyprctl loop as hyprtest
sample() {
    local ms=$1 file=$2; shift 2
    SU env XDG_RUNTIME_DIR="$RT" HYPRLAND_INSTANCE_SIGNATURE="$SIG" sh -c '
        end=$(( $(date +%s%3N) + '"$ms"' ))
        while [ $(date +%s%3N) -lt $end ]; do t=$(date +%s%3N)
            for c in '"$*"'; do
                echo "$t $(hyprctl dispatch "hl.plugin.hyprdrive.debug(\"drawn $c\")" 2>&1 | grep -o "drawn $c [0-9 -]*")"
            done
        done' > "$file"
}
# rest CLASS FILE -> "cx cy w" of the class's last rest sample (x2, so half px stay integers)
rest_of() { grep " drawn $1 " "$2" | tail -1 | awk '{print 2*$4+$6, 2*$5+$7, $6}'; }
# worst FILE CLASS RESTCX2 RESTCY2 -> the largest |centre shift| in half px
worst() { grep " drawn $2 " "$1" | awk -v X="$3" -v Y="$4" '{dx=2*$4+$6-X; dy=2*$5+$7-Y; if(dx<0)dx=-dx; if(dy<0)dy=-dy; d=dx>dy?dx:dy; if(d>m)m=d} END{print m+0}'; }
maxw() { grep " drawn $2 " "$1" | awk '{if($6>m)m=$6} END{print m+0}'; }

rig_start
ev 'hl.monitor({ output = "HEADLESS-1", mode = "1920x1080@60", position = "0x0", scale = 1.5, reserved = { top = 26 } })'
cfg '{general={gaps_in=5,gaps_out=10,border_size=2,layout="dwindle"}}'
cfg '{plugin={hyprdrive={scale=0.82,layout="horizontal",workspace_gap=40}}}'
sleep 0.5
go 1; kw n1
go 3; kw a1; kw a2
sleep 1

# run_case TAG ANIM PROBE CLASSES...
run_case() {
    local tag=$1 anim=$2 probe=$3; shift 3
    local classes="$*" c r
    cfg "{plugin={hyprdrive={anim_scale=$anim}}}"
    $R ptr move $CORNER >/dev/null; sleep 0.3
    $R overview >/dev/null; sleep 1.5
    dbg probe_draw "$probe" >/dev/null
    sample 600 "$GOUT/$tag.rest" $classes
    declare -A RX RY RW
    for c in $classes; do
        r=$(rest_of "$c" "$GOUT/$tag.rest")
        [ -n "$r" ] || { check "$tag: $c is drawn at rest" 0 "no drawn sample"; continue; }
        set -- $r; RX[$c]=$1; RY[$c]=$2; RW[$c]=$3
    done
    for h in $classes; do
        [ -n "${RX[$h]:-}" ] || continue
        $R ptr move $(( RX[$h] / 2 )) $(( RY[$h] / 2 )) >/dev/null   # half px -> device px
        sleep 0.4
        sample 1200 "$GOUT/$tag.$h" $classes
        local bad="" n
        n=$(grep -c " drawn " "$GOUT/$tag.$h")
        for c in $classes; do
            local w; w=$(worst "$GOUT/$tag.$h" "$c" "${RX[$c]}" "${RY[$c]}")
            [ "$w" -le 2 ] || bad="$bad $c:$(( w / 2 ))px"
        done
        check "$tag: hovering $h moves no tile's centre (<= 1 px, $n samples)" "$(is "$bad" "")" "moved:$bad"
        check "$tag: ...and $h is drawn zoomed" "$(ge "$(maxw "$GOUT/$tag.$h" "$h")" $(( RW[$h] + 4 )))" "max w $(maxw "$GOUT/$tag.$h" "$h") vs rest ${RW[$h]}"
        $R ptr move $CORNER >/dev/null; sleep 0.6
    done
    dbg probe_draw off >/dev/null
    $R overview >/dev/null; sleep 1.2
}

# Every tile order (plugin:hyprdrive:overview_tile_order): in `recent` the
# hover's focus must not reorder the grid either.
for mode in created recent position; do
    cfg "{plugin={hyprdrive={overview_tile_order=\"$mode\"}}}"
    if [ -n "$(addr_of a3)" ]; then close_win "$(addr_of a3)"; sleep 1; fi
    run_case $mode.off2 0 on a1 a2
    run_case $mode.on2 1 on a1 a2
    kw a3; sleep 1
    run_case $mode.off3 0 on a1 a2 a3
    run_case $mode.on3 1 on a1 a2 a3
    run_case $mode.plain3 0 off a1 a2 a3
done

gate_end
