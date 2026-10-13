#!/usr/bin/env python3
"""omarchy-plugin/bin/hdicons -- the settings panel's per-app dock icons.

Offline and against a throwaway HOME: the selfh.st path is checked only for
refusing while it is off and for refusing a bad reference; the download itself
needs the network and is checked by hand. Part of `make check`.
"""
import json
import os
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
HD = ROOT / "omarchy-plugin/bin/hdicons"
os.umask(0o022)
fails = []


def run(env, *args):
    r = subprocess.run([sys.executable, str(HD), *args], env=env, capture_output=True, text=True)
    try:
        return json.loads(r.stdout)
    except ValueError:
        return {"ok": False, "error": "no JSON: " + r.stdout + r.stderr}


def check(what, cond):
    print("%-58s %s" % (what, "ok" if cond else "FAILED"))
    if not cond:
        fails.append(what)


with tempfile.TemporaryDirectory() as home:
    env = dict(os.environ, HOME=home, XDG_STATE_HOME=home + "/state", XDG_DATA_HOME=home + "/data",
               XDG_CACHE_HOME=home + "/cache")
    state = pathlib.Path(home, "state/hyprdrive"); state.mkdir(parents=True)
    (state / "dock-apps").write_text("org.gnome.Nautilus\tFiles\torg.gnome.Nautilus\nzen\tZen Browser\tzen-browser\n")
    overrides = state / "icons"
    store = pathlib.Path(home, "data/hyprdrive/icons")

    d = run(env, "list")
    check("list: the dock's apps, in order", [a["id"] for a in d.get("apps", [])] == ["org.gnome.Nautilus", "zen"])
    check("list: selfh.st off by default", d.get("settings", {}).get("selfhst") is False)

    # A real picture from a folder: copied, named by content, private store file
    pics = pathlib.Path(home, "pics"); pics.mkdir()
    svg = pics / "my-files.svg"
    svg.write_text('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 8 8"><rect width="8" height="8"/></svg>')
    d = run(env, "set", "org.gnome.Nautilus", "file:" + str(svg))
    check("set file: ok", d.get("ok") is True)
    copied = pathlib.Path(d.get("icon", "/nonexistent"))
    check("set file: copied into the icon store", copied.parent == store and copied.read_bytes() == svg.read_bytes())
    check("store file written 0600", (os.stat(overrides).st_mode & 0o777) == 0o600)
    check("store line is id TAB path", ("org.gnome.Nautilus\t%s\n" % copied) in overrides.read_text())
    svg.unlink()
    check("the dock keeps its copy when the folder's file goes", copied.exists())

    # Same picture again: same name, no duplicate
    svg.write_text(copied.read_text())
    run(env, "set", "org.gnome.Nautilus", "file:" + str(svg))
    check("same picture = same file, no duplicate", len(list(store.iterdir())) == 1)

    # Refusals
    fake = pics / "fake.png"; fake.write_text("not a png")
    check("refuses a .png that is not a PNG", run(env, "set", "zen", "file:" + str(fake)).get("ok") is False)
    check("refuses a non-image file", run(env, "set", "zen", "file:/etc/hostname").get("ok") is False)
    check("refuses a relative path", run(env, "set", "zen", "file:pics/x.svg").get("ok") is False)
    check("refuses an id with a newline", run(env, "set", "a\nb", "theme:folder").get("ok") is False)
    check("refuses a theme name with shell in it", run(env, "set", "zen", "theme:$(id)").get("ok") is False)
    check("refuses a theme name the theme lacks", run(env, "set", "zen", "theme:no-such-icon-hd-test").get("ok") is False)
    check("refuses selfh.st while it is off", run(env, "set", "zen", "selfhst:firefox").get("ok") is False)
    run(env, "config", '{"selfhst": true}')
    check("refuses a bad selfh.st reference", run(env, "set", "zen", "selfhst:../../etc").get("ok") is False)
    run(env, "config", '{"selfhst": false}')
    check("search offline with selfh.st off: no selfh.st rows", run(env, "search", "files").get("selfhst") == [])

    # Folder search finds the folder's pictures
    (pics / "terminal-dark.png").write_bytes(b"\x89PNG\r\n\x1a\n" + b"0" * 16)
    run(env, "config", json.dumps({"dirs": [str(pics)]}))
    hits = run(env, "search", "terminal").get("folders", [])
    check("folder search finds terminal-dark.png", any(h["label"] == "terminal-dark" for h in hits))

    # Reset drops the override and the now-unused copy
    run(env, "reset", "org.gnome.Nautilus")
    check("reset removes the line", "org.gnome.Nautilus" not in overrides.read_text())
    check("reset prunes the unused copy", not any(store.iterdir()))

    run(env, "set", "zen", "file:" + str(svg))
    run(env, "reset-all")
    check("reset-all clears every override", all(not l or l.startswith("#") for l in overrides.read_text().splitlines()))

sys.exit(1 if fails else 0)
