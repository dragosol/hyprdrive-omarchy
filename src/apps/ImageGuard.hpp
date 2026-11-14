#pragma once
// How big an image file decodes, read from its header without decoding it.
//
// WHY (marketplace review, #13890). An icon is decoded on the compositor's own
// thread, whole, before it is scaled down. An icon is normally a file a
// package installed, but a window's app id reaches the same code, and an app
// id may be an absolute path: any program could point the dock at an image of
// its choosing -- 30 000 x 30 000 px in a file of a few kilobytes -- and have
// Hyprland allocate gigabytes or stall. So nothing is decoded as an icon
// before its size is known and within budget.
//
// decodable(path, maxPixels) is the one question callers ask:
//   * a file over MAX_FILE_BYTES: no
//   * PNG, JPEG, WebP, BMP, AVIF/HEIF, JPEG XL (the rasters hyprgraphics
//     decodes): yes when width x height <= maxPixels; no when larger, or when
//     the header does not give a size (a file that claims a format and hides
//     its size is not given the benefit of the doubt)
//   * anything else (an SVG, which is rasterised at the size asked; a file
//     that is no image at all, which the decoder rejects): yes
//
// Pure: no Hyprland headers. tests/unit/image_guard_test.cpp.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Hyprdrive::ImageGuard {

inline constexpr uint64_t MAX_FILE_BYTES  = 64ULL << 20;      // no icon is 64 MB
inline constexpr uint64_t MAX_ICON_PIXELS = 4096ULL * 4096ULL; // 64 MB decoded, RGBA
inline constexpr size_t   HEADER_BYTES    = 1 << 20;           // how far a size is looked for

struct SSize {
    uint64_t w = 0, h = 0;
};

enum class EKind { Unknown, Png, Jpeg, Webp, Bmp, Avif, Jxl };

namespace detail {
    inline uint32_t be32(const uint8_t* p) {
        return (uint32_t{p[0]} << 24) | (uint32_t{p[1]} << 16) | (uint32_t{p[2]} << 8) | p[3];
    }
    inline uint32_t le32(const uint8_t* p) {
        return (uint32_t{p[3]} << 24) | (uint32_t{p[2]} << 16) | (uint32_t{p[1]} << 8) | p[0];
    }
    inline uint32_t be16(const uint8_t* p) {
        return (uint32_t{p[0]} << 8) | p[1];
    }
    inline uint32_t le16(const uint8_t* p) {
        return (uint32_t{p[1]} << 8) | p[0];
    }
    inline bool at(std::string_view d, size_t off, std::string_view what) {
        return d.size() >= off + what.size() && d.substr(off, what.size()) == what;
    }
    inline const uint8_t* u8(std::string_view d, size_t off = 0) {
        return reinterpret_cast<const uint8_t*>(d.data()) + off;
    }

    inline std::optional<SSize> png(std::string_view d) {
        if (d.size() < 24 || !at(d, 12, "IHDR"))
            return std::nullopt;
        return SSize{be32(u8(d, 16)), be32(u8(d, 20))};
    }

    // The first frame header (SOFn). Markers with no length: RSTn, SOI, EOI, TEM.
    inline std::optional<SSize> jpeg(std::string_view d) {
        size_t i = 2;
        while (i + 4 <= d.size()) {
            if (u8(d)[i] != 0xFF) {
                ++i;
                continue;
            }
            const uint8_t M = u8(d)[i + 1];
            if (M == 0xFF) {
                ++i;
                continue;
            }
            if (M == 0x00 || M == 0x01 || (M >= 0xD0 && M <= 0xD9)) {
                i += 2;
                continue;
            }
            const bool SOF = M >= 0xC0 && M <= 0xCF && M != 0xC4 && M != 0xC8 && M != 0xCC;
            if (SOF) {
                if (i + 9 > d.size())
                    return std::nullopt;
                return SSize{be16(u8(d, i + 7)), be16(u8(d, i + 5))};
            }
            i += 2 + be16(u8(d, i + 2));
        }
        return std::nullopt;
    }

    inline std::optional<SSize> webp(std::string_view d) {
        if (d.size() < 30)
            return std::nullopt;
        if (at(d, 12, "VP8X"))
            return SSize{uint64_t{le32(u8(d, 24)) & 0xFFFFFF} + 1, (uint64_t{u8(d)[27]} | (uint64_t{u8(d)[28]} << 8) | (uint64_t{u8(d)[29]} << 16)) + 1};
        if (at(d, 12, "VP8 "))
            return SSize{le16(u8(d, 26)) & 0x3FFF, le16(u8(d, 28)) & 0x3FFF};
        if (at(d, 12, "VP8L")) {
            const uint32_t B = le32(u8(d, 21));
            return SSize{uint64_t{B & 0x3FFF} + 1, uint64_t{(B >> 14) & 0x3FFF} + 1};
        }
        return std::nullopt;
    }

    inline std::optional<SSize> bmp(std::string_view d) {
        if (d.size() < 26)
            return std::nullopt;
        const auto W = static_cast<int64_t>(static_cast<int32_t>(le32(u8(d, 18))));
        const auto H = static_cast<int64_t>(static_cast<int32_t>(le32(u8(d, 22))));
        return SSize{static_cast<uint64_t>(W < 0 ? -W : W), static_cast<uint64_t>(H < 0 ? -H : H)};
    }

    // Every image spatial extent box; the largest is what a decode can cost.
    inline std::optional<SSize> avif(std::string_view d) {
        std::optional<SSize> best;
        for (size_t i = d.find("ispe"); i != std::string_view::npos; i = d.find("ispe", i + 4)) {
            if (i + 16 > d.size())
                break;
            const SSize S{be32(u8(d, i + 8)), be32(u8(d, i + 12))};
            if (!best || S.w * S.h > best->w * best->h)
                best = S;
        }
        return best;
    }

    // JPEG XL's SizeHeader, bit-packed, least significant bit first.
    inline std::optional<SSize> jxlCodestream(std::string_view d) {
        if (d.size() < 2 || u8(d)[0] != 0xFF || u8(d)[1] != 0x0A)
            return std::nullopt;
        size_t     bit = 16;
        bool       ok  = true;
        const auto get = [&](int n) {
            uint64_t v = 0;
            for (int k = 0; k < n; ++k, ++bit) {
                if (bit / 8 >= d.size()) {
                    ok = false;
                    return uint64_t{0};
                }
                v |= uint64_t{(u8(d)[bit / 8] >> (bit % 8)) & 1U} << k;
            }
            return v;
        };
        static constexpr int BITS[4] = {9, 13, 18, 30};
        const bool           SMALL   = get(1);
        const uint64_t       H       = SMALL ? (get(5) + 1) * 8 : get(BITS[get(2)]) + 1;
        const uint64_t       RATIO   = get(3);
        uint64_t             w       = 0;
        switch (RATIO) {
            case 0: w = SMALL ? (get(5) + 1) * 8 : get(BITS[get(2)]) + 1; break;
            case 1: w = H; break;
            case 2: w = H * 12 / 10; break;
            case 3: w = H * 4 / 3; break;
            case 4: w = H * 3 / 2; break;
            case 5: w = H * 16 / 9; break;
            case 6: w = H * 5 / 4; break;
            default: w = H * 2; break;
        }
        if (!ok)
            return std::nullopt;
        return SSize{w, H};
    }

    // The container: boxes of [size:4][type:4]; the codestream is in `jxlc`,
    // or starts in the first `jxlp` (after its 4-byte part index).
    inline std::optional<SSize> jxlContainer(std::string_view d) {
        size_t i = 0;
        while (i + 8 <= d.size()) {
            uint64_t   size = be32(u8(d, i));
            size_t     head = 8;
            const auto TYPE = d.substr(i + 4, 4);
            if (size == 1) {
                if (i + 16 > d.size())
                    return std::nullopt;
                size = (uint64_t{be32(u8(d, i + 8))} << 32) | be32(u8(d, i + 12));
                head = 16;
            }
            if (TYPE == "jxlc")
                return jxlCodestream(d.substr(std::min(d.size(), i + head)));
            if (TYPE == "jxlp")
                return jxlCodestream(d.substr(std::min(d.size(), i + head + 4)));
            if (size == 0 || size < head)
                return std::nullopt; // to the end of the file, or malformed: no codestream seen
            i += size;
        }
        return std::nullopt;
    }
}

inline EKind kindOf(std::string_view d) {
    using detail::at;
    if (at(d, 0, "\x89PNG\r\n\x1a\n"))
        return EKind::Png;
    if (at(d, 0, "\xFF\xD8\xFF"))
        return EKind::Jpeg;
    if (at(d, 0, "RIFF") && at(d, 8, "WEBP"))
        return EKind::Webp;
    if (at(d, 0, "BM") && d.size() >= 14 && detail::le32(detail::u8(d, 6)) == 0) // the two reserved words
        return EKind::Bmp;
    if (at(d, 4, "ftyp"))
        return EKind::Avif; // every ISO-BMFF image: avif, heic, and their sequences
    if (at(d, 0, "\xFF\x0A") || at(d, 0, std::string_view("\x00\x00\x00\x0CJXL \r\n\x87\n", 12)))
        return EKind::Jxl;
    return EKind::Unknown;
}

// The size a decode of this header would produce; nullopt when it gives none.
inline std::optional<SSize> sizeOf(std::string_view d) {
    switch (kindOf(d)) {
        case EKind::Png: return detail::png(d);
        case EKind::Jpeg: return detail::jpeg(d);
        case EKind::Webp: return detail::webp(d);
        case EKind::Bmp: return detail::bmp(d);
        case EKind::Avif: return detail::avif(d);
        case EKind::Jxl: return detail::at(d, 0, "\xFF\x0A") ? detail::jxlCodestream(d) : detail::jxlContainer(d);
        case EKind::Unknown: return std::nullopt;
    }
    return std::nullopt;
}

// The verdict for a header and the file's size (see the top of the file).
inline bool decodable(std::string_view header, uint64_t fileBytes, uint64_t maxPixels) {
    if (fileBytes > MAX_FILE_BYTES)
        return false;
    if (kindOf(header) == EKind::Unknown)
        return true;
    const auto S = sizeOf(header);
    if (!S || S->w == 0 || S->h == 0)
        return false;
    return S->w <= maxPixels && S->h <= maxPixels && S->w * S->h <= maxPixels;
}

// The same for a file. One that cannot be opened is left to the decoder,
// which fails on it the same way.
inline bool decodable(const std::string& path, uint64_t maxPixels = MAX_ICON_PIXELS) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in)
        return true;
    const auto END = in.tellg();
    if (END < 0)
        return true;
    const auto BYTES = static_cast<uint64_t>(END);
    if (BYTES > MAX_FILE_BYTES)
        return false;
    std::string header(std::min<uint64_t>(BYTES, HEADER_BYTES), '\0');
    in.seekg(0);
    in.read(header.data(), static_cast<std::streamsize>(header.size()));
    header.resize(static_cast<size_t>(std::max<std::streamsize>(0, in.gcount())));
    return decodable(std::string_view{header}, BYTES, maxPixels);
}

}
