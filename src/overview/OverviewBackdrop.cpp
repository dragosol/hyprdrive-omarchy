// COverview: wallpaper stash, wallpaper rate and listeners, card wallpaper and backdrop frost caches.
// Cut from Overview.cpp by pure code motion.
#include "OverviewParts.hpp"

using namespace OverviewDetail;


namespace {
    constexpr double                              AUTO_LIVE_MAX_HZ = 4.0; // a clock tick or a slideshow stays live; an animation is frozen
    constexpr auto                                LIVE_MIN_INTERVAL = std::chrono::milliseconds(33); // LIVE re-blurs at most 30 times a second

    double secondsSince(const Time::steady_tp& t, const Time::steady_tp& now) {
        return std::chrono::duration<double>(now - t).count();
    }

    void noteWallpaperCommit(MONITORID id) {
        auto&      R   = g_wallpaperRate[id];
        const auto NOW = Time::steadyNow();
        const auto E   = secondsSince(R.windowStart, NOW);
        if (R.windowStart.time_since_epoch().count() == 0 || E >= 1.0) {
            R.rate        = R.windowStart.time_since_epoch().count() == 0 ? 0.0 : R.count / std::max(1.0, E);
            R.windowStart = NOW;
            R.count       = 0;
        }
        ++R.count;
    }

    double wallpaperRate(MONITORID id) {
        const auto IT = g_wallpaperRate.find(id);
        if (IT == g_wallpaperRate.end())
            return 0.0;
        const auto E = secondsSince(IT->second.windowStart, Time::steadyNow());
        // A wallpaper that stopped: the second that is over counts, not the last busy one.
        return E >= 1.0 ? IT->second.count / E : std::max(IT->second.rate, sc<double>(IT->second.count));
    }

    // The configured mode reads BOTTOM too (the live wallpaper's layer).
    bool backdropSeesBottom(EBackdrop m) {
        return m == EBackdrop::SNAPSHOT || m == EBackdrop::LIVE || m == EBackdrop::AUTO;
    }
}

namespace OverviewDetail {

    // What this monitor's frost does right now: AUTO picks LIVE or SNAPSHOT.
    EBackdrop resolvedBackdrop(MONITORID id) {
        const auto M = Hyprdrive::Config::getBackdrop();
        if (M != EBackdrop::AUTO)
            return M;
        return wallpaperRate(id) >= AUTO_LIVE_MAX_HZ ? EBackdrop::SNAPSHOT : EBackdrop::LIVE;
    }
}

namespace {

    void listenWhileOpen(const PHLMONITOR& monitor, uint64_t layerSig) {
        auto& L = g_openListen[monitor->m_id];
        if (!L.listeners.empty() && L.layerSig == layerSig)
            return;
        L.listeners.clear();
        L.layerSig     = layerSig;
        const auto ID  = monitor->m_id;
        for (const auto LAYER : {ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM}) {
            for (const auto& weak : monitor->m_layerSurfaceLayers[LAYER]) {
                const auto LS   = weak.lock();
                const auto RES  = LS ? LS->m_layerSurface.lock() : nullptr;
                const auto SURF = RES ? RES->m_surface.lock() : nullptr;
                if (SURF)
                    L.listeners.emplace_back(SURF->m_events.commit.listen([ID] {
                        ++g_wallpaperSeq[ID];
                        noteWallpaperCommit(ID);
                    }));
            }
        }
    }

    void markStashDirty(MONITORID id, bool background) {
        ++g_wallpaperSeq[id];
        noteWallpaperCommit(id);
        const auto IT = g_wallpaperStash.find(id);
        if (IT == g_wallpaperStash.end())
            return;
        ++g_wallpaperCommits;
        // Same split as shouldHandleSurfaceDamage: the card fill composites
        // BACKGROUND and BOTTOM, the backdrop BACKGROUND, and BOTTOM too when
        // the backdrop mode shows a live wallpaper -- the next open then blurs
        // a fresh frame of it (the snapshot), and a still one costs nothing.
        IT->second.cardWallpaperDirty = true;
        if (background || backdropSeesBottom(Hyprdrive::Config::getBackdrop()))
            IT->second.backdropDirty = true;
    }

    void listenToWallpaper(const PHLMONITOR& monitor, SWallpaperStash& st) {
        const auto ID = monitor->m_id;
        for (const auto LAYER : {ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM}) {
            const bool BACKDROP = LAYER == ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND; // markStashDirty's `background`
            for (const auto& weak : monitor->m_layerSurfaceLayers[LAYER]) {
                const auto LS   = weak.lock();
                const auto RES  = LS ? LS->m_layerSurface.lock() : nullptr;
                const auto SURF = RES ? RES->m_surface.lock() : nullptr;
                if (SURF)
                    st.listeners.emplace_back(SURF->m_events.commit.listen([ID, BACKDROP] { markStashDirty(ID, BACKDROP); }));
            }
        }
        // A wallpaper layer that appears while closed (theme switch, swaybg
        // restart): its first frame counts as a change; the signature check
        // catches the layer set itself.
        st.listeners.emplace_back(Event::bus()->m_events.layer.opened.listen([ID](PHLLS ls) {
            if (ls && ls->m_layer <= ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM && ls->m_monitor && ls->m_monitor->m_id == ID)
                markStashDirty(ID, true);
        }));
    }

    void releaseStash(SWallpaperStash& st) {
        if (st.backdropFB)
            st.backdropFB->release();
        if (st.cardWallpaperFB)
            st.cardWallpaperFB->release();
        st = {};
    }
}

namespace Hyprdrive {

    std::string wallpaperStashReport() {
        std::string out = std::format("  | wallpaper stash: commits while closed {}", g_wallpaperCommits);
        for (const auto& [id, seq] : g_wallpaperSeq)
            out += std::format(" [mon {} wallpaper commits {} rate {:.1f}/s listening-open {} backdrop {}]", id, seq, wallpaperRate(id),
                               g_openListen.contains(id) ? g_openListen.at(id).listeners.size() : 0, sc<int>(resolvedBackdrop(id)));
        for (const auto& [id, st] : g_wallpaperStash)
            out += std::format(" [mon {} backdrop {} dirty {} card {} dirty {}]", id, (bool)st.backdropFB, st.backdropDirty, (bool)st.cardWallpaperFB, st.cardWallpaperDirty);
        return out;
    }

    // Needs the EGL context current. std::nullopt = every monitor (unload).
    void releaseWallpaperStash(std::optional<MONITORID> monitor) {
        for (auto it = g_openListen.begin(); it != g_openListen.end();)
            it = (!monitor || it->first == *monitor) ? g_openListen.erase(it) : std::next(it);
        for (auto it = g_wallpaperStash.begin(); it != g_wallpaperStash.end();) {
            if (!monitor || it->first == *monitor) {
                releaseStash(it->second);
                it = g_wallpaperStash.erase(it);
            } else
                ++it;
        }
    }
}

void COverview::adoptWallpaperStash() {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR)
        return;
    const auto IT = g_wallpaperStash.find(MONITOR->m_id);
    if (IT == g_wallpaperStash.end())
        return;
    auto& st                        = IT->second;
    frost.backdropFB                = std::move(st.backdropFB);
    frost.cardWallpaperFB           = std::move(st.cardWallpaperFB);
    frost.backdropDirty             = st.backdropDirty;
    frost.cardWallpaperDirty        = st.cardWallpaperDirty;
    frost.lastBackdropSig           = st.lastBackdropSig;
    frost.lastCardWallpaperSig      = st.lastCardWallpaperSig;
    frost.lastBackdropWallpaperMode = st.lastBackdropWallpaperMode;
    g_wallpaperStash.erase(IT);
}

void COverview::stashWallpaperCaches() {
    const auto MONITOR = pMonitor.lock();
    if (MONITOR)
        g_openListen.erase(MONITOR->m_id); // closed: the stash's own listeners take over
    // A monitor that is gone (or going) takes its caches with it: releaseGL.
    if (!MONITOR || !MONITOR->m_enabled || (!frost.backdropFB && !frost.cardWallpaperFB))
        return;
    auto& st = g_wallpaperStash[MONITOR->m_id];
    releaseStash(st); // a second overview on the same monitor closing: newest wins
    st.backdropFB                = std::move(frost.backdropFB);
    st.cardWallpaperFB           = std::move(frost.cardWallpaperFB);
    st.backdropDirty             = frost.backdropDirty;
    st.cardWallpaperDirty        = frost.cardWallpaperDirty;
    st.lastBackdropSig           = frost.lastBackdropSig;
    st.lastCardWallpaperSig      = frost.lastCardWallpaperSig;
    st.lastBackdropWallpaperMode = frost.lastBackdropWallpaperMode;
    listenToWallpaper(MONITOR, st);
}

void COverview::renderWallpaperLayers(PHLMONITOR monitor, const CBox& workspaceBox, float renderScale, const Time::steady_tp& now, float alpha) {
    if (!monitor)
        return;

    renderOverviewLayerLevel(monitor, ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, workspaceBox, renderScale, now, alpha);
}

void COverview::renderGlobalWallpaper(PHLMONITOR monitor, const Time::steady_tp& now, bool includeBottom) {
    if (!monitor)
        return;

    g_pHyprRenderer->renderBackground(monitor);

    for (const auto LAYER : {ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM}) {
        if (LAYER == ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM && !includeBottom)
            break;
        for (auto const& ls : monitor->m_layerSurfaceLayers[LAYER]) {
            if (!Desktop::View::validMapped(ls.lock()))
                continue;

            g_pHyprRenderer->renderLayer(ls.lock(), monitor, now);
        }
    }
}

// Y17 (GPU, 2026-09-14). The two wallpaper caches below are rebuilt only when
// something they draw changed. Two halves make that safe:
//
//   1. frost.cardWallpaperDirty / frost.backdropDirty, set from the wallpaper
//      LAYER COMMIT hook (shouldHandleSurfaceDamage) -- the same damageSurface
//      signal the overview already trusts to repaint a live tile. If a commit
//      did not reach that hook the tile would be stale too, so the caches are
//      exactly as fresh as the rest of the overview.
//
//   2. this signature, for everything a commit does NOT report: the monitor's
//      pixel size, transform and scale, the render target's format and image
//      description, renderBackground's inputs (the wallpaper texture object,
//      its fade-in opacity animation and the misc:* keys that pick what it
//      paints), each wallpaper layer's identity/geometry/alpha/texture (an
//      alpha fade or slide animates with no commit at all), and -- for the
//      backdrop only -- the blur knobs blurFramebufferWithDamage reads
//      globally and updateBackdropBlurCache does NOT force (brightness,
//      vibrancy, vibrancy_darkness, the blend-path switch), so a theme reload
//      rebuilds the cached blur.
//
// Cost is a handful of pointer reads and config-map lookups, twice a frame.
static inline void frostSigMix(uint64_t& h, uint64_t v) {
    h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
}

static inline void frostSigMixD(uint64_t& h, double v) {
    static_assert(sizeof(uint64_t) == sizeof(double));
    frostSigMix(h, __builtin_bit_cast(uint64_t, v));
}

static inline void frostSigMixP(uint64_t& h, const void* p) {
    frostSigMix(h, sc<uint64_t>(rc<uintptr_t>(p)));
}

// includeTextures = false for the backdrop: a commit is what tells it a
// wallpaper changed (markStashDirty closed, shouldHandleSurfaceDamage open),
// and a frozen mode must not see a moving wallpaper's new buffer every frame.
static uint64_t wallpaperCacheSignature(PHLMONITOR monitor, bool includeBottomLayer, bool includeBlurKnobs, bool includeTextures = true) {
    // Never 0: 0 is the "no signature yet" sentinel in COverviewFrostCache.
    uint64_t h = 0xcbf29ce484222325ULL;
    if (!monitor)
        return h;

    frostSigMixD(h, monitor->m_pixelSize.x);
    frostSigMixD(h, monitor->m_pixelSize.y);
    frostSigMixD(h, monitor->m_transformedSize.x);
    frostSigMixD(h, monitor->m_transformedSize.y);
    frostSigMix(h, sc<uint64_t>(monitor->m_transform));
    frostSigMixD(h, monitor->m_scale);

    if (const auto& CURRENTFB = g_pHyprRenderer->m_renderData.currentFB; CURRENTFB) {
        frostSigMix(h, sc<uint64_t>(CURRENTFB->m_drmFormat));
        frostSigMixP(h, CURRENTFB->imageDescription().get());
    }

    // renderBackground(): the hyprpaper/fallback wallpaper and its fade.
    frostSigMixP(h, monitor->m_background.get());
    if (monitor->m_backgroundOpacity)
        frostSigMixD(h, monitor->m_backgroundOpacity->value());
    frostSigMix(h, sc<uint64_t>(Hyprdrive::Config::getValue<int>("misc:background_color")));
    frostSigMix(h, sc<uint64_t>(Hyprdrive::Config::getValue<int>("misc:disable_hyprland_logo")));
    frostSigMix(h, sc<uint64_t>(Hyprdrive::Config::getValue<int>("misc:disable_splash_rendering")));

    const auto MIXLAYER = [&h, includeTextures](const PHLLS& ls) {
        if (!Desktop::View::validMapped(ls))
            return;

        frostSigMixP(h, ls.get());
        const auto BOX = ls->geometricBox(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
        frostSigMixD(h, BOX.x);
        frostSigMixD(h, BOX.y);
        frostSigMixD(h, BOX.width);
        frostSigMixD(h, BOX.height);
        frostSigMixD(h, ls->alpha().value());
        if (!includeTextures)
            return;
        if (const auto RES = ls->m_layerSurface ? ls->m_layerSurface->m_surface.lock() : SP<CWLSurfaceResource>{})
            frostSigMixP(h, RES->m_current.texture.get());
    };

    for (auto const& ls : monitor->m_layerSurfaceLayers[ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND])
        MIXLAYER(ls.lock());

    // The card fill also composites BOTTOM (Omarchy's wallpaper often lives
    // there); the backdrop only in the modes that show a live wallpaper.
    if (includeBottomLayer) {
        for (auto const& ls : monitor->m_layerSurfaceLayers[ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM])
            MIXLAYER(ls.lock());
    }

    if (includeBlurKnobs) {
        frostSigMixD(h, Hyprdrive::Config::getValue<float>("decoration:blur:brightness"));
        frostSigMixD(h, Hyprdrive::Config::getValue<float>("decoration:blur:vibrancy"));
        frostSigMixD(h, Hyprdrive::Config::getValue<float>("decoration:blur:vibrancy_darkness"));
        frostSigMix(h, sc<uint64_t>(Hyprdrive::Config::getValue<int>("render:use_shader_blur_blend")));
    }

    return h;
}

// E8-c, GNOME-style: the wallpaper at native size, so each card can composite
// it with shader rounding (real clip + AA) instead of a painted mask.
// Y17: no longer re-rendered every frame -- see wallpaperCacheSignature above
// for the two halves of the invalidation.
void COverview::updateCardWallpaperFB(PHLMONITOR monitor, const Time::steady_tp& now) {
    if (!monitor || !g_pHyprRenderer->m_renderData.currentFB)
        return;

    if (const auto SIG = wallpaperCacheSignature(monitor, /* includeBottomLayer */ true, /* includeBlurKnobs */ false); SIG != frost.lastCardWallpaperSig) {
        frost.cardWallpaperDirty  = true;
        frost.lastCardWallpaperSig = SIG;
    }

    const auto FBSIZE     = monitor->m_pixelSize;
    const auto RENDERSIZE = monitor->m_transformedSize;
    const auto FBFORMAT   = g_pHyprRenderer->m_renderData.currentFB->m_drmFormat;
    if (!frost.cardWallpaperFB)
        frost.cardWallpaperFB = g_pHyprRenderer->createFB("hyprdrive_card_wallpaper");

    if (!frost.cardWallpaperFB || !frost.cardWallpaperFB->isAllocated() || frost.cardWallpaperFB->m_size != FBSIZE || frost.cardWallpaperFB->m_drmFormat != FBFORMAT) {
        if (frost.cardWallpaperFB)
            frost.cardWallpaperFB->release();
        if (!frost.cardWallpaperFB || !frost.cardWallpaperFB->alloc(sc<int>(FBSIZE.x), sc<int>(FBSIZE.y), FBFORMAT))
            return;
        // A fresh allocation holds nothing; the size/format check above is also
        // what catches a mode or scale change.
        frost.cardWallpaperDirty = true;
    }

    frost.cardWallpaperFB->setImageDescription(g_pHyprRenderer->m_renderData.currentFB->imageDescription());

    // Y17: everything below re-renders the whole wallpaper into the FB. The
    // cached pixels do not depend on the overview's scale or view offset -- the
    // cards sample this FB through their own UVs -- so nothing per-frame forces
    // a rebuild.
    if (!frost.cardWallpaperDirty)
        return;

    const CRegion fullDamage{CBox{0, 0, RENDERSIZE.x, RENDERSIZE.y}};

    {
        auto bindCard = g_pHyprRenderer->bindTempFB(frost.cardWallpaperFB);

        // The frame damage can be tiny on an idle frame, and queued surface
        // elements are culled against it -- which left this FB black except the
        // clear. Same save/override/restore dance the shadow element does.
        const auto SAVEDDAMAGE               = g_pHyprRenderer->m_renderData.damage;
        g_pHyprRenderer->m_renderData.damage = fullDamage;
        auto restoreDamage = Hyprutils::Utils::CScopeGuard([SAVEDDAMAGE] { g_pHyprRenderer->m_renderData.damage = SAVEDDAMAGE; });

        g_pHyprRenderer->draw(CClearPassElement::SClearData{CHyprColor{0.F, 0.F, 0.F, 0.F}}, fullDamage);
        // Same sources as renderGlobalWallpaper: hyprpaper via renderBackground,
        // then BACKGROUND/BOTTOM layers (Omarchy's wallpaper is often a BOTTOM
        // layer). The card fill must be the undimmed wallpaper, or the dimmed
        // canvas shows through the floating workspace.
        g_pHyprRenderer->renderBackground(monitor);
        for (const auto LAYER : {ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM}) {
            for (auto const& ls : monitor->m_layerSurfaceLayers[LAYER]) {
                if (!Desktop::View::validMapped(ls.lock()))
                    continue;

                g_pHyprRenderer->renderLayer(ls.lock(), monitor, now);
            }
        }
        OverviewRender::flushPass(monitor);
    }

    frost.cardWallpaperDirty = false;
}

void COverview::updateBackdropBlurCache(PHLMONITOR monitor, int wallpaperMode, const Time::steady_tp& now) {
    if (!monitor || wallpaperMode == 1 || !g_pHyprRenderer->m_renderData.currentFB)
        return;

    // The backdrop mode is part of the key: switching it in the panel rebuilds.
    const auto MODE      = Hyprdrive::Config::getBackdrop();
    const auto RESOLVED  = resolvedBackdrop(monitor->m_id);
    const bool BOTTOM    = backdropSeesBottom(MODE);
    const int  MODEKEY   = wallpaperMode * 8 + sc<int>(MODE);
    if (frost.lastBackdropWallpaperMode != MODEKEY) {
        frost.backdropDirty             = true;
        frost.lastBackdropWallpaperMode = MODEKEY;
    }

    // Y17: keyed on the blur knobs too -- this cache holds BLURRED pixels, so a
    // theme reload that moves brightness/vibrancy has to rebuild it.
    if (const auto SIG = wallpaperCacheSignature(monitor, /* includeBottomLayer */ BOTTOM, /* includeBlurKnobs */ true, /* includeTextures */ false);
        SIG != frost.lastBackdropSig) {
        frost.backdropDirty    = true;
        frost.lastBackdropSig  = SIG;
    }

    // A new frame of the wallpaper (its own commits, heard while open --
    // listenWhileOpen): LIVE re-blurs, capped below. A commit's damage does not
    // reliably reach shouldHandleSurfaceDamage while the overview is up
    // (headless, 2026-11-03: a live BOTTOM wallpaper never re-blurred LIVE).
    listenWhileOpen(monitor, frost.lastBackdropSig);
    if (const auto SEQ = g_wallpaperSeq[monitor->m_id]; SEQ != frost.lastBackdropSeq) {
        frost.lastBackdropSeq = SEQ;
        if (RESOLVED == EBackdrop::LIVE)
            frost.backdropDirty = true;
    }

    const auto FBSIZE     = monitor->m_pixelSize;
    const auto RENDERSIZE = monitor->m_transformedSize;
    const auto FBFORMAT   = g_pHyprRenderer->m_renderData.currentFB->m_drmFormat;
    if (!frost.backdropFB)
        frost.backdropFB = g_pHyprRenderer->createFB("hyprdrive_backdrop_blur");

    if (!frost.backdropFB || !frost.backdropFB->isAllocated() || frost.backdropFB->m_size != FBSIZE || frost.backdropFB->m_drmFormat != FBFORMAT) {
        if (frost.backdropFB)
            frost.backdropFB->release();
        if (!frost.backdropFB || !frost.backdropFB->alloc(sc<int>(FBSIZE.x), sc<int>(FBSIZE.y), FBFORMAT))
            return;
        frost.backdropDirty = true;
    }

    frost.backdropFB->setImageDescription(g_pHyprRenderer->m_renderData.currentFB->imageDescription());

    // Y17 (GPU, 2026-09-14). THE early-out. backdropDirty was written at the
    // bottom of this function and never read, so every frame of every monitor
    // re-rendered the wallpaper and ran a forced 4-pass full-resolution kawase
    // over it -- the single largest GPU item in an overview frame. The cached
    // image is independent of the overview's scale and view offset (the open
    // animation rides on renderBackdropBlurCache's alpha, and the 80% dim is a
    // separate rect drawn by the caller), so there is nothing per-frame left in
    // it to recompute.
    if (!frost.backdropDirty)
        return;

    // LIVE follows a moving wallpaper at most 30 times a second; the frame
    // that skips asks for the next one, so the last frame of it is not missed.
    if (RESOLVED == EBackdrop::LIVE && frost.lastBackdropBuild.time_since_epoch().count() != 0 && now - frost.lastBackdropBuild < LIVE_MIN_INTERVAL &&
        now >= frost.lastBackdropBuild) {
        requestInputFrame();
        return;
    }
    frost.lastBackdropBuild = now;

    const CRegion fullDamage{CBox{0, 0, RENDERSIZE.x, RENDERSIZE.y}};

    {
        auto bindBackdrop = g_pHyprRenderer->bindTempFB(frost.backdropFB);
        g_pHyprRenderer->draw(CClearPassElement::SClearData{CHyprColor{0.F, 0.F, 0.F, 1.F}}, fullDamage);
        renderGlobalWallpaper(monitor, now, BOTTOM);
        // OFF: the wallpaper darkened, no blur -- the same baked-in dim.
        if (MODE == EBackdrop::OFF)
            g_pHyprRenderer->draw(CRectPassElement::SRectData{.box = CBox{0, 0, RENDERSIZE.x, RENDERSIZE.y}, .color = CHyprColor{0.F, 0.F, 0.F, BACKDROP_DIM}},
                                  fullDamage);
        OverviewRender::flushPass(monitor);
    }
    if (MODE == EBackdrop::OFF) {
        frost.backdropDirty = false;
        return;
    }

    const auto PREV_ENABLED  = Hyprdrive::Config::getValue<bool>("decoration:blur:enabled");
    const auto PREV_SIZE     = Hyprdrive::Config::getValue<int>("decoration:blur:size");
    const auto PREV_PASSES   = Hyprdrive::Config::getValue<int>("decoration:blur:passes");
    const auto PREV_NOISE    = Hyprdrive::Config::getValue<float>("decoration:blur:noise");
    const auto PREV_CONTRAST = Hyprdrive::Config::getValue<float>("decoration:blur:contrast");
    Hyprdrive::Config::setValue("decoration:blur:enabled", true);
    Hyprdrive::Config::setValue("decoration:blur:size", LOCKSCREEN_BLUR_SIZE);
    Hyprdrive::Config::setValue("decoration:blur:passes", LOCKSCREEN_BLUR_PASSES);
    Hyprdrive::Config::setValue("decoration:blur:noise", 0.F);
    Hyprdrive::Config::setValue("decoration:blur:contrast", LOCKSCREEN_BLUR_CONTRAST);
    auto restoreBlur = Hyprutils::Utils::CScopeGuard([PREV_ENABLED, PREV_SIZE, PREV_PASSES, PREV_NOISE, PREV_CONTRAST, monitor] {
        Hyprdrive::Config::setValue("decoration:blur:enabled", PREV_ENABLED);
        Hyprdrive::Config::setValue("decoration:blur:size", PREV_SIZE);
        Hyprdrive::Config::setValue("decoration:blur:passes", PREV_PASSES);
        Hyprdrive::Config::setValue("decoration:blur:noise", PREV_NOISE);
        Hyprdrive::Config::setValue("decoration:blur:contrast", PREV_CONTRAST);
        // Y17: this guard is now only constructed on the rebuild path, so
        // m_blurFBDirty is set only when the forced-config blur actually ran --
        // it used to be set on every frame of every monitor, which asked
        // Hyprland to redo its own precomputed blur for nothing.
        if (monitor)
            monitor->m_blurFBDirty = true;
    });

    auto       blurDamage = fullDamage;
    const auto BLURREDTEX = g_pHyprRenderer->blurFramebuffer(frost.backdropFB, 1.F, &blurDamage);
    if (!BLURREDTEX || !BLURREDTEX->m_size.x || !BLURREDTEX->m_size.y)
        return;

    {
        auto bindBackdrop = g_pHyprRenderer->bindTempFB(frost.backdropFB);
        g_pHyprRenderer->draw(CClearPassElement::SClearData{CHyprColor{0.F, 0.F, 0.F, 0.F}}, fullDamage);

        const auto SAVEDTRANSFORM = BLURREDTEX->m_transform;
        BLURREDTEX->m_transform   = Math::wlTransformToHyprutils(Math::invertTransform(monitor->m_transform));
        auto restoreTransform     = Hyprutils::Utils::CScopeGuard([BLURREDTEX, SAVEDTRANSFORM] { BLURREDTEX->m_transform = SAVEDTRANSFORM; });

        g_pHyprRenderer->pushMonitorTransformEnabled(true);
        auto restoreMonitorTransform = Hyprutils::Utils::CScopeGuard([] { g_pHyprRenderer->popMonitorTransformEnabled(); });

        g_pHyprRenderer->draw(
            CTexPassElement::SRenderData{
                .tex    = BLURREDTEX,
                .box    = CBox{0, 0, RENDERSIZE.x, RENDERSIZE.y},
                .damage = fullDamage,
            },
            fullDamage);

        // The overview's dim, baked in (see render()).
        g_pHyprRenderer->draw(CRectPassElement::SRectData{.box = CBox{0, 0, RENDERSIZE.x, RENDERSIZE.y}, .color = CHyprColor{0.F, 0.F, 0.F, BACKDROP_DIM}}, fullDamage);
    }

    frost.backdropDirty = false;
}

bool COverview::renderBackdropBlurCache(PHLMONITOR monitor, float alpha) {
    if (!monitor || alpha <= 0.F || !frost.backdropFB || !frost.backdropFB->isAllocated() || !frost.backdropFB->getTexture())
        return false;

    const auto TEX = frost.backdropFB->getTexture();
    const CRegion fullDamage{CBox{0, 0, monitor->m_transformedSize.x, monitor->m_transformedSize.y}};

    g_pHyprRenderer->draw(
        CTexPassElement::SRenderData{
            .tex          = TEX,
            .box          = CBox{0, 0, monitor->m_transformedSize.x, monitor->m_transformedSize.y},
            .a            = std::clamp(alpha, 0.F, 1.F),
            .damage       = fullDamage,
            .flipEndFrame = true,
        },
        fullDamage);
    return true;
}
