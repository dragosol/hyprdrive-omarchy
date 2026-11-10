#!/usr/bin/env python3
"""A settings panel older than its files (user, 2026-11-10: "the setting is not
visible"). The Omarchy shell keeps the panel it compiled when it started
(omacom/omarchy#6981), so hdpanel compares the panel's QML/JS with the shell
process (dump's "ui", `shell-stale`), and make install restarts a stale shell:
once, not for a reinstall of the same files, not when `hyprdrive update` asks
it to wait, and not when no shell is running.

Every shell here is a fake -- a python named quickshell in a throwaway tree --
and OMARCHY_PATH points at it, so the session's own shell is never matched,
let alone restarted."""
import importlib.machinery
import importlib.util
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
HDPANEL = ROOT / "ui/bin/hdpanel"
failed = checks = 0


def expect(ok, what):
    global failed, checks
    checks += 1
    if not ok:
        failed += 1
        print("FAIL", what)


def load(name, path):
    loader = importlib.machinery.SourceFileLoader(name, str(path))
    spec = importlib.util.spec_from_loader(name, loader)
    m = importlib.util.module_from_spec(spec)
    loader.exec_module(m)
    return m


def comm_is(pid, want, timeout=5.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            if pathlib.Path("/proc/%d/comm" % pid).read_text().strip() == want:
                return True
        except OSError:
            pass
        time.sleep(0.02)
    return False


SLEEP = "import time; time.sleep(120)"

with tempfile.TemporaryDirectory() as tmp:
    tmp = pathlib.Path(tmp)
    fakebin = tmp / "fakebin"
    fakebin.mkdir()
    qs = fakebin / "quickshell"
    qs.symlink_to(sys.executable)  # comm comes from the name it was run by
    procs = []

    def fake_shell(path, code=SLEEP, **kw):
        p = subprocess.Popen([str(qs), "-c", code, "-n", "-p", str(path)], **kw)
        procs.append(p)
        expect(comm_is(p.pid, "quickshell"), "a fake shell runs as quickshell")
        return p

    def stop(p):
        p.kill()
        p.wait()

    try:
        os.environ.pop("HYPRDRIVE_UI_DIR", None)
        os.environ["OMARCHY_PATH"] = str(tmp / "omarchy")
        hd = load("hdpanel", HDPANEL)

        ui = tmp / "ui"
        ui.mkdir()
        old = time.time() - 300
        for n in ("HyprdrivePanel.qml", "Panel.qml", "Schema.js", "Model.js"):
            (ui / n).write_text("// %s\n" % n)
            os.utime(ui / n, (old, old))

        before = time.time()
        shell = fake_shell(tmp / "omarchy/shell")
        other = fake_shell(tmp / "elsewhere/shell")
        started = hd.proc_started(shell.pid)
        expect(started is not None and before - 0.1 <= started <= time.time() + 0.1,
               "a process's start time, to well under a second (%r, spawned at %r)" % (started, before))

        expect(set(hd.omarchy_shells()) == {shell.pid}, "OMARCHY_PATH set: its shell, and only that one")
        os.environ.pop("OMARCHY_PATH")
        found = hd.omarchy_shells()
        expect(shell.pid in found and other.pid not in found, "no OMARCHY_PATH (ssh): any .../omarchy/shell")
        os.environ["OMARCHY_PATH"] = str(tmp / "omarchy")

        st = hd.ui_state(shell.pid, ui)
        expect(st is not None and st["stale"] is False, "files older than the shell: not stale (%r)" % st)
        # Times are set clear of the ~10 ms that a start time is known to: a
        # file written within that of the shell starting is a coin toss (no
        # update or restart is that quick), and this script is.
        after = started + 2
        os.utime(ui / "Schema.js", (after, after))
        st = hd.ui_state(shell.pid, ui)
        expect(st is not None and st["stale"] is True and st["newest"] == "Schema.js",
               "a file changed after the shell started: stale, and which (%r)" % st)
        os.utime(ui / "Schema.js", (old, old))
        for n in ("Panel.qml.bak-20260919-154806", "notes.txt"):
            (ui / n).write_text("x")
        st = hd.ui_state(shell.pid, ui)
        expect(st is not None and st["stale"] is False, "a newer backup or text file is not code (%r)" % st)
        ui2 = tmp / "ui2"
        ui2.mkdir()
        target = tmp / "Schema-target.js"
        target.write_text("//\n")
        os.utime(target, (after, after))
        (ui2 / "Schema.js").symlink_to(target)
        expect((hd.ui_state(shell.pid, ui2) or {}).get("stale") is True,
               "a symlinked file counts with its target's time (settings/Schema.js)")
        expect(hd.ui_state(None, ui) is None and hd.ui_state(shell.pid, tmp / "none") is None,
               "no shell, or no files: no answer rather than a guess")

        # The panel's dump runs under the shell, maybe through a wrapper: the
        # shell is found up the parents (here: quickshell -> sh -> hdpanel).
        probe = tmp / "probe.py"
        probe.write_text(
            "import importlib.machinery, importlib.util, sys\n"
            "l = importlib.machinery.SourceFileLoader('hdpanel', sys.argv[1])\n"
            "s = importlib.util.spec_from_loader('hdpanel', l)\n"
            "m = importlib.util.module_from_spec(s); l.exec_module(m)\n"
            "print(m.host_shell())\n")
        code = ("import subprocess, sys; sys.exit(subprocess.call(['sh', '-c', '\"$0\" \"$1\" \"$2\"; exit $?', "
                "sys.executable, %r, %r]))" % (str(probe), str(HDPANEL)))
        p = fake_shell(tmp / "omarchy/shell", code=code, stdout=subprocess.PIPE, text=True)
        out = p.communicate(timeout=20)[0].strip()
        expect(out == str(p.pid), "the shell above a wrapper is the host (%r, want %d)" % (out, p.pid))

        env = dict(os.environ, HYPRDRIVE_UI_DIR=str(ui))
        r = subprocess.run([str(HDPANEL), "shell-stale"], env=env, capture_output=True, text=True)
        expect(r.returncode == 1, "shell-stale: the shell is newer than the panel -> 1 (%d %r)" % (r.returncode, r.stdout))
        os.utime(ui / "HyprdrivePanel.qml", (after, after))
        r = subprocess.run([str(HDPANEL), "shell-stale"], env=env, capture_output=True, text=True)
        expect(r.returncode == 0 and "HyprdrivePanel.qml" in r.stdout,
               "shell-stale: the panel is newer -> 0, and says what changed (%d %r)" % (r.returncode, r.stdout))
        stop(shell)
        stop(other)

        # MAKE INSTALL, against a copy of what it reads (not the 150 MB .so).
        root = tmp / "root"
        (root / "scripts").mkdir(parents=True)
        for f in ("scripts/install-fragments.sh", "scripts/fragment.py", "manifest.json", "config/hyprdrive.lua"):
            (root / f).parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ROOT / f, root / f)
        shutil.copytree(ROOT / "config/hyprdrive", root / "config/hyprdrive")
        shutil.copytree(ROOT / "ui", root / "ui", symlinks=True, ignore=shutil.ignore_patterns("__pycache__"))
        # The native tree's install also reads these, and copies the panel only
        # on a desktop it takes for Omarchy: an install ($OMARCHY_PATH, below)
        # and Omarchy's state directory.
        if (ROOT / "scripts/hyprdrive-integration").exists():
            shutil.copy2(ROOT / "scripts/hyprdrive-integration", root / "scripts/hyprdrive-integration")
        home = tmp / "home"
        (home / ".local/state/omarchy/current").mkdir(parents=True)
        stub = tmp / "stub"
        stub.mkdir()
        log = tmp / "restarts.log"
        (stub / "omarchy-restart-shell").write_text('#!/bin/sh\necho restart >> "$RESTARTS"\n')
        (stub / "omarchy-restart-shell").chmod(0o755)
        # Nothing of the session's: no Hyprland to ask (the native install's
        # Omarchy check would ask the running plugin), no forced integration.
        ienv = {k: v for k, v in os.environ.items() if k not in (
            "HYPRDRIVE_SHELL_RESTART", "HYPRDRIVE_UI_DIR", "HYPRLAND_INSTANCE_SIGNATURE",
            "HYPRDRIVE_INTEGRATION", "HYPRDRIVE_NO_OMARCHY")}
        ienv.update(HOME=str(home), XDG_CONFIG_HOME=str(home / ".config"), XDG_STATE_HOME=str(home / ".local/state"),
                    PATH="%s:/usr/bin:/bin" % stub, OMARCHY_PATH=str(tmp / "omarchy"), RESTARTS=str(log))

        def install(**extra):
            r = subprocess.run(["bash", str(root / "scripts/install-fragments.sh")], env=dict(ienv, **extra),
                               capture_output=True, text=True)
            expect(r.returncode == 0, "install-fragments.sh exits 0 (%d: %s)" % (r.returncode, r.stderr[-400:]))
            return r

        def restarts():
            return len(log.read_text().splitlines()) if log.exists() else 0

        s1 = fake_shell(tmp / "omarchy/shell")
        time.sleep(0.3)  # the copy lands clearly after this shell started
        r = install()
        expect(restarts() == 1 and "shell restart:" in r.stdout,
               "a shell older than the panel just installed is restarted (%d)" % restarts())
        panel = home / ".config/omarchy/plugins/dragos.hyprdrive"
        expect((panel / "HyprdrivePanel.qml").is_file(), "the panel was copied")
        stop(s1)
        time.sleep(0.3)
        s2 = fake_shell(tmp / "omarchy/shell")  # the restarted shell, clearly after the copy
        time.sleep(0.3)
        mtimes = {p.name: p.stat().st_mtime_ns for p in panel.iterdir() if p.is_file()}
        install()
        expect(restarts() == 1, "the same files again: the shell is left alone (%d)" % restarts())
        expect({p.name: p.stat().st_mtime_ns for p in panel.iterdir() if p.is_file()} == mtimes,
               "a reinstall of the same files keeps their times, the manifest's too")
        with open(root / "ui/Schema.js", "a") as fh:
            fh.write("\n// changed\n")
        install(HYPRDRIVE_SHELL_RESTART="0")
        expect(restarts() == 1, "HYPRDRIVE_SHELL_RESTART=0 (hyprdrive update): not restarted here")
        install()
        expect(restarts() == 2, "nothing new to copy, but the shell is still older than the panel: restarted")
        stop(s2)
        with open(root / "ui/Schema.js", "a") as fh:
            fh.write("// again\n")
        install()
        expect(restarts() == 2, "no Omarchy shell running: nothing to restart")
    finally:
        for p in procs:
            if p.poll() is None:
                p.kill()
                p.wait()

print("shell_restart_test: %d checks, %d failed" % (checks, failed))
sys.exit(1 if failed else 0)
