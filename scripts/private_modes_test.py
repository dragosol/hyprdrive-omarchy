#!/usr/bin/env python3
"""A private file stays private through every writer (marketplace review, #11012).

Under umask 022, a 0600 file rewritten by fragment.py, or backed up by
uninstall.sh, must come out 0600 -- not as a default-permission 0644 copy. Runs
against a throwaway HOME; touches nothing real. Part of `make check`.
"""
import os
import pathlib
import stat
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
os.umask(0o022)
fails = []


def mode(p):
    return stat.S_IMODE(os.stat(p).st_mode)


def expect(what, p, want):
    got = mode(p)
    print("%-44s %04o %s" % (what, got, "ok" if got == want else "WANT %04o" % want))
    if got != want:
        fails.append(what)


with tempfile.TemporaryDirectory() as home:
    env = dict(os.environ, HOME=home, XDG_CONFIG_HOME=home + "/.config")
    hypr = pathlib.Path(home, ".config/hypr")
    (hypr / "hyprdrive").mkdir(parents=True)

    # fragment.py write over an existing 0600 fragment
    frag = hypr / "hyprdrive/50-test.lua"
    frag.write_text("-- old\n")
    os.chmod(frag, 0o600)
    subprocess.run([sys.executable, ROOT / "scripts/fragment.py", "write", frag],
                   input="-- new\n", text=True, check=True, env=env, capture_output=True)
    expect("fragment.py write keeps 0600", frag, 0o600)

    # ... and a new fragment is created owner-only
    new = hypr / "hyprdrive/60-new.lua"
    subprocess.run([sys.executable, ROOT / "scripts/fragment.py", "write", new],
                   input="-- new\n", text=True, check=True, env=env, capture_output=True)
    expect("fragment.py write, new file, is 0600", new, 0o600)

    # ... and an existing 0644 one is not narrowed behind the user's back
    pub = hypr / "hyprdrive/70-public.lua"
    pub.write_text("-- old\n")
    os.chmod(pub, 0o644)
    subprocess.run([sys.executable, ROOT / "scripts/fragment.py", "write", pub],
                   input="-- new\n", text=True, check=True, env=env, capture_output=True)
    expect("fragment.py write keeps an existing 0644", pub, 0o644)

    # uninstall.sh: the hyprland.lua backup takes hyprland.lua's mode
    lua = hypr / "hyprland.lua"
    lua.write_text('-- mine\n\n-- Hyprdrive (overview + dock). Fragments live in hypr/hyprdrive/.\n'
                   'require("hypr.hyprdrive")\n')
    os.chmod(lua, 0o600)
    subprocess.run(["bash", ROOT / "scripts/uninstall.sh"], check=True, env=env, capture_output=True)
    bak = pathlib.Path(str(lua) + ".hyprdrive-bak")
    expect("uninstall.sh hyprland.lua stays 0600", lua, 0o600)
    expect("uninstall.sh backup is 0600", bak, 0o600)
    if 'require("hypr.hyprdrive")' in lua.read_text():
        print("uninstall.sh left the require in hyprland.lua")
        fails.append("uninstall require")

    # The settings panel's back end (omarchy-plugin/bin/hdpanel, its own copy of
    # the writer): panel.json and a fragment, both already 0600.
    import importlib.machinery, importlib.util
    os.environ["XDG_STATE_HOME"] = home + "/.local/state"
    os.environ["HYPRDRIVE_FRAGMENTS"] = str(hypr / "hyprdrive")
    loader = importlib.machinery.SourceFileLoader("hdpanel", str(ROOT / "omarchy-plugin/bin/hdpanel"))
    spec = importlib.util.spec_from_loader("hdpanel", loader)
    hdpanel = importlib.util.module_from_spec(spec)
    loader.exec_module(hdpanel)
    hdpanel.save_store({"keys": {}})
    expect("hdpanel panel.json, new, is 0600", hdpanel.STORE, 0o600)
    os.chmod(hdpanel.STORE, 0o600)
    hdpanel.save_store({"keys": {}, "gestures": [{"args": "private"}]})
    expect("hdpanel panel.json keeps 0600", hdpanel.STORE, 0o600)
    (hypr / "hyprdrive").mkdir(exist_ok=True)  # uninstall above moved it aside
    pfrag = hypr / "hyprdrive/40-panel.lua"
    pfrag.write_text("-- old\n")
    os.chmod(pfrag, 0o600)
    hdpanel.write_fragment(pfrag, "-- new\n")
    expect("hdpanel fragment keeps 0600", pfrag, 0o600)

    # The wallpaper rotation rewrites illogical-impulse's whole config.json.
    # Stand-ins first in PATH: the script may pkill mpvpaper and must not touch
    # the real desktop from a test.
    shims = pathlib.Path(home, "shims"); shims.mkdir()
    for tool in ("pkill", "hyprctl", "mpvpaper", "matugen"):
        (shims / tool).write_text("#!/bin/sh\nexit 0\n"); os.chmod(shims / tool, 0o755)
    walls = pathlib.Path(home, "walls"); walls.mkdir()
    (walls / "a.jpg").write_bytes(b"x"); (walls / "b.jpg").write_bytes(b"x")
    ii = pathlib.Path(home, ".config/illogical-impulse"); ii.mkdir(parents=True)
    cfg = ii / "config.json"
    cfg.write_text('{"background": {"wallpaperPath": "%s"}}\n' % (walls / "a.jpg"))
    os.chmod(cfg, 0o600)
    wenv = dict(env, PATH="%s:%s" % (shims, os.environ["PATH"]), DIR=str(walls), WALL=str(walls / "b.jpg"))
    subprocess.run(["bash", ROOT / "scripts/hyprwall-rotate.sh"], check=True, env=wenv, capture_output=True)
    expect("hyprwall-rotate.sh config.json keeps 0600", cfg, 0o600)
    if "b.jpg" not in cfg.read_text():
        print("hyprwall-rotate.sh did not write the new wallpaper")
        fails.append("rotate write")

sys.exit(1 if fails else 0)
