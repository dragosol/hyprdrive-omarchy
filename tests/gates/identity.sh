#!/usr/bin/env bash
# identity -- AppIdentity names Wine/Proton/Steam windows by app, not by class.
# Protects: the owner, 2026-11-11: "too many times I've launched proton and
# steam apps and they've launched without a proper icon and app name".
# Fixture HOME: tests/fixtures/identity/{steam-native,umu} with /home/user
# rewritten to it. Clients: tests/rig/identity/fake_wine.c (WM_CLASS,
# _NET_WM_PID, _NET_WM_ICON, argv0 = C:\...\X.exe, env as given).
# Asserts:
#   A. steam_app_2357570 (no env at all): `debug identity` key steam-app-2357570,
#      the shortcut's name, dock group hd:steam-app-2357570, a dock icon for it
#   B. two umu games, both class steam_app_default, one prefix: two dock items
#      (hd:hyprdrive-wine-alpha.exe, hd:hyprdrive-wine-beta.exe), each resolved
#      by the worker from /proc (argv0 + WINEPREFIX)
#   C. a native window (kitty, class kk): group kk, unchanged
#   D. 30 windows mapping with a slow source (HYPRDRIVE_IDENTITY_DELAY_MS=400,
#      jobs queue): no compositor frame gap over 50 ms while they map, then the
#      plugin unloads with jobs still queued: no core, the compositor answers
# Control: --so a4d6892 (hyprdrive-native's installed release): B shows ONE
# dock item steam_app_default; A has no `debug identity`.
source "$(dirname "$0")/lib.sh"
gate_begin id
control_build "a4d6892 (the release before AppIdentity went live)"
cc -O2 -o "$GOUT/fake_wine" "$ROOT/tests/rig/identity/fake_wine.c" $(pkg-config --cflags --libs xcb) || skip "cannot build fake_wine (xcb headers?)"
# Not under /tmp: a Wine program under /tmp/ is an installer's temp copy to
# AppIdentity (Resolvers::isWineSystemPath) and is never an app.
H=/var/tmp/hdg-$HL_NAME-home; SU rm -rf "$H" 2>/dev/null; rm -rf "$H"; mkdir -p "$H"
trap '_gate_cleanup; SU rm -rf "$H" 2>/dev/null; rm -rf "$H" 2>/dev/null' EXIT
for f in steam-native umu; do cp -a "$ROOT/tests/fixtures/identity/$f/home/user/." "$H/"; done
grep -rlI /home/user "$H" | while read -r f; do sed -i "s|/home/user|$H|g" "$f"; done
chmod -R a+rwX "$H"; chmod 755 "$GOUT" "$GOUT/fake_wine"
xdisplay() { hlog 4000 | grep -o 'DISPLAY: :[0-9]*' | tail -1 | grep -o ':[0-9]*$'; }
idline() { dbg identity | grep -F "class=$1 " | sed -n "${2:-1}p"; }
icons() { dbg dockmenu | tee "$GOUT/dockmenu.txt" | grep -o '^ icon [^ ]*' | awk '{print $2}' | sort -u | tr '\n' ' '; }
fake() { local cls=$1 a0=$2 title=$3; shift 3; $R win env DISPLAY="$D" "$@" "$GOUT/fake_wine" "$cls" "$a0" "$title" 60 >/dev/null; }

rig_start --home "$H"; go 1
cfg '{plugin={hyprdrive={dock={autohide=false}}}}'
wait_for 6 '[ -n "$(xdisplay)" ]' || { $R win env DISPLAY=:99 true >/dev/null 2>&1; wait_for 6 '[ -n "$(xdisplay)" ]'; }
D=$(xdisplay); [ -n "$D" ] || skip "XWayland never announced a display"
kw kk
fake steam_app_2357570 /usr/bin/fake_wine Overwatch
PFX=$H/Games/umu/umu-default
fake steam_app_default 'C:\Games\Alpha\Alpha.exe' Alpha WINEPREFIX="$PFX" GAMEID=umu-default PROTONPATH=GE-Proton
fake steam_app_default 'C:\Games\Beta\Beta.exe' Beta WINEPREFIX="$PFX" GAMEID=umu-default PROTONPATH=GE-Proton
wait_for 8 '[ "$(J clients | jq "[.[] | select(.class == \"steam_app_default\")] | length")" = 2 ]'
wait_for 6 '[ "$(dbg identity | grep -c "state=resolved")" -ge 4 ]'
dbg identity > "$GOUT/identity.txt" 2>&1
A=$(idline steam_app_2357570)
check "A: the Steam class is the Steam game (key)" "$(echo "$A" | grep -q 'key=steam-app-2357570 ' && echo 1 || echo 0)" "${A:0:300}"
check "A: named by its shortcut, not by its class" "$(echo "$A" | grep -q 'name="Overwatch' && echo 1 || echo 0)" "$(echo "$A" | grep -o 'name="[^"]*"')"
check "A: grouped as hd:steam-app-2357570" "$(echo "$A" | grep -q 'group=hd:steam-app-2357570 ' && echo 1 || echo 0)" "$(echo "$A" | grep -o 'group=[^ ]*')"
I=$(icons); echo "$I" > "$GOUT/icons.txt"
check "A: a dock icon for the game" "$(echo " $I" | grep -q ' hd:steam-app-2357570 ' && echo 1 || echo 0)" "icons: $I"
check "B: two umu games, two dock items" "$(echo " $I" | grep -q ' hd:hyprdrive-wine-alpha.exe ' && echo " $I" | grep -q ' hd:hyprdrive-wine-beta.exe ' && echo 1 || echo 0)" "icons: $I"
check "B: no shared steam_app_default item" "$(echo " $I" | grep -q ' steam_app_default ' && echo 0 || echo 1)" "icons: $I"
C=$(idline kk)
check "C: a native window keeps its class as its group" "$(echo "$C" | grep -q 'group=kk ' && echo 1 || echo 0)" "$(echo "$C" | grep -o 'group=[^ ]*')"
rig_stop

rig_start --home "$H" --env HYPRDRIVE_IDENTITY_DELAY_MS=400; go 1
wait_for 6 '[ -n "$(xdisplay)" ]' || { $R win env DISPLAY=:99 true >/dev/null 2>&1; wait_for 6 '[ -n "$(xdisplay)" ]'; }
D=$(xdisplay); [ -n "$D" ] || skip "XWayland never announced a display"
kw kk; sleep 1
# The main loop while 30 windows map and their jobs queue behind a 400 ms
# source: an IPC round trip each 50 ms. A compositor waiting on the worker
# would answer one of them >= 400 ms late. (Not `debug frames`: an idle
# headless output draws no frames to measure a gap between.)
for i in $(seq 1 30); do fake "steam_app_$((100 + i))" "C:\\G\\g$i.exe" "g$i" WINEPREFIX="$PFX"; done &
worst=0; end=$(( $(date +%s) + 6 ))
while [ "$(date +%s)" -lt $end ]; do
    t0=$(date +%s%N); hq version >/dev/null 2>&1; t1=$(date +%s%N)
    ms=$(( (t1 - t0) / 1000000 )); [ "$ms" -gt "$worst" ] && worst=$ms; sleep 0.05
done
wait
wait_for 10 '[ "$(J clients | jq "[.[] | select(.class | startswith(\"steam_app_1\"))] | length")" -ge 30 ]'
check "D: the compositor answers within 150 ms while 30 windows map (worker at 400 ms a job)" "$(le "$worst" 150)" "worst IPC round trip ${worst} ms"
Q=$(dbg identity | head -1 | grep -o 'queued [0-9]*' | grep -o '[0-9]*')
check "D: jobs still queued at the unload (the slow source)" "$(ge "${Q:-0}" 1)" "queued ${Q:-?}"
hq plugin unload "$LOADED" >/dev/null 2>&1; sleep 2.5
check "D: the compositor answers after an unload with jobs queued" "$(hq version >/dev/null 2>&1 && echo 1 || echo 0)" ""
rig_stop
check "D: no core" "$(is "$CORES" 0)" "$(cat "$GOUT/stop.log")"
gate_end
