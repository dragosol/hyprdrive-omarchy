#!/usr/bin/env python3
"""Hyprdrive config fragments: write the machine-owned half, keep the user's.

A fragment is one file under ~/.config/hypr/hyprdrive/. It has exactly two
regions, split by two marker lines:

    <everything above>              MACHINE-OWNED. Regenerated wholesale.
    -- BEGIN raw-snippet ...        marker
    <everything between>            USER-OWNED. Never touched.
    -- END raw-snippet              marker

That is the whole contract, and it is deliberately this dumb: two line-anchored
markers, no nesting, no escaping, no format of its own. The settings app
(Phase 11) writes the machine half through this tool and edits the raw half in
place; a person editing the file by hand only has to know which side of the
BEGIN line to type on. A parser with any more shape than this is a parser that
can lose somebody's config.

The raw block sits LAST on purpose. Fragments are Lua and Lua's last assignment
wins, so a hand-written line always overrides the generated one above it --
which is the only ordering under which "your edits survive" also means "your
edits take effect".

    fragment.py write     FILE < body   rewrite the machine half, keep the raw half
    fragment.py raw       FILE          print the raw half (what the settings app edits)
    fragment.py raw-write FILE < body   rewrite the raw half, keep the machine half
    fragment.py check     FILE          exit 1 if the markers are missing or malformed

`raw-write` is `write` with the two halves swapped and it exists for exactly one
caller: the settings app's raw-snippet editor, which is a text area over the raw
block. Doing that edit any other way would mean a second implementation of the
marker contract, and the whole point of this file is that there is one.
"""
import sys, os, pathlib, stat


def replace_file(path, data, fsync=False):
    """Atomic replace that never widens access (marketplace review, #11012).

    The temp file is CREATED with the destination's mode -- 0600 when there is
    no destination yet -- so a private file is never, even for an instant,
    replaced by a umask-default 0644 one. Every config and state writer in
    scripts/ goes through here. `data` is str or bytes."""
    path = pathlib.Path(path)
    try:
        mode = stat.S_IMODE(path.stat().st_mode)
    except FileNotFoundError:
        mode = 0o600
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.unlink(missing_ok=True)  # a leftover temp would keep ITS mode
    fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_EXCL, mode)
    with os.fdopen(fd, "wb") as fh:
        fh.write(data.encode() if isinstance(data, str) else data)
        if fsync:
            fh.flush()
            os.fsync(fh.fileno())
    os.chmod(tmp, mode)  # the umask can only have narrowed it; restore it exactly
    os.replace(tmp, path)

BEGIN = "-- BEGIN raw-snippet (hand edits below survive regeneration)"
END   = "-- END raw-snippet"


def split(text):
    """-> (machine, raw). A file with no markers is all machine, empty raw."""
    lines = text.splitlines()
    b = next((i for i, l in enumerate(lines) if l.strip().startswith("-- BEGIN raw-snippet")), None)
    if b is None:
        return text, ""
    e = next((i for i in range(b + 1, len(lines)) if lines[i].strip() == END), None)
    if e is None:
        raise SystemExit("fragment: %s has a BEGIN marker and no END -- refusing to guess where the raw block stops")
    return "\n".join(lines[:b]), "\n".join(lines[b + 1:e])


def compose(machine, raw):
    return machine.rstrip("\n") + "\n\n" + BEGIN + "\n" + raw.strip("\n") + ("\n" if raw.strip("\n") else "") + END + "\n"


def main(argv):
    if len(argv) != 3:
        raise SystemExit(__doc__)
    cmd, path = argv[1], pathlib.Path(argv[2])
    old = path.read_text() if path.exists() else ""

    if cmd == "raw":
        print(split(old)[1])
        return 0

    if cmd == "check":
        machine, raw = split(old)
        if BEGIN not in old:
            print("fragment: %s has no raw-snippet block" % path, file=sys.stderr)
            return 1
        print("%s: %d machine lines, %d raw lines" % (path, len(machine.splitlines()), len(raw.splitlines())))
        return 0

    if cmd in ("write", "raw-write"):
        machine, raw = split(old)
        body = sys.stdin.read()
        if cmd == "write":
            machine = body
        else:
            raw = body
        path.parent.mkdir(parents=True, exist_ok=True)
        # Atomic: a half-written fragment is a config that does not parse, and
        # this file is sourced by the session's own hyprland.lua. The temp name
        # ends .lua.tmp, which hyprland.lua's `%.lua$` matcher does not take --
        # so even the instant it exists it is not a config file.
        replace_file(path, compose(machine, raw))
        kept = "raw block: %d lines kept" if cmd == "write" else "raw block: %d lines written"
        print(("wrote %s (" + kept + ")") % (path, len(raw.splitlines())))
        return 0

    raise SystemExit(__doc__)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
