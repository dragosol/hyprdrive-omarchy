#!/usr/bin/env python3
"""The config fragments' raw-snippet contract, in both of its copies
(scripts/fragment.py and omarchy-plugin/bin/hdpanel's compose_fragment), the
panel's one-time gesture migration, and uninstall's require() removal -- all in
a throwaway HOME (review, 2026-11-09: each of these lost something a person had
written or chosen, silently)."""
import contextlib
import importlib.machinery
import importlib.util
import io
import json
import os
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
failed = checks = 0


def expect(ok, what):
    global failed, checks
    checks += 1
    if not ok:
        failed += 1
        print("FAIL", what)


def load(name, path):
    loader = importlib.machinery.SourceFileLoader(name, str(path))
    spec = importlib.util.spec_from_loader(name, loader)
    m = importlib.util.module_from_spec(spec)
    loader.exec_module(m)
    return m


with tempfile.TemporaryDirectory() as tmp:
    home = pathlib.Path(tmp)
    os.environ["HOME"] = str(home)
    os.environ["XDG_CONFIG_HOME"] = str(home / ".config")
    os.environ["XDG_STATE_HOME"] = str(home / ".local/state")
    frags = home / ".config/hypr/hyprdrive"
    frags.mkdir(parents=True)
    os.environ["HYPRDRIVE_FRAGMENTS"] = str(frags)
    fr = load("fragment", ROOT / "scripts/fragment.py")
    hd = load("hdpanel", ROOT / "omarchy-plugin/bin/hdpanel")
    hd.hyprctl = lambda *a, **k: (0, "")  # never the live session

    B, E = fr.BEGIN, fr.END
    cases = {
        "intact": ("-- machine\n\n" + B + "\nRAW = 1\n" + E + "\n", "RAW = 1"),
        "no END": ("-- machine\n\n" + B + "\nRAW = 1\nMORE = 2\n", "RAW = 1\nMORE = 2"),
        "after END": ("-- machine\n\n" + B + "\nRAW = 1\n" + E + "\nTAIL = 3\n", "RAW = 1\nTAIL = 3"),
        "blank after END": ("-- machine\n\n" + B + "\nRAW = 1\n" + E + "\n\n\n", "RAW = 1"),
        "no markers": ("-- all machine\n", ""),
    }
    def split_raw(text):
        try:
            return fr.split(text)[1]
        except SystemExit as e:  # what 1.9.57 did on a missing END
            return "SystemExit: %s" % e
    for name, (text, raw) in cases.items():
        expect(split_raw(text) == raw, "fragment.split keeps the raw block: " + name)
        f = frags / ("case-%s.lua" % name.replace(" ", "-"))
        f.write_text(text)
        expect(hd.compose_fragment("-- machine v2\n", f) == fr.compose("-- machine v2\n", raw),
               "hdpanel's compose_fragment keeps the raw block too: " + name)

    # fragment.py write, as make install runs it, on a file whose END is gone
    f = frags / "10-theme.lua"
    f.write_text("-- machine\n\n" + B + "\nMINE = 1\n")
    sys.stdin = io.StringIO("-- machine v2\n")
    try:
        with contextlib.redirect_stdout(io.StringIO()):
            rc = fr.main(["fragment.py", "write", str(f)])
    except SystemExit as e:
        rc = "SystemExit: %s" % e
    t = f.read_text()
    expect(rc == 0 and "MINE = 1" in t and t.rstrip("\n").endswith(E) and t.startswith("-- machine v2"),
           "fragment.py write keeps a raw block whose END is gone, and closes it (rc %r)" % (rc,))
    f.write_text("-- machine\n\n" + B + "\nMINE = 1\n")
    with contextlib.redirect_stderr(io.StringIO()) as err:
        try:
            rc = fr.main(["fragment.py", "check", str(f)])
        except SystemExit as e:
            rc = "SystemExit: %s" % e
    expect(rc == 1 and str(f) in err.getvalue(), "fragment.py check still reports the missing END, naming the file")

    # the gesture migration on a fresh store, then a map saved afterwards
    hd.migrate_gestures()
    s = hd.load_store()
    expect("canvas_zoom_down" in s.get("migrations", []), "migrate_gestures marks itself done when no map is saved")
    s["gestures"] = [{"fingers": 3, "direction": "down", "action": "overview"}]
    hd.save_store(s)
    hd.migrate_gestures()
    g = hd.load_store()["gestures"][0]
    expect(g["action"] == "overview", "a 3-finger down saved as overview afterwards stays overview (got %r)" % g["action"])

    # a saved map from before the migration still moves, once
    s = hd.load_store()
    s["migrations"] = [m for m in s.get("migrations", []) if m != "canvas_zoom_down"]
    hd.save_store(s)
    hd.migrate_gestures()
    expect(hd.load_store()["gestures"][0]["action"] == "canvas_zoom", "an old saved overview map is still migrated once")

    # uninstall: a single-quoted, indented require, in a hyprland.lua that is a symlink
    dots = home / "dotfiles"
    dots.mkdir()
    real = dots / "hyprland.lua"
    real.write_text("-- mine\n  require('hypr.hyprdrive')\nother = 1\n")
    link = home / ".config/hypr/hyprland.lua"
    link.symlink_to(real)
    env = dict(os.environ, PREFIX=str(home / ".local"))
    r = subprocess.run(["bash", str(ROOT / "scripts/uninstall.sh")], env=env, capture_output=True, text=True)
    after = real.read_text()
    expect(r.returncode == 0, "uninstall.sh ran (%s)" % r.stderr.strip()[-200:])
    expect("hyprdrive" not in after and "other = 1" in after and "-- mine" in after,
           "uninstall removes a single-quoted, indented require and keeps the rest (%r)" % after)
    expect(link.is_symlink() and link.resolve() == real.resolve(), "uninstall keeps a symlinked hyprland.lua a symlink")
    bak = link.parent / "hyprland.lua.hyprdrive-bak"
    expect(bak.exists() and "require('hypr.hyprdrive')" in bak.read_text(),
           "uninstall keeps a backup of the original beside the hyprland.lua it was given")

# misc:size_limits_tiled clamps EVERY tiled window's position into the screen's
# work area, not only the size of windows with size rules: 20-layout.lua set it
# for Super+T's own-size tiles on 2026-11-10 and it piled the infinite canvas's
# windows on top of each other (user: "something is wrong with infinite canvas
# ... you broke it"). OwnSizeTile::place does that job for those windows alone.
shipped = sorted((ROOT / "config").rglob("*.lua")) + [ROOT / "omarchy-plugin" / "bin" / "hdpanel"]
setters = [str(p.relative_to(ROOT)) for p in shipped if p.exists() and "size_limits_tiled" in p.read_text()]
expect(not setters, "no shipped config turns on misc:size_limits_tiled (%s)" % setters)

print("fragment_test: %d checks, %d failed" % (checks, failed))
sys.exit(1 if failed else 0)
