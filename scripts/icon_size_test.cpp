// Host test for IconSize.hpp: the icon texture size buckets, the never-upscale
// fit, and that the downscale really filters (a 1 px stripe pattern resampled
// 4x smaller comes out flat grey, where a single bilinear step aliases it).
#include "../IconSize.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>

using namespace OverviewRender::IconSize;

static int g_checks = 0, g_failed = 0;
#define CHECK(c)                                                                                                                                                                   \
    do {                                                                                                                                                                           \
        ++g_checks;                                                                                                                                                                \
        if (!(c)) {                                                                                                                                                                \
            ++g_failed;                                                                                                                                                            \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c);                                                                                                               \
        }                                                                                                                                                                          \
    } while (0)

// Vertical 1 px stripes, black/white, opaque.
static cairo_surface_t* stripes(int w, int h) {
    auto* s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    auto* d = cairo_image_surface_get_data(s);
    const int ST = cairo_image_surface_get_stride(s);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            auto* p = reinterpret_cast<uint32_t*>(d + y * ST) + x;
            *p      = (x & 1) ? 0xFFFFFFFFu : 0xFF000000u;
        }
    cairo_surface_mark_dirty(s);
    return s;
}

// min/max of the green channel and min alpha over the whole surface.
static void range(cairo_surface_t* s, int& lo, int& hi, int& aLo) {
    cairo_surface_flush(s);
    auto* d = cairo_image_surface_get_data(s);
    const int W = cairo_image_surface_get_width(s), H = cairo_image_surface_get_height(s), ST = cairo_image_surface_get_stride(s);
    lo = 255, hi = 0, aLo = 255;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const uint32_t P = reinterpret_cast<uint32_t*>(d + y * ST)[x];
            const int      G = (P >> 8) & 0xFF, A = P >> 24;
            lo = std::min(lo, G), hi = std::max(hi, G), aLo = std::min(aLo, A);
        }
}

int main() {
    // bucket: nothing asked -> 0; otherwise the drawn size itself, 8..512.
    CHECK(bucket(0) == 0);
    CHECK(bucket(-3) == 0);
    CHECK(bucket(std::nan("")) == 0);
    CHECK(bucket(1) == 8);
    CHECK(bucket(54) == 54);   // fedora 4K at scale 1: a 54 px cell, a 54 px texture
    CHECK(bucket(86.4) == 86); // the XPS at 1.6
    CHECK(bucket(53.6) == 54);
    CHECK(bucket(4000) == 512);
    // Every dock-sized draw maps 1:1: no GL_LINEAR minification left at all.
    for (double d = 8; d <= 512; d += 1.0)
        CHECK(bucket(d) == static_cast<int>(d));

    // fitDown: longer side to px, never up, aspect kept.
    CHECK(fitDown(256, 256, 64).w == 64 && fitDown(256, 256, 64).h == 64);
    CHECK(fitDown(48, 48, 64).w == 48);
    CHECK(fitDown(256, 128, 64).w == 64 && fitDown(256, 128, 64).h == 32);
    CHECK(fitDown(0, 10, 64).w == 0);
    CHECK(fitDown(300, 200, 0).w == 300); // no size asked: as it is

    // downscale: no-op when already small enough.
    {
        auto* S = stripes(48, 48);
        CHECK(downscale(S, 64) == nullptr);
        CHECK(downscale(nullptr, 64) == nullptr);
        cairo_surface_destroy(S);
    }
    // The filter: 1 px stripes at 256 -> 54 (the 4K dock cell) must average to grey (~127)
    // everywhere, opaque to the edge. A single GL_LINEAR-like step lands on
    // whole texels and keeps them black or white.
    {
        auto* S   = stripes(256, 256);
        auto* OUT = downscale(S, 54);
        CHECK(OUT != nullptr);
        if (OUT) {
            CHECK(cairo_image_surface_get_width(OUT) == 54 && cairo_image_surface_get_height(OUT) == 54);
            int lo, hi, aLo;
            range(OUT, lo, hi, aLo);
            CHECK(lo >= 100 && hi <= 155);
            CHECK(aLo == 255);
            std::printf("stripes 256->54: green %d..%d, alpha >= %d\n", lo, hi, aLo);
            cairo_surface_destroy(OUT);
        }
        // For contrast: what bilinear does to the same pattern.
        auto* BL = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 54, 54);
        auto* cr = cairo_create(BL);
        cairo_scale(cr, 54.0 / 256, 54.0 / 256);
        cairo_set_source_surface(cr, S, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
        cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
        cairo_paint(cr);
        cairo_destroy(cr);
        int lo, hi, aLo;
        range(BL, lo, hi, aLo);
        std::printf("stripes 256->54 bilinear (for contrast): green %d..%d\n", lo, hi);
        cairo_surface_destroy(BL);
        cairo_surface_destroy(S);
    }

    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
