#!/usr/bin/env python3
"""The repository has exactly one plugin manifest, at its root.

The Omarchy marketplace refuses a submission with any other manifest.json in
the top two levels of the tree (build-catalog.mjs, inspectPluginManifests:
"exactly one plugin manifest at the repository root"). `omarchy plugin add`
clones the repository as the plugin folder, so the root manifest points the
shell at omarchy-plugin/Panel.qml; `make install` generates the copied
panel's manifest from it (scripts/install-fragments.sh).
"""
import json, pathlib, subprocess, sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
bad = []
tracked = subprocess.run(["git", "-C", str(ROOT), "ls-files"], capture_output=True, text=True).stdout.split()
extra = [p for p in tracked if p.endswith("manifest.json") and p != "manifest.json" and p.count("/") <= 1]
if extra:
    bad.append("a second manifest near the root: " + ", ".join(extra))
m = json.loads((ROOT / "manifest.json").read_text())
if m.get("entryPoints", {}).get("barWidget") != "omarchy-plugin/Panel.qml":
    bad.append("entryPoints.barWidget must be omarchy-plugin/Panel.qml")
if len(m.get("description", "")) > 500:
    bad.append("description over the marketplace's 500 characters")
if bad:
    print("manifest_check: FAIL -- " + "; ".join(bad))
    sys.exit(1)
print(f"manifest_check: OK -- one plugin manifest, at the root (v{m['version']})")
