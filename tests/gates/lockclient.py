#!/usr/bin/env python3
# tests/gates/lockclient.py -- a "game" for the rig: one opaque xdg toplevel
# (an XRGB8888 buffer, so a fullscreen one is eligible for Hyprland's solitary
# path) that logs what reaches it and, on request, holds the pointer the way a
# game does.
#
#   lockclient.py LOG APP_ID [none|lock|confine]
#
# LOG gets one line per event, flushed: `key <code> <state>`, `button <code>
# <state>`, `enter`, `leave`, `rel` (one zwp_relative_pointer motion),
# `locked`/`unlocked`/`confined`/`unconfined`, `kbenter`/`kbleave`.
# lock/confine are requested once the surface first has a buffer, persistent
# lifetime, on the whole surface -- what XWayland asks for a Wine game in
# mouse-look (lock) or with its cursor clipped to the window (confine).
# Needs bindings for xdg-shell, pointer-constraints and relative-pointer on
# PYTHONPATH as package `lcproto` (the gate generates them with
# pywayland.scanner).
import mmap
import os
import sys

from pywayland.client import Display
from lcproto.wayland import WlCompositor, WlSeat, WlShm
from lcproto.xdg_shell import XdgWmBase
from lcproto.pointer_constraints_unstable_v1 import ZwpPointerConstraintsV1
from lcproto.relative_pointer_unstable_v1 import ZwpRelativePointerManagerV1

LOG, APP_ID, MODE = sys.argv[1], sys.argv[2], (sys.argv[3] if len(sys.argv) > 3 else "none")
log = open(LOG, "a", buffering=1)


def note(s):
    log.write(s + "\n")


d = Display()
d.connect()
reg = d.get_registry()
g = {}


def on_global(_r, name, iface, ver):
    g.setdefault(iface, (name, ver))


reg.dispatcher["global"] = on_global
d.roundtrip()

comp = reg.bind(g["wl_compositor"][0], WlCompositor, 4)
shm = reg.bind(g["wl_shm"][0], WlShm, 1)
seat = reg.bind(g["wl_seat"][0], WlSeat, 5)
wm = reg.bind(g["xdg_wm_base"][0], XdgWmBase, 1)
pc = reg.bind(g["zwp_pointer_constraints_v1"][0], ZwpPointerConstraintsV1, 1)
rpm = reg.bind(g["zwp_relative_pointer_manager_v1"][0], ZwpRelativePointerManagerV1, 1)


def on_ping(wmb, serial):
    wmb.pong(serial)


wm.dispatcher["ping"] = on_ping

surf = comp.create_surface()
xs = wm.get_xdg_surface(surf)
top = xs.get_toplevel()
top.set_app_id(APP_ID)
top.set_title(APP_ID)
state = {"w": 800, "h": 600, "buf": None, "held": None, "pending": None}


def on_top_configure(_t, w, h, _states):
    if w > 0 and h > 0:
        state["pending"] = (w, h)


def on_close(_t):
    os._exit(0)  # not sys.exit: a raise inside a cffi callback crashed the interpreter


top.dispatcher["configure"] = on_top_configure
top.dispatcher["close"] = on_close


def make_buffer(w, h):
    stride = w * 4
    size = stride * h
    fd = os.memfd_create("lockclient", 0)
    os.ftruncate(fd, size)
    mm = mmap.mmap(fd, size)
    mm.write(b"\x30\x60\x30\xff" * (w * h))  # an opaque green
    pool = shm.create_pool(fd, size)
    buf = pool.create_buffer(0, w, h, stride, WlShm.format.xrgb8888.value)
    pool.destroy()
    os.close(fd)
    return buf


def hold_pointer():
    if MODE == "lock" and not state["held"]:
        lk = pc.lock_pointer(surf, ptr, None, ZwpPointerConstraintsV1.lifetime.persistent.value)
        lk.dispatcher["locked"] = lambda *_: note("locked")
        lk.dispatcher["unlocked"] = lambda *_: note("unlocked")
        state["held"] = lk
    elif MODE == "confine" and not state["held"]:
        cf = pc.confine_pointer(surf, ptr, None, ZwpPointerConstraintsV1.lifetime.persistent.value)
        cf.dispatcher["confined"] = lambda *_: note("confined")
        cf.dispatcher["unconfined"] = lambda *_: note("unconfined")
        state["held"] = cf


def on_xs_configure(x, serial):
    x.ack_configure(serial)
    if state["pending"]:
        state["w"], state["h"] = state["pending"]
        state["pending"] = None
    state["buf"] = make_buffer(state["w"], state["h"])
    surf.attach(state["buf"], 0, 0)
    surf.damage_buffer(0, 0, state["w"], state["h"])
    # The whole surface is opaque: Hyprland's opaque() reads this region.
    rg = comp.create_region()
    rg.add(0, 0, state["w"], state["h"])
    surf.set_opaque_region(rg)
    surf.commit()
    hold_pointer()


xs.dispatcher["configure"] = on_xs_configure

ptr = seat.get_pointer()
kbd = seat.get_keyboard()
ptr.dispatcher["enter"] = lambda *_: note("enter")
ptr.dispatcher["leave"] = lambda *_: note("leave")
ptr.dispatcher["button"] = lambda _p, _serial, _t, button, st: note(f"button {button} {st}")
kbd.dispatcher["key"] = lambda _k, _serial, _t, key, st: note(f"key {key} {st}")
kbd.dispatcher["enter"] = lambda *_: note("kbenter")
kbd.dispatcher["leave"] = lambda *_: note("kbleave")
kbd.dispatcher["keymap"] = lambda _k, _fmt, fd, _size: os.close(fd)
rel = rpm.get_relative_pointer(ptr)
rel.dispatcher["relative_motion"] = lambda *_: note("rel")

surf.commit()
note(f"start {MODE}")
while d.dispatch(block=True) != -1:
    pass
os._exit(0)
