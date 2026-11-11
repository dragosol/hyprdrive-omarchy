#pragma once

// A Windows program's own name and icon, read from its .exe: the PE resource
// section only -- its version info (ProductName, else FileDescription) and its
// first icon group, largest image first (Ico.hpp decodes it; IconPng.hpp
// writes it). The section is read through a callback, so a file (read()), a
// Sources (AppIdentity) and a buffer (fromBuffer(), the fuzz tests) all take
// the same bounded path: at most Caps::RSRC of resources, 96 sections, 4096
// directory entries. Pure: no Hyprland, no cairo.

#include "Ico.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace Hyprdrive::WinExe {
    using AppIdentity::Bytes;
    using AppIdentity::rd;

    struct SPe {
        std::vector<uint8_t> rsrc;
        uint32_t             rva = 0;
        bool                 ok  = false;
    };
    // Bytes [off, off + n) of the file, fewer at its end, none past it.
    using ReadAt = std::function<std::vector<uint8_t>(uint64_t off, size_t n)>;

    inline SPe parse(const ReadAt& readAt) {
        SPe pe;
        try {
            const auto DOS    = readAt(0, 64);
            uint32_t   lfanew = 0;
            if (DOS.size() < 64 || DOS[0] != 'M' || DOS[1] != 'Z' || !rd(Bytes{DOS}, 0x3C, lfanew) || lfanew > (1u << 20))
                return pe;
            const auto HDR = readAt(lfanew, 24 + 240);
            if (HDR.size() < 24 + 96 || std::memcmp(HDR.data(), "PE\0\0", 4) != 0)
                return pe;
            uint16_t nsec = 0, optSize = 0, magic = 0;
            rd(Bytes{HDR}, 6, nsec);
            rd(Bytes{HDR}, 20, optSize);
            rd(Bytes{HDR}, 24, magic);
            const size_t DD      = magic == 0x20b ? 24 + 112 : 24 + 96; // data directories
            uint32_t     rsrcRva = 0;
            if (!rd(Bytes{HDR}, DD + 2 * 8, rsrcRva) || !rsrcRva)
                return pe;
            const auto SECS = readAt(uint64_t{lfanew} + 24 + optSize, static_cast<size_t>(std::min<uint16_t>(nsec, 96)) * 40);
            for (size_t i = 0; i + 40 <= SECS.size(); i += 40) {
                uint32_t vsize = 0, va = 0, rawSize = 0, rawPtr = 0;
                rd(Bytes{SECS}, i + 8, vsize);
                rd(Bytes{SECS}, i + 12, va);
                rd(Bytes{SECS}, i + 16, rawSize);
                rd(Bytes{SECS}, i + 20, rawPtr);
                if (rsrcRva >= va && uint64_t{rsrcRva} < uint64_t{va} + std::max(vsize, rawSize)) {
                    const uint32_t SKIP = rsrcRva - va;
                    if (rawSize <= SKIP || rawSize - SKIP > AppIdentity::Caps::RSRC)
                        return pe;
                    pe.rsrc = readAt(uint64_t{rawPtr} + SKIP, rawSize - SKIP);
                    pe.rva  = rsrcRva;
                    pe.ok   = !pe.rsrc.empty();
                    return pe;
                }
            }
        } catch (...) { return SPe{}; }
        return pe;
    }

    inline SPe fromBuffer(Bytes file) {
        return parse([file](uint64_t off, size_t n) {
            if (off >= file.size())
                return std::vector<uint8_t>{};
            const size_t N = std::min<uint64_t>(n, file.size() - off);
            return std::vector<uint8_t>(file.begin() + static_cast<std::ptrdiff_t>(off), file.begin() + static_cast<std::ptrdiff_t>(off + N));
        });
    }

    // The resource section of a PE file on disk (only that is read).
    inline SPe read(const std::string& path) {
        std::ifstream f(path, std::ios::binary);
        if (!f)
            return {};
        return parse([&f](uint64_t off, size_t n) {
            std::vector<uint8_t> b(n);
            f.clear();
            f.seekg(static_cast<std::streamoff>(off));
            f.read(reinterpret_cast<char*>(b.data()), static_cast<std::streamsize>(n));
            b.resize(f ? n : static_cast<size_t>(std::max<std::streamsize>(f.gcount(), 0)));
            return b;
        });
    }

    struct SRes {
        uint32_t id = 0;
        Bytes    data; // into SPe::rsrc
    };
    inline constexpr size_t MAX_RESOURCES = 4096;

    // Every (id, data) of one resource type, the first language of each.
    inline std::vector<SRes> resources(const SPe& pe, uint32_t type) {
        std::vector<SRes> out;
        const Bytes       B{pe.rsrc};
        size_t            budget = MAX_RESOURCES; // directory entries visited, all levels
        auto              entries = [&](uint32_t dir) {
            std::vector<std::pair<uint32_t, uint32_t>> e;
            uint16_t                                   named = 0, ids = 0;
            if (!rd(B, dir + size_t{12}, named) || !rd(B, dir + size_t{14}, ids))
                return e;
            for (uint32_t k = 0; k < uint32_t{named} + ids && budget; ++k, --budget) {
                uint32_t name = 0, off = 0;
                if (!rd(B, dir + size_t{16} + k * size_t{8}, name) || !rd(B, dir + size_t{20} + k * size_t{8}, off))
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
                    if ((LOFF & 0x80000000u) || !rd(B, LOFF, rva) || !rd(B, LOFF + size_t{4}, size) || rva < pe.rva)
                        continue;
                    const size_t AT = rva - pe.rva;
                    if (AT > B.size() || size > B.size() - AT)
                        continue;
                    out.push_back({ID, B.subspan(AT, size)});
                    break;
                }
            }
        }
        return out;
    }

    inline std::string utf16(const uint8_t* p, size_t maxChars) {
        std::string out;
        for (size_t i = 0; i < maxChars && out.size() < 1024; ++i) {
            uint32_t c = p[2 * i] | (p[2 * i + 1] << 8);
            if (!c)
                break;
            if (c >= 0xD800 && c < 0xDC00 && i + 1 < maxChars) {
                const uint32_t LO = p[2 * i + 2] | (p[2 * i + 3] << 8);
                if (LO < 0xDC00 || LO > 0xDFFF)
                    continue; // a lone high surrogate: dropped
                c = 0x10000 + ((c - 0xD800) << 10) + (LO - 0xDC00);
                ++i;
            } else if (c >= 0xD800 && c < 0xE000)
                continue;
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
    // FileDescription). An engine's or Windows's name is not the program's.
    inline std::string name(const SPe& pe) {
        try {
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
                    const std::string L = AppIdentity::lower(v);
                    if (L == "unreal engine" || L == "unrealengine" || L.find("operating system") != std::string::npos || L == "unity" || L == "unity player")
                        continue;
                    if (!v.empty() && v.size() < 64)
                        return v;
                }
            }
        } catch (...) {}
        return "";
    }

    // The program's icon: the first group (the app's own), largest image
    // that decodes.
    inline std::optional<AppIdentity::Ico::Picked> iconImage(const SPe& pe) {
        try {
            const auto ICONS  = resources(pe, 3 /* RT_ICON */);
            const auto GROUPS = resources(pe, 14 /* RT_GROUP_ICON */);
            if (ICONS.empty() || GROUPS.empty())
                return std::nullopt;
            auto cands = AppIdentity::Ico::directory(GROUPS.front().data, 14);
            AppIdentity::Ico::sortBest(cands);
            for (const auto& C : cands) {
                const auto IT = std::ranges::find_if(ICONS, [&](const SRes& r) { return r.id == C.ref; });
                if (IT == ICONS.end())
                    continue;
                if (auto p = AppIdentity::Ico::decode(IT->data))
                    return p;
            }
        } catch (...) {}
        return std::nullopt;
    }
}
