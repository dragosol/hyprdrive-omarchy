#!/usr/bin/env bash
# reload_blocks -- `hyprdrive reload` with Omarchy's blocks, Mosaic in the config file and a disabled panel: every fullscreen window stays fullscreen.
# Protects: the owner's report 2026-11-11 (laptop, Omarchy edition 8210965):
# `hyprdrive reload` took three fullscreen windows out of fullscreen, and the
# one on a fullscreen trip "went home". Layout mosaic IN THE CONFIG FILE; one
# monitor (block 2) with desktops 1..n left outside its block by an older
# build; the internal panel disabled at runtime, so every config reload
# enables it for a moment (Omarchy moves desktops onto it and back).
# Cause: the config reload every `plugin unload` schedules runs REFRESH_ALL ->
# updateWorkspaceLayouts, which swaps every desktop configured "mosaic"
# (missing) from dwindle to a NEW dwindle, whose predecessor's removeTarget
# takes the window out of fullscreen -- after the unload's carry, before the
# load's. Fix: FullscreenCarry's SHandover (Mosaic.cpp handOver/restoreGap).
# Steps: HEADLESS-2 = the panel (enabled by the config file, disabled by
# `eval`); HEADLESS-1 owns block 0 while the windows are made (f1 alone on 1,
# t2 + k2 on 2, k3 on 3, the XWayland wine_state on 4, k5 on 5); f1 and
# wine_state fullscreen alone (settled), t2 fullscreen with company (a trip to
# 3, a debt: k3.. move along); then the laptop's blocks (panel block 0,
# HEADLESS-1 block 2) and the CLI's steps exactly (ui/bin/hyprdrive
# cmd_power("reload")): plugin list, 0.3 s, plugin unload, plugin list, plugin
# load, plugin list, `hyprctl reload`; the panel is disabled again 0.6 s later.
# Asserts:
#   A. after the CLI: f1, t2 and wine_state fullscreen (2), f1 and wine_state
#      alone on their desktops, t2 alone and not with k2 (the trip kept), the
#      trace has `reloaded: 1 trips` and no `home to` / `paying back`; the
#      blocks did their part (changeworkspaceid seen, a monitor came and went)
#   B. then a plain `hyprctl reload` (panel on, off again): no sample of the
#      three leaves fullscreen, the trip is kept
#   C. t2 out of fullscreen: home, on k2's desktop (whatever its id became)
# Control: --so a4d6892 (/home/dragos/Projects/hyprdrive-native/hyprdrive.so,
# the build before the fix): A fails (fullscreen 0 on f1 and t2, t2 home with
# k2). Recorded 2026-11-11: FAILS as it must.
source "$(dirname "$0")/lib.sh"
gate_begin rlb
control_build "a4d6892, e.g. /home/dragos/Projects/hyprdrive-native/hyprdrive.so"

RIG=$ROOT/tests/rig/headless
seed() { SU bash -c "mkdir -p '$ST/state/omarchy' && chmod 755 '$ST' '$ST/state' '$ST/state/omarchy' && printf '$1' > '$ST/state/omarchy/monitor-workspaces'"; }
cc -O2 -o "$GOUT/wine_state" "$ROOT/tests/rig/xstate/wine_state.c" $(pkg-config --cflags --libs xcb) || skip "cannot build wine_state (xcb headers?)"
chmod 755 "$GOUT" "$GOUT/wine_state"

seed '0\tHEADLESS-1\tHEADLESS-1\n'
rig_start --config "$RIG/blocks.lua" --env "HD_BLOCKS_LUA=$RIG/omarchy-blocks.lua" --env "HD_PLUGIN_LUA=$ROOT/config/hyprdrive/00-plugin.lua" \
    --env "HYPRDRIVE_SO=$SO" --env "XDG_STATE_HOME=$ST/state" --env "HD_LAYOUT=mosaic"
trace_on
panel_off() { ev 'hl.monitor({ output = "HEADLESS-2", disabled = true })'; }
check "mosaic from the config file" "$(is "$(hq getoption general:layout -j | jq -r .str)" mosaic)" "$(hq getoption general:layout -j | jq -r .str)"
$R mon2 >/dev/null; sleep 0.8; panel_off; sleep 0.8
check "the panel is off" "$(is "$(J monitors | jq -r '[.[].name] | join(" ")')" HEADLESS-1)" "$(J monitors | jq -r '[.[].name] | join(" ")')"
ev 'hl.dispatch(hl.dsp.focus({ monitor = "HEADLESS-1" }))'
xdisplay() { hlog 4000 | grep -o 'DISPLAY: :[0-9]*' | tail -1 | grep -o ':[0-9]*$'; }
go 1; kw f1; go 2; kw t2; kw k2; go 3; kw k3
wait_for 6 '[ -n "$(xdisplay)" ]' || { $R win env DISPLAY=:99 true >/dev/null 2>&1; wait_for 6 '[ -n "$(xdisplay)" ]'; }
go 4; $R win env DISPLAY="$(xdisplay)" "$GOUT/wine_state" fs 120 >/dev/null
wait_for 6 '[ -n "$(addr_of wine_state.exe)" ]' || echo "warn: wine_state never mapped"
go 5; kw k5; sleep 0.5
F1=$(addr_of f1); T2=$(addr_of t2); WN=$(addr_of wine_state.exe)
focus "$F1"; fs "$F1" set; sleep 1
wait_for 4 '[ "$(win wine_state.exe .fullscreen)" = 2 ]'
focus "$T2"; fs "$T2" set; sleep 1.5; settle 4 ids
TRIP=$(win t2 .workspace.id)
fsd() { J clients | jq -r '[.[] | select(.class == "f1" or .class == "t2" or .class == "wine_state.exe") | "\(.class)=\(.fullscreen)"] | sort | join(" ")'; }
alone() { local w; w=$(win "$1" .workspace.id); J clients | jq "[.[] | select(.workspace.id == $w)] | length"; }
check "set up: three fullscreen, t2 on a trip (not on 2)" "$(is "$(fsd) $([ "$TRIP" != 2 ] && echo trip)" 'f1=2 t2=2 wine_state.exe=2 trip')" "$(fsd), t2 on $TRIP; $(ids)"

# the laptop: block 0's panel, block 2 this monitor -- 1..5 are now outside
seed '0\t\tHEADLESS-2\n2\tHEADLESS-1\tHEADLESS-1\n'; sleep 0.5
: > "$GOUT/events.log"; SU socat -u "UNIX-CONNECT:$RT/hypr/$SIG/.socket2.sock" - > "$GOUT/events.log" 2>/dev/null & _bg_pids="${_bg_pids:-} $!"; sleep 0.3
T0=$(trace | wc -l)
sampler_start s 'echo "$(date +%s.%N | cut -c7-14) $(fsd) | $(ids)"'
# ui/bin/hyprdrive cmd_power("reload"), step by step
hq plugin list >/dev/null; sleep 0.3
hq plugin unload "$LOADED" >/dev/null; hq plugin list >/dev/null
hq plugin load "$LOADED" >/dev/null; hq plugin list >/dev/null
hq reload >/dev/null
sleep 0.6; panel_off
sleep 2.5; settle 4 'ids; fsd'
sampler_stop s >/dev/null
TR=$(trace | tail -n +$((T0 + 1)))
echo "      after the CLI: $(fsd); f1 on $(win f1 .workspace.id), t2 on $(win t2 .workspace.id), k2 on $(win k2 .workspace.id); $(ids)"
echo "      changeworkspaceid: $(grep -c '^changeworkspaceid>>' "$GOUT/events.log"), monitoradded $(grep -c '^monitoradded>>' "$GOUT/events.log"), monitorremoved $(grep -c '^monitorremoved>>' "$GOUT/events.log")"
check "A: the blocks renumbered and the panel came and went" "$([ "$(grep -c '^changeworkspaceid>>' "$GOUT/events.log")" -ge 1 ] && grep -q '^monitoradded>>HEADLESS-2' "$GOUT/events.log" && grep -q '^monitorremoved>>HEADLESS-2' "$GOUT/events.log" && echo 1)" "$(grep -E '^(changeworkspaceid|monitoradded|monitorremoved)>>' "$GOUT/events.log" | tr '\n' ' ')"
check "A: all three still fullscreen" "$(is "$(fsd)" 'f1=2 t2=2 wine_state.exe=2')" "$(fsd)"
check "A: f1 and wine_state alone on their desktops" "$(is "$(alone f1) $(alone wine_state.exe)" '1 1')" "$(alone f1) $(alone wine_state.exe); $(ids)"
check "A: t2's trip kept (alone, not with k2)" "$([ "$(alone t2)" = 1 ] && [ "$(win t2 .workspace.id)" != "$(win k2 .workspace.id)" ] && echo 1)" "t2 on $(win t2 .workspace.id) k2 on $(win k2 .workspace.id)"
check "A: the trip handed over, not sent home" "$(echo "$TR" | grep -q 'reloaded: 1 trips' && ! echo "$TR" | grep -qE 'home to|paying back' && echo 1)" "$(echo "$TR" | grep -E 'reloaded:|home to|paying back|lost while' | tr '\n' ';' | cut -c1-300)"
echo "      fullscreen samples: $(awk '{ sub(/^[^ ]* /, ""); sub(/ \|.*/, ""); print }' "$GOUT/s.samp" | sort | uniq -c | awk '{printf "%sx[%s %s %s] ", $1, $2, $3, $4}')"

# B. a plain config reload with blocks on: nothing leaves fullscreen
T1=$(trace | wc -l)
sampler_start b 'fsd'
hq reload >/dev/null; sleep 0.6; panel_off; sleep 2; settle 4 'ids; fsd'
sampler_stop b >/dev/null
check "B: hyprctl reload: no sample leaves fullscreen" "$(is "$(sort -u "$GOUT/b.samp" | tr '\n' ';')" 'f1=2 t2=2 wine_state.exe=2;')" "$(sort "$GOUT/b.samp" | uniq -c | tr '\n' ';')"
check "B: the trip kept" "$([ "$(alone t2)" = 1 ] && ! trace | tail -n +$((T1 + 1)) | grep -qE 'home to|paying back' && echo 1)" "t2 on $(win t2 .workspace.id); $(ids)"
# C. the trip still works: out of fullscreen, t2 goes home to k2
fs "$T2" unset; sleep 1.5; settle 4 ids
check "C: t2 out of fullscreen: home with k2" "$(is "$(win t2 .workspace.id)" "$(win k2 .workspace.id)")" "t2 on $(win t2 .workspace.id) k2 on $(win k2 .workspace.id); $(ids)"
echo "      after C: $(ids); $(refused | tail -2 | tr '\n' ';' | cut -c1-250)"
gate_end
