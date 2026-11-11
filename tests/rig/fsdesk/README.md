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
