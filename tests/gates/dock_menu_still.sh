#!/usr/bin/env bash
# dock_menu_still -- motion off, a click with the pointer held still draws what it opened: the dock menu, Get Info, a preview's window menu.
# Protects (owner, 2026-11-13, on hardware): with anim_scale = 0, right-clicking
# a dock icon with the pointer still showed no menu until something else
# repainted. The click asked for no frame (main.cpp's button hook); with motion
# on, the tooltip fade or the hover lean happened to be pumping frames anyway.
# The menu's rows are laid out by the frame that draws it, so a row with a real
# size in `hyprdrive:debug dockmenu` -- read BEFORE any screenshot (a grim
# forces a frame on the rig) and with no pointer motion after the click -- is a
# frame that was drawn on its own.
# Asserts, anim_scale = 0, then Hyprland's animations off (anim_scale 1):
#   1. pointer parked on appa's icon, then on the Trash, a right-click: within
#      200 ms the menu's rows have real sizes
#   2. pointer parked on the Trash menu's Get Info row, a left-click (the rows swap in
#      place): within 200 ms the rows are the info view's, with real sizes
#   3. pointer parked on a preview in appa's hover pane, a right-click: within
#      200 ms the window menu's rows have real sizes
#   4. the screenshot then taken shows the menu (pixels differ from the shot
#      taken before the click, over the menu's box)
# Control (--control --control-so <a build of 2326491, 1.9.60>): 1 fails on
# the Trash, the owner's case -- rows 0x0 at +200 ms, motion off both ways.
# The app's menu, Get Info and the preview's menu pass on the control too: an
# app with windows has its hover pane's live previews pumping frames, which
# drew them by chance; they are here so the fix cannot lose them. Recorded
# 2026-11-13: control FAILS as it must.
source "$(dirname "$0")/lib.sh"
gate_begin dms
control_build "a build of 2326491 (1.9.60), whose click asks for no frame"
rig_start
cfg '{plugin={hyprdrive={dock={autohide=false}}}}'
kw appa 700 450; kw appa 600 400; sleep 1
dm() { dbg dockmenu; }
centre() { set -- $(echo "$1" | tr ',x' '  '); echo "$(( $1 + $3 / 2 )) $(( $2 + $4 / 2 ))"; }
rows() { dm | grep '^ row '; }
# every row has a real size: "N" rows, none of them 0x0
sized() { local r; r=$(rows); [ -n "$r" ] && ! echo "$r" | grep -q ' 0x0' && echo 1; }
menubox() { dm | sed -n 's/^ menu [a-z]* \([-0-9]*,[-0-9]* [0-9]*x[0-9]*\).*/\1/p'; }
park() { $R ptr move $1 $2 >/dev/null; sleep 1.5; }   # arrive, and let every frame it asked for pass
# closed by a press that misses it (Escape reaches it only with the keyboard on the dock)
esc() { $R ptr move 700 300 >/dev/null; sleep 0.3; $R ptr click left >/dev/null; sleep 0.8; }
shot_menu_differs() { # BEFORE AFTER: pixels changed over the menu's box
    local b; b=$(menubox); [ -n "$b" ] || return 1
    set -- "$1" "$2" $(echo "$b" | tr ',x' '  ')
    python3 -P - "$@" <<'PY'
import sys
import numpy as np
from PIL import Image
a = np.asarray(Image.open(sys.argv[1]).convert("RGB")).astype(int)
b = np.asarray(Image.open(sys.argv[2]).convert("RGB")).astype(int)
x, y, w, h = map(int, sys.argv[3:7])
d = (np.abs(a - b).max(axis=2) > 24)[y:y + h, x:x + w]
print(1 if d.mean() > 0.03 else 0)   # text and edges: the plate is dark on a dark window
PY
}
box_of() { dm | grep -o "$1 [-0-9]*,[-0-9]* [0-9]*x[0-9]*" | head -1 | awk '{print $(NF-1), $NF}'; }
run_steps() {
    local ICON TRASH GI CELL
    ICON=$(box_of 'icon appa'); TRASH=$(box_of 'location trash:///')
    [ -n "$ICON" ] && [ -n "$TRASH" ] || { check "$MODE: dockmenu reports appa's icon and the Trash" 0 "$(dm | tr '\n' ' ')"; return; }
    # 1. the app menu
    park $(centre "$ICON")
    $R ptr click right >/dev/null; sleep 0.2
    check "$MODE, 1: right-click on an app, pointer still: its menu is drawn within 200 ms" "$(sized)" "$(rows | head -3 | tr '\n' ' ')"
    esc
    # 1b. the Trash's menu (no hover pane under it, so the shot shows it alone)
    park $(centre "$TRASH"); shot "$GOUT/before.png"; sleep 1
    $R ptr click right >/dev/null; sleep 0.2
    check "$MODE, 1: right-click on the Trash, pointer still: its menu is drawn within 200 ms" "$(sized)" "$(rows | head -3 | tr '\n' ' ')"
    $R shot "$GOUT/after.png" >/dev/null 2>&1
    check "$MODE, 4: the screenshot shows the menu" "$(shot_menu_differs "$GOUT/before.png" "$GOUT/after.png")" "menu $(menubox)"
    # 2. Get Info, the rows swapped in place
    GI=$(rows | grep '"Get Info"' | grep -o '[-0-9]*,[-0-9]* [0-9]*x[0-9]*' | head -1)
    if [ -n "$GI" ] && ! echo "$GI" | grep -q ' 0x0'; then
        park $(centre "$GI")
        $R ptr click left >/dev/null; sleep 0.2
        check "$MODE, 2: Get Info, pointer still: the info view is drawn within 200 ms" \
            "$( [ "$(sized)" = 1 ] && ! rows | grep -q '"Get Info"' && echo 1)" "$(rows | head -3 | tr '\n' ' ')"
    else
        check "$MODE, 2: the Trash's menu has a Get Info row with a size" 0 "$(rows | tr '\n' ' ')"
    fi
    esc
    # 3. a preview's own menu: the pane opens on hover, then a cell, still
    park $(centre "$ICON")
    CELL=$(dm | grep '^ cell ' | grep -o '[-0-9]*,[-0-9]* [0-9]*x[0-9]*' | head -1)
    if [ -n "$CELL" ]; then
        park $(centre "$CELL")
        $R ptr click right >/dev/null; sleep 0.2
        check "$MODE, 3: right-click on a preview, pointer still: its menu is drawn within 200 ms" \
            "$( [ "$(sized)" = 1 ] && dm | grep -q '^ menu window' && echo 1)" "$(dm | grep -E '^ (menu|row)' | head -3 | tr '\n' ' ')"
    else
        check "$MODE, 3: appa's hover pane shows its previews" 0 "$(dm | tr '\n' ' ')"
    fi
    esc
}
MODE="anim_scale 0"; cfg '{plugin={hyprdrive={anim_scale=0}}}'; sleep 0.5; run_steps
MODE="animations off"; cfg '{plugin={hyprdrive={anim_scale=1}}, animations={enabled=false}}'; sleep 0.5; run_steps
gate_end
