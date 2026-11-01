#!/usr/bin/env python3
"""omarchy-plugin/bin/hyprdrive: versions, the bounded no-redirect fetch, the
marketplace reading, the check's cache and the update decision -- offline (the
network calls are replaced)."""
import importlib.machinery
import importlib.util
import json
import os
import pathlib
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
SRC = ROOT / "omarchy-plugin/bin/hyprdrive"
failed = checks = 0


def load(flavor, home):
    os.environ["HOME"] = str(home)
    for k in ("XDG_STATE_HOME", "XDG_CACHE_HOME"):
        os.environ.pop(k, None)
    text = SRC.read_text().replace('FLAVOR = "native"', 'FLAVOR = "%s"' % flavor).replace('FLAVOR = "omarchy"', 'FLAVOR = "%s"' % flavor)
    path = pathlib.Path(home) / ("hd_%s.py" % flavor)
    path.write_text(text)
    spec = importlib.util.spec_from_loader("hd_" + flavor, importlib.machinery.SourceFileLoader("hd_" + flavor, str(path)))
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def expect(ok, what):
    global failed, checks
    checks += 1
    if not ok:
        failed += 1
        print("FAIL", what)


with tempfile.TemporaryDirectory() as tmp:
    home = pathlib.Path(tmp)
    m = load("native", home)

    # versions
    expect(m.parse_version("v1.9.36.7") == (1, 9, 36, 7) and m.parse_version("1.9.36") == (1, 9, 36), "versions parse with or without v")
    expect(m.parse_version("1.9.36; rm -rf ~") is None and m.parse_version("latest") is None and m.parse_version(None) is None, "anything else is no version")
    expect(m.newer((1, 9, 36, 8), (1, 9, 36, 7)) and not m.newer((1, 9, 36), (1, 9, 36, 0)) and m.newer((1, 9, 37), (1, 9, 36, 9)), "newer compares numerically, missing parts are 0")

    # the fetch refuses anything but GitHub over HTTPS
    for bad in ("http://api.github.com/x", "https://example.org/", "https://api.github.com.evil.org/", "file:///etc/passwd"):
        try:
            m.fetch(bad)
            expect(False, "refused " + bad)
        except ValueError:
            expect(True, "refused " + bad)
    expect(m._NoRedirect().redirect_request(None, None, 302, "", {}, "https://example.org") is None, "redirects are not followed")

    # NATIVE check: installed from make install's record, latest from releases
    (home / ".local/state/hyprdrive").mkdir(parents=True)
    (home / ".local/state/hyprdrive/install.json").write_text(json.dumps({"version": "1.9.36.7", "source": str(home / "src"), "prefix": str(home / ".local")}))
    calls = []
    m.fetch_json = lambda url, limit=0: calls.append(url) or {"tag_name": "v1.9.36.8"}
    r = m.check()
    expect(r["available"] and r["installed"] == "1.9.36.7" and r["latest"]["version"] == "1.9.36.8", "native: a newer release is available")
    expect(r["latest"]["url"] == "https://github.com/dragosol/Hyprdrive/releases/tag/v1.9.36.8", "native: the release URL is built, not taken from the response")
    m.check()
    expect(len(calls) == 1, "a second check inside 6 hours is answered from the cache")
    m.check(refresh=True)
    expect(len(calls) == 2, "--refresh asks again")
    m.fetch_json = lambda url, limit=0: {"tag_name": "v1.9.36.7"}
    expect(not m.check(refresh=True)["available"], "native: the same version is not an update")
    cache = home / ".cache/hyprdrive/update-check.json"
    c = json.loads(cache.read_text())
    c["repo"] = "dragosol/hyprdrive-omarchy"
    c["remote"]["latest"]["version"] = "9.9.9"
    cache.write_text(json.dumps(c))
    expect(m.check()["latest"]["version"] == "1.9.36.7", "the other Hyprdrive's cached answer is not used")
    (home / ".local/state/hyprdrive/no-update-check").write_text("x")
    m.fetch_json = lambda url, limit=0: expect(False, "no network while checks are off") or {}
    expect(m.check()["disabled"] and not m.check()["available"], "--never-check: the settings stop asking")
    (home / ".local/state/hyprdrive/no-update-check").unlink()

    def boom(url, limit=0):
        raise OSError("offline")
    m.fetch_json = boom
    r = m.check(refresh=True)
    expect(r["error"] and r["latest"]["version"] == "1.9.36.7", "offline: the stale answer, with the error")

    # OMARCHY: the marketplace's approved and pending requests
    o = load("omarchy", home)
    (home / ".local/state/hyprdrive/install.json").write_text(json.dumps({"version": "1.9.36"}))
    A, P = "a" * 40, "b" * 40
    search = {"items": [
        {"number": 12000, "state": "open", "title": "[Verify]: Hyprdrive 1.9.38 (dragos.hyprdrive)", "labels": [{"name": "plugin-update"}],
         "body": "commit %s" % P, "html_url": "https://evil.example/"},
        {"number": 11900, "state": "closed", "closed_at": "2026-11-01T00:00:00Z", "title": "[Verify]: Hyprdrive 1.9.37 (dragos.hyprdrive)",
         "labels": [{"name": "approved-and-verified"}], "body": "sha %s" % A},
        {"number": 11012, "state": "closed", "closed_at": "2026-10-13T00:00:00Z", "title": "[Plugin]: Hyprdrive", "labels": [{"name": "submission"}], "body": "x " + "c" * 40},
    ]}

    def fake(url, limit=0):
        if "search/issues" in url:
            return search
        if url.endswith("/releases/latest"):
            return {"tag_name": "v1.9.38"}
        return {"version": "9.9.9"}
    o.fetch_json = fake
    r = o.check(refresh=True)
    mk = r["marketplace"]
    expect(mk["approved"]["version"] == "1.9.37" and mk["approved"]["sha"] == A and mk["approved"]["issue"] == 11900, "approved: the closed approved-and-verified request")
    expect(mk["pending"]["version"] == "1.9.38" and mk["pending"]["url"] == "https://github.com/omacom/omarchy-plugin-marketplace/issues/12000", "pending: the open one, its URL built here")
    expect(r["available"] and r["approvedNewer"], "omarchy: both an approved and a newer pending version")

    # the update decision, without installing
    picked = []
    o.install = lambda ref, label, yes, info: picked.append(ref) or 0
    o.update({"--approved"})
    o.update({"--github"})
    expect(picked == [A, "v1.9.38"], "--approved installs the approved commit, --github the newest tag")
    search["items"][1]["title"] = "[Verify]: Hyprdrive 1.9.38 (dragos.hyprdrive)"
    picked.clear()
    o.update({"--yes"})
    expect(picked == [A], "the newest release IS the approved one: install it")
    search["items"] = search["items"][:1]
    picked.clear()
    o.update({"--yes"})
    expect(picked == ["v1.9.38"], "nothing approved yet: offer GitHub")

    # the terminal
    os.environ["PATH"] = str(home / "bin")
    (home / "bin").mkdir()
    for t in ("kitty",):
        (home / "bin" / t).write_text("#!/bin/sh\n")
        (home / "bin" / t).chmod(0o755)
    os.environ.pop("TERMINAL", None)
    cmd = o.terminal_command(["hyprdrive", "update"])
    expect(cmd and cmd[1:] == ["-e", "hyprdrive", "update"], "a terminal is found and runs the command")
    (home / "bin" / "kitty").unlink()
    expect(o.terminal_command(["x"]) is None, "no terminal: None (the settings open GitHub instead)")

print("update_cli_test: %d checks, %d failed" % (checks, failed))
sys.exit(1 if failed else 0)
