#!/usr/bin/env python3
"""THE parser of Config.cpp's registerConfigValuesFor(). One copy, two readers.

docs/CONFIG.md and the settings schema both have to say exactly what the plugin
registers, and before this file existed the first of them owned a regex that the
second would have had to copy. A duplicated mirror of Config.cpp is a mirror that
drifts on the day somebody adds a key and updates one of the two -- which is the
same failure `gen_keys_table.sh` was written to end for the hand-maintained
README table. So the parse lives here and both generators import it.

    from configkeys import parse
    keys = parse("Config.cpp")     # -> [Key(...), ...] in registration order

A Key is deliberately close to the source text: `section` is the sub-namespace
("", "input", "dock", "shadow"), `name` the leaf, `default` and `range` already
prettified for display, and `raw_default` kept because the schema needs the
value, not the rendering of it.
"""
import re
from dataclasses import dataclass, field
from typing import Optional

TYPE = {"CIntValue": "int", "CFloatValue": "float", "CBoolValue": "bool",
        "CStringValue": "string", "CColorValue": "color", "CGradientValue": "color/gradient"}


@dataclass
class Key:
    section: str
    name: str
    type: str
    default: str        # prettified, as docs/CONFIG.md prints it
    raw_default: str    # the C++ expression, verbatim
    range: str          # "0 .. 20" / ">= 1" / ""
    min: Optional[str] = None
    max: Optional[str] = None
    description: str = ""

    @property
    def path(self):
        """The key's tail under the plugin prefix: 'dock:autohide', 'scale'."""
        return f"{self.section}:{self.name}" if self.section else self.name


def _clean_desc(d):
    # Adjacent string literals in the source are one description.
    return re.sub(r'"\s*"', "", d).replace("\\\"", '"').strip()


def _split_default(rest):
    """The default value, and the min/max options that follow it, if any."""
    depth = 0
    for i, ch in enumerate(rest):
        if ch in "({[":
            depth += 1
        elif ch in ")}]":
            depth -= 1
        elif ch == "," and depth == 0:
            return rest[:i].strip(), rest[i + 1:].strip()
    return rest.strip(), ""


def num(v):
    """0.82F -> 0.82, 20.F -> 20, 1.F -> 1."""
    v = re.sub(r"([0-9.])F$", r"\1", v.strip())
    return v[:-1] if v.endswith(".") else v


def pretty_default(v, kind):
    v = v.strip()
    v = re.sub(r'^Hyprlang::STRING\{"(.*)"\}$', r'"\1"', v)
    v = re.sub(r"^\(Hyprlang::INT\)", "", v)
    m = re.match(r"^CHyprColor\{(.*)\}$", v)
    if m:
        return "rgba(" + ", ".join(num(x) for x in m.group(1).split(",")) + ")"
    return (num(v) if kind in ("int", "float") else v) or "-"


def _range(lo, hi):
    if not lo and not hi:
        return ""
    if lo and hi:
        return f"{num(lo)} .. {num(hi)}"
    return f">= {num(lo)}" if lo else f"<= {num(hi)}"


def parse(path):
    src = open(path).read()
    body = src.split("static void registerConfigValuesFor()", 1)[1]

    # Each registration is one makeShared<CxxxValue>(...) argument list; they are
    # wrapped over several lines, so flatten and split on the constructor name.
    flat = re.sub(r"\s+", " ", body)
    calls = re.findall(
        r'makeShared<(C\w+Value)>\(\s*KEY\("([^"]+)"\)\s*,\s*"((?:[^"\\]|\\.|"\s*")*)"\s*,\s*(.*?)\)\s*\)\s*;', flat)

    out = []
    for kind, tail, desc, rest in calls:
        section, _, name = tail.rpartition(":")
        default, opts = _split_default(rest)
        lo = re.search(r"\.min\s*=\s*([-\w.]+)", opts)
        hi = re.search(r"\.max\s*=\s*([-\w.]+)", opts)
        t = TYPE.get(kind, kind)
        out.append(Key(section=section, name=name, type=t,
                       default=pretty_default(default, t), raw_default=default.strip(),
                       range=_range(lo.group(1) if lo else None, hi.group(1) if hi else None),
                       min=num(lo.group(1)) if lo else None,
                       max=num(hi.group(1)) if hi else None,
                       description=_clean_desc(desc)))

    # The per-animation motion blur keys are registered in a loop over
    # FlightBlur.hpp's SITES table (one row per blur site), so the regex above
    # never sees them: expand the table the way the loop does, at the loop.
    if "FlightBlur::SITES" in body:
        import os
        hdr = open(os.path.join(os.path.dirname(os.path.abspath(path)), "FlightBlur.hpp")).read()
        table = hdr.split("inline constexpr SSite SITES[] = {", 1)[1].split("};", 1)[0]
        sites = re.findall(r'\{"(\w+)",\s*"([^"]*)",\s*([0-9.]+)F,\s*"(\w+)"\}', table)
        # Insert where the loop sits: after the key registered just before it.
        before = re.search(r'KEY\("([^"]+)"\)[^;]*;\s*for \(const auto& SITE : FlightBlur::SITES\)', flat)
        at = next((i + 1 for i, k in enumerate(out) if before and k.path == before.group(1)), len(out))
        extra = []
        for name, label, strength, fade in sites:
            extra.append(Key(section=f"motion_blur:{name}", name="strength", type="float", default=num(strength + "F"),
                             raw_default=strength + "F", range="0 .. 1", min="0", max="1",
                             description=f"motion blur: {label} -- share of the motion smeared (0 = off)"))
            extra.append(Key(section=f"motion_blur:{name}", name="fade", type="string", default=f'"{fade}"',
                             raw_default=f'Hyprlang::STRING{{"{fade}"}}', range="",
                             description=f"motion blur: {label} -- early, full, late, linear or constant"))
        out[at:at] = extra
    return out
