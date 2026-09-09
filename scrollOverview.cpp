#include "OverviewInternal.hpp"
#include "SearchLetters.hpp"

#include "Compat.hpp"
using namespace OvInternal; // opted in here rather than leaked from the header

// The definition behind the global declaration in OverviewInternal.hpp. One real
// symbol in one TU -- TileChrome.cpp binds to it through the PLT, so it cannot be
// inline-in-a-header (emission would then depend on some includer happening to
// ODR-use it) and cannot be namespaced (that renames the symbol; the compositor
// died at the first mid-close tile ease when the split did exactly that).
float houseEase(bool decel, float t) {
    const float T = std::clamp(t, 0.F, 1.F);

    if (Animation::mgr()) {
        if (const auto CURVE = Animation::mgr()->getBezier(decel ? HOUSE_DECEL_BEZIER : HOUSE_ACCEL_BEZIER))
            return CURVE->getYForPoint(T);
    }

    // Not registered yet: ease-out cubic, or its mirror for an exit.
    const float INV = 1.F - T;
    return decel ? 1.F - INV * INV * INV : T * T * T;
}

CScrollOverview::~CScrollOverview() {
    TileChrome::forget();
    // X51: the cache used to be dropped here, which meant every open re-rendered
    // the same strings -- measured 14 cold pango renders per open, forever,
    // because a rice draws the SAME labels every time. The sizes ride the open
    // animation, so an open samples one string at a dozen sizes; keeping them is
    // what lets the second open hit. Bounded by TEXT_CACHE_MAX, and the icon
    // texture cache beside it already lives for the life of the plugin.
    
    grid().dragExcluded = nullptr;
    if (g_pointerGrabOverview == this)
        g_pointerGrabOverview = nullptr;
    transferSharedStateOwnership();
    restoreSubmapIfActive();
    if (const auto OPENGL = g_pHyprRenderer ? g_pHyprRenderer->glBackend() : WP<Render::GL::CHyprOpenGLImpl>{})
        OPENGL->makeEGLCurrent();
    if (realtimePreviewTimer) {
        wl_event_source_remove(realtimePreviewTimer);
        realtimePreviewTimer = nullptr;
    }
    frost.releaseGL();
    OverviewWindow::releaseTexturePrefilterCache();
    const auto MONITOR = pMonitor.lock();
    const auto WORKSPACE = MONITOR ? MONITOR->m_activeWorkspace : PHLWORKSPACE{};
    emitFullscreenVisibilityState(getOverviewFullscreenVisibilityWindow(WORKSPACE, Desktop::focusState()->window()), false);
    restoreWorkspaceAnimationOverrides();
    restoreInputConfigOverrides();
    restoreOverviewKeyboard(getOverviewWindowToShow(closeOnWindow.lock()));
    forced.restoreAll();
    if (const auto MONITOR = pMonitor.lock()) {
        for (auto const& WINDOW : Desktop::windowState()->windows()) {
            if (!WINDOW || WINDOW->m_monitor.lock() != MONITOR)
                continue;
            OverviewWindow::forceDecoRecalc(WINDOW);
        }
    }
    images.clear(); // otherwise we get a vram leak
    trailingWorkspace.reset();
    gapWorkspaces.clear();
    if (scrollOverviews().empty())
        Pointer::Cursor::overrideController->unsetOverride(Pointer::Cursor::CURSOR_OVERRIDE_SPECIAL_ACTION);
    if (const auto MONITOR = pMonitor.lock())
        MONITOR->m_blurFBDirty = true;
}

CScrollOverview::CScrollOverview(PHLWORKSPACE startedOn_, bool swipe_, PHLMONITOR monitor_) : startedOn(startedOn_), swipe(swipe_) {
    const auto          PMONITOR = monitor_ ? monitor_ : (startedOn_ && startedOn_->m_monitor ? startedOn_->m_monitor.lock() : Desktop::focusState()->monitor());
    pMonitor                     = PMONITOR;
    layout                       = ScrollOverview::Config::getLayout();
    sharedStateOwner             = scrollOverviews().empty();
    usesSubmapKeybinds           = hasOverviewSubmap();

    applyWorkspaceAnimationOverrides();
    if (sharedStateOwner)
        forceWorkspaceAlphaVisible();
    ScrollOverview::Dock::releasePointerFocus();
    applyInputConfigOverrides();
    g_pInputManager->unconstrainMouse();
    realtimePreviewTimer = wl_event_loop_add_timer(g_pCompositor->m_wlEventLoop, realtimePreviewTimerCallback, this);
    scheduleMinimumPreviewFrame();

    const auto WINDOWSMOVECONFIG = Config::animationTree()->getAnimationPropertyConfig("windowsMove");
    const auto WINDOWSMOVEVALUES = WINDOWSMOVECONFIG && WINDOWSMOVECONFIG->pValues ? WINDOWSMOVECONFIG->pValues.lock() : WINDOWSMOVECONFIG;
    if (!Animation::mgr()->bezierExists(OVERVIEW_INSERT_FADE_BEZIER))
        Animation::mgr()->addBezierWithName(OVERVIEW_INSERT_FADE_BEZIER, Vector2D{0.5, 0.0}, Vector2D{0.5, 0.0});
    if (!Animation::mgr()->bezierExists(OVERVIEW_REMOVE_FADE_BEZIER))
        Animation::mgr()->addBezierWithName(OVERVIEW_REMOVE_FADE_BEZIER, Vector2D{0.5, 1.0}, Vector2D{0.5, 1.0});
    // Decelerate in: most of the distance is covered early, so the overview is
    // legible well before it stops moving.
    if (!Animation::mgr()->bezierExists(OVERVIEW_OPEN_BEZIER))
        Animation::mgr()->addBezierWithName(OVERVIEW_OPEN_BEZIER, Vector2D{0.16, 1.0}, Vector2D{0.3, 1.0});
    // Accelerate out: it leaves slowly enough to read as motion, then gets out
    // of the way. This is the half the user is waiting on.
    if (!Animation::mgr()->bezierExists(OVERVIEW_CLOSE_BEZIER))
        Animation::mgr()->addBezierWithName(OVERVIEW_CLOSE_BEZIER, Vector2D{0.7, 0.0}, Vector2D{0.84, 0.0});

    workspaceInsertFadeConfig                  = makeShared<Hyprutils::Animation::SAnimationPropertyConfig>();
    workspaceInsertFadeConfig->overridden      = true;
    workspaceInsertFadeConfig->internalBezier  = OVERVIEW_INSERT_FADE_BEZIER;
    workspaceInsertFadeConfig->internalSpeed   = WINDOWSMOVEVALUES ? WINDOWSMOVEVALUES->internalSpeed * 1.2F : 12.F;
    workspaceInsertFadeConfig->internalEnabled = WINDOWSMOVEVALUES ? WINDOWSMOVEVALUES->internalEnabled : 1;
    workspaceInsertFadeConfig->internalStyle   = WINDOWSMOVEVALUES ? WINDOWSMOVEVALUES->internalStyle : "";
    workspaceInsertFadeConfig->pValues         = workspaceInsertFadeConfig;

    workspaceRemoveFadeConfig                  = makeShared<Hyprutils::Animation::SAnimationPropertyConfig>();
    workspaceRemoveFadeConfig->overridden      = true;
    workspaceRemoveFadeConfig->internalBezier  = OVERVIEW_REMOVE_FADE_BEZIER;
    workspaceRemoveFadeConfig->internalSpeed   = WINDOWSMOVEVALUES ? WINDOWSMOVEVALUES->internalSpeed : 10.F;
    workspaceRemoveFadeConfig->internalEnabled = WINDOWSMOVEVALUES ? WINDOWSMOVEVALUES->internalEnabled : 1;
    workspaceRemoveFadeConfig->internalStyle   = WINDOWSMOVEVALUES ? WINDOWSMOVEVALUES->internalStyle : "";
    workspaceRemoveFadeConfig->pValues         = workspaceRemoveFadeConfig;

    openAnimConfig                  = makeShared<Hyprutils::Animation::SAnimationPropertyConfig>();
    openAnimConfig->overridden      = true;
    openAnimConfig->internalBezier  = OVERVIEW_OPEN_BEZIER;
    openAnimConfig->internalSpeed   = ScrollOverview::Config::getOpenSpeed();
    openAnimConfig->internalEnabled = WINDOWSMOVEVALUES ? WINDOWSMOVEVALUES->internalEnabled : 1;
    openAnimConfig->internalStyle   = "";
    openAnimConfig->pValues         = openAnimConfig;

    closeAnimConfig                  = makeShared<Hyprutils::Animation::SAnimationPropertyConfig>();
    closeAnimConfig->overridden      = true;
    closeAnimConfig->internalBezier  = OVERVIEW_CLOSE_BEZIER;
    closeAnimConfig->internalSpeed   = ScrollOverview::Config::getCloseSpeed();
    closeAnimConfig->internalEnabled = WINDOWSMOVEVALUES ? WINDOWSMOVEVALUES->internalEnabled : 1;
    closeAnimConfig->internalStyle   = "";
    closeAnimConfig->pValues         = closeAnimConfig;

    reverseAnimConfig                  = makeShared<Hyprutils::Animation::SAnimationPropertyConfig>();
    reverseAnimConfig->overridden      = true;
    reverseAnimConfig->internalBezier  = OVERVIEW_OPEN_BEZIER; // ease-OUT: maximum slope at t=0
    reverseAnimConfig->internalSpeed   = ScrollOverview::Config::getCloseSpeed();
    reverseAnimConfig->internalEnabled = WINDOWSMOVEVALUES ? WINDOWSMOVEVALUES->internalEnabled : 1;
    reverseAnimConfig->internalStyle   = "";
    reverseAnimConfig->pValues         = reverseAnimConfig;

    Animation::mgr()->createAnimation(1.F, scale, openAnimConfig, AVARDAMAGE_NONE);
    Animation::mgr()->createAnimation({}, viewOffset, openAnimConfig, AVARDAMAGE_NONE);
    Animation::mgr()->createAnimation(1.F, workspaceInsertProgress, WINDOWSMOVECONFIG, AVARDAMAGE_NONE);
    Animation::mgr()->createAnimation(1.F, workspaceInsertFadeProgress, workspaceInsertFadeConfig, AVARDAMAGE_NONE);
    Animation::mgr()->createAnimation(1.F, reflowPacer, WINDOWSMOVECONFIG, AVARDAMAGE_NONE);

    searchAnimConfig                  = makeShared<Hyprutils::Animation::SAnimationPropertyConfig>();
    searchAnimConfig->overridden      = true;
    searchAnimConfig->internalBezier  = OVERVIEW_OPEN_BEZIER;
    searchAnimConfig->internalSpeed   = 1.8F; // ~180ms; windowsMove is ~400ms and feels stuck
    searchAnimConfig->internalEnabled = WINDOWSMOVEVALUES ? WINDOWSMOVEVALUES->internalEnabled : 1;
    searchAnimConfig->internalStyle   = "";
    searchAnimConfig->pValues         = searchAnimConfig;

    Animation::mgr()->createAnimation(0.F, resultsBand, searchAnimConfig, AVARDAMAGE_NONE);
    Animation::mgr()->createAnimation(0.F, resultsWidth, searchAnimConfig, AVARDAMAGE_NONE);
    // X50 rides the same config as the band it travels with: the pill lifting on
    // one curve while the panel it uncovers grows on another reads as two events.
    Animation::mgr()->createAnimation(0.F, searchDock, searchAnimConfig, AVARDAMAGE_NONE);
    syncPluginAnimEnabled();

    scale->setUpdateCallback([this](auto) { damage(); });
    viewOffset->setUpdateCallback([this](auto) { damage(); });
    workspaceInsertProgress->setUpdateCallback([this](auto) { damage(); });
    workspaceInsertFadeProgress->setUpdateCallback([this](auto) { damage(); });
    reflowPacer->setUpdateCallback([this](auto) { damage(); });
    resultsBand->setUpdateCallback([this](auto) {
        g_search.layout.bandH = resultsBand->value();
        damage();
    });
    resultsWidth->setUpdateCallback([this](auto) { damage(); });
    searchDock->setUpdateCallback([this](auto) { damage(); });

    // A previous overview's band must not open the next one already pushed, nor
    // its pill start docked. The published layout is the ONLY group of session
    // state this may touch: the query and the nav zone belong to the dispatcher
    // that asked for the overview, and a second monitor opening must not wipe
    // the query the first one is showing.
    g_search.resetLayout();

    if (!swipe)
        *scale = ScrollOverview::Config::getScale();

    const auto initialFullscreenWindow =
        PMONITOR && PMONITOR->m_activeWorkspace ? getOverviewWindowToShow(Fullscreen::controller()->getFullscreenWindow(PMONITOR->m_activeWorkspace)) : PHLWINDOW{};
    emitFullscreenVisibilityState(initialFullscreenWindow ? initialFullscreenWindow : Desktop::focusState()->window(), true);

    lastMousePosLocal = getOverviewMousePosLocal(pMonitor.lock());

    mouseMoveHook = Event::bus()->m_events.input.mouse.move.listen([this](Vector2D a0, Event::SCallbackInfo& a1) { onMouseMove(a0, a1); });
    touchMoveHook = Event::bus()->m_events.input.touch.motion.listen([this](ITouch::SMotionEvent a0, Event::SCallbackInfo& a1) { onTouchMove(a0, a1); });
    mouseAxisHook = Event::bus()->m_events.input.mouse.axis.listen([this](IPointer::SAxisEvent a0, Event::SCallbackInfo& a1) { onMouseAxis(a0, a1); });

    mouseButtonHook = Event::bus()->m_events.input.mouse.button.listen([this](IPointer::SButtonEvent a0, Event::SCallbackInfo& a1) { onMouseButton(a0, a1); });
    touchDownHook   = Event::bus()->m_events.input.touch.down.listen([this](ITouch::SDownEvent a0, Event::SCallbackInfo& a1) { onCursorSelect(a0, a1); });

    windowOpenHook      = Event::bus()->m_events.window.open.listen([this](PHLWINDOW a0) { onWindowOpen(a0); });
    windowCloseHook     = Event::bus()->m_events.window.close.listen([this](PHLWINDOW a0) { onWindowClose(a0); });
    windowMoveHook      = Event::bus()->m_events.window.moveToWorkspace.listen([this](PHLWINDOW a0, PHLWORKSPACE a1) { onWindowMove(a0, a1); });
    windowActiveHook    = Event::bus()->m_events.window.active.listen([this](PHLWINDOW a0, Desktop::eFocusReason a1) { onWindowActive(a0, a1); });
    windowFullscreenHook = Event::bus()->m_events.window.fullscreen.listen([this](PHLWINDOW a0) { onWindowFullscreen(a0); });
    workspaceCreatedHook = Event::bus()->m_events.workspace.created.listen([this](PHLWORKSPACEREF a0) { onWorkspaceLifecycle(a0); });
    workspaceRemovedHook = Event::bus()->m_events.workspace.removed.listen([this](PHLWORKSPACEREF a0) { onWorkspaceLifecycle(a0); });
    activateSubmapIfConfigured();
    // The submap only owns the KEYBOARD if it actually binds keys. Gating this
    // on the submap merely existing left arrows, Tab and Return with no handler
    // at all in a rice whose submap exists purely for the mouse P0 fix (a click
    // with SUPER still held); measured 2026-08-01, the arrow keys were dead.
    if (!hasOverviewSubmapKeyBinds())
        keyboardKeyHook = Event::bus()->m_events.input.keyboard.key.listen([this](IKeyboard::SKeyEvent a0, Event::SCallbackInfo& a1) { onKeyboardKey(a0, a1); });

    Pointer::Cursor::overrideController->setOverride("left_ptr", Pointer::Cursor::CURSOR_OVERRIDE_SPECIAL_ACTION);

    redrawAll();

    rememberSelection(Desktop::focusState()->window());
    viewportCurrentWorkspace = activeWorkspaceIndex();
    syncSelectionToViewport();
    stealOverviewKeyboard();
}

void CScrollOverview::renderWallpaperLayers(PHLMONITOR monitor, const CBox& workspaceBox, float renderScale, const Time::steady_tp& now, float alpha) {
    if (!monitor)
        return;

    renderOverviewLayerLevel(monitor, ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, workspaceBox, renderScale, now, alpha);
}

void CScrollOverview::renderGlobalWallpaper(PHLMONITOR monitor, const Time::steady_tp& now) {
    if (!monitor)
        return;

    g_pHyprRenderer->renderBackground(monitor);

    for (auto const& ls : monitor->m_layerSurfaceLayers[ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND]) {
        if (!Desktop::View::validMapped(ls.lock()))
            continue;

        g_pHyprRenderer->renderLayer(ls.lock(), monitor, now);
    }
}

// E8-c, GNOME-style: the wallpaper at native size, once per frame, so each card
// can composite it with shader rounding (real clip + AA) instead of a painted
// mask. Re-rendered every frame because wallpapers animate.
// ponytail: no dirty tracking; add a dirty flag if profiling ever cares.
void CScrollOverview::updateCardWallpaperFB(PHLMONITOR monitor, const Time::steady_tp& now) {
    if (!monitor || !g_pHyprRenderer->m_renderData.currentFB)
        return;

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
    }

    frost.cardWallpaperFB->setImageDescription(g_pHyprRenderer->m_renderData.currentFB->imageDescription());

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
}

// Omarchy lockscreen (shell/plugins/lock/LockView.qml MultiEffect):
//   blur: 1.0, blurMax: 128, blurMultiplier: 1.25, contrast: -0.08
// Dual-kawase size * 2^passes ≈ 128px; contrast 0.92 ≈ 1 - 0.08; no grain.
// Independent of decoration:blur:enabled — the lockscreen blurs even when
// Omarchy windows do not.
constexpr int   LOCKSCREEN_BLUR_SIZE     = 8;
constexpr int   LOCKSCREEN_BLUR_PASSES   = 4;
constexpr float LOCKSCREEN_BLUR_CONTRAST = 0.92F;

void CScrollOverview::updateBackdropBlurCache(PHLMONITOR monitor, int wallpaperMode, const Time::steady_tp& now) {
    if (!monitor || wallpaperMode == 1)
        return;

    if (frost.lastBackdropWallpaperMode != wallpaperMode) {
        frost.backdropDirty             = true;
        frost.lastBackdropWallpaperMode = wallpaperMode;
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

    if (g_pHyprRenderer->m_renderData.currentFB)
        frost.backdropFB->setImageDescription(g_pHyprRenderer->m_renderData.currentFB->imageDescription());

    const CRegion fullDamage{CBox{0, 0, RENDERSIZE.x, RENDERSIZE.y}};

    {
        auto bindBackdrop = g_pHyprRenderer->bindTempFB(frost.backdropFB);
        g_pHyprRenderer->draw(CClearPassElement::SClearData{CHyprColor{0.F, 0.F, 0.F, 1.F}}, fullDamage);
        renderGlobalWallpaper(monitor, now);
        OverviewRender::flushPass(monitor);
    }

    const auto PREV_ENABLED  = ScrollOverview::Config::getValue<bool>("decoration:blur:enabled");
    const auto PREV_SIZE     = ScrollOverview::Config::getValue<int>("decoration:blur:size");
    const auto PREV_PASSES   = ScrollOverview::Config::getValue<int>("decoration:blur:passes");
    const auto PREV_NOISE    = ScrollOverview::Config::getValue<float>("decoration:blur:noise");
    const auto PREV_CONTRAST = ScrollOverview::Config::getValue<float>("decoration:blur:contrast");
    ScrollOverview::Config::setValue("decoration:blur:enabled", true);
    ScrollOverview::Config::setValue("decoration:blur:size", LOCKSCREEN_BLUR_SIZE);
    ScrollOverview::Config::setValue("decoration:blur:passes", LOCKSCREEN_BLUR_PASSES);
    ScrollOverview::Config::setValue("decoration:blur:noise", 0.F);
    ScrollOverview::Config::setValue("decoration:blur:contrast", LOCKSCREEN_BLUR_CONTRAST);
    auto restoreBlur = Hyprutils::Utils::CScopeGuard([PREV_ENABLED, PREV_SIZE, PREV_PASSES, PREV_NOISE, PREV_CONTRAST, monitor] {
        ScrollOverview::Config::setValue("decoration:blur:enabled", PREV_ENABLED);
        ScrollOverview::Config::setValue("decoration:blur:size", PREV_SIZE);
        ScrollOverview::Config::setValue("decoration:blur:passes", PREV_PASSES);
        ScrollOverview::Config::setValue("decoration:blur:noise", PREV_NOISE);
        ScrollOverview::Config::setValue("decoration:blur:contrast", PREV_CONTRAST);
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
    }

    frost.backdropDirty = false;
}

void CScrollOverview::renderBackdropBlurCache(PHLMONITOR monitor, float alpha) {
    if (!monitor || alpha <= 0.F || !frost.backdropFB || !frost.backdropFB->isAllocated() || !frost.backdropFB->getTexture())
        return;

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
}

size_t CScrollOverview::activeWorkspaceIndex() const {
    for (size_t i = 0; i < images.size(); ++i) {
        if (images[i]->pWorkspace && images[i]->pWorkspace == startedOn)
            return i;
    }

    return 0;
}

bool CScrollOverview::isSelectedWorkspace(const PHLWORKSPACE& workspace) const {
    return workspace && viewportCurrentWorkspace < images.size() && images[viewportCurrentWorkspace] &&
        images[viewportCurrentWorkspace]->pWorkspace == workspace;
}

float CScrollOverview::workspaceOverviewOffset(size_t workspaceIdx, size_t activeIdx, float workspacePitch) const {
    const auto MONITOR             = pMonitor.lock();
    const auto MONITORSCALE        = MONITOR ? std::max(MONITOR->m_scale, 0.01F) : 1.F;
    const auto MONITORSIZE         = MONITOR ? axisSize(MONITOR->m_size, layout) : 0.F;
    const auto RENDERSCALE         = MONITOR && MONITORSIZE > 0 ?
        std::max(0.01F, (workspacePitch / MONITORSCALE - sc<float>(ScrollOverview::Config::getWorkspaceGap())) / sc<float>(MONITORSIZE)) :
        std::max(scale->value(), 0.01F);
    const auto LOGICALPITCH        = MONITOR ? getWorkspaceLogicalPitch(MONITOR, RENDERSCALE, layout) : workspacePitch / std::max(RENDERSCALE * MONITORSCALE, 0.01F);
    const auto RENDEREDLOGICALUNIT = RENDERSCALE * MONITORSCALE;
    const auto DEFAULTOFFSET       = workspaceOverviewLogicalOffset(workspaceIdx, activeIdx, LOGICALPITCH) * RENDEREDLOGICALUNIT;

    if (!workspaceInsertTransition.active || workspaceIdx >= images.size() || !images[workspaceIdx] || !images[workspaceIdx]->pWorkspace)
        return DEFAULTOFFSET;

    const auto WORKSPACEID = images[workspaceIdx]->pWorkspace->m_id;
    const auto NEWIT       = workspaceInsertTransition.newRelativeOffsets.find(WORKSPACEID);
    if (NEWIT == workspaceInsertTransition.newRelativeOffsets.end())
        return DEFAULTOFFSET;

    const float T         = std::clamp(workspaceInsertProgress->value(), 0.F, 1.F);
    const float NEWOFFSET = NEWIT->second * RENDEREDLOGICALUNIT;

    if (const auto OLDIT = workspaceInsertTransition.oldRelativeOffsets.find(WORKSPACEID); OLDIT != workspaceInsertTransition.oldRelativeOffsets.end()) {
        const float OLDOFFSET = OLDIT->second * RENDEREDLOGICALUNIT;
        return OLDOFFSET + (NEWOFFSET - OLDOFFSET) * T;
    }

    return NEWOFFSET;
}

float CScrollOverview::workspaceOverviewLogicalOffset(size_t workspaceIdx, size_t activeIdx, float workspacePitch) const {
    const auto EXTRAINTERVAL = [this](size_t workspaceIdx_) -> float {
        if (workspaceIdx_ + 1 >= images.size() || !images[workspaceIdx_] || !images[workspaceIdx_ + 1])
            return 0.F;

        if (layout == ScrollOverview::Config::ELayout::HORIZONTAL)
            return images[workspaceIdx_]->overflowRight + images[workspaceIdx_ + 1]->overflowLeft;

        return images[workspaceIdx_]->overflowBottom + images[workspaceIdx_ + 1]->overflowTop;
    };

    float offset = 0.F;

    if (workspaceIdx > activeIdx) {
        for (size_t i = activeIdx; i < workspaceIdx; ++i)
            offset += workspacePitch + EXTRAINTERVAL(i);
    } else {
        for (size_t i = workspaceIdx; i < activeIdx; ++i)
            offset -= workspacePitch + EXTRAINTERVAL(i);
    }

    return offset;
}

float CScrollOverview::workspaceOverviewAlpha(size_t workspaceIdx) const {
    if (!workspaceInsertTransition.active || workspaceIdx >= images.size() || !images[workspaceIdx] || !images[workspaceIdx]->pWorkspace)
        return 1.F;

    if (images[workspaceIdx]->pWorkspace->m_id != workspaceInsertTransition.transitionWorkspaceID)
        return 1.F;

    if (!workspaceInsertTransition.transitionFadeIn)
        return 1.F;

    if (workspaceInsertTransition.oldRelativeOffsets.contains(workspaceInsertTransition.transitionWorkspaceID))
        return 1.F;

    return std::clamp(workspaceInsertFadeProgress->value(), 0.F, 1.F);
}

// Highest workspace on this monitor that still has windows. Empty desktops
// after that are the ponytail, not occupied space -- using max-id here made
// the trailing empty pin every id below it, so emptying the 2nd-last desktop
// never shrank the strip.
static WORKSPACEID highestOccupiedWorkspaceID(PHLMONITOR MONITOR) {
    WORKSPACEID maxOccupied = 0;
    if (!MONITOR)
        return maxOccupied;

    for (const auto& w : State::workspaceState()->workspaces()) {
        const auto WORKSPACE = w.lock();
        if (!valid(WORKSPACE) || WORKSPACE->m_monitor != MONITOR || WORKSPACE->m_isSpecialWorkspace)
            continue;
        if (WORKSPACE->getWindowCount() > 0)
            maxOccupied = std::max(maxOccupied, WORKSPACE->m_id);
    }

    return maxOccupied;
}

// ponytail: E2 keeps the GNOME invariant "the highest occupied workspace is
// followed by one empty" by creating the real workspace, not a phantom strip
// card. The strong ref below is the only thing keeping it alive -- Hyprland
// reaps empty workspaces the moment nothing owns them -- so closing the
// overview cleans it up for free. Extra empties above occupied+1 are dropped
// so moving the last window off the 2nd-last desktop shrinks the strip.
void CScrollOverview::ensureTrailingWorkspace() {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR || closing)
        return;

    const WORKSPACEID maxOccupied = highestOccupiedWorkspaceID(MONITOR);
    if (maxOccupied <= 0)
        return;

    const WORKSPACEID trailingID = maxOccupied + 1;

    PHLWORKSPACE last;
    WORKSPACEID  maxID = 0;
    for (const auto& w : State::workspaceState()->workspaces()) {
        const auto WORKSPACE = w.lock();
        if (!valid(WORKSPACE) || WORKSPACE->m_monitor != MONITOR || WORKSPACE->m_isSpecialWorkspace)
            continue;

        if (!last || WORKSPACE->m_id > maxID) {
            maxID = WORKSPACE->m_id;
            last  = WORKSPACE;
        }
    }

    if (trailingWorkspace && trailingWorkspace->m_id != trailingID)
        trailingWorkspace.reset();

    if (last && last->m_id == trailingID && last->getWindowCount() == 0) {
        if (trailingWorkspace != last)
            trailingWorkspace.reset();
        return;
    }

    trailingWorkspace = State::workspaceState()->create(trailingID, MONITOR->m_id, std::to_string(trailingID));
}

// ponytail: X3 population parity. The reference grid is indexed by workspace
// NUMBER, not by the workspaces that happen to exist -- it lays out cells 1..N
// and only hides those past the dynamic count (OverviewWidget.qml:33-36, :161,
// :167), so jumping straight to desktop 5 shows five desktops, not two.
// Materialising the missing ids as real workspaces buys that with no new
// placeholder-card path through the renderer: the strong refs below are the only
// thing keeping them alive, exactly as E2's trailing workspace works, so closing
// the overview reaps them for free.
//
// v1.1 item 0. That purchase had NO CEILING, and the F3B report caught the bill:
// a long session wedged the compositor with its main thread at 86% inside this
// function. The highest live id only ever RATCHETS UP -- an output cycle leaves
// the session parked one desktop higher (measured: five headless_on/off cycles
// took maxID 1 -> 4), a window parked on a high desktop pins it there, and the
// user can jump to any id at all -- and this fill then re-creates every missing
// id below it on EVERY open and hands the renderer a card and a blurred strip
// cell for each one.
//
// Measured on b78cd50, one window parked on desktop 300, session on desktop 1
// (zrig/v6/item0): the open itself stays cheap (7 ms) -- the cost is what the
// open LEAVES BEHIND. Compositor main-thread CPU while the overview is up:
// 0.03 s/s at maxID 2, 0.06 s/s at 50, 0.68 s/s at 300, with the hyprctl round
// trip going 6 ms -> 28 ms. (It falls again past ~500 only because the cells
// have shrunk below a pixel and the renderer drops them -- the peak is where a
// cell is small enough to be free of nothing and large enough to be drawn.)
//
// So: BOUND it. Parity is a claim about the first few desktops -- GNOME's own
// dynamic count is "highest used + 1" and nobody's is thirty-two -- and a strip
// with more cells than this is a smear, not a row of desktops. Past the cap the
// strip shows what actually EXISTS, sparsely: a real desktop 300 still gets its
// own card, so nothing the user made disappears; only the phantoms stop.
constexpr WORKSPACEID GAP_POPULATION_MAX = 32;

void CScrollOverview::ensureNoWorkspaceGaps() {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR || closing) {
        gapWorkspaces.clear();
        return;
    }

    WORKSPACEID                    maxID = 0;
    std::unordered_set<WORKSPACEID> existing;
    for (const auto& w : State::workspaceState()->workspaces()) {
        const auto WORKSPACE = w.lock();
        if (!valid(WORKSPACE) || WORKSPACE->m_isSpecialWorkspace)
            continue;

        // Ids on ANOTHER monitor are not gaps here, and re-creating one would
        // move it, so they count as occupied without becoming ours.
        existing.insert(WORKSPACE->m_id);
        if (WORKSPACE->m_monitor == MONITOR)
            maxID = std::max(maxID, WORKSPACE->m_id);
    }

    std::vector<PHLWORKSPACE> kept;
    // Fill holes only below the last occupied desktop. Filling up to the
    // trailing empty (old maxID) re-created the emptied 2nd-last as a gap
    // and the strip never shrank.
    const WORKSPACEID         maxOccupied = highestOccupiedWorkspaceID(MONITOR);
    const WORKSPACEID         FILLTO      = std::min(maxOccupied > 0 ? maxOccupied : maxID, GAP_POPULATION_MAX + 1);
    for (WORKSPACEID id = 1; id < FILLTO; ++id) {
        if (existing.contains(id)) {
            // Keep paying for the ones we already materialised; anything else is
            // the user's and must not be adopted.
            if (const auto it = std::find_if(gapWorkspaces.begin(), gapWorkspaces.end(), [id](const auto& w) { return w && w->m_id == id; }); it != gapWorkspaces.end())
                kept.emplace_back(*it);
            continue;
        }

        if (const auto CREATED = State::workspaceState()->create(id, MONITOR->m_id, std::to_string(id)); CREATED)
            kept.emplace_back(CREATED);
    }

    gapWorkspaces = std::move(kept);
}

void CScrollOverview::rebuildWorkspaceImages() {
    ensureTrailingWorkspace();
    ensureNoWorkspaceGaps();
    // Workspaces created after we opened (the trailing one, or any made while the
    // overview is up) were never shown, so their alpha is still 0 and
    // renderWindow would multiply their windows away.
    if (sharedStateOwner)
        forceWorkspaceAlphaVisible();
    const auto selectedWorkspace = closeOnWindow ? closeOnWindow->m_workspace : startedOn;
    const auto selectedWindow    = closeOnWindow;
    const auto viewportWorkspace = viewportCurrentWorkspace < images.size() ? images[viewportCurrentWorkspace]->pWorkspace : startedOn;
    const auto REMOVEDWORKSPACE  = pendingRemovedWorkspace.lock();

    images.clear();

    for (const auto& w : State::workspaceState()->workspaces()) {
        const auto WORKSPACE = w.lock();
        if (!valid(WORKSPACE) || WORKSPACE->m_monitor != pMonitor || WORKSPACE->m_isSpecialWorkspace)
            continue;

        if (WORKSPACE == REMOVEDWORKSPACE)
            continue;

        images.emplace_back(makeShared<SWorkspaceImage>(WORKSPACE));
    }

    std::sort(images.begin(), images.end(), [](const auto& a, const auto& b) { return a->pWorkspace->m_id < b->pWorkspace->m_id; });

    if (images.empty()) {
        viewportCurrentWorkspace = 0;
        closeOnWindow.reset();
        return;
    }

    viewportCurrentWorkspace = 0;
    for (size_t i = 0; i < images.size(); ++i) {
        if (images[i]->pWorkspace == viewportWorkspace) {
            viewportCurrentWorkspace = i;
            break;
        }
    }
    if (images[viewportCurrentWorkspace]->pWorkspace != viewportWorkspace) {
        for (size_t i = 0; i < images.size(); ++i) {
            if (images[i]->pWorkspace == selectedWorkspace) {
                viewportCurrentWorkspace = i;
                break;
            }
        }
    }

    closeOnWindow = selectedWindow;
}

void CScrollOverview::seedRememberedSelections() {
    for (const auto& img : images) {
        if (!img->pWorkspace)
            continue;

        const auto WORKSPACEID = img->pWorkspace->m_id;

        if (const auto it = rememberedSelection.find(WORKSPACEID); it != rememberedSelection.end()) {
            const auto rememberedWindow = getOverviewWindowToShow(it->second.lock());
            if (rememberedWindow && rememberedWindow->m_workspace == img->pWorkspace && shouldShowOverviewWindow(rememberedWindow))
                continue;
        }

        const auto lastFocusedWindow = getOverviewWindowToShow(img->pWorkspace->getLastFocusedWindow());
        if (!lastFocusedWindow || lastFocusedWindow->m_workspace != img->pWorkspace || !shouldShowOverviewWindow(lastFocusedWindow))
            continue;

        rememberedSelection[WORKSPACEID] = lastFocusedWindow;
    }
}

void CScrollOverview::rememberSelection(PHLWINDOW window) {
    window = getOverviewWindowToShow(window);

    if (!window || !window->m_workspace)
        return;

    rememberedSelection[window->m_workspace->m_id] = window;
}

void CScrollOverview::updateWorkspaceOverflow() {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR)
        return;

    for (const auto& img : images) {
        if (!img)
            continue;

        img->overflowLeft   = 0.F;
        img->overflowRight  = 0.F;
        img->overflowTop    = 0.F;
        img->overflowBottom = 0.F;
    }

    for (const auto& img : images) {
        if (!img || !img->pWorkspace)
            continue;

        for (const auto& windowRef : img->windows) {
            const auto window = getOverviewWindowToShow(windowRef.lock());
            if (!shouldShowOverviewWindow(window) || window->m_isFloating)
                continue;

            const auto POS  = window->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT) - MONITOR->m_position;
            const auto SIZE = window->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
            // Windows hidden under a fullscreen window can get sentinel geometry like -2x-2.
            if (SIZE.x <= 0 || SIZE.y <= 0)
                continue;

            img->overflowLeft   = std::max(img->overflowLeft, std::max(0.F, sc<float>(-POS.x)));
            img->overflowRight  = std::max(img->overflowRight, std::max(0.F, sc<float>(POS.x + SIZE.x - MONITOR->m_size.x)));
            img->overflowTop    = std::max(img->overflowTop, std::max(0.F, sc<float>(-POS.y)));
            img->overflowBottom = std::max(img->overflowBottom, std::max(0.F, sc<float>(POS.y + SIZE.y - MONITOR->m_size.y)));
        }
    }
}

CBox CScrollOverview::workspaceOverviewVisibleBox(size_t workspaceIdx, const CBox& workspaceBox, float renderScale, PHLMONITOR monitor) const {
    if (workspaceIdx >= images.size() || !images[workspaceIdx] || !monitor)
        return workspaceBox;

    auto box = workspaceBox;
    const auto LEFT   = images[workspaceIdx]->overflowLeft * renderScale * monitor->m_scale;
    const auto RIGHT  = images[workspaceIdx]->overflowRight * renderScale * monitor->m_scale;
    const auto TOP    = images[workspaceIdx]->overflowTop * renderScale * monitor->m_scale;
    const auto BOTTOM = images[workspaceIdx]->overflowBottom * renderScale * monitor->m_scale;

    box.x -= LEFT;
    box.y -= TOP;
    box.width += LEFT + RIGHT;
    box.height += TOP + BOTTOM;

    return box;
}

PHLWINDOW CScrollOverview::windowAtOverviewPoint(const Vector2D& point, size_t* hoveredWorkspaceIdx) const {
    size_t activeIdx = activeWorkspaceIndex();
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR)
        return nullptr;

    const auto WORKSPACEPITCH = getWorkspaceRenderedPitch(MONITOR, scale->value(), layout);
    for (size_t workspaceIdx = 0; workspaceIdx < images.size(); ++workspaceIdx) {
        const auto& wimg = images[workspaceIdx];
        const auto  offset = workspaceOverviewOffset(workspaceIdx, activeIdx, WORKSPACEPITCH);

        const auto selectWindow = [&](const PHLWINDOW& window) -> PHLWINDOW {
            if (hoveredWorkspaceIdx)
                *hoveredWorkspaceIdx = workspaceIdx;

            return window;
        };

        const auto fullscreenWindow = wimg->pWorkspace ? getOverviewWindowToShow(Fullscreen::controller()->getFullscreenWindow(wimg->pWorkspace)) : PHLWINDOW{};

        if (!isWorkspaceScrolling(wimg->pWorkspace) && shouldShowOverviewWindow(fullscreenWindow)) {
            for (auto it = wimg->windows.rbegin(); it != wimg->windows.rend(); ++it) {
                const auto window = getOverviewWindowToShow(it->lock());
                if (!overviewTileShown(window, MONITOR) || !window->m_isFloating)
                    continue;

                const auto texbox = getOverviewWindowBox(window, MONITOR, scale->value(), viewOffset->value(), offset, layout);

                if (texbox.containsPoint(point))
                    return selectWindow(window);
            }

            const auto texbox = getOverviewWindowBox(fullscreenWindow, MONITOR, scale->value(), viewOffset->value(), offset, layout);

            if (texbox.containsPoint(point))
                return selectWindow(fullscreenWindow);

            continue;
        }

        for (const bool floating : {true, false}) {
            for (auto it = wimg->windows.rbegin(); it != wimg->windows.rend(); ++it) {
                const auto window = getOverviewWindowToShow(it->lock());
                if (!overviewTileShown(window, MONITOR) || window->m_isFloating != floating)
                    continue;

                const auto texbox = getOverviewWindowBox(window, MONITOR, scale->value(), viewOffset->value(), offset, layout);

                if (texbox.containsPoint(point))
                    return selectWindow(window);
            }
        }
    }

    return nullptr;
}

PHLWINDOW CScrollOverview::windowAtOverviewCursor(size_t* hoveredWorkspaceIdx) {
    return windowAtOverviewPoint(lastMousePosLocal, hoveredWorkspaceIdx);
}

// The tile as DRAWN -- grown by the hover ease, shrunk by the press ease -- so
// the close button the user aims at and the circle that gets hit-tested are the
// same circle.
CBox CScrollOverview::tileBox(const PHLWINDOW& window) const {
    const auto MONITOR = pMonitor.lock();
    if (!window || !MONITOR)
        return {};

    size_t workspaceIdx = images.size();
    for (size_t i = 0; i < images.size(); ++i) {
        if (images[i] && images[i]->pWorkspace == window->m_workspace) {
            workspaceIdx = i;
            break;
        }
    }

    if (workspaceIdx >= images.size())
        return {};

    const auto OFFSET = workspaceOverviewOffset(workspaceIdx, activeWorkspaceIndex(), getWorkspaceRenderedPitch(MONITOR, scale->value(), layout));
    return TileChrome::scaledBox(window, getOverviewWindowBox(window, MONITOR, scale->value(), viewOffset->value(), OFFSET, layout));
}

CBox CScrollOverview::overviewBoxForGlobal(WORKSPACEID workspaceId, const CBox& globalBox) const {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR)
        return {};

    size_t workspaceIdx = images.size();
    for (size_t i = 0; i < images.size(); ++i) {
        if (images[i] && images[i]->pWorkspace && images[i]->pWorkspace->m_id == workspaceId) {
            workspaceIdx = i;
            break;
        }
    }

    if (workspaceIdx >= images.size())
        return {};

    const auto OFFSET = workspaceOverviewOffset(workspaceIdx, activeWorkspaceIndex(), getWorkspaceRenderedPitch(MONITOR, scale->value(), layout));
    return getOverviewGlobalBox(globalBox, MONITOR, scale->value(), viewOffset->value(), OFFSET, layout);
}

bool CScrollOverview::selectAndClose(const PHLWINDOW& window) {
    if (!window)
        return false;

    // The window's own workspace, not the one in view: X28's menu can list a
    // window on any desktop, and committing the selection without moving the
    // viewport to it would close the overview onto the wrong one.
    size_t workspaceIdx = images.size();
    for (size_t i = 0; i < images.size(); ++i) {
        if (images[i] && images[i]->pWorkspace == window->m_workspace) {
            workspaceIdx = i;
            break;
        }
    }

    if (workspaceIdx >= images.size()) {
        closeOnWindow = window;
        for (const auto& overview : scrollOverviews()) {
            if (overview && overview.get() != this)
                overview->close();
        }
        close();
        return true;
    }

    if (!selectOverviewWindow(window, workspaceIdx, true))
        return false;

    for (const auto& overview : scrollOverviews()) {
        if (overview && overview.get() != this)
            overview->close();
    }
    close();
    return true;
}

CBox CScrollOverview::tileCloseButtonBox(const PHLWINDOW& window) const {
    const auto MONITOR = pMonitor.lock();
    // The HIT box, not the drawn one: this is used for the hover keep-alive and
    // for clicks, and the drawn box empties itself as the button fades.
    return MONITOR ? TileChrome::closeButtonHitBox(window, tileBox(window), MONITOR->m_scale) : CBox{};
}

// X7. Hover is hit-tested against the UNSCALED slot. Testing the grown box would
// let a tile that grew under the cursor hold its own hover, and the shrink on the
// way out would re-enter it -- a tile that flutters rather than one that bounces.
// The close button is the single exception: it is drawn on the grown box, so it
// pokes a few px past the slot, and without this the hover (and the button with
// it) would vanish exactly as the cursor arrived on it.
void CScrollOverview::updateTileHover() {
    // The pointer moved, so the pointer owns the ring again. Whatever the
    // arrows had selected keeps its role for Return and for close(); it just
    // stops being the thing drawn.
    g_search.nav.keyboardTile = false;

    if (closing || drag.activeWindow || resize.activeWindow || pan.pointerDown) {
        TileChrome::setHovered(PHLWINDOW{});
        return;
    }

    // The linger is for reaching the close button through empty space. The
    // dock is a different surface: keep the hold and the title sits on the
    // pill, so drop it the moment the pointer is on the dock.
    if (ScrollOverview::Dock::uiContains(pMonitor.lock(), lastMousePosLocal)) {
        TileChrome::setHovered(PHLWINDOW{}, false);
        return;
    }

    // X62. A result under the cursor takes the selection, so pointing at a row
    // and arrowing onto it land on the SAME highlight -- there is only one
    // selection and both devices move it. It also swallows the tile hover: the
    // panel is drawn over the grid while the band eases, and a tile lighting up
    // through it would be hovering something the user cannot click.
    if (const auto* ROW = searchRowAt(lastMousePosLocal); ROW) {
        TileChrome::setHovered(PHLWINDOW{});
        g_search.nav.zone = EOverviewNavZone::Result;
        if (g_search.nav.selection != ROW->index) {
            g_search.nav.selection = ROW->index;
            damage();
        }
        return;
    }

    // The button is drawn on the GROWN box so it pokes a few px past the slot,
    // and during the post-hover linger it belongs to a tile the hover test no
    // longer reports at all. Both cases RE-ARM rather than just bail: bailing
    // left the linger counting down under a cursor that had arrived on the
    // button, so the tile could shrink away mid-aim.
    if (const auto HOVERED = TileChrome::hovered(); HOVERED && tileCloseButtonBox(HOVERED).containsPoint(lastMousePosLocal)) {
        TileChrome::setHovered(HOVERED);
        return;
    }

    TileChrome::setHovered(windowAtOverviewPoint(lastMousePosLocal));
}

PHLWINDOW CScrollOverview::windowClosestToWorkspaceCenter(size_t workspaceIdx) const {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR || workspaceIdx >= images.size() || !images[workspaceIdx] || !images[workspaceIdx]->pWorkspace)
        return {};

    const auto& WORKSPACEIMAGE  = images[workspaceIdx];
    const auto  SCALE           = scale->value();
    const auto  WORKSPACEOFFSET =
        workspaceOverviewOffset(workspaceIdx, activeWorkspaceIndex(), getWorkspaceRenderedPitch(MONITOR, SCALE, layout));
    const auto WORKSPACEBOX = getOverviewWorkspaceBox(MONITOR, SCALE, viewOffset->value(), WORKSPACEOFFSET, layout);
    const auto FULLSCREENWINDOW = getOverviewWindowToShow(Fullscreen::controller()->getFullscreenWindow(WORKSPACEIMAGE->pWorkspace));
    const bool HASFULLSCREENPATH = !isWorkspaceScrolling(WORKSPACEIMAGE->pWorkspace) && shouldShowOverviewWindow(FULLSCREENWINDOW) &&
        FULLSCREENWINDOW->m_workspace == WORKSPACEIMAGE->pWorkspace;

    PHLWINDOW bestWindow;
    double    bestDistance = std::numeric_limits<double>::max();

    for (const auto& windowRef : WORKSPACEIMAGE->windows) {
        const auto WINDOW = getOverviewWindowToShow(windowRef.lock());
        if (!shouldShowOverviewWindow(WINDOW))
            continue;
        if (HASFULLSCREENPATH && WINDOW != FULLSCREENWINDOW && !WINDOW->m_isFloating)
            continue;

        const auto WINDOWBOX = getOverviewWindowBox(WINDOW, MONITOR, SCALE, viewOffset->value(), WORKSPACEOFFSET, layout);
        const auto DISTANCE  = overviewBoxCenterDistanceSquared(WINDOWBOX, WORKSPACEBOX);
        if (DISTANCE >= bestDistance)
            continue;

        bestWindow   = WINDOW;
        bestDistance = DISTANCE;
    }

    return bestWindow;
}

CDropIndicator::SDropAnchor CScrollOverview::dropAnchorAtOverviewCursorOnWorkspace(size_t workspaceIdx, const PHLWINDOW& ignoredWindow,
                                                                                   CScrollOverview* dragContext) {
    CDropIndicator::SDropAnchor result;
    const auto                  DRAGCONTEXT = dragContext ? dragContext : this;

    const auto MONITOR = pMonitor.lock();
    if (!MONITOR || workspaceIdx >= images.size() || !images[workspaceIdx])
        return result;

    const auto& IMAGE     = images[workspaceIdx];
    const auto  WORKSPACE = IMAGE->pWorkspace;
    if (!WORKSPACE)
        return result;

    const auto WORKSPACE_OFFSET =
        workspaceOverviewOffset(workspaceIdx, activeWorkspaceIndex(), getWorkspaceRenderedPitch(MONITOR, scale->value(), layout));
    const auto WORKSPACEBOX = getOverviewWorkspaceBox(MONITOR, scale->value(), viewOffset->value(), WORKSPACE_OFFSET, layout);

    if (!overviewBoxFullyVisibleOnMonitor(WORKSPACEBOX, MONITOR))
        return result;

    if (!DRAGCONTEXT->drag.startedTiled)
        return result;

    const auto boxesForWindow = [&](const PHLWINDOW& window) {
        const auto TARGET = window->layoutTarget();
        return std::pair{
            getOverviewWindowBox(window, MONITOR, scale->value(), viewOffset->value(), WORKSPACE_OFFSET, layout, false),
            TARGET ? getOverviewGlobalBox(TARGET->position(), MONITOR, scale->value(), viewOffset->value(), WORKSPACE_OFFSET, layout, false) : CBox{},
        };
    };
    const auto setAnchor = [](CDropIndicator::SDropAnchor& anchor, const PHLWINDOW& window, const CBox& box, const CBox& logicalBox = {}, const std::string& direction = {}) {
        anchor.window     = window;
        anchor.box        = box;
        anchor.logicalBox = logicalBox;
        anchor.direction  = direction;
    };

    const auto directionForBox = [&](const CBox& box) {
        const auto LOCAL_X = lastMousePosLocal.x - box.x;
        const auto LOCAL_Y = lastMousePosLocal.y - box.y;

        if (LOCAL_X < box.width / 3.F)
            return std::string{"l"};
        if (LOCAL_X > box.width * 2.F / 3.F)
            return std::string{"r"};
        return LOCAL_Y < box.height / 2.F ? std::string{"u"} : std::string{"d"};
    };

    DRAGCONTEXT->refreshDragOriginalOverviewBoxes();

    const auto ORIGINALHITBOX =
        DRAGCONTEXT->drag.originalOverviewHitbox.empty() ? DRAGCONTEXT->drag.originalOverviewBox : DRAGCONTEXT->drag.originalOverviewHitbox;
    if (ignoredWindow && !DRAGCONTEXT->drag.originalOverviewBox.empty() && ORIGINALHITBOX.containsPoint(lastMousePosLocal) &&
        WORKSPACE == DRAGCONTEXT->drag.originalWorkspace.lock()) {
        setAnchor(result, ignoredWindow, DRAGCONTEXT->drag.originalOverviewBox);
        return result;
    }

    const auto ALGO = overviewScrollingAlgorithmForWorkspace(WORKSPACE);
    const bool PRIMARYHORIZONTAL = ALGO && ALGO->m_scrollingData && ALGO->m_scrollingData->controller && ALGO->m_scrollingData->controller->isPrimaryHorizontal();
    const auto pointsToOriginalStackSlot = [&](const PHLWINDOW& anchor, const std::string& direction) {
        if (!ALGO || !ALGO->m_scrollingData || !ALGO->m_scrollingData->controller || !ignoredWindow || DRAGCONTEXT->drag.originalOverviewBox.empty() ||
            WORKSPACE != DRAGCONTEXT->drag.originalWorkspace.lock() || !anchor || !anchor->layoutTarget() || !ignoredWindow->layoutTarget())
            return false;

        const auto ANCHORDATA  = ALGO->dataFor(anchor->layoutTarget());
        const auto ORIGINALDATA = ALGO->dataFor(ignoredWindow->layoutTarget());
        const auto ANCHORCOL   = ANCHORDATA ? ANCHORDATA->column.lock() : nullptr;
        const auto ORIGINALCOL = ORIGINALDATA ? ORIGINALDATA->column.lock() : nullptr;
        if (!ANCHORCOL || ANCHORCOL != ORIGINALCOL)
            return false;

        const auto ANCHORIDX   = ANCHORCOL->idx(anchor->layoutTarget());
        const auto ORIGINALIDX = ORIGINALCOL->idx(ignoredWindow->layoutTarget());
        if (ANCHORIDX == ORIGINALIDX)
            return false;

        const bool ANCHORPREVIOUS = ANCHORIDX < ORIGINALIDX;
        if ((ANCHORPREVIOUS && ANCHORIDX + 1 != ORIGINALIDX) || (!ANCHORPREVIOUS && ORIGINALIDX + 1 != ANCHORIDX))
            return false;

        if (PRIMARYHORIZONTAL)
            return (ANCHORPREVIOUS && direction == "d") || (!ANCHORPREVIOUS && direction == "u");

        return (ANCHORPREVIOUS && direction == "r") || (!ANCHORPREVIOUS && direction == "l");
    };
    const auto pointsToOriginalColumnSlot = [&](const PHLWINDOW& anchor, const std::string& direction) {
        if (!ALGO || !ALGO->m_scrollingData || !ALGO->m_scrollingData->controller || !ignoredWindow || DRAGCONTEXT->drag.originalOverviewBox.empty() ||
            WORKSPACE != DRAGCONTEXT->drag.originalWorkspace.lock() || !anchor || !anchor->layoutTarget() || !ignoredWindow->layoutTarget())
            return false;

        const auto ANCHORDATA   = ALGO->dataFor(anchor->layoutTarget());
        const auto ORIGINALDATA = ALGO->dataFor(ignoredWindow->layoutTarget());
        const auto ANCHORCOL    = ANCHORDATA ? ANCHORDATA->column.lock() : nullptr;
        const auto ORIGINALCOL  = ORIGINALDATA ? ORIGINALDATA->column.lock() : nullptr;
        if (!ANCHORCOL || !ORIGINALCOL || ORIGINALCOL->targetDatas.size() != 1)
            return false;

        const auto ANCHORCOLIDX   = ALGO->m_scrollingData->idx(ANCHORCOL);
        const auto ORIGINALCOLIDX = ALGO->m_scrollingData->idx(ORIGINALCOL);
        if (ANCHORCOLIDX < 0 || ORIGINALCOLIDX < 0 || ANCHORCOLIDX == ORIGINALCOLIDX)
            return false;

        const bool ANCHORPREVIOUS = ANCHORCOLIDX < ORIGINALCOLIDX;
        if ((ANCHORPREVIOUS && ANCHORCOLIDX + 1 != ORIGINALCOLIDX) || (!ANCHORPREVIOUS && ORIGINALCOLIDX + 1 != ANCHORCOLIDX))
            return false;

        if (PRIMARYHORIZONTAL)
            return (ANCHORPREVIOUS && direction == "r") || (!ANCHORPREVIOUS && direction == "l");

        return (ANCHORPREVIOUS && direction == "d") || (!ANCHORPREVIOUS && direction == "u");
    };
    const auto normalizeOriginalSlotAnchor = [&]() {
        if (pointsToOriginalStackSlot(result.window, result.direction) || pointsToOriginalColumnSlot(result.window, result.direction))
            setAnchor(result, ignoredWindow, DRAGCONTEXT->drag.originalOverviewBox);
    };

    float bestDistanceSq = std::numeric_limits<float>::max();
    for (auto it = IMAGE->windows.rbegin(); it != IMAGE->windows.rend(); ++it) {
        const auto WINDOW = getOverviewWindowToShow(it->lock());
        if (!shouldShowOverviewWindow(WINDOW) || WINDOW == ignoredWindow || WINDOW->m_isFloating)
            continue;

        const auto [BOX, LOGICALBOX] = boxesForWindow(WINDOW);
        const auto HITBOX            = LOGICALBOX.empty() ? BOX : LOGICALBOX;

        if (!HITBOX.containsPoint(lastMousePosLocal))
            continue;

        const auto distanceSq = overviewPointDistanceSqToBox(lastMousePosLocal, BOX);
        if (distanceSq >= bestDistanceSq)
            continue;

        setAnchor(result, WINDOW, BOX, LOGICALBOX, directionForBox(BOX));
        bestDistanceSq = distanceSq;
    }

    if (result.window) {
        normalizeOriginalSlotAnchor();
        return result;
    }

    if (!ALGO || !ALGO->m_scrollingData || !ALGO->m_scrollingData->controller)
        return result;

    const auto POINTER   = PRIMARYHORIZONTAL ? sc<float>(lastMousePosLocal.x) : sc<float>(lastMousePosLocal.y);
    float      firstEdge = std::numeric_limits<float>::max();
    float      lastEdge  = std::numeric_limits<float>::lowest();

    for (const auto& windowRef : IMAGE->windows) {
        const auto WINDOW = getOverviewWindowToShow(windowRef.lock());
        if (!shouldShowOverviewWindow(WINDOW) || WINDOW == ignoredWindow || WINDOW->m_isFloating)
            continue;

        const auto [BOX, LOGICALBOX] = boxesForWindow(WINDOW);
        const auto HITBOX            = LOGICALBOX.empty() ? BOX : LOGICALBOX;
        const auto MIN               = PRIMARYHORIZONTAL ? sc<float>(HITBOX.x) : sc<float>(HITBOX.y);
        const auto MAX               = PRIMARYHORIZONTAL ? sc<float>(HITBOX.x + HITBOX.width) : sc<float>(HITBOX.y + HITBOX.height);

        firstEdge = std::min(firstEdge, MIN);
        lastEdge  = std::max(lastEdge, MAX);
    }

    if (POINTER >= firstEdge && POINTER <= lastEdge)
        return result;

    const bool FIND_FIRST = POINTER < firstEdge;
    float      bestEdge   = FIND_FIRST ? std::numeric_limits<float>::max() : std::numeric_limits<float>::lowest();

    for (const auto& windowRef : IMAGE->windows) {
        const auto WINDOW = getOverviewWindowToShow(windowRef.lock());
        if (!shouldShowOverviewWindow(WINDOW) || WINDOW == ignoredWindow || WINDOW->m_isFloating)
            continue;

        const auto [BOX, LOGICALBOX] = boxesForWindow(WINDOW);
        const auto HITBOX            = LOGICALBOX.empty() ? BOX : LOGICALBOX;
        const auto EDGE              = PRIMARYHORIZONTAL ? sc<float>(FIND_FIRST ? HITBOX.x : HITBOX.x + HITBOX.width) :
                                                           sc<float>(FIND_FIRST ? HITBOX.y : HITBOX.y + HITBOX.height);

        if (FIND_FIRST ? EDGE < bestEdge : EDGE > bestEdge) {
            bestEdge = EDGE;
            setAnchor(result, WINDOW, BOX, LOGICALBOX);
        }
    }

    if (result.window) {
        result.direction = PRIMARYHORIZONTAL ? (FIND_FIRST ? "l" : "r") : (FIND_FIRST ? "u" : "d");
        normalizeOriginalSlotAnchor();
    }

    return result;
}

PHLWORKSPACE CScrollOverview::workspaceAtOverviewPoint(const Vector2D& point, size_t* hoveredWorkspaceIdx) const {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR)
        return nullptr;

    for (size_t workspaceIdx = 0; workspaceIdx < images.size(); ++workspaceIdx) {
        const auto& wimg = images[workspaceIdx];
        if (!wimg || !wimg->pWorkspace)
            continue;

        const auto WORKSPACEBOX = getOverviewWorkspaceUsableBox(wimg->pWorkspace, MONITOR, scale->value(), viewOffset->value(),
                                                                workspaceOverviewOffset(workspaceIdx, activeWorkspaceIndex(), getWorkspaceRenderedPitch(MONITOR, scale->value(), layout)),
                                                                layout);

        if (WORKSPACEBOX.containsPoint(point)) {
            if (hoveredWorkspaceIdx)
                *hoveredWorkspaceIdx = workspaceIdx;

            return wimg->pWorkspace;
        }
    }

    return nullptr;
}

PHLWORKSPACE CScrollOverview::workspaceAtOverviewDropPoint(const Vector2D& point, size_t* hoveredWorkspaceIdx, const PHLWINDOW& draggedWindow) const {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR)
        return nullptr;

    // X26: the dock is not a workspace, so it answers nullptr here -- and it
    // answers FIRST, because the pill floats over the main area and the card
    // underneath would otherwise claim the drop. Saying it once here is what
    // keeps the drop indicator, the mini shrink and endWindowDrag agreeing.
    if (ScrollOverview::Dock::pillContains(MONITOR, point))
        return nullptr;

    const auto ACTIVEIDX       = activeWorkspaceIndex();
    const auto WORKSPACEPITCH = getWorkspaceRenderedPitch(MONITOR, scale->value(), layout);

    // The band overlaps the main tiles it floats above, so it must be tested
    // first or a drop on the strip would land on whatever tile is underneath.
    if (const auto STRIPWORKSPACE = workspaceAtStripPoint(point, hoveredWorkspaceIdx); STRIPWORKSPACE)
        return STRIPWORKSPACE;

    for (size_t workspaceIdx = 0; workspaceIdx < images.size(); ++workspaceIdx) {
        const auto& wimg = images[workspaceIdx];
        if (!wimg || !wimg->pWorkspace)
            continue;

        const auto WORKSPACEOFFSET = workspaceOverviewOffset(workspaceIdx, ACTIVEIDX, WORKSPACEPITCH);
        for (const bool floating : {true, false}) {
            for (auto it = wimg->windows.rbegin(); it != wimg->windows.rend(); ++it) {
                const auto WINDOW = getOverviewWindowToShow(it->lock());
                if (!shouldShowOverviewWindow(WINDOW) || WINDOW == draggedWindow || WINDOW->m_isFloating != floating)
                    continue;

                const auto WINDOWBOX = getOverviewDragWindowBox(WINDOW, MONITOR, scale->value(), viewOffset->value(), WORKSPACEOFFSET, layout);
                if (!WINDOWBOX.containsPoint(point))
                    continue;

                if (hoveredWorkspaceIdx)
                    *hoveredWorkspaceIdx = workspaceIdx;

                return wimg->pWorkspace;
            }
        }
    }

    for (size_t workspaceIdx = 0; workspaceIdx < images.size(); ++workspaceIdx) {
        const auto& wimg = images[workspaceIdx];
        if (!wimg || !wimg->pWorkspace)
            continue;

        const auto WORKSPACEBOX = getOverviewWorkspaceUsableBox(wimg->pWorkspace, MONITOR, scale->value(), viewOffset->value(),
                                                                workspaceOverviewOffset(workspaceIdx, ACTIVEIDX, WORKSPACEPITCH), layout);
        const bool ONLAYOUTAXIS = layout == ScrollOverview::Config::ELayout::HORIZONTAL ?
            point.x >= WORKSPACEBOX.x && point.x <= WORKSPACEBOX.x + WORKSPACEBOX.width :
            point.y >= WORKSPACEBOX.y && point.y <= WORKSPACEBOX.y + WORKSPACEBOX.height;

        if (WORKSPACEBOX.containsPoint(point) || (ONLAYOUTAXIS && isWorkspaceScrolling(wimg->pWorkspace))) {
            if (hoveredWorkspaceIdx)
                *hoveredWorkspaceIdx = workspaceIdx;

            return wimg->pWorkspace;
        }
    }

    return nullptr;
}

PHLWORKSPACE CScrollOverview::workspaceAtOverviewCursor(size_t* hoveredWorkspaceIdx) const {
    return workspaceAtOverviewPoint(lastMousePosLocal, hoveredWorkspaceIdx);
}

bool CScrollOverview::selectOverviewWindow(PHLWINDOW window, size_t workspaceIdx, bool syncFocus) {
    if (!window)
        return false;

    closeOnWindow            = window;
    viewportCurrentWorkspace = workspaceIdx;
    rememberSelection(window);
    if (syncFocus)
        syncFocusedSelection();
    damage();
    return true;
}

bool CScrollOverview::selectWindowAtOverviewCursor(bool syncFocus) {
    lastMousePosLocal = getOverviewMousePosLocal(pMonitor.lock());

    size_t    workspaceIdx = viewportCurrentWorkspace;
    PHLWINDOW window       = windowAtOverviewCursor(&workspaceIdx);

    return selectOverviewWindow(window, workspaceIdx, syncFocus);
}

void CScrollOverview::focusOverviewWorkspace(const PHLWORKSPACE& workspace, size_t workspaceIdx) {
    if (!workspace)
        return;

    closeOnWindow.reset();
    viewportCurrentWorkspace = workspaceIdx;

    if (pMonitor && pMonitor->m_activeWorkspace != workspace) {
        if (focusSyncedFromWorkspaceID == WORKSPACE_INVALID)
            focusSyncedFromWorkspaceID = pMonitor->m_activeWorkspace ? pMonitor->m_activeWorkspace->m_id : WORKSPACE_INVALID;
        pMonitor->changeWorkspace(workspace, false, true, true);
    }

    damage();
}

// The full default click action, shared between the plain mouse path and the
// submap's `window click` dispatcher so SUPER-held clicks behave identically.
void CScrollOverview::performPrimaryClickAction() {
    // E6: the strip band floats above the main tiles, so it has to be
    // tested before them, exactly as the drop path does.
    //
    // E8-b: clicking a desktop -- on the strip or adjacent on screen --
    // switches to it AND exits. focusOverviewWorkspace changes the workspace,
    // so the viewport slide starts before the close animation takes over.
    lastMousePosLocal = getOverviewMousePosLocal(pMonitor.lock());
    size_t stripIdx   = viewportCurrentWorkspace;
    if (const auto STRIPWORKSPACE = workspaceAtStripPoint(lastMousePosLocal, &stripIdx); STRIPWORKSPACE) {
        focusOverviewWorkspace(STRIPWORKSPACE, stripIdx);
        close();
        return;
    }

    selectHoveredWorkspace();
    g_search.nav.zone = EOverviewNavZone::Tile;
    selectWindowAtOverviewCursor(true);
    close();
}

void CScrollOverview::selectHoveredWorkspace() {
    lastMousePosLocal = getOverviewMousePosLocal(pMonitor.lock());

    // workspaceAtOverviewCursor writes workspaceIdx; reading it in the same call
    // expression is unsequenced, and the stale index reached close() as the
    // "selected" workspace and undid the switch (X15).
    size_t     workspaceIdx = viewportCurrentWorkspace;
    const auto HOVEREDWORKSPACE = workspaceAtOverviewCursor(&workspaceIdx);
    focusOverviewWorkspace(HOVEREDWORKSPACE, workspaceIdx);
}

bool CScrollOverview::windowDispatcherAction(const std::string& action) {
    const auto CURRENTKEYBIND = g_pKeybindManager ? g_pKeybindManager->m_currentKeybind : SP<SKeybind>{};
    const bool FROMMOUSEBIND  = CURRENTKEYBIND && CURRENTKEYBIND->key.starts_with("mouse:");

    PHLWINDOW WINDOW;
    size_t    workspaceIdx = viewportCurrentWorkspace;

    if (FROMMOUSEBIND) {
        lastMousePosLocal = getOverviewMousePosLocal(pMonitor.lock());
        WINDOW            = windowAtOverviewCursor(&workspaceIdx);
    } else
        WINDOW = getOverviewWindowToShow(closeOnWindow.lock());

    if (!WINDOW)
        return false;

    if (action == "select")
        return selectOverviewWindow(WINDOW, workspaceIdx, true);

    if (action == "close") {
        WINDOW->sendClose();
        damage();
        return true;
    }

    return false;
}

Vector2D CScrollOverview::overviewPointToGlobal(size_t workspaceIdx, const Vector2D& pointLocal) const {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR)
        return pointLocal;

    const auto  SAFE_SCALE       = std::max(scale->value(), 0.01F);
    const auto  SAFE_MON_SCALE   = std::max(MONITOR->m_scale, 0.01F);
    const auto  VIEWPORT_CENTER  = CBox{{}, MONITOR->m_size * MONITOR->m_scale}.middle();
    const auto  VIEWPORT_CENTER_LOGICAL = CBox{{}, MONITOR->m_size}.middle();
    const auto  WORKSPACE_OFFSET   = workspaceOverviewOffset(workspaceIdx, activeWorkspaceIndex(), getWorkspaceRenderedPitch(MONITOR, scale->value(), layout));

    return ((pointLocal - axisOffsetVector(WORKSPACE_OFFSET, layout) + viewOffset->value() * scale->value() * SAFE_MON_SCALE - VIEWPORT_CENTER) * (1.F / (SAFE_SCALE * SAFE_MON_SCALE))) +
        VIEWPORT_CENTER_LOGICAL + MONITOR->m_position;
}

CBox CScrollOverview::draggedWindowBox(size_t workspaceIdx) const {
    const auto WINDOW  = getOverviewWindowToShow(drag.activeWindow.lock());
    const auto MONITOR = pMonitor.lock();
    if (!WINDOW || !MONITOR || workspaceIdx >= images.size())
        return {};

    const auto WORKSPACE_OFFSET = workspaceOverviewOffset(workspaceIdx, activeWorkspaceIndex(), getWorkspaceRenderedPitch(MONITOR, scale->value(), layout));
    auto       box               = getOverviewDragWindowBox(WINDOW, MONITOR, scale->value(), viewOffset->value(), WORKSPACE_OFFSET, layout);
    box.x = lastMousePosLocal.x - drag.grabOffsetLocal.x;
    box.y = lastMousePosLocal.y - drag.grabOffsetLocal.y;

    return box;
}

CBox CScrollOverview::draggedWindowBoxFor(PHLWINDOW window, size_t workspaceIdx, const Vector2D& pointLocal, const Vector2D& grabRatio) const {
    window             = getOverviewWindowToShow(window);
    const auto MONITOR = pMonitor.lock();
    if (!window || !MONITOR || workspaceIdx >= images.size())
        return {};

    const auto WORKSPACEOFFSET = workspaceOverviewOffset(workspaceIdx, activeWorkspaceIndex(), getWorkspaceRenderedPitch(MONITOR, scale->value(), layout));
    auto       box             = getOverviewDragWindowBox(window, MONITOR, scale->value(), viewOffset->value(), WORKSPACEOFFSET, layout);
    box.x = pointLocal.x - box.width * std::clamp(grabRatio.x, 0.0, 1.0);
    box.y = pointLocal.y - box.height * std::clamp(grabRatio.y, 0.0, 1.0);
    return box;
}

// D'1/D'2: the drag proxy is main-tile sized over its own workspace and shrinks
// to mini scale while it hovers a foreign one, so a drop target reads as a target
// before the button comes up. Wall clock like the grid reflow, so a motionless
// cursor still finishes the shrink.
float CScrollOverview::dragMiniProgress() const {
    const int   DURATION = ScrollOverview::Config::getDragMiniMs();
    const float TARGET   = drag.miniOver ? 1.F : 0.F;
    if (DURATION <= 0)
        return TARGET;

    const float ELAPSED = ScrollOverview::Config::animElapsedMs(drag.miniChangedAt);
    const float T       = gridReflowEase(ELAPSED / sc<float>(DURATION));
    if (T < 1.F)
        grid().reflowActive = true; // rides the overview's existing next-frame request

    return drag.miniFrom + (TARGET - drag.miniFrom) * T;
}

// X3 delta 6. The reference slides the proxy's grab point to its centre while it
// is over a drop target and keeps the raw grab fraction when it is free of one
// (Overview.qml:391-397), "because an edge grab looks wrong once the proxy is
// thumbnail-sized". Its predicate is `overTarget`, i.e. targetKind "cell" OR
// "tiles" (OverviewDrag.qml:48-49) -- the main area counts, so this is NOT the
// same predicate as the mini shrink, which fires only on a FOREIGN workspace.
// Hence its own progress rather than a reuse of dragMiniProgress().
//
// Deviation from the reference, deliberate: its Behavior carries no easing curve,
// so the QML default makes the slide LINEAR. This uses the house gridReflowEase
// that every other overview motion uses, including the size shrink this rides
// alongside on the same proxy -- two coupled motions on one object reading on two
// different curves is worse than the deviation.
float CScrollOverview::dragRecentreProgress() const {
    const int   DURATION = ScrollOverview::Config::getDragRecentreMs();
    const float TARGET   = drag.recentreOver ? 1.F : 0.F;
    if (DURATION <= 0)
        return TARGET;

    const float ELAPSED = ScrollOverview::Config::animElapsedMs(drag.recentreChangedAt);
    const float T       = gridReflowEase(ELAPSED / sc<float>(DURATION));
    if (T < 1.F)
        grid().reflowActive = true; // rides the overview's existing next-frame request

    return drag.recentreFrom + (TARGET - drag.recentreFrom) * T;
}

Vector2D CScrollOverview::dragEffectiveGrabRatio() const {
    const float T = std::clamp(dragRecentreProgress(), 0.F, 1.F);
    return Vector2D{
        std::clamp(drag.grabRatio.x, 0.0, 1.0) + (0.5 - std::clamp(drag.grabRatio.x, 0.0, 1.0)) * T,
        std::clamp(drag.grabRatio.y, 0.0, 1.0) + (0.5 - std::clamp(drag.grabRatio.y, 0.0, 1.0)) * T,
    };
}

CBox CScrollOverview::draggedWindowGlobalBox() const {
    const auto WINDOW  = getOverviewWindowToShow(drag.activeWindow.lock());
    const auto MONITOR = pMonitor.lock();
    if (!WINDOW || !MONITOR)
        return {};

    const auto WORKSPACEIDX = dragWorkspaceIndex(WINDOW);
    if (WORKSPACEIDX >= images.size())
        return {};

    const auto WORKSPACEOFFSET =
        workspaceOverviewOffset(WORKSPACEIDX, activeWorkspaceIndex(), getWorkspaceRenderedPitch(MONITOR, scale->value(), layout));
    const auto SOURCEBOX =
        getOverviewDragWindowBox(WINDOW, MONITOR, scale->value(), viewOffset->value(), WORKSPACEOFFSET, layout, false);
    const float MINI       = 1.F - dragMiniProgress() * (1.F - ScrollOverview::Config::getDragMiniScale());
    const auto  GLOBALSIZE = SOURCEBOX.size() * (1.F / std::max(MONITOR->m_scale, 0.01F)) * MINI;
    const auto CURSOR     = g_pInputManager->getMouseCoordsInternal();

    const auto GRAB = dragEffectiveGrabRatio();

    return {
        CURSOR.x - GLOBALSIZE.x * GRAB.x,
        CURSOR.y - GLOBALSIZE.y * GRAB.y,
        GLOBALSIZE.x,
        GLOBALSIZE.y,
    };
}

void CScrollOverview::refreshDragOriginalOverviewBoxes() {
    const auto WINDOW    = getOverviewWindowToShow(drag.activeWindow.lock());
    const auto WORKSPACE = drag.originalWorkspace.lock();
    const auto MONITOR   = pMonitor.lock();
    if (!WINDOW || !WORKSPACE || !MONITOR || drag.originalVisualBox.empty() || drag.originalBox.empty())
        return;

    size_t workspaceIdx = images.size();
    for (size_t i = 0; i < images.size(); ++i) {
        if (images[i] && images[i]->pWorkspace == WORKSPACE) {
            workspaceIdx = i;
            break;
        }
    }

    if (workspaceIdx >= images.size())
        return;

    Vector2D tapeDelta;
    if (const auto ALGO = overviewScrollingAlgorithmForWorkspace(WORKSPACE); ALGO && ALGO->m_scrollingData && ALGO->m_scrollingData->controller)
        tapeDelta = ALGO->m_scrollingData->controller->getCameraTranslation(ALGO->usableArea()) - drag.originalTapeTranslation;

    auto visualBox = drag.originalVisualBox;
    auto hitbox    = drag.originalBox;
    visualBox.translate(tapeDelta);
    hitbox.translate(tapeDelta);

    const auto WORKSPACEOFFSET = workspaceOverviewOffset(workspaceIdx, activeWorkspaceIndex(), getWorkspaceRenderedPitch(MONITOR, scale->value(), layout));

    // OVERVIEW_CPP_SPEC 13.5 X17. These MUST use the same grid-aware mapping as
    // getOverviewWindowBox(), which is what drew the tile and what
    // windowAtOverviewPoint() hit-tested. Mapping the raw desktop geometry with
    // getOverviewGlobalBox() put this box wherever the window sits on the DESKTOP,
    // not where its tile sits in the overview grid -- measured 800+ px apart on a
    // 6-window workspace. Consequence: the "cursor is still over where the drag
    // started" no-op guard in dropAnchorAtOverviewCursorOnWorkspace() essentially
    // never fired, so every drag, however short, fell through to anchor selection
    // and swapped the window into another tiling slot. Both windows then took
    // their new slots' sizes -- the user sees "dragging resizes my windows".
    drag.originalOverviewBox    = getOverviewWindowBox(WINDOW, MONITOR, scale->value(), viewOffset->value(), WORKSPACEOFFSET, layout, true, &visualBox);
    drag.originalOverviewHitbox = getOverviewWindowBox(WINDOW, MONITOR, scale->value(), viewOffset->value(), WORKSPACEOFFSET, layout, false, &hitbox);
}

CBox CScrollOverview::resizedWindowBox() const {
    const auto WINDOW  = getOverviewWindowToShow(resize.activeWindow.lock());
    const auto MONITOR = pMonitor.lock();
    if (!WINDOW || !MONITOR || resize.workspaceIdx >= images.size() || !WINDOW->m_isFloating)
        return {};

    const float SCALEFACTOR = std::max(scale->value(), 0.01F) * std::max(MONITOR->m_scale, 0.01F);
    const auto  TARGET      = WINDOW->layoutTarget();
    const auto  DELTA       = lastMousePosLocal - resize.startMouseLocal;

    Vector2D    minSizePx   = Vector2D{1.F, 1.F};
    if (TARGET) {
        if (const auto MINSIZE = TARGET->minSize(); MINSIZE.has_value())
            minSizePx = Vector2D{std::max(1.F, sc<float>(MINSIZE->x * SCALEFACTOR)), std::max(1.F, sc<float>(MINSIZE->y * SCALEFACTOR))};
    }

    std::optional<Vector2D> maxSizePx;
    if (TARGET) {
        if (const auto MAXSIZE = TARGET->maxSize(); MAXSIZE.has_value() && MAXSIZE->x > 0.F && MAXSIZE->y > 0.F)
            maxSizePx = Vector2D{std::max(minSizePx.x, sc<double>(MAXSIZE->x * SCALEFACTOR)), std::max(minSizePx.y, sc<double>(MAXSIZE->y * SCALEFACTOR))};
    }

    auto box = resizedOverviewBoxFromCorner(resize.originalBox, DELTA, resize.corner, minSizePx, maxSizePx);

    const auto WORKSPACEOFFSET = workspaceOverviewOffset(resize.workspaceIdx, activeWorkspaceIndex(), getWorkspaceRenderedPitch(MONITOR, scale->value(), layout));
    const auto WORKSPACEBOX = getOverviewWorkspaceBox(MONITOR, scale->value(), viewOffset->value(), WORKSPACEOFFSET, layout);
    const auto BORDERMARGIN = WINDOW->getRealBorderSize() * MONITOR->m_scale * scale->value();

    return clampResizedOverviewBoxToWorkspace(box, WORKSPACEBOX, resize.corner, BORDERMARGIN);
}

void CScrollOverview::beginWindowDrag(PHLWINDOW window, const CBox* stripMiniOrigin) {
    const auto WINDOW = getOverviewWindowToShow(window);
    if (!shouldShowOverviewWindow(WINDOW) || !WINDOW->layoutTarget())
        return;

    closeOnWindow = WINDOW;
    rememberSelection(WINDOW);

    size_t workspaceIdx = viewportCurrentWorkspace;
    for (size_t i = 0; i < images.size(); ++i) {
        if (images[i]->pWorkspace == WINDOW->m_workspace) {
            // D6: a strip grab must NOT drag the main view onto the mini's
            // workspace — the whole point is to pull the window out of there.
            if (!stripMiniOrigin)
                viewportCurrentWorkspace = i;
            workspaceIdx = i;
            break;
        }
    }

    const auto TARGET             = WINDOW->layoutTarget();
    drag.startedTiled              = !TARGET->floating();
    drag.originalFloatSize         = TARGET->lastFloatingSize();
    drag.originalTapeTranslation  = Vector2D{};
    drag.originalWorkspace         = WINDOW->m_workspace;
    drag.originalBox               = TARGET->position();
    drag.originalVisualBox         = WINDOW->m_group ? TARGET->position() : WINDOW->geometricBox(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
    drag.originalOverviewBox       = CBox{};
    drag.originalOverviewHitbox    = CBox{};
    drag.activeWindow              = WINDOW;


    const auto MONITOR = pMonitor.lock();
    if (MONITOR && workspaceIdx < images.size()) {
        const auto WORKSPACEOFFSET = workspaceOverviewOffset(workspaceIdx, activeWorkspaceIndex(), getWorkspaceRenderedPitch(MONITOR, scale->value(), layout));
        // D1 stays cursor-anchored for a strip grab too: the ratio has to come
        // from the mini the cursor actually pressed, not from the tile the
        // window would occupy in a card that may be off-screen.
        const auto WINDOWBOX       = stripMiniOrigin ? *stripMiniOrigin :
                                                       getOverviewDragWindowBox(WINDOW, MONITOR, scale->value(), viewOffset->value(), WORKSPACEOFFSET, layout);
        drag.grabOffsetLocal        = drag.startMouseLocal - WINDOWBOX.pos();
        drag.grabRatio              = Vector2D{
            WINDOWBOX.width > 0.0 ? drag.grabOffsetLocal.x / WINDOWBOX.width : 0.5,
            WINDOWBOX.height > 0.0 ? drag.grabOffsetLocal.y / WINDOWBOX.height : 0.5,
        };
        if (const auto ALGO = overviewScrollingAlgorithmForWorkspace(WINDOW->m_workspace); ALGO && ALGO->m_scrollingData && ALGO->m_scrollingData->controller)
            drag.originalTapeTranslation = ALGO->m_scrollingData->controller->getCameraTranslation(ALGO->usableArea());
        refreshDragOriginalOverviewBoxes();
    } else {
        drag.grabOffsetLocal = Vector2D{};
        drag.grabRatio       = Vector2D{0.5, 0.5};
    }

    // A strip grab is already at mini scale, so it starts there and grows on the
    // way into the main area instead of popping to full size for one frame.
    drag.miniOver      = stripMiniOrigin != nullptr;
    drag.miniFrom      = drag.miniOver ? 1.F : 0.F;
    drag.miniChangedAt = Time::steadyNow();

    // The grab starts at the fraction the cursor actually pressed (D1); the
    // updateWindowDrag() below is what notices a drop target and starts the slide.
    drag.recentreOver      = false;
    drag.recentreFrom      = 0.F;
    drag.recentreChangedAt = Time::steadyNow();

    // Last, after refreshDragOriginalOverviewBoxes() above has read the boxes it
    // needs out of the full grid.
    grid().dragExcluded = WINDOW.get();
    TileChrome::setHovered(PHLWINDOW{});

    updateWindowDrag();
}

void CScrollOverview::beginWindowResize() {
    const auto WINDOW  = getOverviewWindowToShow(resize.pendingWindow.lock());
    const auto MONITOR = pMonitor.lock();
    if (!shouldShowOverviewWindow(WINDOW) || shouldShowPinnedFloatingOverviewWindow(WINDOW) || !WINDOW->layoutTarget() || !MONITOR || resize.workspaceIdx >= images.size())
        return;

    closeOnWindow = WINDOW;
    rememberSelection(WINDOW);

    for (size_t i = 0; i < images.size(); ++i) {
        if (images[i]->pWorkspace == WINDOW->m_workspace) {
            viewportCurrentWorkspace = i;
            resize.workspaceIdx       = i;
            break;
        }
    }

    const auto WORKSPACEOFFSET = workspaceOverviewOffset(resize.workspaceIdx, activeWorkspaceIndex(), getWorkspaceRenderedPitch(MONITOR, scale->value(), layout));
    resize.originalBox   = getOverviewWindowBox(WINDOW, MONITOR, scale->value(), viewOffset->value(), WORKSPACEOFFSET, layout);
    resize.activeWindow  = WINDOW;
    resize.lastMouseLocal = lastMousePosLocal;

    updateWindowResize();
}

void CScrollOverview::updateWindowDrag() {
    if (!drag.activeWindow)
        return;

    size_t     dropWorkspaceIdx = 0;
    const auto DROPWORKSPACE    = workspaceAtOverviewDropPoint(lastMousePosLocal, &dropWorkspaceIdx, getOverviewWindowToShow(drag.activeWindow.lock()));

    // D'1/D'2: mini scale means "over a strip cell or a foreign card", i.e.
    // anywhere that is not the main area. Keyed off the VIEWPORT workspace, not
    // the drag's origin — for a D6 strip grab the origin is foreign by
    // definition, and keying off it would keep the proxy shrunk all the way
    // into the main area. Identical for a main-grid grab, where the two agree.
    // X26: the dock takes the same shrink the strip gets, which is the spec's
    // "not a new animation" -- the proxy is already thumbnail-sized by the time
    // the button comes up, so the minimize continues a motion instead of
    // starting one.
    const bool OVERDOCK = ScrollOverview::Dock::pillContains(pMonitor.lock(), lastMousePosLocal);

    const auto VIEWPORTWORKSPACE = viewportCurrentWorkspace < images.size() && images[viewportCurrentWorkspace] ? images[viewportCurrentWorkspace]->pWorkspace : PHLWORKSPACE{};
    if (const bool OVERFOREIGN = OVERDOCK || (DROPWORKSPACE && DROPWORKSPACE != VIEWPORTWORKSPACE); OVERFOREIGN != drag.miniOver) {
        drag.miniFrom      = dragMiniProgress(); // from where it actually is, not from the end
        drag.miniOver      = OVERFOREIGN;
        drag.miniChangedAt = Time::steadyNow();
    }

    // X3 delta 6. Any drop target, own workspace included -- the reference's
    // "tiles" DropArea spans the whole main area (WindowTiles.qml:444-452).
    if (const bool OVERTARGET = OVERDOCK || DROPWORKSPACE != nullptr; OVERTARGET != drag.recentreOver) {
        drag.recentreFrom      = dragRecentreProgress(); // from where it actually is
        drag.recentreOver      = OVERTARGET;
        drag.recentreChangedAt = Time::steadyNow();
    }

    for (const auto& overview : scrollOverviews()) {
        if (!overview)
            continue;

        overview->requestInputFrame();
        overview->damage();
    }
}

void CScrollOverview::updateWindowResize() {
    const auto WINDOW  = getOverviewWindowToShow(resize.activeWindow.lock());
    const auto TARGET  = WINDOW ? WINDOW->layoutTarget() : nullptr;
    const auto MONITOR = pMonitor.lock();

    if (!WINDOW || !TARGET || !MONITOR || resize.workspaceIdx >= images.size())
        return;

    TARGET->damageEntire();

    if (WINDOW->m_isFloating) {
        const auto RESIZEDBOX  = resizedWindowBox();
        const auto SCALEFACTOR = std::max(scale->value(), 0.01F) * std::max(MONITOR->m_scale, 0.01F);
        const auto GLOBALBOX   = CBox{overviewPointToGlobal(resize.workspaceIdx, RESIZEDBOX.pos()), RESIZEDBOX.size() * (1.F / SCALEFACTOR)};

        TARGET->rememberFloatingSize(GLOBALBOX.size());
        TARGET->setPositionGlobal(GLOBALBOX);
        TARGET->warpPositionSize();
    } else {
        const auto DELTALOCAL = lastMousePosLocal - resize.lastMouseLocal;
        const auto SAFEFACTOR = std::max(scale->value(), 0.01F) * std::max(MONITOR->m_scale, 0.01F);
        const auto DELTAGLOBAL = DELTALOCAL * (1.F / SAFEFACTOR);

        if (std::abs(DELTAGLOBAL.x) > 0.01F || std::abs(DELTAGLOBAL.y) > 0.01F) {
            g_layoutManager->resizeTarget(DELTAGLOBAL, TARGET, resize.corner);
            syncWorkspaceGeometry(TARGET->workspace());
            resize.lastMouseLocal = lastMousePosLocal;
        }
    }

    TARGET->damageEntire();
    damage();
}

void CScrollOverview::clearDragPending() {
    grid().dragExcluded        = nullptr;
    drag.pendingPrimary          = false;
    drag.activeWindow.reset();
    drag.originalWorkspace.reset();
    drag.startedTiled            = false;
    drag.originalFloatSize       = Vector2D{};
    drag.originalTapeTranslation = Vector2D{};
    drag.grabOffsetLocal         = Vector2D{};
    drag.grabRatio               = Vector2D{0.5, 0.5};
    drag.recentreOver            = false;
    drag.recentreFrom            = 0.F;
    drag.originalBox             = CBox{};
    drag.originalVisualBox       = CBox{};
    drag.originalOverviewBox     = CBox{};
    drag.originalOverviewHitbox  = CBox{};
}

void CScrollOverview::updateScrollingPan() {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR)
        return;

    const auto WORKSPACE = pan.workspace.lock();
    const auto ALGO      = overviewScrollingAlgorithmForWorkspace(WORKSPACE);
    if (!ALGO || !ALGO->m_scrollingData || !ALGO->m_scrollingData->controller) {
        pan.lastMouseLocal = lastMousePosLocal;
        return;
    }

    const auto DELTALOCAL = lastMousePosLocal - pan.lastMouseLocal;
    const auto SAFEFACTOR = std::max(scale->value(), 0.01F) * std::max(MONITOR->m_scale, 0.01F);
    const auto DELTAGLOBAL = DELTALOCAL * (1.F / SAFEFACTOR);
    const auto DELTA       = ALGO->m_scrollingData->controller->isPrimaryHorizontal() ? DELTAGLOBAL.x : DELTAGLOBAL.y;

    if (std::abs(DELTA) > 0.01F) {
        const double OFFSET = ALGO->m_scrollingData->controller->getOffset() - DELTA;
        ALGO->m_scrollingData->controller->setOffset(OFFSET);
        ALGO->m_scrollingData->lockedCameraOffset = OFFSET;
        ALGO->m_scrollingData->recalculate(true);
        pan.lastMouseLocal = lastMousePosLocal;
        markBlurDirty();
        damage();
    }
}

void CScrollOverview::beginScrollingPan(PHLWORKSPACE workspace) {
    const auto ALGO = overviewScrollingAlgorithmForWorkspace(workspace);
    if (!ALGO)
        return;

    pan.pointerDown    = true;
    pan.workspace      = workspace;
    pan.lastMouseLocal = lastMousePosLocal;
    pan.initialWindow  = getOverviewWindowToShow(closeOnWindow.lock());
    if (!pan.initialWindow || pan.initialWindow->m_workspace != workspace)
        pan.initialWindow = getOverviewWindowToShow(Desktop::focusState()->window());
    if (pan.initialWindow && pan.initialWindow->m_workspace != workspace)
        pan.initialWindow.reset();

    ALGO->m_scrollingData->lockedCameraOffset = ALGO->m_scrollingData->controller->getOffset();

    if (Desktop::focusState()->window() && Desktop::focusState()->window()->m_workspace == workspace)
        Desktop::focusState()->fullWindowFocus(nullptr, Desktop::FOCUS_REASON_DESKTOP_STATE_CHANGE);
}

void CScrollOverview::endScrollingPan() {
    const auto WORKSPACE = pan.workspace.lock();
    const auto ALGO      = overviewScrollingAlgorithmForWorkspace(WORKSPACE);
    if (ALGO) {
        const double OFFSET = clampOverviewScrollingOffset(ALGO, ALGO->m_scrollingData->controller->getOffset());
        ALGO->m_scrollingData->controller->setOffset(OFFSET);
        ALGO->m_scrollingData->lockedCameraOffset.reset();
        ALGO->m_scrollingData->recalculate();
    }

    pan.pointerDown    = false;
    pan.lastMouseLocal = Vector2D{};
    pan.workspace.reset();

    focusMostVisibleScrollingWindow(WORKSPACE);
    pan.initialWindow.reset();
}

void CScrollOverview::focusMostVisibleScrollingWindow(const PHLWORKSPACE& workspace) {
    const auto MONITOR = workspace && workspace->m_monitor ? workspace->m_monitor.lock() : pMonitor.lock();
    if (!workspace || !MONITOR)
        return;

    PHLWINDOW  bestFullWindow;
    PHLWINDOW  bestPartialWindow;
    double     bestFullDistance     = std::numeric_limits<double>::max();
    double     bestPartialVisibleArea = 0.0;
    double     bestPartialDistance  = std::numeric_limits<double>::max();
    const auto MONITORBOX           = MONITOR->logicalBox();
    const auto ALGO                 = overviewScrollingAlgorithmForWorkspace(workspace);
    auto       WORKSPACEBOX         = ALGO ? ALGO->usableArea() : MONITORBOX;
    if (ALGO)
        WORKSPACEBOX.translate(MONITOR->m_position);

    const auto preferredWindow = getOverviewWindowToShow(pan.initialWindow.lock());
    if (shouldShowOverviewWindow(preferredWindow) && preferredWindow->m_workspace == workspace && !preferredWindow->m_isFloating && preferredWindow->layoutTarget()) {
        const auto WINDOWBOX   = preferredWindow->layoutTarget()->position();
        const auto AREA        = overviewBoxArea(WINDOWBOX);
        const auto VISIBLEAREA = overviewBoxIntersectionArea(WINDOWBOX, WORKSPACEBOX);
        if (AREA > 0.0 && std::abs(VISIBLEAREA - AREA) < 0.5) {
            closeOnWindow = preferredWindow;
            rememberSelection(preferredWindow);
            Desktop::focusState()->fullWindowFocus(preferredWindow, Desktop::FOCUS_REASON_DESKTOP_STATE_CHANGE);
            return;
        }
    }

    for (const auto& windowRef : Desktop::windowState()->windows()) {
        const auto WINDOW = getOverviewWindowToShow(windowRef);
        if (!shouldShowOverviewWindow(WINDOW) || WINDOW->m_workspace != workspace || WINDOW->m_isFloating || !WINDOW->layoutTarget())
            continue;

        const auto WINDOWBOX = WINDOW->layoutTarget()->position();
        const auto AREA      = overviewBoxArea(WINDOWBOX);
        if (AREA <= 0.0)
            continue;

        const auto VISIBLEAREA = overviewBoxIntersectionArea(WINDOWBOX, WORKSPACEBOX);
        if (VISIBLEAREA <= 0.0)
            continue;

        const auto DISTANCE = overviewBoxCenterDistanceSquared(WINDOWBOX, WORKSPACEBOX);
        if (std::abs(VISIBLEAREA - AREA) < 0.5) {
            if (DISTANCE < bestFullDistance) {
                bestFullWindow   = WINDOW;
                bestFullDistance = DISTANCE;
            }
            continue;
        }

        if (VISIBLEAREA > bestPartialVisibleArea || (std::abs(VISIBLEAREA - bestPartialVisibleArea) < 0.5 && DISTANCE < bestPartialDistance)) {
            bestPartialWindow     = WINDOW;
            bestPartialVisibleArea = VISIBLEAREA;
            bestPartialDistance   = DISTANCE;
        }
    }

    const auto bestWindow = bestFullWindow ? bestFullWindow : bestPartialWindow;
    if (!bestWindow)
        return;

    closeOnWindow = bestWindow;
    rememberSelection(bestWindow);
    Desktop::focusState()->fullWindowFocus(bestWindow, Desktop::FOCUS_REASON_DESKTOP_STATE_CHANGE);
}

bool CScrollOverview::moveScrollingColumnSelection(bool next) {
    if (images.empty() || viewportCurrentWorkspace >= images.size())
        return false;

    const auto& WORKSPACEIMAGE = images[viewportCurrentWorkspace];
    if (!WORKSPACEIMAGE || !WORKSPACEIMAGE->pWorkspace)
        return false;

    const auto ALGO = overviewScrollingAlgorithmForWorkspace(WORKSPACEIMAGE->pWorkspace);
    if (!ALGO || !ALGO->m_scrollingData || ALGO->m_scrollingData->columns.empty())
        return false;

    if (!closeOnWindow || closeOnWindow->m_workspace != WORKSPACEIMAGE->pWorkspace || !shouldShowOverviewWindow(closeOnWindow.lock()) || closeOnWindow->m_isFloating)
        syncSelectionToViewport();

    const auto CURRENT = getOverviewWindowToShow(closeOnWindow.lock());
    const auto CURRENTDATA = CURRENT && CURRENT->layoutTarget() ? ALGO->dataFor(CURRENT->layoutTarget()) : nullptr;
    const auto CURRENTCOL  = CURRENTDATA ? CURRENTDATA->column.lock() : ALGO->currentColumn();
    if (!CURRENTCOL)
        return false;

    const auto firstWindowInColumn = [&](const SP<Layout::Tiled::SColumnData>& column) -> PHLWINDOW {
        if (!column)
            return {};

        for (const auto& targetData : column->targetDatas) {
            if (!targetData || !targetData->target)
                continue;

            for (const auto& windowRef : WORKSPACEIMAGE->windows) {
                const auto WINDOW = getOverviewWindowToShow(windowRef.lock());
                if (shouldShowOverviewWindow(WINDOW) && !WINDOW->m_isFloating && WINDOW->layoutTarget() == targetData->target && WINDOW->m_workspace == WORKSPACEIMAGE->pWorkspace)
                    return WINDOW;
            }
        }

        return {};
    };

    std::vector<SP<Layout::Tiled::SColumnData>> columns;
    for (const auto& column : ALGO->m_scrollingData->columns) {
        if (firstWindowInColumn(column))
            columns.emplace_back(column);
    }

    const bool PRIMARYHORIZONTAL = ALGO->m_scrollingData->controller && ALGO->m_scrollingData->controller->isPrimaryHorizontal();
    std::ranges::sort(columns, [PRIMARYHORIZONTAL](const auto& a, const auto& b) {
        const auto getColumnPosition = [PRIMARYHORIZONTAL](const auto& column) {
            if (!column || column->targetDatas.empty() || !column->targetDatas[0])
                return std::numeric_limits<double>::max();

            return PRIMARYHORIZONTAL ? column->targetDatas[0]->layoutBox.x : column->targetDatas[0]->layoutBox.y;
        };

        return getColumnPosition(a) < getColumnPosition(b);
    });

    const auto CURRENTIT = std::ranges::find(columns, CURRENTCOL);
    if (CURRENTIT == columns.end())
        return false;

    const auto CURRENTIDX = sc<int64_t>(std::distance(columns.begin(), CURRENTIT));
    const auto TARGETIDX  = CURRENTIDX + (next ? 1 : -1);
    if (TARGETIDX < 0 || TARGETIDX >= sc<int64_t>(columns.size()))
        return false;

    const auto TARGETCOL = columns[TARGETIDX];
    const auto WINDOW    = firstWindowInColumn(TARGETCOL);
    if (!WINDOW)
        return false;

    ALGO->m_scrollingData->centerOrFitCol(TARGETCOL);
    ALGO->m_scrollingData->recalculate();

    closeOnWindow = WINDOW;
    rememberSelection(WINDOW);
    syncFocusedSelection();
    damage();

    return true;
}

// The live handles a drop needs, filled in by the three gather steps below in
// the order endWindowDrag() runs them. The pure half -- what to DO with them --
// is ScrollOverview::Drop::resolve(facts) in DropDecision.hpp.
struct SOverviewDropSite {
    ScrollOverview::Drop::SFacts facts;

    // stage 1
    PHLWINDOW              window;
    SP<Layout::ITarget>    target;
    SP<Layout::CSpace>     space;
    SP<Layout::CAlgorithm> algo;
    CScrollOverview*       dropOverview = nullptr;
    PHLMONITOR             dropMonitor;
    Vector2D               dropPointLocal;

    // stage 2
    size_t                              dropWorkspaceIdx = 0;
    PHLWORKSPACE                        dropWorkspace;
    Layout::Tiled::CScrollingAlgorithm* dropScrollingAlgo = nullptr;
    bool                                dropScrollingPrimaryHorizontal = false;
    PHLWORKSPACE                        originalWorkspace;
    CBox                                dragBox;

    // stage 3
    PHLWINDOW                   sourceFullscreenWindow;
    PHLMONITOR                  monitor;
    PHLWORKSPACE                activeWorkspaceBeforeDrop;
    PHLWORKSPACE                dropActiveWorkspaceBeforeDrop;
    int                         dropSide = 0;
    CDropIndicator::SDropAnchor dropAnchor;
    std::string                 dropDirection;
};

// Stage 1. The window that was dragged, its layout handles, and the overview
// the button came up over. Deliberately asks nothing about a workspace: a
// release on the dock pill returns before that question is worth money.
void CScrollOverview::gatherDropSite(SOverviewDropSite& site) {
    site.window = getOverviewWindowToShow(drag.activeWindow.lock());
    site.target = site.window ? site.window->layoutTarget() : nullptr;
    site.space  = site.target ? site.target->space() : nullptr;
    site.algo   = site.space ? site.space->algorithm() : nullptr;

    const auto TARGETOVERVIEW = scrollOverviewAt(g_pInputManager->getMouseCoordsInternal());
    site.dropOverview         = TARGETOVERVIEW ? dynamic_cast<CScrollOverview*>(TARGETOVERVIEW.get()) : nullptr;

    site.dropMonitor    = site.dropOverview ? site.dropOverview->pMonitor.lock() : PHLMONITOR{};
    site.dropPointLocal = getOverviewMousePosLocal(site.dropMonitor);

    site.facts.haveWindow = static_cast<bool>(site.window);
    site.facts.haveTarget = static_cast<bool>(site.target);
    // OVERVIEW_CPP_SPEC X26. Released on the dock: minimize, and nothing else
    // runs -- the window must not also be moved to whatever card the pill floats
    // over. Everything else about a missed drop is unchanged, because
    // workspaceAtOverviewDropPoint already answers nullptr over the pill.
    // Asked only with a window in hand, as the guard it replaced was.
    site.facts.overPill = site.window && ScrollOverview::Dock::pillContains(site.dropMonitor, site.dropPointLocal);
}

// Stage 2. Which workspace the cursor is over and what kind of layout it runs.
void CScrollOverview::gatherDropWorkspace(SOverviewDropSite& site) {
    site.dropWorkspace = site.dropOverview ? site.dropOverview->workspaceAtOverviewDropPoint(site.dropPointLocal, &site.dropWorkspaceIdx, site.window) : PHLWORKSPACE{};
    site.dropScrollingAlgo = overviewScrollingAlgorithmForWorkspace(site.dropWorkspace);
    site.dropScrollingPrimaryHorizontal = site.dropScrollingAlgo && site.dropScrollingAlgo->m_scrollingData && site.dropScrollingAlgo->m_scrollingData->controller &&
        site.dropScrollingAlgo->m_scrollingData->controller->isPrimaryHorizontal();
    site.originalWorkspace = drag.originalWorkspace.lock();
    site.dragBox = site.dropWorkspace ? site.dropOverview->draggedWindowBoxFor(site.window, site.dropWorkspaceIdx, site.dropPointLocal, dragEffectiveGrabRatio()) : CBox{};

    site.facts.haveDropWorkspace = static_cast<bool>(site.dropWorkspace);
    site.facts.retileOnEnd       = drag.startedTiled && site.target && site.space && site.algo;
    site.facts.moveWorkspace     = site.dropWorkspace && site.dropWorkspace != site.originalWorkspace;
    site.facts.startedTiled      = drag.startedTiled;
    site.facts.scrollingLayout   = site.dropScrollingAlgo != nullptr;
    site.facts.haveOriginalBox   = !drag.originalBox.empty();
}

// Stage 3. The anchor window under the cursor, and everything the commit has to
// have read BEFORE it starts moving things: the source workspace's fullscreen
// window and both monitors' active workspaces.
void CScrollOverview::gatherDropAnchor(SOverviewDropSite& site) {
    site.sourceFullscreenWindow =
        site.originalWorkspace ? getOverviewWindowToShow(Fullscreen::controller()->getFullscreenWindow(site.originalWorkspace)) : PHLWINDOW{};
    site.facts.restoreSourceFullscreenFocus = site.window && site.window->m_isFloating && site.facts.moveWorkspace &&
        shouldShowOverviewWindow(site.sourceFullscreenWindow) && site.sourceFullscreenWindow != site.window &&
        site.sourceFullscreenWindow->m_workspace == site.originalWorkspace && Fullscreen::controller()->isFullscreen(site.sourceFullscreenWindow);

    const bool DROPSIDEHORIZONTAL = site.dropOverview->layout != ScrollOverview::Config::ELayout::HORIZONTAL || site.dropScrollingPrimaryHorizontal;

    site.monitor                       = pMonitor.lock();
    site.activeWorkspaceBeforeDrop     = site.monitor ? site.monitor->m_activeWorkspace : PHLWORKSPACE{};
    site.dropActiveWorkspaceBeforeDrop = site.dropMonitor ? site.dropMonitor->m_activeWorkspace : PHLWORKSPACE{};

    // Z3. Whether the drop point can be read as a point INSIDE the destination
    // card, which is the only thing that makes mapping the cursor into that
    // workspace meaningful. Two conditions, and X42 only had the first:
    //
    //   * the card is fully on screen, and
    //   * the drop did not land on the DESKTOP STRIP.
    //
    // X42 assumed the second followed from the first -- "a strip-mini drop
    // targets a card that is off-screen by construction". It does not: the strip
    // shows every desktop including the one whose card fills the viewport, so
    // dropping on THAT mini gave a fully-visible card and a cursor sitting up in
    // the strip band. overviewPointToGlobal then mapped a strip coordinate into
    // the workspace and the window landed jammed against the top edge --
    // measured (200,150) -> (324,1) -- which is the user's "subsequent moves of
    // that same window do not remember coordinates". The first move is to
    // another desktop, whose card is off-screen, so it kept its position; moving
    // it back to the desktop you are looking at is the one that loses it.
    //
    // workspaceAtStripPoint is the honest test rather than "is the point in the
    // card box": the strip band deliberately OVERLAPS the tiles it floats above,
    // which workspaceAtOverviewDropPoint already has to say out loud for the
    // same reason.
    if (site.dropMonitor) {
        const auto WORKSPACEBOX = getOverviewWorkspaceBox(
            site.dropMonitor, site.dropOverview->scale->value(), site.dropOverview->viewOffset->value(),
            site.dropOverview->workspaceOverviewOffset(site.dropWorkspaceIdx, site.dropOverview->activeWorkspaceIndex(),
                                                       getWorkspaceRenderedPitch(site.dropMonitor, site.dropOverview->scale->value(), site.dropOverview->layout)),
            site.dropOverview->layout);
        site.facts.dropPointMapsIntoCard =
            overviewBoxFullyVisibleOnMonitor(WORKSPACEBOX, site.dropMonitor) && !site.dropOverview->workspaceAtStripPoint(site.dropPointLocal);

        if (DROPSIDEHORIZONTAL) {
            if (site.dropPointLocal.x < WORKSPACEBOX.x)
                site.dropSide = -1;
            else if (site.dropPointLocal.x > WORKSPACEBOX.x + WORKSPACEBOX.width)
                site.dropSide = 1;
        } else {
            if (site.dropPointLocal.y < WORKSPACEBOX.y)
                site.dropSide = -1;
            else if (site.dropPointLocal.y > WORKSPACEBOX.y + WORKSPACEBOX.height)
                site.dropSide = 1;
        }
    }

    // The one write in the gathering half, and it is an argument hand-off:
    // dropAnchorAtOverviewCursorOnWorkspace reads the destination overview's
    // cursor rather than taking a point.
    site.dropOverview->lastMousePosLocal = site.dropPointLocal;
    site.dropAnchor                      = site.dropOverview->dropAnchorAtOverviewCursorOnWorkspace(site.dropWorkspaceIdx, site.window, this);
    site.dropDirection                   = site.dropAnchor.direction;

    site.facts.haveAnchor           = static_cast<bool>(site.dropAnchor.window);
    site.facts.haveDirection        = !site.dropDirection.empty();
    site.facts.onScrollingCrossAxis = site.dropScrollingPrimaryHorizontal ? site.dropDirection == "u" || site.dropDirection == "d" :
                                                                           site.dropDirection == "l" || site.dropDirection == "r";
    // The two queries below are asked under exactly the guards the branches
    // that used to ask them asked under. They are reads, but a refactor that
    // quietly starts calling layoutTarget() or isFullscreen() on windows the
    // old code never asked about is not a refactor.
    site.facts.anchorHasLayoutTarget =
        site.facts.haveAnchor && site.facts.haveDirection && !site.facts.scrollingLayout && static_cast<bool>(site.dropAnchor.window->layoutTarget());
    site.facts.anchorFullscreen = site.facts.scrollingLayout && site.facts.haveAnchor && site.facts.onScrollingCrossAxis &&
        Fullscreen::controller()->isFullscreen(site.dropAnchor.window);
}

void CScrollOverview::commitDropMinimize(const SOverviewDropSite& site) {
    // Y5/Y8. Was a bare Minimize::minimize, which is exactly what X54's
    // "everything that minimizes must come through these" forbids -- so a
    // window dropped on the dock TELEPORTED while a clicked one flew. Now it
    // flies like every other minimize, and `true` asks the icon it lands in
    // to bounce once.
    ScrollOverview::Dock::minimizeWithFlight(site.dropMonitor, site.window, true);
    clearDragPending();
    damage();
    if (site.dropOverview && site.dropOverview != this)
        site.dropOverview->damage();
}

void CScrollOverview::commitDropCancel(const SOverviewDropSite& site) {
    // OVERVIEW_CPP_SPEC D5. Nothing is moved, so the window keeps its
    // workspace, position and size — the drop is correctly a no-op. Without
    // the seed below the tile still teleports home in one frame (measured:
    // 326 px in 1 frame at 60fps), which reads as a glitch rather than a
    // rejected drop. U4's reflow already eases a tile from wherever it is to
    // its slot, so hand it the proxy's box and it carries the return.
    if (const auto MON = pMonitor.lock(); site.window && MON) {
        if (CBox slot; overviewGridSlot(site.window, MON, slot)) {
            const auto  WSIDX    = dragWorkspaceIndex(site.window);
            const auto  FLOATBOX = draggedWindowBox(WSIDX);
            const float SCALEDIV = std::max(scale->value(), 0.01F) * std::max(MON->m_scale, 0.01F);

            grid().reflows[site.window.get()] = SGridReflow{site.window, CBox{overviewPointToGlobal(WSIDX, FLOATBOX.pos()), FLOATBOX.size() * (1.F / SCALEDIV)}, slot,
                                                          Time::steadyNow(), g_overviewFrame, false};
            grid().reflowActive               = true;
        }
    }

    clearDragPending();
    damage();
    if (site.dropOverview && site.dropOverview != this)
        site.dropOverview->damage();
}

void CScrollOverview::commitDrop(const SOverviewDropSite& site, const ScrollOverview::Drop::SDecision& DECISION) {
    const auto  WINDOW        = site.window;
    const auto& TARGET        = site.target;
    const auto& SPACE         = site.space;
    const auto  DROPWORKSPACE = site.dropWorkspace;
    const auto  DROPMONITOR   = site.dropMonitor;
    const auto  MONITOR       = site.monitor;
    const auto  DROPANCHOR    = site.dropAnchor.window;
    auto* const dropOverview  = site.dropOverview;

    const auto RESTOREACTIVEWORKSPACE = [&]() {
        if (MONITOR && site.activeWorkspaceBeforeDrop && MONITOR->m_activeWorkspace != site.activeWorkspaceBeforeDrop)
            MONITOR->changeWorkspace(site.activeWorkspaceBeforeDrop, false, true, true);
        if (DROPMONITOR && DROPMONITOR != MONITOR && site.dropActiveWorkspaceBeforeDrop && DROPMONITOR->m_activeWorkspace != site.dropActiveWorkspaceBeforeDrop)
            DROPMONITOR->changeWorkspace(site.dropActiveWorkspaceBeforeDrop, false, true, true);
    };
    const auto RESTOREVIEWPORTWORKSPACE = [&]() {
        if (!site.activeWorkspaceBeforeDrop)
            return;

        for (size_t i = 0; i < images.size(); ++i) {
            if (images[i] && images[i]->pWorkspace == site.activeWorkspaceBeforeDrop) {
                viewportCurrentWorkspace = i;
                return;
            }
        }
    };

    if (DECISION.clearAnchorFullscreen) {
        Fullscreen::controller()->setFullscreenMode(DROPANCHOR, Fullscreen::FSMODE_NONE);
        if (const auto ANCHORDATA = site.dropScrollingAlgo->dataFor(DROPANCHOR->layoutTarget()); ANCHORDATA) {
            if (const auto ANCHORCOL = ANCHORDATA->column.lock())
                ANCHORCOL->setColumnWidth(1.F);
        }
    }

    using enum ScrollOverview::Drop::EAction;
    using EPlacement = ScrollOverview::Drop::EPlacement;

    switch (DECISION.action) {
        case RetileMove: {
            Desktop::globalWindowController()->moveWindowToWorkspace(WINDOW, DROPWORKSPACE);
            RESTOREACTIVEWORKSPACE();

            switch (DECISION.placement) {
                case EPlacement::ScrollEdgeFar: moveOverviewScrollingTargetToWorkspaceEdge(TARGET, 1); break;
                case EPlacement::NextToAnchor: moveOverviewTargetNextToWindow(TARGET, DROPANCHOR, site.dropDirection); break;
                case EPlacement::ScrollEdgeSide: moveOverviewScrollingTargetToWorkspaceEdge(TARGET, site.dropSide); break;
                default: break;
            }

            TARGET->rememberFloatingSize(drag.originalFloatSize);
            RESTOREACTIVEWORKSPACE();
            RESTOREVIEWPORTWORKSPACE();
            break;
        }
        case RetileLocal: {
            TARGET->damageEntire();

            switch (DECISION.placement) {
                case EPlacement::SwitchTargets:
                    DROPANCHOR->layoutTarget()->damageEntire();
                    g_layoutManager->switchTargets(TARGET, DROPANCHOR->layoutTarget(), true);
                    DROPANCHOR->layoutTarget()->damageEntire();
                    break;
                case EPlacement::NextToAnchor: moveOverviewTargetNextToWindow(TARGET, DROPANCHOR, site.dropDirection); break;
                case EPlacement::ScrollEdgeSide: moveOverviewScrollingTargetToWorkspaceEdge(TARGET, site.dropSide); break;
                default: break;
            }

            TARGET->rememberFloatingSize(drag.originalFloatSize);
            TARGET->warpPositionSize();
            TARGET->damageEntire();

            Desktop::focusState()->fullWindowFocus(WINDOW, Desktop::FOCUS_REASON_DESKTOP_STATE_CHANGE);

            if (const auto WORKSPACE = SPACE->workspace())
                WORKSPACE->updateWindows();
            break;
        }
        case FloatMove: {
            Desktop::globalWindowController()->moveWindowToWorkspace(WINDOW, DROPWORKSPACE);
            RESTOREACTIVEWORKSPACE();
            if (TARGET) {
                // X41/X42: the proxy box is the MISSION-CONTROL SLOT, i.e. real size
                // x bestScale (overviewGridLayout), so descaling it by the overview
                // and monitor scale alone still commits a shrunken window. bestScale
                // is capped at 1.0, which is why uniform fixture tiles never caught
                // this and only a large window shows it. A drag moves a window; it
                // must never resize it.
                const auto GLOBALSIZE = DECISION.useOriginalBox ?
                    drag.originalBox.size() :
                    site.dragBox.size() * (1.F / (std::max(dropOverview->scale->value(), 0.01F) * std::max(DROPMONITOR ? DROPMONITOR->m_scale : 1.F, 0.01F)));
                // X42: a strip-mini drop targets a card that is off-screen by
                // construction, so there is no in-card point to map the cursor to.
                // Keeping the window where it already was is what stops every
                // arrival stacking up on the workspace centre.
                auto GLOBALBOX = DECISION.mapPointIntoCard ? CBox{dropOverview->overviewPointToGlobal(site.dropWorkspaceIdx, site.dragBox.pos()), GLOBALSIZE} :
                    DECISION.useOriginalBox                ? CBox{drag.originalBox.pos(), GLOBALSIZE} :
                                                             centerBoxInWorkspace(CBox{Vector2D{}, GLOBALSIZE}, DROPWORKSPACE, DROPMONITOR);
                GLOBALBOX      = clampBoxToWorkspace(GLOBALBOX, DROPWORKSPACE, DROPMONITOR, WINDOW->getRealBorderSize());


                TARGET->setPositionGlobal(GLOBALBOX);
                TARGET->warpPositionSize();
            }

            RESTOREACTIVEWORKSPACE();
            RESTOREVIEWPORTWORKSPACE();
            break;
        }
        case FloatLocal: {
            const auto workspaceIdx = dragWorkspaceIndex(WINDOW);

            const auto FLOATBOX  = draggedWindowBox(workspaceIdx);
            const auto GLOBALPOS = overviewPointToGlobal(workspaceIdx, FLOATBOX.pos());
            // X41: same mission-control-slot descaling bug as the branch above. This
            // is the path a CANCELLED drag takes when the button comes up over the
            // window's own card, so committing the slot size shrinks it every time.
            const auto GLOBALSIZE = DECISION.useOriginalBox ?
                drag.originalBox.size() :
                FLOATBOX.size() * (1.F / (std::max(scale->value(), 0.01F) * std::max(MONITOR ? MONITOR->m_scale : 1.F, 0.01F)));
            auto       GLOBALBOX  = CBox{GLOBALPOS, GLOBALSIZE};
            const auto WORKSPACE  = workspaceIdx < images.size() && images[workspaceIdx] ? images[workspaceIdx]->pWorkspace : WINDOW->m_workspace;

            GLOBALBOX = clampBoxToWorkspace(GLOBALBOX, WORKSPACE, MONITOR, WINDOW->getRealBorderSize());

            TARGET->damageEntire();
            TARGET->setPositionGlobal(GLOBALBOX);
            TARGET->warpPositionSize();
            TARGET->damageEntire();
            break;
        }
        default: break;
    }

    if (DECISION.restoreSourceFullscreenFocus) {
        const bool POSTHIDDENEVENT = MONITOR && site.originalWorkspace == MONITOR->m_activeWorkspace;

        if (POSTHIDDENEVENT) {
            closeOnWindow = site.sourceFullscreenWindow;
            rememberSelection(site.sourceFullscreenWindow);
            focusOverviewFullscreenWindowIfActiveWorkspace(site.sourceFullscreenWindow, site.originalWorkspace, MONITOR);
            emitFullscreenVisibilityState(site.sourceFullscreenWindow, true);
        }
    }

    if (DROPWORKSPACE && DROPMONITOR && DROPWORKSPACE == DROPMONITOR->m_activeWorkspace) {
        const auto FULLSCREENWINDOW = getOverviewWindowToShow(Fullscreen::controller()->getFullscreenWindow(DROPWORKSPACE));
        if (shouldShowOverviewWindow(FULLSCREENWINDOW) && FULLSCREENWINDOW->m_workspace == DROPWORKSPACE)
            dropOverview->emitFullscreenVisibilityState(FULLSCREENWINDOW, true);
    }

    clearDragPending();
    rebuildPending = true;
    damage();
    if (dropOverview != this) {
        dropOverview->rebuildPending = true;
        dropOverview->damage();
    }
}

// Gather, decide, commit -- and ask the pure resolver after each gathering
// stage, so the two early exits are the same ladder the final one is rather
// than a second copy of it.
void CScrollOverview::endWindowDrag() {
    // Before anything below reads a slot: the D5 return seed wants this window's
    // slot in the FULL grid, not the gap it has been leaving open.
    grid().dragExcluded = nullptr;

    SOverviewDropSite site;

    gatherDropSite(site);
    if (ScrollOverview::Drop::resolve(site.facts).action == ScrollOverview::Drop::EAction::Minimize) {
        commitDropMinimize(site);
        return;
    }

    gatherDropWorkspace(site);
    if (ScrollOverview::Drop::resolve(site.facts).action == ScrollOverview::Drop::EAction::CancelToGrid) {
        commitDropCancel(site);
        return;
    }

    gatherDropAnchor(site);
    commitDrop(site, ScrollOverview::Drop::resolve(site.facts));
}

void CScrollOverview::endWindowResize() {
    const auto WINDOW  = getOverviewWindowToShow(resize.activeWindow.lock());
    const auto TARGET  = WINDOW ? WINDOW->layoutTarget() : nullptr;
    const auto MONITOR = pMonitor.lock();

    if (WINDOW && TARGET && WINDOW->m_isFloating && resize.workspaceIdx < images.size()) {
        const auto RESIZEDBOX  = resizedWindowBox();
        const auto SCALEFACTOR = std::max(scale->value(), 0.01F) * std::max(MONITOR ? MONITOR->m_scale : 1.F, 0.01F);
        auto       GLOBALBOX   = CBox{overviewPointToGlobal(resize.workspaceIdx, RESIZEDBOX.pos()), RESIZEDBOX.size() * (1.F / SCALEFACTOR)};

        TARGET->damageEntire();
        TARGET->rememberFloatingSize(GLOBALBOX.size());
        TARGET->setPositionGlobal(GLOBALBOX);
        TARGET->warpPositionSize();
        TARGET->damageEntire();
    }

    resize.pointerDown = false;
    resize.pendingWindow.reset();
    resize.activeWindow.reset();
    resize.originalBox  = CBox{};
    resize.lastMouseLocal = Vector2D{};
    resize.corner       = Layout::CORNER_NONE;
    rebuildPending     = true;
    damage();
}

void CScrollOverview::moveViewportWorkspace(bool up) {
    if (images.empty())
        return;

    if (viewportCurrentWorkspace == 0 && !up)
        return;
    if (viewportCurrentWorkspace == images.size() - 1 && up)
        return;

    if (up)
        viewportCurrentWorkspace++;
    else
        viewportCurrentWorkspace--;

    const auto& TARGETWORKSPACEIMAGE = images[viewportCurrentWorkspace];
    if (!TARGETWORKSPACEIMAGE || !TARGETWORKSPACEIMAGE->pWorkspace)
        return;

    closeOnWindow.reset();

    if (const auto it = rememberedSelection.find(TARGETWORKSPACEIMAGE->pWorkspace->m_id); it != rememberedSelection.end()) {
        const auto rememberedWindow = getOverviewWindowToShow(it->second.lock());
        if (rememberedWindow && rememberedWindow->m_workspace == TARGETWORKSPACEIMAGE->pWorkspace && shouldShowOverviewWindow(rememberedWindow))
            closeOnWindow = rememberedWindow;
    }

    if (!closeOnWindow)
        closeOnWindow = windowClosestToWorkspaceCenter(viewportCurrentWorkspace);

    if (pMonitor && pMonitor->m_activeWorkspace != TARGETWORKSPACEIMAGE->pWorkspace)
        pMonitor->changeWorkspace(TARGETWORKSPACEIMAGE->pWorkspace, false, true, true);

    damage();
}

bool CScrollOverview::scrollStepAllowed(uint32_t timeMs) {
    const uint32_t DELAY = sc<uint32_t>(ScrollOverview::Config::getScrollEventDelay());

    // throttle discrete scroll steps so a single notch / a burst of high-res events only steps once
    if (lastScrollStepTimeMs != 0 && timeMs >= lastScrollStepTimeMs && timeMs - lastScrollStepTimeMs < DELAY)
        return false;

    lastScrollStepTimeMs = timeMs;
    return true;
}

void CScrollOverview::trackpadSwipeLayout(const PHLWORKSPACE target, const double delta) {
    const float SCALE = std::max<float>(scale->value(), 0.01F);
    const auto ALGO = overviewScrollingAlgorithmForWorkspace(target);

    if (!ALGO) {
        return;
    }

    // fingers lifted — snap to the nearest column
    if (delta == 0.0) {
        if (trackpadTapeFollowing) {
            trackpadTapeFollowing = false;
            ALGO->snapToGrid();
            focusMostVisibleScrollingWindow(target);
            damage();
        }
        return;
    }

    trackpadTapeFollowing = true;
    ALGO->moveTape(sc<float>(-1 * delta * ScrollOverview::Config::getTouchpadScrollFactor() / SCALE));
    damage();
}

void CScrollOverview::trackpadSwipeWorkspace(const double delta) {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR)
        return;

    // fingers lifted — snap to the nearest workspace
    if (delta == 0.0) {
        finishWorkspaceScrollFollow();
        return;
    }

    const float SCALE = std::max<float>(scale->value(), 0.01F);

    trackpadWorkspaceFollowing  = true;
    trackpadScrollAccum         += delta * ScrollOverview::Config::getTouchpadScrollFactor();

    viewOffset->setValueAndWarp(axisOffsetVector(sc<float>(trackpadWorkspaceScrollOffset(MONITOR, SCALE)), layout));
    damage();
}

double CScrollOverview::trackpadWorkspaceScrollOffset(PHLMONITOR monitor, float renderScale) {
    const float RENDEREDLOGICALUNIT = renderScale * std::max<float>(monitor->m_scale, 0.01F);
    const float LOGICALPITCH        = getWorkspaceLogicalPitch(monitor, renderScale, layout);

    double offset    = trackpadScrollAccum / RENDEREDLOGICALUNIT;
    double minOffset = 0.0;
    double maxOffset = 0.0;

    for (size_t i = 0; i < images.size(); ++i) {
        if (!images[i] || !images[i]->pWorkspace)
            continue;

        const double WORKSPACEOFFSET = workspaceOverviewLogicalOffset(i, viewportCurrentWorkspace, LOGICALPITCH);
        minOffset                    = std::min(minOffset, WORKSPACEOFFSET);
        maxOffset                    = std::max(maxOffset, WORKSPACEOFFSET);
    }

    offset              = std::clamp(offset, minOffset, maxOffset);
    trackpadScrollAccum = offset * RENDEREDLOGICALUNIT;

    return offset;
}

void CScrollOverview::finishWorkspaceScrollFollow() {
    if (!trackpadWorkspaceFollowing)
        return;

    const auto MONITOR = pMonitor.lock();
    if (!MONITOR) {
        trackpadWorkspaceFollowing = false;
        trackpadScrollAccum        = 0.0;
        return;
    }

    trackpadWorkspaceFollowing  = false;
    trackpadScrollAccum         = 0.0;

    const float  SCALE          = std::max<float>(scale->value(), 0.01F);
    const float  LOGICALPITCH   = getWorkspaceLogicalPitch(MONITOR, SCALE, layout);
    const float  RENDEREDPITCH  = getWorkspaceRenderedPitch(MONITOR, SCALE, layout);
    const size_t ACTIVEIDX      = activeWorkspaceIndex();
    const double VIEWPORTCENTER = axisSize(MONITOR->m_size * MONITOR->m_scale, layout) / 2.0;

    size_t targetIdx    = viewportCurrentWorkspace;
    double bestDistance = std::numeric_limits<double>::max();

    for (size_t i = 0; i < images.size(); ++i) {
        if (!images[i] || !images[i]->pWorkspace)
            continue;

        const auto   WORKSPACEBOX = getOverviewWorkspaceBox(MONITOR, SCALE, viewOffset->value(), workspaceOverviewOffset(i, ACTIVEIDX, RENDEREDPITCH), layout);
        const double DISTANCE     = std::abs(axisValue(WORKSPACEBOX.middle(), layout) - VIEWPORTCENTER);
        if (DISTANCE < bestDistance) {
            bestDistance = DISTANCE;
            targetIdx    = i;
        }
    }

    // Slow swipes often stop short of halfway. Commit from ~28% of a pitch
    // toward a neighbour instead of requiring the card to become nearest.
    const double DRAG = axisValue(viewOffset->value(), layout);
    if (targetIdx == viewportCurrentWorkspace && std::abs(DRAG) >= 0.28 * LOGICALPITCH) {
        if (DRAG > 0 && viewportCurrentWorkspace + 1 < images.size())
            targetIdx = viewportCurrentWorkspace + 1;
        else if (DRAG < 0 && viewportCurrentWorkspace > 0)
            targetIdx = viewportCurrentWorkspace - 1;
    }

    if (targetIdx == viewportCurrentWorkspace) {
        *viewOffset = Vector2D{};
        return;
    }

    const double OFFSET       = axisValue(viewOffset->value(), layout);
    const double TARGETOFFSET = workspaceOverviewLogicalOffset(targetIdx, viewportCurrentWorkspace, LOGICALPITCH);

    trackpadGestureSettleOffset  = OFFSET - TARGETOFFSET;
    trackpadGestureSettlePending = true;

    const size_t BEFORE = viewportCurrentWorkspace;
    const bool   NEXT   = targetIdx > viewportCurrentWorkspace;
    const size_t STEPS  = sc<size_t>(std::abs(sc<long>(targetIdx) - sc<long>(viewportCurrentWorkspace)));
    for (size_t i = 0; i < STEPS; ++i)
        moveViewportWorkspace(NEXT);

    if (viewportCurrentWorkspace == BEFORE) {
        trackpadGestureSettlePending = false;
        *viewOffset                  = Vector2D{};
    }
}

void CScrollOverview::syncSelectionToViewport() {
    if (images.empty() || viewportCurrentWorkspace >= images.size()) {
        closeOnWindow.reset();
        return;
    }

    const auto& WSPACE = images[viewportCurrentWorkspace];

    if (closeOnWindow && closeOnWindow->m_workspace == WSPACE->pWorkspace) {
        const auto selectedWindow = getOverviewWindowToShow(closeOnWindow.lock());
        for (const auto& windowRef : WSPACE->windows) {
            if (getOverviewWindowToShow(windowRef.lock()) == selectedWindow) {
                closeOnWindow = selectedWindow;
                rememberSelection(selectedWindow);
                syncFocusedSelection();
                return;
            }
        }
    }

    if (const auto it = rememberedSelection.find(WSPACE->pWorkspace->m_id); it != rememberedSelection.end()) {
        const auto rememberedWindow = getOverviewWindowToShow(it->second.lock());
        if (rememberedWindow && rememberedWindow->m_workspace == WSPACE->pWorkspace && shouldShowOverviewWindow(rememberedWindow)) {
            for (const auto& windowRef : WSPACE->windows) {
                if (getOverviewWindowToShow(windowRef.lock()) == rememberedWindow) {
                    closeOnWindow = rememberedWindow;
                    syncFocusedSelection();
                    return;
                }
            }
        }
    }

    const auto focusedWindow = Desktop::focusState()->window();
    if (shouldShowOverviewWindow(focusedWindow) && focusedWindow->m_workspace == WSPACE->pWorkspace) {
        closeOnWindow = focusedWindow;
        rememberSelection(focusedWindow);
        syncFocusedSelection();
        return;
    }

    if (const auto window = windowClosestToWorkspaceCenter(viewportCurrentWorkspace)) {
        closeOnWindow = window;
        rememberSelection(window);
        syncFocusedSelection();
        return;
    }

    closeOnWindow.reset();
    if (activeScrollOverview().get() == this)
        Desktop::focusState()->fullWindowFocus(nullptr, Desktop::FOCUS_REASON_DESKTOP_STATE_CHANGE);
}

void CScrollOverview::syncFocusedSelection() {
    const auto window = getOverviewWindowToShow(closeOnWindow.lock());
    if (!shouldShowOverviewWindow(window))
        return;

    closeOnWindow = window;

    if (activeScrollOverview().get() != this)
        return;

    if (Desktop::focusState()->window() == window && window->m_workspace == pMonitor->m_activeWorkspace)
        return;

    const auto PREVIOUSWORKSPACE = pMonitor ? pMonitor->m_activeWorkspace : PHLWORKSPACE{};

    Desktop::focusState()->fullWindowFocus(window, Desktop::FOCUS_REASON_KEYBIND);

    if (window->m_workspace != PREVIOUSWORKSPACE && focusSyncedFromWorkspaceID == WORKSPACE_INVALID)
        focusSyncedFromWorkspaceID = PREVIOUSWORKSPACE ? PREVIOUSWORKSPACE->m_id : WORKSPACE_INVALID;
}

size_t CScrollOverview::dragWorkspaceIndex(PHLWINDOW window) const {
    if (viewportCurrentWorkspace < images.size() && images[viewportCurrentWorkspace] && images[viewportCurrentWorkspace]->pWorkspace)
        return viewportCurrentWorkspace;

    if (!window)
        return images.size();

    for (size_t i = 0; i < images.size(); ++i) {
        if (images[i] && images[i]->pWorkspace == window->m_workspace)
            return i;
    }

    return images.size();
}

// X61. ONE selection, moved by what is actually DRAWN ON SCREEN.
//
// What was here before was upstream's SCROLLING-LAYOUT walk, and it could not
// see this product at all: it skipped every candidate with `m_isFloating` and
// navigated by the window's REAL `middle()`. In this rice every window floats
// and the tiles are laid out in a mission-control GRID with no relation to real
// geometry, so the arrow keys were dead by construction -- and would have
// pointed the wrong way even if they had not been.
//
// Everything the user can see is collected as a box: strip cells, result rows
// and grid cells, window tiles. An arrow then picks the nearest box in that
// direction. Crossing from one surface to the next costs nothing because it is
// the same geometry either way, which is what "arrow through everything" means.
struct SOverviewNavTarget {
    CBox             box;
    EOverviewNavZone zone;
    int              index = -1; // Desktop: workspace index. Result: result index.
    PHLWINDOW        window;     // Tile
};

// Everything on screen an arrow can land on, in one list of boxes. Reads
// state; changes none.
void CScrollOverview::collectNavTargets(PHLMONITOR MONITOR, std::vector<SOverviewNavTarget>& targets) const {
    for (size_t i = 0; i < images.size(); ++i) {
        if (!images[i] || !images[i]->pWorkspace)
            continue;

        if (const CBox BOX = stripCellBox(i, MONITOR, scale->value()); BOX.width > 1.F && BOX.height > 1.F)
            targets.push_back({BOX, EOverviewNavZone::Desktop, sc<int>(i), {}});
    }

    // The results as the last frame DREW them -- the same boxes the mouse
    // hit-tests against, so the two devices cannot disagree about where a row
    // is. G5's grid cells and the list rows are both in here already.
    for (const auto& HIT : g_search.layout.hits)
        targets.push_back({HIT.box, EOverviewNavZone::Result, HIT.index, {}});

    // Every workspace's tiles, filtered to the ones actually ON the monitor.
    // Asking only images[viewportCurrentWorkspace] made the tiles unreachable:
    // that index is the VIEWPORT's, and a tile's box already carries its
    // workspace offset, so the honest test for "can the user see this" is
    // whether the drawn box lands on the monitor -- the same test the render
    // pass uses. Measured 2026-08-05: with the current-workspace filter, arrows
    // produced a byte-identical frame on a desktop with two visible tiles.
    for (const auto& IMAGE : images) {
        if (!IMAGE || !IMAGE->pWorkspace)
            continue;

        for (const auto& WINDOW : overviewMainGridWindows(IMAGE->pWorkspace, MONITOR)) {
            const CBox BOX = tileBox(WINDOW);
            if (BOX.width > 1.F && BOX.height > 1.F && overviewBoxIntersectsMonitor(BOX, MONITOR))
                targets.push_back({BOX, EOverviewNavZone::Tile, -1, WINDOW});
        }
    }

}

// Where the selection is NOW. Empty when the zone has nothing selected in it,
// which is what the caller's "land somewhere" fallback keys off.
CBox CScrollOverview::currentNavBox(const std::vector<SOverviewNavTarget>& targets, PHLMONITOR MONITOR) const {
    CBox current;
    switch (g_search.nav.zone) {
        case EOverviewNavZone::Search:
            current = g_search.layout.pill;
            break;
        case EOverviewNavZone::Result:
            for (const auto& TARGET : targets)
                if (TARGET.zone == EOverviewNavZone::Result && TARGET.index == g_search.nav.selection)
                    current = TARGET.box;
            break;
        case EOverviewNavZone::Tile:
            if (const auto SELECTED = closeOnWindow.lock(); SELECTED)
                current = tileBox(SELECTED);
            break;
        case EOverviewNavZone::Desktop: current = stripCellBox(viewportCurrentWorkspace, MONITOR, scale->value()); break;
    }
    return current;
}

// The pick, and it is a pure function of the boxes: no compositor state, no
// members, so the walk can be reasoned about (and one day tabled) on its own.
static const SOverviewNavTarget* pickNavTarget(const std::vector<SOverviewNavTarget>& targets, const CBox& current, EOverviewNavZone zone, bool LEFT, bool RIGHT,
                                               bool DOWN) {
    const Vector2D CUR = current.middle();

    // Ordering, in this order and for this reason:
    //
    //   1. a candidate the selection OVERLAPS on the perpendicular axis beats
    //      one it does not. Scoring by weighted distance instead sent UP from a
    //      tile PAST the strip and PAST the result list to a grid cell at the
    //      very top, because that cell happened to be column-aligned with the
    //      tile and alignment was worth more than being 300px closer.
    //      Measured 2026-08-05: one UP press jumped from the tiles to the last
    //      apps cell, skipping both surfaces in between.
    //   2. then nearest in the direction of travel.
    //   3. then nearest sideways, which only ever breaks a tie.
    //
    // Same shape as upstream's own bestHasOverlap/bestPrimaryDistance walk --
    // the geometry it applied that to was simply the wrong geometry.
    struct SScore {
        bool  noOverlap = true;
        float primary   = 0.F;
        float secondary = 0.F;

        bool beats(const SScore& other) const {
            if (noOverlap != other.noOverlap)
                return !noOverlap;
            if (primary != other.primary)
                return primary < other.primary;
            return secondary < other.secondary;
        }
    };

    const SOverviewNavTarget* best = nullptr;
    SScore                    bestScore;
    const SOverviewNavTarget* near = nullptr;
    SScore                    nearScore;

    for (const auto& TARGET : targets) {
        const Vector2D CENTER  = TARGET.box.middle();
        const float    PRIMARY = RIGHT ? CENTER.x - CUR.x : LEFT ? CUR.x - CENTER.x : DOWN ? CENTER.y - CUR.y : CUR.y - CENTER.y;

        // Strictly in the direction asked for, so an arrow can never pick the
        // box it is already on or one behind it.
        if (PRIMARY <= 1.F)
            continue;

        const SScore SCORE{
            .noOverlap = !(LEFT || RIGHT ? TARGET.box.y < current.y + current.height && current.y < TARGET.box.y + TARGET.box.height :
                                           TARGET.box.x < current.x + current.width && current.x < TARGET.box.x + TARGET.box.width),
            .primary   = PRIMARY,
            .secondary = (LEFT || RIGHT) ? std::abs(CENTER.y - CUR.y) : std::abs(CENTER.x - CUR.x),
        };

        if (!best || SCORE.beats(bestScore)) {
            bestScore = SCORE;
            best      = &TARGET;
        }

        // An arrow LEAVES a surface only when that surface has nowhere left to
        // go in that direction. Without this, RIGHT on a full-width result row
        // -- whose centre is the overview's centre, so half the tiles are "to
        // the right" of it -- jumped straight out of the list and into the grid.
        // Crossing between surfaces is still the point of this walk, it just has
        // to be the LAST resort rather than the first.
        if (TARGET.zone == zone && (!near || SCORE.beats(nearScore))) {
            nearScore = SCORE;
            near      = &TARGET;
        }
    }

    return near ? near : best;
}

bool CScrollOverview::moveSelection(const std::string& direction) {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR)
        return false;

    const bool LEFT = direction == "left", RIGHT = direction == "right", UP = direction == "up", DOWN = direction == "down";
    if (!LEFT && !RIGHT && !UP && !DOWN)
        return false;

    // 2D Navigation inside the Results zone
    if (g_search.nav.zone == EOverviewNavZone::Result && !g_search.layout.hits.empty()) {
        const SSearchRowHit* curHit = nullptr;
        for (const auto& hit : g_search.layout.hits) {
            if (hit.index == g_search.nav.selection) {
                curHit = &hit;
                break;
            }
        }

        if (curHit) {
            g_search.nav.explicitSelection = true;
            g_search.nav.intent            = ESearchIntent::Browsing;

            if (UP) {
                if (curHit->rowInCol == 0) {
                    // Return focus to search bar
                    g_search.nav.zone              = EOverviewNavZone::Search;
                    g_search.nav.selection         = -1;
                    g_search.nav.explicitSelection = false;
                    g_search.nav.intent            = ESearchIntent::Typing;
                    damage();
                    return true;
                }
                for (const auto& hit : g_search.layout.hits) {
                    if (hit.column == curHit->column && hit.rowInCol == curHit->rowInCol - 1) {
                        g_search.nav.selection = hit.index;
                        damage();
                        return true;
                    }
                }
                return true;
            }

            if (DOWN) {
                for (const auto& hit : g_search.layout.hits) {
                    if (hit.column == curHit->column && hit.rowInCol == curHit->rowInCol + 1) {
                        g_search.nav.selection = hit.index;
                        damage();
                        return true;
                    }
                }
                return true;
            }

            if (RIGHT) {
                int maxCol = 0;
                for (const auto& hit : g_search.layout.hits)
                    maxCol = std::max(maxCol, hit.column);

                for (int targetCol = curHit->column + 1; targetCol <= maxCol; ++targetCol) {
                    const SSearchRowHit* bestHit = nullptr;
                    int bestDiff = 999;
                    for (const auto& hit : g_search.layout.hits) {
                        if (hit.column == targetCol) {
                            int diff = std::abs(hit.rowInCol - curHit->rowInCol);
                            if (diff < bestDiff) {
                                bestDiff = diff;
                                bestHit  = &hit;
                            }
                        }
                    }
                    if (bestHit) {
                        g_search.nav.selection = bestHit->index;
                        damage();
                        return true;
                    }
                }
                return true;
            }

            if (LEFT) {
                if (curHit->column == 0) {
                    if (OmarchySearch::canNavigateMenuBack()) {
                        OmarchySearch::navigateMenuBack();
                        g_search.nav.selection = 0;
                        damage();
                        return true;
                    }
                    return true;
                }

                for (int targetCol = curHit->column - 1; targetCol >= 0; --targetCol) {
                    const SSearchRowHit* bestHit = nullptr;
                    int bestDiff = 999;
                    for (const auto& hit : g_search.layout.hits) {
                        if (hit.column == targetCol) {
                            int diff = std::abs(hit.rowInCol - curHit->rowInCol);
                            if (diff < bestDiff) {
                                bestDiff = diff;
                                bestHit  = &hit;
                            }
                        }
                    }
                    if (bestHit) {
                        g_search.nav.selection = bestHit->index;
                        damage();
                        return true;
                    }
                }
                return true;
            }
        }
    }

    if ((g_search.nav.zone == EOverviewNavZone::Search || g_search.nav.zone != EOverviewNavZone::Result) && DOWN) {
        if (!g_search.layout.hits.empty()) {
            g_search.nav.zone              = EOverviewNavZone::Result;
            g_search.nav.selection         = g_search.layout.hits[0].index;
            g_search.nav.explicitSelection = true;
            g_search.nav.intent            = ESearchIntent::Browsing;
            damage();
            return true;
        }
    }

    std::vector<SOverviewNavTarget> targets;
    collectNavTargets(MONITOR, targets);

    if (targets.empty())
        return false;

    CBox current = currentNavBox(targets, MONITOR);

    if (current.width <= 1.F || current.height <= 1.F)
        current = targets.front().box;

    const auto* const best = pickNavTarget(targets, current, g_search.nav.zone, LEFT, RIGHT, DOWN);
    if (!best)
        return false;

    switch (best->zone) {
        case EOverviewNavZone::Desktop:
            g_search.nav.zone      = EOverviewNavZone::Desktop;
            g_search.nav.selection = -1;
            g_search.nav.keyboardTile = false;
            TileChrome::setHovered(PHLWINDOW{}, false);
            if (const auto WORKSPACE = images[sc<size_t>(best->index)]->pWorkspace; WORKSPACE)
                focusOverviewWorkspace(WORKSPACE, sc<size_t>(best->index));
            break;
        case EOverviewNavZone::Result:
            g_search.nav.zone              = EOverviewNavZone::Result;
            g_search.nav.selection         = best->index;
            g_search.nav.explicitSelection = true;
            g_search.nav.intent            = ESearchIntent::Browsing;
            g_search.nav.keyboardTile      = false;
            TileChrome::setHovered(PHLWINDOW{}, false);
            break;
        case EOverviewNavZone::Tile:
            g_search.nav.zone         = EOverviewNavZone::Tile;
            g_search.nav.selection    = -1;
            closeOnWindow             = best->window;
            g_search.nav.keyboardTile = true;
            TileChrome::setHovered(best->window, false);
            break;
        case EOverviewNavZone::Search:
            g_search.nav.zone              = EOverviewNavZone::Search;
            g_search.nav.selection         = -1;
            g_search.nav.explicitSelection = false;
            g_search.nav.intent            = ESearchIntent::Typing;
            g_search.nav.keyboardTile      = false;
            TileChrome::setHovered(PHLWINDOW{}, false);
            break;
    }

    damage();
    return true;
}


void CForcedVisibility::forceSurface(SP<CWLSurfaceResource> surface) {
    if (!surface)
        return;

    const auto HLSURFACE = Desktop::View::CWLSurface::fromResource(surface);
    if (!HLSURFACE)
        return;

    for (auto& entry : m_surfaces) {
        if (entry.surface.lock() == surface) {
            HLSURFACE->m_visibleRegion = {};
            return;
        }
    }

    m_surfaces.push_back({surface, HLSURFACE->m_visibleRegion});
    HLSURFACE->m_visibleRegion = {};
}

void CForcedVisibility::forceWindowSurfaces(PHLWINDOW window) {
    if (!window || !window->wlSurface() || !window->wlSurface()->resource())
        return;

    window->wlSurface()->resource()->breadthfirst([this](SP<CWLSurfaceResource> surface, const Vector2D&, void*) { forceSurface(surface); }, nullptr);

    if (window->m_isX11 || !window->m_popupHead)
        return;

    window->m_popupHead->breadthfirst([this](WP<Desktop::View::CPopup> popup, void*) {
        if (!popup || !popup->aliveAndVisible() || !popup->wlSurface() || !popup->wlSurface()->resource())
            return;

        popup->wlSurface()->resource()->breadthfirst([this](SP<CWLSurfaceResource> surface, const Vector2D&, void*) { forceSurface(surface); }, nullptr);
    }, nullptr);
}

void CForcedVisibility::forceWindow(PHLWINDOW window) {
    if (!window)
        return;

    constexpr auto FULLSCREENALPHA = Desktop::View::WINDOW_ALPHA_FULLSCREEN;

    for (auto& entry : m_windows) {
        if (entry.window == window) {
            window->m_hidden = false;
            window->alpha(FULLSCREENALPHA)->setValueAndWarp(1.F);
            return;
        }
    }

    auto& entry                  = m_windows.emplace_back();
    entry.window                 = window;
    entry.hidden                 = window->m_hidden;

    window->m_hidden = false;
    window->alpha(FULLSCREENALPHA)->setValueAndWarp(1.F);
}

void CForcedVisibility::forceLayersAboveFullscreen(PHLMONITOR monitor) {
    if (!monitor)
        return;

    for (const auto LAYER : {ZWLR_LAYER_SHELL_V1_LAYER_TOP, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY}) {
        for (const auto& ls : monitor->m_layerSurfaceLayers[LAYER]) {
            if (!ls)
                continue;

            bool known = false;
            for (auto& entry : m_layers) {
                if (entry.layer == ls) {
                    known = true;
                    break;
                }
            }

			auto& lsAlpha = ls->alpha()[Desktop::View::LS_ALPHA_FADE];
            if (!known)
                m_layers.push_back({ls, ls->m_aboveFullscreen, lsAlpha->value()});

            if (!ls->m_aboveFullscreen)
                ls->m_aboveFullscreen = true;

            if (lsAlpha->value() != 1.F || lsAlpha->goal() != 1.F || lsAlpha->isBeingAnimated())
                lsAlpha->setValueAndWarp(1.F);
        }
    }
}

void CForcedVisibility::restoreSurfaces() {
    for (auto& entry : m_surfaces) {
        const auto SURFACE = entry.surface.lock();
        if (!SURFACE)
            continue;

        const auto HLSURFACE = Desktop::View::CWLSurface::fromResource(SURFACE);
        if (!HLSURFACE)
            continue;

        HLSURFACE->m_visibleRegion = entry.visibleRegion;
    }

    m_surfaces.clear();
}

void CForcedVisibility::restoreWindows() {
    std::vector<SP<Desktop::View::CGroup>> groupsToRefresh;

    for (auto& entry : m_windows) {
        const auto WINDOW = entry.window.lock();
        if (!WINDOW)
            continue;

        constexpr auto FULLSCREENALPHA = Desktop::View::WINDOW_ALPHA_FULLSCREEN;
        WINDOW->updateFullscreenInputState();
        *WINDOW->alpha(FULLSCREENALPHA) = WINDOW->isBlockedByFullscreen() ? 0.F : 1.F;

        if (WINDOW->m_group) {
            if (std::ranges::find(groupsToRefresh, WINDOW->m_group) == groupsToRefresh.end())
                groupsToRefresh.emplace_back(WINDOW->m_group);
            continue;
        }

        WINDOW->m_hidden = entry.hidden;
    }

    for (const auto& group : groupsToRefresh) {
        if (group)
            group->updateWindowVisibility();
    }

    m_windows.clear();
}

void CForcedVisibility::restoreLayers() {
    for (auto& entry : m_layers) {
        if (!entry.layer)
            continue;

        entry.layer->m_aboveFullscreen = entry.aboveFullscreen;

		auto& entryLsAlpha = entry.layer->alpha()[Desktop::View::LS_ALPHA_FADE];
        const auto MONITOR = entry.layer->m_monitor.lock();
        if (!MONITOR) {
            entryLsAlpha->setValueAndWarp(entry.alpha);
            continue;
        }

        const bool fullscreen = Fullscreen::controller()->hasFullscreen(MONITOR);
        const bool visible    = !fullscreen || entry.layer->m_layer >= ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY || entry.layer->m_aboveFullscreen;
        entryLsAlpha->setValueAndWarp(visible ? 1.F : 0.F);
    }

    m_layers.clear();
}

// The order the destructor used to spell out three lines at a time. Idempotent:
// each restore clears its own vector, so calling it again -- or letting the
// destructor call it -- does nothing.
void CForcedVisibility::restoreAll() {
    restoreSurfaces();
    restoreWindows();
    restoreLayers();
}

void CScrollOverview::applyWorkspaceAnimationOverrides() {
    if (!sharedStateOwner || workspaceAnimationsOverridden)
        return;

    savedWorkspaceAnimationConfigs.clear();

    for (const std::string name : {"workspaces", "workspacesIn", "workspacesOut"}) {
        const auto CONFIG = Config::animationTree()->getAnimationPropertyConfig(name);
        if (!CONFIG)
            continue;

        auto& saved   = savedWorkspaceAnimationConfigs.emplace_back();
        saved.name    = name;
        saved.config  = *CONFIG;
    }

    for (const auto& saved : savedWorkspaceAnimationConfigs)
        Config::animationTree()->setConfigForNode(saved.name, false, 1.F, "default", "");

    workspaceAnimationsOverridden = true;
}

void CScrollOverview::restoreWorkspaceAnimationOverrides() {
    if (!workspaceAnimationsOverridden)
        return;

    const auto propagateAnimationValues = [](const SP<Hyprutils::Animation::SAnimationPropertyConfig>& parent, auto&& self) -> void {
        if (!parent)
            return;

        for (const auto& [name, animation] : Config::animationTree()->getAnimationConfig()) {
            if (!animation || animation->overridden || animation->pParentAnimation != parent)
                continue;

            animation->pValues = parent->pValues;
            self(animation, self);
        }
    };

    for (const auto& saved : savedWorkspaceAnimationConfigs) {
        const auto CONFIG = Config::animationTree()->getAnimationPropertyConfig(saved.name);
        if (!CONFIG)
            continue;

        *CONFIG = saved.config;
        propagateAnimationValues(CONFIG, propagateAnimationValues);
    }

    savedWorkspaceAnimationConfigs.clear();
    workspaceAnimationsOverridden = false;
}

void CScrollOverview::forceWorkspaceAlphaVisible(const PHLWORKSPACE& workspace) {
    if (!workspace || !workspace->m_alpha)
        return;

    if (workspace->m_alpha->value() == 1.F && workspace->m_alpha->goal() == 1.F && !workspace->m_alpha->isBeingAnimated())
        return;

    workspace->m_alpha->setValueAndWarp(1.F);
    *workspace->m_alpha = 1.F;
}

void CScrollOverview::forceWorkspaceAlphaVisible() {
    for (const auto& workspace : State::workspaceState()->workspaces())
        forceWorkspaceAlphaVisible(workspace.lock());
}

void CScrollOverview::forceWorkspaceWindowsDecoRecalc(const PHLWORKSPACE& workspace) {
    if (!workspace)
        return;

    const auto workspaceImage = std::ranges::find_if(images, [&workspace](const auto& image) { return image && image->pWorkspace == workspace; });
    if (workspaceImage == images.end())
        return;

    for (const auto& windowRef : (*workspaceImage)->windows)
        OverviewWindow::forceDecoRecalc(windowRef.lock());
}

void CScrollOverview::applyInputConfigOverrides() {
    if (!sharedStateOwner || inputConfigOverridden)
        return;

    previousNoWarps                    = ScrollOverview::Config::getValue<int>("cursor:no_warps");
    previousWarpOnChangeWorkspace      = ScrollOverview::Config::getValue<int>("cursor:warp_on_change_workspace");
    previousWarpOnToggleSpecial        = ScrollOverview::Config::getValue<int>("cursor:warp_on_toggle_special");
    previousWarpBackAfterNonMouseInput = ScrollOverview::Config::getValue<int>("cursor:warp_back_after_non_mouse_input");
    previousFollowMouse                = ScrollOverview::Config::getValue<int>("input:follow_mouse");
    inputConfigOverridden = true;

    ScrollOverview::Config::setValue("cursor:no_warps", 1);
    ScrollOverview::Config::setValue("cursor:warp_on_change_workspace", 0);
    ScrollOverview::Config::setValue("cursor:warp_on_toggle_special", 0);
    ScrollOverview::Config::setValue("cursor:warp_back_after_non_mouse_input", 0);
    ScrollOverview::Config::setValue("input:follow_mouse", 0);
}

void CScrollOverview::restoreInputConfigOverrides() {
    if (!inputConfigOverridden)
        return;

    ScrollOverview::Config::setValue("cursor:no_warps", previousNoWarps);
    ScrollOverview::Config::setValue("cursor:warp_on_change_workspace", previousWarpOnChangeWorkspace);
    ScrollOverview::Config::setValue("cursor:warp_on_toggle_special", previousWarpOnToggleSpecial);
    ScrollOverview::Config::setValue("cursor:warp_back_after_non_mouse_input", previousWarpBackAfterNonMouseInput);
    ScrollOverview::Config::setValue("input:follow_mouse", previousFollowMouse);

    inputConfigOverridden = false;
}

void CScrollOverview::transferSharedStateOwnership() {
    if (!sharedStateOwner)
        return;

    CScrollOverview* successor = nullptr;
    for (const auto& overview : scrollOverviews()) {
        auto* candidate = overview ? dynamic_cast<CScrollOverview*>(overview.get()) : nullptr;
        if (candidate && candidate != this && !candidate->closing) {
            successor = candidate;
            break;
        }
    }

    if (!successor)
        return;

    successor->sharedStateOwner = true;

    successor->savedWorkspaceAnimationConfigs = std::move(savedWorkspaceAnimationConfigs);
    successor->workspaceAnimationsOverridden  = workspaceAnimationsOverridden;
    savedWorkspaceAnimationConfigs.clear();
    workspaceAnimationsOverridden = false;

    successor->previousNoWarps                    = previousNoWarps;
    successor->previousWarpOnChangeWorkspace      = previousWarpOnChangeWorkspace;
    successor->previousWarpOnToggleSpecial        = previousWarpOnToggleSpecial;
    successor->previousWarpBackAfterNonMouseInput = previousWarpBackAfterNonMouseInput;
    successor->previousFollowMouse                = previousFollowMouse;
    successor->inputConfigOverridden              = inputConfigOverridden;
    inputConfigOverridden = false;

    successor->usesSubmapKeybinds   = usesSubmapKeybinds;
    successor->submapActive         = submapActive;
    successor->previousSubmapName   = std::move(previousSubmapName);
    if (successor->usesSubmapKeybinds)
        successor->keyboardKeyHook.reset();
    usesSubmapKeybinds = false;
    submapActive       = false;
    sharedStateOwner   = false;
}

void CScrollOverview::emitFullscreenVisibilityState(PHLWINDOW window, bool hideFullscreen) {
    if (emittingFullscreenVisibilityState)
        return;

    emittingFullscreenVisibilityState = true;
    auto resetEmittingFullscreenVisibilityState = Hyprutils::Utils::CScopeGuard([this] { emittingFullscreenVisibilityState = false; });

    window = getOverviewWindowToShow(window);

    if (!validMapped(window) || !window->m_workspace || window->m_monitor != pMonitor) {
        g_pEventManager->postEvent(SHyprIPCEvent{.event = "fullscreen", .data = "0"});
        return;
    }

    if (!hideFullscreen || !Fullscreen::controller()->isFullscreen(window)) {
        Event::bus()->m_events.window.fullscreen.emit(window);

        g_pEventManager->postEvent(SHyprIPCEvent{.event = "fullscreen", .data = Fullscreen::controller()->isFullscreen(window) ? "1" : "0"});

        return;
    }

    const auto SAVEDMODES = Fullscreen::controller()->getFullscreenModes(window);

    Fullscreen::controller()->setFullscreenMode(window, Fullscreen::FSMODE_NONE, Fullscreen::FSMODE_NONE);

    Event::bus()->m_events.window.fullscreen.emit(window);

    g_pEventManager->postEvent(SHyprIPCEvent{.event = "fullscreen", .data = "0"});

    Fullscreen::controller()->setFullscreenMode(window, SAVEDMODES.internal, SAVEDMODES.client);
}

void CScrollOverview::renderWorkspaceBackground(PHLMONITOR monitor, size_t workspaceIdx, size_t activeIdx, float workspacePitch, float renderScale, int wallpaperMode,
                                                const Time::steady_tp& now) {
    const auto& workspaceImage = images[workspaceIdx];
    if (!workspaceImage || !workspaceImage->pWorkspace)
        return;

    const auto WORKSPACEOFFSET = workspaceOverviewOffset(workspaceIdx, activeIdx, workspacePitch);
    const auto WORKSPACEBOX     = getOverviewWorkspaceBox(monitor, renderScale, viewOffset->value(), WORKSPACEOFFSET, layout);
    const auto WORKSPACEALPHA   = workspaceOverviewAlpha(workspaceIdx);

    if (!overviewBoxIntersectsMonitor(WORKSPACEBOX, monitor))
        return;

    const auto workspace         = workspaceImage->pWorkspace;
    const bool WASVISIBLE        = workspace->m_visible;
    const bool WASFORCERENDERING = workspace->m_forceRendering;
    workspace->m_visible         = true;
    workspace->m_forceRendering  = true;

    auto restoreWorkspaceState = Hyprutils::Utils::CScopeGuard([workspace, WASVISIBLE, WASFORCERENDERING] {
        workspace->m_visible        = WASVISIBLE;
        workspace->m_forceRendering = WASFORCERENDERING;
    });

    renderOverviewWorkspaceShadow(monitor, WORKSPACEBOX, renderScale, wallpaperMode == 0, WORKSPACEALPHA);

    // X86: workspace cards do NOT get their own live COverviewBlurPassElement.
    // Windows on the card use the same renderWindow live frost path as main tiles
    // (decoration:blur:*, blockBlurOptimization when needed). A whole-card live
    // blur was a different technique and compounded once per visible desktop,
    // so empty-search (many cards) frosted harder than search mode for free.
    // Wallpaper stays the card fill; glass comes from the windows themselves.

    if (wallpaperMode != 0 && WORKSPACEALPHA > 0.001F)
        renderWallpaperLayers(monitor, WORKSPACEBOX, renderScale, now, WORKSPACEALPHA);

    // GNOME-style rounding (user-directed): composite the native-size wallpaper
    // FB into the card box with shader round/roundingPower. The corners are
    // genuinely clipped and antialiased against whatever is behind them --
    // shadow included -- which no painted-on mask could match exactly.
    if (WORKSPACEALPHA > 0.001F && frost.cardWallpaperFB && frost.cardWallpaperFB->isAllocated() && frost.cardWallpaperFB->getTexture()) {
        const auto    CARDROUND = getOverviewCardRounding(monitor, ScrollOverview::Config::getScale());
        const auto    BOXPX     = WORKSPACEBOX.copy().round();
        const CRegion DMG{BOXPX};

        g_pHyprRenderer->draw(
            CTexPassElement::SRenderData{
                .tex           = frost.cardWallpaperFB->getTexture(),
                .box           = BOXPX,
                .a             = WORKSPACEALPHA,
                .damage        = DMG,
                .round         = CARDROUND.rounding,
                .roundingPower = CARDROUND.power,
                .flipEndFrame  = true,
            },
            DMG);
    } else if (WORKSPACEALPHA > 0.001F)
        renderOverviewLayerLevel(monitor, ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM, WORKSPACEBOX, renderScale, now);

    // Outline the floating card so it separates from the dimmed canvas behind
    // it. Same accent the tile ring and strip use; same weight as Hyprland
    // window borders, floored at 1 so a theme with border_size 0 still frames
    // the card.
    if (WORKSPACEALPHA > 0.001F) {
        const auto CARDROUND   = getOverviewCardRounding(monitor, ScrollOverview::Config::getScale());
        const int  BORDERSIZE  = std::max(1, ScrollOverview::Config::getValue<int>("general:border_size"));
        const auto ACCENT      = ScrollOverview::Config::highlightColor();

        CBorderPassElement::SBorderData border;
        border.box           = WORKSPACEBOX.copy().round();
        border.grad1.reset(ACCENT.modifyA(1.F));
        border.a             = WORKSPACEALPHA;
        border.borderSize    = BORDERSIZE;
        border.round         = CARDROUND.rounding;
        border.roundingPower = CARDROUND.power;
        g_pHyprRenderer->m_renderPass.add(makeUnique<CBorderPassElement>(border));
    }
}

void CScrollOverview::renderWorkspaceLive(PHLMONITOR monitor, size_t workspaceIdx, size_t activeIdx, float workspacePitch, float renderScale, int wallpaperMode, const Time::steady_tp& now) {
    const auto& workspaceImage = images[workspaceIdx];
    if (!workspaceImage || !workspaceImage->pWorkspace)
        return;

    const auto WORKSPACEOFFSET = workspaceOverviewOffset(workspaceIdx, activeIdx, workspacePitch);
    const auto WORKSPACEBOX     = getOverviewWorkspaceBox(monitor, renderScale, viewOffset->value(), WORKSPACEOFFSET, layout);
    const auto VISIBLEBOX       = workspaceOverviewVisibleBox(workspaceIdx, WORKSPACEBOX, renderScale, monitor);
    const bool WORKSPACEVISIBLE = overviewBoxIntersectsMonitor(VISIBLEBOX, monitor);

    // B2: a promoted tile is drawn here, in its own workspace's pass — that is
    // where the m_visible / m_forceRendering override below lives — but at the
    // active card's offset. So an off-screen workspace still has to be walked.
    const auto ACTIVECARDBOX = getOverviewWorkspaceBox(monitor, renderScale, viewOffset->value(), 0.F, layout);
    const bool HASPROMOTED   = !WORKSPACEVISIBLE && std::ranges::any_of(workspaceImage->windows, [&](const auto& ref) {
        return overviewGridPromoted(getOverviewWindowToShow(ref.lock()), monitor);
    });

    if (!WORKSPACEVISIBLE && !HASPROMOTED)
        return;

    const auto workspace         = workspaceImage->pWorkspace;
    const bool WASVISIBLE        = workspace->m_visible;
    const bool WASFORCERENDERING = workspace->m_forceRendering;
    workspace->m_visible         = true;
    workspace->m_forceRendering  = true;

    auto restoreWorkspaceState = Hyprutils::Utils::CScopeGuard([workspace, WASVISIBLE, WASFORCERENDERING] {
        workspace->m_visible        = WASVISIBLE;
        workspace->m_forceRendering = WASFORCERENDERING;
    });

    // X7/X8 chrome is drawn in a SECOND pass. The app icon straddles the tile's
    // bottom edge and a hovered tile grows into its neighbours, so chrome drawn
    // with its own tile gets painted over by the next one.
    std::vector<std::pair<PHLWINDOW, CBox>> tileChrome;

    const auto renderOverviewWindow = [&](const PHLWINDOW& window) {
        // F3-b outbound: a releasing tile no longer matches, so the search half
        // of shouldShowOverviewWindow would delete it on its first outbound
        // frame. overviewGridReleasing only admits a window that was legally
        // shown a moment ago and is still validMapped.
        //
        // ONE predicate, and windowAtOverviewPoint asks the same one -- a tile
        // this function paints is a tile that function can hand back.
        bool PROMOTED = false;
        if (!overviewTileShown(window, monitor, &PROMOTED))
            return;
        if (drag.activeWindow && window == getOverviewWindowToShow(drag.activeWindow.lock()))
            return;

        if (!WORKSPACEVISIBLE && !PROMOTED)
            return;

        const auto windowBox = TileChrome::scaledBox(window, getOverviewWindowBox(window, monitor, renderScale, viewOffset->value(), WORKSPACEOFFSET, layout));
        if (!overviewBoxIntersectsMonitor(windowBox, monitor))
            return;

        // INSTRUMENT (Membership::renderShadowEnabled). PAST the cull, so a line
        // here means a rectangle really was painted on this output.
        if (Membership::renderShadowEnabled()) {
            const auto LIVEWS = window->m_workspace ? window->m_workspace->m_id : WORKSPACE_INVALID;
            const auto CENTRE = windowBox.middle();
            // Would the hit-test hand this tile back? Same walk windowAtOverviewPoint does.
            const auto HIT    = windowAtOverviewPoint(CENTRE);
            if (HIT != window)
                Membership::renderDivergence("UNCLICK", workspace->m_id, LIVEWS, window->m_class,
                                             window->m_title + std::format("  box={},{} {}x{}  hit={}", (int)windowBox.x, (int)windowBox.y, (int)windowBox.width,
                                                                           (int)windowBox.height, HIT ? HIT->m_class : std::string{"(none)"}));
            if (PROMOTED && g_search.query.text.empty())
                Membership::renderDivergence("PROMONOQ", workspace->m_id, LIVEWS, window->m_class,
                                             window->m_title + std::format("  box={},{} {}x{}", (int)windowBox.x, (int)windowBox.y, (int)windowBox.width, (int)windowBox.height));
        }

        renderWindowLive(monitor, window, windowBox, renderScale, now, PROMOTED ? &ACTIVECARDBOX : &WORKSPACEBOX);
        tileChrome.emplace_back(window, windowBox);
    };

    const auto renderTileChrome = [&] {
        // The same progress the pill, the strip and the result cards fade on,
        // so the whole overview arrives and leaves as one thing.
        const float CHROMEFADE = overviewGridProgress(renderScale);
        for (const auto& [window, box] : tileChrome)
            TileChrome::render(monitor, window, box, monitor->m_scale, CHROMEFADE);
    };

    const auto fullscreenWindow = getOverviewWindowToShow(Fullscreen::controller()->getFullscreenWindow(workspace));
    const bool scrollingLayout   = isWorkspaceScrolling(workspace);
    const bool hasFullscreenPath = shouldShowOverviewWindow(fullscreenWindow) && fullscreenWindow->m_workspace == workspace;
    const auto renderDropIndicator = [&] {
        if (hasRunningWorkspaceAnimation())
            return;

        auto*      dragContext = g_pointerGrabOverview && g_pointerGrabOverview->drag.activeWindow ? g_pointerGrabOverview : this;
        const auto DRAGGED     = getOverviewWindowToShow(dragContext->drag.activeWindow.lock());
        if (!DRAGGED)
            return;

        if (!isOverviewPointerOnMonitor(monitor))
            return;

        size_t     dropWorkspaceIdx = 0;
        const auto DROPWORKSPACE    = workspaceAtOverviewDropPoint(lastMousePosLocal, &dropWorkspaceIdx, DRAGGED);
        if (DROPWORKSPACE != workspace || dropWorkspaceIdx != workspaceIdx)
            return;

        const auto ANCHOR      = dropAnchorAtOverviewCursorOnWorkspace(workspaceIdx, DRAGGED, dragContext);
        const auto WORKSPACEBOX = getOverviewWorkspaceBox(monitor, renderScale, viewOffset->value(), WORKSPACEOFFSET, layout);

        CDropIndicator::renderDropIndicator({
            .monitor               = monitor,
            .workspace             = workspace,
            .workspaceUsableBox    = getOverviewWorkspaceUsableBox(workspace, monitor, renderScale, viewOffset->value(), WORKSPACEOFFSET, layout),
            .anchor                = ANCHOR,
            .renderScale           = renderScale,
            .workspaceFullyVisible = overviewBoxFullyVisibleOnMonitor(WORKSPACEBOX, monitor),
            .floating              = DRAGGED->m_isFloating,
            .layout                = layout,
        });
    };

    if (!scrollingLayout && hasFullscreenPath) {
        renderOverviewWindow(fullscreenWindow);
        OverviewRender::flushPass(monitor);
        for (const auto& windowRef : workspaceImage->windows) {
            const auto window = getOverviewWindowToShow(windowRef.lock());
            if (!shouldShowOverviewWindow(window) || !window->m_isFloating || window == fullscreenWindow)
                continue;

            renderOverviewWindow(window);
        }
        renderTileChrome();
        if (WORKSPACEVISIBLE)
            renderDropIndicator();
        return;
    }

    const auto renderWindowsByState = [&](bool floating) {
        for (const auto& windowRef : workspaceImage->windows) {
            const auto window = getOverviewWindowToShow(windowRef.lock());
            if (!window || window->m_isFloating != floating)
                continue;

            renderOverviewWindow(window);
        }
    };

    renderWindowsByState(false);
    renderWindowsByState(true);
    renderTileChrome();
    if (WORKSPACEVISIBLE)
        renderDropIndicator();
}

float CScrollOverview::workspaceScrollingSpanRatio(const PHLWORKSPACE& workspace, PHLMONITOR monitor) const {
    if (!workspace || !monitor || monitor->m_size.x <= 0.F || monitor->m_size.y <= 0.F)
        return 1.0F;

    if (!isWorkspaceScrolling(workspace))
        return 1.0F;

    const auto ALGO = overviewScrollingAlgorithmForWorkspace(workspace);
    if (!ALGO || !ALGO->m_scrollingData || !ALGO->m_scrollingData->controller)
        return 1.0F;

    if (!ALGO->m_scrollingData->controller->isPrimaryHorizontal())
        return 1.0F;

    const auto USABLE = ALGO->usableArea();
    const auto CAMERA_TRANS = ALGO->m_scrollingData->controller->getCameraTranslation(USABLE);

    const SWorkspaceImage* wimgPtr = nullptr;
    for (const auto& img : images) {
        if (img && img->pWorkspace == workspace) {
            wimgPtr = img.get();
            break;
        }
    }

    float minX = sc<float>(monitor->m_position.x) + sc<float>(monitor->m_size.x);
    float maxX = sc<float>(monitor->m_position.x);
    size_t tiledCount = 0;

    if (wimgPtr && !wimgPtr->windows.empty()) {
        for (const auto& winRef : wimgPtr->windows) {
            const auto WINDOW = getOverviewWindowToShow(winRef.lock());
            if (!WINDOW || !overviewStripMiniShown(WINDOW, workspace) || WINDOW->m_isFloating)
                continue;

            const auto GEOM = WINDOW->geometricBox(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
            const float LEFT = sc<float>(GEOM.x) - sc<float>(CAMERA_TRANS.x);
            const float RIGHT = LEFT + sc<float>(GEOM.w);

            minX = std::min(minX, LEFT);
            maxX = std::max(maxX, RIGHT);
            tiledCount++;
        }
    } else {
        for (const auto& w : Desktop::windowState()->windows()) {
            const auto WINDOW = getOverviewWindowToShow(w);
            if (!WINDOW || WINDOW->m_workspace != workspace || !overviewStripMiniShown(WINDOW, workspace) || WINDOW->m_isFloating)
                continue;

            const auto GEOM = WINDOW->geometricBox(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
            const float LEFT = sc<float>(GEOM.x) - sc<float>(CAMERA_TRANS.x);
            const float RIGHT = LEFT + sc<float>(GEOM.w);

            minX = std::min(minX, LEFT);
            maxX = std::max(maxX, RIGHT);
            tiledCount++;
        }
    }

    if (tiledCount == 0)
        return 1.0F;

    float virtualRight = maxX;
    const double MAXW = ALGO->m_scrollingData->maxWidth();
    if (MAXW > 0.0) {
        const float TAPE_RIGHT = sc<float>(USABLE.x) + sc<float>(MAXW);
        virtualRight = std::max(virtualRight, TAPE_RIGHT);
    }

    const float LEFT_MARGIN = std::max(0.F, minX - sc<float>(monitor->m_position.x));
    const float TOTAL_VIRTUAL_WIDTH = (virtualRight - sc<float>(monitor->m_position.x)) + LEFT_MARGIN;

    if (TOTAL_VIRTUAL_WIDTH <= sc<float>(monitor->m_size.x))
        return 1.0F;

    return TOTAL_VIRTUAL_WIDTH / sc<float>(monitor->m_size.x);
}

CBox CScrollOverview::stripCellLogicalBox(size_t workspaceIdx) const {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR || workspaceIdx >= images.size() || images.empty())
        return {};

    const float BAND = overviewStripLogicalHeight(MONITOR);
    if (BAND <= 0.F)
        return {};

    const float  PAD    = BAND * OVERVIEW_STRIP_BAND_PAD_FRAC;
    const float  GAP    = sc<float>(ScrollOverview::Config::getStripGap());
    const float  ASPECT = MONITOR->m_size.y > 1 ? sc<float>(MONITOR->m_size.x) / sc<float>(MONITOR->m_size.y) : 1.6F;
    const size_t COUNT  = images.size();

    float cellH = BAND - 2.F * PAD;
    const float baseW = cellH * ASPECT;

    std::vector<float> cellWidths(COUNT, baseW);
    float sumCellW = 0.F;

    for (size_t i = 0; i < COUNT; ++i) {
        const float RATIO = images[i] ? workspaceScrollingSpanRatio(images[i]->pWorkspace, MONITOR) : 1.0F;
        cellWidths[i] = baseW * RATIO;
        sumCellW += cellWidths[i];
    }

    float total = sumCellW + GAP * sc<float>(COUNT - 1);

    // Many workspaces must shrink the cells rather than overflow the monitor.
    if (const float MAXW = sc<float>(MONITOR->m_size.x) - 2.F * PAD; total > MAXW && sumCellW > 0.F) {
        const float SCALE_DOWN = (MAXW - GAP * sc<float>(COUNT - 1)) / sumCellW;
        cellH *= SCALE_DOWN;
        for (size_t i = 0; i < COUNT; ++i)
            cellWidths[i] *= SCALE_DOWN;
        total = MAXW;
    }

    if (cellWidths[workspaceIdx] <= 1.F || cellH <= 1.F)
        return {};

    float currentX = sc<float>(MONITOR->m_position.x) + (sc<float>(MONITOR->m_size.x) - total) / 2.F;
    for (size_t i = 0; i < workspaceIdx; ++i)
        currentX += cellWidths[i] + GAP;

    // X10: the search row owns the very top; the band starts under it.
    // X60: and the RESULTS sit between the two, so a growing result list pushes
    // the strip down rather than being anchored to the bottom half. This
    // reverses X48's arrangement (and with it DoD 18's "the strip never moves
    // under a query") on the user's instruction of 2026-08-05.
    const float Y = sc<float>(MONITOR->m_position.y) + overviewSearchLogicalHeight(MONITOR) + overviewResultsLogicalHeight(MONITOR) + PAD +
        (BAND - 2.F * PAD - cellH) / 2.F;

    return CBox{currentX, Y, cellWidths[workspaceIdx], cellH};
}

// X22: screen-fixed. The strip used to ride the active card, which meant a
// desktop pan carried it clean off the monitor along with the search bar.
CBox CScrollOverview::stripCellBox(size_t workspaceIdx, PHLMONITOR monitor, float renderScale) const {
    const auto LOGICAL = stripCellLogicalBox(workspaceIdx);
    if (!monitor || LOGICAL.width <= 0)
        return {};

    return getOverviewGlobalBox(LOGICAL, monitor, renderScale, OVERVIEW_CHROME_NO_PAN, 0.F, layout);
}

PHLWORKSPACE CScrollOverview::workspaceAtStripPoint(const Vector2D& point, size_t* hoveredWorkspaceIdx) const {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR || overviewStripLogicalHeight(MONITOR) <= 0.F)
        return nullptr;

    for (size_t workspaceIdx = 0; workspaceIdx < images.size(); ++workspaceIdx) {
        const auto& wimg = images[workspaceIdx];
        if (!wimg || !wimg->pWorkspace)
            continue;

        if (!stripCellBox(workspaceIdx, MONITOR, scale->value()).containsPoint(point))
            continue;

        if (hoveredWorkspaceIdx)
            *hoveredWorkspaceIdx = workspaceIdx;

        return wimg->pWorkspace;
    }

    return nullptr;
}

PHLWORKSPACE CScrollOverview::stripWorkspaceAt(const Vector2D& point) const {
    return workspaceAtStripPoint(point);
}

// Minis map the window's REAL desktop rect into the cell, not its grid slot: a
// thumbnail of the desktop, not a second copy of the grid. Shared by the render
// and the D6 hit-test so the two can never disagree about where a mini is.
CBox CScrollOverview::stripMiniBox(const CBox& cell, PHLWINDOW window, PHLMONITOR monitor) const {
    if (!window || !monitor || cell.width <= 0 || cell.height <= 0)
        return {};

    const float SCALEY = sc<float>(cell.height) / std::max(1.F, sc<float>(monitor->m_size.y));
    const auto  GEOM   = window->geometricBox(Desktop::View::IGeometric::GEOMETRIC_CURRENT);

    const auto WORKSPACE = window->m_workspace;
    const float SPAN_RATIO = workspaceScrollingSpanRatio(WORKSPACE, monitor);

    if (SPAN_RATIO > 1.0F) {
        const auto ALGO = overviewScrollingAlgorithmForWorkspace(WORKSPACE);
        Vector2D cameraTrans{};
        if (ALGO && ALGO->m_scrollingData && ALGO->m_scrollingData->controller)
            cameraTrans = ALGO->m_scrollingData->controller->getCameraTranslation(ALGO->usableArea());

        const float VIRTUAL_WIDTH = sc<float>(monitor->m_size.x) * SPAN_RATIO;
        const float SCALEX = sc<float>(cell.width) / std::max(1.F, VIRTUAL_WIDTH);
        const float UNTRANSLATED_X = sc<float>(GEOM.x) - sc<float>(cameraTrans.x);

        return CBox{
            sc<float>(cell.x) + (UNTRANSLATED_X - sc<float>(monitor->m_position.x)) * SCALEX,
            sc<float>(cell.y) + (sc<float>(GEOM.y) - sc<float>(monitor->m_position.y)) * SCALEY,
            sc<float>(GEOM.w) * SCALEX,
            sc<float>(GEOM.h) * SCALEY,
        };
    }

    const float SCALEX = sc<float>(cell.width) / std::max(1.F, sc<float>(monitor->m_size.x));
    return CBox{
        sc<float>(cell.x) + (sc<float>(GEOM.x) - sc<float>(monitor->m_position.x)) * SCALEX,
        sc<float>(cell.y) + (sc<float>(GEOM.y) - sc<float>(monitor->m_position.y)) * SCALEY,
        sc<float>(GEOM.w) * SCALEX,
        sc<float>(GEOM.h) * SCALEY,
    };
}

// D6: which window mini is under the cursor, topmost first.
PHLWINDOW CScrollOverview::windowAtStripPoint(const Vector2D& point, CBox* miniBox) const {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR || overviewStripLogicalHeight(MONITOR) <= 0.F)
        return nullptr;

    for (size_t workspaceIdx = 0; workspaceIdx < images.size(); ++workspaceIdx) {
        const auto& wimg = images[workspaceIdx];
        if (!wimg || !wimg->pWorkspace)
            continue;

        const auto CELL = stripCellBox(workspaceIdx, MONITOR, scale->value());
        if (CELL.width <= 1 || !CELL.containsPoint(point))
            continue;

        for (auto it = wimg->windows.rbegin(); it != wimg->windows.rend(); ++it) {
            const auto WINDOW = getOverviewWindowToShow(it->lock());
            if (!overviewStripMiniShown(WINDOW, wimg->pWorkspace))
                continue;

            const auto MINI = stripMiniBox(CELL, WINDOW, MONITOR);
            if (MINI.width < 2 || MINI.height < 2 || !MINI.containsPoint(point))
                continue;

            if (miniBox)
                *miniBox = MINI;

            return WINDOW;
        }

        return nullptr;
    }

    return nullptr;
}

// X3: the reference wraps the whole strip in ONE rounded container panel
// (ii/modules/ii/overview/OverviewWidget.qml:126-135) -- padding 10, radius
// largeWorkspaceRadius + padding, a translucent surface colour. Drawn once,
// before the cells, so the cells read as sitting on it.
void CScrollOverview::renderStripPanel(PHLMONITOR monitor, float renderScale, const SStripStyle& STYLE) {
    const float FADE  = STYLE.fade;
    const int   ROUND = STYLE.round;

    if (!images.empty()) {
        const auto FIRSTLOGICAL = stripCellLogicalBox(0);
        const auto LASTLOGICAL  = stripCellLogicalBox(images.size() - 1);

        if (FIRSTLOGICAL.width > 1 && LASTLOGICAL.width > 1) {
            // ponytail: absolute logical px, not a fraction of the cell. E8-c
            // already settled that the theme's absolute radii are used unshrunk;
            // a padding derived from them has to follow the same rule or the
            // panel's curve stops being concentric with the corner cells.
            constexpr float PANELPAD = OVERVIEW_STRIP_PANELPADL;

            const CBox PANELLOGICAL = {
                FIRSTLOGICAL.x - PANELPAD,
                FIRSTLOGICAL.y - PANELPAD,
                sc<float>(LASTLOGICAL.x) + sc<float>(LASTLOGICAL.width) - sc<float>(FIRSTLOGICAL.x) + 2.F * PANELPAD,
                sc<float>(FIRSTLOGICAL.height) + 2.F * PANELPAD,
            };

            const auto PANEL = getOverviewGlobalBox(PANELLOGICAL, monitor, renderScale, OVERVIEW_CHROME_NO_PAN, 0.F, layout);

            if (PANEL.width > 1 && overviewBoxIntersectsMonitor(PANEL, monitor)) {
                // Radius in RENDER space = the cells' radius plus the padding as
                // it actually landed after the transform. Measuring the gap off
                // the transformed boxes avoids duplicating the scale maths.
                const auto FIRSTCELL = stripCellBox(0, monitor, renderScale);
                // Concentric with the cells only when the theme actually rounds.
                // Omarchy's squared default (decoration:rounding 0) must stay
                // squared; inflating by the padding used to force a curve.
                const int PADPX      = std::max(0, sc<int>(std::round(sc<float>(FIRSTCELL.x) - sc<float>(PANEL.x))));
                const int PANELROUND = ROUND <= 0 ? 0 : ROUND + PADPX;

                // Same plate as the dock (Omarchy terminal fill, accent border,
                // frost only if window blur is on). Rounding is the theme's,
                // not the dock's Dash-to-Dock 24.
                OverviewRender::themePlate(PANEL, PANELROUND, STYLE.power, FADE, true);
            }
        }
    }
}

// X3: the reference rounds only the GRID's outer corners at the large
// radius and every inner corner at verysmall (OverviewWidget.qml:171-175).
// The strip is one row, so cell 0 is large on its left corners, the last
// cell large on its right, and every facing corner is tight.
//
// CRectPassElement carries ONE radius, so the asymmetry is drawn as two
// rects of the SAME box clipped to opposite halves. The clips do not
// overlap, which is the point: a translucent backdrop drawn twice over the
// same pixel would blend to the wrong alpha.
void CScrollOverview::renderStripCellRect(const CBox& cell, const CHyprColor& color, bool largeLeft, bool largeRight, const SStripStyle& STYLE) const {
    if (largeLeft == largeRight) {
        CRectPassElement::SRectData r;
        r.box           = cell;
        r.color         = color;
        r.round         = largeLeft ? STYLE.round : STYLE.innerRound;
        r.roundingPower = STYLE.power;
        g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(r));
        return;
    }

    const float MID = sc<float>(cell.x) + std::round(sc<float>(cell.width) / 2.F);

    for (int half = 0; half < 2; ++half) {
        const bool LEFT = half == 0;

        CRectPassElement::SRectData r;
        r.box           = cell;
        r.color         = color;
        r.round         = (LEFT ? largeLeft : largeRight) ? STYLE.round : STYLE.innerRound;
        r.roundingPower = STYLE.power;
        r.clipBox       = LEFT ? CBox{sc<float>(cell.x), sc<float>(cell.y), MID - sc<float>(cell.x), sc<float>(cell.height)} :
                                 CBox{MID, sc<float>(cell.y), sc<float>(cell.x) + sc<float>(cell.width) - MID, sc<float>(cell.height)};
        g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(r));
    }
}

    // X3: a 2px accent border on a cell, the reference's border.width 2. Used
    // both for the focused desktop (OverviewWidget.qml:432-433) and for the
    // drag-hover state (OverviewWidget.qml:179-180) -- one function so the two
    // cannot drift apart.
    // BUG (user, 2026-08-02): this used ONE radius for every cell, so the
    // outline traced a shape the cell underneath does not have -- large curves
    // over the tight facing corners of an inner cell, and the wrong radius on
    // the inside edge of each end cell. It takes the SAME largeLeft/largeRight
    // the fill does, and COverviewSplitBorderPassElement draws the asymmetric
    // case as two damage-clipped halves.
    // (user, 2026-08-03): the stroke sits on the cell's INNER edge, not around
    // its outside.
    // X56 (user, 2026-08-06): and it is the THEME's accent again, not the flat
    // white that instruction had also asked for -- the strip outline, the dock
    // highlight and the tile ring now all read general:col.active_border, so a
    // theme switch moves them together. The 2026-08-03 white is superseded
    // here deliberately; do not restore it as a regression fix.
void CScrollOverview::renderStripCellBorder(PHLMONITOR monitor, const CBox& cell, bool largeLeft, bool largeRight, const SStripStyle& STYLE) const {
    const auto OUTLINE = ScrollOverview::Config::highlightColor();
    const int  BORDER  = std::max(1, ScrollOverview::Config::getValue<int>("general:border_size"));
    // Inset by the full stroke width: renderBorder centres nothing, it grows
    // outward from the box, so the box has to be the INNER rectangle for the
    // stroke to land inside the tile. Radii shrink with it or the curve would
    // no longer be concentric with the fill's.
    const CBox INNER = cell.copy().expand(-BORDER);

    g_pHyprRenderer->m_renderPass.add(makeUnique<COverviewSplitBorderPassElement>(COverviewSplitBorderPassElement::SData{
        .box           = INNER,
        .color         = ::Config::CGradientValueData{OUTLINE.modifyA(1.F)},
        .roundLeft     = std::max(0, (largeLeft ? STYLE.round : STYLE.innerRound) - BORDER),
        .roundRight    = std::max(0, (largeRight ? STYLE.round : STYLE.innerRound) - BORDER),
        .roundingPower = STYLE.power,
        .borderSize    = BORDER,
        .a             = STYLE.fade,
    }));
}

// One strip cell: backdrop, workspace number, the minis of every window on it,
// and the hover/active outlines. Drawn in this order for a reason -- the
// outlines have to land on top of a cell full of previews.
void CScrollOverview::renderStripCell(PHLMONITOR monitor, size_t workspaceIdx, const CBox& CELL, const PHLWINDOW& DRAGGED, bool hovered, float renderScale,
                                      const Time::steady_tp& now, const SStripStyle& STYLE) {
    const auto& wimg = images[workspaceIdx];
    const float FADE = STYLE.fade;

    renderOverviewWorkspaceShadow(monitor, CELL, renderScale, false, FADE);

    const bool ATLEFT  = workspaceIdx == 0;
    const bool ATRIGHT = workspaceIdx + 1 == images.size();

    // Resting fill is the strip panel (the same Omarchy terminal plate as the
    // dock). Only hover tints the cell itself.

    // X3: the deprecated overview centres a large, faint workspace number in
    // every cell (ii/modules/ii/overview/OverviewWidget.qml:182-193): 250px
    // type at the overview's own scale, i.e. 250/1600 = 15.6% of a cell's
    // height, DemiBold, foreground at alpha 0.2. Reproduced as a fraction of
    // the strip cell so it tracks the strip instead of a fixed pixel size.
    const auto NUMTEX = OverviewRender::text(std::to_string(wimg->pWorkspace->m_id), ScrollOverview::Config::themeForeground().modifyA(0.20F),
                                                    std::max(1, sc<int>(std::round(sc<float>(CELL.height) * 0.156F))), false, "", 0, 600);
    if (NUMTEX && NUMTEX->ok()) {
        CBox numBox = {
            CELL.x + std::round((sc<float>(CELL.width) - sc<float>(NUMTEX->m_size.x)) / 2.F),
            CELL.y + std::round((sc<float>(CELL.height) - sc<float>(NUMTEX->m_size.y)) / 2.F),
            sc<float>(NUMTEX->m_size.x),
            sc<float>(NUMTEX->m_size.y),
        };

        CTexPassElement::SRenderData num;
        num.tex = NUMTEX;
        num.box = numBox.round();
        num.a   = FADE;
        g_pHyprRenderer->m_renderPass.add(makeUnique<CTexPassElement>(num));
    }

    const float MINIRENDERSCALE = renderScale * sc<float>(CELL.height) / std::max(1.F, sc<float>(monitor->m_size.y) * monitor->m_scale);

    for (const auto& windowRef : wimg->windows) {
        const auto WINDOW = getOverviewWindowToShow(windowRef.lock());
        if (!overviewStripMiniShown(WINDOW, wimg->pWorkspace) || WINDOW == DRAGGED)
            continue;

        const auto MINI = stripMiniBox(CELL, WINDOW, monitor);

        if (MINI.width < 2 || MINI.height < 2 || MINI.intersection(CELL).empty())
            continue;

        renderWindowLive(monitor, WINDOW, MINI.copy().round(), MINIRENDERSCALE, now, &CELL, false, FADE);
    }

    if (hovered) {
        // X4: the hover tint is themed, from the same general:col.active_border
        // that DropIndicator already reads. White is only the fallback for a
        // theme that defines no gradient.
        const auto ACCENT = ScrollOverview::Config::highlightColor();

        renderStripCellRect(CELL, ACCENT.modifyA(0.22F * FADE), ATLEFT, ATRIGHT, STYLE);

        // X3 delta 3: the reference borders the hovered cell as well as
        // tinting it. Drawn after the tint and after the minis, so the drop
        // target reads as a target even over a cell full of previews.
        renderStripCellBorder(monitor, CELL, ATLEFT, ATRIGHT, STYLE);
    }

    // X3: the reference marks the focused desktop with a 2px accent border
    // drawn over the windows (OverviewWidget.qml:414-436, colSecondary).
    // The accent here is general:col.active_border -- the same source the
    // hover tint above reads, so both follow the theme (X4).
    // ponytail: no slide between cells. The reference animates x/y because
    // its indicator is one moving item; here the cell rects are already
    // laid out, so the border simply draws on whichever cell is active.
    if (monitor->m_activeWorkspace && wimg->pWorkspace->m_id == monitor->m_activeWorkspace->m_id) {
        renderStripCellBorder(monitor, CELL, ATLEFT, ATRIGHT, STYLE);
    }
}

// The strip: one container panel, then one pass per cell. The style the two
// share is resolved once here; nothing else about the order moved.
void CScrollOverview::renderStrip(PHLMONITOR monitor, float renderScale, const Time::steady_tp& now) {
    if (!monitor || overviewStripLogicalHeight(monitor) <= 0.F)
        return;

    // User, 2026-08-06: the strip FADES in, and a toggle halfway through
    // reverses the fade. Driven off the open scale the overview already
    // animates, so the reversal is not a second state that could disagree with
    // the first -- the close retargets that scale from wherever it is and this
    // follows. Everything the strip draws is multiplied by it, minis included.
    const float FADE = overviewGridProgress(renderScale);
    if (FADE < 0.01F)
        return;

    auto*      dragContext = g_pointerGrabOverview && g_pointerGrabOverview->drag.activeWindow ? g_pointerGrabOverview : this;
    const auto DRAGGED     = getOverviewWindowToShow(dragContext->drag.activeWindow.lock());

    size_t hoveredIdx = images.size();
    if (DRAGGED && isOverviewPointerOnMonitor(monitor)) {
        const auto HOVERED = workspaceAtStripPoint(lastMousePosLocal, &hoveredIdx);

        // X3 delta 5: the reference sets hoveredWhileDragging only when the cell is
        // NOT the drag's own source workspace (OverviewWidget.qml:215). Dropping a
        // window back where it came from is a no-op, so it must not light up as a
        // target. Only the HIGHLIGHT is suppressed -- the cell stays a live drop
        // target, exactly as the reference still claims it.
        if (HOVERED && HOVERED == dragContext->drag.originalWorkspace.lock())
            hoveredIdx = images.size();
    }

    const auto  CARDROUND = getOverviewCardRounding(monitor, renderScale);
    SStripStyle style;
    style.round = CARDROUND.rounding;
    style.power = CARDROUND.power;
    style.fade  = FADE;
    // ponytail: the inner radius is the reference's verysmall:large ratio
    // (8:23) applied to whatever the outer radius resolves to, rather than a
    // literal 8. E8-c pinned the outer radius to the live desktop curve, so a
    // hardcoded inner would stop tracking the theme the moment that changes.
    style.innerRound = std::max(0, sc<int>(std::round(sc<float>(style.round) * 8.F / 23.F)));

    renderStripPanel(monitor, renderScale, style);

    for (size_t workspaceIdx = 0; workspaceIdx < images.size(); ++workspaceIdx) {
        const auto& wimg = images[workspaceIdx];
        if (!wimg || !wimg->pWorkspace)
            continue;

        const auto CELL = stripCellBox(workspaceIdx, monitor, renderScale);
        if (CELL.width <= 1 || !overviewBoxIntersectsMonitor(CELL, monitor))
            continue;

        renderStripCell(monitor, workspaceIdx, CELL, DRAGGED, workspaceIdx == hoveredIdx, renderScale, now, style);
    }
}

// How many open windows the query reaches. Counted across every workspace, not
// just the focused one: the strip shows matching minis from elsewhere, so "no
// matches" has to agree with that.
size_t CScrollOverview::matchingWindowCount() const {
    if (g_search.query.text.empty())
        return 0;

    size_t matches = 0;
    for (const auto& wimg : images) {
        if (wimg && wimg->pWorkspace)
            matches += overviewGridWindows(wimg->pWorkspace).size();
    }
    return matches;
}

// ---------------------------------------------------------------------------
// The search render step, in four named stages: ANIMATE -> LAYOUT -> PUBLISH ->
// DRAW.
//
// This was one 450-line function that computed a box, drew it, wrote a global,
// computed the next box out of that global, and drew that. The stages are not
// decoration. The two heights the GRID and the STRIP lay themselves out against
// are produced HERE, and they are read by code that ran EARLIER in the same
// frame -- the only thing that ever made that work was the order the statements
// happened to be written in. It is a signature now: layoutSearch() is handed
// everything it reads and returns everything it produces, and exactly one
// assignment makes any of it visible to anybody else.
//
// The frame order itself is preserved verbatim, because it is load-bearing:
//   * the band target and X64's section trim are computed against the PREVIOUS
//     frame's pill row -- they run before the pill is laid out, as they always
//     have;
//   * the results panel is placed against THIS frame's pill row, which is why
//     that row is a parameter and not a read-back of the published value;
//   * the publish lands where the old scattered writes landed, so the grid and
//     the strip -- which render before this step -- go on reading the frame
//     before's values. Publishing earlier would slide the grid's climb one
//     frame ahead of the pill it is following, which is a pixel change to X50.

// Everything layoutSearch() reads that is neither a result nor a section. The
// animated values are sampled ONCE here, at a named moment, instead of each
// consumer calling ->value() whenever it happens to run.
struct SSearchLayoutInputs {
    PHLMONITOR                     monitor;
    float                          renderScale = 1.F;
    ScrollOverview::Config::ELayout layout{};
    float                          fade = 0.F;  // the overview's open progress
    float                          dock = 0.F;  // X50: searchDock->value()
    float                          band = 0.F;  // X21/X23: resultsBand->value()
    float                          width = 0.F; // animated container width
    float                          colW[3] = {};
    float                          inspectorW = SEARCH_INSP_WL;
    // The published pill row is kept, not recomputed, when the pill culls: that
    // frame has no answer of its own and the grid must not be told it does.
    float                          rowLCarry = OVERVIEW_SEARCH_ROWHL;
    std::string_view               query;
    bool                           queried = false, emptyResult = false;
    int                            selection = -1;
};

// LAYOUT. Pure: monitor, animation values and the section set in, boxes out. It
// reads no global and no member and writes nothing -- which is exactly what
// lets the painter, the pointer hit-test and the keyboard walk all be handed
// the same answer instead of each deriving its own.
static SSearchLayout layoutSearch(const SSearchLayoutInputs& IN, const std::vector<SSearchRow>& RESULTS, const std::vector<SSearchSection>& SECTIONS) {
    SSearchLayout out;
    out.bandH       = IN.band;
    out.rowL        = IN.rowLCarry;
    out.fade        = IN.fade;
    out.queried     = IN.queried;
    out.emptyResult = IN.emptyResult;
    out.selection   = IN.selection;

    const auto MONITOR = IN.monitor;

    out.label = !IN.queried ? "Search, calculate or run" : (IN.emptyResult ? std::string(IN.query) + "    no matches" : std::string(IN.query));

    // The pill is laid out in the SAME logical space as the strip it sits above.
    // That placement is load-bearing and is NOT the part X22 changed: laid out
    // at the monitor's true top edge it landed underneath the layer-shell bar,
    // which this pass draws after the whole overview, so the pill was present
    // but invisible (X10). The overview scale insets this logical row far
    // enough down to clear the bar; X22 only removed the pan (see
    // OVERVIEW_CHROME_NO_PAN).
    const float FIELDWL     = IN.queried ? OVERVIEW_PILL_FIELDQL : OVERVIEW_PILL_FIELDL;
    const float PILLWL      = OVERVIEW_PILL_WL(FIELDWL);
    const CBox  PILLLOGICAL = {
        sc<float>(MONITOR->m_position.x) + (sc<float>(MONITOR->m_size.x) - PILLWL) / 2.F,
        sc<float>(MONITOR->m_position.y) + (OVERVIEW_SEARCH_ROWHL - OVERVIEW_PILL_HL) / 2.F,
        PILLWL,
        OVERVIEW_PILL_HL,
    };

    CBox pill = getOverviewGlobalBox(PILLLOGICAL, MONITOR, IN.renderScale, OVERVIEW_CHROME_NO_PAN, 0.F, IN.layout);

    // Kept so the dock offset below can be read back out in logical px.
    const float PILLUNDOCKEDY = sc<float>(pill.y);

    // X50: docked, the pill leaves the card's inset behind and sits just under
    // the layer-shell bar, which is what the work area's top edge means. Only y
    // moves -- the card scales about the viewport centre, so a logically centred
    // box is still centred after the transform. getOverviewBox works in
    // monitor-local RENDERED px, hence the m_scale on the way in.
    if (const float DOCK = std::clamp(IN.dock, 0.F, 1.F); DOCK > 0.001F) {
        float dockTopL = overviewTopChromeBottomL(MONITOR);
        if (const auto WS = MONITOR->m_activeWorkspace; WS && WS->m_space) {
            if (const auto WA = WS->m_space->workArea(); WA.w > 1 && WA.h > 1)
                dockTopL = std::max(dockTopL, sc<float>(WA.y)); // a bar that DOES reserve
        }

        const float DOCKY = (dockTopL - sc<float>(MONITOR->m_position.y) + OVERVIEW_SEARCH_DOCKPADL) * sc<float>(MONITOR->m_scale);
        pill.y += (DOCKY - sc<float>(pill.y)) * DOCK;
    }

    if (pill.width <= 1.F || !overviewBoxIntersectsMonitor(pill, MONITOR))
        return out; // nothing to draw, and the published heights carry unchanged
    pill.round();

    out.drawable = true;
    out.pill     = pill;

    // Same rounding the workspace cards and the strip use: decoration:rounding
    // (0 on Omarchy) and decoration:rounding_power. Clamped per-box at draw.
    const auto CARDROUND = getOverviewCardRounding(MONITOR, IN.renderScale);
    out.rounding         = CARDROUND.rounding;
    out.roundingPower    = CARDROUND.power;

    // Everything inside the pill is measured off the box as it actually landed,
    // so the badge and the type follow the card transform for free.
    out.scale          = sc<float>(pill.width) / PILLWL;
    const float SCALE  = out.scale;
    const float BADGED = std::round(OVERVIEW_PILL_BADGEL * SCALE);
    const float PADL   = std::round(OVERVIEW_PILL_PADL * SCALE);
    out.gap            = std::round(OVERVIEW_PILL_GAPL * SCALE);
    out.fieldW         = std::round(FIELDWL * SCALE);

    out.badge = CBox{pill.x + PADL, pill.y + std::round((pill.height - BADGED) / 2.F), BADGED, BADGED};
    out.badge.round();

    // The row the pill vacates, reclaimed -- the user asked, so the ~32px the
    // old ponytail note deferred is taken here.
    //
    // The floor is the pill's own foot, brought back into the card's logical
    // space: getOverviewGlobalBox is affine and the pill's width just measured
    // its scale, so the undocked box inverts it from one known point. Undocked
    // this evaluates to 8 + 48 + 8 = the full 64px row, so the un-queried
    // layout is unchanged by construction rather than by a special case.
    const float PILLFOOTL = PILLLOGICAL.y + OVERVIEW_PILL_HL + ((sc<float>(pill.y) - PILLUNDOCKEDY) / std::max(0.001F, SCALE));
    out.rowL              = std::clamp(PILLFOOTL - sc<float>(MONITOR->m_position.y) + OVERVIEW_SEARCH_DOCKPADL, 0.F, OVERVIEW_SEARCH_ROWHL);

    // F3: the app half of the search, laid out under the pill. This is a second,
    // ordered list ALONGSIDE the window gate -- it deliberately does not change
    // which windows the grid shows (RICE_PROGRESS.md "F3-scope" §1).
    //
    // X49: one rounded card PER PROVIDER, spanning the same width the grid tiles
    // do, each with the provider's icon and name on its leading edge and its
    // rows laid out across the rest. There is no outer panel -- the cards are
    // the surface.
    const float BANDHL = std::max(0.F, out.bandH);
    if (SECTIONS.empty() || BANDHL <= (OVERVIEW_RESULT_PANELGAPL * 2.F) + 1.F)
        return out;

    // The ranking can shrink under a selection made against a longer list. It is
    // clamped by the frame that noticed and handed back by the publish step.
    if (out.selection >= sc<int>(RESULTS.size()))
        out.selection = sc<int>(RESULTS.size()) - 1;

    const float PAD_L      = SEARCH_PAD_L;
    const float DIV_GAPL   = SEARCH_DIV_GAPL;
    const float MAX_SPANWL = overviewResultsSpanW(MONITOR);

    float CONTAINER_WL = IN.width > 1.F ? std::min(IN.width, MAX_SPANWL) : 0.F;
    if (CONTAINER_WL < 8.F)
        return out;

    const float CONTAINER_XL = sc<float>(MONITOR->m_position.x) + ((sc<float>(MONITOR->m_size.x) - CONTAINER_WL) / 2.F);
    const float CONTAINER_YL = sc<float>(MONITOR->m_position.y) + out.rowL + OVERVIEW_RESULT_PANELGAPL;
    const float CONTAINER_HL = BANDHL - (OVERVIEW_RESULT_PANELGAPL * 2.F);

    const CBox CONTAINER_LOGICAL{CONTAINER_XL, CONTAINER_YL, CONTAINER_WL, CONTAINER_HL};
    CBox container = getOverviewGlobalBox(CONTAINER_LOGICAL, MONITOR, IN.renderScale, OVERVIEW_CHROME_NO_PAN, 0.F, IN.layout);
    if (container.width <= 1.F || !overviewBoxIntersectsMonitor(container, MONITOR))
        return out;
    container.round();
    out.containerBox = container;

    const auto ADDITEM = [&](const CBox& box, size_t I, int col, int rowInCol) {
        out.hits.emplace_back(SSearchRowHit{
            .box       = box,
            .desktopId = RESULTS[I].app ? RESULTS[I].app->id : "",
            .icon      = RESULTS[I].icon,
            .index     = sc<int>(I),
            .column    = col,
            .rowInCol  = rowInCol,
        });
        return SSearchLayout::SItem{
            .hit         = out.hits.size() - 1,
            .name        = RESULTS[I].name,
            .description = RESULTS[I].description,
            .icon        = RESULTS[I].icon,
            .iconFont    = RESULTS[I].iconFont,
            .selected    = (g_search.nav.zone == EOverviewNavZone::Result && sc<int>(I) == out.selection),
        };
    };

    const float HEAD_HL  = 28.F;
    const float ROW_HL   = 32.F;
    const float ROW_GAPL = 4.F;

    const SSearchSection* sectionByCol[3] = {};
    for (const auto& sec : SECTIONS) {
        if (sec.column >= 0 && sec.column < 3)
            sectionByCol[sec.column] = &sec;
    }

    const float INSP_WL = std::clamp(IN.inspectorW, 140.F, std::max(140.F, CONTAINER_WL - (PAD_L * 2.F)));
    const float INSP_XL = CONTAINER_XL + CONTAINER_WL - PAD_L - INSP_WL;
    const float COL_MAX_R = INSP_XL - DIV_GAPL;

    float colX        = CONTAINER_XL + PAD_L;
    int   drawn       = 0;
    for (int c = 0; c < 3; ++c) {
        const float COL_WL = IN.colW[c];
        if (COL_WL < SEARCH_COL_HIDE_WL)
            continue;

        const auto* SECTION = sectionByCol[c];
        if (!SECTION)
            continue;

        const float COL_XL = colX;
        const float COL_YL = CONTAINER_YL + PAD_L;
        const float COL_HL = CONTAINER_HL - (PAD_L * 2.F);
        if (COL_XL + COL_WL > COL_MAX_R + 1.F && COL_MAX_R - COL_XL < SEARCH_COL_HIDE_WL)
            break;

        const CBox COL_LOGICAL{COL_XL, COL_YL, COL_WL, COL_HL};
        CBox colBox = getOverviewGlobalBox(COL_LOGICAL, MONITOR, IN.renderScale, OVERVIEW_CHROME_NO_PAN, 0.F, IN.layout);
        colBox.round();

        SSearchLayout::SCard cardOut;
        cardOut.box   = colBox;
        cardOut.alpha = std::clamp(COL_WL / 120.F, 0.F, 1.F);
        cardOut.grid  = false;
        cardOut.title = SECTION->title;
        cardOut.icon  = SECTION->icon;
        cardOut.more  = SECTION->total > SECTION->rows.size() ? SECTION->total - SECTION->rows.size() : 0;
        if (SECTION->rows.empty())
            cardOut.emptyHint = (c == 1 && OmarchySearch::fileSearchBusy()) ? "Searching…" : "No items";

        for (size_t n = 0; n < SECTION->rows.size(); ++n) {
            const size_t I      = SECTION->rows[n];
            const float  ROW_YL = COL_YL + HEAD_HL + sc<float>(n) * (ROW_HL + ROW_GAPL);
            if (ROW_YL + ROW_HL > COL_YL + COL_HL + 2.F)
                break;

            const CBox ROWLOGICAL{COL_XL, ROW_YL, COL_WL, ROW_HL};
            CBox row = getOverviewGlobalBox(ROWLOGICAL, MONITOR, IN.renderScale, OVERVIEW_CHROME_NO_PAN, 0.F, IN.layout);
            row.round();

            cardOut.items.emplace_back(ADDITEM(row, I, SECTION->column, sc<int>(n)));
        }

        out.cards.emplace_back(std::move(cardOut));
        ++drawn;

        const float DIV_XL = COL_XL + COL_WL + (DIV_GAPL / 2.F);
        if (DIV_XL < INSP_XL - 2.F) {
            const CBox DIV_LOGICAL{DIV_XL, CONTAINER_YL + PAD_L, 1.F, CONTAINER_HL - (PAD_L * 2.F)};
            CBox divBox = getOverviewGlobalBox(DIV_LOGICAL, MONITOR, IN.renderScale, OVERVIEW_CHROME_NO_PAN, 0.F, IN.layout);
            divBox.round();
            divBox.width = std::max(1.F, std::round(1.F * out.scale));
            out.dividers.push_back(divBox);
        }

        colX += COL_WL + DIV_GAPL;
    }

    if (drawn == 0)
        return out;

    const float INSP_YL = CONTAINER_YL + PAD_L;
    const float INSP_HL = CONTAINER_HL - (PAD_L * 2.F);

    const CBox INSP_LOGICAL{INSP_XL, INSP_YL, INSP_WL, INSP_HL};
    CBox inspBox = getOverviewGlobalBox(INSP_LOGICAL, MONITOR, IN.renderScale, OVERVIEW_CHROME_NO_PAN, 0.F, IN.layout);
    inspBox.round();
    if (out.containerBox.width > 1.F)
        inspBox = inspBox.intersection(out.containerBox);
    inspBox.round();

    out.inspector.box     = inspBox;
    out.inspector.visible = inspBox.width > 8.F && inspBox.height > 8.F;
    out.inspector.alpha   = 1.F;

    const int INSP_IDX = (out.selection >= 0 && out.selection < sc<int>(RESULTS.size())) ? out.selection : 0;
    if (INSP_IDX >= 0 && INSP_IDX < sc<int>(RESULTS.size())) {
        const auto& ROW = RESULTS[INSP_IDX];
        out.inspector.title    = ROW.name;
        out.inspector.subtitle = ROW.description;
        out.inspector.icon     = ROW.icon;

        if (ROW.omarchy) {
            const auto& o = *ROW.omarchy;
            out.inspector.category     = o.category;
            out.inspector.canQuickLook = o.canQuickLook;
            if (o.kind == OmarchySearch::EItemKind::File) {
                out.inspector.detail1      = o.fileTypeStr;
                out.inspector.detail2      = o.fileSizeStr + "  ·  " + o.fileModifiedStr;
                out.inspector.detail3      = o.filePath;
                out.inspector.shortcutHint = "Space  Quick Look";
                out.inspector.shortcutHint2 = "Enter  Open";
            } else if (o.kind == OmarchySearch::EItemKind::Menu) {
                out.inspector.detail1      = "Opens this submenu in the left column";
                out.inspector.shortcutHint = "Enter  Open submenu";
                out.inspector.shortcutHint2 = "Left  Back";
            } else if (o.kind == OmarchySearch::EItemKind::Back) {
                out.inspector.detail1      = "Return to the parent menu";
                out.inspector.shortcutHint = "Enter  Go back";
            } else if (o.kind == OmarchySearch::EItemKind::Action) {
                out.inspector.detail1      = o.description.empty() ? o.name : o.description;
                out.inspector.detail2      = o.actionCommand;
                out.inspector.shortcutHint = "Enter  Run";
            } else if (o.kind == OmarchySearch::EItemKind::Agent) {
                out.inspector.detail1      = o.description;
                out.inspector.shortcutHint = "Enter  Launch agent";
            } else if (o.kind == OmarchySearch::EItemKind::Calculator) {
                out.inspector.detail1      = o.description;
                out.inspector.detail2      = "Enter copies " + o.actionCommand + " to the clipboard";
                out.inspector.shortcutHint = "Enter  Copy result";
            }
        } else if (ROW.app) {
            const auto LIVE = overviewWindowForApp(*ROW.app);
            out.inspector.category       = "Application";
            out.inspector.running        = !!LIVE;
            out.inspector.canNewInstance = !!LIVE;
            if (!ROW.app->comment.empty())
                out.inspector.subtitle = ROW.app->comment;
            else if (!ROW.app->generic.empty())
                out.inspector.subtitle = ROW.app->generic;
            if (LIVE) {
                out.inspector.status = "Open";
                if (!LIVE->m_title.empty())
                    out.inspector.detail1 = LIVE->m_title;
                if (LIVE->m_workspace)
                    out.inspector.detail2 = "Workspace " + std::to_string(LIVE->m_workspace->m_id);
                out.inspector.shortcutHint  = "Enter  Switch to window";
                out.inspector.shortcutHint2 = "Shift×2  New window";
            } else {
                out.inspector.status       = "Not running";
                out.inspector.detail1      = ROW.app->generic;
                out.inspector.shortcutHint = "Enter  Launch";
            }
            out.inspector.detail3 = ROW.app->id;
        } else {
            out.inspector.category     = "Search Provider";
            out.inspector.detail1      = ROW.provider.providerDesktopId;
            out.inspector.detail2      = ROW.provider.description;
            out.inspector.shortcutHint = "Enter  Open";
        }
    }

    return out;
}

// DRAW. Paints the layout and decides nothing: every box it uses was decided
// above, so the pixels and the hit boxes cannot drift apart.
static void drawSearch(const SSearchLayout& L) {
    if (!L.drawable)
        return;

    const float FADE  = L.fade;
    const float SCALE = L.scale;
    const float POWER = L.roundingPower;
    const CBox& PILL  = L.pill;
    const CBox& BADGE = L.badge;

    const auto roundFor = [&](const CBox& box) {
        return std::min(L.rounding, sc<int>(std::round(std::min(box.w, box.h) / 2.0)));
    };

    const int PILLROUND  = roundFor(PILL);
    const int BADGEROUND = roundFor(BADGE);
    const auto ACCENT    = ScrollOverview::Config::highlightColor();

    OverviewRender::themePlate(PILL, PILLROUND, POWER, FADE, false);

    CRectPassElement::SRectData badgeBg;
    badgeBg.box           = BADGE;
    badgeBg.color         = ACCENT.modifyA(0.30F * FADE);
    badgeBg.round         = BADGEROUND;
    badgeBg.roundingPower = POWER;
    g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(badgeBg));

    float      texFade = FADE;
    const auto ADDTEX  = [&](const auto& tex, const CBox& box) {
        CTexPassElement::SRenderData data;
        data.tex = tex;
        data.box = box;
        data.a   = texFade;
        g_pHyprRenderer->m_renderPass.add(makeUnique<CTexPassElement>(data));
    };

    const auto GLYPHTEX = OverviewRender::text("\uF002", ScrollOverview::Config::highlightForeground().modifyA(0.92F), std::max(1, sc<int>(std::round(OVERVIEW_PILL_GLYPHPX * SCALE))), false,
                                                      "JetBrainsMono Nerd Font", 0);
    if (GLYPHTEX && GLYPHTEX->ok()) {
        CBox glyphBox = {
            BADGE.x + std::round((BADGE.width - sc<float>(GLYPHTEX->m_size.x)) / 2.F),
            BADGE.y + std::round((BADGE.height - sc<float>(GLYPHTEX->m_size.y)) / 2.F),
            sc<float>(GLYPHTEX->m_size.x),
            sc<float>(GLYPHTEX->m_size.y),
        };
        ADDTEX(GLYPHTEX, glyphBox.round());
    }

    if (L.containerBox.width > 1.F) {
        OverviewRender::themePlate(L.containerBox, roundFor(L.containerBox), POWER, FADE, true);

        for (const auto& divBox : L.dividers) {
            CRectPassElement::SRectData div;
            div.box   = divBox;
            div.color = ScrollOverview::Config::themeForeground().modifyA(0.12F * FADE);
            g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(div));
        }
    }

    for (const auto& CARD : L.cards) {
        texFade = FADE * CARD.alpha;

        // Header at top of column
        const float PADL       = std::round(8.F * SCALE);
        float       headIndent = PADL;

        const auto  HEADICON   = OverviewRender::appIcon(CARD.icon);
        const float HEAD_ICOND = std::round(16.F * SCALE);
        if (HEADICON && HEADICON->ok()) {
            CBox iconBox{CARD.box.x + headIndent, CARD.box.y + PADL + std::round((18.F * SCALE - HEAD_ICOND) / 2.F), HEAD_ICOND, HEAD_ICOND};
            ADDTEX(HEADICON, iconBox.round());
            headIndent += HEAD_ICOND + std::round(8.F * SCALE);
        }

        const int  HEADPX  = std::max(1, sc<int>(std::round(12.F * SCALE)));
        const auto HEADTEX = OverviewRender::text(CARD.title, ScrollOverview::Config::themeHeading(), HEADPX, false, "", std::max(1, sc<int>(CARD.box.width - headIndent - PADL)), 600);
        if (HEADTEX && HEADTEX->ok()) {
            CBox headText{CARD.box.x + headIndent, CARD.box.y + PADL, sc<float>(HEADTEX->m_size.x), sc<float>(HEADTEX->m_size.y)};
            ADDTEX(HEADTEX, headText.round());
        }

        if (CARD.more > 0) {
            const int   MOREPX  = std::max(1, sc<int>(std::round(11.F * SCALE)));
            std::string moreStr = "+" + std::to_string(CARD.more);
            const auto  MORETEX = OverviewRender::text(moreStr, ScrollOverview::Config::themeMutedText(), MOREPX, false, "", 100, 500);
            if (MORETEX && MORETEX->ok()) {
                CBox moreBox{(CARD.box.x + CARD.box.width) - PADL - sc<float>(MORETEX->m_size.x), CARD.box.y + PADL, sc<float>(MORETEX->m_size.x), sc<float>(MORETEX->m_size.y)};
                ADDTEX(MORETEX, moreBox.round());
            }
        }

        // Rows
        for (const auto& ITEM : CARD.items) {
            const CBox& ROW = L.hits[ITEM.hit].box;

            if (ITEM.selected) {
                CRectPassElement::SRectData sel;
                sel.box           = ROW;
                const auto SELFILL = ScrollOverview::Config::themeSelectedFill();
                sel.color          = SELFILL.modifyA(SELFILL.a * FADE * CARD.alpha);
                sel.round         = roundFor(ROW);
                sel.roundingPower = POWER;
                g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(sel));
            }

            const float ROWPAD     = std::round(6.F * SCALE);
            float       textIndent = ROWPAD;

            const float ICOND = std::round(18.F * SCALE);
            if (OmarchySearch::isGlyphIcon(ITEM.icon)) {
                const int         GLYPHPX = std::max(1, sc<int>(std::round(16.F * SCALE)));
                const std::string FAMILY  = ITEM.iconFont.empty() ? "JetBrainsMono Nerd Font" : ITEM.iconFont;
                const auto        GLYPHTEX =
                    OverviewRender::text(ITEM.icon, ITEM.selected ? ScrollOverview::Config::themeSelectedText() : ScrollOverview::Config::themeForeground(), GLYPHPX, false,
                                         FAMILY, sc<int>(ICOND + std::round(8.F * SCALE)), 400);
                if (GLYPHTEX && GLYPHTEX->ok()) {
                    CBox glyphBox{ROW.x + ROWPAD, ROW.y + std::round((ROW.height - sc<float>(GLYPHTEX->m_size.y)) / 2.F), sc<float>(GLYPHTEX->m_size.x),
                                  sc<float>(GLYPHTEX->m_size.y)};
                    ADDTEX(GLYPHTEX, glyphBox.round());
                    textIndent += std::max(ICOND, sc<float>(GLYPHTEX->m_size.x)) + std::round(8.F * SCALE);
                }
            } else if (const auto ICONTEX = OverviewRender::appIcon(ITEM.icon); ICONTEX && ICONTEX->ok()) {
                CBox iconBox{ROW.x + ROWPAD, ROW.y + std::round((ROW.height - ICOND) / 2.F), ICOND, ICOND};
                ADDTEX(ICONTEX, iconBox.round());
                textIndent += ICOND + std::round(8.F * SCALE);
            }

            const int  ROWPX  = std::max(1, sc<int>(std::round(13.F * SCALE)));
            const auto FG     = ITEM.selected ? ScrollOverview::Config::themeSelectedText() : ScrollOverview::Config::themeForeground();
            const auto ROWTEX = OverviewRender::text(ITEM.name, FG, ROWPX, false, "", std::max(1, sc<int>(ROW.width - textIndent) - sc<int>(ROWPAD)), 500);
            if (ROWTEX && ROWTEX->ok()) {
                CBox rowText{ROW.x + textIndent, ROW.y + std::round((ROW.height - sc<float>(ROWTEX->m_size.y)) / 2.F), sc<float>(ROWTEX->m_size.x), sc<float>(ROWTEX->m_size.y)};
                ADDTEX(ROWTEX, rowText.round());

                if (!ITEM.description.empty()) {
                    const float DESCX = rowText.x + rowText.width + std::round(8.F * SCALE);
                    const int   DESCW = sc<int>((ROW.x + ROW.width - ROWPAD) - DESCX);
                    if (DESCW > sc<int>(std::round(24.F * SCALE))) {
                        const auto DESCFG = ITEM.selected ? FG : ScrollOverview::Config::themeMutedText();
                        const int  DESCPX = std::max(1, sc<int>(std::round(11.F * SCALE)));
                        if (const auto DESCTEX = OverviewRender::text(ITEM.description, DESCFG, DESCPX, false, "", DESCW); DESCTEX && DESCTEX->ok()) {
                            CBox descText{DESCX, ROW.y + std::round((ROW.height - sc<float>(DESCTEX->m_size.y)) / 2.F), sc<float>(DESCTEX->m_size.x), sc<float>(DESCTEX->m_size.y)};
                            ADDTEX(DESCTEX, descText.round());
                        }
                    }
                }
            }
        }

        if (CARD.items.empty()) {
            const int  EMPTYPX  = std::max(1, sc<int>(std::round(12.F * SCALE)));
            const auto EMPTYTEX = OverviewRender::text(CARD.emptyHint.empty() ? "No items" : CARD.emptyHint, ScrollOverview::Config::themeMutedText(), EMPTYPX, false, "", sc<int>(CARD.box.width - PADL * 2.F), 400);
            if (EMPTYTEX && EMPTYTEX->ok()) {
                CBox emptyBox{CARD.box.x + PADL, CARD.box.y + PADL + std::round(30.F * SCALE), sc<float>(EMPTYTEX->m_size.x), sc<float>(EMPTYTEX->m_size.y)};
                ADDTEX(EMPTYTEX, emptyBox.round());
            }
        }
    }

    // Draw Inspector Card (inside unified block on the right)
    if (L.inspector.visible) {
        texFade = FADE * L.inspector.alpha;
        const CBox& IBOX = L.inspector.box;
        const float PADL  = std::round(14.F * SCALE);
        const float ICOND = std::round(48.F * SCALE);
        const float CLIPB = IBOX.y + IBOX.height - PADL;

        const auto ADDINSP = [&](const SP<Render::ITexture>& tex, CBox box) {
            if (!tex || !tex->ok() || box.width < 1.F)
                return;
            CTexPassElement::SRenderData data;
            data.tex     = tex;
            data.box     = box;
            data.a       = texFade;
            data.clipBox = IBOX;
            g_pHyprRenderer->m_renderPass.add(makeUnique<CTexPassElement>(data));
        };

        const auto ICONTEX = OverviewRender::appIcon(L.inspector.icon);
        if (ICONTEX && ICONTEX->ok()) {
            CBox iconBox{IBOX.x + PADL, IBOX.y + PADL, ICOND, ICOND};
            ADDINSP(ICONTEX, iconBox.round());
        } else if (!L.inspector.icon.empty()) {
            const int  GLYPHPX = std::max(1, sc<int>(std::round(28.F * SCALE)));
            const auto GLYPHTEX = OverviewRender::text(L.inspector.icon, ScrollOverview::Config::themeHeading(), GLYPHPX, false, "", sc<int>(ICOND), 400);
            if (GLYPHTEX && GLYPHTEX->ok()) {
                CBox glyphBox{IBOX.x + PADL, IBOX.y + PADL, sc<float>(GLYPHTEX->m_size.x), sc<float>(GLYPHTEX->m_size.y)};
                ADDINSP(GLYPHTEX, glyphBox.round());
            }
        }

        float textY = IBOX.y + PADL;
        const float TEXTX = IBOX.x + PADL + ICOND + std::round(12.F * SCALE);
        const int   TEXTW = std::max(1, sc<int>(IBOX.width - (TEXTX - IBOX.x) - PADL));
        const int   FULLW = std::max(1, sc<int>(IBOX.width - (PADL * 2.F)));

        const auto paintWrap = [&](const std::string& s, const CHyprColor& col, int px, int weight, float x, int w, int maxH, float gapAfter) {
            if (s.empty() || textY + sc<float>(px) > CLIPB)
                return;
            const int W = std::max(16, ((w + 15) / 16) * 16);
            const auto TEX = OverviewRender::textWrapped(s, col, px, W, maxH, false, "", weight);
            if (!TEX || !TEX->ok())
                return;
            CBox box{x, textY, sc<float>(TEX->m_size.x), sc<float>(TEX->m_size.y)};
            ADDINSP(TEX, box.round());
            textY += sc<float>(TEX->m_size.y) + gapAfter;
        };

        if (!L.inspector.category.empty()) {
            auto catCol = ScrollOverview::Config::themeHeading();
            paintWrap(L.inspector.category, catCol, std::max(1, sc<int>(std::round(10.F * SCALE))), 600, TEXTX, TEXTW, sc<int>(18.F * SCALE), std::round(3.F * SCALE));
        }
        if (!L.inspector.status.empty()) {
            const auto STCOL = L.inspector.running ? ScrollOverview::Config::themeHeading() : ScrollOverview::Config::themeMutedText();
            paintWrap(L.inspector.status, STCOL, std::max(1, sc<int>(std::round(11.F * SCALE))), 600, TEXTX, TEXTW, sc<int>(18.F * SCALE), std::round(3.F * SCALE));
        }
        paintWrap(L.inspector.title, ScrollOverview::Config::themeForeground(), std::max(1, sc<int>(std::round(15.F * SCALE))), 600, TEXTX, TEXTW, sc<int>(40.F * SCALE),
                  std::round(4.F * SCALE));

        textY = std::max(textY, sc<float>(IBOX.y + PADL + ICOND)) + std::round(10.F * SCALE);

        paintWrap(L.inspector.subtitle, ScrollOverview::Config::themeMutedText(), std::max(1, sc<int>(std::round(12.F * SCALE))), 400, IBOX.x + PADL, FULLW, sc<int>(48.F * SCALE),
                  std::round(8.F * SCALE));
        const auto DETCOL = ScrollOverview::Config::themeMutedText();
        const int  DETPX  = std::max(1, sc<int>(std::round(11.F * SCALE)));
        paintWrap(L.inspector.detail1, DETCOL, DETPX, 400, IBOX.x + PADL, FULLW, sc<int>(40.F * SCALE), std::round(6.F * SCALE));
        paintWrap(L.inspector.detail2, DETCOL, DETPX, 400, IBOX.x + PADL, FULLW, sc<int>(28.F * SCALE), std::round(6.F * SCALE));
        paintWrap(L.inspector.detail3, DETCOL, std::max(1, sc<int>(std::round(10.F * SCALE))), 400, IBOX.x + PADL, FULLW, sc<int>(36.F * SCALE),
                  std::round(8.F * SCALE));

        float hintY = CLIPB;
        const int HINTPX = std::max(1, sc<int>(std::round(11.F * SCALE)));
        const auto HINTCOL = ScrollOverview::Config::themeMutedText();
        const auto measureHint = [&](const std::string& s) -> float {
            if (s.empty())
                return 0.F;
            const int W = std::max(16, ((FULLW + 15) / 16) * 16);
            const auto TEX = OverviewRender::textWrapped(s, HINTCOL, HINTPX, W, sc<int>(36.F * SCALE), false, "", 500);
            return (TEX && TEX->ok()) ? sc<float>(TEX->m_size.y) + std::round(4.F * SCALE) : 0.F;
        };
        hintY -= measureHint(L.inspector.shortcutHint2);
        hintY -= measureHint(L.inspector.shortcutHint);
        if (hintY > textY + std::round(8.F * SCALE))
            textY = hintY;

        paintWrap(L.inspector.shortcutHint, HINTCOL, HINTPX, 500, IBOX.x + PADL, FULLW, sc<int>(36.F * SCALE), std::round(4.F * SCALE));
        paintWrap(L.inspector.shortcutHint2, HINTCOL, HINTPX, 500, IBOX.x + PADL, FULLW, sc<int>(36.F * SCALE), 0.F);
    }

    texFade = FADE;
    const CHyprColor FG = !L.queried ? ScrollOverview::Config::themeMutedText() : (L.emptyResult ? ScrollOverview::Config::themeUrgent() : ScrollOverview::Config::themeForeground());

    const int  FONTPX = std::max(1, sc<int>(std::round(OVERVIEW_PILL_TEXTPX * SCALE)));
    const auto NOW    = Time::steadyNow();

    const float FIELDX = BADGE.x + BADGE.width + L.gap;
    const CBox  FIELD{FIELDX, PILL.y, L.fieldW, PILL.height};

    const auto ADDLETTER = [&](const SP<Render::ITexture>& tex, const CBox& box, float a) {
        if (!tex || !tex->ok() || a <= 0.F)
            return;
        CTexPassElement::SRenderData data;
        data.tex     = tex;
        data.box     = box;
        data.a       = texFade * a;
        data.clipBox = FIELD;
        g_pHyprRenderer->m_renderPass.add(makeUnique<CTexPassElement>(data));
    };

    if (!SearchLetters::g_searchLetters.empty()) {
        // Serpantinum Input.qml: one texture per codepoint, scale/y/opacity from
        // the add/remove transitions, packed left with 1 logical px of tracking.
        const auto  LIVE    = SearchLetters::g_searchLetters.liveGlyphs();
        const float SPACING = std::max(1.F, SCALE);
        std::vector<float> widths;
        widths.reserve(LIVE.size());
        for (const auto& G : LIVE) {
            const auto TEX = OverviewRender::text(G, FG, FONTPX, false, "", 0, 600);
            widths.push_back((TEX && TEX->ok()) ? sc<float>(TEX->m_size.x) : sc<float>(FONTPX) * 0.5F);
        }

        const auto PLACED = SearchLetters::g_searchLetters.place(widths, SPACING, SCALE, NOW);
        for (const auto& P : PLACED) {
            const auto TEX = OverviewRender::text(P.glyph, FG, FONTPX, false, "", 0, 600);
            if (!TEX || !TEX->ok())
                continue;
            const float W  = sc<float>(TEX->m_size.x);
            const float H  = sc<float>(TEX->m_size.y);
            const float CX = FIELDX + P.x + W * 0.5F;
            const float CY = PILL.y + std::round((PILL.height - H) / 2.F) + H * 0.5F + P.y;
            const float DW = W * P.scale;
            const float DH = H * P.scale;
            ADDLETTER(TEX, CBox{CX - DW * 0.5F, CY - DH * 0.5F, DW, DH}, P.opacity);
        }

        if (L.emptyResult && !LIVE.empty()) {
            const auto SUFFIX = OverviewRender::text("    no matches", FG, FONTPX, false, "", std::max(1, sc<int>(L.fieldW)), 600);
            if (SUFFIX && SUFFIX->ok()) {
                const float X = FIELDX + SearchLetters::g_searchLetters.liveEndX(widths, SPACING);
                CBox        suffixBox{X, PILL.y + std::round((PILL.height - sc<float>(SUFFIX->m_size.y)) / 2.F), sc<float>(SUFFIX->m_size.x), sc<float>(SUFFIX->m_size.y)};
                ADDLETTER(SUFFIX, suffixBox, 1.F);
            }
        }
        return;
    }

    const auto TEX = OverviewRender::text(L.label, FG, FONTPX, false, "", sc<int>(L.fieldW), L.queried ? 600 : 0);
    if (!TEX || !TEX->ok())
        return;

    CBox textBox = {
        FIELDX,
        PILL.y + std::round((PILL.height - sc<float>(TEX->m_size.y)) / 2.F),
        sc<float>(TEX->m_size.x),
        sc<float>(TEX->m_size.y),
    };
    ADDTEX(TEX, textBox.round());
}

// OVERVIEW_CPP_SPEC §5 F1 + §13.4 X2. The search bar: a field carrying an
// accent badge with a Nerd Font magnifying glass, a placeholder while empty,
// and a fixed field width that widens once a query is typed. Rounding, border,
// fill and frost come from the Hyprland theme (decoration:rounding, blur,
// general:border_size, col.inactive_border) rather than a forced capsule.
// The reference's metrics are the OVERVIEW_PILL_* family.
// ponytail: the width change is a hard cut; the reference eases it over 300ms.
// The reference's two trailing icon buttons (Google Lens, SongRec) are not
// drawn -- neither action exists in this overview, so they would be decoration
// for nothing.
void CScrollOverview::renderSearch(PHLMONITOR monitor, float renderScale) {
    Prof::Timer PROF{Prof::srchNs, Prof::srchCalls};
    if (!monitor)
        return;

    // G12. Cleared here rather than where the rows are drawn: every early
    // return below is a frame with no rows on it, and a stale box would hand
    // the next press a result that is no longer on screen.
    g_search.layout.hits.clear();

    // User, 2026-08-06: the pill fades in with the overview, and reverses if
    // the overview is toggled off mid-flight. See renderStrip for why the open
    // scale is the driver rather than a fade var of its own.
    const float FADE = overviewGridProgress(renderScale);
    if (FADE < 0.01F)
        return;

    // ---- ANIMATE: re-aim the search animations from this frame's results.
    // Results exist once there is query text, or while browsing an Omarchy
    // submenu after Enter (query is cleared so children are listed).
    const bool QUERIED = !g_search.query.text.empty() || OmarchySearch::canNavigateMenuBack();

    const size_t MATCHES = !g_search.query.text.empty() ? matchingWindowCount() : 0;
    const auto   RESULTS = QUERIED ? overviewSearchRows() : std::vector<SSearchRow>{};

    // An app hit is a hit: "no matches" must not contradict a listed result.
    const bool EMPTYRESULT = QUERIED && !g_search.query.text.empty() && MATCHES == 0 && RESULTS.empty();

    auto SECTIONS = overviewSearchSections(RESULTS);
    overviewTrimSectionsToFit(SECTIONS, monitor);

    // Typing arms first-app selection; results land over the next frames, so
    // the spend lives here -- once, on the first frame that actually has an
    // Omarchy/apps row. Not Browsing: auto-select is for Enter, not SPACE.
    if (g_search.nav.selectFirstPending) {
        const int FIRST = overviewFirstAppIndex(RESULTS);
        if (FIRST >= 0) {
            g_search.nav.selection         = FIRST;
            g_search.nav.zone              = EOverviewNavZone::Result;
            g_search.nav.selectFirstPending = false;
            g_search.nav.explicitSelection = false;
        }
    }

    const float TARGET = QUERIED ? overviewResultsBandTarget(monitor, SECTIONS) : 0.F;
    if (std::abs(resultsBand->goal() - TARGET) > 0.5F)
        *resultsBand = TARGET;

    const float WIDTH_TARGET = QUERIED ? overviewResultsWidthTarget(monitor, SECTIONS) : 0.F;
    // Grow from the pill, not from 0: a 0→span ease spends its first frames
    // on an invisible sliver, then pops. Warp onto the pill width once.
    if (WIDTH_TARGET > 50.F && resultsWidth->value() < 50.F)
        resultsWidth->setValueAndWarp(std::min(WIDTH_TARGET, overviewPillTargetWL(true)));
    if (std::abs(resultsWidth->goal() - WIDTH_TARGET) > 0.5F)
        *resultsWidth = WIDTH_TARGET;

    if (const float DOCKGOAL = QUERIED && !EMPTYRESULT && !RESULTS.empty() ? 1.F : 0.F; std::abs(searchDock->goal() - DOCKGOAL) > 0.01F)
        *searchDock = DOCKGOAL;

    const uint8_t COL_MASK = QUERIED ? overviewSearchColMask(SECTIONS) : 0;
    const auto    COL_TGT  = overviewSearchColWidths(resultsWidth->value(), COL_MASK);

    // ---- LAYOUT: the animated values are sampled once, right here.
    const SSearchLayoutInputs IN{
        .monitor     = monitor,
        .renderScale = renderScale,
        .layout      = this->layout,
        .fade        = FADE,
        .dock        = searchDock->value(),
        .band        = resultsBand->value(),
        .width       = resultsWidth->value(),
        .colW        = {COL_TGT.col[0], COL_TGT.col[1], COL_TGT.col[2]},
        .inspectorW  = COL_TGT.inspector,
        .rowLCarry   = g_search.layout.rowL,
        .query       = g_search.query.text,
        .queried     = QUERIED,
        .emptyResult = EMPTYRESULT,
        .selection   = g_search.nav.selection,
    };

    SSearchLayout LAYOUT = layoutSearch(IN, RESULTS, SECTIONS);

    // ---- PUBLISH: the one assignment the rest of the overview can see.
    g_search.nav.selection = LAYOUT.selection;
    g_search.layout        = std::move(LAYOUT);

    if (QUERIED && g_search.nav.selection >= 0 && g_search.nav.selection < sc<int>(RESULTS.size())) {
        if (RESULTS[g_search.nav.selection].omarchy && !RESULTS[g_search.nav.selection].omarchy->filePath.empty()) {
            OmarchySearch::updateSushiIfActive(RESULTS[g_search.nav.selection].omarchy->filePath);
        }
    }

    // ---- DRAW: out of the published object, so the painter, the pointer and
    // the keyboard are provably looking at the same boxes.
    SearchLetters::g_searchLetters.sync(g_search.query.text, Time::steadyNow());
    drawSearch(g_search.layout);

    // The letter bounce is timestamped, not a PHLANIMVAR, so the compositor
    // will not schedule the rest of the pop on its own.
    if (SearchLetters::g_searchLetters.busy(Time::steadyNow())) {
        damage();
        requestInputFrame();
    }
}

static const std::string* searchPreviewPath(const std::vector<SSearchRow>& results, int index) {
    if (index < 0 || index >= sc<int>(results.size()))
        return nullptr;
    const auto& ROW = results[sc<size_t>(index)];
    if (ROW.omarchy && ROW.omarchy->canQuickLook && !ROW.omarchy->filePath.empty())
        return &ROW.omarchy->filePath;
    return nullptr;
}

bool overviewToggleSushiForSelection() {
    const auto        RESULTS = overviewSearchRows();
    const std::string* PATH   = searchPreviewPath(RESULTS, g_search.nav.selection);
    if (!PATH)
        return false;
    OmarchySearch::toggleSushi(*PATH);
    return true;
}

bool overviewHandleSearchSpace() {
    const auto RESULTS = overviewSearchRows();
    const std::string* HOVER = searchPreviewPath(RESULTS, g_search.nav.hoverIndex);
    const std::string* SEL   = searchPreviewPath(RESULTS, g_search.nav.selection);
    if (!overviewSpacePreviews(g_search.nav.intent, HOVER != nullptr, SEL != nullptr, g_search.nav.explicitSelection)) {
        if (OmarchySearch::isSushiActive())
            OmarchySearch::closeSushi();
        return false;
    }

    const std::string* PATH = (g_search.nav.intent == ESearchIntent::Hovering && HOVER) ? HOVER : SEL;
    if (!PATH)
        return false;
    OmarchySearch::toggleSushi(*PATH);
    return true;
}

bool overviewHandleShiftTap() {
    if (g_search.query.text.empty())
        return false;

    const auto RESULTS = overviewSearchRows();
    int        idx     = g_search.nav.selection;
    if (idx < 0 || idx >= sc<int>(RESULTS.size()))
        idx = overviewFirstAppIndex(RESULTS);
    if (idx < 0 || idx >= sc<int>(RESULTS.size()) || !RESULTS[sc<size_t>(idx)].app)
        return false;

    AppSearch::launch(*RESULTS[sc<size_t>(idx)].app, ScrollOverview::Config::getTerminal());
    for (const auto& overview : scrollOverviews()) {
        if (overview)
            overview->close();
    }
    return true;
}

void CScrollOverview::renderDraggedWindow(PHLMONITOR monitor, size_t activeIdx, float workspacePitch, float renderScale, const Time::steady_tp& now) {
    auto*      dragOwner = g_pointerGrabOverview && g_pointerGrabOverview->drag.activeWindow ? g_pointerGrabOverview : this;
    const auto WINDOW    = getOverviewWindowToShow(dragOwner->drag.activeWindow.lock());
    if (!shouldShowOverviewWindow(WINDOW) || !WINDOW->m_workspace)
        return;

    const auto GLOBALBOX  = dragOwner->draggedWindowGlobalBox();
    const auto MONITORBOX = monitor ? monitor->logicalBox() : CBox{};
    if (GLOBALBOX.empty() || MONITORBOX.empty() || GLOBALBOX.intersection(MONITORBOX).empty())
        return;

    // ROUNDED, for X73's reason: the box follows the cursor and is scaled, so it
    // lands on fractional coordinates, and a 1px border drawn in the annulus
    // outside a fractional box keeps only its top and left edges. Measured on
    // the drag path exactly as on the hover path -- one ring row, one ring
    // column -- once the dragged window started drawing a ring at all.
    auto windowBox = CBox{
        (GLOBALBOX.pos() - monitor->m_position) * monitor->m_scale,
        GLOBALBOX.size() * monitor->m_scale,
    };
    windowBox.round();

    renderWindowLive(monitor, WINDOW, windowBox, dragOwner->scale->value(), now, nullptr, true);
}

bool CScrollOverview::hasVisiblePrecomputedBlurWindow(PHLMONITOR monitor, size_t activeIdx, float workspacePitch, float renderScale) const {
    if (!monitor)
        return false;

    const auto DRAGGEDWINDOW = getOverviewWindowToShow(drag.activeWindow.lock());

    for (size_t workspaceIdx = 0; workspaceIdx < images.size(); ++workspaceIdx) {
        const auto& workspaceImage = images[workspaceIdx];
        if (!workspaceImage || !workspaceImage->pWorkspace)
            continue;

        const auto WORKSPACEOFFSET = workspaceOverviewOffset(workspaceIdx, activeIdx, workspacePitch);
        const auto WORKSPACEBOX     = getOverviewWorkspaceBox(monitor, renderScale, viewOffset->value(), WORKSPACEOFFSET, layout);
        const auto VISIBLEBOX       = workspaceOverviewVisibleBox(workspaceIdx, WORKSPACEBOX, renderScale, monitor);
        // B2: with a query typed, an off-screen workspace can still own a
        // promoted tile drawn on the active card, so it must not be culled here.
        // ...and after the query is EMPTIED a releasing tile is still crossing
        // the active card, so it must keep its frost the whole way: section C
        // outranks this cull.
        const bool HASPROMOTED = std::ranges::any_of(workspaceImage->windows, [&](const auto& ref) { return overviewGridPromoted(getOverviewWindowToShow(ref.lock()), monitor); });
        if (!overviewBoxIntersectsMonitor(VISIBLEBOX, monitor) && g_search.query.text.empty() && !HASPROMOTED)
            continue;

        const auto workspace = workspaceImage->pWorkspace;

        const auto isVisiblePrecomputedBlurWindow = [&](const PHLWINDOW& window) {
            if (window == DRAGGEDWINDOW || !OverviewWindow::shouldUseBlurFramebuffer(window))
                return false;

            const auto windowBox = getOverviewWindowBox(window, monitor, renderScale, viewOffset->value(), WORKSPACEOFFSET, layout);
            return overviewBoxIntersectsMonitor(windowBox, monitor);
        };

        if (!isWorkspaceScrolling(workspace)) {
            const auto fullscreenWindow = getOverviewWindowToShow(Fullscreen::controller()->getFullscreenWindow(workspace));
            if (shouldShowOverviewWindow(fullscreenWindow) && fullscreenWindow->m_workspace == workspace) {
                if (isVisiblePrecomputedBlurWindow(fullscreenWindow))
                    return true;

                continue;
            }
        }

        for (const auto& windowRef : workspaceImage->windows) {
            if (isVisiblePrecomputedBlurWindow(getOverviewWindowToShow(windowRef.lock())))
                return true;
        }
    }

    return false;
}

void CScrollOverview::renderPinnedFloatingWindows(PHLMONITOR monitor, float overviewScale, const Time::steady_tp& now) {
    if (!monitor)
        return;

    const auto TARGETOVERVIEWSCALE = ScrollOverview::Config::getScale();
    const auto ANIMATIONPROGRESS   = (1.F - TARGETOVERVIEWSCALE) > 0.001F ? (1.F - overviewScale) / (1.F - TARGETOVERVIEWSCALE) : 1.F;

    for (const auto& windowRef : pinnedFloatingWindows) {
        const auto window = getOverviewWindowToShow(windowRef.lock());
        if (!shouldShowPinnedFloatingOverviewWindow(window))
            continue;
        if (drag.activeWindow && window == getOverviewWindowToShow(drag.activeWindow.lock()))
            continue;

        if (window->m_monitor != monitor)
            continue;

        float renderScale = 1.F;
        CBox  windowBox   = getPinnedFloatingOverviewWindowBox(monitor, window, TARGETOVERVIEWSCALE, ANIMATIONPROGRESS, &renderScale);

        if (!overviewBoxIntersectsMonitor(windowBox, monitor))
            continue;

        renderWindowLive(monitor, window, windowBox, renderScale, now);
    }
}

void CScrollOverview::renderWindowLive(PHLMONITOR monitor, PHLWINDOW window, const CBox& windowBox, float renderScale, const Time::steady_tp& now, const CBox* workspaceBox,
                                       bool dragged, float alphaMul) {
    if (!window)
        return;

    forced.forceWindow(window);
    forced.forceWindowSurfaces(window);
    // E8-f: renderWindow multiplies the texture by the WORKSPACE alpha, so a
    // workspace sitting at 0 draws this window's border and nothing else. The
    // window's own alpha is already forced per frame right above; forcing the
    // workspace only on rebuild is what let a later zeroing survive until the
    // overview was reopened -- which was the user's workaround, not a fix.
    forceWorkspaceAlphaVisible(window->m_workspace);

    // alphaMul is the strip's fade. It has to be applied AFTER the forces
    // above, which exist to pin this very channel at 1 (E8-f), and restored
    // immediately -- same set/render/restore shape renderOverviewLayerLevel
    // uses for layer surfaces, and it works for the same reason: the pass
    // elements read the alpha as they are built, not at draw time.
    const bool  MODULATE = alphaMul < 0.999F;
    const auto& WALPHA   = window->alpha(Desktop::View::WINDOW_ALPHA_FULLSCREEN);
    const float PREVA    = WALPHA ? WALPHA->value() : 1.F;
    if (MODULATE && WALPHA)
        WALPHA->setValueAndWarp(PREVA * std::clamp(alphaMul, 0.F, 1.F));

    OverviewWindow::renderOverviewWindow({
        .monitor              = monitor,
        .window               = window,
        .windowBox            = windowBox,
        .renderScale          = renderScale,
        .now                  = now,
        .workspaceBox         = workspaceBox,
        // The ring has three owners, and DRAGGED has to be one of them in its
        // own right. It used to ride the keyboard clause by accident -- a drag
        // leaves closeOnWindow on the window it picked up -- and X74 gating that
        // clause behind g_search.nav.keyboardTile took the ring off the dragged
        // window with it, because a drag is pointer input and clears the flag.
        //
        // It cannot come from hovered() either: updateTileHover drops the hover
        // outright while a drag is live, so the tile under the cursor is not
        // reported as hovered and the window being carried is not under the
        // cursor anyway once it has left its slot.
        .selected             = (g_search.nav.keyboardTile && g_search.nav.zone == EOverviewNavZone::Tile && closeOnWindow == window) ||
            TileChrome::hovered() == window || dragged,
        .dragged              = dragged,
    });

    if (MODULATE && WALPHA)
        WALPHA->setValueAndWarp(PREVA);
}

void CScrollOverview::refreshWindows() {
    redrawAll();
}

// P6i2 INSTRUMENT. See Membership::renderShadowEnabled. Runs at the end of
// onPreRender -- after the rebuild decision, so it grades the list the frame
// about to be drawn will actually walk, not the one before it.
void CScrollOverview::renderListShadow() {
    if (!Membership::renderShadowEnabled())
        return;

    std::unordered_map<const void*, WORKSPACEID> seenOnCard;

    for (const auto& img : images) {
        if (!img || !img->pWorkspace)
            continue;

        const auto CARDID = img->pWorkspace->m_id;

        std::vector<PHLWINDOW> snapshot;
        for (const auto& ref : img->windows) {
            const auto WINDOW = getOverviewWindowToShow(ref.lock());
            if (!WINDOW || !shouldShowOverviewWindow(WINDOW))
                continue;

            const auto LIVEWS = WINDOW->m_workspace ? WINDOW->m_workspace->m_id : WORKSPACE_INVALID;

            if (std::ranges::find(snapshot, WINDOW) != snapshot.end()) {
                Membership::renderDivergence("DUP", CARDID, LIVEWS, WINDOW->m_class, WINDOW->m_title);
                continue;
            }
            snapshot.emplace_back(WINDOW);

            if (const auto IT = seenOnCard.find(WINDOW.get()); IT != seenOnCard.end())
                Membership::renderDivergence("CROSS", CARDID, IT->second, WINDOW->m_class, WINDOW->m_title);
            else
                seenOnCard.emplace(WINDOW.get(), CARDID);
        }

        const auto LIVE = overviewGridWindows(img->pWorkspace);

        for (const auto& WINDOW : snapshot) {
            if (std::ranges::find(LIVE, WINDOW) == LIVE.end())
                Membership::renderDivergence("STALE", CARDID, WINDOW->m_workspace ? WINDOW->m_workspace->m_id : WORKSPACE_INVALID, WINDOW->m_class, WINDOW->m_title);
        }

        for (const auto& WINDOW : LIVE) {
            if (std::ranges::find(snapshot, WINDOW) == snapshot.end())
                Membership::renderDivergence("MISSING", CARDID, WINDOW->m_workspace ? WINDOW->m_workspace->m_id : WORKSPACE_INVALID, WINDOW->m_class, WINDOW->m_title);
        }
    }

    Membership::renderPass();
}

void CScrollOverview::redrawAll(bool forcelowres) {
    rebuildWorkspaceImages();
    seedRememberedSelections();

    for (const auto& img : images) {
        img->windows.clear();
        img->overflowLeft   = 0.F;
        img->overflowRight  = 0.F;
        img->overflowTop    = 0.F;
        img->overflowBottom = 0.F;
    }
    pinnedFloatingWindows.clear();

    std::unordered_map<WORKSPACEID, SP<SWorkspaceImage>> imagesByWorkspace;
    imagesByWorkspace.reserve(images.size());

    for (const auto& img : images) {
        if (img && img->pWorkspace)
            imagesByWorkspace.emplace(img->pWorkspace->m_id, img);
    }

    std::vector<PHLWINDOW> addedWindows;
    addedWindows.reserve(Desktop::windowState()->windows().size());

    std::vector<PHLWINDOW> addedPinnedFloatingWindows;
    addedPinnedFloatingWindows.reserve(Desktop::windowState()->windows().size());

    // X58(a)'s remaining half needs the ACTIVE card by id, not by pointer: the
    // fallback below hands a homeless promoted window to it.
    const auto MONITOR  = pMonitor.lock();
    const auto ACTIVEWS = MONITOR ? MONITOR->m_activeWorkspace : PHLWORKSPACE{};
    const auto ACTIVEID = ACTIVEWS ? ACTIVEWS->m_id : WORKSPACE_INVALID;

    const auto addOverviewWindow = [&](const PHLWINDOW& window) {
        const auto overviewWindow = getOverviewWindowToShow(window);
        if (!shouldShowOverviewWindow(overviewWindow) || !overviewWindow->m_workspace)
            return;

        if (std::ranges::find(addedWindows, overviewWindow) != addedWindows.end())
            return;

        // Y9, and THIS is the site that mattered. `aae900b` taught the grid
        // LAYOUT that a minimized window belongs to the desktop it came from, so
        // it got a slot -- but the render walks `workspaceImage->windows`, and
        // membership of that list is decided here by workspace id. A stashed
        // window's m_workspace is the stash, the stash owns no card, this lookup
        // missed, and the window was never added to anything and so never drawn.
        // One lookup, not the twenty scattered `m_workspace ==` comparisons a
        // first reading suggested.
        WORKSPACEID homeId = ScrollOverview::Minimize::originalWorkspace(overviewWindow);
        if (homeId == WORKSPACE_INVALID)
            homeId = overviewWindow->m_workspace->m_id;

        auto imageIt = imagesByWorkspace.find(homeId);

        // X58(a), the half Y9 could not reach: THE HOME DESKTOP CAN BE GONE.
        // Minimizing the LAST window of a desktop lets Hyprland reap it -- the
        // same reap Minimize::restore already re-creates around -- so the home
        // id above names a desktop with no card, no image, and no place in a
        // render list that is keyed by card. The window was dropped here, and a
        // query could never bring it back: X58(a)'s "into the grid like any
        // other match" failing on exactly the windows Y9 made findable.
        //
        // Measured on b05b72c (zrig/v1/probeA): the SAME stashed window, same
        // query, viewed from desktop 3 -- with a second window keeping desktop 5
        // alive it promotes at 310720 px; with desktop 5 reaped it is 0 px. And
        // the grid had already solved for it either way, because
        // overviewMainGridWindows scans every window rather than a card's list:
        // the survivor in the reaped run sits a half-tile left of centre with an
        // empty slot beside it. So this was never a membership question -- the
        // layout said the tile was there and only the DRAWING disagreed.
        //
        // The active card is where a promoted tile is laid out (overviewGridSlot)
        // and drawn (its per-card offset is suppressed), so it is the honest
        // owner for one that has no card of its own. Scoped to promotable: with
        // an empty query shouldShowOverviewWindow already dropped the window
        // above, so Y9's "put away stays put away" is untouched, and a window
        // whose home card still exists never reaches this line.
        if (imageIt == imagesByWorkspace.end() && ACTIVEID != WORKSPACE_INVALID && overviewGridPromotable(overviewWindow, MONITOR))
            imageIt = imagesByWorkspace.find(ACTIVEID);

        if (imageIt == imagesByWorkspace.end())
            return;

        addedWindows.emplace_back(overviewWindow);
        imageIt->second->windows.emplace_back(overviewWindow);
    };

    const auto addPinnedFloatingWindow = [&](const PHLWINDOW& window) {
        const auto overviewWindow = getOverviewWindowToShow(window);
        if (!shouldShowPinnedFloatingOverviewWindow(overviewWindow))
            return;

        if (std::ranges::find(addedPinnedFloatingWindows, overviewWindow) != addedPinnedFloatingWindows.end())
            return;

        addedPinnedFloatingWindows.emplace_back(overviewWindow);
        pinnedFloatingWindows.emplace_back(overviewWindow);
    };

    for (const auto& window : Desktop::windowState()->windows()) {
        if (getOverviewWindowToShow(window) != window)
            continue;

        addOverviewWindow(window);
        addPinnedFloatingWindow(window);
    }

    for (const auto& window : Desktop::windowState()->windows()) {
        if (getOverviewWindowToShow(window) == window)
            continue;

        addOverviewWindow(window);
        addPinnedFloatingWindow(window);
    }

    updateWorkspaceOverflow();
}

void CScrollOverview::damage() {
    blockDamageReporting = true;
    g_pHyprRenderer->damageMonitor(pMonitor.lock());
    blockDamageReporting = false;
}

void CScrollOverview::requestInputFrame() {
    if (closing)
        return;

    const auto MONITOR = pMonitor.lock();
    if (!MONITOR)
        return;

    inputFramePending = true;
    MONITOR->scheduleFrame(Aquamarine::IOutput::AQ_SCHEDULE_CURSOR_MOVE);
}

void CScrollOverview::markBlurDirty() {
    frost.overviewDirty = true;
}

void CScrollOverview::markBackdropBlurDirty() {
    frost.backdropDirty = true;
}

bool CScrollOverview::shouldAllowRealtimePreviewFrame() const {
    if (lastRealtimePreviewFrame.time_since_epoch().count() == 0)
        return true;

    return Time::steadyNow() - lastRealtimePreviewFrame >= OVERVIEW_WINDOW_FRAME_INTERVAL;
}

bool CScrollOverview::shouldAllowRealtimePreviewSchedule() {
    if (inputFramePending) {
        inputFramePending = false;
        return true;
    }

    if (selectedWorkspaceFramePending) {
        selectedWorkspaceFramePending = false;
        return true;
    }

    if (closing)
        return true;

    if (scale->isBeingAnimated() || viewOffset->isBeingAnimated() || reflowPacer->isBeingAnimated() || resultsBand->isBeingAnimated() ||
        resultsWidth->isBeingAnimated() || searchDock->isBeingAnimated())
        return true;

    if (realtimePreviewFrameQueued) {
        scheduleRealtimePreviewFrame();
        return false;
    }

    if (shouldAllowRealtimePreviewFrame()) {
        realtimePreviewFrameQueued = true;
        return true;
    }

    scheduleRealtimePreviewFrame();
    return false;
}

void CScrollOverview::schedulePreviewFrameAfter(std::chrono::milliseconds delay) {
    if (!realtimePreviewTimer)
        return;

    const auto DELAY = std::max<int>(1, sc<int>(delay.count()));
    const auto DUE   = Time::steadyNow() + std::chrono::milliseconds(DELAY);

    if (realtimePreviewTimerArmed && realtimePreviewTimerDue <= DUE)
        return;

    realtimePreviewTimerArmed = true;
    realtimePreviewTimerDue   = DUE;
    wl_event_source_timer_update(realtimePreviewTimer, DELAY);
}

void CScrollOverview::scheduleMinimumPreviewFrame() {
    schedulePreviewFrameAfter(getOverviewIdleFrameInterval());
}

void CScrollOverview::scheduleRealtimePreviewFrame() {
    const auto NOW     = Time::steadyNow();
    const auto ELAPSED = lastRealtimePreviewFrame.time_since_epoch().count() == 0 ? OVERVIEW_WINDOW_FRAME_INTERVAL :
                                                                                   std::chrono::duration_cast<std::chrono::milliseconds>(NOW - lastRealtimePreviewFrame);
    const auto DELAY   = OVERVIEW_WINDOW_FRAME_INTERVAL - std::min(ELAPSED, OVERVIEW_WINDOW_FRAME_INTERVAL);
    schedulePreviewFrameAfter(DELAY);
}

int CScrollOverview::realtimePreviewTimerCallback(void* data) {
    const auto OVERVIEW = sc<CScrollOverview*>(data);
    if (!OVERVIEW)
        return 0;

    OVERVIEW->realtimePreviewTimerArmed  = false;
    OVERVIEW->realtimePreviewTimerDue    = {};
    OVERVIEW->realtimePreviewFrameQueued = false;
    OVERVIEW->damage();
    OVERVIEW->scheduleMinimumPreviewFrame();
    return 0;
}

bool CScrollOverview::hasRunningWorkspaceAnimation() const {
    return viewOffset->isBeingAnimated() || workspaceInsertProgress->isBeingAnimated() || workspaceInsertFadeProgress->isBeingAnimated();
}

bool CScrollOverview::shouldSuppressRenderDamage() const {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR || closing)
        return false;

    if (scale->isBeingAnimated() || viewOffset->isBeingAnimated())
        return false;

    const auto ACTIVEIDX = activeWorkspaceIndex();
    const auto SCALE     = scale->value();
    const auto PITCH     = getWorkspaceRenderedPitch(MONITOR, SCALE, layout);
    const auto DRAGGED   = getOverviewWindowToShow(drag.activeWindow.lock());

    const auto isVisibleAnimatedWindow = [&](const PHLWINDOW& window, float workspaceOffset) {
        if (!shouldShowOverviewWindow(window) || window == DRAGGED)
            return false;

        const auto WINDOWBOX = getOverviewWindowBox(window, MONITOR, SCALE, viewOffset->value(), workspaceOffset, layout);
        return overviewBoxIntersectsMonitor(WINDOWBOX, MONITOR) && windowHasOverviewAnimation(window);
    };

    for (const auto& windowRef : pinnedFloatingWindows) {
        const auto window = getOverviewWindowToShow(windowRef.lock());
        if (!shouldShowPinnedFloatingOverviewWindow(window) || window->m_monitor != MONITOR)
            continue;

        if (windowHasOverviewAnimation(window))
            return false;
    }

    for (size_t workspaceIdx = 0; workspaceIdx < images.size(); ++workspaceIdx) {
        const auto& workspaceImage = images[workspaceIdx];
        if (!workspaceImage || !workspaceImage->pWorkspace)
            continue;

        const auto WORKSPACEOFFSET = workspaceOverviewOffset(workspaceIdx, ACTIVEIDX, PITCH);
        const auto WORKSPACEBOX     = getOverviewWorkspaceBox(MONITOR, SCALE, viewOffset->value(), WORKSPACEOFFSET, layout);
        const auto VISIBLEBOX       = workspaceOverviewVisibleBox(workspaceIdx, WORKSPACEBOX, SCALE, MONITOR);
        if (!overviewBoxIntersectsMonitor(VISIBLEBOX, MONITOR))
            continue;

        const auto workspace = workspaceImage->pWorkspace;
        if (!isWorkspaceScrolling(workspace)) {
            const auto fullscreenWindow = getOverviewWindowToShow(Fullscreen::controller()->getFullscreenWindow(workspace));
            if (shouldShowOverviewWindow(fullscreenWindow) && fullscreenWindow->m_workspace == workspace) {
                if (isVisibleAnimatedWindow(fullscreenWindow, WORKSPACEOFFSET))
                    return false;

                for (const auto& windowRef : workspaceImage->windows) {
                    const auto window = getOverviewWindowToShow(windowRef.lock());
                    if (window && window->m_isFloating && isVisibleAnimatedWindow(window, WORKSPACEOFFSET))
                        return false;
                }

                continue;
            }
        }

        for (const auto& windowRef : workspaceImage->windows) {
            const auto window = getOverviewWindowToShow(windowRef.lock());
            if (isVisibleAnimatedWindow(window, WORKSPACEOFFSET))
                return false;
        }
    }

    for (const auto LAYER : {ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM}) {
        for (const auto& layerRef : MONITOR->m_layerSurfaceLayers[LAYER]) {
            if (layerHasOverviewAnimation(layerRef.lock()))
                return false;
        }
    }

    return true;
}

void CScrollOverview::sendOverviewFrameCallbacks(const Time::steady_tp& now) {
    syncPluginAnimEnabled();
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR)
        return;

    const auto ACTIVEIDX = activeWorkspaceIndex();
    const auto SCALE     = scale->value();
    const auto PITCH     = getWorkspaceRenderedPitch(MONITOR, SCALE, layout);
    const auto DRAGGED   = getOverviewWindowToShow(drag.activeWindow.lock());
    const bool CANFRAMETHROTTLEDWINDOWS = closing || shouldAllowRealtimePreviewFrame();
    bool       sentThrottledWindowFrame = false;

    const bool PREVSENDINGFRAMECALLBACKS = sendingOverviewFrameCallbacks;
    sendingOverviewFrameCallbacks        = CANFRAMETHROTTLEDWINDOWS;
    auto resetSendingFrameCallbacks      = Hyprutils::Utils::CScopeGuard([this, PREVSENDINGFRAMECALLBACKS] { sendingOverviewFrameCallbacks = PREVSENDINGFRAMECALLBACKS; });

    const auto frameWindow = [&](const PHLWINDOW& window, float workspaceOffset, bool realtime) {
        if (!shouldShowOverviewWindow(window))
            return;

        const bool ISDRAGGED = window == DRAGGED;
        if (!ISDRAGGED) {
            const auto WINDOWBOX = getOverviewWindowBox(window, MONITOR, SCALE, viewOffset->value(), workspaceOffset, layout);
            if (!overviewBoxIntersectsMonitor(WINDOWBOX, MONITOR))
                return;
        }

        if (!realtime && !ISDRAGGED && !CANFRAMETHROTTLEDWINDOWS) {
            scheduleRealtimePreviewFrame();
            return;
        }

        surfaceTreePresent(window->wlSurface() ? window->wlSurface()->resource() : nullptr, MONITOR, now);
        if (!realtime && !ISDRAGGED)
            sentThrottledWindowFrame = true;
    };

    for (const auto& windowRef : pinnedFloatingWindows) {
        const auto window = getOverviewWindowToShow(windowRef.lock());
        if (!shouldShowPinnedFloatingOverviewWindow(window) || window->m_monitor != MONITOR)
            continue;

        if (!CANFRAMETHROTTLEDWINDOWS) {
            scheduleRealtimePreviewFrame();
            continue;
        }

        surfaceTreePresent(window->wlSurface() ? window->wlSurface()->resource() : nullptr, MONITOR, now);
        sentThrottledWindowFrame = true;
    }

    for (size_t workspaceIdx = 0; workspaceIdx < images.size(); ++workspaceIdx) {
        const auto& workspaceImage = images[workspaceIdx];
        if (!workspaceImage || !workspaceImage->pWorkspace)
            continue;

        const auto WORKSPACEOFFSET = workspaceOverviewOffset(workspaceIdx, ACTIVEIDX, PITCH);
        const auto WORKSPACEBOX     = getOverviewWorkspaceBox(MONITOR, SCALE, viewOffset->value(), WORKSPACEOFFSET, layout);
        const auto VISIBLEBOX       = workspaceOverviewVisibleBox(workspaceIdx, WORKSPACEBOX, SCALE, MONITOR);
        if (!overviewBoxIntersectsMonitor(VISIBLEBOX, MONITOR))
            continue;

        const auto workspace = workspaceImage->pWorkspace;
        const bool REALTIME  = isSelectedWorkspace(workspace);
        if (!isWorkspaceScrolling(workspace)) {
            const auto fullscreenWindow = getOverviewWindowToShow(Fullscreen::controller()->getFullscreenWindow(workspace));
            if (shouldShowOverviewWindow(fullscreenWindow) && fullscreenWindow->m_workspace == workspace) {
                frameWindow(fullscreenWindow, WORKSPACEOFFSET, REALTIME);
                for (const auto& windowRef : workspaceImage->windows) {
                    const auto window = getOverviewWindowToShow(windowRef.lock());
                    if (window && window->m_isFloating)
                        frameWindow(window, WORKSPACEOFFSET, REALTIME);
                }
                continue;
            }
        }

        for (const auto& windowRef : workspaceImage->windows) {
            frameWindow(getOverviewWindowToShow(windowRef.lock()), WORKSPACEOFFSET, REALTIME);
        }
    }

    if (sentThrottledWindowFrame)
        lastRealtimePreviewFrame = now;

    realtimePreviewFrameQueued = false;

    for (const auto LAYER :
         {ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM, ZWLR_LAYER_SHELL_V1_LAYER_TOP, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY}) {
        for (const auto& layerRef : MONITOR->m_layerSurfaceLayers[LAYER]) {
            const auto layer = layerRef.lock();
            if (Desktop::View::validMapped(layer) && surfaceTreeHasFrameCallbacks(layer->wlSurface() ? layer->wlSurface()->resource() : nullptr))
                surfaceTreePresent(layer->wlSurface() ? layer->wlSurface()->resource() : nullptr, MONITOR, now);
        }
    }
}

// The one line that lets a TU without the internals header ask THE gate; see
// the declaration in globals.hpp for why Window.cpp needs it.
bool overviewShowsWindow(const PHLWINDOW& window) {
    return shouldShowOverviewWindow(window);
}

namespace {
struct SSurfaceOwner {
    PHLLS     layer;
    PHLWINDOW windowOwner; // raw owner; the caller applies getOverviewWindowToShow
};

// The surface->owner preamble both frame-pacing entry points carried verbatim:
// resolve a surface to the layer or window that owns it, following a popup to its
// T1 owner. nullopt is the "not ours to pace" early-out they shared -- so their
// identical monitor/surface/view guards collapse to `if (!OWNER) return true`,
// and the popup fallback (a real bug magnet if the two copies ever drifted) lives
// in one place.
std::optional<SSurfaceOwner> resolveSurfaceOwner(const PHLMONITOR& monitor, bool closing, const SP<CWLSurfaceResource>& surface) {
    if (!monitor || closing || !surface)
        return std::nullopt;

    const auto HLSURFACE = Desktop::View::CWLSurface::fromResource(surface);
    if (!HLSURFACE)
        return std::nullopt;

    auto view = HLSURFACE->view();
    if (!view)
        return std::nullopt;

    PHLLS     layerOwner  = Desktop::View::CLayerSurface::fromView(view);
    PHLWINDOW windowOwner = Desktop::View::CWindow::fromView(view);

    if (!layerOwner && !windowOwner) {
        if (const auto POPUP = Desktop::View::CPopup::fromView(view)) {
            if (const auto T1OWNER = POPUP->getT1Owner(); T1OWNER && T1OWNER->view()) {
                layerOwner  = Desktop::View::CLayerSurface::fromView(T1OWNER->view());
                windowOwner = Desktop::View::CWindow::fromView(T1OWNER->view());
            }
        }
    }

    return SSurfaceOwner{layerOwner, windowOwner};
}
} // namespace

bool CScrollOverview::shouldAllowSurfaceFrame(SP<CWLSurfaceResource> surface, const Time::steady_tp& now) {
    const auto MONITOR = pMonitor.lock();
    const auto OWNER   = resolveSurfaceOwner(MONITOR, closing, surface);
    if (!OWNER)
        return true;

    if (OWNER->layer)
        return true;

    auto window = getOverviewWindowToShow(OWNER->windowOwner);
    if (g_pointerGrabOverview && g_pointerGrabOverview->drag.activeWindow &&
        window == getOverviewWindowToShow(g_pointerGrabOverview->drag.activeWindow.lock()))
        return true;

    if (!window || window->m_monitor != MONITOR)
        return true;

    // Y11. THE DOCK IS A SECOND VIEWER OF THIS WINDOW. Everything below decides
    // whether the window is worth drawing IN THE OVERVIEW, and refuses the frame
    // callback when its tile is off screen -- which is right for a tile nobody
    // can see and wrong the moment the same window is in a dock preview.
    //
    // It is not a near miss either: while the overview is up, hkSendFrameEvents-
    // ToWorkspace suppresses Hyprland's own per-workspace dispatch entirely, so
    // this is the ONLY route a frame callback takes. Dock::presentPreview
    // (whose whole job is "the client draws the next frame when the last one is
    // acked") goes through CWLSurfaceResource::frame, which is hooked, which
    // asks this -- so the dock was asking for frames and having them thrown
    // away, and an off-desktop preview was a still. Measured by scripts/
    // y11_rig.sh: 25131 px changed on the screen between two instants and 0 px
    // inside the preview cell.
    //
    // Minimized windows never hit this because their stash workspace fails
    // shouldShowOverviewWindow below and takes the `return true` -- which is why
    // X25's minimized previews measured live and Y11's did not.
    if (ScrollOverview::Dock::isPreviewing(window))
        return true;

    if (shouldShowPinnedFloatingOverviewWindow(window)) {
        if (sendingOverviewFrameCallbacks)
            return true;

        scheduleRealtimePreviewFrame();
        return false;
    }

    if (!shouldShowOverviewWindow(window) || !window->m_workspace)
        return true;

    const auto ACTIVEIDX = activeWorkspaceIndex();
    const auto SCALE     = scale->value();
    const auto PITCH     = getWorkspaceRenderedPitch(MONITOR, SCALE, layout);

    for (size_t workspaceIdx = 0; workspaceIdx < images.size(); ++workspaceIdx) {
        const auto& workspaceImage = images[workspaceIdx];
        if (!workspaceImage || workspaceImage->pWorkspace != window->m_workspace)
            continue;

        if (!isWorkspaceScrolling(workspaceImage->pWorkspace)) {
            const auto fullscreenWindow = getOverviewWindowToShow(Fullscreen::controller()->getFullscreenWindow(workspaceImage->pWorkspace));
            if (shouldShowOverviewWindow(fullscreenWindow) && fullscreenWindow->m_workspace == workspaceImage->pWorkspace && fullscreenWindow != window && !window->m_isFloating)
                return false;
        }

        const auto WORKSPACEOFFSET = workspaceOverviewOffset(workspaceIdx, ACTIVEIDX, PITCH);
        const auto WINDOWBOX        = getOverviewWindowBox(window, MONITOR, SCALE, viewOffset->value(), WORKSPACEOFFSET, layout);
        if (!overviewBoxIntersectsMonitor(WINDOWBOX, MONITOR))
            return false;

        if (isSelectedWorkspace(workspaceImage->pWorkspace))
            return true;

        if (sendingOverviewFrameCallbacks)
            return true;

        scheduleRealtimePreviewFrame();
        return false;
    }

    return false;
}

bool CScrollOverview::shouldHandleSurfaceDamage(SP<CWLSurfaceResource> surface) {
    const auto MONITOR = pMonitor.lock();
    const auto OWNER   = resolveSurfaceOwner(MONITOR, closing, surface);
    if (!OWNER)
        return true;

    const auto layerOwner  = OWNER->layer;
    const auto windowOwner = OWNER->windowOwner;

    if (layerOwner) {
        if (layerOwner->m_monitor != MONITOR)
            return true;

        if (layerOwner->m_layer > ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM)
            return false;

        markBlurDirty();
        if (layerOwner->m_layer == ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND)
            markBackdropBlurDirty();
        return true;
    }

    if (!windowOwner)
        return true;

    auto window = getOverviewWindowToShow(windowOwner);
    if (g_pointerGrabOverview && g_pointerGrabOverview->drag.activeWindow &&
        window == getOverviewWindowToShow(g_pointerGrabOverview->drag.activeWindow.lock())) {
        const auto DRAGBOX = g_pointerGrabOverview->draggedWindowGlobalBox();
        if (!DRAGBOX.empty() && !DRAGBOX.intersection(MONITOR->logicalBox()).empty())
            damage();
        return true;
    }

    if (shouldShowPinnedFloatingOverviewWindow(window)) {
        if (window->m_monitor != MONITOR)
            return true;

        if (!realtimePreviewFrameQueued && shouldAllowRealtimePreviewFrame())
            return true;

        scheduleRealtimePreviewFrame();
        return false;
    }

    if (window && window->m_monitor != MONITOR)
        return true;

    if (!shouldShowOverviewWindow(window) || !window->m_workspace)
        return false;

    const auto ACTIVEIDX = activeWorkspaceIndex();
    const auto SCALE     = scale->value();
    const auto PITCH     = getWorkspaceRenderedPitch(MONITOR, SCALE, layout);

    for (size_t workspaceIdx = 0; workspaceIdx < images.size(); ++workspaceIdx) {
        const auto& workspaceImage = images[workspaceIdx];
        if (!workspaceImage || workspaceImage->pWorkspace != window->m_workspace)
            continue;

        if (!isWorkspaceScrolling(workspaceImage->pWorkspace)) {
            const auto fullscreenWindow = getOverviewWindowToShow(Fullscreen::controller()->getFullscreenWindow(workspaceImage->pWorkspace));
            if (shouldShowOverviewWindow(fullscreenWindow) && fullscreenWindow->m_workspace == workspaceImage->pWorkspace && fullscreenWindow != window && !window->m_isFloating)
                return false;
        }

        const auto WORKSPACEOFFSET = workspaceOverviewOffset(workspaceIdx, ACTIVEIDX, PITCH);
        const auto WINDOWBOX        = getOverviewWindowBox(window, MONITOR, SCALE, viewOffset->value(), WORKSPACEOFFSET, layout);
        if (!overviewBoxIntersectsMonitor(WINDOWBOX, MONITOR))
            return false;

        if (isSelectedWorkspace(workspaceImage->pWorkspace)) {
            selectedWorkspaceFramePending = true;
            return true;
        }

        if (!realtimePreviewFrameQueued && shouldAllowRealtimePreviewFrame())
            return true;

        scheduleRealtimePreviewFrame();
        return false;

    }

    return false;
}

void CScrollOverview::close() {
    // Set at the top, before the dock teardown below, so it doubles as the
    // guard against a second close() -- setClosing(true) further down re-affirms
    // it (idempotent) alongside the state teardown it also owns.
    if (closing) {
        // A close-swipe used to setClosing(true) before calling close(), so
        // this guard fired and left an invisible overview owning the monitor.
        // If we are marked closing but nothing is actually animating out,
        // finish the teardown so Super / a second swipe can always get out.
        m_isSwiping    = false;
        m_swipeClosing = false;
        if (!scale || !scale->isBeingAnimated())
            removeOverview(this);
        return;
    }
    closing = true;

    // X28. The pill only exists while the overview does, so its menu must go
    // with it -- otherwise a stale plate is still holding every pointer press
    // the next time the overview opens.
    ScrollOverview::Dock::closeMenu();

    // X32/G12. Same reasoning for both drags: the overview can close under a
    // held button (Escape, a keybind, a window taking focus), and the release
    // that would have ended the gesture then never reaches this hook. Neither
    // may survive into the next open as a phantom drop.
    ScrollOverview::Dock::dragCancel();
    ScrollOverview::Dock::setForeignInsert(-1);
    OmarchySearch::closeSushi();

    // Click-to-close and swipe-commit never go through the dispatcher, so a
    // typed query used to still be in the bar on the next open. Ending the
    // last overview ends the search session: reopen is empty, and the close
    // animation unfilters instead of popping hidden windows back after it.
    // A second monitor still showing the same session must not lose it.
    if (scrollOverviews().size() <= 1)
        resetSearchSession();
    else
        g_search.endInteraction();

    setClosing(true);

    // P0 failsafe. The monitor-removed teardown in PLUGIN_INIT is the fix; this
    // is the belt behind it, because a close() that reads a dead monitor kills
    // the compositor and there is no cheaper way to be sure no path ever gets
    // here. AFTER setClosing, so the state teardown that call owns -- the
    // shared-state handover, the submap, a held scrolling pan -- still runs;
    // everything BELOW is the part that wants the monitor: its active workspace,
    // its logical pitch, changeWorkspace. With the output gone there is no close
    // animation to play and nothing to play it on, so skip to the teardown the
    // animation's end callback would have reached -- parked and freed one tick
    // later, exactly as it would have been. A silent no-op close on a dead
    // monitor is the correct answer; a crash is not.
    if (!pMonitor.lock()) {
        removeOverview(this);
        return;
    }

    const auto SELECTEDWORKSPACE =
        viewportCurrentWorkspace < images.size() && images[viewportCurrentWorkspace] ? images[viewportCurrentWorkspace]->pWorkspace : PHLWORKSPACE{};

    const auto finishClose = [&](const PHLWORKSPACE& finalWorkspace, const PHLWINDOW& finalWindow) {
        emitFullscreenVisibilityState(getOverviewFullscreenVisibilityWindow(finalWorkspace, finalWindow), false);

        useAnimDirection(false);
        *scale = 1.F;

        // Always recalc decorations on close. Skipping this when animations
        // were on left window frames stuck at the overview tile (often the
        // top-left) after the overview was gone.
        forceWorkspaceWindowsDecoRecalc(finalWorkspace ? finalWorkspace : pMonitor->m_activeWorkspace);
        damage();

        scale->setCallbackOnEnd([this](auto) { removeOverview(this); });
    };

    if (!closeOnWindow && (!SELECTEDWORKSPACE || SELECTEDWORKSPACE == pMonitor->m_activeWorkspace)) {
        const auto FOCUSEDWINDOW = getOverviewWindowToShow(Desktop::focusState()->window());
        if (!SELECTEDWORKSPACE || (FOCUSEDWINDOW && FOCUSEDWINDOW->m_workspace == SELECTEDWORKSPACE))
            closeOnWindow = FOCUSEDWINDOW;
    }

    closeOnWindow = getOverviewWindowToShow(closeOnWindow.lock());

    // Y9 + the section 5 checklist: "Click / activate tile: focus + raise that
    // window, close overview." Y9 put minimized windows in the grid on the
    // desktop they were minimized FROM, so a query can legitimately select one
    // -- and then every workspace decision below reads m_workspace, which for a
    // stashed window is `special:minimized`. changeWorkspace onto a special
    // workspace does nothing, so the overview closed, the workspace did not
    // move and the window stayed stashed: the click was dead. Measured before
    // this: workspace unchanged, still minimized.
    //
    // Un-stash FIRST and the rest of this function needs no special case at
    // all: m_workspace is a real desktop again, the branches below commit an
    // ordinary selection onto it, and the close animation carries the tile to
    // the window's restored rect -- which is exactly the restore the eye
    // expects, without a second flight fighting the close for the same 300ms.
    //
    // Minimize::restore is the house's ONE answer to "something named a
    // minimized window and asked to go to it" -- Dock::activateWindow, the X28
    // menu's window rows and the hover panel's click all call it, with the same
    // note that a stashed window is restored rather than selected because the
    // stash owns no card to land a selection on. This is the fourth caller, not
    // a fourth answer.
    if (closeOnWindow && ScrollOverview::Minimize::isMinimized(closeOnWindow.lock()))
        ScrollOverview::Minimize::restore(closeOnWindow.lock());

    // X43: focus and stacking are two different things. fullWindowFocus() only
    // makes a window active -- it never restacks -- so a selected floating
    // window stayed underneath whatever was covering it. Raise here, at the one
    // point where the overview commits its selection, rather than in
    // syncFocusedSelection(): that runs on every keyboard navigation step and
    // would reshuffle the stack while the user is merely browsing.
    if (closeOnWindow)
        Desktop::windowState()->raise(closeOnWindow.lock());

    if (closeOnWindow && focusSyncedFromWorkspaceID != WORKSPACE_INVALID) {
        const auto FINALWORKSPACE = closeOnWindow->m_workspace;
        size_t     sourceIdx      = images.size();
        size_t     targetIdx      = images.size();

        for (size_t workspaceIdx = 0; workspaceIdx < images.size(); ++workspaceIdx) {
            if (!images[workspaceIdx] || !images[workspaceIdx]->pWorkspace)
                continue;

            if (images[workspaceIdx]->pWorkspace->m_id == focusSyncedFromWorkspaceID)
                sourceIdx = workspaceIdx;
            if (images[workspaceIdx]->pWorkspace == FINALWORKSPACE)
                targetIdx = workspaceIdx;
        }

        if (sourceIdx < images.size() && targetIdx < images.size()) {
            if (FINALWORKSPACE != pMonitor->m_activeWorkspace)
                pMonitor->changeWorkspace(FINALWORKSPACE, false, true, true);

            Desktop::focusState()->fullWindowFocus(closeOnWindow.lock(), Desktop::FOCUS_REASON_DESKTOP_STATE_CHANGE);

            startedOn                = FINALWORKSPACE;
            viewportCurrentWorkspace = targetIdx;

            const auto FINALPITCH = getWorkspaceLogicalPitch(pMonitor.lock(), 1.F, layout);
            viewOffset->setValueAndWarp(axisOffsetVector(workspaceOverviewLogicalOffset(sourceIdx, targetIdx, FINALPITCH), layout));
            *viewOffset = Vector2D{};

            focusSyncedFromWorkspaceID = WORKSPACE_INVALID;

            const auto FINALWINDOW = getOverviewWindowToShow(closeOnWindow.lock());
            finishClose(FINALWORKSPACE, FINALWINDOW);
            restoreOverviewKeyboard(FINALWINDOW);
            return;
        }
    }

    if (!closeOnWindow) {
        const auto ACTIVEIDX = activeWorkspaceIndex();
        const auto FINALPITCH = getWorkspaceLogicalPitch(pMonitor.lock(), 1.F, layout);
        *viewOffset = Vector2D{};

        for (size_t workspaceIdx = 0; workspaceIdx < images.size(); ++workspaceIdx) {
            if (!images[workspaceIdx] || images[workspaceIdx]->pWorkspace != SELECTEDWORKSPACE)
                continue;

            *viewOffset = axisOffsetVector(workspaceOverviewLogicalOffset(workspaceIdx, ACTIVEIDX, FINALPITCH), layout);
            break;
        }

        if (SELECTEDWORKSPACE && SELECTEDWORKSPACE != pMonitor->m_activeWorkspace)
            pMonitor->changeWorkspace(SELECTEDWORKSPACE, false, true, true);
    } else if (closeOnWindow == Desktop::focusState()->window() && closeOnWindow->m_workspace == pMonitor->m_activeWorkspace) {
        if (focusSyncedFromWorkspaceID != WORKSPACE_INVALID) {
            const auto ACTIVEIDX   = activeWorkspaceIndex();
            const auto FINALPITCH = getWorkspaceLogicalPitch(pMonitor.lock(), 1.F, layout);

            for (size_t workspaceIdx = 0; workspaceIdx < images.size(); ++workspaceIdx) {
                if (!images[workspaceIdx] || !images[workspaceIdx]->pWorkspace || images[workspaceIdx]->pWorkspace->m_id != focusSyncedFromWorkspaceID)
                    continue;

                viewOffset->setValueAndWarp(axisOffsetVector(workspaceOverviewLogicalOffset(workspaceIdx, ACTIVEIDX, FINALPITCH), layout));
                break;
            }
        }

        *viewOffset = Vector2D{};
    } else {

        if (closeOnWindow->m_workspace != pMonitor->m_activeWorkspace)
            pMonitor->changeWorkspace(closeOnWindow->m_workspace, false, true, true);

        Desktop::focusState()->fullWindowFocus(closeOnWindow.lock(), Desktop::FOCUS_REASON_DESKTOP_STATE_CHANGE);

        const auto ACTIVEIDX = activeWorkspaceIndex();
        const auto FINALPITCH = getWorkspaceLogicalPitch(pMonitor.lock(), 1.F, layout);
        bool       found      = false;
        const auto selectedWindow = getOverviewWindowToShow(closeOnWindow.lock());
        for (size_t workspaceIdx = 0; workspaceIdx < images.size(); ++workspaceIdx) {
            const auto& wimg = images[workspaceIdx];
            for (const auto& windowRef : wimg->windows) {
                const auto window = getOverviewWindowToShow(windowRef.lock());
                if (window == selectedWindow && window) {
                    *viewOffset = axisOffsetVector(workspaceOverviewLogicalOffset(workspaceIdx, ACTIVEIDX, FINALPITCH), layout);
                    found = true;
                    break;
                }
            }
            if (found)
                break;
        }
    }

    focusSyncedFromWorkspaceID = WORKSPACE_INVALID;

    const auto FINALWINDOW    = getOverviewWindowToShow(closeOnWindow.lock());
    const auto FINALWORKSPACE = FINALWINDOW ? FINALWINDOW->m_workspace : SELECTEDWORKSPACE;
    finishClose(FINALWORKSPACE, FINALWINDOW);
    restoreOverviewKeyboard(FINALWINDOW);
}

void CScrollOverview::onPreRender() {
    if (pMonitor)
        pMonitor->m_solitaryClient.reset();

    // A grid reflow runs on a wall clock, not a Hyprland animation variable, so
    // nothing else asks for the next frame. Re-arming an animvar hands pacing to
    // the animation manager, which damages per tick exactly like the open
    // animation, instead of requesting a frame from inside the frame being
    // rendered. The flag is re-set while this frame evaluates its boxes, so the
    // chain stops on its own.
    // ponytail: this is the tidier mechanism, NOT a measured speedup. A 3-vs-4
    // run A/B on a liveness-certified headless output put the reflow at 27.5fps
    // median before and 32.9fps after -- overlapping spreads, no significant
    // difference. Presented frames sit at a 17ms median gap in BOTH builds.
    // The "intermittent 115-240ms stall" this comment used to send you after is
    // an ARTIFACT: rf_analyze.py groups frames into bursts with a 250ms gap, so
    // the burst swallows the idle screen before and after the animation, and
    // those idle gaps become its "worst". Windowed to the animation itself the
    // reflow presents ~57fps on a 60Hz output. There is no stall to chase.
    // Rig: share/reflow_measure.sh + share/rf_analyze.py -- window the timestamps
    // to the motion before reading a worst-case gap out of them.
    //
    // Churn and the shrink direction cost nothing on top of that: six keystrokes
    // 200ms apart, each swinging the results band its full 146px, presented
    // 57.5fps with 2 of 96 gaps over 34ms -- indistinguishable from one isolated
    // transition (57.2fps, 1 of 35). Collapsing the band back to zero mirrors
    // growing it, monotonic at 9.6% of travel per step, and strip_top/tile_h
    // land back on their exact pre-query values.
    // Rig: share/x23_churn.sh + share/x23_churn_analyze.py. Track the LAYOUT
    // (strip_top, tile_h), not pixel diffs -- strip_top is quantised to 2px, so
    // it holds still through the eased tail and reading THAT as frame timing
    // invents a 196ms stall which the painted-frame signal shows is not there.
    if (TileChrome::animating())
        grid().reflowActive = true;

    // V2. The SUPER-hold pill eases on the same kind of wall clock, and inside
    // an overview it needs the frames asked for the same way: hkScheduleFrame
    // throttles AQ_SCHEDULE_DAMAGE for as long as an overview is open, so the
    // dock's pumpFrame -- a plain damageMonitor -- is swallowed here and the
    // reveal would freeze half out over a still cursor. Joining the reflow
    // pacer rather than adding a fourth pump, for the reason Dock::needsFrames
    // gives: one list, in one place.
    if (WindowPill::animating())
        grid().reflowActive = true;

    if (grid().reflowActive) {
        grid().reflowActive = false;
        reflowPacer->setValueAndWarp(0.F);
        *reflowPacer = 1.F;
        damage();
    }

    forced.forceLayersAboveFullscreen(pMonitor.lock());
    updateWorkspaceOverflow();

    if (closing)
        return;

    if (pMonitor && pMonitor->m_activeWorkspace && pMonitor->m_activeWorkspace != startedOn) {
        rebuildPending = false;
        markBlurDirty();
        onWorkspaceChange();
        focusSyncedFromWorkspaceID = WORKSPACE_INVALID;
        emitFullscreenVisibilityState(Desktop::focusState()->window(), true);
        return;
    }

    focusSyncedFromWorkspaceID = WORKSPACE_INVALID;

    // Y9. A QUERY STARTING OR ENDING CHANGES WHICH WINDOWS EXIST. 50197ec put the
    // rule in shouldShowOverviewWindow -- a minimized window is in the overview
    // only while a query is running -- but the window list is built when the
    // overview opens and rebuilt only on demand, and every open starts
    // unfiltered. So a minimized window was excluded at open time and stayed
    // excluded no matter what was typed: measured with scripts/y9_rig.sh, the
    // filter was plainly live (the non-matching fixture fell from 521403 px to
    // 53) while the minimized one sat at 0 px throughout. That made a minimized
    // window unreachable from the overview, which is worse than not hiding it.
    //
    // The EDGE, not every keystroke. Empty <-> non-empty is the only transition
    // that changes the answer, and X52 is about what a keystroke costs.
    if (const bool QUERYEMPTY = g_search.query.text.empty(); QUERYEMPTY != lastQueryEmpty) {
        lastQueryEmpty = QUERYEMPTY;
        rebuildPending = true;
    }

    if (rebuildPending) {
        rebuildPending = false;
        markBlurDirty();
        redrawAll();
        syncSelectionToViewport();
        damage();
        renderListShadow();
        return;
    }

    renderListShadow();
}

void CScrollOverview::onWorkspaceChange() {
    if (!pMonitor || !pMonitor->m_activeWorkspace)
        return;

    const auto previousActiveIdx = activeWorkspaceIndex();
    const auto previousStartedOn = startedOn;

    // consume any pending gesture-driven settle (set by finishWorkspaceScrollFollow)
    const bool   GESTURESETTLE       = trackpadGestureSettlePending;
    const double GESTURESETTLEOFFSET = trackpadGestureSettleOffset;
    trackpadGestureSettlePending     = false;

    std::vector<WORKSPACEID> previousWorkspaceIDs;
    previousWorkspaceIDs.reserve(images.size());
    std::unordered_map<WORKSPACEID, float> previousWorkspaceOffsets;
    const auto PREVIOUSLOGICALPITCH = getWorkspaceLogicalPitch(pMonitor.lock(), scale->value(), layout);
    for (size_t i = 0; i < images.size(); ++i) {
        const auto& image = images[i];
        if (!image || !image->pWorkspace)
            continue;

        previousWorkspaceIDs.push_back(image->pWorkspace->m_id);
        previousWorkspaceOffsets.emplace(image->pWorkspace->m_id, workspaceOverviewLogicalOffset(i, previousActiveIdx, PREVIOUSLOGICALPITCH));
    }

    const auto NEWWORKSPACE      = pMonitor->m_activeWorkspace;
    const bool INSERTEDWORKSPACE = std::find(previousWorkspaceIDs.begin(), previousWorkspaceIDs.end(), NEWWORKSPACE->m_id) == previousWorkspaceIDs.end();
    const auto REQUESTEDREMOVEDWORKSPACE = pendingRemovedWorkspace.lock();
    // E8-d/E8-e: the E2 trailing workspace is empty on purpose, so upstream's
    // "collapse the empty workspace you just left" rule would delete its card the
    // first time you scroll onto it and back off -- and pendingRemovedWorkspace is
    // sticky, so the card never returns even though the workspace is still alive.
    // A gap workspace is empty BY DEFINITION, so the same rule would reap it the
    // first time you scroll across it and the strip would lose the cell again.
    const bool PREVIOUSISOURS = previousStartedOn &&
        (previousStartedOn == trailingWorkspace ||
         std::find(gapWorkspaces.begin(), gapWorkspaces.end(), previousStartedOn) != gapWorkspaces.end());
    const bool SHOULDREMOVEPREVIOUSWORKSPACE = previousStartedOn && previousStartedOn != NEWWORKSPACE && !PREVIOUSISOURS &&
        !previousStartedOn->m_isSpecialWorkspace && !previousStartedOn->isPersistent() && previousStartedOn->getWindowCount() == 0;
    const auto REMOVEDWORKSPACE = REQUESTEDREMOVEDWORKSPACE ? REQUESTEDREMOVEDWORKSPACE : SHOULDREMOVEPREVIOUSWORKSPACE ? previousStartedOn : PHLWORKSPACE{};

    pendingRemovedWorkspace = REMOVEDWORKSPACE;

    startedOn = NEWWORKSPACE;
    redrawAll();
    viewportCurrentWorkspace = activeWorkspaceIndex();

    const bool REMOVEDPREVIOUSWORKSPACE =
        REMOVEDWORKSPACE &&
        std::find_if(images.begin(), images.end(), [REMOVEDWORKSPACE](const auto& image) { return image && image->pWorkspace == REMOVEDWORKSPACE; }) == images.end();

    if (INSERTEDWORKSPACE || REMOVEDPREVIOUSWORKSPACE) {
        workspaceInsertTransition.active                 = true;
        workspaceInsertTransition.transitionWorkspaceID  = INSERTEDWORKSPACE ? NEWWORKSPACE->m_id : REMOVEDWORKSPACE->m_id;
        workspaceInsertTransition.transitionFadeIn       = INSERTEDWORKSPACE;
        workspaceInsertFadeProgress->setConfig(INSERTEDWORKSPACE ? workspaceInsertFadeConfig : workspaceRemoveFadeConfig);
        workspaceInsertTransition.oldRelativeOffsets.clear();
        workspaceInsertTransition.newRelativeOffsets.clear();
        workspaceInsertTransition.transitionOldRelativeOffset = 0.F;

        for (size_t i = 0; i < previousWorkspaceIDs.size(); ++i) {
            const auto OFFSET = previousWorkspaceOffsets.contains(previousWorkspaceIDs[i]) ? previousWorkspaceOffsets.at(previousWorkspaceIDs[i]) : 0.F;
            workspaceInsertTransition.oldRelativeOffsets.emplace(previousWorkspaceIDs[i], OFFSET);
            if (REMOVEDPREVIOUSWORKSPACE && previousWorkspaceIDs[i] == REMOVEDWORKSPACE->m_id)
                workspaceInsertTransition.transitionOldRelativeOffset = OFFSET;
        }

        const auto NEWLOGICALPITCH = getWorkspaceLogicalPitch(pMonitor.lock(), scale->value(), layout);
        for (size_t i = 0; i < images.size(); ++i) {
            if (!images[i] || !images[i]->pWorkspace)
                continue;

            workspaceInsertTransition.newRelativeOffsets.emplace(images[i]->pWorkspace->m_id, workspaceOverviewLogicalOffset(i, viewportCurrentWorkspace, NEWLOGICALPITCH));
        }

        workspaceInsertProgress->setValueAndWarp(0.F);
        workspaceInsertFadeProgress->setValueAndWarp(0.F);
        *workspaceInsertProgress = 1.F;
        *workspaceInsertFadeProgress = 1.F;
        viewOffset->setValueAndWarp(Vector2D{});
        *viewOffset = Vector2D{};
    } else {
        workspaceInsertTransition.active                 = false;
        workspaceInsertTransition.transitionWorkspaceID  = WORKSPACE_INVALID;
        workspaceInsertTransition.transitionFadeIn       = true;
        workspaceInsertFadeProgress->setConfig(workspaceInsertFadeConfig);
        workspaceInsertTransition.oldRelativeOffsets.clear();
        workspaceInsertTransition.newRelativeOffsets.clear();
        workspaceInsertTransition.transitionOldRelativeOffset = 0.F;
        workspaceInsertProgress->setValueAndWarp(1.F);
        workspaceInsertFadeProgress->setValueAndWarp(1.F);
        if (GESTURESETTLE) // a trackpad follow committed this change: settle from the drag position, not a full pitch
            viewOffset->setValueAndWarp(axisOffsetVector(sc<float>(GESTURESETTLEOFFSET), layout));
        else
            viewOffset->setValueAndWarp(
            axisOffsetVector(workspaceOverviewLogicalOffset(previousActiveIdx, viewportCurrentWorkspace, getWorkspaceLogicalPitch(pMonitor.lock(), scale->value(), layout)), layout));
        *viewOffset = Vector2D{};
    }

    syncSelectionToViewport();
    markBlurDirty();
    damage();
}

void CScrollOverview::render() {
    Prof::Timer PROF{Prof::frameNs, Prof::frameCalls};
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR)
        return;

    ++g_overviewFrame;

    if (g_pointerGrabOverview && g_pointerGrabOverview != this && g_pointerGrabOverview->drag.activeWindow && isOverviewPointerOnMonitor(MONITOR))
        lastMousePosLocal = getOverviewMousePosLocal(MONITOR);

    const bool PREVBLOCKSURFACEFEEDBACK       = g_pHyprRenderer->m_bBlockSurfaceFeedback;
    g_pHyprRenderer->m_bBlockSurfaceFeedback  = true;
    auto restoreSurfaceFeedback               = Hyprutils::Utils::CScopeGuard([PREVBLOCKSURFACEFEEDBACK] { g_pHyprRenderer->m_bBlockSurfaceFeedback = PREVBLOCKSURFACEFEEDBACK; });

    const auto NOW       = Time::steadyNow();
    const auto ACTIVEIDX = activeWorkspaceIndex();
    const auto SCALE     = scale->value();
    const auto PITCH     = getWorkspaceRenderedPitch(MONITOR, SCALE, layout);

    const auto VIEWOFFSET = viewOffset->value();
    if (!frost.overviewStateValid || std::abs(frost.lastOverviewScale - SCALE) > 0.001F || frost.lastOverviewViewOffset.distanceSq(VIEWOFFSET) > 0.001F) {
        markBlurDirty();
        frost.overviewStateValid     = true;
        frost.lastOverviewScale      = SCALE;
        frost.lastOverviewViewOffset = VIEWOFFSET;
    }

    const auto WALLPAPERMODE = ScrollOverview::Config::getWallpaperMode();

    if (WALLPAPERMODE == 0 || WALLPAPERMODE == 2)
        renderGlobalWallpaper(MONITOR, NOW);
    else
        g_pHyprRenderer->draw(CClearPassElement::SClearData{CHyprColor{0.F, 0.F, 0.F, 1.F}}, {});

    Event::bus()->m_events.render.stage.emit(RENDER_POST_WALLPAPER);

    // Commit the queued backdrop to the screen BEFORE the temp-FB work below:
    // flushing inside the bind would swallow those elements into the FB (that
    // exact bug blacked out the whole backdrop once). And the FB fill has to
    // happen HERE, not at the top of render() -- layer surfaces demonstrably
    // render at this stage and not earlier (empty-FB bug, take two).
    OverviewRender::flushPass(MONITOR);
    updateCardWallpaperFB(MONITOR, NOW);

    // The wallpaper canvas is still the monitor fill; the cards float on top
    // of it. Blur it the way the Omarchy lockscreen blurs its wallpaper, then
    // dim 80% black, both faded with open progress so a swipe does not slam
    // them on. Cards keep the undimmed, unblurred fill captured above.
    // Flush BEFORE the cards so the scrim cannot land on top of the card fill.
    {
        const float OPEN     = ScrollOverview::Config::getScale();
        const float RANGE    = std::max(0.01F, std::abs(1.F - OPEN));
        const float PROGRESS = std::clamp(std::abs(SCALE - 1.F) / RANGE, 0.F, 1.F);
        const float DIM      = 0.80F * PROGRESS;
        if (PROGRESS > 0.001F && WALLPAPERMODE != 1) {
            updateBackdropBlurCache(MONITOR, WALLPAPERMODE, NOW);
            renderBackdropBlurCache(MONITOR, PROGRESS);
        }
        if (DIM > 0.001F) {
            CRectPassElement::SRectData scrim;
            scrim.box   = CBox{0, 0, MONITOR->m_transformedSize.x, MONITOR->m_transformedSize.y};
            scrim.color = CHyprColor{0.F, 0.F, 0.F, DIM};
            g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(scrim));
            OverviewRender::flushPass(MONITOR);
        }
    }

    {
        Prof::Timer PROFBG{Prof::bgNs, Prof::bgCalls};
        for (size_t workspaceIdx = 0; workspaceIdx < images.size(); ++workspaceIdx) {
            renderWorkspaceBackground(MONITOR, workspaceIdx, ACTIVEIDX, PITCH, SCALE, WALLPAPERMODE, NOW);
        }
    }

    if (workspaceInsertTransition.active && !workspaceInsertTransition.transitionFadeIn) {
        const auto GHOSTALPHA   = 1.F - std::clamp(workspaceInsertFadeProgress->value(), 0.F, 1.F);
        const auto GHOSTOFFSET = workspaceInsertTransition.transitionOldRelativeOffset * SCALE * MONITOR->m_scale;
        const auto GHOSTBOX     = getOverviewWorkspaceBox(MONITOR, SCALE, viewOffset->value(), GHOSTOFFSET, layout);

        if (GHOSTALPHA > 0.001F && overviewBoxIntersectsMonitor(GHOSTBOX, MONITOR)) {
            renderOverviewWorkspaceShadow(MONITOR, GHOSTBOX, SCALE, WALLPAPERMODE == 0, GHOSTALPHA);

            if (WALLPAPERMODE != 0)
                renderWallpaperLayers(MONITOR, GHOSTBOX, SCALE, NOW, GHOSTALPHA);

            renderOverviewLayerLevel(MONITOR, ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM, GHOSTBOX, SCALE, NOW);
        }
    }

    const bool NEEDS_PRECOMPUTED_BLUR = hasVisiblePrecomputedBlurWindow(MONITOR, ACTIVEIDX, PITCH, SCALE);
    if (NEEDS_PRECOMPUTED_BLUR && frost.overviewDirty)
        g_pHyprRenderer->m_renderPass.add(makeUnique<CPreBlurElement>());

    OverviewRender::flushPass(MONITOR);

    if (NEEDS_PRECOMPUTED_BLUR)
        frost.overviewDirty = false;

    {
        Prof::Timer PROFLIVE{Prof::liveNs, Prof::liveCalls};
        for (size_t workspaceIdx = 0; workspaceIdx < images.size(); ++workspaceIdx) {
            renderWorkspaceLive(MONITOR, workspaceIdx, ACTIVEIDX, PITCH, SCALE, WALLPAPERMODE, NOW);
        }
    }

    OverviewRender::flushPass(MONITOR);

    // X92 / X97. Blur the overview main FB (cards + tiles) into a PRIVATE FB.
    // Gate on decoration:blur:enabled — the same switch window frost uses — NOT
    // plugin:scrolloverview:blur (that only means wallpaper backdrop blur and
    // defaults false). X96 made strip sample this FB; gating on the wallpaper
    // option wiped strip + search-result frost entirely.
    if (ScrollOverview::Config::getValue<bool>("decoration:blur:enabled"))
        OverviewRender::captureFrameChromeBlur(MONITOR);
    else
        OverviewRender::clearFrameChromeBlur();

    {
        Prof::Timer PROFSTRIP{Prof::stripNs, Prof::stripCalls};
        renderStrip(MONITOR, SCALE, NOW);
    }
    // X87: search (and its frost) under the drag proxy. Previously renderSearch
    // ran after renderDraggedWindow, so the pill painted over the window you
    // were dragging -- empty or with results.
    renderSearch(MONITOR, SCALE);
    renderPinnedFloatingWindows(MONITOR, SCALE, NOW);

    if (OmarchySearch::isSushiActive()) {
        for (const auto& w : Desktop::windowState()->windows()) {
            if (Desktop::View::validMapped(w) && OmarchySearch::isOverviewSushiWindow(w->m_class, w->getPID())) {
                if (w->m_monitor == MONITOR) {
                    g_pHyprRenderer->renderWindow(w, MONITOR, NOW, false, Render::RENDER_PASS_ALL, false, false);
                }
                break;
            }
        }
    }

    renderDraggedWindow(MONITOR, ACTIVEIDX, PITCH, SCALE, NOW);

    const size_t FIRSTLAYERELEMENT = g_pHyprRenderer->m_renderPass.m_passElements.size();

    for (const auto LAYER : {ZWLR_LAYER_SHELL_V1_LAYER_TOP, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY}) {
        for (auto const& ls : MONITOR->m_layerSurfaceLayers[LAYER]) {
            if (!Desktop::View::validMapped(ls.lock()))
                continue;

            g_pHyprRenderer->renderLayer(ls.lock(), MONITOR, NOW);
        }
    }

    blockOverviewLayerBlurOptimization(FIRSTLAYERELEMENT);

    // OVERVIEW_CPP_SPEC §10 clause 6, the pinned-in-overview half X27 left open:
    // the dock stays on screen with the overview over it. Its RENDER_POST_WINDOWS
    // hook fires far below this, so the overview buried it -- and X26 cannot ask
    // the user to drop a window on a target that is not drawn. Last, so it is
    // above the cards, the strip and the search field, which is where a dock the
    // user is dropping onto belongs. main.cpp skips the desktop hook while an
    // overview owns the monitor, so the pill is still drawn exactly once.
    // Y7. The pill glows as a dragged window nears it; the dock computes the
    // distance itself, this only says whether a drag is happening at all.
    ScrollOverview::Dock::setWindowDragActive(drag.activeWindow.operator bool());
    ScrollOverview::Dock::render(MONITOR);
    // Hover titles hang off the bottom of a tile, so they collide with the
    // dock on the active card. Flush them after the pill so the label is the
    // thing you read, not the thing the dock covers.
    TileChrome::renderCaptions(MONITOR);

    // X52. One line per frame after a keystroke: the frame that PAINTS the new
    // results is frame 1, so its total is the keystroke-to-paint cost.
    if (Prof::enabled() && Prof::pending) {
        ++Prof::frames;
        const auto SINCEKEY = std::chrono::duration<double, std::milli>(Prof::clock::now() - Prof::keyAt).count();
        if (FILE* const F = std::fopen("/tmp/hyprdrive-prof.log", "a")) {
            std::fprintf(F, "q=%-6s f=%llu sinceKey=%7.2fms  FRAME=%6.2fms  bg=%6.2fms  live=%6.2fms  strip=%6.2fms  renderSearch=%6.2fms  match=%5.2fms/%llu  rows=%5.2fms  app=%5.2fms  scan=%5.2fms/%llu  TEXT[cold=%llu %6.2fms]  ICON[cold=%llu resolve=%6.2f decode=%6.2f upload=%6.2f]\n",
                         g_search.query.text.c_str(), (unsigned long long)Prof::frames, SINCEKEY,
                         Prof::frameNs / 1e6, Prof::bgNs / 1e6, Prof::liveNs / 1e6, Prof::stripNs / 1e6, Prof::srchNs / 1e6,
                         Prof::matchNs / 1e6, (unsigned long long)Prof::matchCalls,
                         Prof::rowsNs / 1e6, Prof::appNs / 1e6, Prof::scanNs / 1e6, (unsigned long long)Prof::scanCalls,
                         (unsigned long long)Prof::txtCold, Prof::txtNs / 1e6, (unsigned long long)Prof::icoCold, Prof::icoResolveNs / 1e6, Prof::icoDecodeNs / 1e6, Prof::icoUploadNs / 1e6);
            std::fclose(F);
        }
        if (Prof::frames >= 3)
            Prof::pending = false;
    }

    // G12. The dragged result follows the pointer, AFTER the dock: the whole
    // gesture is a journey from the panel to the pill, and an icon that vanished
    // under the thing it is being dropped on would read as a failed drop.
    if (g_search.pointer.drag && g_search.pointer.drag->live) {
        if (const auto TEX = OverviewRender::appIcon(g_search.pointer.drag->icon); TEX && TEX->ok()) {
            const float        D = std::round(48.F * sc<float>(MONITOR->m_scale));
            CTexPassElement::SRenderData ghost;
            ghost.tex = TEX;
            ghost.box = CBox{lastMousePosLocal.x - D / 2.0, lastMousePosLocal.y - D / 2.0, D, D}.round();
            ghost.a   = 0.85F;
            g_pHyprRenderer->m_renderPass.add(makeUnique<CTexPassElement>(ghost));
        }
    }

    sendOverviewFrameCallbacks(NOW);
}

void CScrollOverview::setClosing(bool closing_) {
    closing = closing_;
    if (closing) {
        if (OmarchySearch::isSushiActive())
            OmarchySearch::closeSushi();
        transferSharedStateOwnership();
        inputFramePending = false;
        clearDragPending();
        restoreSubmapIfActive();
        suspendInputListeners();
    } else {
        applyWorkspaceAnimationOverrides();
        activateSubmapIfConfigured();
    }
}

// The hooks deliberately SURVIVE a close. Every handler already early-returns
// on `closing`, so an inert overview costs nothing by keeping them -- and they
// are constructor-bound lambdas, so dropping them would make reopen() a rebuild
// of six closures instead of the clearing of one bool.
void CScrollOverview::suspendInputListeners() {
    if (pan.pointerDown)
        endScrollingPan();
    clearDragPending();
    submapMouseClickPending = false;
    submapMouseClickButton  = 0;
}

EOverviewPhase CScrollOverview::phase() const {
    return closing ? EOverviewPhase::Closing : EOverviewPhase::Open;
}

// Entrance and exit are not the same motion, so they do not share a curve. The
// swap has to happen BEFORE the new target is assigned: the animation reads its
// config when it is re-aimed, so setting it afterwards would shape the next
// transition rather than this one.
void CScrollOverview::syncPluginAnimEnabled() {
    // Overview open/close is a Hyprland animated variable with a COPIED
    // internalEnabled, so plugin:hyprdrive:anim_scale 0 would otherwise still
    // ease the overview while dock flights snap. Refresh every frame so a live
    // panel toggle takes effect on an already-open overview.
    const int ON = ScrollOverview::Config::motionEnabled() ? 1 : 0;
    const auto set = [ON](const SP<Hyprutils::Animation::SAnimationPropertyConfig>& cfg) {
        if (cfg)
            cfg->internalEnabled = ON;
    };
    set(openAnimConfig);
    set(closeAnimConfig);
    set(reverseAnimConfig);
    set(searchAnimConfig);
    set(workspaceInsertFadeConfig);
    set(workspaceRemoveFadeConfig);
}

void CScrollOverview::useAnimDirection(bool opening) {
    syncPluginAnimEnabled();
    // X75 (user, 2026-08-06): spamming Super "seems to have a decel+reverse
    // thing going on, I want it to immediately reverse at the same inverse
    // momentum it was going at".
    //
    // A REVERSAL is neither an entrance nor an exit. The variable already
    // carries speed, and the exit curve is an ease-IN -- zero slope at t=0 --
    // so re-aiming with it stalls the motion dead and then builds back up. That
    // stall IS the decel the user is describing; the value was always
    // continuous, the VELOCITY was not.
    //
    // The ease-out curve starts at maximum slope, so the turn happens at the
    // speed the motion already had. The duration is scaled by how far is
    // actually left to travel, or a reversal caught at 90% open would spend a
    // full close on the last 10% and feel slower than no animation at all.
    if (scale && scale->isBeingAnimated() && reverseAnimConfig) {
        const float OPEN   = ScrollOverview::Config::getScale();
        const float TARGET = opening ? OPEN : 1.F;
        const float RANGE  = std::max(0.01F, std::abs(1.F - OPEN));
        const float LEFT   = std::clamp(std::abs(TARGET - scale->value()) / RANGE, 0.F, 1.F);
        const float BASE   = opening ? ScrollOverview::Config::getOpenSpeed() : ScrollOverview::Config::getCloseSpeed();

        // Floor it: a hair of travel still deserves a frame or two of motion
        // rather than a teleport.
        reverseAnimConfig->internalSpeed = std::max(0.3F, BASE * LEFT);

        scale->setConfig(reverseAnimConfig);
        viewOffset->setConfig(reverseAnimConfig);
        return;
    }

    const auto CONFIG = opening ? openAnimConfig : closeAnimConfig;
    if (!CONFIG)
        return;

    scale->setConfig(CONFIG);
    viewOffset->setConfig(CONFIG);
}

void CScrollOverview::reopen() {
    if (!closing)
        return;

    // Before anything re-targets the animation: close() left an end callback
    // that DESTROYS this overview. Re-aiming `scale` below would otherwise run
    // that teardown at the end of the re-open.
    scale->setCallbackOnEnd([](auto) {});

    closeOnWindow.reset();
    setClosing(false); // flips `closing` back to false + re-arms the overrides

    if (sharedStateOwner)
        forceWorkspaceAlphaVisible();

    emitFullscreenVisibilityState(Desktop::focusState()->window(), true);

    // close() commits its selection BEFORE it animates, so it may already have
    // changed workspace and focused a window. That commit is deliberately not
    // undone: the overview shows every workspace anyway, so the honest resting
    // place is wherever the desktop now is -- which is offset zero.
    //
    // But the workspace handler that would have rebuilt the strip for that
    // change bails while `closing` is set, so the images still describe the
    // desk as it was before the close committed -- measured as an overview that
    // came back with its window previews missing. Rebuilding them here is still
    // far cheaper than a destroy-and-reconstruct: the blur framebuffers and the
    // texture prefilter cache survive, and only the workspace images are redone.
    startedOn = pMonitor ? pMonitor->m_activeWorkspace : startedOn;
    redrawAll();
    viewportCurrentWorkspace   = activeWorkspaceIndex();
    focusSyncedFromWorkspaceID = WORKSPACE_INVALID;

    // Both animate FROM their current value, so the reversal picks up exactly
    // where the close had got to: no jump, and no restart from the endpoint.
    useAnimDirection(true);
    *viewOffset = Vector2D{};
    *scale      = ScrollOverview::Config::getScale();

    stealOverviewKeyboard();
    damage();
}

void CScrollOverview::activateSubmapIfConfigured() {
    if (!sharedStateOwner || !usesSubmapKeybinds || !g_pKeybindManager)
        return;

    previousSubmapName = g_pKeybindManager->getCurrentSubmap().name;

    const auto SUBMAP = boundOverviewSubmap();
    if (SUBMAP != std::string_view{OVERVIEW_SUBMAP})
        ScrollOverview::Compat::noteLegacyUse(ScrollOverview::Compat::ELegacy::KEY, std::string{"submap "} + SUBMAP);

    const auto RESULT = Config::Actions::setSubmap(SUBMAP);
    if (!RESULT) {
        usesSubmapKeybinds = false;
        return;
    }

    submapActive = true;
}

void CScrollOverview::restoreSubmapIfActive() {
    if (!submapActive || !g_pKeybindManager)
        return;

    const auto CURRENT = g_pKeybindManager->getCurrentSubmap().name;
    if (isOverviewSubmapName(CURRENT)) {
        (void)Config::Actions::setSubmap(previousSubmapName.empty() ? "reset" : previousSubmapName);
    }

    submapActive = false;
}

bool CScrollOverview::dispatchSubmapMouseClick(uint32_t button) {
    if (!usesSubmapKeybinds || !isOverviewSubmapActive() || !g_pKeybindManager || !g_pInputManager)
        return false;

    const auto KEYNAME = "mouse:" + std::to_string(button);
    const auto MODS    = g_pInputManager->getModsFromAllKBs();

    const auto KEYBIND = std::ranges::find_if(g_pKeybindManager->m_keybinds, [&](const auto& keybind) {
        return keybind && keybind->enabled && !keybind->shadowed && keybind->key == KEYNAME && isOverviewSubmapName(keybind->submap.name) &&
            (keybind->modmask == MODS || keybind->ignoreMods);
    });

    if (KEYBIND == g_pKeybindManager->m_keybinds.end())
        return false;

    const auto INDEX = Hyprutils::String::strToNumber<int>((*KEYBIND)->arg);
    const auto MGR   = Config::Lua::mgr();
    if (!INDEX || !MGR)
        return false;

    const auto PREVIOUSKEYBIND = g_pKeybindManager->m_currentKeybind;
    g_pKeybindManager->m_currentKeybind = *KEYBIND;
    auto restoreKeybind = Hyprutils::Utils::CScopeGuard([PREVIOUSKEYBIND] { g_pKeybindManager->m_currentKeybind = PREVIOUSKEYBIND; });

    const int PREVIOUSPASSPRESSED = Config::Actions::state()->m_passPressed;
    Config::Actions::state()->m_passPressed = 0;
    auto restorePassPressed = Hyprutils::Utils::CScopeGuard([PREVIOUSPASSPRESSED] { Config::Actions::state()->m_passPressed = PREVIOUSPASSPRESSED; });

    // ponytail: 0.56.1 has void callLuaFn(int); callLuaFnBind + .success is 0.57+.
    // No success signal available here, so assume dispatched.
    MGR->callLuaFn(*INDEX);
    return true;
}

void CScrollOverview::resetSwipe() {
    m_isSwiping    = false;
    m_swipeClosing = false;
}

float CScrollOverview::openProgress() const {
    if (!scale)
        return 0.F;

    return overviewGridProgress(scale->value());
}

void CScrollOverview::onWorkspaceSwipeBegin() {
    trackpadWorkspaceFollowing = true;
    trackpadTapeFollowing      = false;
    // Logical units, same as viewOffset — a slow swipe must not have to cover
    // a full card width on the pad.
    trackpadScrollAccum = viewOffset ? axisValue(viewOffset->value(), layout) : 0.0;
}

void CScrollOverview::onWorkspaceSwipeUpdate(double delta) {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR || !viewOffset)
        return;

    const float SCALE = std::max<float>(scale->value(), 0.01F);
    const float PITCH = getWorkspaceLogicalPitch(MONITOR, SCALE, layout);
    // gesture_distance px on the pad = one desktop. Linear in finger travel,
    // not velocity, so a slow swipe still reaches the next workspace.
    const float DIST  = std::max(80.F, sc<float>(ScrollOverview::Config::getGestureDistance()));

    trackpadWorkspaceFollowing = true;
    trackpadScrollAccum += delta * (PITCH / DIST);

    double minOffset = 0.0;
    double maxOffset = 0.0;
    for (size_t i = 0; i < images.size(); ++i) {
        if (!images[i] || !images[i]->pWorkspace)
            continue;
        const double WORKSPACEOFFSET = workspaceOverviewLogicalOffset(i, viewportCurrentWorkspace, PITCH);
        minOffset                    = std::min(minOffset, WORKSPACEOFFSET);
        maxOffset                    = std::max(maxOffset, WORKSPACEOFFSET);
    }
    trackpadScrollAccum = std::clamp(trackpadScrollAccum, minOffset, maxOffset);
    viewOffset->setValueAndWarp(axisOffsetVector(sc<float>(trackpadScrollAccum), layout));
    damage();
}

void CScrollOverview::onWorkspaceSwipeEnd() {
    finishWorkspaceScrollFollow();
}

void CScrollOverview::onSwipeUpdate(double delta) {
    const int DISTANCE = ScrollOverview::Config::getGestureDistance();

    m_isSwiping = true;

    const bool  DISMISS = m_swipeClosing || closing;
    const float PERC    = DISMISS ? 1.0 - std::clamp(delta / sc<double>(DISTANCE), 0.0, 1.0) : std::clamp(delta / sc<double>(DISTANCE), 0.0, 1.0);

    scale->setValueAndWarp(hyprlerp(1.F, ScrollOverview::Config::getScale(), PERC));
}

void CScrollOverview::onSwipeEnd() {
    const bool DISMISS = m_swipeClosing;
    m_isSwiping        = false;
    m_swipeClosing     = false;

    // Scale 1 is the desktop, the configured overview scale is fully open.
    // A reverse mid-gesture is just this number coming back the other way.
    const float OPEN     = ScrollOverview::Config::getScale();
    const float RANGE    = std::max(0.01F, std::abs(1.F - OPEN));
    const float OPENNESS = std::clamp(std::abs(scale->value() - 1.F) / RANGE, 0.F, 1.F);

    if (DISMISS) {
        if (OPENNESS <= 0.65F)
            close();
        else {
            useAnimDirection(true);
            *scale = OPEN;
        }
        return;
    }

    if (OPENNESS >= 0.35F) {
        useAnimDirection(true);
        *scale = OPEN;
    } else
        close();
}
