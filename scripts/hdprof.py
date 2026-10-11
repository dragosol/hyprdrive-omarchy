#!/usr/bin/env python3
"""Profiles: a git repository over the whole config surface, and one undo button.

WHY GIT AND NOT A ZIP OF THE FILES. Because the hard part of "put it back" is
not storing the bytes, it is knowing which bytes and being able to say what
changed since. git does both, it has done them for twenty years, and it costs
one directory nobody has to look at. What this file adds is the part git cannot
know: WHICH files are the config surface, and WHAT has to be poked afterwards so
a restored file becomes a running session.

THE SURFACE, and why ii's config.json is in it. Three locations, because the
product writes three:

    ~/.config/hypr/hyprdrive/*.lua           the fragments -- every pane's
                                             machine half AND raw block, the
                                             derived theme, the monitor block.
    ~/.local/state/hyprdrive/settings.json   what the app owns (= what is pinned).
    ~/.config/illogical-impulse/config.json  ii's knobs AND the wallpaper path.

The third one is not obviously ours, and leaving it out was the tempting call.
It is in for two reasons that are the same reason: the settings app writes it
(the Quickshell pane's rows are `hdctl ii-set`, and `wallpaper.path` is a line in
that file), and the wallpaper lives in it. A profile that restored the accents
but not the wallpaper they were derived FROM would be a profile that looks wrong
the first time anything re-derives -- and a "revert" that leaves the shell's
transparency where the bad import put it is not a revert. So the rule is: if the
settings app can write it, a snapshot carries it.

What is deliberately NOT in it: the matugen palette
(~/.local/state/quickshell/user/generated/colors.json). It is DERIVED from the
wallpaper, the wallpaper is in the surface, and 05-theme-derived.lua -- which is
in the surface -- carries the values the session actually reads. Snapshotting a
derivation as well as its input is how the two get to disagree.

SNAPSHOTS ARE TAKEN BEFORE, NEVER AFTER, and that is the whole design.
`snapshot_before` is called by the three things that change many values at once
-- a theme import, a bulk apply, a monitor change -- and NOTHING commits
afterwards. So the repository is a chain of before-states, HEAD is "the surface as
it was before the last risky thing", and the difference between HEAD and the
files on disk IS the change you might want to undo.

That is what makes one-click revert one click: it takes no argument. It also
makes the log readable as an undo stack rather than as a diary -- every entry is
a place you can stand, and `status` saying "dirty" is the app saying there is
something to go back from. `snapshot` (or `save NAME`) is the other half: it
records the current state, which is how you say "this one is fine", and after it
there is nothing to revert until the next change.

CRASH SAFETY IS hdmon's SHAPE, because that shape was already right. The pending
record is written and fsynced BEFORE the mutation and deleted after it finishes,
so a process killed in the middle leaves one behind; `hdprof check` -- which the
launcher runs before the window is drawn, next to `hdmon check` -- finds it and
restores. An apply that dies half-written is undone by something that never
heard of the process that started it.

    hdprof.py init                    create the repo and take snapshot zero
    hdprof.py snapshot [--reason WHY] commit the surface as it stands
    hdprof.py list [-n N]             the snapshots, newest first
    hdprof.py save NAME               name this state -- a profile is a git tag
    hdprof.py profiles                the named ones
    hdprof.py apply NAME              restore a profile and make it live
    hdprof.py revert [REF]            restore REF (default HEAD) and make it live
    hdprof.py diff [REF]              what has changed since REF
    hdprof.py status                  clean, or which files are dirty
    hdprof.py check                   finish an apply that died halfway (run at start)
"""
import argparse
import json
import os
import pathlib
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "scripts"))
import hdctl  # noqa: E402  -- the state paths, the compositor helpers, atomic_json

REPO = hdctl.STATE / "profiles"
PENDING = hdctl.STATE / "apply-pending.json"
LOCK = hdctl.STATE / "apply-pending.lock"

# A commit needs an identity and this must not depend on the user having set a
# global one -- a settings app that fails to snapshot because ~/.gitconfig is
# empty is a settings app that fails to snapshot on a fresh install.
IDENT = ["-c", "user.name=hyprdrive", "-c", "user.email=hyprdrive@localhost",
         "-c", "commit.gpgsign=false"]

# (subdirectory in the repo, source directory, glob). The subdirectory exists so
# three unrelated locations can live in one tree without colliding, and so `git
# log --stat` reads like the product instead of like a filesystem.
SURFACE = [
    ("hyprdrive", hdctl.FRAGDIR, "*.lua"),
    ("state", hdctl.STORE.parent, "settings.json"),
    ("ii", hdctl.IICONF.parent, "config.json"),
]


def git(*args, check=True, repo=REPO):
    r = subprocess.run(["git", "-C", str(repo), *IDENT, *args],
                       capture_output=True, text=True)
    if check and r.returncode != 0:
        sys.stderr.write(r.stderr)
        raise SystemExit("hdprof: git %s failed" % " ".join(args))
    return r


def initialised():
    return (REPO / ".git").is_dir()


def need_repo():
    if not initialised():
        raise SystemExit("hdprof: no profile repository -- run `hdprof.py init` first")


# ------------------------------------------------------------------- surface --
def surface_files():
    """-> {relative path in the repo: absolute path on disk}. Only what exists."""
    out = {}
    for sub, src, pattern in SURFACE:
        if not src.is_dir():
            continue
        for p in sorted(src.glob(pattern)):
            if p.is_file():
                out["%s/%s" % (sub, p.name)] = p
    return out


def surface_dest(rel):
    """The other direction: a path in the repo -> where it belongs on disk."""
    sub, _, name = rel.partition("/")
    for s, src, _pattern in SURFACE:
        if s == sub:
            return src / name
    return None


def stage():
    """Copy the surface into the work tree; drop what is no longer there.

    A copy and not a symlink farm: the repo has to hold the BYTES, because the
    thing it is insuring against is those bytes changing."""
    # The repo holds copies of hyprland.lua and every fragment: owner-only, like
    # a private config itself (marketplace review, #11012). Re-applied on every
    # stage so a repo made before this rule is closed too.
    REPO.mkdir(parents=True, exist_ok=True)
    os.chmod(REPO, 0o700)
    live = surface_files()
    for rel, path in live.items():
        dst = REPO / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        dst.write_bytes(path.read_bytes())
    for sub, _src, _pat in SURFACE:
        d = REPO / sub
        if not d.is_dir():
            continue
        for p in d.iterdir():
            if p.is_file() and "%s/%s" % (sub, p.name) not in live:
                p.unlink()
    git("add", "-A")
    return live


def dirty():
    """What has changed on disk since HEAD.

    It has to `stage()` first, and that is not an optimisation: the work tree is
    a COPY of the surface, so asking git about it without refreshing the copy
    answers a question about the last snapshot instead of about the system.
    Measured 2026-08-12 -- the first round-trip reported a clean surface
    immediately after a bulk apply had changed two files."""
    stage()
    return [l[3:] for l in git("status", "--porcelain").stdout.splitlines()]


def head():
    r = git("rev-parse", "--short", "HEAD", check=False)
    return r.stdout.strip() if r.returncode == 0 else None


def tree_files(ref):
    return [l for l in git("ls-tree", "-r", "--name-only", ref).stdout.splitlines() if l]


def restore(ref):
    """Put every file of REF back on disk, byte for byte, and remove what REF
    does not have. Reads through `git show` rather than checking the ref out:
    nothing here needs HEAD to move, and a detached work tree is a second state
    to reason about."""
    want = set(tree_files(ref))
    written, removed = [], []
    for rel in sorted(want):
        dest = surface_dest(rel)
        if dest is None:
            continue
        blob = subprocess.run(["git", "-C", str(REPO), "show", "%s:%s" % (ref, rel)],
                              capture_output=True, check=True).stdout
        dest.parent.mkdir(parents=True, exist_ok=True)
        if dest.exists() and dest.read_bytes() == blob:
            continue
        # Atomic per file: a fragment caught half-written is a config that does
        # not parse, and hyprland.lua sources these [fragment.py].
        hdctl.replace_file(dest, blob)
        written.append(rel)
    for rel in sorted(surface_files()):
        if rel not in want:
            surface_dest(rel).unlink(missing_ok=True)
            removed.append(rel)
    return written, removed


# ------------------------------------------------------------------ snapshots --
def snapshot(reason, quiet=False):
    """Commit the surface as it stands. Returns the sha, or HEAD when nothing
    moved -- an unchanged surface does not need a commit to be restorable."""
    need_repo()
    stage()
    if not git("status", "--porcelain").stdout.strip():
        h = head()
        if not quiet:
            print("hdprof: nothing changed since %s" % h)
        return h
    git("commit", "-m", reason, "-q")
    h = head()
    if not quiet:
        print("hdprof: snapshot %s -- %s" % (h, reason))
    return h


def snapshot_before(reason):
    """THE auto-snapshot. One function, three callers: the theme importer, a bulk
    apply, and a monitor change. Each of them is a change that touches many
    values at once, which is exactly when "what did that do" stops being a
    question anybody can answer from memory.

    Returns the ref to go back to, and leaves a pending record on disk saying so.
    Call `finish()` when the mutation completed -- which only drops the record.
    There is deliberately no after-snapshot: see the header."""
    if not initialised():
        cmd_init(argparse.Namespace(quiet=True))
    ref = snapshot("before: " + reason, quiet=True)
    hdctl.atomic_json(PENDING, {"ref": ref, "reason": reason, "started": time.time()})
    return ref


def finish():
    """The mutation completed, so the crash-safety record is no longer owed.

    It does NOT snapshot. Committing the result here would move HEAD onto the
    change and leave `revert` with nothing to go back to -- the exact button this
    whole file exists to make work."""
    PENDING.unlink(missing_ok=True)


def read_pending():
    if not PENDING.exists():
        return None
    try:
        return json.loads(PENDING.read_text())
    except ValueError:
        return None


# ---------------------------------------------------------------------- apply --
def apply_live(written, quiet=False):
    """Make restored files into a running session.

    Two mechanisms, because the surface has two consumers. The compositor re-reads
    its whole config chain on `hyprctl reload` -- that covers every fragment,
    including the derived one and the monitor block. Quickshell watches its own
    config.json for most keys but not all of them (fonts, transparency and the
    palette type are read at construction), so an ii change gets a restart. It is
    only fired when ii's file actually moved: restarting the shell tears down the
    bar and the dock for a second, and doing that on a change it would have picked
    up by itself is a worse experience than the change."""
    hdctl.hyprctl("reload")
    if not quiet:
        print("hdprof: compositor reloaded")
    if any(r.startswith("ii/") for r in written):
        qs_restart(quiet=quiet)


def _session_env():
    """An environment a Wayland client can actually start in.

    hdctl._env() finds the compositor's IPC socket, which is all hyprctl needs.
    Quickshell needs the DISPLAY socket and the session bus as well, and the app
    is not always what starts this -- a gate, a cron, an ssh shell all get here
    with a bare environment, and a shell restarted without WAYLAND_DISPLAY exits
    immediately and takes the bar with it. Measured 2026-08-12: the first revert
    that touched ii's config left the desktop with no shell at all.

    Freshest socket wins, never a hardcoded wayland-1: a restarted session leaves
    the old, wedged compositor holding that name [lib/rig.sh]."""
    env = hdctl._env() or dict(os.environ)
    run = pathlib.Path(env.get("XDG_RUNTIME_DIR", "/run/user/%d" % os.getuid()))
    if not env.get("WAYLAND_DISPLAY") or not (run / env["WAYLAND_DISPLAY"]).exists():
        socks = sorted((p for p in run.glob("wayland-*") if not p.name.endswith(".lock")),
                       key=lambda p: p.stat().st_mtime, reverse=True)
        if socks:
            env["WAYLAND_DISPLAY"] = socks[0].name
    env.setdefault("DBUS_SESSION_BUS_ADDRESS", "unix:path=%s/bus" % run)
    return env


def qs_restart(quiet=False):
    """Restart illogical-impulse's quickshell. `qs -c ii` is how the session
    starts it (hyprland's exec-once), and quickshell brings its own layer
    surfaces back; killing it without restarting it would leave the desktop with
    no bar, which is why this is one function and not two calls."""
    r = subprocess.run(["pgrep", "-x", "qs"], capture_output=True, text=True)
    if r.returncode != 0:
        if not quiet:
            print("hdprof: quickshell is not running -- nothing to restart")
        return False
    env = _session_env()
    subprocess.run(["pkill", "-x", "qs"], capture_output=True)
    time.sleep(1.0)
    subprocess.Popen(["setsid", "qs", "-c", "ii"], env=env, start_new_session=True,
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    # Wait for it to be back before returning: a caller that reloads the
    # compositor next would otherwise race the shell's layer surfaces, and a
    # gate would photograph a desktop with no bar and call it a result.
    for _ in range(40):
        time.sleep(0.25)
        if subprocess.run(["pgrep", "-x", "qs"], capture_output=True).returncode == 0:
            break
    else:
        print("hdprof: quickshell did not come back", file=sys.stderr)
        return False
    if not quiet:
        print("hdprof: quickshell restarted")
    return True


# ------------------------------------------------------------------- commands --
def cmd_init(a):
    quiet = getattr(a, "quiet", False)
    if initialised():
        if not quiet:
            print("hdprof: already initialised at %s (HEAD %s)" % (REPO, head()))
        return 0
    REPO.mkdir(parents=True, exist_ok=True)
    os.chmod(REPO, 0o700)
    git("init", "-q", "-b", "main")
    (REPO / "README").write_text(
        "Hyprdrive config profiles.\n\n"
        "Every commit is a snapshot of the config surface; every tag is a named\n"
        "profile. Restore with `scripts/hdprof.py revert <ref>` -- copying files\n"
        "out of here by hand skips the reload that makes them live.\n\n"
        "There are no remotes and there is nothing to push. This repository\n"
        "exists so that a bad import is one command away from never having\n"
        "happened.\n")
    for sub, _src, _pat in SURFACE:
        (REPO / sub).mkdir(exist_ok=True)
    stage()
    git("commit", "-m", "snapshot zero: the surface as hdprof found it", "-q",
        "--allow-empty")
    if not quiet:
        print("hdprof: initialised %s -- snapshot zero is %s" % (REPO, head()))
        for rel in sorted(surface_files()):
            print("    %s" % rel)
    return 0


def cmd_snapshot(a):
    need_repo()
    snapshot(a.reason)
    if a.tag:
        git("tag", "-f", a.tag, "HEAD")
        print("hdprof: profile '%s' -> %s" % (a.tag, head()))
    return 0


def cmd_list(a):
    need_repo()
    fmt = "%h\t%ct\t%D\t%s"
    out = git("log", "-n", str(a.n), "--pretty=format:" + fmt).stdout
    rows = []
    for line in out.splitlines():
        h, ct, refs, subject = line.split("\t", 3)
        tags = [r.strip().replace("tag: ", "") for r in refs.split(",") if "tag:" in r]
        rows.append({"ref": h, "when": int(ct), "profiles": tags, "reason": subject})
    if a.json:
        json.dump({"head": head(), "dirty": dirty(), "snapshots": rows}, sys.stdout, indent=1)
        print()
        return 0
    for r in rows:
        print("%s  %s  %s%s" % (r["ref"], time.strftime("%Y-%m-%d %H:%M", time.localtime(r["when"])),
                                r["reason"], ("   [%s]" % ", ".join(r["profiles"])) if r["profiles"] else ""))
    return 0


def cmd_save(a):
    need_repo()
    snapshot("profile '%s'" % a.name, quiet=True)
    git("tag", "-f", a.name, "HEAD")
    print("hdprof: profile '%s' saved at %s" % (a.name, head()))
    return 0


def cmd_profiles(a):
    need_repo()
    rows = []
    for name in git("tag", "--sort=-creatordate").stdout.split():
        sha = git("rev-parse", "--short", name).stdout.strip()
        when = int(git("log", "-1", "--pretty=format:%ct", name).stdout.strip())
        rows.append({"name": name, "ref": sha, "when": when})
    if a.json:
        json.dump(rows, sys.stdout, indent=1)
        print()
        return 0
    for r in rows:
        print("%-24s %s  %s" % (r["name"], r["ref"],
                                time.strftime("%Y-%m-%d %H:%M", time.localtime(r["when"]))))
    return 0


def _restore_and_apply(ref, no_apply):
    written, removed = restore(ref)
    for rel in written:
        print("    restored %s" % rel)
    for rel in removed:
        print("    removed  %s (it did not exist at %s)" % (rel, ref))
    if not written and not removed:
        print("    the surface already matches %s" % ref)
    if not no_apply:
        apply_live(written + removed)
    return 0


def cmd_apply(a):
    need_repo()
    if git("rev-parse", "--verify", "-q", a.name, check=False).returncode != 0:
        raise SystemExit("hdprof: no profile called '%s'" % a.name)
    print("hdprof: applying profile '%s'" % a.name)
    snapshot_before("apply profile '%s'" % a.name)
    rc = _restore_and_apply(a.name, a.no_apply)
    finish()
    return rc


def cmd_revert(a):
    need_repo()
    ref = a.ref or "HEAD"
    if git("rev-parse", "--verify", "-q", ref, check=False).returncode != 0:
        raise SystemExit("hdprof: no snapshot called '%s'" % ref)
    sha = git("rev-parse", "--short", ref).stdout.strip()
    print("hdprof: reverting to %s (%s)" % (sha, git("log", "-1", "--pretty=format:%s", ref).stdout))
    rc = _restore_and_apply(ref, a.no_apply)
    PENDING.unlink(missing_ok=True)
    return rc


def cmd_diff(a):
    need_repo()
    stage()
    ref = a.ref or "HEAD"
    r = git("diff", "--stat" if not a.full else "--patch", ref, check=False)
    sys.stdout.write(r.stdout or "    (identical to %s)\n" % ref)
    return 0


def cmd_status(a):
    need_repo()
    d = dirty()
    p = read_pending()
    if a.json:
        json.dump({"head": head(), "dirty": d, "pending": p,
                   "repo": str(REPO), "surface": sorted(surface_files())},
                  sys.stdout, indent=1)
        print()
        return 0
    print("repo   %s" % REPO)
    print("head   %s  %s" % (head(), git("log", "-1", "--pretty=format:%s").stdout))
    print("files  %d" % len(surface_files()))
    if p:
        print("PENDING an apply started %.0fs ago and never finished: %s" %
              (time.time() - p["started"], p["reason"]))
    print("dirty  %s" % (", ".join(d) if d else "nothing -- the surface matches HEAD"))
    return 0


def cmd_check(a):
    """Run at app start, beside `hdmon check`. A pending record here means an
    apply began and never reported finishing -- the importer was killed, the app
    was killed, the machine went down. Whatever landed is half of something, so
    it goes back to the snapshot the apply took before it started."""
    if not initialised():
        print("hdprof: no profile repository")
        return 0
    p = read_pending()
    if not p:
        print("hdprof: clean")
        return 0
    fh = hdctl.flock_or_none(LOCK)
    if fh is None:
        print("hdprof: another process is already restoring")
        return 0
    try:
        p = read_pending()
        if not p:
            print("hdprof: clean")
            return 0
        print("hdprof: an apply did not finish (%s) -- restoring %s" % (p["reason"], p["ref"]))
        written, removed = restore(p["ref"])
        for rel in written + removed:
            print("    restored %s" % rel)
        PENDING.unlink(missing_ok=True)
        if not a.no_apply:
            apply_live(written + removed)
        return 0
    finally:
        fh.close()


def main(argv):
    ap = argparse.ArgumentParser(description="hyprdrive config profiles (git-backed)")
    sub = ap.add_subparsers(dest="cmd", required=True)

    sub.add_parser("init").set_defaults(fn=cmd_init)
    s = sub.add_parser("snapshot")
    s.add_argument("--reason", default="manual snapshot")
    s.add_argument("--tag", help="also name this state as a profile")
    s.set_defaults(fn=cmd_snapshot)
    s = sub.add_parser("list"); s.add_argument("-n", type=int, default=20)
    s.add_argument("--json", action="store_true"); s.set_defaults(fn=cmd_list)
    s = sub.add_parser("save"); s.add_argument("name"); s.set_defaults(fn=cmd_save)
    s = sub.add_parser("profiles"); s.add_argument("--json", action="store_true")
    s.set_defaults(fn=cmd_profiles)
    s = sub.add_parser("apply"); s.add_argument("name")
    s.add_argument("--no-apply", action="store_true"); s.set_defaults(fn=cmd_apply)
    s = sub.add_parser("revert"); s.add_argument("ref", nargs="?")
    s.add_argument("--no-apply", action="store_true"); s.set_defaults(fn=cmd_revert)
    s = sub.add_parser("diff"); s.add_argument("ref", nargs="?")
    s.add_argument("--full", action="store_true"); s.set_defaults(fn=cmd_diff)
    s = sub.add_parser("status"); s.add_argument("--json", action="store_true")
    s.set_defaults(fn=cmd_status)
    s = sub.add_parser("check"); s.add_argument("--no-apply", action="store_true")
    s.set_defaults(fn=cmd_check)

    a = ap.parse_args(argv[1:])
    return a.fn(a)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
