# xstate: a client that insists on maximized / fullscreen the way Wine does

`wine_state.c` maps an 800x600 X11 window (class `wine_state.exe`, so
Hyprdrive treats it as a game) and, on every `_NET_WM_STATE` PropertyNotify and
every ConfigureNotify, reads `_NET_WM_STATE` back; when the state it wants is
missing it sends `_NET_WM_STATE ADD` for it to the root (`max`: one message
naming MAXIMIZED_VERT + MAXIMIZED_HORZ; `fs`: FULLSCREEN). It prints, per
second, how many requests it sent (`sent=`) and how often the property changed
(`changes=`), and the property's current contents.

The fix it checks: `XGeometry.hpp`, "A GAME FLIPPING MAXIMIZED".

## Build (on fedora, as dragos)

    cc -O2 -o /tmp/hl-xstate/wine_state tests/rig/xstate/wine_state.c $(pkg-config --cflags --libs xcb)
    chmod 755 /tmp/hl-xstate /tmp/hl-xstate/wine_state   # hyprtest must be able to run it

## Each run

Four builds, one instance each (`HL_NAME=xs` keeps it apart from other agents):

| run | start                                                         |
|-----|---------------------------------------------------------------|
| A   | `HL_NAME=xs tests/rig/headless/hl.sh start --no-plugin`        |
| B   | `--so` a build of `d7771cc` (before the stale-request reset was adapted; has the 1.9.37 hook) |
| C   | `--so` a build of `8bb258c` (this branch's base)               |
| D   | `--so` this branch's `hyprdrive.so`                            |

Build B/C with `git worktree add /tmp/hd-<sha> <sha>` + `source ~/sdk/hyprland-0.56.2/env.sh; make -s -j8`.

1. Find the instance's X display: `HL_NAME=xs tests/rig/headless/hl.sh log 2000 | grep 'suitable display socket'`
   (Hyprland logs `XWayland found a suitable display socket at DISPLAY: :N`; XWayland starts
   lazily in 0.56, so if nothing is there yet launch any X client once, e.g. step 2, and look again).
2. Start the client (it exits by itself after the given seconds):

       HL_NAME=xs tests/rig/headless/hl.sh win env DISPLAY=:N /tmp/hl-xstate/wine_state max 20

   Its output goes to the instance's `client.<pid>.log` in the state dir (`/tmp/hl-xs/`).
3. While it runs, sample Hyprland's view every 50 ms for 10 s and count changes:

       for i in $(seq 200); do
         HL_NAME=xs tests/rig/headless/hl.sh h -j clients |
           jq -r '.[] | select(.class=="wine_state.exe") | "\(.fullscreen) \(.fullscreenClient)"'
         sleep 0.05
       done | uniq | wc -l        # distinct consecutive states = changes + 1

4. X's view, while it is maximized (run D): `DISPLAY=:N xprop -id <window id from the client's first line> _NET_WM_STATE`
   (run as hyprtest, or `hl.sh win env DISPLAY=:N sh -c 'xprop ... > /tmp/hl-xs/xprop.txt'`).
5. `HL_NAME=xs tests/rig/headless/hl.sh h hyprdrive:debug xgeom` (runs C/D): the
   `xstate: noop= blocked= storms= flips= told=` counters (D only).
6. Repeat 2-5 with `fs` instead of `max`.
7. `HL_NAME=xs tests/rig/headless/hl.sh stop` (must report no core).

## Pass criteria

`max` mode:

- A, B, C (expected FAIL, the bug): `fullscreen`/`fullscreenClient` keep alternating 0 <-> 1
  for the whole run (several changes per second, sustained); the client's `sent=` stays > 1
  every second; `xprop` never shows `_NET_WM_STATE_MAXIMIZED_*`.
- D (PASS): at most ONE change (0 0 -> 1 1) in the 10 s; the client's `sent=` is at most 3 in the
  first second (Hyprland's own write without MAXIMIZED can be read before ours; the extra ADDs are
  stripped and counted as `noop=`) and 0 after; `xprop` shows `_NET_WM_STATE_MAXIMIZED_VERT, _NET_WM_STATE_MAXIMIZED_HORZ`
  (and `_NET_WM_STATE_FOCUSED` if focused); `told>=1`. `storms=0`.

`fs` mode (Hyprland's fullscreen add is already idempotent, so all four should hold):

- at most one change (-> `2 2`), `sent=` 1 then 0, `xprop` shows `_NET_WM_STATE_FULLSCREEN`.
  A fullscreen loop seen here on any build is a NEW finding (it is not this bug).

Storm guard, `flap` mode (asks for the opposite of the FULLSCREEN it reads, every time):

- A, B, C: flips for the whole 20 s.
- D (PASS): about 7 changes in the first second, then none for 4 s (the block), then another
  burst of ~7 and another block, and so on -- never sustained flipping; the Hyprland log
  (`hl.sh log 400 | grep 'flipped its own'`) has ONE line per block; `storms>=1`, `blocked>0`.
- D, user actions are never blocked: during a block, the maximize/fullscreen dispatcher
  (`hl.sh h dispatch ...` / `hl.sh eval` with 0.56's Lua form -- see the Lua-IPC note) still
  changes the window every time, and in `max` mode `xprop` follows each change (MAXIMIZED
  present when maximized, absent when not).

Must not regress (D): an X11 window sending `_NET_WM_STATE ADD DEMANDS_ATTENTION` while unfocused
still becomes urgent (`hyprctl -j clients` / the `urgent` event); a fullscreen game swapped away from
and back to stays fullscreen.
