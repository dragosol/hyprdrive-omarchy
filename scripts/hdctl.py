#!/usr/bin/env python3
"""The settings app's whole back end. One durable write path, and this is it.

WHY A CLI AND NOT QML. The app is QML and QML must not learn the fragment
contract: the moment it can compose a fragment itself there are two
implementations of "how a value becomes config", and the second one is the one
that loses somebody's raw block. So the app draws rows and this file writes
files -- every durable write in the product goes through `hdctl set`, which goes
through scripts/fragment.py, which is the single implementation of the marker
contract. It is also why this is testable without a display: every round-trip
gate in scripts/gates/ drives THIS, not the QML.

PREVIEW IS NOT A WRITE. `hdctl preview` is `hyprctl keyword` and it is
deliberately not durable, because Phase 10 measured both reasons it cannot be:
a live keyword is discarded by the next `hyprctl reload` (theme-sync issues one
on every wallpaper change), and it lands AFTER the config parse, so it would beat
a pin -- and pin > derived > default is the only precedence the fragment layer
has. A slider drag is therefore preview; letting go is `set`.

WHERE VALUES LIVE. Two places, on purpose:

    ~/.local/state/hyprdrive/settings.json   what the APP owns, per pane.
    the fragment files themselves            what the SESSION reads.

The second is generated from the first, and the displayed value comes from
neither -- it comes from `hyprctl getoption`, the live truth, which already
accounts for the raw block, for a pin in some other fragment, and for a value
the user typed by hand. The store answers a different question: is this key
OURS. That is the same question as "is it pinned", which is why the pin toggle in
the Dock pane is `set`/`unset` and not a new mechanism.

    hdctl.py schema                 the schema, with live values merged in
    hdctl.py get KEY...             live values only
    hdctl.py preview KEY VALUE      hyprctl keyword -- instant, temporary
    hdctl.py set KEY VALUE          durable: whatever this row's apply says
    hdctl.py bulk KEY=VALUE ...     many rows at once, behind one auto-snapshot
    hdctl.py unset KEY              stop owning it (back to derived/default)
    hdctl.py raw PANE               the pane fragment's raw block
    hdctl.py raw-write PANE         stdin -> that raw block, then reload
    hdctl.py ii-set PATH VALUE      illogical-impulse's config.json + qs restart
    hdctl.py wallpaper PATH         the rice's rotation (regenerates the palette)
    hdctl.py reload                 apply whatever is on disk

ONE WRITE VERB, WHATEVER THE ROW IS (Phase 12). `set` used to mean "write a
fragment" and refused every curated row, which made "an option is one schema row"
true for two of the four sources and false for the other two -- the Quickshell
pane's own switches could not be written by the row that drew them. It now
dispatches on the row's `apply` strategy: reload -> fragment, qs-restart -> ii's
config.json, wallpaper -> the rotation, none -> the app's own store. The UI does
not branch on source, and neither does the importer, and neither does a profile
restore. That single verb IS the extension contract docs/EXTENDING.md documents.
"""
import argparse
import json
import os
import pathlib
import re
import subprocess
import sys

from fragment import replace_file  # noqa: E402  -- the one atomic, mode-keeping writer

ROOT = pathlib.Path(__file__).resolve().parent.parent
FRAGMENT = ROOT / "scripts" / "fragment.py"
SCHEMA = ROOT / "docs" / "schema.json"

STATE = pathlib.Path(os.environ.get("XDG_STATE_HOME", os.path.expanduser("~/.local/state"))) / "hyprdrive"
STORE = STATE / "settings.json"
FRAGDIR = pathlib.Path(os.environ.get("HYPRDRIVE_FRAGMENTS", os.path.expanduser("~/.config/hypr/hyprdrive")))
IICONF = pathlib.Path(os.path.expanduser("~/.config/illogical-impulse/config.json"))

PLUGIN_PREFIX = "plugin:hyprdrive:"

# The one fragment this tool may never write. It is machine-derived from the
# wallpaper's palette by theme-sync, and a settings app that regenerates it would
# be overwriting the theme with a snapshot of the theme. To override a derived
# value you PIN it -- which means writing it in a pane fragment, all of which
# sort after 05, so the pin wins by load order. That is `set` on a derived key.
DERIVED_FRAGMENT = "05-theme-derived.lua"


# ---------------------------------------------------------------- compositor --
def _env():
    env = dict(os.environ)
    run = env.get("XDG_RUNTIME_DIR") or "/run/user/%d" % os.getuid()
    sig = env.get("HYPRLAND_INSTANCE_SIGNATURE")
    # An inherited signature is a hint, not an answer [theme-sync.py:126].
    if not sig or not (pathlib.Path(run) / "hypr" / sig / ".socket.sock").exists():
        d = pathlib.Path(run) / "hypr"
        sigs = sorted((p for p in d.glob("*") if (p / ".socket.sock").exists()),
                      key=lambda p: p.stat().st_mtime, reverse=True) if d.is_dir() else []
        if not sigs:
            return None
        env["HYPRLAND_INSTANCE_SIGNATURE"] = sigs[0].name
    env["XDG_RUNTIME_DIR"] = run
    return env


def hyprctl(*args, check=False):
    env = _env()
    if env is None:
        return None
    try:
        r = subprocess.run(["hyprctl", *args], capture_output=True, text=True, timeout=20, env=env)
    except (OSError, subprocess.SubprocessError):
        return None
    if check and r.returncode != 0:
        return None
    return r.stdout


def getoption(key):
    """Live value of one option, typed. None when there is no compositor."""
    out = hyprctl("getoption", key, "-j")
    if not out:
        return None
    try:
        d = json.loads(out)
    except ValueError:
        return None
    # 0.56 answers with exactly one typed field: bool/int/float/str/gradient/
    # vec2/custom. `set` says whether anything ever assigned it. Take the first
    # typed field that is present -- False and 0 are values, so this cannot test
    # truthiness.
    for k in ("bool", "int", "float", "str", "gradient", "vec2", "css", "custom", "data"):
        if k in d:
            return d[k]
    return None


# --------------------------------------------------------------------- store --
def load_store():
    if STORE.exists():
        try:
            return json.loads(STORE.read_text())
        except ValueError:
            pass
    return {"panes": {}}


def atomic_json(path, obj):
    """Written AND fsynced before the caller changes anything.

    Lifted out of hdmon.py in Phase 12 because hdprof needs the same guarantee
    for the same reason, and two implementations of "this record survives the
    power going out" is one implementation and one hope. A revert record still in
    the page cache when the machine goes down is not a revert record."""
    path.parent.mkdir(parents=True, exist_ok=True)
    replace_file(path, json.dumps(obj, indent=1), fsync=True)
    d = os.open(str(path.parent), os.O_RDONLY)
    try:
        os.fsync(d)
    finally:
        os.close(d)


def flock_or_none(path):
    """One restore, whoever gets there first. flock, so a killed holder releases."""
    import fcntl
    path.parent.mkdir(parents=True, exist_ok=True)
    fh = path.open("w")
    try:
        fcntl.flock(fh, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        fh.close()
        return None
    return fh


def save_store(s):
    STATE.mkdir(parents=True, exist_ok=True)
    replace_file(STORE, json.dumps(s, indent=1, sort_keys=True) + "\n")


def schema():
    return json.loads(SCHEMA.read_text())


def entry_for(sch, key):
    for e in sch["entries"]:
        if e["key"] == key:
            return e
    return None


def pane_of(sch, key):
    e = entry_for(sch, key)
    return e["pane"] if e else None


def ii_get(path):
    """A dotted path into illogical-impulse's config.json."""
    try:
        cur = json.loads(IICONF.read_text())
    except (OSError, ValueError):
        return None
    for part in path.split("."):
        if not isinstance(cur, dict) or part not in cur:
            return None
        cur = cur[part]
    return cur


def curated_current(e):
    """The live value of a row that is not a compositor option.

    Three kinds and each one is read where it actually lives, never from the
    store: the store says what the app OWNS, and a row has to show what the
    system HAS -- they differ the moment anything else writes the file."""
    key = e["key"]
    if key.startswith("ii:"):
        return ii_get(key[3:])
    if key == "wallpaper.path":
        return ii_get("background.wallpaperPath")
    return load_store()["panes"].get(e["pane"], {}).get(key, e.get("default"))


def fragment_of(sch, pane):
    for p in sch["panes"]:
        if p["id"] == pane:
            return p["fragment"]
    return None


# ------------------------------------------------------------ lua rendering --
def lua_value(v, kind):
    if kind == "bool":
        if isinstance(v, str):
            v = v.strip().lower() in ("1", "true", "yes", "on")
        return "true" if v else "false"
    if kind in ("int", "float"):
        return str(v)
    if kind == "vec2":
        if isinstance(v, (list, tuple)):
            return "{%s}" % ", ".join(str(x) for x in v)
        return "{%s}" % str(v).strip("{}")
    if kind.startswith("color"):
        s = str(v).strip()
        # 0xAARRGGBB stays a number; rgba(...)/rgb(...) and anything else is the
        # string form Hyprland's own parser takes.
        return s if re.match(r"^0[xX][0-9a-fA-F]+$", s) else '"%s"' % s
    s = str(v)
    # Long brackets: no escaping rules to get wrong, and the fragments already
    # use them for every path and selector they carry.
    return "[[%s]]" % s if "]]" not in s else '"%s"' % s.replace("\\", "\\\\").replace('"', '\\"')


def nest(pairs):
    """[('decoration:blur:size', '8')] -> {'decoration': {'blur': {'size': '8'}}}"""
    tree = {}
    for path, rendered in pairs:
        cur = tree
        parts = path.split(":")
        for p in parts[:-1]:
            cur = cur.setdefault(p, {})
        cur[parts[-1]] = rendered
    return tree


def render_tree(tree, indent):
    out = []
    pad = " " * indent
    for k in sorted(tree):
        v = tree[k]
        if isinstance(v, dict):
            out.append("%s%s = {" % (pad, k))
            out += render_tree(v, indent + 4)
            out.append("%s}," % pad)
        else:
            out.append("%s%s = %s," % (pad, k, v))
    return out


def render_fragment(sch, pane, values):
    """The machine half of a pane's fragment, from the values the app owns."""
    p = next(x for x in sch["panes"] if x["id"] == pane)
    plugin_pairs, hypr_pairs = [], []
    for key, val in sorted(values.items()):
        e = entry_for(sch, key)
        kind = e["type"] if e else "string"
        rendered = lua_value(val, kind)
        if key.startswith(PLUGIN_PREFIX):
            plugin_pairs.append((key[len(PLUGIN_PREFIX):], rendered))
        else:
            hypr_pairs.append((key, rendered))

    L = ["-- hyprdrive fragment %s -- the %s pane of the settings app." % (p["fragment"][:-4], p["title"]),
         "--",
         "-- MACHINE-OWNED above the raw-snippet marker; USER-OWNED below it. This half",
         "-- is regenerated wholesale from ~/.local/state/hyprdrive/settings.json every",
         "-- time the app commits a value, so do not hand-edit it -- type below the",
         "-- BEGIN line instead, where Lua's last-assignment-wins makes your line the",
         "-- one that takes effect.",
         "--",
         "-- %s" % p["blurb"],
         "--",
         "-- It sorts after 05-theme-derived.lua, so anything set here PINS the derived",
         "-- value: to go back to inheriting the wallpaper's palette, unset the row in",
         "-- the app and this file stops carrying the key.",
         ""]

    if not plugin_pairs and not hypr_pairs:
        L.append("-- (nothing set on this pane yet)")
        return "\n".join(L) + "\n"

    if hypr_pairs:
        L.append("hl.config({")
        L += render_tree(nest(hypr_pairs), 4)
        L.append("})")
        L.append("")
    if plugin_pairs:
        # The guard: plugin:hyprdrive:* does not exist until the .so has
        # registered it, and an unguarded block throws on every boot [00-plugin].
        L.append("if hyprdrive_loaded() then")
        L.append("    hl.config({")
        L.append("        plugin = {")
        L.append("            hyprdrive = {")
        L += render_tree(nest(plugin_pairs), 16)
        L.append("            },")
        L.append("        },")
        L.append("    })")
        L.append("end")
    return "\n".join(L) + "\n"


def write_fragment(sch, pane, quiet=False):
    frag = fragment_of(sch, pane)
    if not frag:
        raise SystemExit("hdctl: pane %s is not fragment-backed" % pane)
    if frag == DERIVED_FRAGMENT:
        raise SystemExit("hdctl: refusing to write %s -- it is theme-sync's" % frag)
    body = render_fragment(sch, pane, load_store()["panes"].get(pane, {}))
    r = subprocess.run([sys.executable, str(FRAGMENT), "write", str(FRAGDIR / frag)],
                       input=body, text=True, capture_output=True)
    if r.returncode != 0:
        sys.stderr.write(r.stderr)
        raise SystemExit(1)
    if not quiet:
        print(r.stdout.strip())
    return FRAGDIR / frag


def reload_compositor():
    ok = hyprctl("reload") is not None
    print("hdctl: %s" % ("reloaded" if ok else "no compositor -- the fragment is on disk and applies at next parse"))
    return ok


# ------------------------------------------------------------------ commands --
def cmd_schema(a):
    sch = schema()
    if a.live:
        for e in sch["entries"]:
            if e["source"] in ("plugin", "hyprland"):
                e["current"] = getoption(e["key"])
            else:
                e["current"] = curated_current(e)
        store = load_store()["panes"]
        for e in sch["entries"]:
            e["owned"] = e["key"] in store.get(e["pane"], {})
    json.dump(sch, sys.stdout, indent=1)
    print()
    return 0


def cmd_get(a):
    for key in a.keys:
        print("%s = %s" % (key, getoption(key)))
    return 0


def cmd_preview(a):
    """Instant, and gone at the next reload. That is the point.

    NOT `hyprctl keyword`: this session's config is Lua, and 0.56 answers a
    keyword with "keyword can't work with non-legacy parsers. Use eval."
    (measured 2026-08-12). So a preview is the same one-shot `hl.config` write
    the gates' cfg_dock already uses [lib/rig.sh:121] -- which lands after the
    parse, exactly like keyword would have, and is discarded by the next reload
    for exactly the same reason. `hyprctl eval` never returns a value, so the
    read-back is a separate getoption rather than this call's output."""
    sch = schema()
    e = entry_for(sch, a.key)
    kind = e["type"] if e else "string"
    rendered = lua_value(a.value, kind)
    if a.key.startswith(PLUGIN_PREFIX):
        tree = nest([("plugin:hyprdrive:" + a.key[len(PLUGIN_PREFIX):], rendered)])
    else:
        tree = nest([(a.key, rendered)])
    body = "hl.config({\n" + "\n".join(render_tree(tree, 4)) + "\n}) return 0"
    if hyprctl("eval", body) is None:
        print("hdctl: no compositor", file=sys.stderr)
        return 1
    print("preview %s = %s (live only -- the next reload discards it)" % (a.key, getoption(a.key)))
    return 0


def theme_follows(sch):
    """Does a wallpaper change re-derive the palette?

    The wallpaper.theme ROW answers it -- the store if anybody set it, the row's
    own default otherwise. The first cut read the default off whichever row was
    being written, which for wallpaper.path is the empty string; it came out True
    by accident rather than by asking."""
    e = entry_for(sch, "wallpaper.theme")
    if e is None:
        return True
    v = load_store()["panes"].get(e["pane"], {}).get("wallpaper.theme", e.get("default", True))
    return str(v).strip().lower() not in ("false", "0", "no", "off")


def set_one(sch, key, value, force=False):
    """Write one row, by whatever mechanism its apply-strategy names.

    Returns the strategy used, so a bulk caller can do the expensive part
    (a reload, a shell restart) ONCE at the end instead of per row."""
    e = entry_for(sch, key)
    if e is None:
        raise SystemExit("hdctl: %s is not in the schema (run scripts/gen_schema.py)" % key)
    if e.get("locked") and not force:
        raise SystemExit("hdctl: %s is locked -- %s" % (key, e["locked"]))

    how = e.get("apply", "reload")
    store = load_store()
    store["panes"].setdefault(e["pane"], {})[key] = value
    save_store(store)

    if how == "qs-restart":
        # ii's own file. The key carries the `ii:` marker so that one glance at a
        # schema row says which config it lands in.
        ii_write(key[3:] if key.startswith("ii:") else key, value)
    elif how == "wallpaper":
        rotate(value, theme=theme_follows(sch))
    elif how == "none":
        pass          # the store IS where this one lives (slideshow interval, ...)
    else:
        write_fragment(sch, e["pane"], quiet=True)
    return how


def cmd_set(a):
    sch = schema()
    how = set_one(sch, a.key, a.value, force=a.force)
    print("hdctl: %s = %s (%s)" % (a.key, a.value, how))
    if not a.no_apply and how == "reload":
        reload_compositor()
    return 0


def cmd_bulk(a):
    """Many rows, one snapshot, one reload. THE second auto-snapshot site.

    A bulk apply is where a settings app stops being undoable by memory: after
    twelve values land in one go, "put it back" is not something a person can do
    from the UI they just used. So it is bracketed exactly like an import --
    snapshot before, pending record on disk, finish after."""
    import hdprof
    sch = schema()
    pairs = []
    for item in a.pairs:
        if "=" not in item:
            raise SystemExit("hdctl: bulk takes KEY=VALUE, got %r" % item)
        k, _, v = item.partition("=")
        pairs.append((k, v))
    ref = hdprof.snapshot_before(a.reason or "bulk apply of %d values" % len(pairs))
    print("hdctl: snapshot %s taken before %d writes" % (ref, len(pairs)))
    used = set()
    for k, v in pairs:
        used.add(set_one(sch, k, v, force=a.force))
        print("    %s = %s" % (k, v))
    if not a.no_apply:
        if "reload" in used:
            reload_compositor()
        if "qs-restart" in used:
            hdprof.qs_restart()
    hdprof.finish()
    return 0


def cmd_unset(a):
    sch = schema()
    e = entry_for(sch, a.key)
    if e is None:
        raise SystemExit("hdctl: %s is not in the schema" % a.key)
    store = load_store()
    if store["panes"].get(e["pane"], {}).pop(a.key, None) is None:
        print("hdctl: %s was not owned by the app" % a.key)
        return 0
    save_store(store)
    write_fragment(sch, e["pane"])
    if not a.no_apply:
        reload_compositor()
    return 0


def cmd_owned(a):
    store = load_store()["panes"]
    for pane in sorted(store):
        for k in sorted(store[pane]):
            print("%-12s %s = %s" % (pane, k, store[pane][k]))
    return 0


def _frag_path(pane):
    sch = schema()
    frag = fragment_of(sch, pane)
    if not frag:
        raise SystemExit("hdctl: pane %s is not fragment-backed" % pane)
    return FRAGDIR / frag


def cmd_raw(a):
    p = _frag_path(a.pane)
    if not p.exists():
        return 0
    r = subprocess.run([sys.executable, str(FRAGMENT), "raw", str(p)], capture_output=True, text=True)
    sys.stdout.write(r.stdout)
    return r.returncode


def cmd_raw_write(a):
    p = _frag_path(a.pane)
    body = sys.stdin.read()
    r = subprocess.run([sys.executable, str(FRAGMENT), "raw-write", str(p)],
                       input=body, text=True, capture_output=True)
    if r.returncode != 0:
        sys.stderr.write(r.stderr)
        return 1
    print(r.stdout.strip())
    if not a.no_apply:
        reload_compositor()
    return 0


def ii_write(path, value):
    """illogical-impulse's own config.json. Not a fragment: quickshell owns it,
    and quickshell reloads itself when it changes -- no compositor involved.
    The value is coerced to the type the file already has, because json is typed
    and a settings row hands over a string."""
    d = json.loads(IICONF.read_text())
    cur = d
    parts = path.split(".")
    for p in parts[:-1]:
        cur = cur.setdefault(p, {})
    old = cur.get(parts[-1])
    val = value
    if isinstance(old, bool):
        val = str(val).strip().lower() in ("1", "true", "yes", "on")
    elif isinstance(old, int) and not isinstance(old, bool):
        val = int(float(val))
    elif isinstance(old, float):
        val = float(val)
    cur[parts[-1]] = val
    replace_file(IICONF, json.dumps(d, indent=2) + "\n")
    print("ii: %s = %s (was %s)" % (path, val, old))
    return val


def rotate(path, theme=True):
    """The rice's own rotation script, which is also the wallpaper pane's write
    path. THEME=1 is what makes the palette -- and every accent derived from it
    -- follow the picture (Phase 10). The default rotation stays palette-neutral;
    a swap a person asked for does not."""
    env = _env() or dict(os.environ)
    env["WALL"] = str(path)
    env["THEME"] = "1" if theme else "0"
    r = subprocess.run(["bash", str(ROOT / "scripts" / "hyprwall-rotate.sh")],
                       env=env, capture_output=True, text=True)
    sys.stdout.write(r.stdout)
    sys.stderr.write(r.stderr)
    return r.returncode


def cmd_ii_set(a):
    ii_write(a.path, a.value)
    return 0


def cmd_wallpaper(a):
    return rotate(a.path, theme=a.theme)


def cmd_reload(a):
    return 0 if reload_compositor() else 1


def main(argv):
    ap = argparse.ArgumentParser(description="hyprdrive settings back end")
    sub = ap.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser("schema"); s.add_argument("--live", action="store_true"); s.set_defaults(fn=cmd_schema)
    s = sub.add_parser("get"); s.add_argument("keys", nargs="+"); s.set_defaults(fn=cmd_get)
    s = sub.add_parser("preview"); s.add_argument("key"); s.add_argument("value"); s.set_defaults(fn=cmd_preview)
    s = sub.add_parser("set"); s.add_argument("key"); s.add_argument("value")
    s.add_argument("--force", action="store_true", help="write a locked key anyway")
    s.add_argument("--no-apply", action="store_true"); s.set_defaults(fn=cmd_set)
    s = sub.add_parser("unset"); s.add_argument("key")
    s.add_argument("--no-apply", action="store_true"); s.set_defaults(fn=cmd_unset)
    s = sub.add_parser("bulk"); s.add_argument("pairs", nargs="+", metavar="KEY=VALUE")
    s.add_argument("--reason"); s.add_argument("--force", action="store_true")
    s.add_argument("--no-apply", action="store_true"); s.set_defaults(fn=cmd_bulk)
    s = sub.add_parser("owned"); s.set_defaults(fn=cmd_owned)
    s = sub.add_parser("raw"); s.add_argument("pane"); s.set_defaults(fn=cmd_raw)
    s = sub.add_parser("raw-write"); s.add_argument("pane")
    s.add_argument("--no-apply", action="store_true"); s.set_defaults(fn=cmd_raw_write)
    s = sub.add_parser("ii-set"); s.add_argument("path"); s.add_argument("value"); s.set_defaults(fn=cmd_ii_set)
    s = sub.add_parser("wallpaper"); s.add_argument("path")
    s.add_argument("--theme", action="store_true"); s.set_defaults(fn=cmd_wallpaper)
    s = sub.add_parser("reload"); s.set_defaults(fn=cmd_reload)

    a = ap.parse_args(argv[1:])
    return a.fn(a)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
