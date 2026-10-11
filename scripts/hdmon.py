#!/usr/bin/env python3
"""The Monitors pane: per-output mode/scale, on a fifteen-second leash.

WHY THE COUNTDOWN IS THE FEATURE. A wrong mode is the one settings change that
can take away the screen you would use to undo it. Every desktop that ships this
pane therefore ships "keep these settings?", and the interesting half is not the
dialog -- it is what happens when the thing that was supposed to count down is
not there any more. If the countdown lives in the app, then the app crashing (or
the compositor taking it down with a mode the GPU cannot scan out) leaves the
bad mode permanent, which is the exact case the countdown exists for.

So the revert is written to DISK BEFORE the change is applied, and there are two
independent things that can carry it out:

    the watcher     a detached process (setsid, own session) that outlives the
                    app. Kill the app, the watcher still reverts on time.
    the next start  `hdmon check`, which the launcher runs before the window is
                    even mapped. Kill the app AND the watcher -- kill the whole
                    process group, pull the plug -- and the revert happens the
                    next time anything opens the settings.

Both read the same pending file and both take the same lock, so a revert can
never run twice. Confirming is deleting that file, and that is the only way it
is deleted other than by a revert.

THE PENDING FILE HOLDS THE PREVIOUS FRAGMENT VERBATIM, not a description of it.
Restoring "what it was" from a re-derivation is restoring what the code thinks it
was; restoring bytes is restoring it.

    hdmon.py list                       outputs, modes, scales, as json
    hdmon.py apply --output N ...       apply behind the countdown
    hdmon.py confirm                    keep them
    hdmon.py revert                     put them back now
    hdmon.py pending                    seconds left, or none
    hdmon.py check                      revert a countdown nobody finished (run at start)
    hdmon.py watch                      the detached timer (started by apply)
"""
import argparse
import json
import pathlib
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "scripts"))
import hdctl  # noqa: E402  -- the compositor helpers and the fragment writer, once
import hdprof  # noqa: E402  -- the auto-snapshot; a monitor change is one of its three sites

PENDING = hdctl.STATE / "monitor-pending.json"
LOCK = hdctl.STATE / "monitor-pending.lock"
FRAGMENT_NAME = "45-monitors.lua"
DEFAULT_TIMEOUT = 15


# ------------------------------------------------------------------- outputs --
def outputs():
    out = hdctl.hyprctl("monitors", "all", "-j")
    if not out:
        return []
    return [{
        "name": m["name"],
        "description": m.get("description", ""),
        "mode": "%dx%d@%.2fHz" % (m["width"], m["height"], m.get("refreshRate", 0)),
        "width": m["width"], "height": m["height"], "refresh": m.get("refreshRate", 0),
        "scale": m.get("scale", 1.0),
        "position": "%dx%d" % (m.get("x", 0), m.get("y", 0)),
        "disabled": m.get("disabled", False),
        "availableModes": m.get("availableModes", []),
    } for m in json.loads(out)]


# ------------------------------------------------------------------ fragment --
def render(settings):
    L = ["-- hyprdrive fragment 45-monitors -- the Monitors pane of the settings app.",
         "--",
         "-- MACHINE-OWNED above the raw-snippet marker; USER-OWNED below it.",
         "--",
         "-- Every line here was applied behind a keep-these-settings countdown, and the",
         "-- bytes this file had BEFORE that change were written to",
         "-- ~/.local/state/hyprdrive/monitor-pending.json first -- so an unconfirmed",
         "-- change is undone by a detached watcher, or by the next `hdmon check` if the",
         "-- watcher went down with the app.",
         ""]
    if not settings:
        L.append("-- (no output overridden yet)")
        return "\n".join(L) + "\n"
    for name in sorted(settings):
        s = settings[name]
        if s.get("enabled") is False:
            L.append("hl.monitor({output = [[%s]], disabled = true})" % name)
            continue
        parts = ["output = [[%s]]" % name]
        if s.get("mode"):
            parts.append("mode = [[%s]]" % s["mode"])
        if s.get("position"):
            parts.append("position = [[%s]]" % s["position"])
        if s.get("scale") is not None:
            parts.append("scale = %s" % s["scale"])
        L.append("hl.monitor({%s})" % ", ".join(parts))
    return "\n".join(L) + "\n"


def frag_path():
    return hdctl.FRAGDIR / FRAGMENT_NAME


def write_fragment(settings, quiet=False):
    r = subprocess.run([sys.executable, str(hdctl.FRAGMENT), "write", str(frag_path())],
                       input=render(settings), text=True, capture_output=True)
    if r.returncode != 0:
        sys.stderr.write(r.stderr)
        raise SystemExit(1)
    if not quiet:
        print(r.stdout.strip())


# ------------------------------------------------------------------- pending --
# Written-and-fsynced-before-the-change, and the one-winner lock, both live in
# hdctl now: hdprof needs the identical guarantee for the identical reason, and
# the second copy of "this record survives a power cut" is the one that quietly
# stops calling fsync. Same behaviour, one implementation [hdctl.py].
atomic_json = hdctl.atomic_json


def read_pending():
    if not PENDING.exists():
        return None
    try:
        return json.loads(PENDING.read_text())
    except ValueError:
        return None


def _locked():
    return hdctl.flock_or_none(LOCK)


def do_revert(why):
    fh = _locked()
    if fh is None:
        print("hdmon: another process is already reverting")
        return 0
    try:
        p = read_pending()
        if p is None:
            print("hdmon: nothing pending")
            return 0
        # The fragment, byte for byte, as it was.
        if p["previous"] is None:
            frag_path().unlink(missing_ok=True)
        else:
            hdctl.replace_file(frag_path(), p["previous"])
        store = hdctl.load_store()
        store["panes"]["monitors"] = p["previous_settings"]
        hdctl.save_store(store)
        PENDING.unlink(missing_ok=True)
        hdctl.hyprctl("reload")
        print("hdmon: REVERTED (%s) -- %s is back to what it was" % (why, FRAGMENT_NAME))
        return 0
    finally:
        fh.close()


# ------------------------------------------------------------------ commands --
def cmd_list(a):
    json.dump(outputs(), sys.stdout, indent=1)
    print()
    return 0


def cmd_apply(a):
    if read_pending():
        raise SystemExit("hdmon: a countdown is already running -- confirm or revert it first")
    store = hdctl.load_store()
    previous_settings = dict(store["panes"].get("monitors", {}))
    settings = json.loads(json.dumps(previous_settings))
    cur = settings.setdefault(a.output, {})
    if a.mode is not None:
        cur["mode"] = a.mode
    if a.scale is not None:
        cur["scale"] = a.scale
    if a.position is not None:
        cur["position"] = a.position
    if a.enabled is not None:
        cur["enabled"] = a.enabled == "1"

    # The profile snapshot, which is a DIFFERENT promise from the countdown below
    # and not a duplicate of it. The countdown undoes this one change within
    # fifteen seconds; the snapshot is what lets somebody undo it next week,
    # together with whatever else was going on. Same function the importer and
    # the bulk apply call [hdprof.snapshot_before].
    hdprof.snapshot_before("monitor change on %s" % a.output)

    # DISK FIRST. Everything after this line can be undone by somebody who has
    # never heard of this process.
    atomic_json(PENDING, {
        "fragment": FRAGMENT_NAME,
        "previous": frag_path().read_text() if frag_path().exists() else None,
        "previous_settings": previous_settings,
        "applied": {a.output: cur},
        "deadline": time.time() + a.timeout,
        "timeout": a.timeout,
    })

    store["panes"]["monitors"] = settings
    hdctl.save_store(store)
    write_fragment(settings)
    hdctl.hyprctl("reload")

    if not a.no_watch:
        # setsid: its own session, so killing the app's process group does not
        # take the timer with it. The gate proves both halves.
        subprocess.Popen(["setsid", sys.executable, str(pathlib.Path(__file__).resolve()), "watch"],
                         start_new_session=True,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    # The mutation itself is complete -- the countdown is a separate, longer
    # leash that hdmon owns. Leaving hdprof's pending record open past this point
    # would make the next `hdprof check` undo a change that landed properly.
    hdprof.finish()
    print("hdmon: applied %s -- %ds to confirm, or it goes back" % (a.output, a.timeout))
    return 0


def cmd_confirm(a):
    if not read_pending():
        print("hdmon: nothing pending")
        return 0
    PENDING.unlink(missing_ok=True)
    print("hdmon: kept")
    return 0


def cmd_revert(a):
    return do_revert("asked")


def cmd_pending(a):
    p = read_pending()
    if not p:
        print("none")
        return 0
    print("%.1f" % max(0.0, p["deadline"] - time.time()))
    return 0


def cmd_check(a):
    """Run at app start. A pending file at start means nobody confirmed --
    either the countdown expired with the watcher gone, or the app died."""
    p = read_pending()
    if not p:
        print("hdmon: clean")
        return 0
    return do_revert("unconfirmed at startup")


def cmd_watch(a):
    p = read_pending()
    if not p:
        return 0
    while True:
        p = read_pending()
        if not p:            # confirmed or reverted under us
            return 0
        left = p["deadline"] - time.time()
        if left <= 0:
            break
        time.sleep(min(left, 0.5))
    return do_revert("countdown expired")


def main(argv):
    ap = argparse.ArgumentParser(description="hyprdrive monitors, with the keep-these-settings countdown")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("list").set_defaults(fn=cmd_list)
    s = sub.add_parser("apply")
    s.add_argument("--output", required=True)
    s.add_argument("--mode")
    s.add_argument("--scale")
    s.add_argument("--position")
    s.add_argument("--enabled", choices=["0", "1"])
    s.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT)
    s.add_argument("--no-watch", action="store_true", help="do not start the detached timer (gates)")
    s.set_defaults(fn=cmd_apply)
    sub.add_parser("confirm").set_defaults(fn=cmd_confirm)
    sub.add_parser("revert").set_defaults(fn=cmd_revert)
    sub.add_parser("pending").set_defaults(fn=cmd_pending)
    sub.add_parser("check").set_defaults(fn=cmd_check)
    sub.add_parser("watch").set_defaults(fn=cmd_watch)
    a = ap.parse_args(argv[1:])
    return a.fn(a)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
