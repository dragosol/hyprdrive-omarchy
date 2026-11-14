#!/usr/bin/env python3
# tests/gates/sessionlock.py -- a minimal ext-session-lock-v1 locker for the rig.
#
#   sessionlock.py LOG
#
# Locks the session and puts a lock surface on every output (a solid shm
# buffer, each configure acked). LOG gets one line per event, flushed:
# `pid <n>` (first, so the gate can signal it), `locked`, `finished` (the
# compositor refused or ended the lock), `key <code> <state>` for every key the
# lock screen receives, `kbenter`, `unlocked` (last, just before a clean exit).
# SIGTERM or SIGUSR1 unlocks (unlock_and_destroy, then a roundtrip so the
# request reaches the compositor) and exits. A locker that dies without
# unlocking leaves Hyprland locked with no surface: the instance is then done.
# Needs bindings for wayland.xml and ext-session-lock-v1 on PYTHONPATH as
# package `lcproto` (the gate generates them with pywayland.scanner).
import mmap
import os
import select
import signal
import sys

from pywayland.client import Display
from lcproto.wayland import WlCompositor, WlOutput, WlSeat, WlShm
from lcproto.ext_session_lock_v1 import ExtSessionLockManagerV1

log = open(sys.argv[1], "a", buffering=1)


def note(s):
    log.write(s + "\n")


note(f"pid {os.getpid()}")
stop = {"asked": False}


def on_signal(_sig, _frm):
    stop["asked"] = True


signal.signal(signal.SIGTERM, on_signal)
signal.signal(signal.SIGUSR1, on_signal)

d = Display()
d.connect()
reg = d.get_registry()
g = {}
outputs = []


def on_global(_r, name, iface, ver):
    if iface == "wl_output":
        outputs.append(name)
    else:
        g.setdefault(iface, (name, ver))


reg.dispatcher["global"] = on_global
d.roundtrip()

comp = reg.bind(g["wl_compositor"][0], WlCompositor, 4)
shm = reg.bind(g["wl_shm"][0], WlShm, 1)
seat = reg.bind(g["wl_seat"][0], WlSeat, 5)
mgr = reg.bind(g["ext_session_lock_manager_v1"][0], ExtSessionLockManagerV1, 1)
outs = [reg.bind(n, WlOutput, 1) for n in outputs]

kbd = seat.get_keyboard()
kbd.dispatcher["key"] = lambda _k, _serial, _t, key, st: note(f"key {key} {st}")
kbd.dispatcher["enter"] = lambda *_: note("kbenter")
kbd.dispatcher["keymap"] = lambda _k, _fmt, fd, _size: os.close(fd)

lock = mgr.lock()
state = {"locked": False, "finished": False}


def on_locked(_l):
    state["locked"] = True
    note("locked")


def on_finished(_l):
    state["finished"] = True
    note("finished")


lock.dispatcher["locked"] = on_locked
lock.dispatcher["finished"] = on_finished

keep = []


def make_buffer(w, h):
    stride = w * 4
    size = stride * h
    fd = os.memfd_create("sessionlock", 0)
    os.ftruncate(fd, size)
    mm = mmap.mmap(fd, size)
    mm.write(b"\x40\x20\x20\xff" * (w * h))  # a solid dark blue
    pool = shm.create_pool(fd, size)
    buf = pool.create_buffer(0, w, h, stride, WlShm.format.xrgb8888.value)
    pool.destroy()
    os.close(fd)
    keep.append(mm)
    return buf


for out in outs:
    surf = comp.create_surface()
    ls = lock.get_lock_surface(surf, out)

    def on_configure(lsurf, serial, w, h, surf=surf):
        lsurf.ack_configure(serial)
        buf = make_buffer(w, h)
        keep.append(buf)
        surf.attach(buf, 0, 0)
        surf.damage_buffer(0, 0, w, h)
        surf.commit()

    ls.dispatcher["configure"] = on_configure
    keep.append((surf, ls))

d.roundtrip()
fd = d.get_fd()
while not stop["asked"] and not state["finished"]:
    d.flush()
    try:
        r, _, _ = select.select([fd], [], [], 0.2)
    except InterruptedError:
        continue
    if r and d.dispatch(block=True) == -1:
        break
if state["locked"] and not state["finished"]:
    lock.unlock_and_destroy()
    d.roundtrip()
    note("unlocked")
d.disconnect()
os._exit(0)  # not sys.exit: pywayland's cffi teardown has crashed the interpreter
