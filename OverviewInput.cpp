#include "OverviewInternal.hpp"
#include "DockInput.hpp"
using namespace OvInternal; // opted in here rather than leaked from the header

// ---------------------------------------------------------------------------
// Event handlers. These were inline [this] lambdas inside the constructor,
// which is why it was 958 lines. Moved verbatim -- the [this]-only capture
// list is the proof none of them could reference a constructor local.
// ponytail: their own TU now -- the helpers they need live in OverviewInternal.hpp.
// ---------------------------------------------------------------------------

void CScrollOverview::onMouseMove(Vector2D, Event::SCallbackInfo& info) {
        const auto INPUTOVERVIEW = scrollOverviewAt(g_pInputManager->getMouseCoordsInternal());
        if (INPUTOVERVIEW) {
            const auto INPUTMONITOR = INPUTOVERVIEW->pMonitor.lock();
            if (INPUTMONITOR && INPUTMONITOR != Desktop::focusState()->monitor())
                Desktop::focusState()->rawMonitorFocus(INPUTMONITOR);
        }

        if (closing || (g_pointerGrabOverview && g_pointerGrabOverview != this) || (!g_pointerGrabOverview && INPUTOVERVIEW.get() != this))
            return;

        const bool     LEFT_HANDED           = ScrollOverview::Config::getLeftHanded();
        const uint32_t MAIN_BUTTON           = LEFT_HANDED ? BTN_RIGHT : BTN_LEFT;
        const bool     INVERT_DRAG_MODE      = ScrollOverview::Config::getDragMode() == 1;
        const uint32_t SECONDARY_DRAG_BUTTON = BTN_MIDDLE;
        const float    DRAGTHRESHOLD         = ScrollOverview::Config::getDragThreshold() * (pMonitor ? pMonitor->m_scale : 1.F);
        const float    DRAGTHRESHOLDSQ       = std::pow(DRAGTHRESHOLD, 2);

        lastMousePosLocal = getOverviewMousePosLocal(pMonitor.lock());

        updateTileHover();

        // The dock's hover highlight is the only thing on screen that changes on
        // a bare pointer move over an otherwise idle overview: no tile hover
        // changes there, so nothing else asks for a frame and the highlight would
        // simply never appear (measured -- the two captures differed by 490 px,
        // all of them the cursor plane). A plain damageMonitor is not enough
        // either: hkScheduleFrame throttles AQ_SCHEDULE_DAMAGE for as long as an
        // overview is open, so the frame has to be asked for the overview's way.
        if (ScrollOverview::Dock::damageIfChanged(pMonitor.lock())) {
            damage();
            requestInputFrame();
        }

        // X32. The dock's drag lives in the dock; the overview only forwards the
        // motion, exactly as it forwards the button edges.
        if (ScrollOverview::Dock::dragPending() && ScrollOverview::Dock::dragMove(pMonitor.lock(), lastMousePosLocal)) {
            damage();
            requestInputFrame();
        }

        // G12. The hold has to pass BEFORE the distance is even looked at.
        if (g_search.pointer.drag && !g_search.pointer.drag->desktopId.empty()) {
            if (!g_search.pointer.drag->live) {
                const float HELDMS = ScrollOverview::Config::animElapsedMs(g_search.pointer.drag->startedAt);
                if (HELDMS >= G12_HOLD_MS && g_search.pointer.drag->start.distanceSq(lastMousePosLocal) > DRAGTHRESHOLDSQ)
                    g_search.pointer.drag->live = true;
            }

            if (g_search.pointer.drag->live) {
                const auto MON = pMonitor.lock();
                ScrollOverview::Dock::setForeignInsert(ScrollOverview::Dock::showAppsContains(MON, lastMousePosLocal) ? -1 : ScrollOverview::Dock::dropIndexAt(MON, lastMousePosLocal));
                damage();
                requestInputFrame();
            }
        }

        const auto beginScrollingPanAtPoint = [&](const Vector2D& point) {
            auto WORKSPACE = workspaceAtOverviewPoint(point);
            if (!WORKSPACE) {
                const auto WINDOW = windowAtOverviewPoint(point);
                if (WINDOW)
                    WORKSPACE = WINDOW->m_workspace;
            }

            if (!isWorkspaceScrolling(WORKSPACE))
                return false;

            beginScrollingPan(WORKSPACE);
            pan.lastMouseLocal = point;
            updateScrollingPan();
            return true;
        };

        // submapMouseClickPending IS a held button, exactly like drag.pendingPrimary.
        // Without it here, any pointer transit over a top layer (the bar -- and
        // ydotool absolute moves slam through the corner every step) silently
        // cleared the pending press and no submap drag could ever start.
        if (!drag.pendingPrimary && !submapMouseClickPending && !resize.pointerDown && !pan.pointerDown && !drag.activeWindow && !resize.activeWindow && isPointerOnTopLayer(pMonitor.lock())) {
            submapMouseClickPending = false;
            submapMouseClickButton  = 0;
            // Hyprland restores keyboard focus to a window whenever it is
            // null and the pointer moves. Steal it back so the bar taking
            // pointer hover cannot keep the keys.
            stealOverviewKeyboard();
            return;
        }

        info.cancelled = true;
        stealOverviewKeyboard();
        requestInputFrame();

        // D6: a drag that STARTS on a strip mini picks up that window, not
        // whatever main tile the band happens to float over. Same ordering as
        // the drop path and E6's click path.
        const auto beginDragAtStart = [this]() {
            CBox       mini;
            const auto STRIPWINDOW = windowAtStripPoint(drag.startMouseLocal, &mini);
            if (STRIPWINDOW)
                beginWindowDrag(STRIPWINDOW, &mini);
            else
                beginWindowDrag(windowAtOverviewPoint(drag.startMouseLocal));
        };

        if (submapMouseClickPending) {
            const bool DRAGTHRESHOLDREACHED = drag.startMouseLocal.distanceSq(lastMousePosLocal) > DRAGTHRESHOLDSQ;

            if (submapMouseClickButton == MAIN_BUTTON && !drag.activeWindow && !pan.pointerDown && DRAGTHRESHOLDREACHED) {
                // OVERVIEW_CPP_SPEC 13.5 X16. Do NOT clear the pending press here.
                // finishWindowDragOrClick() turns a drag that ends near its start back
                // into a click, but shouldRunDefaultClickAction() needs the pending
                // press to know the click is ours -- clearing it here meant that
                // recovery path fell through to "not our click" and the press vanished:
                // no focus, no close, overview left open. The committed-drag path
                // already calls clearSubmapMouseClickPending() after endWindowDrag().
                // Safe to keep set: the !drag.activeWindow guard above stops a re-drag,
                // and the SECONDARY branches below require a different button.

                if (!INVERT_DRAG_MODE)
                    beginDragAtStart();
                else
                    beginScrollingPanAtPoint(drag.startMouseLocal);
            }

            if (submapMouseClickButton == SECONDARY_DRAG_BUTTON && !INVERT_DRAG_MODE && !pan.pointerDown && DRAGTHRESHOLDREACHED) {
                if (beginScrollingPanAtPoint(drag.startMouseLocal)) {
                    submapMouseClickPending = false;
                    submapMouseClickButton  = 0;
                }
            }

            if (submapMouseClickButton == SECONDARY_DRAG_BUTTON && INVERT_DRAG_MODE && !drag.activeWindow && DRAGTHRESHOLDREACHED) {
                submapMouseClickPending = false;
                submapMouseClickButton  = 0;
                beginDragAtStart();
            }
        }

        if (drag.pendingPrimary) {
            if (!drag.activeWindow && !pan.pointerDown && drag.startMouseLocal.distanceSq(lastMousePosLocal) > DRAGTHRESHOLDSQ) {
                if (!INVERT_DRAG_MODE) {
                    beginDragAtStart();
                } else
                    beginScrollingPanAtPoint(drag.startMouseLocal);
            }
        }

        if (drag.activeWindow)
            updateWindowDrag();

        if (resize.pointerDown && resize.pendingWindow) {
            if (!resize.activeWindow && resize.startMouseLocal.distanceSq(lastMousePosLocal) > DRAGTHRESHOLDSQ)
                beginWindowResize();

            if (resize.activeWindow)
                updateWindowResize();
        }

        if (pan.pointerDown)
            updateScrollingPan();

}

void CScrollOverview::onTouchMove(ITouch::SMotionEvent, Event::SCallbackInfo& info) {
        if (closing || scrollOverviewAt(g_pInputManager->getMouseCoordsInternal()).get() != this)
            return;

        info.cancelled    = true;
        lastMousePosLocal = getOverviewMousePosLocal(pMonitor.lock());
        requestInputFrame();
}

void CScrollOverview::onMouseButton(IPointer::SButtonEvent event, Event::SCallbackInfo& info) {
        const auto INPUTOVERVIEW = scrollOverviewAt(g_pInputManager->getMouseCoordsInternal());
        if (closing || (g_pointerGrabOverview && g_pointerGrabOverview != this) || (!g_pointerGrabOverview && INPUTOVERVIEW.get() != this))
            return;

        const bool RELEASESPOINTERGRAB = event.state == WL_POINTER_BUTTON_STATE_RELEASED;
        auto       releasePointerGrab  = Hyprutils::Utils::CScopeGuard([this, RELEASESPOINTERGRAB] {
            if (RELEASESPOINTERGRAB && g_pointerGrabOverview == this)
                g_pointerGrabOverview = nullptr;
        });

        if (!drag.pendingPrimary && !resize.pointerDown && !pan.pointerDown && !drag.activeWindow && !resize.activeWindow && isPointerOnTopLayer(pMonitor.lock())) {
            submapMouseClickPending = false;
            submapMouseClickButton  = 0;
            return;
        }

        if (event.state == WL_POINTER_BUTTON_STATE_PRESSED)
            g_pointerGrabOverview = this;

        info.cancelled = true;
        Config::Actions::state()->m_lastMouseCode = event.button;
        Config::Actions::state()->m_lastCode      = 0;
        Config::Actions::state()->m_timeLastMs    = event.timeMs;
        // Without releasing buttons, mouse-triggered overview consumes release
        // events
        // before they reach Hyprland's input manager, leaving it stuck thinking
        // buttons are still pressed, which locks focus.
        g_pInputManager->releaseAllMouseButtons();

        const bool     LEFT_HANDED        = ScrollOverview::Config::getLeftHanded();
        const uint32_t MAIN_BUTTON        = LEFT_HANDED ? BTN_RIGHT : BTN_LEFT;
        const uint32_t RESIZE_BUTTON      = LEFT_HANDED ? BTN_LEFT : BTN_RIGHT;
        const bool     INVERT_DRAG_MODE   = ScrollOverview::Config::getDragMode() == 1;
        const uint32_t SECONDARY_DRAG_BUTTON = BTN_MIDDLE;
        const auto     clearSubmapMouseClickPending = [&]() {
            submapMouseClickPending = false;
            submapMouseClickButton  = 0;
        };
        const auto     beginSubmapMouseClickPending = [&](uint32_t button) {
            if (!usesSubmapKeybinds || !isOverviewSubmapActive())
                return false;

            submapMouseClickPending = true;
            submapMouseClickButton  = button;
            drag.startMouseLocal     = lastMousePosLocal;
            return true;
        };
        const auto     shouldRunDefaultClickAction = [&](uint32_t button) {
            if (submapMouseClickPending && submapMouseClickButton == button) {
                clearSubmapMouseClickPending();
                // Only claim the click if the submap bind actually ran. Ignoring the
                // return value swallowed the click whenever dispatch failed.
                if (dispatchSubmapMouseClick(button))
                    return false;
            }

            return !usesSubmapKeybinds || !isOverviewSubmapActive();
        };
        const auto     performClickAction = [&](uint32_t button) {
            if (!shouldRunDefaultClickAction(button))
                return;

            performPrimaryClickAction();
        };
        const auto     finishWindowDragOrClick = [&](uint32_t button, bool allowClick) {
            // OVERVIEW_CPP_SPEC 13.5 X16. This MUST exceed the drag threshold or the
            // click branch below is unreachable: a drag only starts past
            // getDragThreshold(), so an equal slop means every started drag also fails
            // the click test. A real click that wobbles 11 px was therefore swallowed --
            // the overview stayed open and nothing got focused. Measured on the repro:
            // distSq 104.4 vs threshSq 100.0 -> drag, no focus, no close.
            // ponytail: flat 3x band (~30 px). If drags start feeling sticky, the upgrade
            // is a press-duration test, not a bigger number.
            const float CLICK_MAX_DRAG_DISTANCE = ScrollOverview::Config::getDragThreshold() * 3.F * (pMonitor ? pMonitor->m_scale : 1.F);

            if (drag.activeWindow) {
                if (drag.startMouseLocal.distanceSq(lastMousePosLocal) < CLICK_MAX_DRAG_DISTANCE * CLICK_MAX_DRAG_DISTANCE) {
                    clearDragPending();

                    if (allowClick)
                        performClickAction(button);
                    else
                        clearSubmapMouseClickPending();

                    return;
                }

                endWindowDrag();
                clearSubmapMouseClickPending();
                return;
            }

            clearDragPending();

            if (allowClick)
                performClickAction(button);
            else
                clearSubmapMouseClickPending();
        };
        const auto     workspaceAtPanPoint = [&](const Vector2D& point) {
            auto WORKSPACE = workspaceAtOverviewPoint(point);
            if (WORKSPACE)
                return WORKSPACE;

            const auto WINDOW = windowAtOverviewPoint(point);
            return WINDOW ? WINDOW->m_workspace : PHLWORKSPACE{};
        };

        // The dock's menu and middle-launch, ahead of every overview gesture:
        // both float over the main area exactly as the pill does. Same ladder the
        // plain desktop runs -- it lives in DockInput. The overview honours the
        // left-handed config (its resize button is the menu), never lets a menu
        // miss through (it always draws), and keeps the dock out of a window drag.
        {
            const auto DOCKMONITOR = pMonitor.lock();
            const auto DOCKPOINT   = getOverviewMousePosLocal(DOCKMONITOR);
            const bool PRESSED     = event.state == WL_POINTER_BUTTON_STATE_PRESSED;
            const auto ROLE        = event.button == RESIZE_BUTTON ? ScrollOverview::DockInput::EButton::Menu
                                   : event.button == BTN_MIDDLE    ? ScrollOverview::DockInput::EButton::Launch
                                   : event.button == MAIN_BUTTON   ? ScrollOverview::DockInput::EButton::Main
                                                                   : ScrollOverview::DockInput::EButton::Other;

            if (ScrollOverview::DockInput::handleMenu(DOCKMONITOR, DOCKPOINT, ROLE, PRESSED, static_cast<bool>(drag.activeWindow), false).claimed) {
                lastMousePosLocal = DOCKPOINT;
                return;
            }
        }

        if (event.button == MAIN_BUTTON) {
            lastMousePosLocal = getOverviewMousePosLocal(pMonitor.lock());

            // X24 dock-side, in the overview. The pill is pinned here (§10
            // clause 6) and floats OVER the main area, so without this the tile
            // underneath takes the press and the click selects a window instead
            // of minimizing one. Both edges are eaten: returning on the press
            // alone would leave the release to performPrimaryClickAction().
            //
            // ...but NOT while a window drag is in flight. X26 drops a dragged
            // window on this same pill, and eating that release stranded the
            // drag: endWindowDrag() never ran, so its OVERDOCK branch never
            // minimized anything and the proxy stayed on screen mid-gesture.
            // A drag release belongs to the drag, whatever it is released over.
            // G12. A result drag ends wherever it is released, and it is the
            // FIRST thing the release is offered to: the dock branch below
            // would otherwise read the drop as a click on the icon under it.
            // On the pill it pins, on Show Applications it unpins, and anywhere
            // else -- including on another result -- it is rejected outright,
            // which is what "must not create a folder or reorder anything" is.
            if (event.state == WL_POINTER_BUTTON_STATE_RELEASED && g_search.pointer.drag) {
                const auto DRAG = *g_search.pointer.drag;
                g_search.pointer.drag.reset();
                ScrollOverview::Dock::setForeignInsert(-1);
                damage();
                requestInputFrame();

                if (DRAG.live && !DRAG.desktopId.empty()) {
                    const auto MON = pMonitor.lock();
                    // The button sits INSIDE the pill, so it has to be asked
                    // first or the drop index would claim it.
                    if (ScrollOverview::Dock::showAppsContains(MON, lastMousePosLocal))
                        ScrollOverview::Dock::unpin(DRAG.desktopId);
                    else if (const int INDEX = ScrollOverview::Dock::dropIndexAt(MON, lastMousePosLocal); INDEX >= 0)
                        ScrollOverview::Dock::pinAt(DRAG.desktopId, INDEX);
                    return;
                }

                // X62. The hold never completed, so this was a CLICK. Released
                // over the row it was pressed on, it activates exactly as Return
                // on that selection would; released anywhere else it is the
                // user changing their mind, which is a no-op. G12's hold stays
                // the drag gate -- a click is simply what happens without it.
                if (const auto* ROW = searchRowAt(lastMousePosLocal); ROW && ROW->index == DRAG.index && activateSearchResult(ROW->index))
                    close();

                return;
            }

            // The pill's own gestures: hover panel > pill drag/click > off-pill
            // cancel. Behind the result-drag release above -- a dropped result
            // sits inside the pill and is offered first -- and behind the
            // window-drag guard. Same dock the plain desktop drives; the ladder
            // and the reasons for its order live in DockInput.
            if (const auto CLAIM = ScrollOverview::DockInput::handlePill(pMonitor.lock(), lastMousePosLocal, ScrollOverview::DockInput::EButton::Main,
                                                         event.state == WL_POINTER_BUTTON_STATE_PRESSED, static_cast<bool>(drag.activeWindow));
                CLAIM.claimed) {
                if (CLAIM.needsFrame) {
                    damage();
                    requestInputFrame();
                }
                return;
            }

            // G12. A press over a result belongs to the panel, which floats over
            // the grid exactly as the pill does. Nothing happens yet: only the
            // hold makes it a drag, and a plain click over a row is still the
            // panel's to define.
            if (event.state == WL_POINTER_BUTTON_STATE_PRESSED) {
                if (const auto* ROW = searchRowAt(lastMousePosLocal); ROW) {
                    g_search.pointer.drag =
                        SResultDrag{.desktopId = ROW->desktopId, .icon = ROW->icon, .start = lastMousePosLocal, .startedAt = Time::steadyNow(), .index = ROW->index};
                    return;
                }
            }

            if (event.state == WL_POINTER_BUTTON_STATE_PRESSED) {
                // The close button eats the press outright -- no drag, no select.
                // User request 2026-08-02: an X on the top right of the hovered
                // tile. Drop the hover first so the button cannot be clicked
                // twice while the window takes its time going away.
                //
                // V2: with SUPER held that box is the PILL's, not the X's --
                // closeButtonHitBox hands back the pill while the modifier is
                // down, so this branch is entered over the whole capsule. WHICH
                // button then decides what happens, and a point on the capsule
                // that is on no button is NOT ours: "the pill never steals a
                // click that misses its buttons", so it falls through to the
                // tile's own click below exactly as a point next to the X did.
                if (const auto HOVERED = TileChrome::hovered(); HOVERED && tileCloseButtonBox(HOVERED).containsPoint(lastMousePosLocal)) {
                    if (WindowPill::superHeld()) {
                        const auto MONITOR = pMonitor.lock();
                        const auto BUTTON  = WindowPill::buttonAt(nullptr, tileBox(HOVERED), MONITOR ? MONITOR->m_scale : 1.F, lastMousePosLocal);
                        if (BUTTON != WindowPill::EButton::None) {
                            TileChrome::setHovered(PHLWINDOW{});
                            WindowPill::activate(MONITOR, HOVERED, BUTTON);
                            damage();
                            requestInputFrame();
                            return;
                        }
                    } else {
                        TileChrome::setHovered(PHLWINDOW{});
                        HOVERED->sendClose();
                        return;
                    }
                }

                // The other half of the one-gesture-at-a-time rule; see the
                // note at the resize press.
                if (resize.activeWindow || resize.pointerDown)
                    return;

                // Before the submap check: that branch returns, and the press
                // shrink has to show whether or not a submap is active.
                TileChrome::setPressed(windowAtOverviewPoint(lastMousePosLocal), true);

                if (beginSubmapMouseClickPending(event.button))
                    return;

                drag.pendingPrimary  = true;
                drag.startMouseLocal = lastMousePosLocal;
                return;
            }

            TileChrome::setPressed(PHLWINDOW{}, false);

            const bool WASPANNING = pan.pointerDown;
            if (pan.pointerDown)
                endScrollingPan();

            finishWindowDragOrClick(event.button, !WASPANNING);
            return;
        }

        if (event.button == SECONDARY_DRAG_BUTTON && !INVERT_DRAG_MODE) {
            lastMousePosLocal = getOverviewMousePosLocal(pMonitor.lock());

            if (event.state == WL_POINTER_BUTTON_STATE_PRESSED) {
                if (beginSubmapMouseClickPending(event.button))
                    return;

                const auto WORKSPACE = workspaceAtPanPoint(lastMousePosLocal);

                if (!isWorkspaceScrolling(WORKSPACE)) {
                    pan.pointerDown = false;
                    return;
                }

                beginScrollingPan(WORKSPACE);
                return;
            }

            if (submapMouseClickPending && submapMouseClickButton == event.button) {
                const bool WASPANNING = pan.pointerDown;
                if (pan.pointerDown)
                    endScrollingPan();

                if (!WASPANNING)
                    shouldRunDefaultClickAction(event.button);
                else
                    clearSubmapMouseClickPending();

                return;
            }

            if (pan.pointerDown)
                endScrollingPan();
            return;
        }

        if (event.button == RESIZE_BUTTON) {
            lastMousePosLocal = getOverviewMousePosLocal(pMonitor.lock());

            if (event.state == WL_POINTER_BUTTON_STATE_PRESSED) {
                // ONE POINTER GESTURE AT A TIME. Drag and resize ride DIFFERENT
                // buttons and had no exclusion between them, so both could own
                // the same window at once: onMouseMove advances each of them
                // independently, and a resize begun mid-drag retargets a window
                // whose position the drag is still writing every frame. Nobody
                // asked for that gesture -- it is what two independent
                // pointer-down flags do when neither knows about the other --
                // and it is the finding that blocked P8's gesture variant.
                //
                // The refusal goes on the PRESS, both ways, so the two states
                // can never coexist and the move handler needs no guard of its
                // own. Panning counts: it is the third thing the main button
                // can be doing.
                if (drag.activeWindow || drag.pendingPrimary || pan.pointerDown)
                    return;

                beginSubmapMouseClickPending(event.button);

                size_t resizeWorkspace = 0;
                const auto window      = windowAtOverviewCursor(&resizeWorkspace);
                if (!shouldShowOverviewWindow(window) || shouldShowPinnedFloatingOverviewWindow(window)) {
                    resize.pointerDown = false;
                    resize.pendingWindow.reset();
                    return;
                }

                const auto MONITOR = pMonitor.lock();
                if (!MONITOR)
                    return;

                const auto WORKSPACEOFFSET =
                    workspaceOverviewOffset(resizeWorkspace, activeWorkspaceIndex(), getWorkspaceRenderedPitch(MONITOR, scale->value(), layout));
                const auto WINDOWBOX = getOverviewWindowBox(window, MONITOR, scale->value(), viewOffset->value(), WORKSPACEOFFSET, layout);

                resize.pointerDown    = true;
                resize.startMouseLocal = lastMousePosLocal;
                resize.pendingWindow   = window;
                resize.workspaceIdx    = resizeWorkspace;
                resize.corner          = Layout::cornerFromBox(WINDOWBOX, lastMousePosLocal);
                return;
            }

            if (resize.activeWindow) {
                endWindowResize();
                clearSubmapMouseClickPending();
                return;
            }

            resize.pointerDown = false;
            resize.pendingWindow.reset();

            shouldRunDefaultClickAction(event.button);
            return;
        }

        if (event.button != SECONDARY_DRAG_BUTTON || !INVERT_DRAG_MODE)
            return;

        lastMousePosLocal = getOverviewMousePosLocal(pMonitor.lock());

        if (event.state == WL_POINTER_BUTTON_STATE_PRESSED) {
            // One gesture at a time; see the note at the resize press.
            if (resize.activeWindow || resize.pointerDown)
                return;

            if (beginSubmapMouseClickPending(event.button))
                return;

            drag.startMouseLocal = lastMousePosLocal;
            beginWindowDrag(windowAtOverviewCursor());
            return;
        }

        finishWindowDragOrClick(event.button, true);
}

void CScrollOverview::onCursorSelect(ITouch::SDownEvent, Event::SCallbackInfo& info) {
        if (closing || scrollOverviewAt(g_pInputManager->getMouseCoordsInternal()).get() != this)
            return;

        if (isPointerOnTopLayer(pMonitor.lock()))
            return;

        info.cancelled = true;

        selectWindowAtOverviewCursor();

        close();
}

void CScrollOverview::onMouseAxis(IPointer::SAxisEvent e, Event::SCallbackInfo& info) {
        if (closing || scrollOverviewAt(g_pInputManager->getMouseCoordsInternal()).get() != this)
            return;

        info.cancelled = true;

        const auto ACTION = e.axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL ? ScrollOverview::Config::getHorizontalScrollAction(layout) :
                                                                          ScrollOverview::Config::getVerticalScrollAction(layout);

        // mouse wheel: discrete stepping, throttled by scroll_event_delay so one notch is one step
        if (e.source == WL_POINTER_AXIS_SOURCE_WHEEL) {
            if (e.delta == 0.0)
                return;
            trackpadScrollAccum        = 0.0;
            trackpadWorkspaceFollowing = false;
            trackpadTapeFollowing      = false;
            if (!scrollStepAllowed(e.timeMs))
                return;

            if (ACTION == ScrollOverview::Config::EScrollAction::WORKSPACE)
                moveViewportWorkspace(e.delta > 0);
            else
                moveScrollingColumnSelection(e.delta > 0);

            return;
        }

        if (images.empty() || viewportCurrentWorkspace >= images.size())
            return;

        // scroll workspace or layout with 1:1 animation (snaping on release)
        if (ACTION == ScrollOverview::Config::EScrollAction::WORKSPACE)
            trackpadSwipeWorkspace(e.delta);
        else
            trackpadSwipeLayout(images[viewportCurrentWorkspace]->pWorkspace, e.delta);
}

void CScrollOverview::onWindowOpen(PHLWINDOW window) {
        if (closing)
            return;

        if (isScreensaverWindow(window)) {
            dropScrollOverviewsForMonitor(pMonitor.lock());
            return;
        }

        rebuildPending = true;
        damage();
}

void CScrollOverview::onWindowClose(PHLWINDOW) {
        if (closing)
            return;

        rebuildPending = true;
        damage();
}

void CScrollOverview::onWindowMove(PHLWINDOW, PHLWORKSPACE) {
        if (closing)
            return;

        rebuildPending = true;
        damage();
}

void CScrollOverview::onWindowActive(PHLWINDOW window, Desktop::eFocusReason) {
        if (closing)
            return;

        if (isScreensaverWindow(window)) {
            dropScrollOverviewsForMonitor(pMonitor.lock());
            return;
        }

        g_pInputManager->unconstrainMouse();

        const auto overviewWindow = getOverviewWindowToShow(window);
        const auto fullscreenWindow = overviewWindow && overviewWindow->m_workspace ? getOverviewWindowToShow(Fullscreen::controller()->getFullscreenWindow(overviewWindow->m_workspace)) : PHLWINDOW{};

        if (shouldShowOverviewWindow(fullscreenWindow) && fullscreenWindow->m_workspace == overviewWindow->m_workspace && overviewWindow->m_isFloating)
            emitFullscreenVisibilityState(fullscreenWindow, true);
        else
            emitFullscreenVisibilityState(overviewWindow, true);

        if (shouldShowOverviewWindow(overviewWindow) && overviewWindow->m_monitor == pMonitor) {
            rebuildPending = true;
            closeOnWindow  = overviewWindow;
            rememberSelection(overviewWindow);

            for (size_t i = 0; i < images.size(); ++i) {
                if (images[i]->pWorkspace == overviewWindow->m_workspace) {
                    viewportCurrentWorkspace = i;
                    break;
                }
            }
        }

        damage();
        stealOverviewKeyboard();
}

void CScrollOverview::onWindowFullscreen(PHLWINDOW window) {
        if (closing || emittingFullscreenVisibilityState)
            return;

        if (isScreensaverWindow(window)) {
            dropScrollOverviewsForMonitor(pMonitor.lock());
            return;
        }

        window = getOverviewWindowToShow(window);
        if (!window || window->m_monitor != pMonitor || !Fullscreen::controller()->isFullscreen(window))
            return;

        emitFullscreenVisibilityState(window, true);
}

void CScrollOverview::onWorkspaceLifecycle(PHLWORKSPACEREF) {
        if (closing)
            return;

        rebuildPending = true;
        damage();
}

void CScrollOverview::onKeyboardKey(IKeyboard::SKeyEvent event, Event::SCallbackInfo& info) {
        if (closing || event.state != WL_KEYBOARD_KEY_STATE_PRESSED || activeScrollOverview().get() != this)
            return;

        if (isTopLayerFocused(pMonitor.lock()))
            return;

        const auto KEYSYM = getOverviewKeysym(event);
        const auto MODS   = g_pInputManager->getModsFromAllKBs() & ~(HL_MODIFIER_CAPS | HL_MODIFIER_MOD2);

        if ((KEYSYM == XKB_KEY_Return || KEYSYM == XKB_KEY_KP_Enter || KEYSYM == XKB_KEY_Left || KEYSYM == XKB_KEY_KP_Left || KEYSYM == XKB_KEY_Right ||
             KEYSYM == XKB_KEY_KP_Right || KEYSYM == XKB_KEY_Up || KEYSYM == XKB_KEY_KP_Up || KEYSYM == XKB_KEY_Down || KEYSYM == XKB_KEY_KP_Down) &&
            MODS != 0)
            return;

        switch (KEYSYM) {
            case XKB_KEY_Left:
            case XKB_KEY_KP_Left:
                moveSelection("left");
                break;
            case XKB_KEY_Right:
            case XKB_KEY_KP_Right:
                moveSelection("right");
                break;
            case XKB_KEY_Up:
            case XKB_KEY_KP_Up:
                moveSelection("up");
                break;
            case XKB_KEY_Down:
            case XKB_KEY_KP_Down:
                if (g_search.nav.zone != EOverviewNavZone::Result) {
                    if (!g_search.layout.hits.empty()) {
                        g_search.nav.zone              = EOverviewNavZone::Result;
                        g_search.nav.selection         = g_search.layout.hits[0].index;
                        g_search.nav.explicitSelection = true;
                        damage();
                        break;
                    }
                }
                moveSelection("down");
                break;
            case XKB_KEY_Tab:
            case XKB_KEY_ISO_Left_Tab: {
                // Tab toggles keyboard focus between Search Results and open window Tiles
                if (g_search.nav.zone == EOverviewNavZone::Result) {
                    g_search.nav.zone         = EOverviewNavZone::Tile;
                    g_search.nav.keyboardTile = true;
                    if (const auto CUR = closeOnWindow.lock(); !CUR) {
                        for (const auto& wimg : images) {
                            if (!wimg || !wimg->pWorkspace)
                                continue;
                            const auto WINS = overviewMainGridWindows(wimg->pWorkspace, pMonitor.lock());
                            if (!WINS.empty()) {
                                closeOnWindow = WINS.front();
                                break;
                            }
                        }
                    }
                } else {
                    const auto RESULTS = overviewSearchRows();
                    if (!RESULTS.empty()) {
                        g_search.nav.zone              = EOverviewNavZone::Result;
                        g_search.nav.explicitSelection = true;
                        if (g_search.nav.selection < 0 || g_search.nav.selection >= sc<int>(RESULTS.size()))
                            g_search.nav.selection = 0;
                    }
                }

                damage();
                break;
            }
            case XKB_KEY_Return:
            case XKB_KEY_KP_Enter: {
                const auto RESULTS = overviewSearchRows();

                // X61. Return means "activate what is selected", and WHAT that is
                // depends on the zone. A desktop needs nothing done to it:
                // arrowing onto a strip cell already switched the viewport, so
                // closing simply lands there. A tile needs nothing either --
                // closeOnWindow is exactly what close() focuses. Only a result
                // has to be run, which is why this is the only branch here.
                //
                // The fallback is unchanged: with no explicit selection and no
                // window matching the query, Return takes the top result. It is
                // suppressed in the Desktop zone, where the user has said where
                // they want to go and launching an app instead would be a
                // surprise.
                int launchIdx = -1;
                if (g_search.nav.zone == EOverviewNavZone::Result && g_search.nav.selection >= 0 && g_search.nav.selection < sc<int>(RESULTS.size()))
                    launchIdx = g_search.nav.selection;
                else if (g_search.nav.zone != EOverviewNavZone::Desktop && !RESULTS.empty() && (matchingWindowCount() == 0 || g_search.nav.zone == EOverviewNavZone::Search))
                    launchIdx = 0;

                activateSearchResult(launchIdx);
                close();
                break;
            }
            default: return;
        }

        info.cancelled = true;
}


