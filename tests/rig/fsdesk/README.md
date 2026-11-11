# fsdesk rig checks: desktop numbers under fullscreen trips

These are for a rig tester. They check the `fix/desktop-ids` commits (DesktopPlan,
FullscreenBook, the reload handover, heal) on the headless rig. The host unit
tests (`tests/unit/desktop_plan_test.cpp`, `tests/unit/fullscreen_book_test.cpp`)
already cover the decisions. Only a real Hyprland shows whether the plugin
takes them at the right moments: an empty desktop is dropped when nothing holds
it, the strip glides, a monitor unplugs, and the plugin reloads.

## Setup (as dragos on fedora)

```sh
cd ~/Projects/hd-fix-desks
source ~/sdk/hyprland-0.56.2/env.sh && nice make -s -j8
export HL_NAME=fsd
R=tests/rig/headless/hl.sh
$R start --so "$PWD/hyprdrive.so"          # mode next is the default
RT=/tmp/hl-fsd/rt
sudo -n -u hyprtest touch $RT/hyprdrive-mosaic.on   # turns the fsdesk trace on
TRACE=$RT/hyprdrive-mosaic.log; REFUSED=$RT/hyprdrive-desktops.log
```

Helpers used below:

```sh
ids()   { $R h -j workspaces | jq -c '[.[] | select(.id > 0) | {id, windows}] | sort_by(.id)'; }
uniq_ok() { $R h -j workspaces | jq -e '[.[] | select(.id > 0) | .id] | (length == (unique | length)) and all(. >= 1)' >/dev/null; }
pop()   { $R h -j workspaces | jq -c '[.[] | select(.id > 0 and .windows > 0) | .id] | sort'; }
addr()  { $R h -j clients | jq -r ".[] | select(.workspace.id == $1) | .address" | sed -n "${2:-1}p"; }
fs()    { $R eval "hl.dispatch(hl.dsp.window.fullscreen({ window = [[address:$1]], action = \"$2\" }))" >/dev/null; }
go()    { $R eval "hl.dispatch(hl.dsp.focus({ workspace = \"$1\" }))" >/dev/null; }
sample(){ for _ in $(seq "$1"); do uniq_ok || { echo "DUPLICATE: $(ids)"; return 1; }; sleep 0.05; done; }
```

Starting layout for every case: two kitty windows on desktop 1, one each on 2
and 3. Build it with `go N; $R win kitty` and wait until `pop` prints `[1,2,3]`.
Record `START=$(pop)`.

**At every sample of every case:** `uniq_ok` holds, meaning no two desktops share
a number and none is below 1. Run `sample` in the background while a case runs
(`sample 2000 &`). `REFUSED` must not appear. Any line in it is a renumber the
plugin refused, and you report it with the trace.

## 1. 50 fullscreen on/off cycles on desktop 1

```sh
go 1; A=$(addr 1 1)
for i in $(seq 50); do fs $A set; sleep 0.5; fs $A unset; sleep 0.5; done   # ~1 Hz
sleep 2; pop
for i in $(seq 50); do fs $A set; sleep 0.03; fs $A unset; sleep 0.03; done # key-repeat speed
sleep 2; pop
```

Pass:
- `pop` equals `START` after each loop.
- In the trace, each on is `fullscreen on 1: to 2 (owed back)`. At rest each off is
  followed by `paying back place 2` within ~1 s. While spamming, the next on takes
  the debt over, and the trace has one `paying back` at the end.
- No `a debt with no place` and no `nothing owed`.

## 2. A window opened beside a fullscreen app, then closed

```sh
go 1; A=$(addr 1 1); fs $A set; sleep 1          # app on 2; old 2,3 now 3,4
$R eval 'hl.dispatch(hl.dsp.exec_cmd("kitty"))'; sleep 1.5
ids; tail -3 $TRACE   # "kitty opened over fullscreen kitty on 2: to 3 (owed back)"
B=$(addr 3 1); $R eval "hl.dispatch(hl.dsp.window.close({ window = [[address:$B]] }))"; sleep 1.5
fs $A unset; sleep 2; pop
```

Pass:
- The new window is visible on 3 at once, and the app stays fullscreen on 2.
- After the close, `paying back place 3 (beside)` appears once desktop 3 is not
  on screen. Switching back to 2 is enough.
- After the off, `pop == START`.

Also run the case once with a different app class opened onto 3 by hand
(`hl.dsp.window.move` of another window there) before closing B. The trace must say
`place 3 is a desktop of its own now: nothing owed`, the designed rule.

## 3. A trip, then the monitor goes, then exit on the other monitor

```sh
$R mon2                                          # HEADLESS-2 at 1920x0
$R eval 'hl.dispatch(hl.dsp.workspace.move({ workspace = "1", monitor = "HEADLESS-2" }))'; sleep 1
go 1; A=$(addr 1 1); fs $A set; sleep 1          # the trip is HEADLESS-2's
$R h output remove HEADLESS-2; sleep 1.5; ids
fs $A unset; sleep 2; ids; pop
$R mon2; sleep 1.5; ids
```

Pass:
- `uniq_ok` holds across the remove and the re-add.
- After the off, the app is back with its sibling. Once nothing owed is on screen,
  `paying back place 2` appears. The debt is no longer dropped with its monitor.
- `pop` equals `START`, ignoring the empty desktop the re-added monitor shows.
- When the monitor comes and goes, no `two desktops shared a number` line appears
  in `REFUSED`. With healthy numbers, heal stays silent.

## 4. Plugin unload/load mid-trip, then exit fullscreen

```sh
go 1; A=$(addr 1 1); fs $A set; sleep 1; ids        # app alone on 2, others 3,4
SO=/tmp/hl-fsd/staged/hyprdrive.so
$R h plugin unload "$SO"; sleep 0.8; $R h plugin load "$SO"; sleep 1.5
tail -2 $TRACE        # "reloaded: 1 trips, 0 debts, 0 settled handed over"
fs $A unset; sleep 2; pop
```

Pass:
- The trace line shows the trip handed over.
- After the off, the app goes home to 1 (`out of fullscreen: home to 1`), desktop 2
  is paid back, and `pop == START`.
- With the old build, the app stayed alone on 2 and 3, 4 never came back. Keep a
  baseline run of 8bb258c for the report.

Variant: unload, close the app while unloaded, then load. The trace says
`0 trips, 1 debts`, and the place is paid back.

## 5. The old "-1" debt (app closed while its desktop is not on screen)

```sh
go 1; A=$(addr 1 1); fs $A set; sleep 1          # app on 2
go 1; sleep 1                                     # look away: 2 is not on screen
$R eval "hl.dispatch(hl.dsp.window.close({ window = [[address:$A]] }))"; sleep 2
grep -c 'paying back place -1' $TRACE             # must be 0
tail -3 $TRACE; ids; pop
```

Pass:
- The trace shows `fullscreen app closed`, then `paying back place 2`. There is
  never `place -1` and never `a debt with no place`.
- `pop` equals `START` minus nothing: desktop 1 keeps its other window.
- On 8bb258c this exact sequence printed `paying back place -1`, and two desktops
  were numbered 1.

Heal check (old damage): with 8bb258c loaded, reproduce case 5 until `ids` shows
two `id: 1`. Then load this build. The emptier one moves to the next free number
above the highest, one `two desktops shared a number` line appears in `REFUSED`,
and no window changes desktop object.

## Report

For each case, report `START`, the final `pop`, the count of `uniq_ok` failures
(must be 0), the relevant trace lines, and the contents of `REFUSED`. Then run
`$R stop` and check that no core dump was reported.

## 6. A display change removes the empty desktops

The owner's report: after an output change his desktops sat at 21, 22, 28, 29, 30
with nothing below. Build that, then change the output's mode or scale.

```sh
# one kitty each on 21, 22, 28, 29, 30, nothing else (move the starting windows there)
for n in 21 22 28 29 30; do go $n; $R win kitty; done
for a in $($R h -j clients | jq -r '.[] | select(.workspace.id < 21) | .address'); do
  $R eval "hl.dispatch(hl.dsp.window.move({ window = [[address:$a]], workspace = \"30\" }))" >/dev/null; done
go 22; sleep 1; pop                                   # [21,22,28,29,30]
B=$($R h -j clients | jq -c '[.[] | {address, ws: .workspace.id}] | sort_by(.address)')
MON=$($R h -j monitors | jq -r '.[0].name')
scale() { $R eval "hl.monitor({ output = \"$MON\", mode = \"preferred\", position = \"auto\", scale = $1 })" >/dev/null; }
scale 1.25; sleep 1.5; pop; ids
$R h -j monitors | jq '.[0].activeWorkspace.id'
tail -3 $REFUSED; grep 'empty desktops removed\|closed by the compaction\|owed back from now on' $TRACE | tail
```

Pass:
- Within ~1.5 s `pop` is `[1,2,3,4,5]`. 21 -> 1, 22 -> 2, 28 -> 3, 29 -> 4, 30 -> 5:
  each window's new desktop number is its old one's rank (compare with `B`). No
  window changes desktop object, and the desktop that was shown (22) is still
  shown, now as 2.
- `REFUSED` has exactly one line for it: `the monitor layout changed: no empty
  desktops between: 21 22 28 29 30 -> 1 2 3 4 5`. A burst of changes (`scale 1.5; sleep 0.3;
  scale 1.25`) still gives one line, ~700 ms after the last one.
- `uniq_ok` holds at every sample.
- `scale 1`: nothing moves (already compact) and no new `REFUSED` line.

Variants:
- **Shown empty desktop.** Go to an empty 25 first (`go 25`), then change the
  scale. The windows' desktops become 1..5, and 25 is still shown, as 6.
- **A fullscreen trip in flight.** Start from 21, 22, 28, 29, 30 with two kittys
  on 21. Fullscreen one of them: it gets 22 and the others move to 23, 29, 30, 31
  (trace `to 22 (owed back)`). Change the scale. Now `pop` is `[1,2,3,4,5,6]`, the
  app is alone on 2, and the trace has no `closed by the compaction` for its
  place. Unfullscreen it: it goes home to 1, place 2 is paid back, and `pop` is
  `[1,2,3,4,5]`.
- **A debt whose place is a hole.** As in case 5, close the trip app while its
  desktop is not on screen, then change the scale within the same second. If the
  debt was still waiting, the trace says `debt at place N: closed by the
  compaction, settled`, and no `paying back place N` follows. `pop` is 1..N with
  no hole.
- **Overview open.** Open the overview, change the scale, wait 2 s: nothing is
  renumbered. Close it: within ~0.1 s the desktops are compacted.
- Disable and re-enable the output, or unplug a headless output
  (`hyprctl output remove`): the same compaction, logged `a monitor went` / `a
  monitor came`.

## 7. A reload on Mosaic keeps every window's fullscreen

The bug: `hyprdrive reload` took every fullscreen window on a Mosaic desktop out
of fullscreen (a running game dropped to windowed), and the fsdesk handover then
sent the windows home and paid their desktops back. The desktops shifted on every
reload. With dwindle, fullscreen survived. Hyprland keeps a tiled window's modes
in the handler owned by the desktop's tiled algorithm. Removing or adding the
`mosaic` algo swaps that object, and the modes go with it. On load,
`CDwindleAlgorithm::removeTarget` also sends a real fullscreen exit. The fix
(`FullscreenCarry.hpp`, `Mosaic::registerLayout` / `unregisterLayout`) writes the
modes into the new handler silently. The trace shows `unload: <app> kept
fullscreen I/C` and `load: ...` for windows on desktops that stayed on their
layout.

Mosaic must be in the config FILE for the whole of this case (as on a real
desktop): every `plugin unload` schedules a config reload, and with the layout
set by `eval` that reload puts the desktops back on dwindle, which hides the
second half of the bug -- while the plugin is out, each config refresh swaps
every desktop configured "mosaic" (missing) to a NEW dwindle, and the old one's
removeTarget sends a fullscreen exit. The load puts those windows back
(`load: <app> on N back to I/C (lost while unloaded)`, FullscreenCarry's
SHandover); gate `reload_blocks` drives it with Omarchy's blocks on.

Smallest repro, which fails on 5338fd3: Mosaic, one kitty fullscreen, then
`$R h plugin unload "$SO"`. `fullscreen` is 0 afterwards. With this build it is
still 2.

Build `wine_state` and find the X display as in `tests/rig/xstate/README.md`
(steps 1 and 2).

```sh
$R eval 'hl.config({ general = { layout = "mosaic" } })' >/dev/null
SO=/tmp/hl-fsd/staged/hyprdrive.so
# Starting layout (above) plus: one kitty on 5, two on 6, wine_state on 7
go 5; $R win kitty; go 6; $R win kitty; $R win kitty
go 7; $R win env DISPLAY=:N /tmp/hl-xstate/wine_state fs 60; sleep 2
LONE=$(addr 5 1); MAXW=$(addr 6 1); A=$(addr 1 1); X=$(addr 7 1)
fs $LONE set                                   # a lone fullscreen window: no trip
$R eval "hl.dispatch(hl.dsp.window.fullscreen({ window = [[address:$MAXW]], mode = \"maximized\", action = \"set\" }))" >/dev/null
go 1; fs $A set; sleep 1                       # a trip: A leaves populated 1 for a desktop of its own
state() { $R h -j clients | jq -c '[.[] | {address, ws: .workspace.id, fullscreen, fullscreenClient}] | sort_by(.address)'; }
lay()   { $R h -j workspaces | jq -c '[.[] | select(.id > 0) | {id, l: .tiledLayout}] | sort_by(.id)'; }
B=$(state); L=$(lay); I=$(ids); T0=$(wc -l < $TRACE)
$R h plugin unload "$SO"; sleep 0.8; state; $R h plugin load "$SO"; sleep 1.5
[ "$(state)" = "$B" ] && echo KEPT || { echo "CHANGED"; diff <(echo "$B" | jq .) <(state | jq .); }
[ "$(ids)" = "$I" ] && echo IDS-SAME; [ "$(lay)" = "$L" ] && echo LAYOUT-SAME
$R h getoption general:layout
tail -n +$((T0 + 1)) $TRACE | grep -E 'home to|paying back|kept fullscreen|reloaded:'
tail -5 /tmp/hl-xs/client.*.log                # wine_state: changes= does not jump at the reload
```

If the workspace JSON has no `tiledLayout` field, read each desktop's layout
another way. One option: `hyprctl layoutmsg` on a window there, which only Mosaic
answers. Another: check that a new kitty on 1 lands Mosaic-style, not as a dwindle
split.

Pass:
- `KEPT`: every window has the same `fullscreen` / `fullscreenClient` (lone and
  trip kitty 2/2, the maximized one 1/1, wine_state 2/2) and the same desktop
  as before. The `state` taken while the plugin is unloaded already shows them
  unchanged (dwindle holds the modes in between).
- `IDS-SAME` and `LAYOUT-SAME`. `general:layout` still reads `mosaic`, and the
  desktops that were Mosaic are Mosaic again after the load.
- The trace after `T0` has `reloaded: 1 trips, ...` and no `home to` or `paying
  back`. `kept fullscreen` lines are expected for unload. Load lines appear
  only where a desktop stayed on its layout.
- The client sees no flap. wine_state's `changes=` does not grow at the reload,
  because it re-requests FULLSCREEN only when the state is lost. kitty's window
  size is unchanged: compare `$R h -j clients | jq '.[] | .size'` before and
  after.
- Then `fs $A unset`: A goes home to 1, and its place is paid back once. This is
  the normal case-4 ending, and the trip and its debt are intact.

Variant: the same run with `general:layout = "dwindle"` gives the same result,
and the trace has no `kept fullscreen` line (nothing is swapped).
