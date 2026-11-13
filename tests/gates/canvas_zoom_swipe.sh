#!/usr/bin/env bash
# canvas_zoom_swipe -- a desktop swipe away from a canvas zoomed out by the fingers lands on the next desktop; a cancelled one leaves every window where it was.
# Protects: the fix for "if I zoom out with 3 finger swipe down, then I do a
# partial swipe left ... the zoomed out windows of the desktop I was swiping
# away from are bumped to the left and off screen, but I haven't actually left
# the desktop" (owner, 2026-11-13, XPS). The swipe had gone past half, so
# Hyprland switched -- and its switch moves the pointer (simulateMouseMovement)
# before it announces the new desktop. The fingers' zoom (MosaicGestures.cpp
# superMove) still took the canvas just left for the live one, picked the
# zoomed window drawn under the pointer and focused it: the monitor went back
# to that desktop mid-switch, and the swipe's end then sent it a screen away.
# Active, a screen off: an empty screen with the overview naming it current.
# Rig: 1920x1080, mosaic + infinite canvas, 3-finger down = canvas_zoom, the
# 3-finger horizontal = workspace; desktop 1 two windows wider than the screen
# (a canvas), desktop 2 one small window (no canvas); the pointer on a zoomed
# window. Asserts, with Hyprdrive motion on (anim_scale 1) and off (0):
#   - cancelled (30% of the swipe): still desktop 1, every window drawn where
#     it was zoomed out (pos and size, +-1 px), the screen as before;
#   - past half (70%, slow): desktop 2 shown, its window focused, the screen
#     not empty;
#   - swiped back: desktop 1 shown, its windows on screen.
# THE SAME SWIPE TOWARD A DESKTOP NOT MADE YET crashed the compositor (rig,
# 2026-11-13; the XPS's unexplained SIGSEGV in COverviewWorkspaceGesture::end
# of 2026-11-10 15:43). Hyprland makes the new desktop and switches to it
# holding it only as the monitor's active one; the zoom's hover switched back
# mid-switch, the new desktop was freed, and Hyprland's swipe end looked it
# up by id and wrote its slide through the null. So, with desktop 2 gone
# (workspace_swipe_create_new, Hyprland's default) and motion on and off:
#   - past half: a new desktop 2 is shown, the compositor still up.
# Control (--control --control-so <70e25cd, the build before the fix>): past
# half, desktop 1 is still shown, focused on its own window, the screen empty;
# toward a desktop not made yet, the compositor dies (SIGSEGV).
source "$(dirname "$0")/lib.sh"
gate_begin czs
control_build "70e25cd (before the canvas zoom swipe fix)"

# drawn CLASS -> "x,y wxh" where the window is drawn now (the zoom draws off the layout)
drawn() { dbg windows | awk -v C="$1" '$1==C || $2==C {for(i=1;i<=NF;i++){if($i=="pos")p=$(i+1); if($i=="size")s=$(i+1)}; split(p,a,"/"); split(s,b,"/"); print a[1], b[1]; exit}'; }
both() { echo "$(drawn za) | $(drawn zb)"; }
g() { $R gesture "$1" >/dev/null; }
# lit FILE -> per mille of pixels brighter than the empty (black) desktop: a window's dark fill counts
lit() { python3 -P -c "
import sys; from PIL import Image; import numpy as np
a=np.asarray(Image.open(sys.argv[1]).convert('L')); print(int((a>12).mean()*1000))" "$1"; }
# near A B -> 1 when two drawn lists agree within 1 px
near() { python3 -P -c "
import re,sys
a=[int(x) for x in re.findall(r'-?\d+',sys.argv[1])]; b=[int(x) for x in re.findall(r'-?\d+',sys.argv[2])]
print(1 if a and len(a)==len(b) and all(abs(x-y)<=1 for x,y in zip(a,b)) else 0)" "$1" "$2"; }

rig_start
dbg "gesture wait 1" | grep -q "expected:" && skip "this build has no synthetic gesture (hyprdrive:debug gesture)"
cfg '{general={layout="mosaic"}}'
cfg '{plugin={hyprdrive={mosaic={infinite_canvas=true}}}}'
ev 'hl.plugin.hyprdrive.gesture({ fingers = 3, direction = "down", action = "canvas_zoom" })'
ev 'hl.plugin.hyprdrive.gesture({ fingers = 3, direction = "horizontal", action = "workspace" })'
go 2; kw zd 700 500
go 1; kw za 1100 900; kw zb 1100 900
sleep 1.5

run_case() {
    local tag=$1 anim=$2
    cfg "{plugin={hyprdrive={anim_scale=$anim}}}"
    go 1; settle 3 geom
    $R ptr move 700 540 >/dev/null; sleep 0.3
    g 'swipe 3 down 500'; sleep 1.2
    local Z0; Z0=$(both); shot "$GOUT/$tag.z0.png"
    # 30% of workspace_swipe_distance (300), slowly: Hyprland calls it back
    g 'swipe 3 left 90 12 12'; sleep 1.5
    shot "$GOUT/$tag.z1.png"
    local P; P=$(pixdiff "$GOUT/$tag.z0.png" "$GOUT/$tag.z1.png")
    check "$tag: cancelled: still desktop 1" "$(is "$(shown)" 1)" "shown $(shown)"
    check "$tag: cancelled: every window drawn where it was zoomed out" "$(near "$Z0" "$(both)")" "$Z0 -> $(both)"
    check "$tag: cancelled: the screen as before (largest change < 4000 px)" "$([ "${P#* }" -lt 4000 ] 2>/dev/null && echo 1)" "changed $P"
    # 70%, slowly: Hyprland switches
    g 'swipe 3 left 210 20 12'; sleep 1.5
    shot "$GOUT/$tag.z2.png"
    local L; L=$(lit "$GOUT/$tag.z2.png")
    check "$tag: past half: desktop 2 shown, its window focused" "$(is "$(shown) $(J activewindow | jq -r .class)" "2 zd")" "shown $(shown), focus $(J activewindow | jq -r .class)"
    check "$tag: past half: the screen is not empty (lit $L/1000)" "$(ge "$L" 20)" "lit $L/1000"
    # and back
    g 'swipe 3 right 210 20 12'; sleep 1.5
    shot "$GOUT/$tag.z3.png"; L=$(lit "$GOUT/$tag.z3.png")
    check "$tag: swiped back: desktop 1 shown, its windows on screen (lit $L/1000)" "$(is "$(shown) $(ge "$L" 100)" "1 1")" "shown $(shown), lit $L/1000"
    # leave no zoom behind for the next case
    $R ptr click >/dev/null; sleep 1
}
run_case on 1
run_case off 0

# new_case TAG ANIM -- past half toward a desktop that does not exist yet
new_case() {
    local tag=$1 anim=$2
    cfg "{plugin={hyprdrive={anim_scale=$anim}}}"
    go 1; settle 3 geom
    $R ptr move 700 540 >/dev/null; sleep 0.3
    g 'swipe 3 down 500'; sleep 1.2
    g 'swipe 3 left 210 20 12'; sleep 1.5
    local S; S=$(shown)
    check "$tag: toward a desktop not made yet: the compositor is up, desktop 2 shown, empty" \
        "$(is "$S $(J workspaces 2>/dev/null | jq -r '.[] | select(.id == 2) | .windows')" "2 0")" "shown '${S:-none: compositor gone}'"
    [ -n "$S" ] || return 1
    go 1; sleep 0.8
    $R ptr click >/dev/null; sleep 1
}
close_win "$(addr_of zd)"; wait_for 3 "[ -z \"\$(addr_of zd)\" ]"; go 1; sleep 1
new_case new-on 1 && new_case new-off 0
gate_end
