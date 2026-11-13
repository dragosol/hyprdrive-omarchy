#!/usr/bin/env bash
# trash_dialog_plate -- Nautilus's "Empty Trash?" from the dock: its plate and its border are one box, at scale 1.6, 1.5 and 1.
# Protects (owner, 2026-11-13, XPS at scale 1.6: the dialog showed "two
# mismatched rounded shapes", the plate in the top-left of a bordered frame
# ~20 px bigger, blur in the gap): a fixed-size xdg window tiles at the size
# it draws. The dialog (an AdwAlertDialog, min == max) maps with GTK's 20 px
# shadow, and OwnSizeTile::fixedSize read Hyprland's layoutMinSize -- the min
# size PLUS the geometry offset -- so it tiled 20 px too big.
# Real path: a file in the rig user's trash, Nautilus on the rig's bus, the
# dock's Trash icon right-clicked (rig pointer), its Empty Trash row clicked;
# Nautilus asks. The dialog is then closed (Cancel's way, a close request): the
# rig user's trash is never emptied, and checked to still hold the file.
# Asserts, Mosaic (the placement enforcer tiles the dialog), border 2 px
# rgb(ff2000), at each scale:
#   1. the client's box is the dialog's own size (the size it maps at floating)
#   2. pixels: the plate (the dialog's white body) sits inside the red ring by
#      the border on all four sides (-1..+2 px), so the two are one shape
#   3. the trash still holds the file
# Control (--control --control-so <a build of the commit that added this
# gate, the parent of the fix>): 1 and 2 fail at every scale (320x270 against
# 300x250; right and bottom insets 35/33/22 px at 1.6/1.5/1), with motion on
# and off. Recorded 2026-11-13: control FAILS as it must.
source "$(dirname "$0")/lib.sh"
gate_begin trd
control_build "a build of the parent of the fix: the dialog tiles at layoutMinSize"
command -v nautilus >/dev/null || skip "no nautilus"
TRASH=/home/hyprtest/.local/share/Trash
SU bash -c "mkdir -p $TRASH/files $TRASH/info && echo x > $TRASH/files/hd-gate-trd.txt &&
    printf '[Trash Info]\nPath=/home/hyprtest/hd-gate-trd.txt\nDeletionDate=2026-11-13T11:00:00\n' > $TRASH/info/hd-gate-trd.txt.trashinfo"
rig_start
BUS=$(SU bash -c "tr '\0' '\n' < /proc/$(SU cat "$ST/hypr.pid" 2>/dev/null)/environ" 2>/dev/null | sed -n 's/^DBUS_SESSION_BUS_ADDRESS=//p')
if [ -z "$BUS" ]; then # hypr.pid is the dbus-run-session wrapper: its child has it too
    for p in $(SU pgrep -P "$(SU cat "$ST/hypr.pid")"); do
        BUS=$(SU bash -c "tr '\0' '\n' < /proc/$p/environ" 2>/dev/null | sed -n 's/^DBUS_SESSION_BUS_ADDRESS=//p'); [ -n "$BUS" ] && break
    done
fi
[ -n "$BUS" ] || skip "no session bus address for the rig's Hyprland"
cfg '{ general = { layout = "mosaic", border_size = 2, col = { active_border = "rgb(ff2000)", inactive_border = "rgb(ff2000)" } } }'
cfg '{ decoration = { shadow = { enabled = false } } }'
cfg '{plugin={hyprdrive={dock={autohide=false}}}}'   # the Trash in the report from the start
S=1
dm() { dbg dockmenu; }
P() { python3 -c "print(round($1*$S), round($2*$S))"; }   # the rig pointer's space: output pixels
centre() { set -- $(echo "$1" | tr ',x' '  '); echo "$(( $1 + $3 / 2 )) $(( $2 + $4 / 2 ))"; }
dlg() { J clients | jq -r '.[] | select(.title == "Empty Trash?") | "\(.address) \(.at[0]) \(.at[1]) \(.size[0]) \(.size[1]) \(.floating)"' | head -1; }
close_dlg() {
    local a; a=$(dlg | cut -d' ' -f1); [ -n "$a" ] && close_win "$a"
    wait_for 5 '[ -z "$(dlg)" ]'
}
# open_dlg: Nautilus up on the rig's bus (it leaves ~10 s after its last window),
# the Trash icon right-clicked, Empty Trash clicked
open_dlg() {
    $R win env DBUS_SESSION_BUS_ADDRESS="$BUS" XDG_SESSION_TYPE=wayland nautilus --gapplication-service >/dev/null
    wait_for 5 "SU env DBUS_SESSION_BUS_ADDRESS='$BUS' busctl --user status org.gnome.Nautilus >/dev/null 2>&1" || return 1
    local t; t=$(dm | sed -n 's|^ location trash:/// \([-0-9]*,[-0-9]* [0-9]*x[0-9]*\).*|\1|p' | head -1)
    [ -n "$t" ] || return 1
    $R ptr move $(P $(centre "$t")) >/dev/null; sleep 0.4; $R ptr click right >/dev/null; sleep 0.8
    local row; row=$(dm | grep 'kind 7 "Empty Trash"' | grep -o '[-0-9]*,[-0-9]* [0-9]*x[0-9]*' | head -1)
    [ -n "$row" ] || return 1
    $R ptr move $(P $(centre "$row")) >/dev/null; sleep 0.3; $R ptr click left >/dev/null
    wait_for 5 '[ -n "$(dlg)" ]' || return 1
    $R ptr move 20 20 >/dev/null; sleep 1.5
}
measure() { # measure PNG X Y W H (logical) -> "plateL plateT plateR plateB ringL ringT ringR ringB" in output px
    python3 -P - "$@" "$S" <<'PY'
import sys
import numpy as np
from PIL import Image
f, x, y, w, h, s = sys.argv[1], *map(float, sys.argv[2:7])
a = np.asarray(Image.open(f).convert("RGB")).astype(int)
pad = 40
X0, Y0 = max(0, int(x * s) - pad), max(0, int(y * s) - pad)
c = a[Y0:int((y + h) * s) + pad, X0:int((x + w) * s) + pad]
ring = (c[..., 0] > 200) & (c[..., 1] < 90) & (c[..., 2] < 90)
plate = (c.min(axis=2) > 230)
def box(m):
    ys, xs = np.nonzero(m)
    return (xs.min() + X0, ys.min() + Y0, xs.max() + X0, ys.max() + Y0) if len(xs) else None
p, r = box(plate), box(ring)
print(*(p or (-1,) * 4), *(r or (-1,) * 4))
PY
}
for SC in 1.6 1.5 1; do
    ev "hl.monitor({ output = \"HEADLESS-1\", mode = \"2880x1800@60\", position = \"0x0\", scale = $SC })"; S=$SC; sleep 1.5
    if ! open_dlg; then check "$SC: the dock opens Nautilus's Empty Trash? dialog" 0 "$(dm | tr '\n' ' ')"; continue; fi
    read -r A X Y W H F <<< "$(dlg)"
    shot "$GOUT/dlg-$SC.png"
    read -r PL PT PR PB QL QT QR QB <<< "$(measure "$GOUT/dlg-$SC.png" "$X" "$Y" "$W" "$H")"
    B=$(python3 -c "print(round(2*$S))")
    INS="$((PL - QL)) $((PT - QT)) $((QR - PR)) $((QB - PB))"
    ok=1; for d in $INS; do [ "$d" -ge $((B - 1)) ] && [ "$d" -le $((B + 2)) ] || ok=0; done
    check "$SC: plate inside the ring by the border on every side (L T R B)" "$ok" "insets $INS px, border $B px; plate $PL,$PT-$PR,$PB ring $QL,$QT-$QR,$QB; client ${W}x$H f$F"
    echo "      $SC: insets L T R B $INS px (border $B px), client ${W}x$H at $X,$Y, floating $F"
    PW=$(python3 -c "print(round(($PR - $PL + 1) / $S))"); PH=$(python3 -c "print(round(($PB - $PT + 1) / $S))")
    check "$SC: the client box is the size the dialog draws" "$( [ $((W - PW)) -le 1 ] && [ $((PW - W)) -le 1 ] && [ $((H - PH)) -le 1 ] && [ $((PH - H)) -le 1 ] && echo 1)" "client ${W}x$H, plate ${PW}x$PH"
    close_dlg
done
check "the rig user's trash still holds its file" "$(SU test -e $TRASH/files/hd-gate-trd.txt && echo 1)" "gone"
SU rm -f $TRASH/files/hd-gate-trd.txt $TRASH/info/hd-gate-trd.txt.trashinfo
gate_end
