// Host test for ImageGuard.hpp: an image's size is read from its header, and
// one too big to decode as an icon is refused before it is decoded.
//
// With a file argument it prints what the guard reads from that file
// ("<kind> <w> <h> <decodable>"), which is how the parsers were checked
// against images written by real encoders.
#include "ImageGuard.hpp"

#include <cstdio>
#include <string>

using namespace Hyprdrive::ImageGuard;

static int g_checks = 0, g_failed = 0;
#define CHECK(c)                                                                                                                                                                   \
    do {                                                                                                                                                                           \
        ++g_checks;                                                                                                                                                                \
        if (!(c)) {                                                                                                                                                                \
            ++g_failed;                                                                                                                                                            \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c);                                                                                                               \
        }                                                                                                                                                                          \
    } while (0)

static std::string be32(uint32_t v) {
    return {char(v >> 24), char(v >> 16), char(v >> 8), char(v)};
}
static std::string le32(uint32_t v) {
    return {char(v), char(v >> 8), char(v >> 16), char(v >> 24)};
}
static std::string be16(uint32_t v) {
    return {char(v >> 8), char(v)};
}

static std::string png(uint32_t w, uint32_t h) {
    return std::string("\x89PNG\r\n\x1a\n", 8) + be32(13) + "IHDR" + be32(w) + be32(h) + std::string("\x08\x06\0\0\0", 5);
}
static std::string jpeg(uint32_t w, uint32_t h) {
    // SOI, an APP0 to step over, then SOF2 (progressive).
    return std::string("\xFF\xD8", 2) + std::string("\xFF\xE0", 2) + be16(16) + std::string(14, 'J') + std::string("\xFF\xC2", 2) + be16(17) + '\x08' + be16(h) + be16(w) +
        std::string(12, '\0');
}
static std::string bmp(int32_t w, int32_t h) {
    return "BM" + le32(0) + le32(0) + le32(54) + le32(40) + le32(uint32_t(w)) + le32(uint32_t(h)) + std::string(28, '\0');
}
static std::string webpX(uint32_t w, uint32_t h) {
    const uint32_t W = w - 1, H = h - 1;
    return "RIFF" + le32(0) + "WEBP" + "VP8X" + le32(10) + le32(0) + std::string{char(W), char(W >> 8), char(W >> 16), char(H), char(H >> 8), char(H >> 16)};
}
static std::string webpL(uint32_t w, uint32_t h) {
    return "RIFF" + le32(0) + "WEBP" + "VP8L" + le32(0) + '\x2f' + le32((w - 1) | ((h - 1) << 14)) + std::string(8, '\0');
}
static std::string avif(uint32_t w, uint32_t h) {
    return be32(24) + "ftypavif" + std::string(12, '\0') + be32(20) + "ispe" + be32(0) + be32(w) + be32(h);
}
// A JPEG XL codestream with explicit 30-bit height and width.
static std::string jxl(uint32_t w, uint32_t h) {
    std::string out("\xFF\x0A", 2);
    uint64_t    acc = 0;
    int         n   = 0;
    const auto  put = [&](uint64_t v, int bits) {
        acc |= v << n;
        n += bits;
        while (n >= 8) {
            out += char(acc & 0xFF);
            acc >>= 8;
            n -= 8;
        }
    };
    put(0, 1);      // not "small"
    put(3, 2);      // 30-bit height
    put(h - 1, 30);
    put(0, 3);      // no fixed ratio
    put(3, 2);      // 30-bit width
    put(w - 1, 30);
    put(0, 8);
    return out;
}
static std::string jxlBoxed(uint32_t w, uint32_t h) {
    const auto C = jxl(w, h);
    return std::string("\x00\x00\x00\x0CJXL \r\n\x87\n", 12) + be32(20) + "ftypjxl " + std::string(8, '\0') + be32(uint32_t(8 + C.size())) + "jxlc" + C;
}

static bool sized(const std::string& d, EKind kind, uint64_t w, uint64_t h) {
    const auto S = sizeOf(d);
    return kindOf(d) == kind && S && S->w == w && S->h == h;
}

int main(int argc, char** argv) {
    if (argc > 1) {
        std::ifstream in(argv[1], std::ios::binary);
        std::string   d((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        d.resize(std::min(d.size(), HEADER_BYTES));
        const auto S = sizeOf(d);
        std::printf("%d %llu %llu %d\n", int(kindOf(d)), S ? (unsigned long long)S->w : 0ULL, S ? (unsigned long long)S->h : 0ULL, decodable(std::string(argv[1])) ? 1 : 0);
        return 0;
    }

    // Each format's size is read.
    CHECK(sized(png(256, 128), EKind::Png, 256, 128));
    CHECK(sized(jpeg(640, 480), EKind::Jpeg, 640, 480));
    CHECK(sized(bmp(100, -50), EKind::Bmp, 100, 50)); // top-down: a negative height
    CHECK(sized(webpX(1000, 700), EKind::Webp, 1000, 700));
    CHECK(sized(webpL(300, 200), EKind::Webp, 300, 200));
    CHECK(sized(avif(512, 512), EKind::Avif, 512, 512));
    CHECK(sized(jxl(800, 600), EKind::Jxl, 800, 600));
    CHECK(sized(jxlBoxed(800, 600), EKind::Jxl, 800, 600));

    // An icon is decoded; the same file with a huge size in its header is not.
    for (const auto& make : {+[](uint32_t w, uint32_t h) { return png(w, h); }, +[](uint32_t w, uint32_t h) { return jpeg(w, h); },
                             +[](uint32_t w, uint32_t h) { return bmp(int32_t(w), int32_t(h)); }, +[](uint32_t w, uint32_t h) { return webpX(w, h); },
                             +[](uint32_t w, uint32_t h) { return avif(w, h); }, +[](uint32_t w, uint32_t h) { return jxl(w, h); },
                             +[](uint32_t w, uint32_t h) { return jxlBoxed(w, h); }}) {
        CHECK(decodable(make(256, 256), 4096, MAX_ICON_PIXELS));
        CHECK(decodable(make(4096, 4096), 4096, MAX_ICON_PIXELS));  // exactly the budget
        CHECK(!decodable(make(30000, 30000), 4096, MAX_ICON_PIXELS)); // 3.6 GB decoded
        CHECK(!decodable(make(60000, 300), 4096, MAX_ICON_PIXELS));   // over on area, a strip
    }
    CHECK(!decodable(png(0x7FFFFFFF, 0x7FFFFFFF), 4096, MAX_ICON_PIXELS)); // the product does not wrap

    // A file that claims a format and gives no size is refused.
    CHECK(!decodable(std::string("\x89PNG\r\n\x1a\n", 8), 8, MAX_ICON_PIXELS));
    CHECK(!decodable(std::string("\xFF\xD8\xFF\xE0", 4) + be16(4) + "xx", 8, MAX_ICON_PIXELS));
    CHECK(!decodable(png(0, 64), 64, MAX_ICON_PIXELS));

    // What is no raster is left to the decoder: an SVG, text.
    CHECK(decodable(std::string("<svg xmlns='http://www.w3.org/2000/svg'/>"), 40, MAX_ICON_PIXELS));
    CHECK(decodable(std::string("not an image"), 12, MAX_ICON_PIXELS));
    CHECK(decodable(std::string(), 0, MAX_ICON_PIXELS));

    // A file too big to be an icon, whatever it is.
    CHECK(!decodable(png(64, 64), MAX_FILE_BYTES + 1, MAX_ICON_PIXELS));
    CHECK(!decodable(std::string("<svg/>"), MAX_FILE_BYTES + 1, MAX_ICON_PIXELS));

    // A path that cannot be opened is the decoder's to fail on.
    CHECK(decodable(std::string("/nonexistent/hyprdrive-image-guard")));

    std::printf("image_guard_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
