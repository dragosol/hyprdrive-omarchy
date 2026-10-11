#!/usr/bin/env python3
"""Wallpaper repositories, and the slideshow that walks them.

A REPOSITORY IS A DIRECTORY. That is the whole model, and it is what makes the
remote case cheap: a remote repository is a directory that git put there. Add a
path and it is a local repo; add a URL and it is cloned once and pulled on
demand. There is no index, no cache, no thumbnail database and nothing to
invalidate -- `list` is a glob, every time, so a picture dropped into the folder
by any other means is simply there.

    hdwall.py repos                     what is configured
    hdwall.py repo-add NAME SOURCE      a directory, or a git URL to clone
    hdwall.py repo-remove NAME          forget it (--delete also removes a clone)
    hdwall.py repo-pull [NAME]          git pull, for the cloned ones
    hdwall.py list [--repo NAME]        the wallpapers in it
    hdwall.py set PATH                  swap to one, through the rotation script
    hdwall.py next                      advance the slideshow by hand
    hdwall.py tick                      what the timer calls
    hdwall.py slideshow                 match the timer to the settings
    hdwall.py status                    repo, mode, interval, what is up

IT SWAPS THROUGH hyprwall-rotate.sh AND NOTHING ELSE. That script is the rice's
own rotation and it is load-bearing for the gate suite: its default is
palette-neutral on purpose (a rotation must not move the goalposts of every
colour measurement in the suite) and THEME=1 is the opt-in that re-derives the
matugen palette. A swap a PERSON asked for takes the THEME path, because
"the theme follows the wallpaper" is the product behaviour Phase 10 built; the
timer's own palette-neutral default is left exactly where it was.

THE SETTINGS ARE SCHEMA ROWS, and this file reads them out of the app's store
rather than owning its own copies -- wallpaper.repo, wallpaper.mode,
wallpaper.interval and wallpaper.theme are rows on the Wallpaper pane, written
through `hdctl set` like every other row. Only the repository LIST lives here,
because a list of directories is data and not a setting.

THE TIMER RE-READS THE SETTINGS ON EVERY TICK, which is why it fires once a
minute regardless of the interval and decides for itself whether enough time has
passed. Changing the interval therefore needs no unit rewritten, no daemon
reloaded and no state kept anywhere: the only thing that has to be true is that
the setting is on disk, and it is.
"""
import argparse
import json
import os
import pathlib
import random
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "scripts"))
import hdctl  # noqa: E402

REPOS = hdctl.STATE / "wallpapers.json"
CLONES = pathlib.Path(os.environ.get(
    "HYPRDRIVE_WALLPAPERS", os.path.expanduser("~/Pictures/hyprdrive-wallpapers")))
ROTATE = ROOT / "scripts" / "hyprwall-rotate.sh"
PANE = "wallpaper"

STILLS = (".jpg", ".jpeg", ".png", ".webp", ".bmp")
VIDEOS = (".mp4", ".webm", ".mkv", ".avi", ".mov")


# --------------------------------------------------------------------- state --
def load():
    if REPOS.exists():
        try:
            return json.loads(REPOS.read_text())
        except ValueError:
            pass
    # The guest's own directory is the one repository that always exists; on a
    # real install this is wherever the user keeps pictures. Seeded rather than
    # required so that `list` answers something on a machine nobody configured.
    default = os.environ.get("HYPRDRIVE_WALLPAPER_DIR", "/mnt/host/wallpapers")
    return {"repos": ([{"name": "local", "kind": "local", "path": default}]
                      if pathlib.Path(default).is_dir() else []),
            "last": 0}


def save(d):
    hdctl.STATE.mkdir(parents=True, exist_ok=True)
    hdctl.replace_file(REPOS, json.dumps(d, indent=1, sort_keys=True) + "\n")


def setting(key, fallback=None):
    """A row on the Wallpaper pane. The store is what `hdctl set` writes."""
    v = hdctl.load_store()["panes"].get(PANE, {}).get(key)
    if v is not None:
        return v
    for e in hdctl.schema()["entries"]:
        if e["key"] == key:
            return e.get("default", fallback)
    return fallback


def truthy(v):
    return str(v).strip().lower() in ("1", "true", "yes", "on")


def repo_named(d, name):
    for r in d["repos"]:
        if r["name"] == name:
            return r
    return None


def active_repo(d):
    want = setting("wallpaper.repo")
    return repo_named(d, want) or (d["repos"][0] if d["repos"] else None)


def wallpapers(repo):
    p = pathlib.Path(os.path.expanduser(repo["path"]))
    if not p.is_dir():
        return []
    return sorted(str(f) for f in p.iterdir()
                  if f.is_file() and f.suffix.lower() in STILLS + VIDEOS)


def current():
    return hdctl.ii_get("background.wallpaperPath")


# ---------------------------------------------------------------------- swap --
def swap(path, theme=None, quiet=False):
    """One way in and out: the rice's rotation script, with the palette opt-in
    taken from the wallpaper.theme row. Video files go the same way -- the script
    starts mpvpaper and derives the palette from a frame."""
    if theme is None:
        theme = truthy(setting("wallpaper.theme", True))
    env = hdctl._env() or dict(os.environ)
    env["WALL"] = str(path)
    env["THEME"] = "1" if theme else "0"
    r = subprocess.run(["bash", str(ROTATE)], env=env, capture_output=True, text=True)
    if not quiet:
        sys.stdout.write(r.stdout)
    sys.stderr.write(r.stderr)
    if r.returncode == 0:
        d = load()
        d["last"] = time.time()
        save(d)
    return r.returncode


def pick_next(d, files, mode):
    cur = current()
    if not files:
        return None
    if mode == "random":
        others = [f for f in files if f != cur]
        return random.choice(others or files)
    # ordered: the next one along, wrapping. A slideshow that jumps about is a
    # randomiser; this is the other setting.
    try:
        i = files.index(cur)
    except ValueError:
        return files[0]
    return files[(i + 1) % len(files)]


# ----------------------------------------------------------------- commands --
def cmd_repos(a):
    d = load()
    act = active_repo(d)
    rows = []
    for r in d["repos"]:
        p = pathlib.Path(os.path.expanduser(r["path"]))
        rows.append({**r, "exists": p.is_dir(), "count": len(wallpapers(r)),
                     "active": bool(act and act["name"] == r["name"])})
    if a.json:
        json.dump(rows, sys.stdout, indent=1)
        print()
        return 0
    for r in rows:
        print("%-1s %-14s %-6s %-52s %d wallpapers%s"
              % ("*" if r["active"] else " ", r["name"], r["kind"], r["path"], r["count"],
                 "" if r["exists"] else "   MISSING"))
    return 0


def cmd_repo_add(a):
    d = load()
    if repo_named(d, a.name):
        raise SystemExit("hdwall: there is already a repo called '%s'" % a.name)
    src = a.source
    if "://" in src or src.endswith(".git") or src.startswith("git@"):
        dest = CLONES / a.name
        dest.parent.mkdir(parents=True, exist_ok=True)
        if dest.exists():
            raise SystemExit("hdwall: %s already exists -- remove it or pick another name" % dest)
        print("hdwall: cloning %s -> %s" % (src, dest))
        # --depth 1: a wallpaper repository's history is megabytes of pictures
        # nobody will ever check out. Clone and pull only; there is no push path
        # and no credential handling anywhere in this file.
        r = subprocess.run(["git", "clone", "--depth", "1", src, str(dest)],
                           capture_output=True, text=True)
        sys.stdout.write(r.stdout)
        sys.stderr.write(r.stderr)
        if r.returncode != 0:
            raise SystemExit("hdwall: clone failed")
        entry = {"name": a.name, "kind": "git", "url": src, "path": str(dest)}
    else:
        p = pathlib.Path(os.path.expanduser(src)).resolve()
        if not p.is_dir():
            raise SystemExit("hdwall: %s is not a directory" % p)
        entry = {"name": a.name, "kind": "local", "path": str(p)}
    d["repos"].append(entry)
    save(d)
    print("hdwall: added %s (%s) -- %d wallpapers" % (a.name, entry["kind"], len(wallpapers(entry))))
    return 0


def cmd_repo_remove(a):
    d = load()
    r = repo_named(d, a.name)
    if not r:
        raise SystemExit("hdwall: no repo called '%s'" % a.name)
    d["repos"] = [x for x in d["repos"] if x["name"] != a.name]
    save(d)
    if a.delete and r["kind"] == "git":
        import shutil
        shutil.rmtree(r["path"], ignore_errors=True)
        print("hdwall: removed %s and deleted %s" % (a.name, r["path"]))
    else:
        print("hdwall: removed %s (the files are still in %s)" % (a.name, r["path"]))
    return 0


def cmd_repo_pull(a):
    d = load()
    targets = [repo_named(d, a.name)] if a.name else [r for r in d["repos"] if r["kind"] == "git"]
    if not targets or targets == [None]:
        print("hdwall: nothing to pull")
        return 0
    rc = 0
    for r in targets:
        if r["kind"] != "git":
            print("hdwall: %s is a local directory -- nothing to pull" % r["name"])
            continue
        before = len(wallpapers(r))
        p = subprocess.run(["git", "-C", r["path"], "pull", "--ff-only"],
                           capture_output=True, text=True)
        sys.stdout.write(p.stdout)
        sys.stderr.write(p.stderr)
        rc = rc or p.returncode
        print("hdwall: %s now has %d wallpapers (was %d)" % (r["name"], len(wallpapers(r)), before))
    return rc


def cmd_list(a):
    d = load()
    r = repo_named(d, a.repo) if a.repo else active_repo(d)
    if not r:
        raise SystemExit("hdwall: no wallpaper repository configured")
    files = wallpapers(r)
    if a.json:
        json.dump({"repo": r, "current": current(),
                   "wallpapers": [{"path": f, "video": f.lower().endswith(VIDEOS)} for f in files]},
                  sys.stdout, indent=1)
        print()
        return 0
    cur = current()
    for f in files:
        print("%s %s" % ("*" if f == cur else " ", f))
    return 0


def cmd_set(a):
    p = a.path
    if not pathlib.Path(p).is_file():
        # A bare name is looked up in the active repo, so the pane can hand over
        # what it displays without every caller building paths.
        d = load()
        r = repo_named(d, a.repo) if a.repo else active_repo(d)
        if not r:
            raise SystemExit("hdwall: '%s' is not a file and there is no repository to look it up in" % p)
        cand = [f for f in wallpapers(r)
                if pathlib.Path(f).name == p or pathlib.Path(f).stem == p]
        if not cand:
            raise SystemExit("hdwall: no wallpaper called '%s'" % p)
        p = cand[0]
    return swap(p, theme=None if a.theme is None else truthy(a.theme))


def cmd_next(a):
    d = load()
    r = active_repo(d)
    if not r:
        raise SystemExit("hdwall: no wallpaper repository configured")
    mode = str(setting("wallpaper.mode", "off"))
    nxt = pick_next(d, wallpapers(r), "random" if mode == "random" else "ordered")
    if not nxt:
        print("hdwall: %s has no wallpapers" % r["name"])
        return 0
    print("hdwall: %s -> %s" % (r["name"], nxt))
    return swap(nxt)


def cmd_tick(a):
    """The timer's entry point. Fires once a minute and decides for itself."""
    mode = str(setting("wallpaper.mode", "off"))
    if mode not in ("ordered", "random"):
        return 0
    d = load()
    interval = float(setting("wallpaper.interval", 15)) * 60
    left = d.get("last", 0) + interval - time.time()
    if left > 0:
        if a.verbose:
            print("hdwall: %.0fs to go" % left)
        return 0
    return cmd_next(a)


def cmd_slideshow(a):
    """Match the systemd timer to the setting. Idempotent, and safe to call from
    the pane on every change: the tick re-reads the settings anyway, so the timer
    running with the slideshow off is harmless -- this just stops it burning a
    wakeup a minute for nothing."""
    mode = str(setting("wallpaper.mode", "off"))
    want = mode in ("ordered", "random")
    unit = "hyprdrive-slideshow.timer"
    if subprocess.run(["systemctl", "--user", "cat", unit],
                      capture_output=True).returncode != 0:
        print("hdwall: %s is not installed (scripts/install-fragments.sh installs it)" % unit)
        return 0
    # --now either way: enabling should start it, disabling should stop it, and
    # a timer that is disabled but still running is the state nobody expects.
    subprocess.run(["systemctl", "--user", "enable" if want else "disable", "--now", unit],
                   capture_output=True)
    state = subprocess.run(["systemctl", "--user", "is-active", unit],
                           capture_output=True, text=True).stdout.strip()
    print("hdwall: slideshow %s, every %s minutes -- %s is %s"
          % (mode, setting("wallpaper.interval", 15), unit, state))
    return 0


def cmd_status(a):
    d = load()
    r = active_repo(d)
    st = {
        "repo": r,
        "repos": [x["name"] for x in d["repos"]],
        "current": current(),
        "mode": setting("wallpaper.mode", "off"),
        "interval": setting("wallpaper.interval", 15),
        "theme": truthy(setting("wallpaper.theme", True)),
        "last": d.get("last", 0),
        "count": len(wallpapers(r)) if r else 0,
        "mpvpaper": subprocess.run(["pgrep", "-x", "mpvpaper"],
                                   capture_output=True).returncode == 0,
    }
    if a.json:
        json.dump(st, sys.stdout, indent=1)
        print()
        return 0
    for k in ("repo", "current", "mode", "interval", "theme", "count", "mpvpaper"):
        print("%-10s %s" % (k, st[k]["name"] if k == "repo" and st[k] else st[k]))
    return 0


def main(argv):
    ap = argparse.ArgumentParser(description="hyprdrive wallpaper repositories and slideshow")
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("repos"); s.add_argument("--json", action="store_true")
    s.set_defaults(fn=cmd_repos)
    s = sub.add_parser("repo-add"); s.add_argument("name"); s.add_argument("source")
    s.set_defaults(fn=cmd_repo_add)
    s = sub.add_parser("repo-remove"); s.add_argument("name")
    s.add_argument("--delete", action="store_true"); s.set_defaults(fn=cmd_repo_remove)
    s = sub.add_parser("repo-pull"); s.add_argument("name", nargs="?")
    s.set_defaults(fn=cmd_repo_pull)
    s = sub.add_parser("list"); s.add_argument("--repo"); s.add_argument("--json", action="store_true")
    s.set_defaults(fn=cmd_list)
    s = sub.add_parser("set"); s.add_argument("path"); s.add_argument("--repo")
    s.add_argument("--theme"); s.set_defaults(fn=cmd_set)
    s = sub.add_parser("next"); s.add_argument("--verbose", action="store_true")
    s.set_defaults(fn=cmd_next)
    s = sub.add_parser("tick"); s.add_argument("--verbose", action="store_true")
    s.set_defaults(fn=cmd_tick)
    s = sub.add_parser("slideshow"); s.set_defaults(fn=cmd_slideshow)
    s = sub.add_parser("status"); s.add_argument("--json", action="store_true")
    s.set_defaults(fn=cmd_status)
    a = ap.parse_args(argv[1:])
    return a.fn(a)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
