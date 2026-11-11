// CScrollOverview: constructor/destructor, close, onPreRender, onWorkspaceChange, submap and passthrough binds, pointer handoff, desktop moves, DesktopTape.
// Cut from scrollOverview.cpp by pure code motion.
#include "OverviewParts.hpp"

using namespace OverviewDetail;


// The search pane, pill and reveals: ~180 ms (Hyprland speed units, tenths of
// a second); windowsMove is ~400 ms and feels stuck. Times anim_scale.
constexpr float SEARCH_ANIM_SPEED = 1.8F;

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
    stashWallpaperCaches();
    frost.releaseGL();
    OverviewWindow::trimTexturePrefilterCache();
    const auto MONITOR = pMonitor.lock();
    const auto WORKSPACE = MONITOR ? MONITOR->m_activeWorkspace : PHLWORKSPACE{};
    emitFullscreenVisibilityState(getOverviewFullscreenVisibilityWindow(WORKSPACE, Desktop::focusState()->window()), false);
    restoreWorkspaceAnimationOverrides();
    restoreInputConfigOverrides();
    // Whatever has focus NOW. Each monitor's overview reaches this when its own
    // close animation ends, in no set order; its own closeOnWindow is only its
    // monitor's guess, and re-applying it took focus -- and the cursor -- back
    // off the window the user picked on the other monitor (2026-09-27).
    restoreOverviewKeyboard(Desktop::focusState()->window(), "teardown");
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
    forgetPhantomsOf(this);
    if (scrollOverviews().empty()) {
        Pointer::Cursor::overrideController->unsetOverride(Pointer::Cursor::CURSOR_OVERRIDE_SPECIAL_ACTION);
        g_overviewCursorYielded = false;
    }
    if (const auto MONITOR = pMonitor.lock())
        MONITOR->m_blurFBDirty = true;
}

// Is the bind running right now a mouse WHEEL bind (Super+scroll and the
// like: Hyprland names the wheel's notches mouse_up / _down / _left / _right)?
static bool currentBindIsWheel() {
    if (!g_pKeybindManager || !g_pKeybindManager->m_currentKeybind)
        return false;
    const auto& KEY = g_pKeybindManager->m_currentKeybind->key;
    return KEY == "mouse_up" || KEY == "mouse_down" || KEY == "mouse_left" || KEY == "mouse_right";
}

CScrollOverview::CScrollOverview(PHLWORKSPACE startedOn_, bool swipe_, PHLMONITOR monitor_) : startedOn(startedOn_), swipe(swipe_) {
    // A shown desktop's windows back first: the overview shows the real ones.
    Hyprdrive::ShowDesktop::restoreNow();
    const auto          PMONITOR = monitor_ ? monitor_ : (startedOn_ && startedOn_->m_monitor ? startedOn_->m_monitor.lock() : Desktop::focusState()->monitor());
    pMonitor                     = PMONITOR;
    adoptWallpaperStash();
    layout                       = Hyprdrive::Config::getLayout();
    sharedStateOwner             = scrollOverviews().empty();
    usesSubmapKeybinds           = hasOverviewSubmap();
    // The overview's own mouse binds must let a press through when the
    // overview itself lets it through -- over a TOP/OVERLAY layer (the bar,
    // notification toasts). A consuming bind ate that press even so (the
    // dispatcher closed the overview, the toast never saw it). Normal clicks
    // never reach the bind: onMouseButton cancels them and dispatches itself.
    if (g_pKeybindManager) {
        for (const auto& keybind : g_pKeybindManager->m_keybinds) {
            if (keybind && isOverviewSubmapName(keybind->submap.name) && (keybind->mouse || keybind->key.starts_with("mouse")))
                keybind->nonConsuming = true;
        }
    }

    // Any door, not just openOverview(): the swipe builds overviews itself.
    if (!OpenProf::active())
        OpenProf::begin(swipe_ ? "swipe" : "ctor");
    OpenProf::mark("ctor start");
    applyWorkspaceAnimationOverrides();
    if (sharedStateOwner)
        forceWorkspaceAlphaVisible();
    Hyprdrive::Dock::releasePointerFocus();
    // A zoomed-out canvas ends here: its presses would be ours too.
    Hyprdrive::Mosaic::onOverviewOpen();
    applyInputConfigOverrides();
    OpenProf::mark("ctor overrides applied");
    g_pInputManager->unconstrainMouse();
    realtimePreviewTimer = wl_event_loop_add_timer(g_pCompositor->m_wlEventLoop, realtimePreviewTimerCallback, this);
    scheduleMinimumPreviewFrame();

    const auto WINDOWSMOVECONFIG = Config::animationTree()->getAnimationPropertyConfig("windowsMove");
    const auto WINDOWSMOVEVALUES = WINDOWSMOVECONFIG && WINDOWSMOVECONFIG->pValues ? WINDOWSMOVECONFIG->pValues.lock() : WINDOWSMOVECONFIG;
    if (!Animation::mgr()->bezierExists(OVERVIEW_INSERT_FADE_BEZIER))
        Animation::mgr()->addBezierWithName(OVERVIEW_INSERT_FADE_BEZIER, Vector2D{0.5, 0.0}, Vector2D{0.5, 0.0});
    if (!Animation::mgr()->bezierExists(OVERVIEW_REMOVE_FADE_BEZIER))
        Animation::mgr()->addBezierWithName(OVERVIEW_REMOVE_FADE_BEZIER, Vector2D{0.5, 1.0}, Vector2D{0.5, 1.0});
    // Decelerate in (most of the distance early, so the overview is legible
    // well before it stops moving), accelerate out (leave slowly enough to read
    // as motion, then get out of the way): the animation:curve preset's pair,
    // registered under the house names. Motion::sync owns the numbers.
    Hyprdrive::Motion::sync();

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
    openAnimConfig->internalSpeed   = Hyprdrive::Config::getOpenSpeed();
    openAnimConfig->internalEnabled = WINDOWSMOVEVALUES ? WINDOWSMOVEVALUES->internalEnabled : 1;
    openAnimConfig->internalStyle   = "";
    openAnimConfig->pValues         = openAnimConfig;

    closeAnimConfig                  = makeShared<Hyprutils::Animation::SAnimationPropertyConfig>();
    closeAnimConfig->overridden      = true;
    closeAnimConfig->internalBezier  = OVERVIEW_CLOSE_BEZIER;
    closeAnimConfig->internalSpeed   = Hyprdrive::Config::getCloseSpeed();
    closeAnimConfig->internalEnabled = WINDOWSMOVEVALUES ? WINDOWSMOVEVALUES->internalEnabled : 1;
    closeAnimConfig->internalStyle   = "";
    closeAnimConfig->pValues         = closeAnimConfig;

    reverseAnimConfig                  = makeShared<Hyprutils::Animation::SAnimationPropertyConfig>();
    reverseAnimConfig->overridden      = true;
    reverseAnimConfig->internalBezier  = OVERVIEW_OPEN_BEZIER; // ease-OUT: maximum slope at t=0
    reverseAnimConfig->internalSpeed   = Hyprdrive::Config::getCloseSpeed();
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
    searchAnimConfig->internalSpeed   = SEARCH_ANIM_SPEED;
    searchAnimConfig->internalEnabled = WINDOWSMOVEVALUES ? WINDOWSMOVEVALUES->internalEnabled : 1;
    searchAnimConfig->internalStyle   = "";
    searchAnimConfig->pValues         = searchAnimConfig;

    Animation::mgr()->createAnimation(0.F, resultsBand, searchAnimConfig, AVARDAMAGE_NONE);
    Animation::mgr()->createAnimation(0.F, resultsWidth, searchAnimConfig, AVARDAMAGE_NONE);
    // X50 rides the same config as the band it travels with: the pill lifting on
    // one curve while the panel it uncovers grows on another reads as two events.
    Animation::mgr()->createAnimation(0.F, searchDock, searchAnimConfig, AVARDAMAGE_NONE);
    Animation::mgr()->createAnimation(0.F, pillNudge, searchAnimConfig, AVARDAMAGE_NONE);
    Animation::mgr()->createAnimation(0.F, gridNudge, searchAnimConfig, AVARDAMAGE_NONE);
    Animation::mgr()->createAnimation(0.F, scratchpadReveal, searchAnimConfig, AVARDAMAGE_NONE);
    Animation::mgr()->createAnimation(0.F, altStripsReveal, searchAnimConfig, AVARDAMAGE_NONE);
    syncPluginAnimEnabled();

    scale->setUpdateCallback([this](auto) { damage(); });
    viewOffset->setUpdateCallback([this](auto) { damage(); });
    workspaceInsertProgress->setUpdateCallback([this](auto) { damage(); });
    workspaceInsertFadeProgress->setUpdateCallback([this](auto) { damage(); });
    reflowPacer->setUpdateCallback([this](auto) { damage(); });
    resultsBand->setUpdateCallback([this](auto) {
        searchLayoutHere().bandH = resultsBand->value();
        damage();
    });
    resultsWidth->setUpdateCallback([this](auto) { damage(); });
    searchDock->setUpdateCallback([this](auto) { damage(); });
    pillNudge->setUpdateCallback([this](auto) { damage(); });
    gridNudge->setUpdateCallback([this](auto) {
        searchLayoutHere().gridShiftL = gridNudge->value();
        damage();
    });
    scratchpadReveal->setUpdateCallback([this](auto) { damage(); });
    altStripsReveal->setUpdateCallback([this](auto) { damage(); });

    // A previous overview's band must not open the next one already pushed, nor
    // its pill start docked. The published layout is the ONLY group of session
    // state this may touch: the query and the nav zone belong to the dispatcher
    // that asked for the overview, and a second monitor opening must not wipe
    // the query the first one is showing.
    g_search.resetLayout(PMONITOR ? PMONITOR->m_id : MONITOR_INVALID);

    if (!swipe)
        *scale = Hyprdrive::Config::getScale();

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
    // KEYBOARD DESKTOP SWITCH, ANY KEY (user, 2026-10-15: "motion blur isn't
    // working at all for some of the intended purposes ... in native only").
    // The keyboard blur only knew the arrow walk (popKeyboardBadge); a desktop
    // switched by a BIND with the overview up -- Super+1..0 (native borrows
    // the user's chords into the overview's submap), Super+Tab, a
    // hyprdrive:desktop key -- panned the cards sharp. The switch itself runs
    // inside the bind (changeWorkspace emits workspace.active synchronously),
    // so the bind is still current here; onWorkspaceChange pans the cards on
    // the next frame and KEYPAN reads this.
    workspaceActiveHook = Event::bus()->m_events.workspace.active.listen([this](PHLWORKSPACE workspace) {
        if (workspace && !workspace->m_isSpecialWorkspace && workspace->m_monitor == pMonitor && g_pKeybindManager && g_pKeybindManager->m_currentKeybind)
            (currentBindIsWheel() ? wheelPanAt : keyPanAt) = Time::steadyNow();
    });
    activateSubmapIfConfigured();
    // The submap only owns the KEYBOARD if it actually binds keys. Gating this
    // on the submap merely existing left arrows, Tab and Return with no handler
    // at all in a rice whose submap exists purely for the mouse P0 fix (a click
    // with SUPER still held); measured 2026-08-01, the arrow keys were dead.
    if (!hasOverviewSubmapKeyBinds())
        keyboardKeyHook = Event::bus()->m_events.input.keyboard.key.listen([this](IKeyboard::SKeyEvent a0, Event::SCallbackInfo& a1) { onKeyboardKey(a0, a1); });

    Pointer::Cursor::overrideController->setOverride("left_ptr", Pointer::Cursor::CURSOR_OVERRIDE_SPECIAL_ACTION);
    g_overviewCursorYielded = false;
    OpenProf::mark("ctor hooks + anims");

    redrawAll();
    OpenProf::mark("ctor redrawAll");

    // The window the session opened on -- the first overview up records it;
    // the others open after the keyboard is gone and would read the same one.
    if (scrollOverviews().size() <= 1)
        OvInternal::g_focusAtOpen = Desktop::focusState()->window();
    Hyprdrive::Stacks::traceEvent(std::format("locks: open on {} (overviews={}) focusAtOpen={}", pMonitor ? pMonitor->m_name : "?", scrollOverviews().size(),
                                                   OvInternal::g_focusAtOpen.lock() ? OvInternal::g_focusAtOpen->m_class : "-"));
    rememberSelection(Desktop::focusState()->window());
    viewportCurrentWorkspace = activeWorkspaceIndex();
    syncSelectionToViewport();
    stealOverviewKeyboard();
    OpenProf::mark("ctor end");
}

void CScrollOverview::stepDesktopCard(bool next) {
    (currentBindIsWheel() ? wheelPanAt : keyPanAt) = Time::steadyNow();
    moveViewportWorkspace(next);
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

    // ONE STATE ACROSS MONITORS (user, 2026-09-27: "sometimes overview will
    // close on one monitor but stay open in the other"). Several close paths
    // reach only their own monitor's overview -- a tile click, the dock's Exit
    // Overview, a scratchpad click -- so the rule lives here, not at each of
    // them: this closing closes every other overview too. They go FIRST, so
    // this one's teardown -- whose focus choice is what the user just asked for
    // -- runs last and has the final word. The static guard stops each of them
    // turning round and closing the rest again.
    static bool s_closingAll = false;
    // focus_follows_pick: closed by another monitor's overview, which holds
    // the user's pick, so this one must not move focus on its way out. Each
    // used to focus its own selected window, and the pointer followed focus
    // onto the wrong monitor (PR #2, 2026-10-19).
    const bool  BYSTANDER    = s_closingAll && pickKeepsFocus();
    if (!s_closingAll) {
        s_closingAll      = true;
        const auto OTHERS = scrollOverviews(); // a copy: closing can unregister
        for (const auto& OV : OTHERS)
            if (OV && OV.get() != this)
                OV->close();
        s_closingAll = false;
    }

    // X28. The pill only exists while the overview does, so its menu must go
    // with it -- otherwise a stale plate is still holding every pointer press
    // the next time the overview opens.
    Hyprdrive::Dock::closeMenu();

    // X32/G12. Same reasoning for both drags: the overview can close under a
    // held button (Escape, a keybind, a window taking focus), and the release
    // that would have ended the gesture then never reaches this hook. Neither
    // may survive into the next open as a phantom drop.
    Hyprdrive::Dock::dragCancel();
    Hyprdrive::Dock::setForeignInsert(-1);
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
    if (closeOnWindow && Hyprdrive::Minimize::isMinimized(closeOnWindow.lock()))
        Hyprdrive::Minimize::restore(closeOnWindow.lock());

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

            if (!BYSTANDER)
                Desktop::focusState()->fullWindowFocus(closeOnWindow.lock(), Desktop::FOCUS_REASON_DESKTOP_STATE_CHANGE);

            startedOn                = FINALWORKSPACE;
            viewportCurrentWorkspace = targetIdx;

            const auto FINALPITCH = getWorkspaceLogicalPitch(pMonitor.lock(), 1.F, layout);
            viewOffset->setValueAndWarp(axisOffsetVector(workspaceOverviewLogicalOffset(sourceIdx, targetIdx, FINALPITCH), layout));
            *viewOffset = Vector2D{};

            focusSyncedFromWorkspaceID = WORKSPACE_INVALID;

            const auto FINALWINDOW = getOverviewWindowToShow(closeOnWindow.lock());
            finishClose(FINALWORKSPACE, FINALWINDOW);
            restoreOverviewKeyboard(FINALWINDOW, "close-synced");
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

        // focus_follows_pick, a desktop picked off its windows: with the other
        // overviews no longer grabbing focus, changeWorkspace(noFocus) would
        // leave it on the desktop just left. Focus goes to the desktop picked:
        // its last window, or this monitor with nothing focused when empty.
        if (pickKeepsFocus() && !BYSTANDER && focusSyncedFromWorkspaceID != WORKSPACE_INVALID && SELECTEDWORKSPACE &&
            SELECTEDWORKSPACE == pMonitor->m_activeWorkspace) {
            const auto LAST = getOverviewWindowToShow(SELECTEDWORKSPACE->getLastFocusedWindow());
            if (LAST && LAST->m_workspace == SELECTEDWORKSPACE && shouldShowOverviewWindow(LAST))
                Desktop::focusState()->fullWindowFocus(LAST, Desktop::FOCUS_REASON_DESKTOP_STATE_CHANGE);
            else {
                Desktop::focusState()->rawMonitorFocus(pMonitor.lock());
                Desktop::focusState()->fullWindowFocus(nullptr, Desktop::FOCUS_REASON_DESKTOP_STATE_CHANGE);
            }
        }
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

        if (!BYSTANDER)
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
    restoreOverviewKeyboard(FINALWINDOW, "close");
}

void CScrollOverview::onPreRender() {
    if (pMonitor)
        pMonitor->m_solitaryClient.reset();

    // The scratchpad sliding in or out over the overview: its animation is the
    // workspace's, which asks for nothing the overview listens to.
    if (const auto SPECIAL = overviewShownSpecialOn(pMonitor.lock()); SPECIAL && (SPECIAL->m_renderOffset->isBeingAnimated() || SPECIAL->m_alpha->isBeingAnimated()))
        requestInputFrame();

    drawnWindows.clear();
    drawnSet.clear();

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
    if (TileChrome::animatingOn(pMonitor.lock()) || searchHoverEasing())
        gridSetReflowActive(pMonitor.lock());

    // V2. The SUPER-hold pill eases on the same kind of wall clock, and inside
    // an overview it needs the frames asked for the same way: hkScheduleFrame
    // throttles AQ_SCHEDULE_DAMAGE for as long as an overview is open, so the
    // dock's pumpFrame -- a plain damageMonitor -- is swallowed here and the
    // reveal would freeze half out over a still cursor. Joining the reflow
    // pacer rather than adding a fourth pump, for the reason Dock::needsFrames
    // gives: one list, in one place.
    // ...and the pill is on the HOVERED tile, so it is this output's business
    // only while this output owns the pointer. It used to pump both.
    if (WindowPill::animating() && isOverviewPointerOnMonitor(pMonitor.lock()))
        gridSetReflowActive(pMonitor.lock());

    // The strip cell's label fades on the same kind of wall clock, and the idle
    // pulse alone (~15/s) gave its 110 ms exit one frame at most: it stood at
    // full strength, then vanished (user, 2026-09-24: "same fading out as the
    // grid"). It joins the pacer too, while it fades.
    if (stripTipAnimating())
        gridSetReflowActive(pMonitor.lock());

    // The border fade (BorderFade.hpp) rings -- tiles, plates, strip cells,
    // the dock pill -- on the same pacer, for the same reason: wall-clock
    // eases, and plain damage is throttled while an overview is open.
    if (Hyprdrive::BorderFade::animatingOn(pMonitor.lock()))
        gridSetReflowActive(pMonitor.lock());

    if (gridTakeReflowActive(pMonitor.lock())) {
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

    // The same reasoning, for the lists' OTHER assumption: they were built for
    // one query and a longer query only narrows it. "Ghostty" replaced by "f"
    // (Show Windows, then typing) never passes through empty, and the windows
    // "f" matches that "Ghostty" did not -- the scratchpad's among them -- were
    // laid out by the grid but never drawn (user, 2026-09-24).
    if (!g_search.query.text.starts_with(lastBuiltQuery))
        rebuildPending = true;

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
    // WHERE THE STRIP IS ON SCREEN RIGHT NOW: a swipe drags the camera
    // (viewOffset) before the workspace changes. The insert/remove transition
    // below restarts from viewOffset 0, so its old offsets must carry the drag
    // or the strip snaps back a drag's length for a frame -- swiping off an
    // empty desktop (which removes it) flashed the neighbour card into the
    // middle (user, 2026-11-03).
    const float PREVIOUSDRAG = sc<float>(axisValue(viewOffset->value(), layout));

    // consume any pending gesture-driven settle (set by finishWorkspaceScrollFollow)
    const bool   GESTURESETTLE       = trackpadGestureSettlePending;
    const double GESTURESETTLEOFFSET = trackpadGestureSettleOffset;
    trackpadGestureSettlePending     = false;

    std::vector<WORKSPACEID> previousWorkspaceIDs;
    previousWorkspaceIDs.reserve(images.size());
    std::unordered_map<WORKSPACEID, float> previousWorkspaceOffsets;
    std::unordered_map<WORKSPACEID, CBox>  previousStripBoxes;
    // Where each tile was last drawn, and on which desktop's card (before the
    // cards are rebuilt: cardIdx is an index into the old row).
    std::unordered_map<const Desktop::View::CWindow*, decltype(workspaceInsertTransition.tileFrom)::mapped_type> previousTiles;
    for (const auto& [WIN, LAST] : lastTileBoxes)
        if (LAST.cardIdx < images.size() && images[LAST.cardIdx] && images[LAST.cardIdx]->pWorkspace)
            previousTiles.emplace(WIN, decltype(workspaceInsertTransition.tileFrom)::mapped_type{LAST.box, workspaceKey(images[LAST.cardIdx]->pWorkspace), LAST.card});
    const auto PREVIOUSLOGICALPITCH = getWorkspaceLogicalPitch(pMonitor.lock(), scale->value(), layout);
    for (size_t i = 0; i < images.size(); ++i) {
        const auto& image = images[i];
        if (!image || !image->pWorkspace)
            continue;

        previousWorkspaceIDs.push_back(workspaceKey(image->pWorkspace));
        previousWorkspaceOffsets.emplace(workspaceKey(image->pWorkspace), workspaceOverviewLogicalOffset(i, previousActiveIdx, PREVIOUSLOGICALPITCH));
        previousStripBoxes.emplace(workspaceKey(image->pWorkspace), stripCellLogicalBox(i));
    }

    const auto NEWWORKSPACE      = pMonitor->m_activeWorkspace;
    const bool INSERTEDWORKSPACE = std::find(previousWorkspaceIDs.begin(), previousWorkspaceIDs.end(), workspaceKey(NEWWORKSPACE)) == previousWorkspaceIDs.end();
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
    const bool LEFTEMPTY = previousStartedOn && previousStartedOn != NEWWORKSPACE && !PREVIOUSISOURS && !previousStartedOn->m_isSpecialWorkspace &&
        !previousStartedOn->isPersistent() && previousStartedOn->getWindowCount() == 0;
    // THE EMPTY DESKTOP YOU LEFT IS THE SPARE ANYWAY (user, 2026-11-03: swiping
    // off it, the top strip "glitches for a frame"). The empty desktop at the
    // end of the strip is exactly the one E2 keeps as its trailing spare: removing
    // it let Hyprland reap it, and the next rebuild made the same id again as the
    // trailing workspace -- one frame with a desktop missing from the strip.
    // Keep it instead, held as the trailing workspace (reaped on close, as that
    // one always is).
    bool keptAsSpare = false;
    if (LEFTEMPTY && pMonitor) {
        const auto ROW = workspacePlanFor(pMonitor.lock());
        if (ROW.spare && ROW.existing && *ROW.spare == previousStartedOn->m_id && previousStartedOn->m_monitor.lock() == pMonitor.lock()) {
            trailingWorkspace = previousStartedOn;
            keptAsSpare       = true;
        }
    }
    const bool SHOULDREMOVEPREVIOUSWORKSPACE = LEFTEMPTY && !keptAsSpare;
    const auto REMOVEDWORKSPACE = REQUESTEDREMOVEDWORKSPACE ? REQUESTEDREMOVEDWORKSPACE : SHOULDREMOVEPREVIOUSWORKSPACE ? previousStartedOn : PHLWORKSPACE{};

    pendingRemovedWorkspace = REMOVEDWORKSPACE;

    startedOn = NEWWORKSPACE;
    redrawAll();
    viewportCurrentWorkspace = activeWorkspaceIndex();

    const bool REMOVEDPREVIOUSWORKSPACE =
        REMOVEDWORKSPACE &&
        std::find_if(images.begin(), images.end(), [REMOVEDWORKSPACE](const auto& image) { return image && image->pWorkspace == REMOVEDWORKSPACE; }) == images.end();

    // REORDERED: the same desktops in another order (one moved along the
    // strip). Every card and cell glides from where it was to its new place;
    // a dropped cell starts from where it was let go (stripDropBoxes).
    bool REORDERED = std::exchange(stripSettle, false);
    {
        std::vector<WORKSPACEID> was, now;
        for (const auto& image : images)
            if (image && image->pWorkspace && std::ranges::find(previousWorkspaceIDs, workspaceKey(image->pWorkspace)) != previousWorkspaceIDs.end())
                now.push_back(workspaceKey(image->pWorkspace));
        for (const auto K : previousWorkspaceIDs)
            if (std::ranges::find(now, K) != now.end())
                was.push_back(K);
        REORDERED |= was != now;
    }
    for (const auto& [K, BOX] : stripDropBoxes)
        previousStripBoxes[K] = BOX;
    stripDropBoxes.clear();

    // A FULLSCREEN APP ON AN EMPTY DESKTOP THAT WAS ALREADY THERE (the next
    // free place, FullscreenDesktop.cpp): shown as if made for it -- out from
    // under the desktop it left; and when the app goes back, that desktop goes
    // back under as a ghost while the empty one fades in at its own place.
    // (Before, both were a plain pan: user's recording, 2026-11-06.)
    const bool PREVKNOWN   = previousStartedOn && previousStartedOn != NEWWORKSPACE && previousWorkspaceOffsets.contains(workspaceKey(previousStartedOn));
    const bool REUSEDENTER = !INSERTEDWORKSPACE && PREVKNOWN && Hyprdrive::FullscreenDesktop::justEntered(NEWWORKSPACE);
    const bool REUSEDEXIT  = !REMOVEDPREVIOUSWORKSPACE && !REUSEDENTER && PREVKNOWN && Hyprdrive::FullscreenDesktop::justLeft(previousStartedOn) &&
        std::ranges::any_of(images, [&](const auto& image) { return image && image->pWorkspace == previousStartedOn; });
    const bool BORNLIKE    = INSERTEDWORKSPACE || REUSEDENTER;

    if (INSERTEDWORKSPACE || REMOVEDPREVIOUSWORKSPACE || REORDERED || REUSEDENTER || REUSEDEXIT) {
        workspaceInsertTransition.active                 = true;
        workspaceInsertTransition.transitionWorkspaceID  = BORNLIKE ? workspaceKey(NEWWORKSPACE) : REMOVEDPREVIOUSWORKSPACE ? workspaceKey(REMOVEDWORKSPACE) :
                                                            REUSEDEXIT                ? workspaceKey(previousStartedOn) :
                                                                                        WORKSPACE_INVALID;
        workspaceInsertTransition.ghost                  = REMOVEDPREVIOUSWORKSPACE || REUSEDEXIT;
        workspaceInsertTransition.bornUnder              = WORKSPACE_INVALID;
        workspaceInsertTransition.ghostTo                = 0.F;
        workspaceInsertTransition.ghostCellTo            = REMOVEDPREVIOUSWORKSPACE ? workspaceKey(NEWWORKSPACE) : WORKSPACE_INVALID;
        workspaceInsertTransition.reorder                = REORDERED && !INSERTEDWORKSPACE && !REMOVEDPREVIOUSWORKSPACE && !REUSEDENTER && !REUSEDEXIT;
        workspaceInsertTransition.oldStripBoxes          = previousStripBoxes;
        workspaceInsertTransition.tileFrom               = previousTiles;
        workspaceInsertTransition.transitionFadeIn       = BORNLIKE;
        workspaceInsertFadeProgress->setConfig(BORNLIKE ? workspaceInsertFadeConfig : workspaceRemoveFadeConfig);
        workspaceInsertTransition.oldRelativeOffsets.clear();
        workspaceInsertTransition.newRelativeOffsets.clear();
        workspaceInsertTransition.transitionOldRelativeOffset = 0.F;

        for (size_t i = 0; i < previousWorkspaceIDs.size(); ++i) {
            const auto OFFSET = (previousWorkspaceOffsets.contains(previousWorkspaceIDs[i]) ? previousWorkspaceOffsets.at(previousWorkspaceIDs[i]) : 0.F) - PREVIOUSDRAG;
            workspaceInsertTransition.oldRelativeOffsets.emplace(previousWorkspaceIDs[i], OFFSET);
            if ((REMOVEDPREVIOUSWORKSPACE && previousWorkspaceIDs[i] == workspaceKey(REMOVEDWORKSPACE)) ||
                (REUSEDEXIT && previousWorkspaceIDs[i] == workspaceKey(previousStartedOn)))
                workspaceInsertTransition.transitionOldRelativeOffset = OFFSET;
        }

        const auto NEWLOGICALPITCH = getWorkspaceLogicalPitch(pMonitor.lock(), scale->value(), layout);
        for (size_t i = 0; i < images.size(); ++i) {
            if (!images[i] || !images[i]->pWorkspace)
                continue;

            workspaceInsertTransition.newRelativeOffsets.emplace(workspaceKey(images[i]->pWorkspace), workspaceOverviewLogicalOffset(i, viewportCurrentWorkspace, NEWLOGICALPITCH));
        }

        // A DESKTOP BORN NEXT TO THE ONE SHOWN (a fullscreen app's, user
        // 2026-11-05: "show another desktop card sliding out from under it")
        // starts where that one was, beneath it, and slides out to its place;
        // its strip cell grows out of that one's cell. A REMOVED one slides
        // back under the desktop shown next.
        const auto FROMKEY = workspaceKey(previousStartedOn);
        if (BORNLIKE && previousStartedOn && previousWorkspaceOffsets.contains(FROMKEY)) {
            workspaceInsertTransition.bornUnder = FROMKEY;
            workspaceInsertTransition.oldRelativeOffsets[workspaceKey(NEWWORKSPACE)] = previousWorkspaceOffsets.at(FROMKEY) - PREVIOUSDRAG;
            // (A reused desktop's strip cell is already in its place.)
            if (INSERTEDWORKSPACE && previousStripBoxes.contains(FROMKEY))
                workspaceInsertTransition.oldStripBoxes[workspaceKey(NEWWORKSPACE)] = previousStripBoxes.at(FROMKEY);
        }
        if (REMOVEDPREVIOUSWORKSPACE || REUSEDEXIT)
            if (const auto IT = workspaceInsertTransition.newRelativeOffsets.find(workspaceKey(NEWWORKSPACE)); IT != workspaceInsertTransition.newRelativeOffsets.end())
                workspaceInsertTransition.ghostTo = IT->second;
        // The emptied desktop itself stays where it is (its ghost does the
        // going) and fades in there late (workspaceOverviewAlpha).
        if (REUSEDEXIT)
            if (const auto IT = workspaceInsertTransition.newRelativeOffsets.find(workspaceKey(previousStartedOn)); IT != workspaceInsertTransition.newRelativeOffsets.end())
                workspaceInsertTransition.oldRelativeOffsets[workspaceKey(previousStartedOn)] = IT->second;

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
        workspaceInsertTransition.oldStripBoxes.clear();
        workspaceInsertTransition.tileFrom.clear();
        workspaceInsertTransition.ghost = false;
        workspaceInsertTransition.bornUnder = WORKSPACE_INVALID;
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

void normalizeScrollingCamera(PHLWORKSPACE workspace) {
    normalizeFittingScrollingOffset(workspace);
}

void normalizeAllScrollingCameras() {
    for (const auto& w : State::workspaceState()->workspaces()) {
        if (const auto WORKSPACE = w.lock(); valid(WORKSPACE))
            normalizeFittingScrollingOffset(WORKSPACE);
    }
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
    const int ON = Hyprdrive::Config::motionEnabled() ? 1 : 0;
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

    // THE SPEED (user, 2026-09-30: "animation speed ... globally"). anim_scale
    // stretched only what runs on animElapsedMs; these Hyprland-animated
    // variables took it as on/off. Hyprland speeds are durations (tenths of a
    // second), so the scale multiplies them. Refreshed here every frame, so a
    // live change reaches the next animation of an open overview. (A reversal
    // sets its own speed from the same product, in useAnimDirection.)
    const float S = std::max(0.01F, Hyprdrive::Config::animScale());
    if (openAnimConfig)
        openAnimConfig->internalSpeed = Hyprdrive::Config::getOpenSpeed() * S;
    if (closeAnimConfig)
        closeAnimConfig->internalSpeed = Hyprdrive::Config::getCloseSpeed() * S;
    if (searchAnimConfig)
        searchAnimConfig->internalSpeed = SEARCH_ANIM_SPEED * S;
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
        const float OPEN   = Hyprdrive::Config::getScale();
        const float TARGET = opening ? OPEN : 1.F;
        const float RANGE  = std::max(0.01F, std::abs(1.F - OPEN));
        const float LEFT   = std::clamp(std::abs(TARGET - scale->value()) / RANGE, 0.F, 1.F);
        const float BASE   = opening ? Hyprdrive::Config::getOpenSpeed() : Hyprdrive::Config::getCloseSpeed();

        // Floor it: a hair of travel still deserves a frame or two of motion
        // rather than a teleport.
        reverseAnimConfig->internalSpeed = std::max(0.3F, BASE * std::max(0.01F, Hyprdrive::Config::animScale()) * LEFT);

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
    // Open again, as from the constructor: a zoom started during the close ends.
    Hyprdrive::Mosaic::onOverviewOpen();

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
    *scale      = Hyprdrive::Config::getScale();

    stealOverviewKeyboard();
    damage();
}

static_assert(Hyprdrive::SubmapBorrow::MOD_SHIFT == HL_MODIFIER_SHIFT && Hyprdrive::SubmapBorrow::MOD_CTRL == HL_MODIFIER_CTRL &&
              Hyprdrive::SubmapBorrow::MOD_ALT == HL_MODIFIER_ALT && Hyprdrive::SubmapBorrow::MOD_META == HL_MODIFIER_META);

static void borrowGlobalPassthroughBinds(const std::string& fromSubmap) {
    if (!g_pKeybindManager)
        return;

    for (const auto& keybind : g_pKeybindManager->m_keybinds) {
        if (!keybind || keybind->submapUniversal || keybind->submap.name != fromSubmap ||
            !Hyprdrive::SubmapBorrow::borrows(keybind->key, keybind->modmask, keybind->mouse, keybind->catchAll))
            continue;

        keybind->submapUniversal = true;
        g_passthroughBinds.emplace_back(keybind);
    }
}

static void returnGlobalPassthroughBinds() {
    for (const auto& ref : g_passthroughBinds) {
        if (const auto keybind = ref.lock())
            keybind->submapUniversal = false;
    }
    g_passthroughBinds.clear();
}

void CScrollOverview::activateSubmapIfConfigured() {
    if (!sharedStateOwner || !usesSubmapKeybinds || !g_pKeybindManager)
        return;

    previousSubmapName = g_pKeybindManager->getCurrentSubmap().name;
    // NEVER return INTO the overview's own submap (user, 2026-09-30: the whole
    // bar stopped taking clicks and every Omarchy shortcut died). Opened while a
    // stale overview submap was still current -- a plugin reload mid-overview
    // left one behind -- this recorded it as "where to go back to", so every
    // close re-entered it: only the submap's two click binds stayed live, and
    // they ate every left click.
    if (isOverviewSubmapName(previousSubmapName))
        previousSubmapName.clear();

    const auto SUBMAP = boundOverviewSubmap();
    if (SUBMAP != std::string_view{OVERVIEW_SUBMAP})
        Hyprdrive::Compat::noteLegacyUse(Hyprdrive::Compat::ELegacy::KEY, std::string{"submap "} + SUBMAP);

    const auto RESULT = Config::Actions::setSubmap(SUBMAP);
    if (!RESULT) {
        usesSubmapKeybinds = false;
        return;
    }

    submapActive = true;
    borrowGlobalPassthroughBinds(previousSubmapName);
}

// No overview, yet Hyprland sits in the overview's submap: every normal bind
// is dead and its click binds eat the pointer. Called at plugin load, on
// every config reload, and after an overview's teardown.
void healStuckOverviewSubmap() {
    if (!g_pKeybindManager || !scrollOverviews().empty())
        return;
    if (isOverviewSubmapName(g_pKeybindManager->getCurrentSubmap().name))
        (void)Config::Actions::setSubmap("reset");
}

void CScrollOverview::restoreSubmapIfActive() {
    if (!submapActive || !g_pKeybindManager)
        return;

    const auto CURRENT = g_pKeybindManager->getCurrentSubmap().name;
    if (isOverviewSubmapName(CURRENT)) {
        (void)Config::Actions::setSubmap(previousSubmapName.empty() ? "reset" : previousSubmapName);
    }

    returnGlobalPassthroughBinds();
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

void OvInternal::releasePointerLocks() {
    // Hyprland lifts a lock only when keyboard focus moves off its owner, and
    // the overview nulls the seat without moving Desktop focus -- so the lock
    // held the pointer still under the overview (user, 2026-09-27: Moonlight).
    if (!g_pInputManager)
        return;
    int active = 0;
    for (const auto& c : g_pInputManager->m_constraints)
        if (const auto C = c.lock(); C && C->isActive())
            ++active;
    if (active)
        Hyprdrive::Stacks::traceEvent(std::format("locks: release {} (overviews={} closing0={})", active, scrollOverviews().size(),
                                                       scrollOverviews().empty() || !scrollOverviews().front() ? -1 : (int)scrollOverviews().front()->isClosing()));
    g_pInputManager->unconstrainMouse();
}

void overviewCenterCursorOnHandoff() {
    g_warpCentred = true;
}

void overviewDesktopsRenumbered() {
    for (const auto& OV : scrollOverviews())
        if (auto* S = dynamic_cast<CScrollOverview*>(OV.get()))
            S->desktopsRenumbered();
}

void overviewSuperChanged() {
    for (const auto& OV : scrollOverviews())
        if (auto* S = dynamic_cast<CScrollOverview*>(OV.get()))
            S->superChanged();
}

bool overviewMoveShownDesktop(int dir) {
    const auto MON = Desktop::focusState()->monitor();
    for (const auto& OV : scrollOverviews())
        if (auto* S = dynamic_cast<CScrollOverview*>(OV.get()); S && S->pMonitor.lock() == MON) {
            S->moveShownDesktop(dir);
            return true;
        }
    return false;
}

void CScrollOverview::superChanged() {
    if (closing)
        return;
    damage();
    requestInputFrame();
}

// A desktop with windows on it, numbered (not special, not named).
bool CScrollOverview::deskMovable(size_t idx) const {
    if (idx >= images.size() || !images[idx] || !Hyprdrive::DesktopOrder::ordinary(images[idx]->pWorkspace))
        return false;
    return std::ranges::any_of(images[idx]->windows, [](const PHLWINDOWREF& r) {
        const auto W = r.lock();
        return W && W->m_isMapped;
    });
}

bool CScrollOverview::moveDesktop(size_t from, size_t to) {
    if (from >= images.size() || to >= images.size())
        return false;
    if (from == to) {
        stripSettle = true; // dropped where it was: it settles back
        onWorkspaceChange();
        return true;
    }
    std::vector<PHLWORKSPACE> order;
    std::vector<WORKSPACEID>  ids;
    for (const auto& I : images) {
        if (!I || !Hyprdrive::DesktopOrder::ordinary(I->pWorkspace))
            return false;
        order.push_back(I->pWorkspace);
        ids.push_back(I->pWorkspace->m_id);
    }
    std::ranges::sort(ids);
    const auto W = order[from];
    order.erase(order.begin() + sc<std::ptrdiff_t>(from));
    order.insert(order.begin() + sc<std::ptrdiff_t>(to), W);
    std::vector<std::pair<PHLWORKSPACE, WORKSPACEID>> moves;
    for (size_t k = 0; k < order.size(); ++k)
        moves.emplace_back(order[k], ids[k]);
    Hyprdrive::DesktopOrder::renumber(moves);
    // (renumber asked for a rebuild; this one runs now, with the transition.)
    onWorkspaceChange();
    return true;
}

bool CScrollOverview::moveShownDesktop(int dir) {
    const auto MON = pMonitor.lock();
    if (!MON || closing || images.empty() || deskDrag.pressed)
        return false;
    const size_t IDX = activeWorkspaceIndex();
    if (!deskMovable(IDX))
        return false;
    const long TO = sc<long>(IDX) + dir;
    if (TO < 0 || TO >= sc<long>(images.size()))
        return false;
    return moveDesktop(IDX, sc<size_t>(TO));
}

// The main button with Super held over a movable desktop: it is picked up.
// Let go without moving it, it was a click (the desktop is picked, as ever).
// THE RIGHT BUTTON picks one up too, no Super needed (user, 2026-11-08: "add
// right click to drag desktops in the desktop strip ... another way to drag a
// workspace around"). Only the button that picked it up puts it down; a right
// click that never moved puts it back without picking that desktop.
bool CScrollOverview::deskDragButton(bool pressed, bool right) {
    if (pressed) {
        if (deskDrag.pressed)
            return true; // the other button, mid-drag: the drag keeps it
        if (!right && (!g_pInputManager || !(g_pInputManager->getModsFromAllKBs() & HL_MODIFIER_META)))
            return false;
        size_t idx = images.size();
        if (!workspaceAtStripPoint(lastMousePosLocal, &idx) || !deskMovable(idx))
            return false;
        const CBox CELL = stripCellBox(idx, pMonitor.lock(), scale->value());
        deskDrag         = {};
        deskDrag.pressed = true;
        deskDrag.right   = right;
        deskDrag.ws      = images[idx]->pWorkspace;
        deskDrag.from = deskDrag.to = idx;
        deskDrag.start              = lastMousePosLocal;
        deskDrag.grab               = lastMousePosLocal - Vector2D{CELL.x, CELL.y};
        damage();
        requestInputFrame();
        return true;
    }
    if (!deskDrag.pressed)
        return false;
    if (deskDrag.right != right)
        return true; // the other button's release: the drag keeps going
    const auto D = std::move(deskDrag);
    deskDrag     = {};
    if (D.live) {
        // The desktop may have moved under the drag (a fullscreen app): find it again.
        size_t from = SIZE_MAX;
        for (size_t i = 0; i < images.size(); ++i)
            if (images[i] && images[i]->pWorkspace == D.ws.lock())
                from = i;
        stripDropBoxes = D.drawn;
        if (from == SIZE_MAX || !moveDesktop(from, std::min(D.to, images.size() - 1)))
            stripDropBoxes.clear();
    } else if (!D.right)
        performPrimaryClickAction();
    damage();
    requestInputFrame();
    return true;
}

bool CScrollOverview::deskDragMove() {
    if (!deskDrag.pressed) {
        // Super over the strip: the lift follows the pointer from cell to cell.
        if (g_pInputManager && (g_pInputManager->getModsFromAllKBs() & HL_MODIFIER_META) && stripUiContains(lastMousePosLocal)) {
            damage();
            requestInputFrame();
        }
        return false;
    }
    const auto  MON = pMonitor.lock();
    const float THR = Hyprdrive::Config::getDragThreshold() * (MON ? MON->m_scale : 1.F);
    if (!deskDrag.live && deskDrag.start.distanceSq(lastMousePosLocal) > THR * THR)
        deskDrag.live = true;
    if (deskDrag.live && MON && deskDrag.from < images.size()) {
        // Its place: the slot whose centre is nearest the lifted cell's.
        const CBox  OWN = stripCellBox(deskDrag.from, MON, scale->value());
        const float CX  = sc<float>(lastMousePosLocal.x - deskDrag.grab.x) + sc<float>(OWN.width) / 2.F;
        float       best = 1e9F;
        for (size_t i = 0; i < images.size(); ++i) {
            const CBox B = stripCellBox(i, MON, scale->value());
            if (const float DX = std::abs(sc<float>(B.x + B.width / 2.0) - CX); DX < best) {
                best        = DX;
                deskDrag.to = i;
            }
        }
    }
    damage();
    requestInputFrame();
    return true;
}

// Unload: the queued warp would call into this plugin after dlclose. And no NEW
// one may be queued after this: PLUGIN_EXIT destroys any open overview AFTER
// calling this, that teardown hands focus back, and the handback queued a fresh
// warp -- a doLater into unloaded code that CEventLoopManager's destructor then
// freed at Hyprland exit (SIGSEGV exiting with the overview open, 2026-09-29).
void OvInternal::cancelPointerHandoff() {
    g_handoffShutdown = true;
    if (g_warpSeq && g_pEventLoopManager)
        g_pEventLoopManager->removeDoLater(g_warpSeq);
    g_warpSeq = 0;
}

void OvInternal::handPointerTo(const PHLWINDOW& window, const SP<CWLSurfaceResource>& surface) {
    if (!window)
        return;

    // Closing back onto the window that holds a lock (Escape out of the
    // overview over a game) gives it the lock back.
    if (const auto HLSURF = surface ? Desktop::View::CWLSurface::fromResource(surface) : nullptr; HLSURF && HLSURF->constraint()) {
        HLSURF->constraint()->activate();
        Hyprdrive::Stacks::traceEvent(std::format("locks: handed back to {} active={}", window->m_class, HLSURF->constraint()->isActive() ? 1 : 0));
        return;
    }

    // The cursor goes with the focus -- ONCE, after the whole close has run.
    // Every monitor's overview hands back in turn, in no set order, and warping
    // at each handback let a late one drag the cursor off the pick (2026-09-27).
    // Left where the tile was clicked it sat over the old window -- a
    // fullscreen game, usually -- and follow_mouse handed focus straight back
    // on the first nudge. Forced: no_warps is about Hyprland's own warps, and
    // this one is the selection itself.
    if (g_warpSeq || !g_pEventLoopManager || g_handoffShutdown)
        return;
    g_warpSeq = g_pEventLoopManager->doLater([] {
        g_warpSeq        = 0;
        const bool CENTRE = std::exchange(g_warpCentred, false);
        const auto FINAL  = Desktop::focusState()->window();
        if (!Desktop::View::validMapped(FINAL) || (FINAL == g_focusAtOpen.lock() && !CENTRE))
            return;
        const CBox BOX{FINAL->m_realPosition->goal(), FINAL->m_realSize->goal()};
        if (BOX.width < 1 || BOX.height < 1 || (!CENTRE && BOX.containsPoint(g_pInputManager->getMouseCoordsInternal())))
            return;
        Hyprdrive::Stacks::traceEvent(std::format("locks: warp into {}", FINAL->m_class));
        Pointer::pointerController()->warpTo(BOX.middle(), true);
    });
}

void OvInternal::traceHandback(const char* who, const PHLWINDOW& asked, const PHLWINDOW& target, const PHLWINDOW& before) {
    const auto NAME = [](const PHLWINDOW& w) { return w ? w->m_class : std::string{"-"}; };
    Hyprdrive::Stacks::traceEvent(std::format("locks: handback {} asked={} target={} before={} after={}", who, NAME(asked), NAME(target), NAME(before),
                                                   NAME(Desktop::focusState()->window())));
}

Hyprdrive::DesktopTape::STape Hyprdrive::DesktopTape::probe(const PHLWORKSPACE& workspace) {
    STape tape;
    const auto ALGO = overviewScrollingAlgorithmForWorkspace(workspace);
    if (!ALGO || !ALGO->m_scrollingData || !ALGO->m_scrollingData->controller || !ALGO->m_scrollingData->controller->isPrimaryHorizontal())
        return tape;
    tape.offset    = ALGO->m_scrollingData->controller->getOffset();
    tape.maxOffset = std::max(0.0, ALGO->m_scrollingData->maxWidth() - ALGO->primaryViewportSize());
    tape.usable    = tape.maxOffset > 1.0;
    return tape;
}

void Hyprdrive::DesktopTape::setOffset(const PHLWORKSPACE& workspace, double offset) {
    const auto ALGO = overviewScrollingAlgorithmForWorkspace(workspace);
    if (!ALGO || !ALGO->m_scrollingData || !ALGO->m_scrollingData->controller)
        return;
    ALGO->m_scrollingData->controller->setOffset(clampOverviewScrollingOffset(ALGO, offset));
    ALGO->m_scrollingData->recalculate();
}
