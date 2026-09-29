#!/bin/bash
# nested.sh start|stop|sig|h ARGS...|shot [FILE]  -- a throwaway Hyprland for hyprdrive tests.
# Render code goes here BEFORE the live session (a zero box to a pass element, or
# a config read before registration, aborts the compositor).
N=$(dirname "$(readlink -f "$0")")
CFG="$N/hyprland.lua"
RUN="${XDG_RUNTIME_DIR:-/tmp}/hyprdrive-nested"; mkdir -p "$RUN"
find_pid() { pgrep -f -x "Hyprland -c $CFG" | head -1; }
find_sig() {
  local pid; pid=$(find_pid); [ -z "$pid" ] && return 1
  hyprctl instances | awk -v p="$pid" '/^instance /{s=$2; sub(":","",s)} /pid:/{ if ($2==p) print s }'
}
sock() { hyprctl instances | awk -v s="$(find_sig)" '$0 ~ s {f=1} f && /wl socket/ {print $3; exit}'; }
case "$1" in
  start)
    [ -n "$(find_pid)" ] && { echo "already running"; exit 0; }
    XDG_STATE_HOME="$RUN/state" WAYLAND_DISPLAY="${HOST_WAYLAND:-wayland-1}" setsid Hyprland -c "$CFG" >"$RUN/log.txt" 2>&1 </dev/null &
    for _ in $(seq 40); do sleep 0.25; [ -n "$(find_sig)" ] && break; done
    pid=$(find_pid)
    # ALWAYS on an empty desktop of the user's (user, 2026-09-29): first id 1..10
    # with no windows that no monitor shows; follow=false keeps their view.
    EMPTY=$( { hyprctl clients -j | jq -r '.[].workspace.id'; hyprctl monitors -j | jq -r '.[].activeWorkspace.id'; } | sort -u > "$RUN/busy"; for i in $(seq 1 10); do grep -qx "$i" "$RUN/busy" || { echo $i; break; }; done)
    ADDR=""
    for _ in $(seq 20); do
      ADDR=$(hyprctl clients -j | jq -r --argjson p "${pid:-0}" '.[] | select(.pid == $p) | .address' | head -1)
      [ -n "$ADDR" ] && break; sleep 0.2
    done
    if [ -n "$EMPTY" ] && [ -n "$ADDR" ]; then
      hyprctl dispatch "hl.dsp.window.move({ workspace = \"$EMPTY\", follow = false, window = \"address:$ADDR\" })" >/dev/null
      echo "nested window on empty desktop $EMPTY"
    else
      echo "WARNING: no empty desktop found (or no window) -- nested left where it opened"
    fi
    # Wallpaper (user, 2026-09-29): a 4K tropical photo, not a test grid.
    if [ -x "$N/bg" ] && [ -f "$N/tropical.jpg" ]; then
      WAYLAND_DISPLAY="$(sock)" GDK_BACKEND=wayland setsid "$N/bg" "$N/tropical.jpg" </dev/null >/dev/null 2>&1 &
    fi
    echo "pid $pid sig $(find_sig) log $RUN/log.txt" ;;
  stop)
    pid=$(find_pid); [ -z "$pid" ] && { echo "not running"; exit 0; }
    [ "$(ps -o args= -p "$pid")" = "Hyprland -c $CFG" ] && kill "$pid" && echo "stopped $pid" ;;
  sig) find_sig ;;
  sock) sock ;;
  log) echo "$RUN/log.txt" ;;
  h) shift; hyprctl --instance "$(find_sig)" "$@" ;;
  shot) WAYLAND_DISPLAY="$(sock)" grim "${2:-$RUN/shot.png}" && echo "${2:-$RUN/shot.png}" ;;
esac
