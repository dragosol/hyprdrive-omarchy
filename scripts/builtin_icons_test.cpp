// Host test for BuiltinIcons (dock stacks C): every embedded SVG is there,
// takes the accent, and librsvg renders it to something with real coverage.
#include "../BuiltinIcons.hpp"

#include <librsvg/rsvg.h>

#include <cstdio>
#include <string>
#include <vector>

using namespace ScrollOverview;

static int g_checks = 0, g_failed = 0;
#define CHECK(c)                                                                                                                                                                   \
    do {                                                                                                                                                                           \
        ++g_checks;                                                                                                                                                                \
        if (!(c)) {                                                                                                                                                                \
            ++g_failed;                                                                                                                                                            \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c);                                                                                                               \
        }                                                                                                                                                                          \
    } while (0)

// The fraction of a 128 px render that is not transparent.
static double coverage(const std::string& svg) {
    GError*     err = nullptr;
    RsvgHandle* h   = rsvg_handle_new_from_data(reinterpret_cast<const guint8*>(svg.data()), svg.size(), &err);
    if (!h) {
        if (err)
            g_error_free(err);
        return -1;
    }
    auto*              surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 128, 128);
    auto*              cr   = cairo_create(surf);
    const RsvgRectangle VP{0, 0, 128, 128};
    const bool         OK   = rsvg_handle_render_document(h, cr, &VP, nullptr);
    cairo_surface_flush(surf);
    const auto* data   = cairo_image_surface_get_data(surf);
    const int   stride = cairo_image_surface_get_stride(surf);
    int         lit    = 0;
    for (int y = 0; y < 128; ++y)
        for (int x = 0; x < 128; ++x)
            lit += data[y * stride + x * 4 + 3] > 16;
    cairo_destroy(cr);
    cairo_surface_destroy(surf);
    g_object_unref(h);
    return OK ? lit / (128.0 * 128.0) : -1;
}

int main() {
    std::vector<std::string> names{std::string(BuiltinIcons::STACK_EMPTY)};
    for (const bool EMPTY : {true, false})
        for (const char* T : {"light", "dark", "clear"})
            for (const char* P : {"white", "color"})
                names.push_back(BuiltinIcons::trashName(EMPTY, T, P));
    for (const auto& NAME : names) {
        const auto SVG = BuiltinIcons::svg(NAME, "#12ab34");
        CHECK(SVG.find("#A_") == std::string::npos && SVG.find("#G_") == std::string::npos && SVG.find("#LIP") == std::string::npos);
        CHECK(SVG.starts_with("<svg"));
        CHECK(SVG.find("#ACCENT") == std::string::npos);
        const double C = coverage(SVG);
        CHECK(C > 0.25 && C < 0.9);
    }
    CHECK(BuiltinIcons::svg(BuiltinIcons::STACK_EMPTY, "#12ab34").find("#12ab34") != std::string::npos);
    CHECK(BuiltinIcons::svg("hyprdrive-nope", "#fff").empty());
    CHECK(BuiltinIcons::trashName(true, "x", "y") == "hyprdrive-trash-empty-light-white");
    CHECK(BuiltinIcons::trashName(false, "dark", "color") == "hyprdrive-trash-full-dark-color");
    CHECK(BuiltinIcons::svg(BuiltinIcons::trashName(true, "light", "white"), "#fff").find("<!--PAPERS-->") == std::string::npos);
    CHECK(BuiltinIcons::svg(BuiltinIcons::trashName(false, "light", "white"), "#fff").find("iri)\"") == std::string::npos ||
          BuiltinIcons::svg(BuiltinIcons::trashName(false, "light", "white"), "#fff").find("<!--COLOR") == std::string::npos);
    std::printf("builtin_icons_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
