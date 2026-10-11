#!/usr/bin/env python3
"""The two manifests must agree.

manifest.json (repo root) is what the Omarchy marketplace and `omarchy plugin
add` read: the repository IS the plugin folder there, so its entry point is
omarchy-plugin/Panel.qml. omarchy-plugin/manifest.json is what `make install`
copies into ~/.config/omarchy/plugins/dragos.hyprdrive, where Panel.qml sits at
the top. They differ in entryPoints and (the root's longer, listing-facing)
description, and in nothing else -- above all not in version, which a release
bumps in both.
"""
import json, pathlib, sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
a = json.loads((ROOT / "manifest.json").read_text())
b = json.loads((ROOT / "omarchy-plugin" / "manifest.json").read_text())
IGNORED = {"entryPoints", "description"}
bad = sorted(k for k in set(a) | set(b) if k not in IGNORED and a.get(k) != b.get(k))
if a.get("entryPoints", {}).get("barWidget") != "omarchy-plugin/Panel.qml":
    bad.append("entryPoints (root must point at omarchy-plugin/Panel.qml)")
if len(a.get("description", "")) > 500:
    bad.append("description (marketplace limit is 500 characters)")
if bad:
    print("manifest_check: FAIL -- the manifests disagree on: " + ", ".join(bad))
    sys.exit(1)
print(f"manifest_check: OK -- root and omarchy-plugin manifests agree (v{a['version']})")
