#include "Window.hpp"
#include "FlightBlur.hpp"
#include <chrono>
#include <algorithm>
#include <cmath>
#include <dlfcn.h>
#include <functional>
#define private public
#define protected public
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/managers/KeybindManager.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/view/Group.hpp>
#include <hyprland/src/desktop/view/Popup.hpp>
#include <hyprland/src/desktop/view/WLSurface.hpp>
#include <hyprland/src/plugins/PluginSystem.hpp>
#include <hyprland/src/render/pass/Pass.hpp>
#include <hyprland/src/render/pass/RectPassElement.hpp>
#include <hyprland/src/render/pass/BorderPassElement.hpp>
#include <hyprland/src/render/pass/RendererHintsPassElement.hpp>
#include <hyprland/src/render/pass/ClearPassElement.hpp>
#include <hyprland/src/render/Framebuffer.hpp>
#include <hyprland/src/render/pass/SurfacePassElement.hpp>
#include <hyprland/src/render/pass/TexPassElement.hpp>
#include <hyprland/src/render/types.hpp>
#include <hyprland/src/render/decorations/DecorationPositioner.hpp>
#include <hyprland/src/config/shared/complex/ComplexDataTypes.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>
#undef protected
#undef private
#include "Config.hpp"
#include "IOverview.hpp"
#include "OverviewPassElement.hpp"
#include "OverviewRender.hpp"

namespace OverviewWindow {
namespace {

struct SOverviewCustomDecorationRenderState {
    bool                                queuedAny = false;
    std::vector<std::function<void()>> restoreFns;
};

struct SHyprbarButtonMirror {
    std::string         cmd     = "";
    bool                userfg  = false;
    CHyprColor          fgcol   = CHyprColor(0, 0, 0, 0);
    CHyprColor          bgcol   = CHyprColor(0, 0, 0, 0);
    float               size    = 10.F;
    std::string         icon    = "";
    SP<Render::ITexture> iconTex;
};

struct SHyprbarGlobalStateMirror {
    std::vector<SHyprbarButtonMirror> buttons;
};

struct SOverviewWindowMetrics {
    float renderScale             = 1.F;
    float pxScale                 = 1.F;
    float targetOpacity           = 1.F;
    float borderSize              = 0.F;
    int   borderPx                = 0;
    float borderPxScaled          = 0.F;
    float roundingBase            = 0.F;
    float roundingPower           = 2.F;
    float correctionOffset        = 0.F;
    float outerRound              = 0.F;
    int   roundingPx              = 0;
    int   outerRoundPx            = 0;
    float hyprbarLogicalHeight    = 0.F;
    int   hyprbarHeightPx         = 0;
    float hyprbarHeightPxScaled   = 0.F;
    bool  shadowIncludesHyprbar   = false;
    bool  borderIncludesHyprbar   = false;
};

// SurfacePassElement only scales subsurface textures while the window size is
// marked as animated. Overview temporarily changes the current window size
// without starting a real animation, so expose that single bit for the duration
// of the synchronous overview render and restore it immediately afterwards.
struct COverviewAnimatedVariableAccess : Hyprutils::Animation::CBaseAnimatedVariable {
    static void setBeingAnimated(Hyprutils::Animation::CBaseAnimatedVariable* variable, bool animated) {
        if (!variable)
            return;

        rc<COverviewAnimatedVariableAccess*>(variable)->m_bIsBeingAnimated = animated;
    }
};

// THE TILE GEOMETRY, AT DRAW TIME. renderOverviewWindow puts the window at the
// tile's box for the duration of its call, and a caller that owns the pass
// flushes before the box is taken back. A caller that does not (the dock's
// flight at RENDER_POST_WINDOWS, the motion-blurred flight's nested pass) has
// its surfaces drawn LATER, against the window's real geometry -- and
// CSurfacePassElement reads that live geometry and the size-animation bit to
// size, place and crop every surface. Zen's page (a subsurface) came out at its
// full client size, cropped into the shrunken window, for the first frames of a
// restore (user, 2026-09-30). So such a caller brackets the window's elements
// with these two: the first re-applies the tile geometry when the pass reaches
// it, the second puts the real geometry back.
struct SDeferredWindowGeometry {
    PHLWINDOWREF    window;
    PHLWORKSPACEREF workspace;
    Vector2D        pos, size;
    bool            zeroWorkspaceOffset = false;

    // what apply() found, for restore()
    bool     applied = false;
    Vector2D prevPos, prevSize, prevOffset;
    bool     prevAnimatingIn = false, prevPosAnimating = false, prevSizeAnimating = false;

    void apply() {
        const auto WINDOW = window.lock();
        if (!WINDOW || applied)
            return;
        applied           = true;
        prevPos           = WINDOW->positionAnimation()->value();
        prevSize          = WINDOW->sizeAnimation()->value();
        prevAnimatingIn   = WINDOW->m_animatingIn;
        prevPosAnimating  = WINDOW->positionAnimation()->isBeingAnimated();
        prevSizeAnimating = WINDOW->sizeAnimation()->isBeingAnimated();
        COverviewAnimatedVariableAccess::setBeingAnimated(WINDOW->positionAnimation().get(), true);
        COverviewAnimatedVariableAccess::setBeingAnimated(WINDOW->sizeAnimation().get(), true);
        WINDOW->positionAnimation()->value() = pos;
        WINDOW->sizeAnimation()->value()     = size;
        WINDOW->m_animatingIn                = true;
        if (const auto WS = workspace.lock(); WS && zeroWorkspaceOffset) {
            prevOffset                = WS->m_renderOffset->value();
            WS->m_renderOffset->value() = {};
        }
    }

    void restore() {
        const auto WINDOW = window.lock();
        if (!WINDOW || !applied)
            return;
        applied                              = false;
        WINDOW->positionAnimation()->value() = prevPos;
        WINDOW->sizeAnimation()->value()     = prevSize;
        WINDOW->m_animatingIn                = prevAnimatingIn;
        COverviewAnimatedVariableAccess::setBeingAnimated(WINDOW->positionAnimation().get(), prevPosAnimating);
        COverviewAnimatedVariableAccess::setBeingAnimated(WINDOW->sizeAnimation().get(), prevSizeAnimating);
        if (const auto WS = workspace.lock(); WS && zeroWorkspaceOffset)
            WS->m_renderOffset->value() = prevOffset;
    }
};

class CDeferredWindowGeometryPassElement : public IPassElement {
  public:
    CDeferredWindowGeometryPassElement(SP<SDeferredWindowGeometry> state, bool apply) : m_state(std::move(state)), m_apply(apply) {}
    // A pop that never runs (the pass is dropped) must still give the geometry back.
    ~CDeferredWindowGeometryPassElement() override {
        if (!m_apply && m_state)
            m_state->restore();
    }

    std::vector<UP<IPassElement>> draw() override {
        if (m_state)
            m_apply ? m_state->apply() : m_state->restore();
        return {};
    }
    bool needsLiveBlur() override {
        return false;
    }
    bool needsPrecomputeBlur() override {
        return false;
    }
    const char* passName() override {
        return m_apply ? "CDeferredWindowGeometryApply" : "CDeferredWindowGeometryRestore";
    }
    ePassElementType type() override {
        return EK_CUSTOM;
    }
    bool undiscardable() override {
        return true;
    }
    bool disableSimplification() override {
        return true;
    }

  private:
    SP<SDeferredWindowGeometry> m_state;
    bool                        m_apply = true;
};

static PHLWINDOW getOverviewWindowToShow(const PHLWINDOW& window) {
    if (!window)
        return nullptr;

    if (window->m_group)
        return window->m_group->current();

    return window;
}

static float getOverviewWindowTargetOpacity(const PHLWINDOW& window) {
    if (!window)
        return 1.F;

    const bool  fullscreen     = Fullscreen::controller()->isFullscreen(window);
    const bool  active         = Desktop::focusState()->window() == window;
    float       targetOpacity  = fullscreen ? ScrollOverview::Config::getValue<float>("decoration:fullscreen_opacity") :
        active ? ScrollOverview::Config::getValue<float>("decoration:active_opacity") : ScrollOverview::Config::getValue<float>("decoration:inactive_opacity");
    const auto& ruleOpacityVar = fullscreen ? window->m_ruleApplicator->alphaFullscreen() : active ? window->m_ruleApplicator->alpha() : window->m_ruleApplicator->alphaInactive();

    targetOpacity = ruleOpacityVar.valueOr(Desktop::Types::SAlphaValue{}).applyAlpha(targetOpacity);

    return std::clamp(targetOpacity, 0.F, 1.F);
}


// U2: below ~0.55 the GPU's 2x2 bilinear tap-pair stops covering the source
// texels and the tile aliases (measured: NEAREST-like by 0.22). The cure is a
// mip chain, but the tile's texture is EGLImage-backed -- glGenerateMipmap on it
// reports GL_NO_ERROR and silently orphans level 0, blanking the window. So copy
// it into an FB we own and mipmap that.
// This sits UPSTREAM of compositing: the pass element still computes frost from
// the screen behind the tile, so section C is untouched by design.
// ponytail: the copy+mipmap is redone every frame for every tile, including
// tiles whose contents never changed. Measured cost of that waste is +3.4pp GPU
// busy (20.4% -> 23.9%) for 8 tiles of 2400x1450, which is affordable, so it is
// left alone. If it ever stops being affordable, the upgrade is to keep the
// mipmap and skip regeneration while the source texture is unchanged.
//
// Y17 (GPU, 2026-09-14): it stopped being affordable, and that upgrade is what
// follows. The old cache was keyed on the GL texture NAME with no notion of
// "unchanged", which cost three ways:
//   * the same texture was cleared, re-copied and re-mipmapped once per DRAW --
//     the tile, its strip mini, the alternate-monitor mini and again for the
//     other monitor's frame -- four times a frame for one static window;
//   * nothing ever expired, and a client cycling a dmabuf swapchain retires
//     texture names that never come back, so the map (and its full-size
//     framebuffers) grew without bound;
//   * every ~CScrollOverview released it for ALL overviews, so closing one
//     monitor's overview threw away the other's mipmaps mid-animation.
//
// The cache is now keyed by SURFACE: one entry per surface, shared by all four
// draws. It is rebuilt only if the source texture object, its size, the target
// format or the target image description changed, or if the surface COMMITTED
// since the copy -- notifySurfaceCommit() below, driven from main.cpp's
// damageSurface hook. That is the same signal the overview already trusts to
// know a live tile must be repainted (CScrollOverview::shouldHandleSurfaceDamage),
// so the mipmap can never be staler than the tile it is drawn into. Entries
// whose surface died, and entries nothing has drawn for a while, are dropped.
struct SU2PrefilterEntry {
    SP<Render::IFramebuffer>            fb;
    WP<CWLSurfaceResource>              surface;
    WP<Render::ITexture>                srcTex;      // key owner when there is no surface
    bool                                keyedOnSurface = false;
    GLuint                              srcTexID       = 0;
    Vector2D                            srcSize;
    uint32_t                            format         = 0;
    NColorManagement::PImageDescription imageDescription;
    bool                                dirty          = true;
    uint64_t                            lastUse        = 0;
    std::chrono::steady_clock::time_point lastCopy{};

    bool                                alive() const {
        return keyedOnSurface ? !surface.expired() : !srcTex.expired();
    }
};

static std::unordered_map<const void*, SU2PrefilterEntry> g_u2PrefilterFBs;
// Bumped once per prefiltered surface, not per frame -- Window.cpp has no frame
// of its own. At ~8 tiles a frame this idle window is roughly a second.
static uint64_t                                           g_u2PrefilterUse  = 0;
static constexpr uint64_t                                 U2_PREFILTER_IDLE = 512;
static constexpr int                                      PREFILTER_FRAME_BUDGET = 2;

// Called with the EGL context current (from the render path), so releasing a
// framebuffer here is safe.
static void pruneTexturePrefilterCache() {
    std::erase_if(g_u2PrefilterFBs, [](auto& kv) {
        auto& entry = kv.second;
        if (entry.alive() && g_u2PrefilterUse - entry.lastUse <= U2_PREFILTER_IDLE)
            return false;

        if (entry.fb)
            entry.fb->release();
        return true;
    });
}

static void prefilterOverviewWindowTextures(const PHLWINDOW& window, PHLMONITOR monitor, double minify, size_t firstElement) {
    // ponytail: default 0.55 is just above the measured 0.5 boundary where
    // bilinear leaves the area-average ideal. Above it the copy would buy
    // nothing. 0 disables the prefilter entirely.
    const double MINIFY_THRESHOLD = ScrollOverview::Config::getValue<float>("plugin:hyprdrive:prefilter_below");

    if (!window || !monitor || minify <= 0.0 || minify > MINIFY_THRESHOLD)
        return;
    if (!g_pHyprRenderer->m_renderData.currentFB)
        return;

    const auto FBFORMAT     = g_pHyprRenderer->m_renderData.currentFB->m_drmFormat;
    // currentPass(), not m_renderPass: the dock's hover previews are queued into
    // a REDIRECTED nested pass (CDockLivePreviewPassElement). For the overview
    // the two are the same pass.
    auto&      passElements = g_pHyprRenderer->currentPass().m_passElements;

    for (size_t i = firstElement; i < passElements.size(); ++i) {
        if (!passElements[i].element)
            continue;

        auto* spe = dc<CSurfacePassElement*>(passElements[i].element.get());
        if (!spe || spe->m_data.pWindow != window)
            continue;

        const auto SRC = spe->m_data.texture;
        if (!SRC || !SRC->m_texID || SRC->m_size.x < 2 || SRC->m_size.y < 2)
            continue;

        const int  W          = sc<int>(SRC->m_size.x);
        const int  H          = sc<int>(SRC->m_size.y);
        // COPIED AT THE SIZE IT IS DRAWN, in powers of two (2026-09-30). The
        // copy used to be full or half resolution whatever the tile's size, so a
        // 2880-px window drawn as a 500-px tile was copied and mipmapped at 1440
        // px -- and while the overview was open those copies were re-made many
        // times a second, which is what drove Hyprland's GPU memory up by
        // gigabytes (the driver keeps freed buffers for reuse). The copy is now
        // the smallest 1/2^k of the source that still covers the drawn size:
        // no finer detail can reach the screen, and the mip chain does the rest.
        // Powers of two, so a zoom does not reallocate every frame.
        //
        // AT MOST HALF (2026-09-30, reverted from 1/16): one bilinear draw
        // straight down to 1/4..1/16 samples a handful of source texels per
        // output pixel, so the copy came out ALIASED and the mip chain built on
        // it could not undo that -- the previews lost their antialiasing for a
        // saving of a few MiB. An exact 2:1 copy averages every 2x2 block, and
        // the mip chain does the rest properly.
        int SHIFT = 0;
        if (minify <= 0.5 && W >= 4 && H >= 4)
            SHIFT = 1;
        const bool HALF       = SHIFT > 0;
        const int  FBW        = W >> SHIFT;
        const int  FBH        = H >> SHIFT;
        const auto SURFACE    = spe->m_data.surface;
        const auto IMAGEDESC  = g_pHyprRenderer->m_renderData.currentFB ? g_pHyprRenderer->m_renderData.currentFB->imageDescription() : NColorManagement::PImageDescription{};
        // A surface pass element always carries its surface; the texture is the
        // fallback key so a hypothetical one without it still gets a mipmap
        // (rebuilt every draw, exactly as the whole cache used to be).
        //
        // AND BY SIZE STEP (2026-09-30). One surface is drawn at two very
        // different sizes in the same frame -- its grid tile and its strip mini
        // -- and a single entry per surface was reallocated between the two on
        // EVERY draw: ~250 full buffer allocations a second with the overview
        // open, which is what drove Hyprland's GPU memory into the gigabytes
        // (the driver keeps freed buffers for reuse) and cost the open its
        // smoothness. Each size step keeps its own copy now.
        const void* const KEY = reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(SURFACE ? sc<const void*>(SURFACE.get()) : sc<const void*>(SRC.get())) +
                                                              sc<uintptr_t>(SHIFT));

        ++g_u2PrefilterUse;
        if ((g_u2PrefilterUse & 0x3FULL) == 0)
            pruneTexturePrefilterCache();

        auto& entry = g_u2PrefilterFBs[KEY];
        if (!entry.fb)
            entry.fb = g_pHyprRenderer->createFB("hyprdrive_u2_prefilter");
        if (!entry.fb)
            continue;

        entry.lastUse        = g_u2PrefilterUse;
        entry.keyedOnSurface = !!SURFACE;
        entry.surface        = SURFACE;
        entry.srcTex         = SRC;

        // The source texture object identity is not enough on its own: a client
        // cycling a small dmabuf swapchain hands back the SAME texture with new
        // content. The commit flag is what makes this sound.
        bool       NEEDSCOPY = entry.dirty || !entry.fb->isAllocated() || entry.srcTexID != SRC->m_texID || entry.srcSize != SRC->m_size || entry.format != FBFORMAT ||
            entry.imageDescription != IMAGEDESC;

        // A NEW FRAME OF CONTENT is not worth a full-resolution copy plus a
        // mipmap chain every time it lands (measured 2026-09-29, openprof: a
        // live Moonlight stream on another desktop cost the STRIP 8-10 ms of GPU
        // a frame -- a 2880x1800 copy + glGenerateMipmap per commit for a cell a
        // few dozen pixels high -- and the overview fell to 30 fps). Content-only
        // refreshes are paced by how small the draw is: a strip thumbnail at
        // ~8 fps, a grid tile at ~30. Size, format or colour changes still copy
        // at once, and the entry stays dirty so the next draw after the interval
        // picks the new content up.
        const bool CONTENTONLY = entry.fb->isAllocated() && entry.fb->m_size == Vector2D{sc<double>(FBW), sc<double>(FBH)} && entry.fb->m_drmFormat == FBFORMAT &&
            entry.srcSize == SRC->m_size && entry.format == FBFORMAT && entry.imageDescription == IMAGEDESC && entry.lastCopy != std::chrono::steady_clock::time_point{};
        if (NEEDSCOPY && CONTENTONLY) {
            const auto NOW     = std::chrono::steady_clock::now();
            const auto REFRESH = std::chrono::milliseconds(minify < 0.2 ? 125 : 33);
            // And at most PREFILTER_FRAME_BUDGET of them in one frame: the
            // overview opening onto a dozen windows that all changed while it
            // was shut used to rebuild every one in its first frame (37 ms of
            // GPU, measured). The rest keep last content for a frame or two.
            // A frame's draws land within a few ms; frames are 16+ ms apart.
            static auto s_budgetAt = std::chrono::steady_clock::time_point{};
            static int  s_budget   = 0;
            if (NOW - s_budgetAt > std::chrono::milliseconds(8)) {
                s_budgetAt = NOW;
                s_budget   = PREFILTER_FRAME_BUDGET;
            }
            if (NOW - entry.lastCopy < REFRESH || s_budget <= 0)
                NEEDSCOPY = false;
            else
                --s_budget;
        }

        if (!entry.fb->isAllocated() || entry.fb->m_size != Vector2D{sc<double>(FBW), sc<double>(FBH)} || entry.fb->m_drmFormat != FBFORMAT) {
            entry.fb->release();
            NEEDSCOPY = true; // a new buffer has no content yet
            if (!entry.fb->alloc(FBW, FBH, FBFORMAT))
                continue;
        }
        if (g_pHyprRenderer->m_renderData.currentFB)
            entry.fb->setImageDescription(IMAGEDESC);

        const auto MIPPED = entry.fb->getTexture();
        if (!MIPPED || !MIPPED->m_texID)
            continue;

        if (NEEDSCOPY) {
            const CBox    FULLBOX{0, 0, sc<double>(FBW), sc<double>(FBH)};
            const CRegion FULLDAMAGE{FULLBOX};

            {
                auto bindPrefilter = g_pHyprRenderer->bindTempFB(entry.fb);
                g_pHyprRenderer->draw(CClearPassElement::SClearData{CHyprColor{0.F, 0.F, 0.F, 0.F}}, FULLDAMAGE);
                g_pHyprRenderer->draw(CTexPassElement::SRenderData{.tex = SRC, .box = FULLBOX, .damage = FULLDAMAGE}, FULLDAMAGE);
            }

            glBindTexture(GL_TEXTURE_2D, MIPPED->m_texID);
            glGenerateMipmap(GL_TEXTURE_2D);
            if (glGetError() != GL_NO_ERROR) {
                glBindTexture(GL_TEXTURE_2D, 0);
                // Leave the entry dirty: a half-built mipmap must not be reused.
                entry.dirty = true;
                continue;
            }
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
            glBindTexture(GL_TEXTURE_2D, 0);

            entry.dirty            = false;
            entry.lastCopy         = std::chrono::steady_clock::now();
            entry.srcTexID         = SRC->m_texID;
            entry.srcSize          = SRC->m_size;
            entry.format           = FBFORMAT;
            entry.imageDescription = IMAGEDESC;
        }

        // Cheap and unconditional: Hyprland re-applies GL_TEXTURE_MIN_FILTER
        // from this field every time it binds the texture (OpenGL.cpp:1536).
        MIPPED->minFilter   = GL_LINEAR_MIPMAP_LINEAR;

        spe->m_data.texture = MIPPED;
    }
}

static void roundStandaloneWindowPassElements(const PHLWINDOW& window, PHLMONITOR monitor, float renderScale, size_t firstElement) {
    if (!window || !monitor)
        return;

    if (Fullscreen::controller()->isFullscreen(window))
        return;

    const int   rounding      = sc<int>(std::round(window->rounding() * monitor->m_scale * renderScale));
    const float roundingPower = window->roundingPower();

    if (rounding <= 0)
        return;

    auto& passElements = g_pHyprRenderer->currentPass().m_passElements;
    for (size_t i = firstElement; i < passElements.size(); ++i) {
        const auto& passElement = passElements[i];
        if (!passElement.element)
            continue;

        auto* surfacePassElement = dc<CSurfacePassElement*>(passElement.element.get());
        if (!surfacePassElement || surfacePassElement->m_data.pWindow != window || surfacePassElement->m_data.popup)
            continue;

        surfacePassElement->m_data.dontRound     = false;
        surfacePassElement->m_data.rounding      = rounding;
        surfacePassElement->m_data.roundingPower = roundingPower;
    }
}

static bool isOverviewHyprbarDecoration(IHyprWindowDecoration* decoration) {
    return decoration && decoration->getDecorationType() == DECORATION_CUSTOM && decoration->getDisplayName() == "Hyprbar";
}

static SHyprbarGlobalStateMirror* getOverviewHyprbarGlobalState() {
    if (!g_pPluginSystem)
        return nullptr;

    HANDLE hyprbarsHandle = nullptr;
    for (const auto* plugin : g_pPluginSystem->getAllPlugins()) {
        if (!plugin)
            continue;
        if (plugin->m_name == "hyprbars" || plugin->m_path.contains("hyprbars")) {
            hyprbarsHandle = plugin->m_handle;
            break;
        }
    }

    if (!hyprbarsHandle)
        return nullptr;

    void* const symbol = dlsym(hyprbarsHandle, "g_pGlobalState");
    if (!symbol)
        return nullptr;

    const auto STATEPTR = sc<UP<SHyprbarGlobalStateMirror>*>(symbol);
    if (!STATEPTR || !STATEPTR->get())
        return nullptr;

    return STATEPTR->get();
}

static float getOverviewHyprbarLogicalHeight(const PHLWINDOW& window) {
    if (!window || !window->m_ruleApplicator->decorate().valueOrDefault())
        return 0.F;

    for (const auto& deco : window->m_windowDecorations) {
        if (!isOverviewHyprbarDecoration(deco.get()))
            continue;

        const auto INFO = deco->getPositioningInfo();
        if (INFO.policy != DECORATION_POSITION_STICKY || INFO.edges != DECORATION_EDGE_TOP)
            continue;

        return std::max(0.F, sc<float>(INFO.desiredExtents.topLeft.y));
    }

    return 0.F;
}

static bool shouldOverviewBorderIncludeHyprbar(const PHLWINDOW& window) {
    const float hyprbarHeight = getOverviewHyprbarLogicalHeight(window);
    if (hyprbarHeight <= 0.F)
        return false;

    return ScrollOverview::Config::getValue<bool>("plugin:hyprbars:bar_precedence_over_border");
}

static bool shouldOverviewShadowIncludeHyprbar(const PHLWINDOW& window, bool borderIncludesHyprbar) {
    const float hyprbarHeight = getOverviewHyprbarLogicalHeight(window);
    if (hyprbarHeight <= 0.F)
        return false;

    if (borderIncludesHyprbar)
        return true;

    return ScrollOverview::Config::getValue<bool>("plugin:hyprbars:bar_part_of_window");
}

static void blockOverviewWindowBlurOptimization(const PHLWINDOW& window, size_t firstElement) {
    if (!window)
        return;

    auto& passElements = g_pHyprRenderer->currentPass().m_passElements;
    for (size_t i = firstElement; i < passElements.size(); ++i) {
        const auto& passElement = passElements[i];
        if (!passElement.element)
            continue;

        auto* surfacePassElement = dc<CSurfacePassElement*>(passElement.element.get());
        if (!surfacePassElement || surfacePassElement->m_data.pWindow != window)
            continue;

        surfacePassElement->m_data.blockBlurOptimization = true;
    }
}

static void scaleOverviewSubsurfaceOffsets(const PHLWINDOW& window, const Vector2D& targetSize, size_t firstElement) {
    if (!window)
        return;

    const auto REPORTEDSIZE = window->getReportedSize();
    if (REPORTEDSIZE.x <= 0 || REPORTEDSIZE.y <= 0)
        return;

    const Vector2D SUBSURFACESCALE = targetSize / REPORTEDSIZE;
    auto&          passElements    = g_pHyprRenderer->currentPass().m_passElements;

    for (size_t i = firstElement; i < passElements.size(); ++i) {
        const auto& passElement = passElements[i];
        if (!passElement.element)
            continue;

        auto* surfacePassElement = dc<CSurfacePassElement*>(passElement.element.get());
        if (!surfacePassElement || surfacePassElement->m_data.pWindow != window || surfacePassElement->m_data.mainSurface || surfacePassElement->m_data.popup)
            continue;

        // Hyprland scales the subsurface texture during a resize, but its
        // cumulative position remains in the client's reported coordinate
        // space. Apply the same scale to the position so embedded dmabuf
        // surfaces (OBS preview, OrcaSlicer viewport, etc.) stay attached.
        surfacePassElement->m_data.localPos *= SUBSURFACESCALE;
    }
}

static SOverviewWindowMetrics getOverviewWindowMetrics(PHLMONITOR monitor, const PHLWINDOW& window, float renderScale) {
    SOverviewWindowMetrics metrics;
    metrics.renderScale   = renderScale;
    metrics.targetOpacity = getOverviewWindowTargetOpacity(window);

    if (!monitor || !window)
        return metrics;

    metrics.pxScale                 = monitor->m_scale * renderScale;
    metrics.borderSize              = window->getRealBorderSize();
    metrics.borderPx                = sc<int>(std::round(metrics.borderSize * monitor->m_scale));
    metrics.borderPxScaled          = metrics.borderSize * metrics.pxScale;
    metrics.roundingBase            = window->rounding();
    metrics.roundingPower           = window->roundingPower();
    metrics.correctionOffset        = metrics.borderSize * (M_SQRT2 - 1) * std::max(2.0 - metrics.roundingPower, 0.0);
    metrics.outerRound              = metrics.roundingBase > 0 ? (metrics.roundingBase + metrics.borderSize) - metrics.correctionOffset : 0.F;
    metrics.roundingPx              = sc<int>(std::round(metrics.roundingBase * metrics.pxScale));
    metrics.outerRoundPx            = sc<int>(std::round(metrics.outerRound * metrics.pxScale));
    metrics.hyprbarLogicalHeight    = getOverviewHyprbarLogicalHeight(window);
    metrics.borderIncludesHyprbar   = shouldOverviewBorderIncludeHyprbar(window);
    metrics.shadowIncludesHyprbar   = shouldOverviewShadowIncludeHyprbar(window, metrics.borderIncludesHyprbar);
    metrics.hyprbarHeightPx         = sc<int>(std::round(metrics.hyprbarLogicalHeight * metrics.pxScale));
    metrics.hyprbarHeightPxScaled   = metrics.hyprbarLogicalHeight * metrics.pxScale;

    return metrics;
}

static std::optional<SDecorationPositioningReply> getOverviewTopStickyDecorationReply(IHyprWindowDecoration* decoration, const PHLWINDOW& window, float heightScale = 1.F) {
    if (!decoration || !window)
        return std::nullopt;

    const auto INFO = decoration->getPositioningInfo();
    if (INFO.policy != DECORATION_POSITION_STICKY || INFO.edges != DECORATION_EDGE_TOP)
        return std::nullopt;

    const float HEIGHT  = std::max(0.F, sc<float>(INFO.desiredExtents.topLeft.y) * heightScale);
    const float BORDER  = sc<float>(window->getRealBorderSize());
    const bool  PRECEDE = shouldOverviewBorderIncludeHyprbar(window);
    const float WIDTH   = PRECEDE ? window->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT).x : window->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT).x + BORDER * 2.F;
    const float YOFFSET = PRECEDE ? 0.F : BORDER;

    return SDecorationPositioningReply{
        .assignedGeometry = CBox{{-WIDTH / 2.F, -HEIGHT - YOFFSET}, {WIDTH, HEIGHT}},
    };
}

static CBox getOverviewOuterBorderBox(const CBox& windowBox, const SOverviewWindowMetrics& metrics) {
    CBox box = windowBox.copy().expand(metrics.borderPx);
    if (metrics.shadowIncludesHyprbar && metrics.hyprbarHeightPx > 0) {
        box.y -= metrics.hyprbarHeightPx;
        box.height += metrics.hyprbarHeightPx;
    }

    return box;
}

static CBox getOverviewBorderBox(const CBox& windowBox, const SOverviewWindowMetrics& metrics) {
    CBox box = windowBox;
    if (metrics.borderIncludesHyprbar && metrics.hyprbarHeightPx > 0) {
        box.y -= metrics.hyprbarHeightPx;
        box.height += metrics.hyprbarHeightPx;
    }

    return box;
}

static void renderOverviewHyprbarDecoration(SOverviewCustomDecorationRenderState& state, PHLMONITOR monitor, const PHLWINDOW& window, IHyprWindowDecoration* decoration,
                                            const CBox& windowBox, const SOverviewWindowMetrics& metrics) {
    if (!monitor || !window || !decoration)
        return;

    auto* const HYPRBARGLOBALSTATE    = getOverviewHyprbarGlobalState();
    const bool   PARTOFWINDOW         = decoration->getDecorationFlags() & DECORATION_PART_OF_MAIN_WINDOW;
    const int   previousBarTextSize   = ScrollOverview::Config::getValue<int>("plugin:hyprbars:bar_text_size");
    const int   previousButtonPadding = ScrollOverview::Config::getValue<int>("plugin:hyprbars:bar_button_padding");
    std::vector<float> previousButtonSizes;
    if (HYPRBARGLOBALSTATE) {
        previousButtonSizes.reserve(HYPRBARGLOBALSTATE->buttons.size());
        for (auto& button : HYPRBARGLOBALSTATE->buttons) {
            previousButtonSizes.push_back(button.size);
            button.size *= metrics.renderScale;
        }
    }
    ScrollOverview::Config::setValue("plugin:hyprbars:bar_text_size", std::max(1, sc<int>(std::round(previousBarTextSize * metrics.renderScale))));
    ScrollOverview::Config::setValue("plugin:hyprbars:bar_button_padding", std::max(0, sc<int>(std::round(previousButtonPadding * metrics.renderScale))));

    const Vector2D previousWindowPos       = window->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
    const Vector2D previousWindowSize      = window->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
    const auto     WORKSPACE               = window->m_workspace;
    const bool     OVERRIDEWORKSPACEOFFSET = WORKSPACE && !window->m_pinned;
    const Vector2D previousWorkspaceOffset = OVERRIDEWORKSPACEOFFSET ? WORKSPACE->m_renderOffset->value() : Vector2D{};
    const auto     previousReplyData       = g_pDecorationPositioner->getDataFor(decoration, window);
    const auto     previousReply           = previousReplyData ? previousReplyData->lastReply : SDecorationPositioningReply{};
    const auto     previousRounding        = window->m_ruleApplicator->rounding();
    const auto     previousBorderSize      = window->m_ruleApplicator->borderSize();
    const bool     previousBorderCacheDirty = window->m_borderSizeCacheDirty;
    const int      previousCachedBorderSize = window->m_cachedBorderSize;

    // ONE RESTORE, TWO EXITS. Every line below has to be put back whether the
    // positioning reply fails here and now or the frame ends normally later, and
    // it was written out twice -- eight lines each -- in a block where a line
    // missed from one copy leaks a hyprbars config value into the user's live
    // session and leaves the plugin's buttons permanently rescaled. Same class
    // as the three copies of the dock's still-moving test; one lambda, two
    // callers. It re-reads the global state rather than closing over the pointer
    // above because the deferred caller runs a frame later.
    const auto RESTOREBARS = [window, previousBarTextSize, previousButtonPadding, previousButtonSizes, previousRounding, previousBorderSize, previousBorderCacheDirty,
                              previousCachedBorderSize] {
        ScrollOverview::Config::setValue("plugin:hyprbars:bar_text_size", previousBarTextSize);
        ScrollOverview::Config::setValue("plugin:hyprbars:bar_button_padding", previousButtonPadding);
        window->m_ruleApplicator->roundingOverride(previousRounding);
        window->m_ruleApplicator->borderSizeOverride(previousBorderSize);
        window->m_borderSizeCacheDirty = previousBorderCacheDirty;
        window->m_cachedBorderSize     = previousCachedBorderSize;
        if (auto* const BARSTATE = getOverviewHyprbarGlobalState()) {
            for (size_t i = 0; i < previousButtonSizes.size() && i < BARSTATE->buttons.size(); ++i)
                BARSTATE->buttons[i].size = previousButtonSizes[i];
        }
    };

    const auto scaledRounding   = std::max<Config::INTEGER>(0, sc<Config::INTEGER>(std::round(window->m_ruleApplicator->rounding().valueOrDefault() * metrics.renderScale)));
    const auto scaledBorderSize = std::max<Config::INTEGER>(0, sc<Config::INTEGER>(std::round(window->getRealBorderSize() * metrics.renderScale)));
    window->m_ruleApplicator->rounding().set(scaledRounding, Desktop::Types::PRIORITY_SET_PROP);
    window->m_ruleApplicator->borderSize().set(scaledBorderSize, Desktop::Types::PRIORITY_SET_PROP);
    window->m_borderSizeCacheDirty = true;

    window->positionAnimation()->value() = monitor->m_position + windowBox.pos() / monitor->m_scale - window->m_floatingOffset;
    window->sizeAnimation()->value()     = windowBox.size() / monitor->m_scale;

    const auto REPLY = getOverviewTopStickyDecorationReply(decoration, window, metrics.renderScale);
    if (!REPLY) {
        window->positionAnimation()->value() = previousWindowPos;
        window->sizeAnimation()->value()     = previousWindowSize;
        RESTOREBARS();
        return;
    }

    if (OVERRIDEWORKSPACEOFFSET)
        WORKSPACE->m_renderOffset->value() = {};

    if (previousReplyData)
        previousReplyData->lastReply = *REPLY;
    decoration->onPositioningReply(*REPLY);

    decoration->draw(monitor, PARTOFWINDOW ? 1.F : metrics.targetOpacity);

    state.queuedAny = true;
    state.restoreFns.emplace_back([window, decoration, WORKSPACE, OVERRIDEWORKSPACEOFFSET, previousWindowPos, previousWindowSize, previousWorkspaceOffset, previousReply,
                                   previousReplyData, RESTOREBARS] {
        if (!window || !decoration)
            return;

        window->positionAnimation()->value() = previousWindowPos;
        window->sizeAnimation()->value()     = previousWindowSize;
        if (OVERRIDEWORKSPACEOFFSET && WORKSPACE)
            WORKSPACE->m_renderOffset->value() = previousWorkspaceOffset;

        if (previousReplyData)
            previousReplyData->lastReply = previousReply;
        decoration->onPositioningReply(previousReply);

        RESTOREBARS();
    });
}

static void renderOverviewWindowShadow(PHLMONITOR monitor, const PHLWINDOW& window, const CBox& windowBox, const SOverviewWindowMetrics& metrics, bool selected) {
    if (!monitor || !window || (!window->m_isMapped))
        return;

    const auto PSHADOWS      = ScrollOverview::Config::getValue<int>("decoration:shadow:enabled");
    const auto PSHADOWSIZE   = ScrollOverview::Config::getValue<int>("decoration:shadow:range");
    const auto PSHADOWSCALE  = ScrollOverview::Config::getValue<float>("decoration:shadow:scale");
    const auto PSHADOWOFFSET = ScrollOverview::Config::getValue<Hyprlang::VEC2>("decoration:shadow:offset");

    if (PSHADOWS != 1 || PSHADOWSIZE <= 0)
        return;

    if (window->isX11OverrideRedirect() || window->m_X11DoesntWantBorders || !window->m_ruleApplicator->decorate().valueOrDefault() ||
        window->m_ruleApplicator->noShadow().valueOrDefault())
        return;

    const int   rangePx          = sc<int>(std::round(PSHADOWSIZE * monitor->m_scale * metrics.renderScale));
    const float shadowScale      = std::clamp(PSHADOWSCALE, 0.F, 1.F);
    const auto  shadowOffset     = Vector2D{PSHADOWOFFSET.x, PSHADOWOFFSET.y} * monitor->m_scale * metrics.renderScale;

    if (rangePx <= 0)
        return;

    CBox shadowBaseBox = getOverviewOuterBorderBox(windowBox, metrics);
    CBox cutoutBox     = windowBox;
    CBox shadowBox     = shadowBaseBox.copy().expand(rangePx).scaleFromCenter(shadowScale).translate(shadowOffset);
    shadowBox.round();

    if (shadowBox.width < 1 || shadowBox.height < 1)
        return;

    const auto& shadowColor = window->m_realShadowColor;
    if (std::ranges::none_of(shadowColor.m_colors, [](const CHyprColor& color) { return color.a > 0.F; }))
        return;

    g_pHyprRenderer->addPassElement(makeUnique<COverviewShadowPassElement>(COverviewShadowPassElement::SData{
        .monitor       = monitor,
        .fullBox       = shadowBox,
        .cutoutBox     = cutoutBox.round(),
        .rounding      = metrics.outerRoundPx,
        .roundingPower = metrics.roundingPower,
        .range         = rangePx,
        .renderPower   = ScrollOverview::Config::getValue<int>("decoration:shadow:render_power"),
        .color         = shadowColor,
        .alpha         = metrics.targetOpacity,
        .ignoreWindow  = true,
    }));
}

static void renderOverviewWindowBorder(PHLMONITOR monitor, const PHLWINDOW& window, const CBox& windowBox, const SOverviewWindowMetrics& metrics, bool selected) {
    if (!monitor || !window || (!window->m_isMapped))
        return;

    // X61. A SELECTED tile draws a ring even where the theme asks for no border
    // at all -- with border_size 0 the keyboard selection would otherwise be
    // invisible, which is the defect, not a styling preference.
    if (metrics.borderSize <= 0.F && !selected)
        return;

    auto grad = window->m_realBorderColor;

    const CBox borderBox = getOverviewBorderBox(windowBox, metrics);

    CBorderPassElement::SBorderData data;
    data.box           = borderBox;
    data.grad1         = grad;
    data.round         = metrics.roundingPx;
    data.outerRound    = metrics.outerRoundPx;
    data.roundingPower = metrics.roundingPower;
    data.a             = metrics.targetOpacity;
    data.borderSize    = metrics.borderSize;

    // X61. The keyboard selection had NO visual at all: this function took
    // `selected` and never read it, so every tile drew m_realBorderColor and a
    // moved selection produced a byte-identical frame. That is what the user
    // reported as the arrows not working -- measured 2026-08-06, moveSelection
    // was picking the right tile the whole time.
    //
    // OPAQUE, for the reason the search rows are opaque (X2). It used to be
    // THICK as well -- 4x the window's own border -- because the only colour
    // available was a dark warm brown within a few units of the blurred
    // wallpaper, so weight was the only channel left to carry the signal.
    //
    // X72 (user, 2026-08-06): "make the window highlight a lot thinner, maybe as
    // thin as the active window edges in hyprland - too overpowering and thick".
    // X68/X69 gave the ring a bright themed colour, so the COLOUR carries it now
    // and the weight does not have to. It takes the window's own border size,
    // which is what makes it match a real Hyprland border in the overview.
    // The floor of 1 stays: with general:border_size 0 there is no border to
    // match, and a selection has to be visible anyway.
    if (selected) {
        // X56: the bright highlight key, not the border accent -- see
        // Config::highlightColor for why those are not the same colour here.
        const auto ACCENT = ScrollOverview::Config::highlightColor();
        data.grad1.reset(ACCENT.modifyA(1.F));
        data.a            = 1.F;
        data.borderSize   = std::max(1.F, metrics.borderSize);
        data.hasGrad2     = false;
        g_pHyprRenderer->addPassElement(makeUnique<CBorderPassElement>(data));
        return;
    }

    if (window->m_borderFadeAnimationProgress->isBeingAnimated()) {
        data.hasGrad2 = true;
        data.grad1    = window->m_realBorderColorPrevious;
        data.grad2    = grad;
        data.lerp     = window->m_borderFadeAnimationProgress->value();
    } else
        data.grad1 = grad;
    g_pHyprRenderer->addPassElement(makeUnique<CBorderPassElement>(data));
}

static void renderOverviewGroupTabIndicators(PHLMONITOR monitor, const PHLWINDOW& window, const CBox& windowBox, const SOverviewWindowMetrics& metrics, float alpha) {
    if (!monitor || !window || !window->m_group || window->m_group->size() < 1)
        return;

    const auto PINDICATORHEIGHT = ScrollOverview::Config::getValue<int>("group:groupbar:indicator_height");
    const auto PINDICATORGAP    = ScrollOverview::Config::getValue<int>("group:groupbar:indicator_gap");
    const auto PHEIGHT          = ScrollOverview::Config::getValue<int>("group:groupbar:height");
    const auto PGRADIENTS       = ScrollOverview::Config::getValue<bool>("group:groupbar:gradients");
    const auto PRENDERTITLES    = ScrollOverview::Config::getValue<bool>("group:groupbar:render_titles");
    const auto PSTACKED         = ScrollOverview::Config::getValue<bool>("group:groupbar:stacked");
    const auto PROUNDING        = ScrollOverview::Config::getValue<int>("group:groupbar:rounding");
    const auto PROUNDINGPOWER   = ScrollOverview::Config::getValue<float>("group:groupbar:rounding_power");
    const auto POUTERGAP        = ScrollOverview::Config::getValue<int>("group:groupbar:gaps_out");
    const auto PINNERGAP        = ScrollOverview::Config::getValue<int>("group:groupbar:gaps_in");

    if (PINDICATORHEIGHT <= 0)
        return;

    const bool  groupLocked  = window->m_group->locked() || g_pKeybindManager->m_groupsLocked;
    const auto  groupWindows = window->m_group->windows();
    const auto  focusedWindow = Desktop::focusState()->window();
    const float indicatorH   = sc<float>(PINDICATORHEIGHT) * metrics.pxScale;
    const float outerGap     = sc<float>(POUTERGAP) * metrics.pxScale;
    const float innerGap     = sc<float>(PINNERGAP) * metrics.pxScale;
    const float oneBarHeight = sc<float>(POUTERGAP + PINDICATORHEIGHT + PINDICATORGAP + ((PGRADIENTS || PRENDERTITLES) ? PHEIGHT : 0)) * metrics.pxScale;
    const int   rounding     = sc<int>(std::round(PROUNDING * metrics.pxScale));
    CBox        indicatorArea = windowBox.copy().expand(metrics.borderPxScaled);
    indicatorArea.y -= metrics.hyprbarHeightPxScaled;
    auto* const GROUPCOLACTIVE         = sc<Config::CGradientValueData*>(ScrollOverview::Config::valueRef<Config::IComplexConfigValue>("group:groupbar:col.active").ptr());
    auto* const GROUPCOLINACTIVE       = sc<Config::CGradientValueData*>(ScrollOverview::Config::valueRef<Config::IComplexConfigValue>("group:groupbar:col.inactive").ptr());
    auto* const GROUPCOLACTIVELOCKED   = sc<Config::CGradientValueData*>(ScrollOverview::Config::valueRef<Config::IComplexConfigValue>("group:groupbar:col.locked_active").ptr());
    auto* const GROUPCOLINACTIVELOCKED = sc<Config::CGradientValueData*>(ScrollOverview::Config::valueRef<Config::IComplexConfigValue>("group:groupbar:col.locked_inactive").ptr());
    const auto* const COLACTIVE        = groupLocked ? GROUPCOLACTIVELOCKED : GROUPCOLACTIVE;
    const auto* const COLINACTIVE      = groupLocked ? GROUPCOLINACTIVELOCKED : GROUPCOLINACTIVE;

    float xoff = 0.F;
    float yoff = 0.F;

    for (size_t i = 0; i < groupWindows.size(); ++i) {
        const size_t windowIdx = PSTACKED ? groupWindows.size() - i - 1 : i;
        const auto   member    = groupWindows[windowIdx].lock();
        if (!member)
            continue;

        CHyprColor color = member == focusedWindow ? COLACTIVE->m_colors[0] : COLINACTIVE->m_colors[0];

        color.a *= alpha;
        if (color.a <= 0.F)
            continue;

        CBox box;
        if (PSTACKED) {
            box = {indicatorArea.x, indicatorArea.y - yoff - outerGap - indicatorH, indicatorArea.width, indicatorH};
            yoff += oneBarHeight;
        } else {
            const float barWidth = (indicatorArea.width - innerGap * (groupWindows.size() - 1)) / groupWindows.size();
            box                  = {indicatorArea.x + xoff, indicatorArea.y - outerGap - indicatorH, barWidth, indicatorH};
            xoff += innerGap + barWidth;
        }

        box.round();
        if (box.empty())
            continue;

        CRectPassElement::SRectData data;
        data.box           = box;
        data.color         = color;
        data.round         = rounding;
        data.roundingPower = PROUNDINGPOWER;
        g_pHyprRenderer->addPassElement(makeUnique<CRectPassElement>(data));
    }
}

static void renderOverviewGroupTabTitles(PHLMONITOR monitor, const PHLWINDOW& window, const CBox& windowBox, const SOverviewWindowMetrics& metrics, float alpha) {
    if (!monitor || !window || !window->m_group || window->m_group->size() < 1 || alpha <= 0.F)
        return;

    const auto PRENDERTITLES            = ScrollOverview::Config::getValue<bool>("group:groupbar:render_titles");
    const auto PTITLEFONTSIZE           = ScrollOverview::Config::getValue<int>("group:groupbar:font_size");
    const auto PHEIGHT                  = ScrollOverview::Config::getValue<int>("group:groupbar:height");
    const auto PINDICATORGAP            = ScrollOverview::Config::getValue<int>("group:groupbar:indicator_gap");
    const auto PINDICATORHEIGHT         = ScrollOverview::Config::getValue<int>("group:groupbar:indicator_height");
    const auto PSTACKED                 = ScrollOverview::Config::getValue<bool>("group:groupbar:stacked");
    const auto POUTERGAP                = ScrollOverview::Config::getValue<int>("group:groupbar:gaps_out");
    const auto PINNERGAP                = ScrollOverview::Config::getValue<int>("group:groupbar:gaps_in");
    const auto PTEXTOFFSET              = ScrollOverview::Config::getValue<int>("group:groupbar:text_offset");
    const auto PTEXTPADDING             = ScrollOverview::Config::getValue<int>("group:groupbar:text_padding");
    const auto PTEXTCOLORACTIVE         = ScrollOverview::Config::getValue<int>("group:groupbar:text_color");
    const auto PTEXTCOLORINACTIVE       = ScrollOverview::Config::getValue<int>("group:groupbar:text_color_inactive");
    const auto PTEXTCOLORLOCKEDACTIVE   = ScrollOverview::Config::getValue<int>("group:groupbar:text_color_locked_active");
    const auto PTEXTCOLORLOCKEDINACTIVE = ScrollOverview::Config::getValue<int>("group:groupbar:text_color_locked_inactive");

    if (!PRENDERTITLES || PHEIGHT <= 0)
        return;

    const bool  groupLocked  = window->m_group->locked() || g_pKeybindManager->m_groupsLocked;
    const auto  groupWindows = window->m_group->windows();
    const auto  focusedWindow = Desktop::focusState()->window();
    const float outerGap     = sc<float>(POUTERGAP) * metrics.pxScale;
    const float innerGap     = sc<float>(PINNERGAP) * metrics.pxScale;
    const float oneBarHeight = sc<float>(POUTERGAP + PINDICATORHEIGHT + PINDICATORGAP + PHEIGHT) * metrics.pxScale;
    const float titleHeight  = sc<float>(PHEIGHT) * metrics.pxScale;
    const float textPadding  = sc<float>(PTEXTPADDING) * metrics.pxScale;
    const float textOffset   = sc<float>(PTEXTOFFSET) * metrics.pxScale;
    const int   fontSizePx   = std::max(1, sc<int>(std::round(PTITLEFONTSIZE * metrics.pxScale)));

    CBox        titleArea = windowBox.copy().expand(metrics.borderPxScaled);
    titleArea.y -= metrics.hyprbarHeightPxScaled;

    const CHyprColor COLORACTIVE         = CHyprColor(PTEXTCOLORACTIVE);
    const CHyprColor COLORINACTIVE       = PTEXTCOLORINACTIVE == -1 ? COLORACTIVE : CHyprColor(PTEXTCOLORINACTIVE);
    const CHyprColor COLORLOCKEDACTIVE   = PTEXTCOLORLOCKEDACTIVE == -1 ? COLORACTIVE : CHyprColor(PTEXTCOLORLOCKEDACTIVE);
    const CHyprColor COLORLOCKEDINACTIVE = PTEXTCOLORLOCKEDINACTIVE == -1 ? COLORINACTIVE : CHyprColor(PTEXTCOLORLOCKEDINACTIVE);
    const auto       FONTWEIGHTACTIVE    = sc<Config::CFontWeightConfigValueData*>(ScrollOverview::Config::valueRef<Config::IComplexConfigValue>("group:groupbar:font_weight_active").ptr());
    const auto       FONTWEIGHTINACTIVE  = sc<Config::CFontWeightConfigValueData*>(ScrollOverview::Config::valueRef<Config::IComplexConfigValue>("group:groupbar:font_weight_inactive").ptr());
    const auto       TITLEFONTFAMILY     = ScrollOverview::Config::getValue<std::string>("group:groupbar:font_family");
    const auto       FONTFAMILY          = TITLEFONTFAMILY != STRVAL_EMPTY ? TITLEFONTFAMILY : ScrollOverview::Config::themeFontFamily();

    float xoff = 0.F;
    float yoff = 0.F;

    for (size_t i = 0; i < groupWindows.size(); ++i) {
        const size_t windowIdx = PSTACKED ? groupWindows.size() - i - 1 : i;
        const auto   member    = groupWindows[windowIdx].lock();
        if (!member)
            continue;

        const float barWidth = PSTACKED ? titleArea.width : (titleArea.width - innerGap * (groupWindows.size() - 1)) / groupWindows.size();

        CBox box;
        if (PSTACKED) {
            box = {titleArea.x, titleArea.y - yoff - outerGap - titleHeight - sc<float>(PINDICATORHEIGHT) * metrics.pxScale - sc<float>(PINDICATORGAP) * metrics.pxScale,
                   titleArea.width, titleHeight};
            yoff += oneBarHeight;
        } else {
            box = {titleArea.x + xoff, titleArea.y - outerGap - titleHeight - sc<float>(PINDICATORHEIGHT) * metrics.pxScale - sc<float>(PINDICATORGAP) * metrics.pxScale,
                   barWidth, titleHeight};
            xoff += innerGap + barWidth;
        }

        box.round();
        if (box.empty())
            continue;

        CHyprColor textColor;
        const bool isCurrent = member == focusedWindow;
        if (groupLocked)
            textColor = isCurrent ? COLORLOCKEDACTIVE : COLORLOCKEDINACTIVE;
        else
            textColor = isCurrent ? COLORACTIVE : COLORINACTIVE;

        textColor.a *= alpha;
        if (textColor.a <= 0.F)
            continue;

        const int maxWidth = std::max(1, sc<int>(std::round(box.width - textPadding * 2.F)));
        auto      titleTex =
            OverviewRender::text(member->m_title, textColor, fontSizePx, false, FONTFAMILY, maxWidth, isCurrent ? FONTWEIGHTACTIVE->m_value : FONTWEIGHTINACTIVE->m_value);
        if (!titleTex || !titleTex->ok())
            continue;

        CBox textBox = box;
        textBox.y += std::ceil((box.height - titleTex->m_size.y) / 2.0 - textOffset);
        textBox.height = titleTex->m_size.y;
        textBox.width  = titleTex->m_size.x;
        textBox.x += std::round((box.width + textPadding) / 2.0 - (titleTex->m_size.x + textPadding) / 2.0);
        textBox.round();

        CTexPassElement::SRenderData data;
        data.tex = titleTex;
        data.box = textBox;
        data.a   = 1.F;
        g_pHyprRenderer->addPassElement(makeUnique<CTexPassElement>(data));
    }
}

static SOverviewCustomDecorationRenderState renderOverviewCustomDecorations(PHLMONITOR monitor, const PHLWINDOW& window, const CBox& workspaceBox, const CBox& windowBox,
                                                                           const SOverviewWindowMetrics& metrics, eDecorationLayer layer) {
    SOverviewCustomDecorationRenderState state;

    if (!monitor || !window)
        return state;

    window->updateWindowDecos();

    Render::SRenderModifData modif;
    modif.modifs.emplace_back(Render::SRenderModifData::RMOD_TYPE_SCALE, metrics.renderScale);
    modif.modifs.emplace_back(Render::SRenderModifData::RMOD_TYPE_TRANSLATE, workspaceBox.pos());
    bool        drewAny = false;

    for (const auto& deco : window->m_windowDecorations) {
        if (!deco || deco->getDecorationType() != DECORATION_CUSTOM || deco->getDecorationLayer() != layer)
            continue;

        if (layer == DECORATION_LAYER_UNDER && isOverviewHyprbarDecoration(deco.get())) {
            renderOverviewHyprbarDecoration(state, monitor, window, deco.get(), windowBox, metrics);
            continue;
        }

        if (!drewAny) {
            g_pHyprRenderer->addPassElement(makeUnique<CRendererHintsPassElement>(CRendererHintsPassElement::SData{.renderModif = modif}));
            drewAny = true;
        }

        deco->updateWindow(window);
        deco->draw(monitor, metrics.targetOpacity);
        state.queuedAny = true;
    }

    if (drewAny)
        g_pHyprRenderer->addPassElement(makeUnique<CRendererHintsPassElement>(CRendererHintsPassElement::SData{.renderModif = Render::SRenderModifData{}}));

    return state;
}

} // namespace

bool shouldBlurBackground(const PHLWINDOW& window) {
    return window && g_pHyprRenderer && g_pHyprRenderer->shouldBlur(window);
}

static bool overviewBoxContains(const CBox& outer, const CBox& inner) {
    constexpr float EPSILON = 0.5F;
    return inner.x >= outer.x - EPSILON && inner.y >= outer.y - EPSILON && inner.x + inner.width <= outer.x + outer.width + EPSILON &&
        inner.y + inner.height <= outer.y + outer.height + EPSILON;
}

static bool overviewWindowFitsWorkspaceBox(const CBox* workspaceBox, const CBox* windowBox) {
    return !workspaceBox || !windowBox || overviewBoxContains(*workspaceBox, *windowBox);
}

static bool overviewWorkspaceBoxReadyForPrecomputedBlur(PHLMONITOR monitor, const CBox* workspaceBox) {
    if (!monitor || !workspaceBox)
        return true;

    constexpr float EPSILON    = 0.5F;
    const auto      RENDERSIZE = monitor->m_size * monitor->m_scale;

    return workspaceBox->x >= -EPSILON && workspaceBox->y >= -EPSILON && workspaceBox->x + workspaceBox->width <= RENDERSIZE.x + EPSILON &&
        workspaceBox->y + workspaceBox->height <= RENDERSIZE.y + EPSILON;
}

bool shouldUsePrecomputedBlur(const PHLWINDOW& window, PHLMONITOR monitor, const CBox* workspaceBox, const CBox* windowBox, bool dragged) {
    return !dragged && ScrollOverview::Config::getValue<bool>("decoration:blur:new_optimizations") && overviewShowsWindow(window) && !window->m_isFloating && shouldBlurBackground(window) &&
        overviewWindowFitsWorkspaceBox(workspaceBox, windowBox) && overviewWorkspaceBoxReadyForPrecomputedBlur(monitor, workspaceBox);
}

bool shouldUseBlurFramebuffer(const PHLWINDOW& window, PHLMONITOR monitor, const CBox* workspaceBox, const CBox* windowBox) {
    return shouldUsePrecomputedBlur(window, monitor, workspaceBox, windowBox) ||
        (overviewShowsWindow(window) && shouldBlurBackground(window) && window->m_ruleApplicator->xray().valueOr(false) &&
         overviewWindowFitsWorkspaceBox(workspaceBox, windowBox) && overviewWorkspaceBoxReadyForPrecomputedBlur(monitor, workspaceBox));
}

void notifySurfaceCommit(const SP<CWLSurfaceResource>& surface) {
    if (!surface || g_u2PrefilterFBs.empty())
        return;

    // Every size step of it (the key is the surface plus its step, 0..1).
    for (uintptr_t step = 0; step <= 1; ++step)
        if (const auto IT = g_u2PrefilterFBs.find(reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(surface.get()) + step)); IT != g_u2PrefilterFBs.end())
            IT->second.dirty = true;
}


// NOT FREED WHEN THE OVERVIEW CLOSES (user, 2026-09-16: "the workspace strip is
// visibly refreshing its previews ... antialiasing is not present prior to the
// refresh"). It was, and every open therefore started cold: the minis and tiles
// drew plain bilinear -- at strip scale, visibly aliased -- until each window's
// mipmap was rebuilt, and Y18's one-allocation-per-frame budget made that a
// window-by-window sharpening anyone could watch. Kept, the next open finds
// every mipmap it drew last time and only re-copies the windows that committed
// in between (a blit, no allocation). The price is the copies' GPU memory
// while the overview is down, bounded by the windows that exist: an entry
// whose surface is gone is dropped here, and one not drawn for a while is
// dropped by the prune on the next open.
void trimTexturePrefilterCache() {
    std::erase_if(g_u2PrefilterFBs, [](auto& kv) {
        if (kv.second.alive())
            return false;
        if (kv.second.fb)
            kv.second.fb->release();
        return true;
    });
}

// Everything, for plugin unload: the framebuffers are this object's to free
// while its code and the EGL context are still there.
std::pair<size_t, size_t> texturePrefilterCacheBytes() {
    size_t n = 0, bytes = 0;
    for (const auto& [key, entry] : g_u2PrefilterFBs) {
        if (!entry.fb || !entry.fb->isAllocated())
            continue;
        ++n;
        bytes += sc<size_t>(entry.fb->m_size.x * entry.fb->m_size.y * 4.0 * 4.0 / 3.0);
    }
    return {n, bytes};
}

void releaseTexturePrefilterCache() {
    for (auto& [key, entry] : g_u2PrefilterFBs) {
        if (entry.fb)
            entry.fb->release();
    }
    g_u2PrefilterFBs.clear();
}

void forceDecoRecalc(const PHLWINDOW& window) {
    const auto WINDOW = getOverviewWindowToShow(window);
    if (!validMapped(WINDOW))
        return;

    g_pDecorationPositioner->forceRecalcFor(WINDOW);
    WINDOW->updateWindowDecos();
}

void renderOverviewWindow(const SRenderParams& params) {
    if (!params.window)
        return;

    // X91. Dock live previews: scale+translate the real window into the cell
    // without rewriting geometry. Rewriting size (and not reliably position for
    // tiled windows) drew a mini rounded surface at the real window's top-left.
    if (params.dockLivePreview && params.monitor) {
        const auto MONITOR = params.monitor;
        const auto WINDOW  = params.window;

        const auto REALSIZE = WINDOW->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
        if (REALSIZE.x < 1.0 || REALSIZE.y < 1.0)
            return;

        // The window as renderWindow(ignorePosition) will lay it out: at the
        // PREVIEW MONITOR'S ORIGIN, not where it really is.
        //
        // (user, 2026-09-14: "previews on dock are flaky if the apps live outside
        // of the monitor I'm looking at the dock from"). Drawn at its real
        // position, a window on another monitor sits outside this monitor's
        // pixels: the nested pass discards its surfaces as off-screen (their
        // bounding box ignores the scale/translate modifier), and the draw
        // clips its damage against that same off-screen box, so the cell stayed
        // empty -- every frame for one monitor away, and whenever a workspace
        // animation parked the window off-screen on this one (Z1, below).
        // Pinned to the origin it is always on-screen before the modifier moves
        // it into the cell, wherever the window lives.
        const CBox REALPX{
            Vector2D{},
            REALSIZE * MONITOR->m_scale,
        };
        if (REALPX.width < 1.0 || REALPX.height < 1.0 || params.windowBox.width < 1.0 || params.windowBox.height < 1.0)
            return;

        // Match prior height-based preview scale; letterbox if aspects differ.
        const float S = std::min(sc<float>(params.windowBox.width) / sc<float>(REALPX.width), sc<float>(params.windowBox.height) / sc<float>(REALPX.height));
        if (S <= 0.F)
            return;

        // SCALE from monitor origin, then TRANSLATE so the scaled real box lands
        // on the preview cell (centered if letterboxed).
        const float SCALEDW = sc<float>(REALPX.width) * S;
        const float SCALEDH = sc<float>(REALPX.height) * S;
        const float OFFX    = sc<float>(params.windowBox.x) + (sc<float>(params.windowBox.width) - SCALEDW) / 2.F - sc<float>(REALPX.x) * S;
        const float OFFY    = sc<float>(params.windowBox.y) + (sc<float>(params.windowBox.height) - SCALEDH) / 2.F - sc<float>(REALPX.y) * S;

        Render::SRenderModifData modif;
        modif.modifs.emplace_back(Render::SRenderModifData::RMOD_TYPE_SCALE, S);
        modif.modifs.emplace_back(Render::SRenderModifData::RMOD_TYPE_TRANSLATE, Vector2D{OFFX, OFFY});

        auto queuePreview = [&]() {
            // BLUR BEHIND THE PREVIEW (user, 2026-09-29, nested test: grid tiles
            // blurred, hover previews showed the dock sharp through a see-through
            // window). renderWindow(standalone) switches blur off outright
            // (Renderer.cpp:600), and switching it back on per surface is wrong:
            // Hyprland's live blur reads the framebuffer at the surface's box
            // BEFORE the scale/translate modifier, i.e. the monitor's top-left
            // corner, and that is what showed through. So the preview frosts its
            // own cell -- what is really under it -- ahead of the modifier, and
            // the window's own alpha goes on top, exactly as on the desktop.
            if (g_pHyprRenderer->shouldBlur(WINDOW)) {
                COverviewBlurPassElement::SData frost;
                // The window's own letterboxed rect in the cell, not the cell.
                frost.box           = CBox{sc<double>(OFFX + sc<float>(REALPX.x) * S), sc<double>(OFFY + sc<float>(REALPX.y) * S), sc<double>(SCALEDW), sc<double>(SCALEDH)}.round();
                frost.rounding      = sc<int>(std::round(WINDOW->rounding() * MONITOR->m_scale * S));
                frost.roundingPower = WINDOW->roundingPower();
                frost.alpha         = 1.F;
                g_pHyprRenderer->addPassElement(makeUnique<COverviewBlurPassElement>(frost));
            }
            g_pHyprRenderer->addPassElement(makeUnique<CRendererHintsPassElement>(CRendererHintsPassElement::SData{.renderModif = modif}));

            // Z1 (a window whose workspace is parked off-screen by a switch
            // animation was drawn a monitor away and clipped to nothing) needed
            // the workspace's m_renderOffset zeroed around this call. With
            // ignorePosition renderWindow never reads the position the offset
            // is added to, so there is nothing left to neutralize.
            auto&            passElements = g_pHyprRenderer->currentPass().m_passElements;
            const size_t     firstElement = passElements.size();
            // standalone: no decorate path side effects; content only. Rounding is
            // forced below so the cell still reads as a rounded thumbnail.
            {
                ScrollOverview::FlightBlur::CSuspend noFlightBlur(WINDOW);
                g_pHyprRenderer->renderWindow(WINDOW, MONITOR, params.now, false, Render::RENDER_PASS_MAIN, /*ignorePosition*/ true, true);
            }
            roundStandaloneWindowPassElements(WINDOW, MONITOR, S, firstElement);
            // The overview's U2 mipmap prefilter, for the dock's thumbnails too
            // (user, 2026-09-24: "dock window previews lack antialiasing"): a
            // window shrunk to a preview is minified well past bilinear's
            // limit, which is what aliased its text and edges.
            prefilterOverviewWindowTextures(WINDOW, MONITOR, S, firstElement);
            // Clip every surface of this window to the preview cell so nothing bleeds.
            for (size_t i = firstElement; i < passElements.size(); ++i) {
                auto* surface = dc<CSurfacePassElement*>(passElements[i].element.get());
                if (!surface || surface->m_data.pWindow != WINDOW)
                    continue;
                surface->m_data.clipBox = params.windowBox;
            }

            g_pHyprRenderer->addPassElement(makeUnique<CRendererHintsPassElement>(CRendererHintsPassElement::SData{.renderModif = Render::SRenderModifData{}}));
        };

        if (params.ownsRenderPass) {
            queuePreview();
            OverviewRender::flushPass(MONITOR);
            return;
        }

        // Hover previews share Hyprland's POST_WINDOWS pass. Queuing a second
        // copy of an on-desktop window there lets pass simplify drop the real
        // draw (same untransformed box) so the window vanishes while hovered.
        auto nested = makeUnique<Render::CRenderPass>();
        {
            auto restorePass = g_pHyprRenderer->redirectPass(nested.get());
            queuePreview();
        }
        g_pHyprRenderer->addPassElement(makeUnique<CDockLivePreviewPassElement>(CDockLivePreviewPassElement::SData{
            .pass = std::move(nested),
            .box  = params.windowBox,
        }));
        return;
    }

    const bool             fullscreen = Fullscreen::controller()->isFullscreen(params.window);
    SOverviewWindowMetrics metrics    = getOverviewWindowMetrics(params.monitor, params.window, params.renderScale);

    // Y5. Fold the flight's fade into the metrics so the shadow, the border and
    // every decoration fade WITH the content instead of staying solid around a
    // vanishing window. The content itself is faded after renderWindow, below --
    // renderWindow reads the window's own alpha, which this must not touch.
    metrics.targetOpacity *= std::clamp(params.alpha, 0.F, 1.F);

    if (!fullscreen)
        renderOverviewWindowShadow(params.monitor, params.window, params.windowBox, metrics, params.selected);

    // The UNDER decorations are inseparable from the flush: they queue, the pass
    // is rendered, and only then may their state be restored. A caller that does
    // not own the pass cannot do the middle step, so it does not do the first
    // either -- queuing them and restoring without rendering would draw them
    // with the wrong state.
    if (params.ownsRenderPass) {
        const auto underDecos =
            renderOverviewCustomDecorations(params.monitor, params.window, params.workspaceBox ? *params.workspaceBox : CBox{}, params.windowBox, metrics, DECORATION_LAYER_UNDER);
        if (underDecos.queuedAny) {
            OverviewRender::flushPass(params.monitor);
            for (auto it = underDecos.restoreFns.rbegin(); it != underDecos.restoreFns.rend(); ++it)
                (*it)();
        }
    }

    const Vector2D previousWindowPos       = params.window->m_realPosition->value();
    const Vector2D previousWindowSize      = params.window->m_realSize->value();
    const bool     previousAnimatingIn     = params.window->m_animatingIn;
    const bool     previousSizeAnimating   = params.window->sizeAnimation()->isBeingAnimated();
    const bool     previousPosAnimating    = params.window->positionAnimation()->isBeingAnimated();
    const auto     WORKSPACE               = params.window->m_workspace;
    const bool     OVERRIDEWORKSPACEOFFSET = WORKSPACE && !params.window->m_pinned;
    const Vector2D previousWorkspaceOffset = OVERRIDEWORKSPACEOFFSET ? WORKSPACE->m_renderOffset->value() : Vector2D{};

    // Mark BOTH animated before writing current. Writing position while the
    // variable is "not being animated" can commit the tile box as the window's
    // goal; on close that left a border/frame at the top-left (X91 found the
    // same leak on dock live previews, which no longer rewrite geometry).
    COverviewAnimatedVariableAccess::setBeingAnimated(params.window->positionAnimation().get(), true);
    COverviewAnimatedVariableAccess::setBeingAnimated(params.window->sizeAnimation().get(), true);
    params.window->positionAnimation()->value() = params.monitor->m_position + params.windowBox.pos() / params.monitor->m_scale - params.window->m_floatingOffset;
    params.window->sizeAnimation()->value()     = params.windowBox.size() / params.monitor->m_scale;
    params.window->m_animatingIn                 = true;
    if (OVERRIDEWORKSPACEOFFSET)
        WORKSPACE->m_renderOffset->value() = {};

    // Not flushed here: carry the tile geometry to the pass's draw time (above).
    SP<SDeferredWindowGeometry> deferredGeometry;
    if (!params.ownsRenderPass) {
        deferredGeometry = makeShared<SDeferredWindowGeometry>(SDeferredWindowGeometry{
            .window              = params.window,
            .workspace           = WORKSPACE,
            .pos                 = params.window->positionAnimation()->value(),
            .size                = params.window->sizeAnimation()->value(),
            .zeroWorkspaceOffset = OVERRIDEWORKSPACEOFFSET,
        });
        g_pHyprRenderer->addPassElement(makeUnique<CDeferredWindowGeometryPassElement>(deferredGeometry, true));
    }

    auto restoreWindowGeometry = Hyprutils::Utils::CScopeGuard([&] {
        params.window->positionAnimation()->value() = previousWindowPos;
        params.window->sizeAnimation()->value()     = previousWindowSize;
        params.window->m_animatingIn                 = previousAnimatingIn;
        COverviewAnimatedVariableAccess::setBeingAnimated(params.window->positionAnimation().get(), previousPosAnimating);
        COverviewAnimatedVariableAccess::setBeingAnimated(params.window->sizeAnimation().get(), previousSizeAnimating);
        if (OVERRIDEWORKSPACEOFFSET && WORKSPACE)
            WORKSPACE->m_renderOffset->value() = previousWorkspaceOffset;
    });

    // X90. Mini renders used to scale decoration:blur:size by renderScale for the
    // whole compositor. Dock live previews call this path with renderScale ~0.05
    // (size collapses to 1). Hyprland rebuilds Monitor::m_blurFB from that value
    // when a window takes the precomputed path; restoring the config int does
    // NOT rebuild the FB, so every real desktop window kept the weak frost
    // until something else dirtied it. Overview tiles with windows present
    // hit the same leak relative to chrome that blurred after them.
    //
    // Rules:
    // 1. Only scale blur when scaleBlurToRenderScale (overview tiles, not dock).
    // 2. While scaled, force LIVE blur (block optimization) so m_blurFB is never
    //    rewritten at the temporary strength.
    // 3. On restore, still mark m_blurFBDirty in case anything else sampled it.
    const auto u3PrevBlurSize = ScrollOverview::Config::getValue<int>("decoration:blur:size");
    const auto u3BlurSize     = std::max(1, sc<int>(std::round(u3PrevBlurSize * params.renderScale)));
    const bool scaleBlur      = params.scaleBlurToRenderScale && u3BlurSize != u3PrevBlurSize && u3PrevBlurSize > 0;
    if (scaleBlur)
        ScrollOverview::Config::setValue("decoration:blur:size", u3BlurSize);
    auto restoreBlurSize = Hyprutils::Utils::CScopeGuard([&] {
        if (!scaleBlur)
            return;
        ScrollOverview::Config::setValue("decoration:blur:size", u3PrevBlurSize);
        if (params.monitor)
            params.monitor->m_blurFBDirty = true;
    });

    const size_t firstWindowPassElement = g_pHyprRenderer->currentPass().m_passElements.size();
    // Never rebuild m_blurFB under a temporary blur size.
    const bool   usePrecomputedBlur     = !scaleBlur && shouldUsePrecomputedBlur(params.window, params.monitor, params.workspaceBox, &params.windowBox, params.dragged);
    {
        ScrollOverview::FlightBlur::CSuspend noFlightBlur(params.window);
        g_pHyprRenderer->renderWindow(params.window, params.monitor, params.now, false, Render::RENDER_PASS_ALL, false, false);
    }
    scaleOverviewSubsurfaceOffsets(params.window, params.window->sizeAnimation()->value(), firstWindowPassElement);
    prefilterOverviewWindowTextures(params.window, params.monitor,
                                    previousWindowSize.x * params.monitor->m_scale > 0.0 ? params.windowBox.width / (previousWindowSize.x * params.monitor->m_scale) : 0.0,
                                    firstWindowPassElement);
    if (!usePrecomputedBlur)
        blockOverviewWindowBlurOptimization(params.window, firstWindowPassElement);

    // Y5. The content's half of the fade. fadeAlpha is what Hyprland multiplies
    // a window's own fade in/out by, so this rides the existing plumbing rather
    // than adding a second notion of opacity.
    if (params.alpha < 1.F) {
        auto& passElements = g_pHyprRenderer->currentPass().m_passElements;
        for (size_t i = firstWindowPassElement; i < passElements.size(); ++i) {
            auto* surface = dc<CSurfacePassElement*>(passElements[i].element.get());
            if (surface && surface->m_data.pWindow == params.window)
                surface->m_data.fadeAlpha *= std::clamp(params.alpha, 0.F, 1.F);
        }
    }
    roundStandaloneWindowPassElements(params.window, params.monitor, params.renderScale, firstWindowPassElement);

    renderOverviewCustomDecorations(params.monitor, params.window, params.workspaceBox ? *params.workspaceBox : CBox{}, params.windowBox, metrics, DECORATION_LAYER_OVER);
    renderOverviewCustomDecorations(params.monitor, params.window, params.workspaceBox ? *params.workspaceBox : CBox{}, params.windowBox, metrics, DECORATION_LAYER_OVERLAY);

    if (!fullscreen)
        renderOverviewGroupTabIndicators(params.monitor, params.window, params.windowBox, metrics, metrics.targetOpacity);
    if (!fullscreen)
        renderOverviewGroupTabTitles(params.monitor, params.window, params.windowBox, metrics, metrics.targetOpacity);

    if (!fullscreen)
        renderOverviewWindowBorder(params.monitor, params.window, params.windowBox, metrics, params.selected);

    // Y17 (GPU, 2026-09-14): the per-window flush was left alone. It is not
    // bookkeeping -- the UNDER decorations above depend on being rendered
    // before their state is restored, the temporary window geometry is undone
    // by a scope guard the moment this returns, and the blur-size override is
    // restored with it. Batching windows into one pass would draw all of them
    // with the last one's state. Not worth the risk for a CPU-side saving.
    if (params.ownsRenderPass)
        OverviewRender::flushPass(params.monitor);
    else if (deferredGeometry)
        g_pHyprRenderer->addPassElement(makeUnique<CDeferredWindowGeometryPassElement>(deferredGeometry, false));
}

} // namespace OverviewWindow
