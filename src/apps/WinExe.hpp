#pragma once

// A Windows program's own name and icon, read from its .exe (GameApps.hpp):
// the PE resource section only -- its version info (ProductName, else
// FileDescription) and its first icon group, largest image first, written
// as a PNG (a PNG image as it is; a 32-bit DIB through cairo). Pure: no
// Hyprland, so a host test can run it on real files.

#include <cairo/cairo.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace ScrollOverview::WinExe {
        struct SPe {
        std::vector<uint8_t>                       rsrc;
        uint32_t                                   rva = 0;
        bool                                       ok  = false;
    };
    template <typename T>
    inline bool rd(const std::vector<uint8_t>& b, size_t at, T& out) {
        if (at + sizeof(T) > b.size())
            return false;
        std::memcpy(&out, b.data() + at, sizeof(T));
        return true;
    }
    // The resource section of a PE file (only that is read).
    inline SPe read(const std::string& path) {
        SPe           pe;
        std::ifstream f(path, std::ios::binary);
        if (!f)
            return pe;
        auto readAt = [&](uint64_t off, size_t n) {
            std::vector<uint8_t> b(n);
            f.clear();
            f.seekg(static_cast<std::streamoff>(off));
            f.read(reinterpret_cast<char*>(b.data()), static_cast<std::streamsize>(n));
            b.resize(static_cast<size_t>(f.gcount()));
            return b;
        };
        const auto DOS = readAt(0, 64);
        uint32_t   lfanew = 0;
        if (DOS.size() < 64 || DOS[0] != 'M' || DOS[1] != 'Z' || !rd(DOS, 0x3C, lfanew) || lfanew > (1u << 20))
            return pe;
        const auto HDR = readAt(lfanew, 24 + 240);
        if (HDR.size() < 24 + 96 || std::memcmp(HDR.data(), "PE\0\0", 4) != 0)
            return pe;
        uint16_t nsec = 0, optSize = 0, magic = 0;
        rd(HDR, 6, nsec);
        rd(HDR, 20, optSize);
        rd(HDR, 24, magic);
        const size_t DD = magic == 0x20b ? 24 + 112 : 24 + 96; // data directories
        uint32_t     rsrcRva = 0;
        if (!rd(HDR, DD + 2 * 8, rsrcRva) || !rsrcRva)
            return pe;
        const auto SECS = readAt(lfanew + 24 + optSize, static_cast<size_t>(nsec) * 40);
        for (size_t i = 0; i + 40 <= SECS.size(); i += 40) {
            uint32_t vsize = 0, va = 0, rawSize = 0, rawPtr = 0;
            rd(SECS, i + 8, vsize);
            rd(SECS, i + 12, va);
            rd(SECS, i + 16, rawSize);
            rd(SECS, i + 20, rawPtr);
            if (rsrcRva >= va && rsrcRva < va + std::max(vsize, rawSize)) {
                const uint32_t SKIP = rsrcRva - va;
                if (rawSize <= SKIP || rawSize - SKIP > (96u << 20))
                    return pe;
                pe.rsrc = readAt(static_cast<uint64_t>(rawPtr) + SKIP, rawSize - SKIP);
                pe.rva  = rsrcRva;
                pe.ok   = !pe.rsrc.empty();
                return pe;
            }
        }
        return pe;
    }
    // Every (id, data) of one resource type, any language.
    inline std::vector<std::pair<uint32_t, std::vector<uint8_t>>> resources(const SPe& pe, uint32_t type) {
        std::vector<std::pair<uint32_t, std::vector<uint8_t>>> out;
        const auto&                                            B = pe.rsrc;
        auto entries = [&](uint32_t dir) {
            std::vector<std::pair<uint32_t, uint32_t>> e;
            uint16_t                                   named = 0, ids = 0;
            if (!rd(B, dir + 12, named) || !rd(B, dir + 14, ids))
                return e;
            for (uint32_t k = 0; k < static_cast<uint32_t>(named) + ids && k < 4096; ++k) {
                uint32_t name = 0, off = 0;
                if (!rd(B, dir + 16 + k * 8, name) || !rd(B, dir + 20 + k * 8, off))
                    break;
                e.emplace_back(name, off);
            }
            return e;
        };
        for (const auto& [T, TOFF] : entries(0)) {
            if (T != type || !(TOFF & 0x80000000u))
                continue;
            for (const auto& [ID, IOFF] : entries(TOFF & 0x7FFFFFFFu)) {
                if (!(IOFF & 0x80000000u))
                    continue;
                for (const auto& [LANG, LOFF] : entries(IOFF & 0x7FFFFFFFu)) {
                    (void)LANG;
                    uint32_t rva = 0, size = 0;
                    if ((LOFF & 0x80000000u) || !rd(B, LOFF, rva) || !rd(B, LOFF + 4, size) || rva < pe.rva)
                        continue;
                    const size_t AT = rva - pe.rva;
                    if (AT + size > B.size())
                        continue;
                    out.emplace_back(ID, std::vector<uint8_t>(B.begin() + AT, B.begin() + AT + size));
                    break;
                }
            }
        }
        return out;
    }
    inline std::string utf16(const uint8_t* p, size_t maxChars) {
        std::string out;
        for (size_t i = 0; i < maxChars; ++i) {
            uint32_t c = p[2 * i] | (p[2 * i + 1] << 8);
            if (!c)
                break;
            if (c >= 0xD800 && c < 0xDC00 && i + 1 < maxChars) {
                const uint32_t LO = p[2 * i + 2] | (p[2 * i + 3] << 8);
                c                 = 0x10000 + ((c - 0xD800) << 10) + (LO - 0xDC00);
                ++i;
            }
            if (c < 0x80)
                out += static_cast<char>(c);
            else if (c < 0x800)
                out += {static_cast<char>(0xC0 | (c >> 6)), static_cast<char>(0x80 | (c & 0x3F))};
            else if (c < 0x10000)
                out += {static_cast<char>(0xE0 | (c >> 12)), static_cast<char>(0x80 | ((c >> 6) & 0x3F)), static_cast<char>(0x80 | (c & 0x3F))};
            else
                out += {static_cast<char>(0xF0 | (c >> 18)), static_cast<char>(0x80 | ((c >> 12) & 0x3F)), static_cast<char>(0x80 | ((c >> 6) & 0x3F)), static_cast<char>(0x80 | (c & 0x3F))};
        }
        return out;
    }
    // The program's own name: its version info's ProductName (else
    // FileDescription).
    inline std::string name(const SPe& pe) {
        for (const auto& [ID, DATA] : resources(pe, 16 /* RT_VERSION */)) {
            (void)ID;
            for (const char* KEY : {"ProductName", "FileDescription"}) {
                std::vector<uint8_t> k;
                for (const char* c = KEY; *c; ++c)
                    k.insert(k.end(), {static_cast<uint8_t>(*c), 0});
                k.insert(k.end(), {0, 0});
                const auto IT = std::search(DATA.begin(), DATA.end(), k.begin(), k.end());
                if (IT == DATA.end())
                    continue;
                size_t at = static_cast<size_t>(IT - DATA.begin()) + k.size();
                at        = (at + 3) & ~size_t{3}; // the value is 32-bit aligned
                while (at + 1 < DATA.size() && DATA[at] == 0 && DATA[at + 1] == 0)
                    at += 2;
                if (at + 2 > DATA.size())
                    continue;
                std::string v = utf16(DATA.data() + at, (DATA.size() - at) / 2);
                v.erase(v.find_last_not_of(" \t") + 1);
                // An engine's or Windows's name is not the program's.
                std::string l = v;
                std::ranges::transform(l, l.begin(), [](unsigned char c) { return std::tolower(c); });
                if (l == "unreal engine" || l == "unrealengine" || l.find("operating system") != std::string::npos || l == "unity" || l == "unity player")
                    continue;
                if (!v.empty() && v.size() < 64)
                    return v;
            }
        }
        return "";
    }
    // The program's icon, largest first, written as a PNG to `out`.
    inline bool icon(const SPe& pe, const std::string& out) {
        const auto ICONS  = resources(pe, 3 /* RT_ICON */);
        const auto GROUPS = resources(pe, 14 /* RT_GROUP_ICON */);
        if (ICONS.empty() || GROUPS.empty())
            return false;
        struct SCand {
            int      size = 0, bpp = 0;
            uint16_t id   = 0;
        };
        std::vector<SCand> cands;
        const auto&        G = GROUPS.front().second; // the first group is the app's icon
        uint16_t           count = 0;
        rd(G, 4, count);
        for (uint16_t i = 0; i < count; ++i) {
            const size_t AT = 6 + i * 14;
            if (AT + 14 > G.size())
                break;
            SCand    c;
            c.size = G[AT] ? G[AT] : 256;
            uint16_t bpp = 0, id = 0;
            rd(G, AT + 6, bpp);
            rd(G, AT + 12, id);
            c.bpp = bpp;
            c.id  = id;
            cands.push_back(c);
        }
        std::ranges::sort(cands, [](const SCand& a, const SCand& b) { return a.size != b.size ? a.size > b.size : a.bpp > b.bpp; });
        for (const auto& C : cands) {
            const auto IT = std::ranges::find_if(ICONS, [&](const auto& p) { return p.first == C.id; });
            if (IT == ICONS.end())
                continue;
            const auto& D = IT->second;
            if (D.size() > 8 && std::memcmp(D.data(), "\x89PNG", 4) == 0) {
                std::ofstream f(out, std::ios::binary | std::ios::trunc);
                f.write(reinterpret_cast<const char*>(D.data()), static_cast<std::streamsize>(D.size()));
                return f.good();
            }
            // A 32-bit DIB: BGRA rows bottom-up, the AND mask after (alpha does the job).
            int32_t  w = 0, h2 = 0;
            uint16_t bpp = 0;
            uint32_t hdr = 0;
            if (!rd(D, 0, hdr) || !rd(D, 4, w) || !rd(D, 8, h2) || !rd(D, 14, bpp) || bpp != 32 || w <= 0 || w > 1024)
                continue;
            const int H = h2 / 2;
            if (H <= 0 || hdr + static_cast<size_t>(w) * H * 4 > D.size())
                continue;
            cairo_surface_t* s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, H);
            if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
                cairo_surface_destroy(s);
                continue;
            }
            cairo_surface_flush(s);
            uint8_t*  px     = cairo_image_surface_get_data(s);
            const int STRIDE = cairo_image_surface_get_stride(s);
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < w; ++x) {
                    const uint8_t* P = D.data() + hdr + (static_cast<size_t>(H - 1 - y) * w + x) * 4;
                    const unsigned A = P[3];
                    uint8_t*       Q = px + y * STRIDE + x * 4;
                    Q[0]             = static_cast<uint8_t>(P[0] * A / 255);
                    Q[1]             = static_cast<uint8_t>(P[1] * A / 255);
                    Q[2]             = static_cast<uint8_t>(P[2] * A / 255);
                    Q[3]             = static_cast<uint8_t>(A);
                }
            cairo_surface_mark_dirty(s);
            const bool OK = cairo_surface_write_to_png(s, out.c_str()) == CAIRO_STATUS_SUCCESS;
            cairo_surface_destroy(s);
            if (OK)
                return true;
        }
        return false;
    }

}
