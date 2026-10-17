#!/usr/bin/env python3
"""The Keybinds section's chord editor, checked without a compositor.

  - hdpanel: chords parse from every accepted spelling and format one way;
    a typing key without Super/Ctrl/Alt and mouse binds are refused;
  - bind-set ID CHORD stores the chord, turns the row on, writes it into
    40-panel.lua (action built before its key is unbound) and reloads;
    on/off still work; "default" and "Restore defaults" bring the default back;
  - bind-check finds who holds a chord in `hyprctl binds -j` -- someone
    else's bind, hyprdrive's own row (which --replace turns off), Omarchy's
    code:N number-row binds that hyprctl reports with no key;
  - capture on/off parks Hyprland in the capture submap and leaves it;
  - Model.js (node): a Qt key event becomes the same chord hdpanel parses,
    and chords read as keycaps.
Touches nothing real: a temp state dir and fragment dir, a fake hyprctl.
Part of `make check`.
"""
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
from importlib.machinery import SourceFileLoader

ROOT = pathlib.Path(__file__).resolve().parent.parent
fails = 0


def check(name, ok, detail=""):
    global fails
    print("%-62s %s" % (name, "ok" if ok else "FAIL " + str(detail)))
    if not ok:
        fails += 1


tmp = pathlib.Path(tempfile.mkdtemp())
os.environ["XDG_STATE_HOME"] = str(tmp / "state")
os.environ["HYPRDRIVE_FRAGMENTS"] = str(tmp / "frag")
os.environ["XDG_RUNTIME_DIR"] = str(tmp / "run")
os.environ["HYPRLAND_INSTANCE_SIGNATURE"] = "none"
hd = SourceFileLoader("hdpanel_kb", str(ROOT / "omarchy-plugin" / "bin" / "hdpanel")).load_module()

# A fake compositor: hyprctl answers from these, and records what it is told.
BINDS = []
SUBMAP = ["default"]
CALLS = []


def fake_hyprctl(*args):
    CALLS.append(args)
    if args[:2] == ("binds", "-j"):
        return json.dumps(BINDS)
    if args == ("submap",):
        return SUBMAP[0] + "\n"
    if args and args[0] == "--batch":
        return ""
    return "ok"


def fake_eval(code):
    CALLS.append(("eval", code))
    if "hl.dsp.submap('reset')" in code:
        SUBMAP[0] = "default"
    elif "hl.dsp.submap(" in code:
        SUBMAP[0] = hd.CAPTURE_SUBMAP
    return True


hd.hyprctl = fake_hyprctl
hd.eval_lua = fake_eval
hd.apply_live_many = lambda pairs, *a: True
hd.LAYOUT_CYCLE = pathlib.Path("/x/hd-layout-cycle")


def run(fn, args):
    """A command's JSON answer."""
    import io
    import contextlib
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        fn(args)
    return json.loads(buf.getvalue())


def frag():
    return (tmp / "frag" / "40-panel.lua").read_text()


def bind(mask, key, desc, keycode=0, submap=""):
    return {"modmask": mask, "key": key, "keycode": keycode, "submap": submap, "description": desc,
            "dispatcher": "__lua", "arg": "1", "mouse": key.startswith("mouse"), "catch_all": False}


# ------------------------------------------------------------ parse / format
for text, want in [
    ("SUPER + SHIFT + K", "SUPER + SHIFT + K"),
    ("SUPER SHIFT, K", "SUPER + SHIFT + K"),
    ("super+shift+k", "SUPER + SHIFT + K"),
    ("Shift+Super+k", "SUPER + SHIFT + K"),
    ("CTRL ALT, Delete", "CTRL + ALT + DELETE"),
    ('{"mods": ["alt", "SUPER"], "key": "j"}', "SUPER + ALT + J"),
    ("SUPER + code:20", "SUPER + code:20"),
    ("SUPER + comma", "SUPER + COMMA"),
    ("F9", "F9"),
    ("SHIFT + Print", "SHIFT + PRINT"),
]:
    try:
        got = hd.canon_chord(text)
    except ValueError as e:
        got = "ValueError: %s" % e
    check("chord %r -> %r" % (text, want), got == want, got)
for text in ["K", "SHIFT + K", "SUPER", "SUPER + mouse:272", "SUPER + mouse_down", "SUPER + K'", "", "HYPER + K",
             "SUPER + + K", "SUPER + Super_L", "Escape"]:
    try:
        hd.canon_chord(text)
        ok = False
    except ValueError:
        ok = True
    check("chord %r refused" % text, ok)
check("wheel chords parse when asked", hd.canon_chord("SUPER + ALT + mouse_down", wheel=True) == "SUPER + ALT + mouse_down")
check("modifier sets", hd.parse_mods("alt super") == ["SUPER", "ALT"] and hd.parse_mods("CTRL + SHIFT") == ["CTRL", "SHIFT"])
meta = {m["key"]: m for m in hd.KEY_META}
check("scroll modifiers take a custom set", hd.coerce(meta[hd.SCROLL_MODS_KEY], "alt + super") == "SUPER + ALT")
try:
    hd.coerce(meta[hd.SCROLL_MODS_KEY], "")
    check("scroll modifiers refuse none", False)
except SystemExit:
    check("scroll modifiers refuse none", True)

# ------------------------------------------------------------ bind-check
BINDS[:] = [
    bind(64, "K", "Keybindings"),
    bind(64, "comma", "Dismiss last notification"),
    bind(64, "", "Switch to workspace 3"),          # Omarchy's code:12
    bind(64, "L", "Cycle workspace layout"),          # ours (mosaic_cycle)
    bind(64, "mouse_down", "Next desktop (empty ones too)"),
    bind(64, "R", "overview thing", submap="hyprdrive"),
]
store = {"keys": {hd.MOSAIC_ON: True}}
c = hd.check_chord(store, "minimize", "SUPER + K")
check("bind-check: someone else's bind", [o["description"] for o in c["owners"]] == ["Keybindings"] and c["owners"][0]["kind"] == "other", c)
c = hd.check_chord(store, "minimize", "SUPER + COMMA")
check("bind-check: key names match case-insensitively", len(c["owners"]) == 1, c)
c = hd.check_chord(store, "minimize", "SUPER + 3")
check("bind-check: Super+3 hits Omarchy's keyless code:12 bind", [o["description"] for o in c["owners"]] == ["Switch to workspace 3"], c)
c = hd.check_chord(store, "minimize", "SUPER + code:12")
check("bind-check: code:12 hits it too", len(c["owners"]) == 1, c)
c = hd.check_chord(store, "minimize", "SUPER + L")
check("bind-check: hyprdrive's own row, named", c["owners"] and c["owners"][0]["kind"] == "hyprdrive"
      and c["owners"][0]["id"] == "mosaic_cycle" and c["owners"][0]["row"] == "Layout cycle", c)
c = hd.check_chord(store, "mosaic_cycle", "SUPER + L")
check("bind-check: the row's own chord is self, not a conflict", c["self"] and not c["owners"], c)
c = hd.check_chord(store, "minimize", "SUPER + R")
check("bind-check: a submap's bind does not count", not c["owners"], c)
c = hd.check_chord(store, "minimize", "SUPER + SHIFT + K")
check("bind-check: other modifiers are another chord", not c["owners"], c)
c = hd.check_chord(store, "scroll_desktops", "SUPER + mouse_down")
check("bind-check: the scroll row's own wheel is self", c["self"] and not c["owners"], c)
a = run(hd.cmd_bind_check, ["SHIFT + K", "minimize"])
check("bind-check: a bad chord answers ok=false, no exit", a["ok"] is False and "typing" in a["error"], a)

# ------------------------------------------------------------ bind-set
CALLS.clear()
a = run(hd.cmd_bind_set, ["minimize", "SUPER SHIFT, K"])
st = hd.load_store()
check("bind-set CHORD: stored, row on", st["bindChords"]["minimize"] == "SUPER + SHIFT + K" and st["binds"]["minimize"] is True, st)
check("bind-set CHORD: answers the chord", a["chord"] == "SUPER + SHIFT + K" and a["on"] is True, a)
f = frag()
check("bind-set CHORD: 40-panel.lua binds it inside hyprdrive_loaded()",
      "if hyprdrive_loaded() then" in f and "hl.bind('SUPER + SHIFT + K', a0, { description = 'Minimize window' })" in f)
check("bind-set CHORD: action built before the key is unbound",
      f.index("pcall(function() return hl.plugin.hyprdrive.minimize('window') end)") < f.index("hl.unbind('SUPER + SHIFT + K')"))
check("bind-set CHORD: reloads", ("reload",) in CALLS)
check("dump: bindChords", hd.bind_chords(hd.load_store())["minimize"] == "SUPER + SHIFT + K")

CALLS.clear()
a = run(hd.cmd_bind_set, ["minimize", "SUPER + K"])
check("bind-set over someone else's chord: takes it, says so", a["took"] == ["Keybindings"] and "hl.unbind('SUPER + K')" in frag(), a)
CALLS.clear()
run(hd.cmd_bind_set, ["minimize", "off"])
check("bind-set off still works, keeps the chord", hd.load_store()["binds"]["minimize"] is False
      and hd.load_store()["bindChords"]["minimize"] == "SUPER + K" and "Minimize window" not in frag() and ("reload",) in CALLS)
run(hd.cmd_bind_set, ["minimize", "on"])
check("bind-set on binds the kept chord", "hl.bind('SUPER + K', a0" in frag())

# Mosaic's cycle on another key; Super+L to minimize with --replace frees the cycle.
st = hd.load_store()
st.setdefault("keys", {})[hd.MOSAIC_ON] = True
hd.save_store(st)
a = run(hd.cmd_bind_set, ["minimize", "SUPER + L", "--replace"])
st = hd.load_store()
check("--replace turns hyprdrive's own owner off", a.get("freed") == ["mosaic_cycle"] and st["binds"]["mosaic_cycle"] is False, a)
check("--replace: Super+L now minimizes, no cycle bind", "hl.bind('SUPER + L', a0, { description = 'Minimize window' })" in frag()
      and "Cycle workspace layout" not in frag())
run(hd.cmd_bind_set, ["mosaic_cycle", "SUPER + ALT + M"])
check("the cycle takes a chord too", "hl.bind('SUPER + ALT + M', hl.dsp.exec_cmd(" in frag() and "hl.unbind('SUPER + ALT + M')" in frag())
run(hd.cmd_bind_set, ["mosaic_cycle", "default"])
check("bind-set default: back to Super+L, not stored", "mosaic_cycle" not in hd.load_store().get("bindChords", {})
      and hd.bind_chords(hd.load_store())["mosaic_cycle"] == "SUPER + L")
for bad in (["minimize", "K"], ["mosaic_focus", "SUPER + J"], ["nope", "on"]):
    try:
        run(hd.cmd_bind_set, bad)
        check("bind-set %r refused" % (bad,), False)
    except SystemExit:
        check("bind-set %r refused" % (bad,), True)

# Restore defaults: chords and switches come back.
run(hd.cmd_bind_set, ["minimize", "SUPER + J"])
CALLS.clear()
run(hd.cmd_reset, ["keybinds"])
st = hd.load_store()
check("reset keybinds: chords and switches gone", "bindChords" not in st and "binds" not in st, st)
check("reset keybinds: default chords, minimize off again", hd.bind_chords(st) == hd.CHORD_DEFAULTS
      and hd.bind_switches(st)["minimize"] is False and "Minimize window" not in frag())

# A hand-broken store chord falls back to the default rather than writing junk.
check("a bad stored chord reads as the default", hd.bind_chords({"bindChords": {"minimize": "K'"}})["minimize"] == "SUPER + H")

# keybind rows report the set chord even while off
rows = {r["id"]: r for r in hd.keybind_rows({"bindChords": {"minimize": "SUPER + J"}}, {})}
check("keybind rows: chord + defaultChord for editable rows", rows["minimize"]["chord"] == "SUPER + J"
      and rows["minimize"]["defaultChord"] == "SUPER + H" and rows["mosaic_cycle"]["chord"] == "SUPER + L")

# ------------------------------------------------------------ capture
CALLS.clear()
popen = []
real_popen = subprocess.Popen
subprocess.Popen = lambda *a, **k: popen.append(a[0])
a = run(hd.cmd_capture, ["on"])
subprocess.Popen = real_popen
check("capture on: defines the submap, enters it", a["ok"] and SUBMAP[0] == hd.CAPTURE_SUBMAP
      and any(c[0] == "eval" and "hl.define_submap('hyprdrive-capture'" in c[1] and "non_consuming = true" in c[1] for c in CALLS))
check("capture on: starts its watchdog with a token", popen and popen[0][-2:] == ["watchdog", hd.CAPTURE_TOKEN.read_text()])
a = run(hd.cmd_capture, ["off"])
check("capture off: back to the default submap", a["left"] and SUBMAP[0] == "default" and not hd.CAPTURE_TOKEN.exists())
SUBMAP[0] = "someone-else"
a = run(hd.cmd_capture, ["off"])
check("capture off leaves another submap alone", not a["left"] and SUBMAP[0] == "someone-else")
SUBMAP[0] = "default"

# ------------------------------------------------------------ Model.js
node = shutil.which("node")
if not node:
    print("node not found: Model.js checks skipped")
else:
    js = r"""
const fs = require("fs"), vm = require("vm");
const c = {}; vm.createContext(c);
vm.runInContext(fs.readFileSync(process.argv[1], "utf8").replace(".pragma library", ""), c);
const META = 0x10000000, SHIFT = 0x02000000, CTRL = 0x04000000, ALT = 0x08000000;
const out = {
  superK: c.chordFromEvent(META, 0x4b, 45),
  superShift1: c.chordFromEvent(META | SHIFT, 0x21, 10),
  ctrlAltDel: c.chordFromEvent(CTRL | ALT, 0x01000007, 119),
  superComma: c.chordFromEvent(META, 0x2c, 59),
  bareK: c.chordFromEvent(0, 0x4b, 45),
  shiftK: c.chordFromEvent(SHIFT, 0x4b, 45),
  f9: c.chordFromEvent(0, 0x01000038, 75),
  onlySuper: c.chordFromEvent(0, 0x01000022, 133),
  cyrillic: c.chordFromEvent(META, 0x0439, 45),
  held: c.heldMods(0, 0x01000022, true),
  released: c.heldMods(META, 0x01000022, false),
  parts: c.chordParts("SUPER + SHIFT + COMMA"),
  pretty: c.chordPretty("CTRL + ALT + PAGE_UP"),
  code: c.chordPretty("SUPER + code:12"),
  wheel: c.chordPretty("SUPER + ALT + mouse_down"),
};
console.log(JSON.stringify(out));
"""
    r = subprocess.run([node, "-e", js, str(ROOT / "omarchy-plugin" / "Model.js")], capture_output=True, text=True)
    try:
        o = json.loads(r.stdout)
    except ValueError:
        o = None
        check("Model.js loads in node", False, r.stderr)
    if o:
        check("js: Super+K", o["superK"]["chord"] == "SUPER + K")
        check("js: Super+Shift+1 (Qt says '!') is the number row's 1", o["superShift1"]["chord"] == "SUPER + SHIFT + 1")
        check("js: Ctrl+Alt+Delete", o["ctrlAltDel"]["chord"] == "CTRL + ALT + DELETE")
        check("js: Super+Comma", o["superComma"]["chord"] == "SUPER + COMMA")
        check("js: a bare letter is refused", o["bareK"]["chord"] == "" and o["bareK"]["error"] != "")
        check("js: Shift+letter is refused", o["shiftK"]["chord"] == "" and o["shiftK"]["error"] != "")
        check("js: F9 alone is fine", o["f9"]["chord"] == "F9")
        check("js: a lone modifier keeps listening", o["onlySuper"] == {"chord": "", "error": ""})
        check("js: a non-Latin letter binds by keycode", o["cyrillic"]["chord"] == "SUPER + code:45")
        check("js: held modifiers track press/release", o["held"] == ["SUPER"] and o["released"] == [])
        check("js: keycaps", o["parts"] == ["Super", "Shift", "Comma"] and o["pretty"] == "Ctrl + Alt + Page Up"
              and o["code"] == "Super + 3" and o["wheel"] == "Super + Alt + Scroll", o)
        # What the panel captures, hdpanel accepts unchanged.
        for k in ("superK", "superShift1", "ctrlAltDel", "superComma", "f9", "cyrillic"):
            check("js -> hdpanel round trip: %s" % k, hd.canon_chord(o[k]["chord"]) == o[k]["chord"])

shutil.rmtree(tmp, ignore_errors=True)
print("keybind_editor_test: %s" % ("OK" if not fails else "%d FAILED" % fails))
sys.exit(1 if fails else 0)
