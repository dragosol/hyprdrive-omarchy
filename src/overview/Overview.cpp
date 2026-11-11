// COverview: render(), forced visibility and animation overrides, config overrides and state transfer.
// Cut from Overview.cpp by pure code motion.
#include "OverviewParts.hpp"

using namespace OverviewDetail;


// The definition behind the global declaration in OverviewInternal.hpp. One real
// symbol in one TU -- TileChrome.cpp binds to it through the PLT, so it cannot be
// inline-in-a-header (emission would then depend on some includer happening to
// ODR-use it) and cannot be namespaced (that renames the symbol; the compositor
// died at the first mid-close tile ease when the split did exactly that).
// The active animation:curve preset's entrance / exit (Motion.hpp) -- the
// house curves by default. It used to read Hyprland's named beziers, which do
// not exist until an overview has been opened once (and a reload can drop
// them), so until then every houseEase site fell back to a plain cubic.
float houseEase(bool decel, float t) {
    return Hyprdrive::Motion::ease(decel ? Hyprdrive::Motion::ERole::DECEL : Hyprdrive::Motion::ERole::ACCEL, t);
}


void CForcedVisibility::forceSurface(SP<CWLSurfaceResource> surface) {
    if (!surface)
        return;

    const auto HLSURFACE = Desktop::View::CWLSurface::fromResource(surface);
    if (!HLSURFACE)
        return;

    if (!m_surfaceSeen.insert(surface.get()).second) {
        HLSURFACE->m_visibleRegion = {};
        return;
    }

    m_surfaces.push_back({surface, HLSURFACE->m_visibleRegion});
    HLSURFACE->m_visibleRegion = {};
}

// A SURFACE ENTERED ON NO OUTPUT IS A SURFACE THE CLIENT STOPS PAINTING
// (user, 2026-09-19: a fullscreen YouTube video "sometimes it updates sometimes
// it doesn't", and reproducibly not after switching desktops from the strip).
//
// wl_surface.enter/leave is how a client learns it is on screen, and Firefox
// uses it the way it uses xdg-toplevel suspended: entered nowhere means paint
// nothing. While the overview is up it, not the desktop, is what the monitor
// shows, so Hyprland can leave a window's surface on no output at all while the
// overview is busily drawing that window's tile -- and then the client is being
// handed frame callbacks it will never answer. Measured: with the video's own
// card SELECTED, so REALTIME frames and no throttle, `sent` climbed and the
// time since its last damage climbed with it (1993ms, 3009ms, 4027ms).
//
// Clearing m_visibleRegion above stops the COMPOSITOR culling the surface; this
// is the other half, and the two are not substitutes. Recorded and undone in
// restoreSurfaces so closing the overview hands the question back to Hyprland.
void CForcedVisibility::enterWindowSurfaces(PHLWINDOW window, PHLMONITOR monitor) {
    if (!window || !monitor || !window->wlSurface() || !window->wlSurface()->resource())
        return;

    std::pair<CForcedVisibility*, PHLMONITOR> data = {this, monitor};
    window->wlSurface()->resource()->breadthfirst(
        [](SP<CWLSurfaceResource> surface, const Vector2D&, void* data) {
            const auto [SELF, MONITOR] = *sc<std::pair<CForcedVisibility*, PHLMONITOR>*>(data);
            if (!surface)
                return;

            if (std::ranges::any_of(surface->m_enteredOutputs, [&](const auto& m) { return m.lock() == MONITOR; }))
                return;

            surface->enter(MONITOR);
            if (SELF->m_enteredSeen.insert(surface.get()).second)
                SELF->m_entered.push_back({surface, MONITOR});
        },
        &data);
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

    const bool FIRST = m_windowSeen.insert(window.get()).second;
    if (FIRST) {
        auto& entry  = m_windows.emplace_back();
        entry.window = window;
        entry.hidden = window->m_hidden;
    }

    window->m_hidden = false;

    // A SUSPENDED CLIENT CANNOT ASK FOR THE FRAME THAT WOULD WAKE IT (user,
    // 2026-09-19: a fullscreen YouTube video in Zen on another desktop "shows
    // static when in fact it's playing" -- measured at 0-2% CPU, i.e. not
    // merely stale but genuinely not decoding).
    //
    // Hyprland gives a window whose workspace is not active the xdg-toplevel
    // `suspended` state, and Firefox honours it literally: it stops painting AND
    // stops decoding, so it never calls CWLSurfaceResource::frame and never
    // reaches the frame gate at all. Forcing m_hidden and alpha is not enough --
    // those make the COMPOSITOR draw the client's last buffer, not the client
    // produce a new one. The dock has cleared this for its previews all along
    // (Dock.cpp forcePreviewVisible), which is why hovering a dock preview made
    // the strip come alive too: one un-suspend serves every viewer of a client.
    //
    // It lives HERE, and not in the card walk where it was first written, because
    // that walk iterates the card's TILED windows and a fullscreen window is not
    // among them -- it is drawn through overviewFullscreenPathWindow instead, so
    // the one window the bug was reported about was the one window the fix
    // missed. forceWindow is called from renderWindowLive for every window the
    // overview draws, fullscreen included, and it is above the alpha early-out
    // deliberately: a window already parked at alpha 1 still needs waking.
    if (Prof::enabled())
        ++g_unsuspendCount[window->m_class].first;

    if (window->m_suspended) {
        if (Prof::enabled())
            ++g_unsuspendCount[window->m_class].second;
        window->setSuspended(false);
    }

    // The same guard forceWorkspaceAlphaVisible uses. setValueAndWarp on an
    // animation that is already parked at 1 still writes through the animation
    // manager, and this runs for every forced window on every frame.
    auto& alpha = window->alpha(FULLSCREENALPHA);
    if (alpha->value() == 1.F && alpha->goal() == 1.F && !alpha->isBeingAnimated())
        return;

    alpha->setValueAndWarp(1.F);
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

    // Hand the question back to Hyprland: a client must not be left believing
    // it is on an output that has stopped drawing it, or it keeps painting (and
    // decoding) for a tile nobody is showing any more.
    for (auto& entry : m_entered) {
        const auto SURFACE = entry.surface.lock();
        const auto MONITOR = entry.monitor.lock();
        if (SURFACE && MONITOR)
            SURFACE->leave(MONITOR);
    }

    m_entered.clear();
    m_enteredSeen.clear();
    m_surfaces.clear();
    m_surfaceSeen.clear();
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
    m_windowSeen.clear();
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

void COverview::applyWorkspaceAnimationOverrides() {
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

void COverview::restoreWorkspaceAnimationOverrides() {
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

void COverview::reapplyConfigOverridesAfterReload() {
    if (!sharedStateOwner || closing)
        return;

    // Re-save from the freshly reloaded config, then re-apply: the apply
    // functions early-return on the overridden flags, so those drop first.
    // The stale pre-reload snapshots are NOT restored -- restoring them would
    // write dead values over the config the user just reloaded.
    if (workspaceAnimationsOverridden) {
        workspaceAnimationsOverridden = false;
        savedWorkspaceAnimationConfigs.clear();
        applyWorkspaceAnimationOverrides();
    }

    if (inputConfigOverridden) {
        inputConfigOverridden = false;
        applyInputConfigOverrides();
    }
}

void COverview::forceWorkspaceAlphaVisible(const PHLWORKSPACE& workspace) {
    if (!workspace || !workspace->m_alpha)
        return;

    if (workspace->m_alpha->value() == 1.F && workspace->m_alpha->goal() == 1.F && !workspace->m_alpha->isBeingAnimated())
        return;

    workspace->m_alpha->setValueAndWarp(1.F);
    *workspace->m_alpha = 1.F;
}

void COverview::forceWorkspaceAlphaVisible() {
    for (const auto& workspace : State::workspaceState()->workspaces())
        forceWorkspaceAlphaVisible(workspace.lock());
}

void COverview::forceWorkspaceWindowsDecoRecalc(const PHLWORKSPACE& workspace) {
    if (!workspace)
        return;

    const auto workspaceImage = std::ranges::find_if(images, [&workspace](const auto& image) { return image && image->pWorkspace == workspace; });
    if (workspaceImage == images.end())
        return;

    for (const auto& windowRef : (*workspaceImage)->windows)
        OverviewWindow::forceDecoRecalc(windowRef.lock());
}

void COverview::applyInputConfigOverrides() {
    if (!sharedStateOwner || inputConfigOverridden)
        return;

    previousNoWarps                    = Hyprdrive::Config::getValue<int>("cursor:no_warps");
    previousWarpOnChangeWorkspace      = Hyprdrive::Config::getValue<int>("cursor:warp_on_change_workspace");
    previousWarpOnToggleSpecial        = Hyprdrive::Config::getValue<int>("cursor:warp_on_toggle_special");
    previousWarpBackAfterNonMouseInput = Hyprdrive::Config::getValue<int>("cursor:warp_back_after_non_mouse_input");
    previousFollowMouse                = Hyprdrive::Config::getValue<int>("input:follow_mouse");
    inputConfigOverridden = true;

    Hyprdrive::Config::setValue("cursor:no_warps", 1);
    Hyprdrive::Config::setValue("cursor:warp_on_change_workspace", 0);
    Hyprdrive::Config::setValue("cursor:warp_on_toggle_special", 0);
    Hyprdrive::Config::setValue("cursor:warp_back_after_non_mouse_input", 0);
    Hyprdrive::Config::setValue("input:follow_mouse", 0);
}

void COverview::restoreInputConfigOverrides() {
    if (!inputConfigOverridden)
        return;

    Hyprdrive::Config::setValue("cursor:no_warps", previousNoWarps);
    Hyprdrive::Config::setValue("cursor:warp_on_change_workspace", previousWarpOnChangeWorkspace);
    Hyprdrive::Config::setValue("cursor:warp_on_toggle_special", previousWarpOnToggleSpecial);
    Hyprdrive::Config::setValue("cursor:warp_back_after_non_mouse_input", previousWarpBackAfterNonMouseInput);
    Hyprdrive::Config::setValue("input:follow_mouse", previousFollowMouse);

    inputConfigOverridden = false;
}

void COverview::transferSharedStateOwnership() {
    if (!sharedStateOwner)
        return;

    COverview* successor = nullptr;
    for (const auto& overview : scrollOverviews()) {
        auto* candidate = overview ? dynamic_cast<COverview*>(overview.get()) : nullptr;
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

void COverview::emitFullscreenVisibilityState(PHLWINDOW window, bool hideFullscreen) {
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

void COverview::render() {
    Prof::Timer PROF{Prof::frameNs, Prof::frameCalls};
    const auto  OPENPROF_T0 = std::chrono::steady_clock::now();
    auto        openProfGuard = Hyprutils::Utils::CScopeGuard([OPENPROF_T0] {
        OpenProf::addOverviewRender(sc<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - OPENPROF_T0).count()));
    });
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR)
        return;

    OpenProf::gpuMark("begin");
    noteFrame(MONITOR->m_id);
    // Smooth-frames hint: only the open/close animation must hit every refresh.
    if (scale->isBeingAnimated())
        Hyprdrive::FramePacing::noteAnimatedFrame(MONITOR);
    else
        Hyprdrive::FramePacing::noteIdleFrame(MONITOR);

    ++g_overviewFrame;
    ++g_overviewFramesByMonitor[MONITOR->m_id];

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

    const auto WALLPAPERMODE = Hyprdrive::Config::getWallpaperMode();

    // Y17 (GPU, 2026-09-14): at PROGRESS >= 1 the blurred backdrop below covers
    // this full-screen wallpaper pass, so skipping it here looks free. It is
    // NOT taken: the backdrop is composited with normal blending at alpha
    // PROGRESS, and its own alpha channel is whatever the kawase left in the
    // cached FB -- which is 1 only if the render target carries no alpha. On a
    // format that does, an alpha a hair under 1 lets this wallpaper show
    // through the 20% the dim scrim does not cover. Unprovable by reading, so
    // it stays.
    //
    // Fully open, though, the blurred backdrop is drawn at alpha 1 over ALL of
    // it, and both wallpaper caches render the wallpaper into their own FBs --
    // so the screen's wallpaper pass (renderBackground plus the BACKGROUND
    // layers, two full-screen passes at 2880x1800) is ~4-5 ms of GPU a frame
    // (measured 2026-09-30, openprof) that nobody sees. A clear costs nothing
    // and, if the backdrop's alpha were ever a hair under 1, what shows
    // through the 80% scrim is black instead of wallpaper: invisible.
    // That frame budget is what decides whether a typed letter's animation
    // holds 60 fps or drops a refresh.
    const float OPENSCALE    = Hyprdrive::Config::getScale();
    const float OPENPROGRESS = std::clamp(std::abs(SCALE - 1.F) / std::max(0.01F, std::abs(1.F - OPENSCALE)), 0.F, 1.F);
    const bool  BACKDROPCOVERS = OPENPROGRESS >= 0.999F && WALLPAPERMODE != 1 && frost.backdropFB && frost.backdropFB->isAllocated() && frost.backdropFB->getTexture();
    if ((WALLPAPERMODE == 0 || WALLPAPERMODE == 2) && !BACKDROPCOVERS)
        renderGlobalWallpaper(MONITOR, NOW);
    else
        g_pHyprRenderer->draw(CClearPassElement::SClearData{CHyprColor{0.F, 0.F, 0.F, 1.F}}, {});

    Event::bus()->m_events.render.stage.emit(RENDER_POST_WALLPAPER);
    OpenProf::gpuMark("wallpaper");

    // Commit the queued backdrop to the screen BEFORE the temp-FB work below:
    // flushing inside the bind would swallow those elements into the FB (that
    // exact bug blacked out the whole backdrop once). And the FB fill has to
    // happen HERE, not at the top of render() -- layer surfaces demonstrably
    // render at this stage and not earlier (empty-FB bug, take two).
    OverviewRender::flushPass(MONITOR);
    OpenProf::gpuMark("wpflush");
    updateCardWallpaperFB(MONITOR, NOW);
    OpenProf::gpuMark("cardwp");

    // The wallpaper canvas is still the monitor fill; the cards float on top
    // of it. Blur it the way the Omarchy lockscreen blurs its wallpaper, then
    // dim 80% black, both faded with open progress so a swipe does not slam
    // them on. Cards keep the undimmed, unblurred fill captured above.
    // Flush BEFORE the cards so the scrim cannot land on top of the card fill.
    {
        const float OPEN     = Hyprdrive::Config::getScale();
        const float RANGE    = std::max(0.01F, std::abs(1.F - OPEN));
        const float PROGRESS = std::clamp(std::abs(SCALE - 1.F) / RANGE, 0.F, 1.F);
        float       DIM      = BACKDROP_DIM * PROGRESS;
        if (PROGRESS > 0.001F && WALLPAPERMODE != 1) {
            updateBackdropBlurCache(MONITOR, WALLPAPERMODE, NOW);
            OpenProf::gpuMark("blurcache");
            // THE DIM IS BAKED INTO THE CACHE (2026-09-30): the backdrop holds
            // blur x (1 - BACKDROP_DIM), so fully open it is ONE opaque blit and
            // no full-screen scrim pass (~1.5 ms GPU at 2880x1800). Mid-open the
            // same pixels as before -- mix(wall, blur, P) dimmed by DIM*P -- come
            // from the pre-dimmed blur at alpha A and a smaller scrim D:
            //   wall(1-A)(1-D) + blur(1-DIM)A(1-D) = (wall(1-P) + blur P)(1-DIM P)
            //   => A = P / (1 - DIM + DIM P),  1-D = (1 - DIM P) / (1 - DIM A)
            const float A = PROGRESS / (1.F - BACKDROP_DIM + BACKDROP_DIM * PROGRESS);
            if (renderBackdropBlurCache(MONITOR, A))
                DIM = 1.F - (1.F - BACKDROP_DIM * PROGRESS) / std::max(0.01F, 1.F - BACKDROP_DIM * A);
        }
        if (DIM > 0.001F) {
            CRectPassElement::SRectData scrim;
            scrim.box   = CBox{0, 0, MONITOR->m_transformedSize.x, MONITOR->m_transformedSize.y};
            scrim.color = CHyprColor{0.F, 0.F, 0.F, DIM};
            g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(scrim));
            OverviewRender::flushPass(MONITOR);
        }
    }

    OpenProf::gpuMark("start");
    {
        Prof::Timer PROFBG{Prof::bgNs, Prof::bgCalls};
        // The removed desktop's ghost first: beneath the cards it slides under.
        if (workspaceInsertTransition.active && workspaceInsertTransition.ghost) {
            // Back under the desktop shown next: a whole card (a bare shadow left
            // a hole the window crossed in the dark -- user's recording,
            // 2026-11-06), solid until that desktop covers it, gone at the end.
            const float GT          = std::clamp(workspaceInsertProgress->value(), 0.F, 1.F);
            const auto GHOSTALPHA   = std::clamp((1.F - GT) / 0.25F, 0.F, 1.F);
            const auto GHOSTOFFSET = (workspaceInsertTransition.transitionOldRelativeOffset +
                                      (workspaceInsertTransition.ghostTo - workspaceInsertTransition.transitionOldRelativeOffset) * GT) * SCALE * MONITOR->m_scale;
            // ...going further back as it does (cardDepth's rule, against where it ends).
            const float GHOSTPITCH  = getWorkspaceRenderedPitch(MONITOR, SCALE, layout);
            // (Against where the desktop covering it IS, not where it ends: it
            // starts uncovered.)
            const float GHOSTUNDER  = GHOSTPITCH > 1.F ? std::clamp(1.F - std::abs(GHOSTOFFSET - workspaceOverviewOffset(ACTIVEIDX, ACTIVEIDX, PITCH)) / GHOSTPITCH, 0.F, 1.F) : 0.F;
            const auto GHOSTREST    = getOverviewWorkspaceBox(MONITOR, SCALE, viewOffset->value(), GHOSTOFFSET, layout);
            const auto GHOSTBOX     = depthScaled(GHOSTREST, GHOSTREST.middle(), 1.F - DEPTH_SHRINK * GHOSTUNDER * GHOSTUNDER * (3.F - 2.F * GHOSTUNDER));

            if (GHOSTALPHA > 0.001F && overviewBoxIntersectsMonitor(GHOSTBOX, MONITOR))
                renderCardBody(MONITOR, GHOSTBOX, SCALE, WALLPAPERMODE, GHOSTALPHA, NOW, 0.F);
        }

        // (In a reorder the desktop in view is drawn last, card and windows
        // together, so the ones passing behind it are behind it whole.)
        const size_t DEPTHANCHOR = depthAnchor();
        for (const size_t workspaceIdx : cardOrder())
            if (workspaceIdx != DEPTHANCHOR)
                renderWorkspaceBackground(MONITOR, workspaceIdx, ACTIVEIDX, PITCH, SCALE, WALLPAPERMODE, NOW);
    }
    OpenProf::gpuMark("bg");

 // Y17 (GPU, 2026-09-14). CPreBlurElement is a full-monitor dual-kawase that
    // writes Monitor::m_blurFB, and m_blurFB is read only by a surface that
    // takes Hyprland's xray path. Every tile here goes through
    // OverviewWindow::renderOverviewWindow with scaleBlurToRenderScale=true
    // (Window.hpp:27), and that scales decoration:blur:size by the tile's
    // render scale (Window.cpp:1062) -- which forces usePrecomputedBlur=false
    // and then calls blockOverviewWindowBlurOptimization over every surface of
    // the window (Window.cpp:1075/1082). blockBlurOptimization is exactly what
    // Hyprland sets on its own BACKGROUND/BOTTOM layers to keep them off the
    // xray FB (Renderer.cpp:969), so while the blur size is scaled NOTHING the
    // overview draws reads m_blurFB and the pre-blur is pure waste. The scale
    // factor is global to the frame, so the predicate is computed once here.
    // The dock's own frost does not read m_blurFB either: queueDockFrost
    // (Dock.cpp:1429) queues COverviewBlurPassElement with xray=false, and that
    // element's needsPrecomputeBlur() is hardcoded false (X88).
    // decoration:blur:xray is the one switch that puts a surface the overview
    // did NOT render through renderOverviewWindow -- the Quick Look preview
    // below, a dock hover preview -- on the xray path (Renderer.cpp:1540), so
    // with it on the pre-blur keeps running regardless.
    const int  BLURSIZE               = Hyprdrive::Config::getValue<int>("decoration:blur:size");
    const bool TILEBLURSCALED         = BLURSIZE > 0 && std::max(1, sc<int>(std::round(BLURSIZE * SCALE))) != BLURSIZE;
    const bool NOPREBLURREADER        = TILEBLURSCALED && !Hyprdrive::Config::getValue<bool>("decoration:blur:xray");
    const bool NEEDS_PRECOMPUTED_BLUR = !NOPREBLURREADER && hasVisiblePrecomputedBlurWindow(MONITOR, ACTIVEIDX, PITCH, SCALE);
    if (NEEDS_PRECOMPUTED_BLUR && frost.overviewDirty)
        g_pHyprRenderer->m_renderPass.add(makeUnique<CPreBlurElement>());
    OpenProf::gpuMark("preblur");

    OverviewRender::flushPass(MONITOR);

    // Cleared only when the element actually ran: a frame that skipped it must
    // leave the flag standing so the pre-blur happens once the scale settles
    // back to 1 and the tiles start reading m_blurFB again.
    if (NEEDS_PRECOMPUTED_BLUR)
        frost.overviewDirty = false;

    // THE LIVE WINDOWS ARE THE TOP LAYER WHILE THE TOGGLE IS IN FLIGHT (user,
    // 2026-09-16: "elements of the workspace strip are showing up halfway
    // through the overview toggle animation"). The strip used to be drawn after
    // the windows unconditionally, so for the middle of every open and close --
    // the stretch where the shrinking window still covers the strip row -- the
    // strip's panel, cards and minis painted OVER the window. While the scale is
    // moving the strip goes underneath instead, and the window reveals it as it
    // shrinks. Settled, nothing overlaps the strip row, so the usual order (and
    // the strip frost sampling cards + tiles) is back with nothing to see
    // switching over.
    const bool TOGGLEINFLIGHT = closing || m_isSwiping || scale->isBeingAnimated();


    // X92 / X97. Blur the overview main FB (cards + tiles) into a PRIVATE FB.
    // Gate on decoration:blur:enabled — the same switch window frost uses — NOT
    // plugin:scrolloverview:blur (that only means wallpaper backdrop blur and
    // defaults false). X96 made strip sample this FB; gating on the wallpaper
    // option wiped strip + search-result frost entirely.
    // The detent badge frosts the digit itself, and does so whatever the theme
    // does with window blur -- the same stance the overview BACKDROP already
    // takes, which blurs the wallpaper even when Omarchy windows do not. So the
    // capture has to run for the badge as well, with the flag forced for its
    // duration exactly as updateBackdropBlurCache forces it around its own.
    //
    // Y17 (GPU, 2026-09-14). The capture is a full-monitor dual-kawase plus a
    // full-monitor copy, and every surface that samples it is behind the SAME
    // `overviewGridProgress(SCALE) < 0.01` early return: the strip panel and
    // the alternate-monitor / scratchpad plates it draws (renderStrip), the
    // search results container (renderSearch) and the detent badge
    // (renderDetentBadge). Below that threshold not one xray=true blur element
    // is queued, so the capture has no reader at all -- skip it exactly the way
    // the blur-off branch does. (The search PILL and the dock pill are
    // xray=false: they blur live and never look at this FB.)
    const auto renderStripLayer = [&]() {
        const bool CHROMEBLURSAMPLED = overviewGridProgress(SCALE) >= 0.01F;
        if (CHROMEBLURSAMPLED && Hyprdrive::Config::getValue<bool>("decoration:blur:enabled"))
            OverviewRender::captureFrameChromeBlur(MONITOR);
        else if (CHROMEBLURSAMPLED && detentBadgeVisible()) {
            Hyprdrive::Config::setValue("decoration:blur:enabled", true);
            auto restoreBlur = Hyprutils::Utils::CScopeGuard([] { Hyprdrive::Config::setValue("decoration:blur:enabled", false); });
            OverviewRender::captureFrameChromeBlur(MONITOR);
        } else
            OverviewRender::clearFrameChromeBlur();

        {
            Prof::Timer PROFSTRIP{Prof::stripNs, Prof::stripCalls};
            renderStrip(MONITOR, SCALE, NOW);
            // On top of the cell the strip just drew, including its minis.
            renderDetentBadge(MONITOR, SCALE);
        }
    };

    if (TOGGLEINFLIGHT) {
        renderStripLayer();
        // Committed before any window queues: a tile's prefilter binds a temp
        // FB, and nothing of the strip may still be waiting in the pass then.
        OverviewRender::flushPass(MONITOR);
    }

    {
        Prof::Timer PROFLIVE{Prof::liveNs, Prof::liveCalls};
        const size_t DEPTHANCHOR = depthAnchor();
        for (const size_t workspaceIdx : cardOrder())
            if (workspaceIdx != DEPTHANCHOR)
                renderWorkspaceLive(MONITOR, workspaceIdx, ACTIVEIDX, PITCH, SCALE, WALLPAPERMODE, NOW);
        if (DEPTHANCHOR != SIZE_MAX) {
            OverviewRender::flushPass(MONITOR);
            renderWorkspaceBackground(MONITOR, DEPTHANCHOR, ACTIVEIDX, PITCH, SCALE, WALLPAPERMODE, NOW);
            OverviewRender::flushPass(MONITOR); // (a tile's prefilter binds a temp FB: nothing may be waiting in the pass)
            renderWorkspaceLive(MONITOR, DEPTHANCHOR, ACTIVEIDX, PITCH, SCALE, WALLPAPERMODE, NOW);
        }
    }
    OpenProf::gpuMark("tiles");


    OverviewRender::flushPass(MONITOR);

    if (!TOGGLEINFLIGHT)
        renderStripLayer();
    OpenProf::gpuMark("strip");
    // X87: search (and its frost) under the drag proxy. Previously renderSearch
    // ran after renderDraggedWindow, so the pill painted over the window you
    // were dragging -- empty or with results.
    renderSearch(MONITOR, SCALE);
    OpenProf::gpuMark("search");
    renderPinnedFloatingWindows(MONITOR, SCALE, NOW);

    if (const auto SUSHI = overviewSushiWindowOn(MONITOR)) {
        // decorate = TRUE, unlike every other renderWindow in the
        // plugin. Those draw scaled tiles that wear TileChrome's
        // border instead; the preview is drawn at its native box on
        // top of the overview, so nothing else would frame it and it
        // was the one window on screen with no Omarchy border --
        // while the very same window, previewed off the overview,
        // has one. Hyprland's own decorations are the border here.
        g_pHyprRenderer->renderWindow(SUSHI, MONITOR, NOW, true, Render::RENDER_PASS_ALL, false, false);
    }

    // THE SCRATCHPAD, over the overview (overviewShownSpecialOn): the dim
    // Hyprland puts behind a special workspace, then its windows, natively --
    // position, slide (the workspace's render offset) and fade all Hyprland's.
    // Above the grid, the strip and the search; below the bar and the dock,
    // as on the desktop.
    if (const auto SPECIAL = overviewShownSpecialOn(MONITOR)) {
        const float DIM = Hyprdrive::Config::getValue<float>("decoration:dim_special") * SPECIAL->m_alpha->value();
        if (DIM > 0.001F) {
            CRectPassElement::SRectData dim;
            dim.box   = CBox{{}, MONITOR->m_transformedSize};
            dim.color = CHyprColor(0, 0, 0, DIM);
            g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(dim));
        }
        for (const auto& W : Desktop::windowState()->windows())
            if (onShownSpecial(W, MONITOR) && !W->m_isFloating)
                g_pHyprRenderer->renderWindow(W, MONITOR, NOW, true, Render::RENDER_PASS_ALL, false, false);
        for (const auto& W : Desktop::windowState()->windows())
            if (onShownSpecial(W, MONITOR) && W->m_isFloating)
                g_pHyprRenderer->renderWindow(W, MONITOR, NOW, true, Render::RENDER_PASS_ALL, false, false);
        OverviewRender::flushPass(MONITOR);
    }

    updateSwapHold();
    renderSwapTargetDim(MONITOR);
    renderDraggedWindow(MONITOR, ACTIVEIDX, PITCH, SCALE, NOW);
    renderSwapLabel(MONITOR);

    // TOP layers (the bar) here, under the dock. OVERLAY waits until after the
    // dock and the captions -- see the end of this function.
    const size_t FIRSTLAYERELEMENT = g_pHyprRenderer->m_renderPass.m_passElements.size();

    for (auto const& ls : MONITOR->m_layerSurfaceLayers[ZWLR_LAYER_SHELL_V1_LAYER_TOP]) {
        if (!Desktop::View::validMapped(ls.lock()))
            continue;

        g_pHyprRenderer->renderLayer(ls.lock(), MONITOR, NOW);
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
    Hyprdrive::Dock::setWindowDragActive(drag.activeWindow.operator bool());
    OpenProf::gpuMark("pinned+top");
    Hyprdrive::Dock::render(MONITOR);
    OpenProf::gpuMark("dock");
    // Hover titles hang off the bottom of a tile, so they collide with the
    // dock on the active card. Flush them after the pill so the label is the
    // thing you read, not the thing the dock covers.
    TileChrome::renderCaptions(MONITOR);

    // OVERLAY layers go LAST, above the dock and the captions. That layer is
    // Omarchy's transient chrome -- the volume/brightness OSD, notification
    // toasts, menus, the polkit prompt -- and it is the one thing on screen that
    // must never be covered. Drawn beside TOP (above), the dock pill painted over
    // the OSD, which Omarchy anchors bottom-centre exactly where the pill sits.
    const size_t FIRSTOVERLAYELEMENT = g_pHyprRenderer->m_renderPass.m_passElements.size();

    for (auto const& ls : MONITOR->m_layerSurfaceLayers[ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY]) {
        if (!Desktop::View::validMapped(ls.lock()))
            continue;

        g_pHyprRenderer->renderLayer(ls.lock(), MONITOR, NOW);
    }

    // And the layers' POPUPS after all of them, as Hyprland's own pass does
    // (Renderer.cpp, after OVERLAY). Without this every popup of a top or
    // overlay layer -- the bar's panels, the computer-use cursor -- vanished
    // while the overview was up (2026-10-01).
    for (const auto LAYER : {ZWLR_LAYER_SHELL_V1_LAYER_TOP, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY}) {
        for (auto const& ls : MONITOR->m_layerSurfaceLayers[LAYER]) {
            if (!Desktop::View::validMapped(ls.lock()))
                continue;

            g_pHyprRenderer->renderLayer(ls.lock(), MONITOR, NOW, true);
        }
    }

    blockOverviewLayerBlurOptimization(FIRSTOVERLAYELEMENT);

    // X52. One line per frame after a keystroke: the frame that PAINTS the new
    // results is frame 1, so its total is the keystroke-to-paint cost.
    if (Prof::enabled() && Prof::pending) {
        ++Prof::frames;
        const auto SINCEKEY = std::chrono::duration<double, std::milli>(Prof::clock::now() - Prof::keyAt).count();
        if (FILE* const F = Hyprdrive::DebugPaths::openLog("hyprdrive-prof.log")) {
            // The query's LENGTH, never its text: a log of timings has no
            // business holding what was searched for (marketplace review, #11392).
            std::fprintf(F, "qlen=%-3zu f=%llu sinceKey=%7.2fms  FRAME=%6.2fms  bg=%6.2fms  live=%6.2fms  strip=%6.2fms  renderSearch=%6.2fms  match=%5.2fms/%llu  rows=%5.2fms  app=%5.2fms  scan=%5.2fms/%llu  TEXT[cold=%llu %6.2fms]  ICON[cold=%llu resolve=%6.2f decode=%6.2f upload=%6.2f]  LETTERS[n=%zu busy=%d]  RETARGET[%d]  SCALE[%.5f]  FIELDW[%.3f]\n",
                         g_search.query.text.size(), (unsigned long long)Prof::frames, SINCEKEY,
                         Prof::frameNs / 1e6, Prof::bgNs / 1e6, Prof::liveNs / 1e6, Prof::stripNs / 1e6, Prof::srchNs / 1e6,
                         Prof::matchNs / 1e6, (unsigned long long)Prof::matchCalls,
                         Prof::rowsNs / 1e6, Prof::appNs / 1e6, Prof::scanNs / 1e6, (unsigned long long)Prof::scanCalls,
                         (unsigned long long)Prof::txtCold, Prof::txtNs / 1e6, (unsigned long long)Prof::icoCold, Prof::icoResolveNs / 1e6, Prof::icoDecodeNs / 1e6, Prof::icoUploadNs / 1e6,
                         SearchLetters::g_searchLetters.letters.size(), (int)SearchLetters::g_searchLetters.busy(Time::steadyNow()),
                         Prof::retargets, Prof::pillScale, Prof::fieldWL);
            std::fclose(F);
        }
        // THREE FRAMES MEASURED THE PAINT, NOT THE ANIMATION (user, 2026-09-19:
        // "around the 5th letter is when it starts lagging"). The keystroke
        // repaint is frames 1-3; the letter bounce runs ENTRANCE_MS = 420ms,
        // about 25 frames at 60Hz, and every one of them after the third was
        // invisible here. A choppy POP cannot be seen in a window that closes
        // before the pop does, so the window is the animation's length.
        if (SINCEKEY > 600.0)
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
