#!/usr/bin/env python3
"""omarchy-plugin/bin/hyprdrive: versions, the bounded no-redirect fetch, the
marketplace reading, the check's cache and the update decision -- offline (the
network calls are replaced)."""
import contextlib
import importlib.machinery
import importlib.util
import io
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


# What the CLI prints is a report for a user, and in a build log its made-up
# versions ("installed v1.9.36", "Waiting for marketplace approval: v1.9.38")
# read as a real one that is out of date (2026-11-09). Shown when a check fails.
shown = io.StringIO()
with contextlib.redirect_stdout(shown), tempfile.TemporaryDirectory() as tmp:
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

    # OMARCHY: the approved version is the marketplace's PUBLISHED snapshot,
    # from its registry -- never issue text (marketplace review, #11392).
    o = load("omarchy", home)
    (home / ".local/state/hyprdrive/install.json").write_text(json.dumps({"version": "1.9.36"}))
    A, B = "a" * 40, "b" * 40
    PID, REPO = "dragos.hyprdrive", "dragosol/hyprdrive-omarchy"

    def source(sha=A, base=None, review=None, history=None, repo="https://github.com/" + REPO, plugins=None):
        s = {"repo": repo, "listingValidatedCommit": sha, "plugins": plugins if plugins is not None else {PID: {}}}
        if base is not None:
            s["automatedSecurityBaseline"] = base
        if review is not None:
            s["maintainerVerificationReview"] = review
        if history is not None:
            s["maintainerVerificationReviewHistory"] = history
        return {"sources": [s], "retiredPluginIds": []}

    rec = lambda commit=A, **kw: dict({"commit": commit, "repository": REPO, "pluginIds": [PID], "findings": []}, **kw)
    snap = o.published_snapshot
    expect(snap(source(review=rec(requestEventId=7))) == A, "a maintainer review of the listed commit: approved")
    expect(snap(source(base=rec(outcome="passed", capabilities=[]))) == A, "a passed baseline of the listed commit: approved")
    expect(snap(source(base=rec(outcome="review-required", capabilities=["installer"]))) is None, "review-required with no review: not approved")
    expect(snap(source(review=rec(B))) is None, "evidence for another commit does not cover the listed one")
    expect(snap(source(review=rec(findings=["x"]))) is None, "a review with findings: not approved")
    expect(snap(source(review=rec(requestEventId=7),
                       history=[{"maintainerVerificationRevocation": {"commit": A, "requestEventId": 7}}])) is None, "a revoked review: not approved")
    expect(snap(source(review=rec(), repo="https://github.com/evil/hyprdrive-omarchy")) is None, "another repository: not ours")
    expect(snap(source(review=rec(), plugins={"other.id": {}})) is None, "another plugin id: not ours")
    expect(snap(source(sha="a" * 39 + "; rm", review=rec())) is None, "a listed commit that is not a full SHA: nothing")
    expect(snap(dict(source(review=rec()), retiredPluginIds=[PID])) is None, "a retired id: nothing")

    registry = source(review=rec(requestEventId=7))
    # The case the review raised: an old approved issue edited to name a later,
    # unreviewed commit. Issues are only ever read for the pending link now.
    search = {"items": [
        {"number": 12000, "state": "open", "title": "[Verify]: Hyprdrive 1.9.38 (dragos.hyprdrive)", "body": "commit %s" % B, "html_url": "https://evil.example/"},
        {"number": 11900, "state": "closed", "closed_at": "2026-11-01T00:00:00Z", "title": "[Verify]: Hyprdrive 9.9.9 (dragos.hyprdrive)",
         "labels": [{"name": "approved-and-verified"}], "body": "edited later to name %s" % B},
    ]}
    seen = []

    def fake_fetch(url, limit=0, etag=None):
        seen.append((url, etag))
        if url == o.REGISTRY_URL:
            if etag == "W/1":
                return None
            o.fetch.etag = "W/1"
            return json.dumps(registry).encode()
        raise AssertionError("unexpected fetch " + url)

    def fake_json(url, limit=0):
        if "search/issues" in url:
            return search
        if url.endswith("/releases/latest"):
            return {"tag_name": "v1.9.38"}
        if url.endswith("/%s/manifest.json" % A):
            # The real fetch_json goes through fetch(), which sets fetch.etag:
            # the registry's own must be the one cached (review, 2026-11-09).
            o.fetch.etag = "W/manifest"
            return {"version": "1.9.37"}
        raise AssertionError("unexpected json " + url)
    o.fetch = fake_fetch
    o.fetch.etag = None
    o.fetch_json = fake_json
    r = o.check(refresh=True)
    mk = r["marketplace"]
    expect(mk["approved"] == {"sha": A, "version": "1.9.37"}, "approved: the registry's published commit, its manifest's version")
    expect(B not in json.dumps(mk), "a SHA in issue text is never taken")
    expect(mk["pending"] == {"issue": 12000, "url": "https://github.com/omacom/omarchy-plugin-marketplace/issues/12000", "version": "1.9.38"},
           "pending: the open request's link and title version only, URL built here")
    expect(r["available"] and r["approvedNewer"], "omarchy: an approved version newer than the installed one")
    o.check(refresh=True)
    expect(seen[-1] == (o.REGISTRY_URL, "W/1") and o.marketplace()["approved"]["sha"] == A, "an unchanged registry (304) reuses the last answer")

    # the update decision, without installing
    picked = []
    o.install = lambda ref, label, yes, info: picked.append(ref) or 0
    o.update({"--approved"})
    o.update({"--github"})
    expect(picked == [A, "v1.9.38"], "--approved installs the published commit, --github the newest tag")
    o.SNAPSHOT_CACHE.unlink()
    registry = source(base=rec(outcome="review-required", capabilities=["installer"]))
    picked.clear()
    o.update({"--approved"})
    expect(picked == [], "nothing published as verified: --approved installs nothing")
    o.update({"--yes"})
    expect(picked == ["v1.9.38"], "nothing approved: offer GitHub")

    # CACHES CANNOT CARRY AN APPROVAL (marketplace review, #11392): an old
    # issue-derived update-check.json, a failed refresh, and the install path.
    import time as _t
    import urllib.error as _ue
    registry = source(review=rec(requestEventId=7))  # the registry publishes A (1.9.37)
    o.SNAPSHOT_CACHE.unlink()  # the registry changed: a real server would send a new ETag
    old_answer = {"latest": {"version": "9.9.9", "tag": "v9.9.9", "url": "x"},
                  "marketplace": {"approved": {"sha": B, "version": "9.9.9", "issue": 11900}, "pending": None}}
    legacy = {"repo": REPO, "checkedAt": _t.time(), "remote": old_answer}  # written before 1.9.40: no schema
    o.CHECK_CACHE.write_text(json.dumps(legacy))
    r = o.check(refresh=False)
    expect(r["marketplace"]["approved"]["sha"] == A and B not in json.dumps(r), "a legacy cache, still fresh, is not used: the registry is asked")

    def offline(url, limit=0):
        raise _ue.URLError("offline")
    good_json = o.fetch_json
    o.fetch_json = offline
    o.CHECK_CACHE.write_text(json.dumps(legacy))
    r = o.check(refresh=True)
    expect(r["error"] and r["marketplace"] is None and B not in json.dumps(r), "a failed refresh does not restore a legacy cache")
    picked.clear()
    rc = o.update({"--approved", "--yes"})
    expect(picked == [] and rc == 1, "failed refresh + legacy cache: nothing installed")

    current = dict(legacy, schema=o.CACHE_SCHEMA, checkedAt=0)  # current format, stale, naming B
    o.CHECK_CACHE.write_text(json.dumps(current))
    r = o.check(refresh=True)
    expect(r["error"] and r["marketplace"]["approved"]["sha"] == B, "a failed refresh shows a current-format cache, marked as an error")
    picked.clear()
    for flags in ({"--approved", "--yes"}, {"--github", "--yes"}, {"--yes"}):
        o.update(flags)
    expect(picked == [], "failed refresh: nothing installed from any cache, whatever it names")
    o.fetch_json = good_json

    # The install path: the check's answer names B (as a cache could), the
    # registry -- read again in full, no ETag -- names A. Only A is built.
    fresh_answer = {"latest": {"version": "9.9.9", "tag": "v9.9.9", "url": "x"},
                    "marketplace": {"approved": {"sha": B, "version": "9.9.9"}, "pending": None}}
    o.CHECK_CACHE.write_text(json.dumps({"schema": o.CACHE_SCHEMA, "repo": REPO, "checkedAt": _t.time(), "remote": fresh_answer}))
    o.SNAPSHOT_CACHE.write_text(json.dumps({"schema": o.CACHE_SCHEMA, "etag": "W/1", "approved": {"sha": B, "version": "9.9.9"}}))
    real_check = o.check
    o.check = lambda refresh=False: real_check(refresh=False)  # use the cached answer naming B
    picked.clear(); seen.clear()
    o.update({"--approved", "--yes"})
    expect(picked == [A], "--approved builds the commit the registry names now, not the cached one")
    expect((o.REGISTRY_URL, None) in seen, "the install reads the whole registry again (no ETag, no snapshot cache)")
    registry = source(base=rec(outcome="review-required", capabilities=["installer"]))  # nothing verified any more
    picked.clear()
    o.update({"--approved", "--yes"})
    expect(picked == [], "the registry no longer verifies it: --approved installs nothing")

    def registry_down(url, limit=0, etag=None):
        raise _ue.URLError("offline")
    o.fetch = registry_down
    picked.clear()
    o.update({"--approved", "--yes"})
    expect(picked == [], "the registry cannot be read at install time: nothing installed")
    o.check = real_check

    # AN UPDATE BUILDS FROM SCRATCH (2026-11-09): what an older build left in
    # .build/ stopped every update across 1.9.50, so make clean comes first.
    # A real (empty) checkout, since install() asks git about it; run() is replaced.
    import subprocess as _sp
    os.environ.pop("HYPRLAND_INSTANCE_SIGNATURE", None)  # nothing is reloaded here
    for mod in (load("native", home), load("omarchy", home)):  # fresh: install() is replaced above
        co = home / ("checkout-" + mod.FLAVOR)
        _sp.run(["git", "-c", "init.defaultBranch=main", "init", "-q", str(co)], check=True)
        _sp.run(["git", "-C", str(co), "remote", "add", "origin", "https://github.com/%s.git" % mod.REPO], check=True)
        (co / "manifest.json").write_text('{"version": "9.9.9"}')
        ran, real_run = [], mod.run
        mod.run = lambda cmd, cwd=None, env=None: ran.append(cmd) or 0
        rc = mod.install("", "x", True, {"source": str(co)}, rebuild=True)
        mod.run = real_run
        makes = [c for c in ran if c[0] == "make"]
        expect(rc == 0 and len(makes) > 1 and makes[0] == ["make", "clean"], "%s: an update runs make clean before it builds" % mod.FLAVOR)

        # NO RELOAD INTO A REPLACED HYPRLAND (review, 2026-11-09): the unload
        # could not be undone this session. Otherwise: unload, load, reload,
        # and the config's errors shown. hyprctl itself is never run here.
        import types as _ty
        mod.shutil = _ty.SimpleNamespace(which=lambda n: "/usr/bin/" + n)
        os.environ["HYPRLAND_INSTANCE_SIGNATURE"] = "test"
        mod.hypr_env = lambda: None
        mod.run = lambda cmd, cwd=None, env=None: ran.append(cmd) or 0
        ran.clear()
        mod.hyprland_replaced = lambda: True
        rc = mod.install("", "x", True, {"source": str(co)}, rebuild=True)
        expect(rc == 0 and not [c for c in ran if c[:3] == ["hyprctl", "plugin", "unload"]],
               "%s: Hyprland replaced under the session: installed, nothing unloaded" % mod.FLAVOR)
        ran.clear()
        mod.hyprland_replaced = lambda: False
        rc = mod.install("", "x", True, {"source": str(co)}, rebuild=True)
        hy = [c[1:3] for c in ran if c[0] == "hyprctl"]
        expect(rc == 0 and hy[:4] == [["plugin", "unload"], ["plugin", "load"], ["reload"], ["configerrors"]],
               "%s: the reload is unload, load, reload, configerrors (%r)" % (mod.FLAVOR, hy))
        mod.hypr_env = lambda: {"HYPRLAND_INSTANCE_SIGNATURE": "test"}
        mod.loaded = lambda env: False
        rc = mod.install("", "x", True, {"source": str(co)}, rebuild=True)
        expect(rc == 1, "%s: a plugin that did not load after the reload is reported" % mod.FLAVOR)

        # THE PANEL IS COMPILED ONCE (user, 2026-11-10: "the setting is not
        # visible"): make install leaves the Omarchy shell alone here
        # (HYPRDRIVE_SHELL_RESTART=0) and the update restarts it after the
        # plugin reload -- on Omarchy, when the shell shows an older panel, and
        # not when Hyprland was replaced (nothing was reloaded then).
        envs = []

        def rec(cmd, cwd=None, env=None):
            ran.append(cmd)
            envs.append((cmd, env))
            return 0
        mod.run = rec
        mod.hypr_env = lambda: None
        for stale, replaced in ((True, False), (False, False), (True, True)):
            ran.clear()
            envs.clear()
            mod.shell_stale = lambda s=stale: s
            mod.hyprland_replaced = lambda r=replaced: r
            rc = mod.install("", "x", True, {"source": str(co)}, rebuild=True)
            want = stale and not replaced and mod.FLAVOR == "omarchy"
            expect(rc == 0 and (["omarchy-restart-shell"] in ran) == want,
                   "%s: shell %s, Hyprland %s: the shell is %srestarted after the reload" % (
                       mod.FLAVOR, "stale" if stale else "current", "replaced" if replaced else "running",
                       "" if want else "not "))
        menv = next((e for c, e in envs if c[:2] == ["make", "install"]), None) or {}
        expect(menv.get("HYPRDRIVE_SHELL_RESTART") == ("0" if mod.FLAVOR == "omarchy" else None),
               "%s: %s (%r)" % (mod.FLAVOR, "make install leaves the shell restart to the update" if mod.FLAVOR == "omarchy"
                                else "make install restarts a stale shell itself", menv))
        mod.hyprland_replaced = lambda: False
        os.environ.pop("HYPRLAND_INSTANCE_SIGNATURE", None)

        # A PARTIAL INSTALL IS NOT ROLLED BACK (review, 2026-11-09): once make
        # install has copied the new .so, checking the old version out would put
        # old settings over the new plugin. A plain build failure still goes back.
        pfx = home / ("prefix-" + mod.FLAVOR)
        so = pfx / "lib/hyprdrive/hyprdrive.so"

        def make_install_fails(copied):
            def fake(cmd, cwd=None, env=None):
                ran.append(cmd)
                if cmd[:2] == ["make", "install"]:
                    if copied:
                        so.parent.mkdir(parents=True, exist_ok=True)
                        so.write_bytes(b"new")
                    return 2
                return 0
            return fake
        for copied, want_back in ((True, False), (False, True)):
            so.unlink(missing_ok=True)
            ran.clear()
            mod.run = make_install_fails(copied)
            rc = mod.install("v9.9.9", "x", True, {"source": str(co), "prefix": str(pfx)})
            went_back = any(c[:2] == ["git", "-C"] and c[3:5] == ["checkout", "-q"] for c in ran)
            expect(rc == 1 and went_back == want_back,
                   "%s: make install failed %s the .so was copied: checkout %s" % (
                       mod.FLAVOR, "after" if copied else "before", "goes back" if want_back else "stays"))
        mod.run = real_run

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

if failed:
    sys.stdout.write(shown.getvalue())
print("update_cli_test: %d checks, %d failed" % (checks, failed))
sys.exit(1 if failed else 0)
