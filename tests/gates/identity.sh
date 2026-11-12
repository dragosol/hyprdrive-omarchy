#!/usr/bin/env bash
# identity -- AppIdentity names Wine/Proton/Steam windows by app, not by class.
# Protects: the owner, 2026-11-11: "too many times I've launched proton and
# steam apps and they've launched without a proper icon and app name".
# Fixture HOME: tests/fixtures/identity/{steam-native,umu} with /home/user
# rewritten to it. Clients: tests/rig/identity/fake_wine.c (WM_CLASS,
# _NET_WM_PID, _NET_WM_ICON, argv0 = C:\...\X.exe, env as given).
# Asserts:
#   A. steam_app_2357570 (no env at all): `debug identity` key steam-app-2357570,
#      the newest shortcut's name (Overwatch®, not the stale "Overwatch 2"), dock group hd:steam-app-2357570, a dock icon for it
#   B. two umu games, both class steam_app_default, one prefix: two dock items
#      (hd:hyprdrive-wine-alpha.exe, hd:hyprdrive-wine-beta.exe), each resolved
#      by the worker from /proc (argv0 + WINEPREFIX)
#   C. a native window (kitty, class kk): group kk, unchanged
#   D. 30 windows mapping with a slow source (HYPRDRIVE_IDENTITY_DELAY_MS=400,
#      jobs queue): no compositor frame gap over 50 ms while they map, then the
#      plugin unloads with jobs still queued: no core, the compositor answers.
#      The worst IPC round trip is printed as a MEASURE line.
#   E. Battle.net (Lutris: pga.db built from the fixture's pga.db.tsv) and the
#      Overwatch it starts (same env: WINEPREFIX, GAME_NAME, LUTRIS_GAME_UUID):
#      two dock items, Battle.net's the Lutris game
#   F. launcher.exe in two prefixes (tests/fixtures/identity/wine-menus): two
#      dock items, Alpha's its winemenubuilder entry
#   G. Proton's explorer.exe in a Steam game's session (SteamGameId): grouped
#      under the game, no dock item of its own
# Control: a build before AppIdentity (native a4d6892, Omarchy 8210965): B
# shows ONE dock item steam_app_default; A has no `debug identity` (and on
# Omarchy its dock item is the bare class steam_app_2357570, no icon).
source "$(dirname "$0")/lib.sh"
gate_begin id
control_build "a4d6892 / Omarchy 8210965 (the builds before AppIdentity went live)"
cc -O2 -o "$GOUT/fake_wine" "$ROOT/tests/rig/identity/fake_wine.c" $(pkg-config --cflags --libs xcb) || skip "cannot build fake_wine (xcb headers?)"
# Not under /tmp: a Wine program under /tmp/ is an installer's temp copy to
# AppIdentity (Resolvers::isWineSystemPath) and is never an app.
H=/var/tmp/hdg-$HL_NAME-home; SU rm -rf "$H" 2>/dev/null; rm -rf "$H"; mkdir -p "$H"
trap '_gate_cleanup; SU rm -rf "$H" 2>/dev/null; rm -rf "$H" 2>/dev/null' EXIT
for f in steam-native umu lutris wine-menus; do cp -a "$ROOT/tests/fixtures/identity/$f/home/user/." "$H/"; done
grep -rlI /home/user "$H" | while read -r f; do sed -i "s|/home/user|$H|g" "$f"; done
# Steam left "Overwatch 2" behind when the game was renamed: the newest
# shortcut speaks for the app id. A checkout gives both files one mtime.
touch -d @1700000000 "$H/.local/share/applications/Overwatch 2.desktop"; touch -d @1700000600 "$H/.local/share/applications/Overwatch.desktop"
# Lutris's pga.db, from the fixture's table (the host tests read the .tsv).
python3 -I - "$H/.local/share/lutris/pga.db.tsv" "$H/.local/share/lutris/pga.db" <<'PY' || skip "cannot build pga.db (python3 sqlite3)"
import sqlite3, sys
rows = [l.rstrip("\n").split("\t") for l in open(sys.argv[1]) if l.strip()]
db = sqlite3.connect(sys.argv[2])
db.execute("CREATE TABLE games (" + ", ".join(rows[0]) + ")")
db.executemany("INSERT INTO games VALUES (" + ",".join("?" * len(rows[0])) + ")", [[None if v == "\\N" else v for v in r] for r in rows[1:]])
db.commit()
PY
chmod -R a+rwX "$H"; chmod 755 "$GOUT" "$GOUT/fake_wine"
xdisplay() { hlog 4000 | grep -o 'DISPLAY: :[0-9]*' | tail -1 | grep -o ':[0-9]*$'; }
idline() { dbg identity | grep -F "class=$1 " | sed -n "${2:-1}p"; }
icons() { dbg dockmenu | tee "$GOUT/dockmenu.txt" | grep -o '^ icon [^ ]*' | awk '{print $2}' | sort -u | tr '\n' ' '; }
grp() { echo "$1" | grep -o ' group=.* state=' | sed 's/^ group=//; s/ state=$//'; }
hasicon() { grep -qF " icon $1 " "$GOUT/dockmenu.txt"; }
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
check "A: named by its newest shortcut, not by its class" "$(echo "$A" | grep -q 'name="Overwatch®"' && echo 1 || echo 0)" "$(echo "$A" | grep -o 'name="[^"]*"')"
check "A: grouped as hd:steam-app-2357570" "$(echo "$A" | grep -q 'group=hd:steam-app-2357570 ' && echo 1 || echo 0)" "$(echo "$A" | grep -o 'group=[^ ]*')"
I=$(icons); echo "$I" > "$GOUT/icons.txt"
check "A: a dock icon for the game" "$(echo " $I" | grep -q ' hd:steam-app-2357570 ' && echo 1 || echo 0)" "icons: $I"
check "B: two umu games, two dock items" "$(echo " $I" | grep -q ' hd:hyprdrive-wine-alpha.exe ' && echo " $I" | grep -q ' hd:hyprdrive-wine-beta.exe ' && echo 1 || echo 0)" "icons: $I"
check "B: no shared steam_app_default item" "$(echo " $I" | grep -q ' steam_app_default ' && echo 0 || echo 1)" "icons: $I"
C=$(idline kk)
check "C: a native window keeps its class as its group" "$(echo "$C" | grep -q 'group=kk ' && echo 1 || echo 0)" "$(echo "$C" | grep -o 'group=[^ ]*')"

BN=$H/Games/battlenet; LENV=(WINEPREFIX="$BN" GAME_NAME=Battle.net GAME_DIRECTORY="$BN" LUTRIS_GAME_UUID=6f1d2c3b-0000-4000-8000-000000000001)
fake battle.net.exe 'C:\Program Files (x86)\Battle.net\Battle.net.exe' Battle.net "${LENV[@]}"
fake overwatch.exe 'C:\Program Files (x86)\Overwatch\_retail_\Overwatch.exe' Overwatch "${LENV[@]}"
fake launcher.exe 'C:\Alpha\launcher.exe' Alpha WINEPREFIX="$H/Games/alpha"
fake launcher.exe 'C:\Beta\launcher.exe' Beta WINEPREFIX="$H/Games/beta"
fake steam_app_1826330 'C:\Games\Gamma\Gamma.exe' Gamma SteamGameId=1826330 SteamAppId=1826330 WINEPREFIX="$PFX"
fake explorer.exe 'C:\windows\explorer.exe' 'Wine Desktop' SteamGameId=1826330 SteamAppId=1826330 WINEPREFIX="$PFX"
wait_for 8 '[ "$(J clients | jq "[.[] | select(.class == \"launcher.exe\" or .class == \"explorer.exe\" or .class == \"battle.net.exe\" or .class == \"overwatch.exe\" or .class == \"steam_app_1826330\")] | length")" = 6 ]'
wait_for 6 '[ "$(dbg identity | grep -c "state=resolved")" -ge 10 ]'
dbg identity > "$GOUT/identity2.txt" 2>&1
I=$(icons); echo "$I" > "$GOUT/icons2.txt"
GB=$(grp "$(idline battle.net.exe)"); GO=$(grp "$(idline overwatch.exe)")
check "E: Battle.net is its Lutris game" "$(is "$GB" hd:net.lutris.battlenet-12)" "group $GB; $(idline battle.net.exe | cut -c1-300)"
check "E: the game it starts is its own dock item" "$([ -n "$GO" ] && [ "$GO" != "$GB" ] && hasicon "$GB" && hasicon "$GO" && echo 1 || echo 0)" "battle.net $GB, overwatch $GO; icons: $I"
GA=$(grp "$(idline launcher.exe 1)"); GZ=$(grp "$(idline launcher.exe 2)")
check "F: launcher.exe in two prefixes, two dock items" "$([ -n "$GA" ] && [ -n "$GZ" ] && [ "$GA" != "$GZ" ] && hasicon "$GA" && hasicon "$GZ" && echo 1 || echo 0)" "groups '$GA' '$GZ'; icons: $I"
check "F: Alpha's is its winemenubuilder entry" "$(dbg identity | grep -F 'class=launcher.exe ' | grep -qF 'group=hd:wine-Programs-Alpha-Alpha Launcher state=' && echo 1 || echo 0)" "groups '$GA' '$GZ'"
GG=$(grp "$(idline steam_app_1826330)"); GE=$(grp "$(idline explorer.exe)")
check "G: explorer.exe joins the game of its session" "$([ -n "$GG" ] && [ "$GE" = "$GG" ] && echo 1 || echo 0)" "game $GG, explorer $GE; $(idline explorer.exe | cut -c1-300)"
check "G: no dock item of its own" "$(hasicon explorer.exe && echo 0 || echo 1)" "icons: $I"
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
echo "MEASURE worst IPC round trip while 30 windows map behind a 400 ms source: ${worst} ms"
check "D: the compositor answers within 150 ms while 30 windows map (worker at 400 ms a job)" "$(le "$worst" 150)" "worst IPC round trip ${worst} ms"
Q=$(dbg identity | head -1 | grep -o 'queued [0-9]*' | grep -o '[0-9]*')
check "D: jobs still queued at the unload (the slow source)" "$(ge "${Q:-0}" 1)" "queued ${Q:-?}"
hq plugin unload "$LOADED" >/dev/null 2>&1; sleep 2.5
check "D: the compositor answers after an unload with jobs queued" "$(hq version >/dev/null 2>&1 && echo 1 || echo 0)" ""
rig_stop
check "D: no core" "$(is "$CORES" 0)" "$(cat "$GOUT/stop.log")"
gate_end
