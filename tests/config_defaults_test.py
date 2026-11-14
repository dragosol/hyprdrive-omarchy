#!/usr/bin/env python3
"""A setting's default is written twice: in Config.cpp (what the plugin uses when
the key is unset) and in hdpanel's KEY_META (what the settings page shows and
what "Restore defaults" writes). They must agree, or the page shows one value
while the plugin runs another and a reset changes behaviour.

Every KEY_META key that is a plugin key must be registered, and for bool, int
and float keys the two defaults must be equal. Strings and enums are compared
when Config.cpp's default is a plain literal.
"""
import importlib.util, pathlib, re, sys
from importlib.machinery import SourceFileLoader

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "scripts" / "lib"))
from configkeys import parse  # noqa: E402

# load_module() is gone in Python 3.15: spec + exec_module instead.
_loader = SourceFileLoader("hdpanel", str(ROOT / "ui" / "bin" / "hdpanel"))
hd = importlib.util.module_from_spec(importlib.util.spec_from_loader("hdpanel", _loader))
_loader.exec_module(hd)
PREFIX = "plugin:hyprdrive:"
reg = {}
for k in parse(str(ROOT / "src" / "core" / "Config.cpp")):
    reg[PREFIX + (k.section + ":" if k.section else "") + k.name] = k

# Panel defaults that differ from the plugin's ON PURPOSE go here, with the reason.
ALLOWED = {}

fails, checked = [], 0
for m in hd.KEY_META:
    key, kind, want = m["key"], m["kind"], m["default"]
    if not key.startswith(PREFIX):
        continue
    k = reg.get(key)
    if k is None:
        fails.append("%s: in the panel, not registered in Config.cpp" % key)
        continue
    raw = k.raw_default.strip()
    got = None
    if k.type == "bool" and raw in ("true", "false"):
        got = raw == "true"
    elif k.type == "bool" and raw in ("0", "1"):
        got = raw == "1"
    elif k.type == "int" and re.fullmatch(r"-?\d+", raw):
        got = int(raw)
    elif k.type == "float" and re.fullmatch(r"-?\d*\.?\d+f?F?", raw):
        got = float(raw.rstrip("fF"))
    elif k.type == "string":
        s = re.fullmatch(r'Hyprlang::STRING\{"((?:[^"\\]|\\.)*)"\}', raw)
        if s:
            got = s.group(1)
    if got is None:
        continue  # a computed default: nothing to compare
    checked += 1
    same = (abs(float(got) - float(want)) < 1e-6) if isinstance(got, float) or (isinstance(want, float) and not isinstance(got, (bool, str))) \
        else (bool(want) == got if isinstance(got, bool) else str(want) == str(got) if isinstance(got, str) else want == got)
    if not same and ALLOWED.get(key) != (got, want):
        fails.append("%s: Config.cpp default %r, panel default %r" % (key, got, want))

# A default the plugin also spells as an enum constant: the constant names the
# registered literal (the overview tile order -- Config.hpp, one default for
# the owner to choose).
ENUM_DEFAULTS = {PREFIX + "overview_tile_order": ("OVERVIEW_TILE_ORDER_DEFAULT", "ETileOrder",
                                                   {"RECENT": "recent", "CREATED": "created", "POSITION": "position"})}
hpp = (ROOT / "src" / "core" / "Config.hpp").read_text()
for key, (const, enum, names) in ENUM_DEFAULTS.items():
    m = re.search(r"constexpr\s+%s\s+%s\s*=\s*%s::(\w+)" % (enum, const, enum), hpp)
    k = reg.get(key)
    lit = re.fullmatch(r'Hyprlang::STRING\{"([^"]*)"\}', k.raw_default.strip()) if k else None
    checked += 1
    if not m or not lit:
        fails.append("%s: %s or its registered literal not found" % (key, const))
    elif names.get(m.group(1)) != lit.group(1):
        fails.append("%s: Config.hpp %s is %s, Config.cpp registers %r" % (key, const, m.group(1), lit.group(1)))

for f in fails:
    print("FAIL", f)
print("config_defaults_test: %d defaults compared, %d failed" % (checked, len(fails)))
sys.exit(1 if fails else 0)
