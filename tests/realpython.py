"""python3 on PATH is the interpreter running the test, whatever HOME says.

A version manager's shim (mise, asdf, pyenv) is a python3 that reads the
user's own config to pick an interpreter. The tests that move HOME and XDG_*
to a throwaway directory then run scripts that call `python3`: the shim finds
no trusted config there and exits 1, the script under test never runs, and
`make install` -- so every update -- fails on a healthy machine (issue #15).

pin() puts a directory first on PATH whose python3 is this interpreter itself
(sys.executable is the real binary, not the shim that started it). The
directory lives for the life of the test process.
"""
import atexit, os, shutil, sys, tempfile


def pin():
    real = os.path.realpath(sys.executable)
    d = tempfile.mkdtemp(prefix="hd-python-")
    atexit.register(shutil.rmtree, d, True)
    for name in ("python3", "python"):
        os.symlink(real, os.path.join(d, name))
    os.environ["PATH"] = d + os.pathsep + os.environ.get("PATH", "")
    return d
