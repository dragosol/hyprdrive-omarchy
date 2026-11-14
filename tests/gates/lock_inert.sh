#!/usr/bin/env bash
# lock_inert -- while the session is locked the plugin hands the keyboard to no window and the dock acts on no click; after the unlock both work again (#13890).
# Protects (restoreOverviewKeyboard / stealOverviewKeyboard, restoreDockSeat /
# stealDockSeat and Dock::inputInert's session-lock clause):
#   locking closes an open overview, and its teardown ends after the locker
#   took the seat: the direct setKeyboardFocus there gave the keyboard (and the
#   password being typed) back to the window focused before the lock. And a
#   dock that never hides kept its hit boxes under the lock screen: a click
#   there acted on an icon (marketplace review, #13890).
# The window is tests/gates/lockclient.py (logs every key and keyboard enter
# it gets); the locker is tests/gates/sessionlock.py (ext-session-lock-v1, one
# solid lock surface per output, logs the keys the lock screen gets).
# Asserts, dock autohide off, `game` focused:
#   1. overview open, then the session locks: the overview closes, and once its
#      close animation has ended keys typed reach the lock screen and NOT the
#      game (no `key` and no `kbenter` in its log after the lock)
#   2. still locked, a left click on the game's dock icon (game focused: an
#      unlocked click minimizes it): nothing happens -- same windows, same
#      boxes, nothing minimized, the same window focused, no new client
#   3. unlocked (the pointer back over the game, which Hyprland's unlock
#      refocuses): the game gets kbenter and keys again, and the same click on its icon
#      minimizes it (the guard is not sticky)
# Control (--control --control-so <922255a>): 1 fails (the game gets kbenter
# and the keys typed at the lock screen) and 2 fails (the click minimizes the
# game under the lock screen). Recorded 2026-11-13: control FAILS as it must.
# The first fixed build still gave the game a kbenter + kbleave the moment the
# lock started (the overview closed in the lock signal, before Hyprland called
# the session locked); hyprdriveSessionLocked now counts from the signal.
# NOT covered: trackpad gestures and the wheel while locked (the rig has no
# touchpad device), a dock drag, hyprlock itself (tests/rig/headless/lock_gate.sh).
source "$(dirname "$0")/lib.sh"
control_build "the build before the lock guards (922255a)"
gate_begin lockgate
EV=$GOUT/ev; mkdir -p "$EV" "$GOUT/py"; chmod 777 "$EV" "$GOUT/py"
cp "$GATES_DIR/lockclient.py" "$GATES_DIR/sessionlock.py" "$GOUT/"; chmod 644 "$GOUT/lockclient.py" "$GOUT/sessionlock.py"
SU python3 -m pywayland.scanner -i /usr/share/wayland/wayland.xml \
    /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml \
    /usr/share/wayland-protocols/unstable/pointer-constraints/pointer-constraints-unstable-v1.xml \
    /usr/share/wayland-protocols/unstable/relative-pointer/relative-pointer-unstable-v1.xml \
    /usr/share/wayland-protocols/staging/ext-session-lock/ext-session-lock-v1.xml \
    -o "$GOUT/py/lcproto" >/dev/null 2>&1 || skip "pywayland scanner failed for hyprtest"
rig_start
cfg '{plugin={hyprdrive={dock={autohide=false}}}}'

GL=$EV/game.log; LL=$EV/lock.log
n() { grep -c "^$2" "$1" 2>/dev/null || true; }
after() { tail -n +"$(( $2 + 1 ))" "$1" 2>/dev/null; }          # after LOG LINES: what came later
lines() { wc -l < "$1" 2>/dev/null || echo 0; }
locked() { dbg lockstate | grep -o 'locked=[0-9]' | cut -d= -f2; }
ovw() { dbg gesture state | grep -q ' 1 overview' && echo 1 || echo 0; }
active() { J activewindow | jq -r '.address // "none"'; }
# lock: the locker, launched by the compositor; waits for `locked`
lock() {
    : > "$LL"; chmod 666 "$LL"
    ev "hl.dispatch(hl.dsp.exec_cmd(\"env PYTHONPATH=$GOUT/py python3 $GOUT/sessionlock.py $LL\"))"
    wait_for 6 "grep -q '^locked' $LL"
    LPID=$(sed -n 's/^pid //p' "$LL" | head -1)
}
# unlock: SIGUSR1 to the locker we started (its pid, as the rig user); waits for `unlocked`
unlock() {
    [ -n "${LPID:-}" ] && SU kill -USR1 "$LPID" 2>/dev/null
    wait_for 5 "grep -q '^unlocked' $LL"; wait_for 3 "[ \"\$(locked)\" = 0 ]"; sleep 0.8
}
# click on the game's dock icon, one pointer device for the whole step
click_icon() { $R ptr seq "move $IX $IY" "wait 600" "click" "wait 400" >/dev/null 2>&1; sleep 1.2; }
state() { echo "$(geom) || active $(active) || n $(nclients)"; }

ev "hl.dispatch(hl.dsp.exec_cmd(\"env PYTHONPATH=$GOUT/py python3 $GOUT/lockclient.py $GL game none\"))"
wait_for 6 "[ -n \"\$(addr_of game)\" ]" || echo "warn: game never mapped"
G=$(addr_of game); focus "$G"; sleep 0.5; settle 3 geom
ICON=$(dbg dockmenu | grep -o 'icon game [-0-9,]* [0-9x]*' | awk '{print $3, $4}' | head -1)
[ -n "$ICON" ] || { check "dockmenu reports the game's icon" 0 "$(dbg dockmenu | tr '\n' ' ' | cut -c1-300)"; gate_end; }
set -- $(echo "$ICON" | tr ',x' '  '); IX=$(( $1 + $3 / 2 )); IY=$(( $2 + $4 / 2 ))
$R ptr move 960 300 >/dev/null; sleep 0.5
K0=$(n "$GL" "key 46 1")
$R type c >/dev/null; sleep 0.5
check "0: unlocked, a key reaches the focused game" "$(is "$(n "$GL" "key 46 1")" $((K0 + 1)))" "keys $K0 -> $(n "$GL" "key 46 1")"

# ---- 1. overview open, then the lock
$R overview >/dev/null; sleep 0.8
check "1: the overview is open" "$(ovw)" "$(dbg gesture state | grep -o '[0-9]* overview.*')"
M1=$(lines "$GL")
lock
check "1: the locker locked the session" "$(is "$(locked)" 1)" "lockstate $(dbg lockstate | grep -o 'locked=.*'); locker: $(tr '\n' ' ' < "$LL" | cut -c1-200)"
sleep 1.5   # past the overview's close animation: its teardown has run
check "1: locking closed the overview" "$(is "$(ovw)" 0)" "$(dbg gesture state | grep -o '[0-9]* overview.*')"
$R type ccc >/dev/null; sleep 0.6
check "1: keys typed while locked reach the lock screen" "$(ge "$(n "$LL" "key 46 1")" 3)" "locker: $(after "$LL" 0 | uniq -c | tr '\n' ' ' | cut -c1-200)"
check "1: ... and NOT the game (no key after the lock)" "$(is "$(after "$GL" "$M1" | grep -c '^key')" 0)" "game after the lock: $(after "$GL" "$M1" | uniq -c | tr '\n' ' ' | cut -c1-200)"
check "1: ... and the game never got the keyboard back (no kbenter after the lock)" "$(is "$(after "$GL" "$M1" | grep -c '^kbenter')" 0)" "game after the lock: $(after "$GL" "$M1" | uniq -c | tr '\n' ' ' | cut -c1-200)"

# ---- 2. still locked: a click on the game's dock icon
S0=$(state)
click_icon
check "2: locked, a click on the game's dock icon does nothing" "$(is "$(state)" "$S0")" "$(state) WAS $S0"
check "2: ... nothing minimized" "$(is "$(J clients | jq '[.[] | select(.workspace.name | startswith("special:minimized"))] | length')" 0)" "$(geom)"
check "2: ... still locked" "$(is "$(locked)" 1)" "lockstate $(dbg lockstate | grep -o 'locked=.*')"

# ---- 3. unlocked: keyboard and dock back
M3=$(lines "$GL")
# back over the game: Hyprland's unlock refocuses the window under the pointer
# (left on the dock icon, nothing gets the keyboard, whichever build)
$R ptr move 960 300 >/dev/null; sleep 0.5
unlock
check "3: the locker unlocked" "$(is "$(locked)" 0)" "lockstate $(dbg lockstate | grep -o 'locked=.*'); locker: $(tail -3 "$LL" | tr '\n' ' ')"
check "3: Hyprland gave the game its keyboard back (kbenter after the unlock)" "$(after "$GL" "$M3" | grep -q '^kbenter' && echo 1)" "after unlock: $(after "$GL" "$M3" | uniq -c | tr '\n' ' ' | cut -c1-200)"
K3=$(n "$GL" "key 46 1")
$R type c >/dev/null; sleep 0.5
check "3: unlocked, keys reach the game again" "$(is "$(n "$GL" "key 46 1")" $((K3 + 1)))" "keys $K3 -> $(n "$GL" "key 46 1"); after unlock: $(after "$GL" "$M3" | uniq -c | tr '\n' ' ' | cut -c1-200)"
click_icon
check "3: ... and the same click on its icon minimizes it" "$(win game .workspace.name | grep -q 'special:minimized' && echo 1)" "game on $(win game .workspace.name); $(geom)"

hq dispatch 'hl.plugin.hyprdrive.minimize("restore")' >/dev/null 2>&1; sleep 0.8
# closed here, not by the rig's stop: pywayland segfaults on a connection the
# compositor drops under it, and that would read as a core of this instance
[ "$(locked)" = 1 ] && unlock
close_win "$(addr_of game)"; sleep 1.2
gate_end
