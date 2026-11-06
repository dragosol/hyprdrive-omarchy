#pragma once

// HOW BIG AN ICON TEXTURE IS (2026-10-15, the stair-stepped dock icons).
//
// Hyprland samples a texture with plain GL_LINEAR and no mipmaps, and resets
// the min/mag filters per draw, so a texture drawn much smaller than it is
// ALIASES: a 256 px PNG (the largest raster wins the icon index, rightly) in a
// 54 px dock cell skipped three texels in four and came out with stair-stepped
// edges. So an icon is resampled ONCE, on the CPU with a real downscale filter,
// the size it is drawn at, and GL_LINEAR is left nothing to resample.
//
//   bucket(drawPx)  the texture size for a draw of `drawPx` device pixels:
//                   that size EXACTLY (rounded; 8..512). A dock cell is drawn
//                   at whole-pixel boxes, so the texture then maps 1:1 onto the
//                   screen and GL_LINEAR does no resampling at all. Measured
//                   (Thunderbird 256 px PNG, mean abs error vs a LANCZOS resize
//                   at the drawn size, cairo's point-sampled BILINEAR standing in
//                   for GL_LINEAR): drawn from the 256 texture 3.71 at 54 px;
//                   from a 64 px texture (the "round up to a multiple of 8"
//                   alternative) 2.42; from a 54 px one 0.79. The ~1.2x step a
//                   bucket leaves is most of the damage, so there is none. The
//                   hover lean (1.03) magnifies, which GL_LINEAR does well. A
//                   size slider dragged across many sizes is the cache's
//                   problem, not this one's: OverviewRender keeps a few sizes
//                   per icon and draws a neighbouring one while a new one waits.
//   fitDown(w,h,px) a W x H source scaled so its longer side is `px` -- never
//                   UP (a 48 px PNG asked for 64 stays 48; GL_LINEAR is fine
//                   magnifying, and inventing texels would only blur it twice).
//   downscale()     that resample: cairo's BEST filter (pixman's separable
//                   convolution -- measured closest to a LANCZOS reference of
//                   every cairo filter: mean abs error 0.8 vs GOOD 1.2, single-
//                   step BILINEAR 3.7, Thunderbird 256 -> 54), on premultiplied
//                   pixels, edges padded so an opaque square icon keeps
//                   opaque edges.
//
// Pure: cairo only, no Hyprland. scripts/icon_size_test.cpp builds it on the
// host.

#include <cairo/cairo.h>

#include <algorithm>
#include <cmath>

namespace OverviewRender::IconSize {

    inline constexpr int MIN_PX = 8;
    inline constexpr int MAX_PX = 512;

    // 0 for "no size asked" (a draw of nothing, or a NaN).
    inline int bucket(double drawPx) {
        if (!(drawPx > 0.0))
            return 0;
        return static_cast<int>(std::lround(std::clamp(drawPx, static_cast<double>(MIN_PX), static_cast<double>(MAX_PX))));
    }

    struct SSize {
        int w = 0, h = 0;
    };

    inline SSize fitDown(int w, int h, int px) {
        if (w < 1 || h < 1)
            return {};
        if (px < 1 || std::max(w, h) <= px)
            return {w, h};
        const double K = static_cast<double>(px) / std::max(w, h);
        return {std::max(1, static_cast<int>(std::lround(w * K))), std::max(1, static_cast<int>(std::lround(h * K)))};
    }

    // A NEW ARGB32 surface holding `src` resampled to fit `px` (the caller
    // destroys it), or nullptr when the source is already no bigger than that
    // or cannot be read -- draw the source as it is then.
    inline cairo_surface_t* downscale(cairo_surface_t* src, int px) {
        if (!src || cairo_surface_status(src) != CAIRO_STATUS_SUCCESS || cairo_surface_get_type(src) != CAIRO_SURFACE_TYPE_IMAGE)
            return nullptr;
        const int W = cairo_image_surface_get_width(src), H = cairo_image_surface_get_height(src);
        const auto TO = fitDown(W, H, px);
        if (TO.w < 1 || TO.h < 1 || (TO.w == W && TO.h == H))
            return nullptr;

        cairo_surface_t* out = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, TO.w, TO.h);
        if (cairo_surface_status(out) != CAIRO_STATUS_SUCCESS) {
            cairo_surface_destroy(out);
            return nullptr;
        }
        cairo_t* cr = cairo_create(out);
        cairo_scale(cr, static_cast<double>(TO.w) / W, static_cast<double>(TO.h) / H);
        cairo_set_source_surface(cr, src, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BEST);
        cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_PAD);
        cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
        cairo_paint(cr);
        const bool OK = cairo_status(cr) == CAIRO_STATUS_SUCCESS;
        cairo_destroy(cr);
        cairo_surface_flush(out);
        if (!OK) {
            cairo_surface_destroy(out);
            return nullptr;
        }
        return out;
    }
}
