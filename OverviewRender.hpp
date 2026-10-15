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
// lit = the accent ring (hovered / keyboard on it); otherwise the resting tile border.
void themePlate(const CBox& box, int rounding, float roundingPower, float alpha, bool usePrecomputedBlur, bool lit = true);

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
// `padded`: the same pixels inside a TEX_PAD transparent border -- for anything
// drawn ROTATED, whose hard quad edge would otherwise stair-step (2026-09-24);
// draw it into padBox(box, tex) so the artwork keeps its size.
// `px`: the size it is DRAWN at, in device pixels, when the caller knows it
// and it holds still (the dock's resting cell): the texture is then resampled
// to exactly that size (IconSize.hpp) instead of being minified by GL_LINEAR,
// which stair-steps it. 0 (the default) = the unsized texture: 64 px for an
// SVG, a raster at its own size.
SP<Render::ITexture> appIcon(const std::string& iconName, bool tintWhite = false, bool padded = false, double px = 0.0);
// True (and cleared) when appIcon drew a held neighbouring size of an icon
// while its new size waits for the decode budget: the caller asks for another
// frame so the exact one lands.
bool takeAppIconsPending();

// THE ICON WARMER (user, 2026-09-30: the first open and the first keystrokes
// lagged, then got snappy). Decoding an icon costs ~1-3 ms, and the first
// open/search decoded a dozen or more inside one frame -- late frames that also
// knocked Hyprland's scheduling into half rate for the rest of the animation.
// While no overview is open, a timer decodes ONE icon per tick into the same
// cache: the open windows' badges first, then every installed app (the search
// grid). ~16 KiB each. Started at load, re-armed when the app set changes.
void startIconWarmer();
void stopIconWarmer();
inline constexpr int TEX_PAD = 2;
CBox                 padBox(const CBox& box, const SP<Render::ITexture>& tex);
// An image FILE at `px` (a thumbnail), cached on path + size + mtime.
// `budgeted`: a decode that is not cached yet is skipped (nullptr, not
// remembered) once ~6 ms of decoding has been spent in the current ~16 ms --
// the caller draws a stand-in and gets the image on a later frame, rather than
// one frame stalling on a folder's worth of thumbnails.
SP<Render::ITexture> imageFile(const std::string& path, int px, bool budgeted = false, bool padded = false);
// A filled rounded rectangle as a TEXTURE, so it can be drawn rotated (a rect
// pass element cannot be). Cached by size, radius and colour; cleared with the
// text cache. Dock stacks: the fan's label pills. Always PADDED (see appIcon):
// draw it into padBox().
SP<Render::ITexture> roundedRect(int w, int h, int radius, const CHyprColor& color, const CHyprColor& border = CHyprColor{0, 0, 0, 0}, int borderPx = 0);

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
