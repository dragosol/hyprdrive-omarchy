#pragma once

// Windows icons: an .ico file (Steam's steam/games/<hash>.ico) and the icon
// group of a PE (WinExe.hpp) share one image format -- each image is either a
// PNG, kept as its bytes, or a BMP-style DIB without its file header, decoded
// here into premultiplied ARGB32 rows (cairo's format). 32-bit DIBs only, with
// the AND mask applied when the alpha channel is empty (old-style icons).
// Pure: the PNG writing is IconPng.hpp's.

#include "Caps.hpp"

#include <optional>
#include <vector>

namespace Hyprdrive::AppIdentity::Ico {
    struct Image {
        int                   w = 0, h = 0;
        std::vector<uint32_t> argb; // premultiplied, top row first
    };
    struct Picked {
        int                  size = 0; // the image's width in px
        std::vector<uint8_t> png;      // the image as a PNG, when it is one
        Image                image;    // else decoded
        bool                 isPng() const {
            return !png.empty();
        }
    };
    // One directory entry: an .ico file's (16 bytes: ..., bytes, offset) or
    // an RT_GROUP_ICON's (14 bytes: ..., bytes, RT_ICON id).
    struct Entry {
        int      size = 0, bpp = 0;
        uint32_t bytes = 0, ref = 0;
    };

    inline constexpr size_t MAX_IMAGES = 256;

    inline std::vector<Entry> directory(Bytes in, size_t entrySize) {
        std::vector<Entry> out;
        uint16_t           reserved = 0, type = 0, count = 0;
        if (!rd(in, 0, reserved) || !rd(in, 2, type) || !rd(in, 4, count) || reserved != 0 || type != 1)
            return out;
        for (size_t i = 0; i < count && i < MAX_IMAGES; ++i) {
            const size_t AT = 6 + i * entrySize;
            uint8_t      w  = 0;
            uint16_t     bpp = 0;
            uint32_t     bytes = 0;
            if (!rd(in, AT, w) || !rd(in, AT + 6, bpp) || !rd(in, AT + 8, bytes))
                break;
            Entry e{.size = w ? w : 256, .bpp = bpp, .bytes = bytes};
            if (entrySize == 14) {
                uint16_t id = 0;
                if (!rd(in, AT + 12, id))
                    break;
                e.ref = id;
            } else if (!rd(in, AT + 12, e.ref))
                break;
            out.push_back(e);
        }
        return out;
    }

    // Largest first, deeper colour breaking a tie.
    inline void sortBest(std::vector<Entry>& v) {
        std::ranges::stable_sort(v, [](const Entry& a, const Entry& b) { return a.size != b.size ? a.size > b.size : a.bpp > b.bpp; });
    }

    inline std::optional<Picked> decode(Bytes d) {
        if (d.size() > Caps::ICO)
            return std::nullopt;
        if (d.size() > 24 && std::memcmp(d.data(), "\x89PNG\r\n\x1a\n", 8) == 0) {
            // IHDR's width, big-endian.
            const uint32_t W = (uint32_t{d[16]} << 24) | (uint32_t{d[17]} << 16) | (uint32_t{d[18]} << 8) | d[19];
            if (W == 0 || W > static_cast<uint32_t>(Caps::ICON_PX))
                return std::nullopt;
            return Picked{.size = static_cast<int>(W), .png = std::vector<uint8_t>(d.begin(), d.end())};
        }
        uint32_t hdr = 0;
        int32_t  w = 0, h2 = 0;
        uint16_t bpp = 0;
        if (!rd(d, 0, hdr) || !rd(d, 4, w) || !rd(d, 8, h2) || !rd(d, 14, bpp))
            return std::nullopt;
        if (hdr < 40 || hdr > 1024 || bpp != 32 || w <= 0 || w > Caps::ICON_PX || h2 <= 0 || h2 > 2 * Caps::ICON_PX)
            return std::nullopt;
        const int    H     = h2 / 2;
        const size_t XOR   = static_cast<size_t>(w) * H * 4;
        const size_t MASKW = ((static_cast<size_t>(w) + 31) / 32) * 4;
        if (H <= 0 || hdr > d.size() || XOR > d.size() - hdr)
            return std::nullopt;
        const bool HASMASK = MASKW * H <= d.size() - hdr - XOR;
        bool       anyAlpha = false;
        for (size_t k = 0; k < static_cast<size_t>(w) * H && !anyAlpha; ++k)
            anyAlpha = d[hdr + k * 4 + 3] != 0;
        Picked p{.size = w};
        p.image.w = w;
        p.image.h = H;
        p.image.argb.resize(static_cast<size_t>(w) * H);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < w; ++x) {
                const size_t   ROW = static_cast<size_t>(H - 1 - y); // bottom-up
                const uint8_t* P   = d.data() + hdr + (ROW * w + x) * 4;
                unsigned       a   = P[3];
                if (!anyAlpha) {
                    a = 255;
                    if (HASMASK && (d[hdr + XOR + ROW * MASKW + x / 8] >> (7 - x % 8) & 1))
                        a = 0;
                }
                const uint32_t R = P[2] * a / 255, G = P[1] * a / 255, B = P[0] * a / 255;
                p.image.argb[static_cast<size_t>(y) * w + x] = (uint32_t{a} << 24) | (R << 16) | (G << 8) | B;
            }
        return p;
    }

    // The best image of an .ico file that decodes.
    inline std::optional<Picked> best(Bytes ico) {
        if (ico.size() > Caps::ICO)
            return std::nullopt;
        auto entries = directory(ico, 16);
        sortBest(entries);
        for (const auto& E : entries) {
            if (E.ref >= ico.size() || E.bytes > ico.size() - E.ref)
                continue;
            if (auto p = decode(ico.subspan(E.ref, E.bytes)))
                return p;
        }
        return std::nullopt;
    }
}
