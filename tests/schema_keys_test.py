#!/usr/bin/env python3
"""Every key the settings panel shows (Schema.js) is one hdpanel can write.

A row whose key is missing from hdpanel's KEY_META cannot be changed: `hdpanel
set` refuses it as an unknown key, and the panel's switch snaps straight back
(2026-10-17: the straight-fan, genie and CPU-pulse switches did exactly that).
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
schema = (ROOT / "ui" / "Schema.js").read_text()
hdpanel = (ROOT / "ui" / "bin" / "hdpanel").read_text()

shown = set(re.findall(r'key:\s*P\s*\+\s*"([^"]+)"', schema))
known = set(re.findall(r'_k\(PLUGIN \+ "([^"]+)"', hdpanel))
missing = sorted(shown - known)
if missing:
    for k in missing:
        print("schema_keys_test: the panel shows plugin:hyprdrive:%s, which hdpanel cannot write (add it to KEY_META)" % k)
    sys.exit(1)
print("schema_keys_test: OK -- all %d panel keys are writable" % len(shown))
