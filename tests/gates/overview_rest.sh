#!/usr/bin/env bash
# overview_rest -- with the pointer still, no overview tile moves while a window's size flickers; a real resize still reflows.
# Protects: the fix for two public reports (2026-11, "opened apps just shake
# weirdly" / "windows ... almost vibrating, random twitchy motion where they move
# to the side and get slightly bigger before going back") -- GridReflowModel.hpp:
# one overview frame owns the grid's eases, a box query only reads them, and the
# grid lays a window out with its settled size goal. Designed behaviours: tiles
# follow real windows; a reflow is animated (mission_control_reflow_ms, house
# decel curve).
# The trigger on the reporters' machines is unknown; this gate forces one: an X11
# client (tests/rig/xstate/size_flicker.c, floating) that resizes itself 15 px
# smaller and back 30 ms later, every 100-600 ms, beside a kitty.
# Asserts (pointer still in a corner, overview open, `hyprdrive:debug tile` sampled
# ~60 Hz from one hyprctl loop):
#   1. 2 s before the flickering starts: each tile has ONE box (the sampler and
#      the rig are quiet)
#   2. 6 s of flickering (the client logs >= 8 flickers): each tile still has ONE
#      box, the same as at rest; the grid trace saw the flicker (`grid size
#      pending` / `rejected` lines) and never retargeted at rest
#   3. a real resize of the flickering window by -300x-200 (relative): both tiles
#      move (the neighbour reflows too), the motion is over within 400 ms of its
#      first change (one reflow: 300 ms, its sub-pixel tail is invisible), and
#      the tiles end still
# Control: --control --control-so a4d6892 (the build before the fix; copy it,
# never rebuild that tree): 2 fails -- the tiles twitch by 1-2 px at rest (3-4
# distinct boxes per tile in 6 s) while the rest box (1) is the fix's, to the
# pixel -- and 3 fails because they keep twitching. Recorded 2026-11-11: control
# FAILS as it must. Without the flicker (measured by hand on the rig) the same
# resize moved the tiles for 765 ms before the fix -- the grid followed the size
# ANIMATION, then eased -- and for 224 ms on it: one reflow toward the size goal.
source "$(dirname "$0")/lib.sh"
gate_begin ovr
control_build "a4d6892 (the build before the fix)"
FIX=$GOUT/size_flicker
cc -O2 -o "$FIX" "$ROOT/tests/rig/xstate/size_flicker.c" $(pkg-config --cflags --libs xcb) || skip "cannot build size_flicker (xcb headers?)"
chmod 755 "$GOUT" "$FIX"
xdisplay() { hlog 4000 | grep -o 'DISPLAY: :[0-9]*' | tail -1 | grep -o ':[0-9]*$'; }
client_log() { SU sh -c "ls -t $ST/client.*.log" | head -1; }
# sample MS FILE -- "ms <k1 box> | <size_flicker box>" lines, one hyprctl loop as hyprtest
sample() {
    SU env XDG_RUNTIME_DIR="$RT" HYPRLAND_INSTANCE_SIGNATURE="$SIG" sh -c '
        end=$(( $(date +%s%3N) + '"$1"' ))
        while [ $(date +%s%3N) -lt $end ]; do
            a=$(hyprctl dispatch "hl.plugin.hyprdrive.debug(\"tile k1\")" 2>&1 | grep -o "tile k1 [0-9 -]*")
            b=$(hyprctl dispatch "hl.plugin.hyprdrive.debug(\"tile size_flicker\")" 2>&1 | grep -o "tile size_flicker [0-9 -]*")
            echo "$(date +%s%3N) $a| $b"
        done' > "$2"
}
boxes() { cut -d' ' -f2- "$1" | cut -d'|' -f"$2" | sort -u | grep -c .; }
lastbox() { tail -1 "$1" | cut -d' ' -f2- | cut -d'|' -f"$2"; }

rig_start; trace_on
go 1; kw k1 900 600
ev 'hl.window_rule({ match = { class = "size_flicker" }, float = true })'
wait_for 6 '[ -n "$(xdisplay)" ]' || { $R win env DISPLAY=:99 true >/dev/null 2>&1; wait_for 6 '[ -n "$(xdisplay)" ]'; }
D=$(xdisplay); [ -n "$D" ] || skip "XWayland never announced a display"
# 40 s, 15 px, held 30 ms, every 100-600 ms, the first one 7 s after it maps
$R win env DISPLAY="$D" "$FIX" 40 15 30 100 600 7 >/dev/null
T0=$(date +%s%3N)
wait_for 6 '[ -n "$(addr_of size_flicker)" ]' || skip "the X11 fixture never mapped"
check "the fixture floats (Hyprland only honours an X11 client's own resize then)" "$(is "$(win size_flicker .floating)" true)" "$(win size_flicker '{floating,size}')"
$R ptr move 1900 1060 >/dev/null
$R overview >/dev/null; sleep 2.5

# 1. at rest, before the first flicker
sample 2000 "$GOUT/rest.samp"
n=$(wc -l < "$GOUT/rest.samp")
check "1: sampled at rest (>= 60 samples in 2 s)" "$(ge "$n" 60)" "$n samples"
check "1: at rest, one box per tile" "$(is "$(boxes "$GOUT/rest.samp" 1) $(boxes "$GOUT/rest.samp" 2)" "1 1")" "$(cut -d' ' -f2- "$GOUT/rest.samp" | sort | uniq -c | head -4 | tr '\n' ';')"
REST=$(lastbox "$GOUT/rest.samp" 1)\|$(lastbox "$GOUT/rest.samp" 2)

# 2. while its size flickers
wait_for 6 "[ \$(( \$(date +%s%3N) - $T0 )) -ge 7500 ]"
RT0=$(trace | grep -c 'grid retarget')
sample 6000 "$GOUT/flicker.samp"
LOG=$(client_log); nf=$(SU cat "$LOG" 2>/dev/null | grep -c '^flicker')
check "2: the client flickered (>= 8 times)" "$(ge "$nf" 8)" "$nf flickers"
n=$(wc -l < "$GOUT/flicker.samp")
check "2: sampled while flickering (>= 180 samples in 6 s)" "$(ge "$n" 180)" "$n samples"
check "2: k1's tile never moves" "$(is "$(boxes "$GOUT/flicker.samp" 1)" 1)" "$(cut -d' ' -f2- "$GOUT/flicker.samp" | cut -d'|' -f1 | sort | uniq -c | sort -rn | head -4 | tr '\n' ';')"
check "2: size_flicker's tile never moves" "$(is "$(boxes "$GOUT/flicker.samp" 2)" 1)" "$(cut -d' ' -f2- "$GOUT/flicker.samp" | cut -d'|' -f2 | sort | uniq -c | sort -rn | head -4 | tr '\n' ';')"
check "2: ...and both are where they were at rest" "$(is "$(lastbox "$GOUT/flicker.samp" 1)|$(lastbox "$GOUT/flicker.samp" 2)" "$REST")" "now $(lastbox "$GOUT/flicker.samp" 1)|$(lastbox "$GOUT/flicker.samp" 2), rest $REST"
if [ "$CONTROL" = 1 ]; then
    subskip "2: the grid trace" "the build before the fix has no grid trace"
else
    seen=$(trace | grep -c 'grid size \(pending\|rejected\) size_flicker')
    rt=$(( $(trace | grep -c 'grid retarget') - RT0 ))
    check "2: the grid heard the flicker (size pending/rejected lines)" "$(ge "$seen" 8)" "$seen lines"
    check "2: ...and never retargeted a tile at rest" "$(is "$rt" 0)" "$rt retargets: $(trace | grep 'grid retarget' | head -2 | tr '\n' ';')"
fi

# 3. a real resize still reflows, as one reflow
sample 2500 "$GOUT/resize.samp" &
SP=$!; _bg_pids="${_bg_pids:-} $SP"
sleep 0.8; TR=$(date +%s%3N)
ev "hl.dispatch(hl.dsp.window.resize({ window = [[address:$(addr_of size_flicker)]], x = -300, y = -200, relative = true }))"
wait "$SP"
SPAN=$(awk -v t0="$TR" '{ ms = $1; $1 = ""; if (NR > 1 && $0 != prev && ms > t0) { if (!first) first = ms; last = ms } prev = $0 }
    END { if (first) printf "%d %d", first - t0, last - first; else print "none" }' "$GOUT/resize.samp")
set -- $SPAN
echo "info  rest $REST; flickers $nf; resize: first change +${1:-?} ms, motion ${2:-?} ms"
check "3: both tiles moved" "$([ "$(lastbox "$GOUT/resize.samp" 1)" != "${REST%%|*}" ] && [ "$(lastbox "$GOUT/resize.samp" 2)" != "${REST#*|}" ] && echo 1)" "k1 $(lastbox "$GOUT/resize.samp" 1) size_flicker $(lastbox "$GOUT/resize.samp" 2), rest $REST"
check "3: one reflow: over within 400 ms of its first change" "$([ "$SPAN" != none ] && [ "$2" -le 400 ] && echo 1)" "first change +${1:-?} ms after the resize, motion lasted ${2:-?} ms"
END=$(awk -v t0="$TR" '$1 > t0 + 1700' "$GOUT/resize.samp" | cut -d' ' -f2- | sort -u | grep -c .)
check "3: the tiles end still" "$(is "$END" 1)" "$END distinct boxes in the last 800 ms"
gate_end
