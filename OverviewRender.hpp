#pragma once

#include "globals.hpp"

#include <hyprland/src/render/types.hpp>

#include <string>

namespace OverviewRender {

void flushPass(PHLMONITOR monitor);
// No-op when decoration:blur:enabled is off, so overview chrome (search, strip,
// result cards) does not frost on a theme with no window blur.
void queueBlur(const CBox& box, int rounding, float roundingPower, float alpha, bool usePrecomputedBlur, int blurPasses = -1);

// Search bar, result cards, and the workspace strip: the same plate the dock
// wears (Omarchy terminal background, accent border, frost only if window blur
// is on). Rounding is the caller's -- themeRounding for these surfaces, the
// Dash-to-Dock 24 stays on the dock itself.
void themePlate(const CBox& box, int rounding, float roundingPower, float alpha, bool usePrecomputedBlur);

// X92. Stable dual-kawase of the overview main FB for strip/result chrome.
// blurMainFramebuffer() returns a TEMPORARY texture that window live-blurs
// overwrite in the same frame — storing only that pointer made the strip frost
// depend on whether tiles ran. captureFrameChromeBlur copies into a private FB.
void                 captureFrameChromeBlur(PHLMONITOR monitor);
SP<Render::ITexture> frameChromeBlur();
void                 clearFrameChromeBlur();

// The texture for a desktop-entry Icon= value, decoded and uploaded once.
// AppSearch deliberately stops at the file path; the upload is render's job,
// and both the overview's result rows and the dock need the same cache.
// `tintWhite` rewrites the artwork to white, keeping its alpha. Symbolic icons
// are monochrome near-black shapes -- GTK recolours them from the stylesheet at
// draw time and this render path cannot, so an icon meant to sit on a dark
// surface arrives invisible. Cached separately from the untinted one.
SP<Render::ITexture> appIcon(const std::string& iconName, bool tintWhite = false);

// OVERVIEW_CPP_SPEC 13.12 X57. The label plate, drawn ONCE for both surfaces
// that need one: hyprgnomedock's icon tooltip and the overview tile's caption.
// GNOME styles its `.dash-label` and its `.window-caption` with a single css
// rule, so a single function here is the same decision -- and it is what stops
// the two drifting apart the next time one of them is tuned.
//
// Centred horizontally on `cx`, with the plate's BOTTOM edge at `bottomY`.
// Returns the plate box, or an empty box if the text could not be rendered.
CBox labelPill(PHLMONITOR monitor, const std::string& str, double cx, double bottomY, float alpha, float scale);
// Where labelPill WOULD draw, without drawing: the same box it computes, so a
// caller can dodge something before committing to a position.
CBox labelPillBox(PHLMONITOR monitor, const std::string& str, double cx, double bottomY, float scale);

// How far a label plate floats above the plate it labels, in logical px: the
// dock tooltip above the pill, and the Scratchpad label above its strip plate
// (user, 2026-09-14: "same padding distance as tooltips from dock icons").
inline constexpr float LABEL_PILL_GAP = 8.F;

// g_pHyprRenderer->renderText(), cached on everything that changes the pixels.
//
// The uncached call rasterises through pango/cairo and then UPLOADS a texture,
// and every caller in this plugin is in a render path -- the dock's tooltip is
// rebuilt on every frame the pointer rests on an icon, and a menu row on every
// frame the menu is open. On this VM's virgl the upload is also a copy across
// to the host, which makes it the most expensive thing a frame can repeat.
//
// Same signature and argument order as the renderer's own call, so a site
// becomes cached by gaining a prefix and nothing else.
SP<Render::ITexture> text(const std::string& str, const CHyprColor& color, int fontSize, bool italic = false, const std::string& fontFamily = "", int maxWidth = 0,
                          int weight = 0);
// Pango wrap inside maxW×maxH. Used by the inspector so long paths and hints
// stay inside the results plate instead of painting off the card.
SP<Render::ITexture> textWrapped(const std::string& str, const CHyprColor& color, int fontSize, int maxW, int maxH, bool italic = false, const std::string& fontFamily = "",
                                 int weight = 0);

// Drop the text cache. Called when an overview goes away: the strings it held
// were window titles and search results that the next one will not ask for.
void                 clearTextCache();

}
