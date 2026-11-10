#!/usr/bin/env python3
"""ui/bin/agent-run: the prompt reaches the agent typed, byte for byte, and is
never in any process's argument list (marketplace review, #11392). A fake
agent stands in for the CLI; every /proc/<pid>/cmdline is scanned while it
runs."""
import os
import pathlib
import subprocess
import sys
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
RUNNER = ROOT / "ui/bin/agent-run"
failed = checks = 0


def expect(ok, what):
    global failed, checks
    checks += 1
    if not ok:
        failed += 1
        print("FAIL", what)


# The fake agent: asks for bracketed paste (or not), shows a prompt line, then
# records what it is typed up to the Enter that follows the paste.
FAKE = r'''
import os, sys
paste = sys.argv[2] == "paste"
out = sys.argv[1]
sys.stdout.write(("\x1b[?2004h" if paste else "") + "fake agent> ")
sys.stdout.flush()
got = b""
while True:
    chunk = os.read(0, 4096)
    if not chunk:
        break
    got += chunk
    if (not paste or b"\x1b[201~" in got) and got.endswith(b"\n"):
        break
open(out, "wb").write(got)
'''


def scan_cmdlines(secret):
    """PIDs whose argument list holds `secret` right now."""
    hits = []
    for d in os.listdir("/proc"):
        if not d.isdigit():
            continue
        try:
            with open("/proc/%s/cmdline" % d, "rb") as f:
                if secret in f.read():
                    hits.append(int(d))
        except OSError:
            pass
    return hits


def run(prompt, mode="paste", file_mode=0o600, stdin=None):
    tmp = pathlib.Path(tempfile.mkdtemp(prefix="hd-agent-run-"))
    os.chmod(tmp, 0o700)
    pfile, out = tmp / "prompt", tmp / "got"
    fd = os.open(pfile, os.O_WRONLY | os.O_CREAT | os.O_EXCL, file_mode)
    os.write(fd, prompt)
    os.close(fd)
    os.chmod(pfile, file_mode)
    fake = tmp / "fake.py"
    fake.write_text(FAKE)
    p = subprocess.Popen([sys.executable, str(RUNNER), "--prompt-file", str(pfile), "--", sys.executable, str(fake), str(out), mode],
                         stdin=subprocess.PIPE if stdin is not None else subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if stdin is not None:
        p.stdin.write(stdin)
        p.stdin.flush()
    leaked, deleted_at = [], None
    t0 = time.monotonic()
    while p.poll() is None and time.monotonic() - t0 < 15:
        leaked += scan_cmdlines(b"SECRET-7f3a")
        if deleted_at is None and not pfile.exists():
            deleted_at = time.monotonic() - t0
        time.sleep(0.02)
    if p.poll() is None:
        p.kill()
    p.wait()
    got = out.read_bytes() if out.exists() else None
    return {"rc": p.returncode, "leaked": sorted(set(leaked)), "deleted": not pfile.exists(), "deleted_at": deleted_at, "got": got, "dir": tmp}


CANARY = pathlib.Path(tempfile.gettempdir()) / ("hd-agent-run-canary-%d" % os.getpid())
PROMPT = ("SECRET-7f3a fix $(touch %s) and `touch %s` for $HOME's \"quoted\" \\ %%P%% it's\nsecond line" % (CANARY, CANARY)).encode()

r = run(PROMPT)
expect(r["rc"] == 0, "the agent ran and exited 0 (rc %s)" % r["rc"])
expect(r["leaked"] == [], "the prompt was in no process's argument list (seen in %s)" % r["leaked"])
expect(r["deleted"] and r["deleted_at"] is not None and r["deleted_at"] < 1.0, "the prompt file was deleted at once")
expect(r["got"] == b"\x1b[200~" + PROMPT + b"\x1b[201~\n", "typed as one bracketed paste, byte for byte, then Enter: %r" % r["got"])
expect(not CANARY.exists(), "nothing in the prompt ran")

r = run(PROMPT, mode="plain")
expect(r["got"] == PROMPT.replace(b"\n", b" ") + b"\n", "no bracketed paste: one line, then Enter: %r" % r["got"])
expect(r["leaked"] == [], "plain typing: no argument list either")

# The terminal's own replies (focus in, a cursor report, a colour answer)
# arriving before the prompt is typed are not the user taking over.
r = run(PROMPT, stdin=b"\x1b[I\x1b[12;40R\x1b]11;rgb:0000/0000/0000\x1b\\")
expect(r["got"] is not None and r["got"].endswith(b"\x1b[200~" + PROMPT + b"\x1b[201~\n"), "terminal replies do not cancel the typing: %r" % r["got"])

r = run(PROMPT, file_mode=0o644)
expect(r["deleted"] and (r["got"] in (None, b"") or b"SECRET" not in r["got"]), "a prompt file others can read is refused (and still removed)")

if CANARY.exists():
    CANARY.unlink()
print("agent_run_test: %d checks, %d failed" % (checks, failed))
sys.exit(1 if failed else 0)
