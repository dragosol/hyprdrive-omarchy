#!/usr/bin/env python3
"""The binary files of tests/fixtures/identity/, made from nothing.

The fixtures copy the LAYOUT of real Steam, Lutris, umu, Wine and Bottles
installs, never their binaries: every .exe, .ico, .png, .jpg and the binary
shortcuts.vdf here is synthetic, a few hundred bytes, written by this script.
Run it from anywhere; it rewrites the files in place and is deterministic, so
a re-run leaves `git status` clean.

    python3 -I tests/fixtures/identity/gen/make_binaries.py

Needs Pillow only for the .jpg/.png placeholders.
"""
import os
import struct
import sys
import zlib

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))


def put(rel, data):
    path = os.path.join(ROOT, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)


# ---- images ---------------------------------------------------------------
def png(w, h, rgba):
    """A plain RGBA PNG, without Pillow (deterministic bytes)."""
    raw = b"".join(b"\0" + bytes(rgba) * w for _ in range(h))

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)

    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")


def jpg(w, h, rgb):
    from io import BytesIO
    from PIL import Image

    out = BytesIO()
    Image.new("RGB", (w, h), rgb).save(out, "JPEG", quality=50)
    return out.getvalue()


def dib32(w, rgba):
    """A 32-bit icon DIB: BITMAPINFOHEADER (height doubled), BGRA rows, AND mask."""
    h = w
    hdr = struct.pack("<IiiHHIIiiII", 40, w, h * 2, 1, 32, 0, w * h * 4, 0, 0, 0, 0)
    r, g, b, a = rgba
    px = bytes((b, g, r, a)) * (w * h)
    mask = b"\0" * (((w + 31) // 32) * 4 * h)
    return hdr + px + mask


def ico(images):
    """An .ico file holding (size, bpp, bytes) images."""
    head = struct.pack("<HHH", 0, 1, len(images))
    off = 6 + 16 * len(images)
    dirs, blobs = b"", b""
    for size, bpp, data in images:
        dirs += struct.pack("<BBBBHHII", size % 256, size % 256, 0, 0, 1, bpp, len(data), off + len(blobs))
        blobs += data
    return head + dirs + blobs


# ---- a minimal PE32+ with RT_ICON, RT_GROUP_ICON and RT_VERSION -------------
def utf16z(s):
    return s.encode("utf-16-le") + b"\0\0"


def align4(b):
    return b + b"\0" * (-len(b) % 4)


def vs_node(key, value=b"", children=b"", text=False):
    """One VS_VERSIONINFO-style node: wLength, wValueLength, wType, szKey, pad, value, pad, children."""
    body = align4(struct.pack("<HHH", 0, 0, 0) + utf16z(key))
    vlen = len(value) // 2 if text else len(value)
    body = align4(body + value) + children
    return struct.pack("<HHH", len(body), vlen, 1 if text else 0) + body[6:]


def versioninfo(product, description):
    fixed = struct.pack("<13I", 0xFEEF04BD, 0x00010000, 0x00010000, 0, 0x00010000, 0, 0x3F, 0, 0x40004, 1, 0, 0, 0)
    strings = align4(vs_node("ProductName", utf16z(product), text=True)) + align4(vs_node("FileDescription", utf16z(description), text=True))
    table = vs_node("040904b0", children=strings)
    sfi = vs_node("StringFileInfo", children=align4(table))
    return vs_node("VS_VERSION_INFO", fixed, align4(sfi))


def rsrc_section(rva, resources):
    """resources: {type: [(id, data)]} -> the .rsrc bytes (three-level tree)."""
    types = sorted(resources)
    # Layout: root dir, type dirs, id dirs (one lang each), data entries, then data.
    def dir_bytes(entries):
        return struct.pack("<IIHHHH", 0, 0, 0, 0, 0, len(entries)) + b"".join(struct.pack("<II", n, o) for n, o in entries)

    root_size = 16 + 8 * len(types)
    type_dirs, id_dirs, leaves = [], [], []
    off = root_size
    type_offs = []
    for t in types:
        type_offs.append(off)
        off += 16 + 8 * len(resources[t])
    id_offs = {}
    for t in types:
        for rid, _ in resources[t]:
            id_offs[(t, rid)] = off
            off += 16 + 8
    leaf_offs = {}
    for t in types:
        for rid, _ in resources[t]:
            leaf_offs[(t, rid)] = off
            off += 16
    data_offs = {}
    blob = b""
    for t in types:
        for rid, data in resources[t]:
            data_offs[(t, rid)] = off + len(blob)
            blob += align4(data)
    out = dir_bytes([(t, 0x80000000 | type_offs[i]) for i, t in enumerate(types)])
    for t in types:
        out += dir_bytes([(rid, 0x80000000 | id_offs[(t, rid)]) for rid, _ in resources[t]])
    for t in types:
        for rid, _ in resources[t]:
            out += dir_bytes([(0x409, leaf_offs[(t, rid)])])
    for t in types:
        for rid, data in resources[t]:
            out += struct.pack("<IIII", rva + data_offs[(t, rid)], len(data), 0, 0)
    return out + blob


def pe(product, description, rgba, png_size=32):
    small = dib32(16, rgba)
    big = png(png_size, png_size, rgba)
    group = struct.pack("<HHH", 0, 1, 2) + struct.pack("<BBBBHHIH", 16, 16, 0, 0, 1, 32, len(small), 1) + struct.pack("<BBBBHHIH", png_size % 256, png_size % 256, 0, 0, 1, 32, len(big), 2)
    RVA = 0x1000
    rsrc = rsrc_section(RVA, {3: [(1, small), (2, big)], 14: [(1, group)], 16: [(1, versioninfo(product, description))]})
    raw = rsrc + b"\0" * (-len(rsrc) % 0x200)
    dos = b"MZ" + b"\0" * 58 + struct.pack("<I", 0x40)
    coff = b"PE\0\0" + struct.pack("<HHIIIHH", 0x8664, 1, 0, 0, 0, 240, 0x22)
    opt = bytearray(240)
    struct.pack_into("<H", opt, 0, 0x20B)
    struct.pack_into("<I", opt, 108, 16)  # NumberOfRvaAndSizes
    struct.pack_into("<II", opt, 112 + 2 * 8, RVA, len(rsrc))  # the resource directory
    sec = struct.pack("<8sIIIIIIHHI", b".rsrc", len(rsrc), RVA, len(raw), 0x200, 0, 0, 0, 0, 0x40000040)
    head = dos + coff + bytes(opt) + sec
    return head + b"\0" * (0x200 - len(head)) + raw


# ---- shortcuts.vdf (binary KeyValues) ---------------------------------------
def kv_str(k, v):
    return b"\x01" + k.encode() + b"\0" + v.encode() + b"\0"


def kv_int(k, v):
    return b"\x02" + k.encode() + b"\0" + struct.pack("<I", v)


def shortcut(i, appid, name, exe, start, icon, opts):
    body = kv_int("appid", appid) + kv_str("AppName", name) + kv_str("Exe", exe) + kv_str("StartDir", start) + kv_str("icon", icon)
    body += kv_str("ShortcutPath", "") + kv_str("LaunchOptions", opts) + kv_int("IsHidden", 0) + kv_int("AllowOverlay", 1) + kv_int("LastPlayTime", 0)
    body += b"\x00tags\0\x08"
    return b"\x00" + str(i).encode() + b"\0" + body + b"\x08"


def main():
    blue, red, green, gold = (40, 90, 200, 255), (200, 40, 40, 255), (40, 160, 60, 255), (220, 170, 30, 255)
    S = "steam-native/home/user/.local/share/Steam"
    put(f"{S}/steam/games/f260267377759a5fea6b651817c9a413c6b562b0.ico", ico([(16, 32, dib32(16, gold)), (48, 32, png(48, 48, gold))]))
    put(f"{S}/appcache/librarycache/3321460/f260267377759a5fea6b651817c9a413c6b562b0.jpg", jpg(32, 32, gold[:3]))
    put(f"{S}/appcache/librarycache/3321460/2a9754975cf3bbf08f89fd82d0e2400505a8f4b8/logo.png", png(64, 36, gold))
    put(f"{S}/appcache/librarycache/2357570/da42bd294c941d5947b1bc2c2b2efa1313d36a91.jpg", jpg(32, 32, red[:3]))
    put(f"{S}/appcache/librarycache/252950/0b6c2d5e4f3a2918171615141312111009080706.jpg", jpg(32, 32, blue[:3]))
    for size in (16, 48, 256):
        put(f"steam-native/home/user/.local/share/icons/hicolor/{size}x{size}/apps/steam_icon_2357570.png", png(size, size, red))
    F = "steam-flatpak/home/user/.var/app/com.valvesoftware.Steam/.local/share/Steam"
    put(f"{F}/appcache/librarycache/1145360/5d0c1e2f3a4b5c6d7e8f90a1b2c3d4e5f6a7b8c9.jpg", jpg(32, 32, green[:3]))

    C = "steam-shortcuts/home/user"
    wow = "/home/user/Games/battlenet/drive_c/Program Files (x86)/World of Warcraft/World of Warcraft Launcher.exe"
    vdf = b"\x00shortcuts\0"
    vdf += shortcut(0, 3965729953, "World of Warcraft Launcher.exe", f'"{wow}"', '"/home/user/Games/battlenet/drive_c/"', "", "")
    vdf += shortcut(1, 4207589967, "Cyberpunk 2077", '"/usr/bin/lutris"', '"/usr/bin/"', "/home/user/.local/share/icons/hicolor/128x128/apps/lutris_cyberpunk-2077.png", "lutris:rungameid/3")
    vdf += b"\x08\x08"
    put(f"{C}/.local/share/Steam/userdata/12345678/config/shortcuts.vdf", vdf)
    put(f"{C}/.local/share/icons/hicolor/128x128/apps/lutris_cyberpunk-2077.png", png(128, 128, gold))
    put(f"{C}/Games/battlenet/drive_c/Program Files (x86)/World of Warcraft/World of Warcraft Launcher.exe", pe("World of Warcraft", "World of Warcraft Launcher", blue))

    L = "lutris/home/user"
    put(f"{L}/.local/share/icons/hicolor/128x128/apps/lutris_battlenet.png", png(128, 128, blue))
    put(f"{L}/.local/share/lutris/coverart/hogwarts-legacy.jpg", jpg(32, 48, gold[:3]))
    put(f"{L}/Games/battlenet/drive_c/Program Files (x86)/Battle.net/Battle.net.exe", pe("Battle.net", "Battle.net Launcher", blue))
    put(f"{L}/Games/battlenet/drive_c/Program Files (x86)/Overwatch/_retail_/Overwatch.exe", pe("Overwatch", "Overwatch", gold))
    put(f"{L}/Games/battlenet/drive_c/Program Files (x86)/Overwatch/_retail_/UnityCrashHandler64.exe", pe("Unity Player", "Unity Crash Handler", gold))
    put(f"{L}/Games/epic-games-store/drive_c/Program Files/Epic Games/Launcher/Portal/Binaries/Win64/EpicGamesLauncher.exe", pe("Epic Games Launcher", "EpicGamesLauncher", green))
    put(f"{L}/Games/epic-games-store/drive_c/Program Files/Epic Games/HogwartsLegacy/Phoenix/Binaries/Win64/HogwartsLegacy.exe", pe("Hogwarts Legacy", "Hogwarts Legacy", gold))

    LF = "lutris-flatpak/home/user/.var/app/net.lutris.Lutris/data"
    put(f"{LF}/icons/hicolor/128x128/apps/lutris_gog-galaxy.png", png(128, 128, green))
    put("lutris-flatpak/home/user/Games/gog-galaxy/drive_c/Program Files (x86)/GOG Galaxy/GalaxyClient.exe", pe("GOG GALAXY", "GOG Galaxy", green))

    U = "umu/home/user/Games/umu/umu-default/drive_c/Games"
    put(f"{U}/Alpha/Alpha.exe", pe("Alpha Game", "Alpha", red))
    put(f"{U}/Beta/Beta.exe", pe("Beta Game", "Beta", blue))

    W = "wine-menus/home/user"
    put(f"{W}/.local/share/icons/hicolor/48x48/apps/5298_World of Warcraft Launcher.0.png", png(48, 48, blue))
    put(f"{W}/.local/share/icons/hicolor/48x48/apps/A1B2_launcher.0.png", png(48, 48, red))
    put(f"{W}/Games/alpha/drive_c/Alpha/launcher.exe", pe("Alpha", "Alpha Launcher", red))
    put(f"{W}/Games/beta/drive_c/Beta/launcher.exe", pe("Beta", "Beta Launcher", blue))
    put(f"{W}/Games/battlenet/drive_c/Program Files (x86)/World of Warcraft/World of Warcraft Launcher.exe", pe("World of Warcraft", "World of Warcraft Launcher", blue))

    B = "bottles/home/user/.var/app/com.usebottles.bottles/data/bottles/bottles/Office/drive_c/Program Files/Notepad++/notepad++.exe"
    put(B, pe("Notepad++", "Notepad++ : a free (GNU) source code editor", green))

    # dosdevices: c: -> ../drive_c, as Wine makes it.
    for prefix in ("umu/home/user/Games/umu/umu-default", "wine-menus/home/user/Games/alpha", "wine-menus/home/user/Games/beta", "lutris/home/user/Games/battlenet",
                   "lutris/home/user/Games/epic-games-store", "bottles/home/user/.var/app/com.usebottles.bottles/data/bottles/bottles/Office"):
        d = os.path.join(ROOT, prefix, "dosdevices")
        os.makedirs(d, exist_ok=True)
        link = os.path.join(d, "c:")
        if os.path.lexists(link):
            os.remove(link)
        os.symlink("../drive_c", link)
    return 0


if __name__ == "__main__":
    sys.exit(main())
