#pragma once

// An Ico::Picked written as a PNG file: a PNG image as it is, a decoded one
// through cairo. The one piece of the module that needs cairo, kept apart so
// the parsers and the resolvers build without it. Writes to a temporary name
// and renames, so a reader never sees half a file.

#include "WinExe.hpp"

#include <cairo/cairo.h>

#include <cstdio>
#include <fstream>
#include <string>

namespace Hyprdrive::AppIdentity::IconPng {
    inline bool write(const Ico::Picked& p, const std::string& out) {
        const std::string TMP = out + ".tmp";
        bool              ok  = false;
        if (p.isPng()) {
            std::ofstream f(TMP, std::ios::binary | std::ios::trunc);
            f.write(reinterpret_cast<const char*>(p.png.data()), static_cast<std::streamsize>(p.png.size()));
            ok = f.good();
        } else if (p.image.w > 0 && p.image.h > 0 && p.image.argb.size() == static_cast<size_t>(p.image.w) * p.image.h) {
            cairo_surface_t* s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, p.image.w, p.image.h);
            if (cairo_surface_status(s) == CAIRO_STATUS_SUCCESS) {
                cairo_surface_flush(s);
                uint8_t*  px     = cairo_image_surface_get_data(s);
                const int STRIDE = cairo_image_surface_get_stride(s);
                for (int y = 0; y < p.image.h; ++y)
                    std::memcpy(px + static_cast<size_t>(y) * STRIDE, p.image.argb.data() + static_cast<size_t>(y) * p.image.w, static_cast<size_t>(p.image.w) * 4);
                cairo_surface_mark_dirty(s);
                ok = cairo_surface_write_to_png(s, TMP.c_str()) == CAIRO_STATUS_SUCCESS;
            }
            cairo_surface_destroy(s);
        }
        if (ok)
            ok = std::rename(TMP.c_str(), out.c_str()) == 0;
        if (!ok)
            std::remove(TMP.c_str());
        return ok;
    }
}

namespace Hyprdrive::WinExe {
    // The program's icon, largest first, written as a PNG to `out`.
    inline bool icon(const SPe& pe, const std::string& out) {
        const auto P = iconImage(pe);
        return P && AppIdentity::IconPng::write(*P, out);
    }
}
