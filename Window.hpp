#pragma once

#include "globals.hpp"
#include <hyprland/src/helpers/time/Time.hpp>

class CWLSurfaceResource;

namespace OverviewWindow {

bool shouldBlurBackground(const PHLWINDOW& window);
bool shouldUsePrecomputedBlur(const PHLWINDOW& window, PHLMONITOR monitor = nullptr, const CBox* workspaceBox = nullptr, const CBox* windowBox = nullptr, bool dragged = false);
bool shouldUseBlurFramebuffer(const PHLWINDOW& window, PHLMONITOR monitor = nullptr, const CBox* workspaceBox = nullptr, const CBox* windowBox = nullptr);
// Drops the U2 texture-prefilter cache. A no-op while any overview is still
// up: the cache is shared by every overview (both monitors sample the same
// client textures), and ~CScrollOverview used to clear it for ALL of them
// whenever one closed. Must be called with the EGL context current.
void releaseTexturePrefilterCache();
// hyprdrive.debug("mem"): entries in the prefilter cache and their bytes
// (RGBA8 plus the mip chain), for measuring.
std::pair<size_t, size_t> texturePrefilterCacheBytes();
// Drops only the entries of windows that are gone; see Window.cpp.
void trimTexturePrefilterCache();
// "This surface committed": marks its cached prefilter copy stale. Called from
// main.cpp's damageSurface hook -- the same signal the overview already uses to
// decide a live tile needs repainting.
void notifySurfaceCommit(const SP<CWLSurfaceResource>& surface);
void forceDecoRecalc(const PHLWINDOW& window);

struct SRenderParams {
    PHLMONITOR            monitor;
    PHLWINDOW             window;
    CBox                  windowBox;
    float                 renderScale          = 1.F;
    Time::steady_tp       now                  = {};
    const CBox*           workspaceBox         = nullptr;
    bool                  selected             = false;
    bool                  dragged              = false;
    // When true (overview tiles), decoration:blur:size is scaled by renderScale
    // for this draw only, and precomputed m_blurFB is NOT used so the desktop
    // blur FB cannot be rebuilt at the temporary strength. Dock previews set
    // false -- they must never touch global blur state.
    bool                  scaleBlurToRenderScale = true;
    // Dock hover panel: render a scaled copy via renderModif WITHOUT rewriting
    // m_realPosition/m_realSize. Mutating geometry left a mini rounded outline
    // at the real window's top-left (tiled position ignores the temp position
    // write; size still shrank).
    bool                  dockLivePreview        = false;
    // Y5. The minimize/restore fade, applied AT RENDER TIME. It deliberately
    // does NOT touch the window's own alpha animation: warping that mutates
    // real compositor state, which outlives the draw and leaks a half-faded
    // window if the flight is ever interrupted. fadeAlpha is the same lever
    // Hyprland's own window fades use.
    float                 alpha                  = 1.F;
    // Y5. Does the CALLER own the render pass? Inside the overview the plugin
    // does, so this path may flush it mid-draw. The dock at RENDER_POST_WINDOWS
    // does NOT: flushing there renders and CLEARS everything Hyprland has queued
    // for the rest of the frame, which wedges the frame loop -- measured, it
    // stranded a restoring window on the stash permanently. False skips the
    // flushes and the decorations that depend on them; everything that makes the
    // frost correct (blur scaled to renderScale, live blur forced) still runs.
    bool                  ownsRenderPass         = true;
};

void renderOverviewWindow(const SRenderParams& params);

}
