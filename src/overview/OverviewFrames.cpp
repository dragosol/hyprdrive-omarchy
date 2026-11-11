// COverview: frame diagnostics, frame pacing, live-preview scheduling, surface frame/damage policy.
// Cut from Overview.cpp by pure code motion.
#include "OverviewParts.hpp"
#include "Ripple.hpp"

using namespace OverviewDetail;


namespace OverviewDetail {

void noteFrame(MONITORID id) {
    auto& C = g_frameCadence[id];
    if (C.last.time_since_epoch().count() != 0) {
        const float GAP = std::chrono::duration<float, std::milli>(Time::steadyNow() - C.last).count();
        if (GAP < 2000.F) { // a gap across a close is not a frame time
            C.gaps[C.next] = GAP;
            C.next         = (C.next + 1) % C.gaps.size();
            C.count        = std::min(C.count + 1, C.gaps.size());
        }
    }
    C.last = Time::steadyNow();
}
}

namespace {

std::string cadenceReport(MONITORID id) {
    const auto IT = g_frameCadence.find(id);
    if (IT == g_frameCadence.end() || IT->second.count < 3)
        return "no frames recorded";

    std::vector<float> gaps(IT->second.gaps.begin(), IT->second.gaps.begin() + sc<long>(IT->second.count));
    std::ranges::sort(gaps);
    const float MED  = gaps[gaps.size() / 2];
    const float P95  = gaps[sc<size_t>(sc<float>(gaps.size()) * 0.95F)];
    const float WORST = gaps.back();
    size_t      late = 0;
    for (const float G : gaps) {
        if (G > MED * 1.5F)
            ++late;
    }

    return std::format("{} frames: median {:.1f}ms ({:.0f} fps), p95 {:.1f}ms, worst {:.1f}ms, {} late", gaps.size(), MED, MED > 0.F ? 1000.F / MED : 0.F, P95, WORST, late);
}
}

std::string COverview::frameCadence(MONITORID monitor) {
    return cadenceReport(monitor);
}

static inline void noteFrameGate(const PHLWINDOW& window, const char* reason) {
    if (!Prof::enabled() || !window)
        return;

    g_frameGateReason[window->m_class] = reason;
}

// The mirror of noteFrameGate for the DAMAGE side. A client can be awake, be
// handed frames and still show a still, if its commits are refused before they
// reach the repaint -- which is indistinguishable from outside.
static inline void noteDamageGate(const PHLWINDOW& window, const char* reason) {
    if (!Prof::enabled() || !window)
        return;

    g_damageGateReason[window->m_class] = reason;
}

std::string COverview::frameDebugLine() const {
    const auto MONITOR = pMonitor.lock();
    std::string out    = MONITOR ? MONITOR->m_name : std::string("?");

    const auto var = [&](const char* name, const auto& v) {
        if (v && v->isBeingAnimated())
            out += std::string(" ") + name;
    };

    out += closing ? " [closing]" : " [open]";
    var("scale", scale);
    var("viewOffset", viewOffset);
    var("wsInsert", workspaceInsertProgress);
    var("wsInsertFade", workspaceInsertFadeProgress);
    var("reflowPacer", reflowPacer);
    var("resultsBand", resultsBand);
    var("resultsWidth", resultsWidth);
    var("searchDock", searchDock);
    var("pillNudge", pillNudge);
    var("gridNudge", gridNudge);
    var("scratchpadReveal", scratchpadReveal);
    var("altStripsReveal", altStripsReveal);

    if (TileChrome::animatingOn(MONITOR))
        out += " tileChrome";

    // The adaptive preview pulse, so "is that tile live or is it a still?" is a
    // question `hyprdrive:debug frames` answers rather than one you settle by
    // staring at a screenshot. streaming/polling, and the window still inside
    // its post-move grace.
    if (!previewPulses.empty()) {
        const auto NOW = Time::steadyNow();
        size_t     streaming = 0, grace = 0;
        for (const auto& [WINDOW, STATE] : previewPulses) {
            if (NOW < STATE.graceUntil)
                ++grace;
            else if (STATE.streaming && NOW - STATE.lastChange <= OVERVIEW_PREVIEW_QUIET_TIMEOUT)
                ++streaming;
        }
        out += std::format(" previews[{}stream/{}grace/{}poll @{}ms", streaming, grace, previewPulses.size() - streaming - grace,
                           previewPulseInterval(NOW).count());

        // Named, because "a video is not updating" needs to say WHICH window is
        // in which state before it can say whose fault it is.
        for (const auto& [WINDOW, STATE] : previewPulses) {
            const auto W = STATE.ref.lock();
            if (!W)
                continue;

            const char* MODE = NOW < STATE.graceUntil                                                      ? "grace" :
                (STATE.streaming && NOW - STATE.lastChange <= OVERVIEW_PREVIEW_QUIET_TIMEOUT)              ? "STREAM" :
                                                                                                             "poll";
            const auto SINCE = STATE.lastChange.time_since_epoch().count() == 0 ?
                -1 :
                std::chrono::duration_cast<std::chrono::milliseconds>(NOW - STATE.lastChange).count();
            // Class alone is ambiguous -- several terminals, one of them the one
            // reading this output -- so carry a slice of the title too.
            auto title = W->m_title.substr(0, 14);
            // Truncating by BYTES can cut a multi-byte character in half; drop
            // any incomplete trailing sequence rather than emit a broken glyph.
            while (!title.empty() && (sc<unsigned char>(title.back()) & 0xC0) == 0x80)
                title.pop_back();
            if (!title.empty() && (sc<unsigned char>(title.back()) & 0xC0) == 0xC0)
                title.pop_back();
            std::ranges::replace(title, ' ', '_');
            out += std::format(" {}/{}={}({}ms{}{})", W->m_class.empty() ? std::string("?") : W->m_class, title, MODE, SINCE,
                               W->m_suspended ? " SUSP" : "", W->isHidden() ? " HIDDEN" : "");
        }

        out += "]";
    }

    if (Prof::enabled() && !g_frameGateReason.empty()) {
        out += " gate[";
        for (const auto& [CLASS, REASON] : g_frameGateReason)
            out += std::format("{}={} ", CLASS, REASON);
        out += "] forced[";
        for (const auto& [CLASS, N] : g_unsuspendCount)
            out += std::format("{}={}/{} ", CLASS, N.first, N.second);
        out += "] sent[";
        for (const auto& [CLASS, N] : g_frameSent)
            out += std::format("{}={}/{} ", CLASS, N.first, N.second);
        out += "] damage[";
        for (const auto& [CLASS, REASON] : g_damageGateReason)
            out += std::format("{}={} ", CLASS, REASON);
        out += "]";
    }
    if (realtimePreviewFrameQueued)
        out += " previewQueued";
    if (previewRepaintPending)
        out += " previewPending";
    if (needsIdleRepaint())
        out += " idleRepaint";
    if (drag.activeWindow)
        out += " drag";
    if (resize.activeWindow)
        out += " resize";
    if (pan.pointerDown)
        out += " pan";
    if (SearchLetters::g_searchLetters.busy(Time::steadyNow()))
        out += " searchLetters";
    if (const auto DOCK = Hyprdrive::Dock::frameReason(MONITOR); DOCK)
        out += std::string(" dock:") + DOCK;

    return out;
}

void COverview::damage() {
    blockDamageReporting = true;
    g_pHyprRenderer->damageMonitor(pMonitor.lock());
    blockDamageReporting = false;
}

void COverview::requestInputFrame() {
    if (closing)
        return;

    const auto MONITOR = pMonitor.lock();
    if (!MONITOR)
        return;

    inputFramePending = true;
    MONITOR->scheduleFrame(Aquamarine::IOutput::AQ_SCHEDULE_CURSOR_MOVE);
}

void COverview::markBlurDirty() {
    frost.overviewDirty = true;
}

void COverview::markBackdropBlurDirty() {
    frost.backdropDirty = true;
}

bool COverview::shouldAllowRealtimePreviewFrame() const {
    if (lastRealtimePreviewFrame.time_since_epoch().count() == 0)
        return true;

    return Time::steadyNow() - lastRealtimePreviewFrame >= OVERVIEW_WINDOW_FRAME_INTERVAL;
}

bool COverview::shouldAllowRealtimePreviewSchedule() {
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
        resultsWidth->isBeingAnimated() || searchDock->isBeingAnimated() || pillNudge->isBeingAnimated() || gridNudge->isBeingAnimated() || stripTipAnimating() || stripCleanupAnimating())
        return true;

    // Beside them for the same reason they are here: typing is the one moment
    // the user is watching a 150ms pop per keystroke, and the 33ms preview
    // throttle below would show it in five frames. See needsIdleRepaint.
    if (SearchLetters::g_searchLetters.busy(Time::steadyNow()))
        return true;

    if (const auto MONITOR = pMonitor.lock(); MONITOR && g_search.fieldOn(MONITOR->m_id).busy())
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

void COverview::schedulePreviewFrameAfter(std::chrono::milliseconds delay) {
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

// ---------------------------------------------------------------------------
// THE ADAPTIVE PREVIEW PULSE. See OVERVIEW_PREVIEW_POLL_INTERVAL for why the
// change detector is the client's own damage and not a comparison of frames.

bool COverview::previewWantsFrame(const PHLWINDOW& window, const Time::steady_tp& now) {
    if (!window)
        return true;

    auto& STATE = previewPulses[window.get()];
    STATE.ref   = window;

    // A MOVE IS DETECTED HERE rather than hooked. The gate already runs for every
    // window it governs, so the workspace it was last seen on is a free place to
    // notice that it is now somewhere else -- no signal to subscribe to, and it
    // cannot miss a move that happened while the overview was closed.
    const auto WORKSPACE = window->m_workspace ? window->m_workspace->m_id : WORKSPACE_INVALID;
    if (STATE.lastSeenWorkspace != WORKSPACE) {
        STATE.lastSeenWorkspace = WORKSPACE;
        STATE.graceUntil        = now + OVERVIEW_PREVIEW_MOVE_GRACE;
    }

    if (now < STATE.graceUntil)
        return true;

    if (STATE.streaming) {
        if (now - STATE.lastChange <= OVERVIEW_PREVIEW_QUIET_TIMEOUT)
            return true;

        // Quiet long enough to stop streaming. The demotion counts as this
        // window's poll: it has just had frames, so it owes nothing for another
        // interval, and starting the clock here stops a window that stopped
        // moving from being polled again on the very next pulse.
        STATE.streaming = false;
        STATE.lastPoll  = now;
        return false;
    }

    if (STATE.lastPoll.time_since_epoch().count() != 0 && now - STATE.lastPoll < OVERVIEW_PREVIEW_POLL_INTERVAL)
        return false;

    // The poll itself: one callback. If the client paints anything new, its
    // commit lands in notePreviewDamage and the tile is streaming before the
    // next pulse -- so a window that started moving costs one poll of latency
    // and not two seconds of it.
    STATE.lastPoll = now;
    return true;
}

void COverview::notePreviewDamage(const PHLWINDOW& window, const Time::steady_tp& now) {
    if (!window)
        return;

    auto& STATE      = previewPulses[window.get()];
    STATE.ref        = window;
    STATE.lastChange = now;
    STATE.streaming  = true;
}

bool COverview::previewIsStreaming(const PHLWINDOW& window, const Time::steady_tp& now) const {
    if (!window)
        return false;

    const auto IT = previewPulses.find(window.get());
    if (IT == previewPulses.end())
        return false;

    const auto& STATE = IT->second;
    if (now < STATE.graceUntil)
        return true;

    return STATE.streaming && now - STATE.lastChange <= OVERVIEW_PREVIEW_QUIET_TIMEOUT;
}

bool COverview::anyPreviewStreaming(const Time::steady_tp& now) const {
    return std::ranges::any_of(previewPulses, [&](const auto& entry) {
        const auto& STATE = entry.second;
        if (now < STATE.graceUntil)
            return true;
        return STATE.streaming && now - STATE.lastChange <= OVERVIEW_PREVIEW_QUIET_TIMEOUT;
    });
}

std::chrono::milliseconds COverview::previewPulseInterval(const Time::steady_tp& now) const {
    // Anything moving on screen: the pulse is the delivery mechanism for its
    // frames, so it keeps up with the MONITOR, not with misc:render_unfocused_fps.
    // That setting is a sensible floor for a desktop nobody is looking at; the
    // overview is the one moment every desktop IS being looked at.
    if (anyPreviewStreaming(now) || wantsWakes()) {
        const auto MONITOR = pMonitor.lock();
        const int  HZ      = MONITOR && MONITOR->m_refreshRate > 1 ? sc<int>(MONITOR->m_refreshRate) : 60;
        return std::min(getOverviewIdleFrameInterval(), std::chrono::milliseconds(std::max(1, 1000 / HZ)));
    }

    // Nothing is moving. The pulse owes two things: the next window due a poll,
    // and the once-a-second floor that keeps the POLLED bits of chrome (the
    // dock's favourites file, the trash icon's name) arriving. Sleeping past
    // the floor would slow those down, so it is the ceiling here -- which still
    // takes an idle overview from fifteen wakeups a second to one.
    auto soonest = OVERVIEW_IDLE_REPAINT_FLOOR;
    for (const auto& [WINDOW, STATE] : previewPulses) {
        if (STATE.lastPoll.time_since_epoch().count() == 0)
            return getOverviewIdleFrameInterval();

        const auto DUE = std::chrono::duration_cast<std::chrono::milliseconds>(STATE.lastPoll + OVERVIEW_PREVIEW_POLL_INTERVAL - now);
        soonest        = std::min(soonest, std::max(std::chrono::milliseconds(1), DUE));
    }

    return std::max(soonest, getOverviewIdleFrameInterval());
}

// THE EVENT-LOOP HALF. Runs from the preview pulse, between frames.
//
// Two jobs, both deliberately off the render path: tell a client it is on this
// output, and ask it for its next frame. Held entirely while the overview is
// still animating -- the pass draws every window from what it last committed
// while that runs, exactly as Mutter's overview does, so nothing looks stale --
// and then spread OVERVIEW_WAKES_PER_FRAME at a time so a desktop full of
// windows ramps over a few frames instead of stalling one.
void COverview::wakeDrawnWindows(const Time::steady_tp& now) {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR || closing)
        return;

    if (scale->isBeingAnimated() || viewOffset->isBeingAnimated())
        return;

    size_t woken = 0;
    for (const auto& WINDOWREF : drawnWindows) {
        const auto WINDOW = WINDOWREF.lock();
        if (!WINDOW)
            continue;

        const bool ALREADYAWAKE = previewPulses.contains(WINDOW.get());
        if (!ALREADYAWAKE && woken >= OVERVIEW_WAKES_PER_FRAME)
            continue;

        if (!ALREADYAWAKE)
            ++woken;

        forced.enterWindowSurfaces(WINDOW, MONITOR);

        if (WINDOW->wlSurface() && WINDOW->wlSurface()->resource()) {
            askingForDrawnWindow = true;
            surfaceTreePresent(WINDOW->wlSurface()->resource(), MONITOR, now);
            askingForDrawnWindow = false;
        }
    }
}

// Is there anything still to wake? Keeps the pulse at a frame interval through
// the ramp instead of letting it stretch to the idle floor mid-way.
bool COverview::wantsWakes() const {
    return std::ranges::any_of(drawnWindows, [&](const auto& ref) {
        const auto WINDOW = ref.lock();
        return WINDOW && !previewPulses.contains(WINDOW.get());
    });
}

void COverview::prunePreviewPulses() {
    std::erase_if(previewPulses, [](const auto& entry) { return !entry.second.ref.lock(); });
}

void COverview::scheduleMinimumPreviewFrame() {
    schedulePreviewFrameAfter(previewPulseInterval(Time::steadyNow()));
}

void COverview::scheduleRealtimePreviewFrame() {
    // EVERY caller of this is a repaint that was refused. hkScheduleFrame sends
    // the dock's own pumpFrame down here, and so does a client commit on a tile
    // -- so this flag is how the idle pulse below knows that the frame it is
    // about to fire is standing in for a real damage rather than being the
    // bare keep-alive it usually is.
    previewRepaintPending = true;

    const auto NOW     = Time::steadyNow();
    const auto ELAPSED = lastRealtimePreviewFrame.time_since_epoch().count() == 0 ? OVERVIEW_WINDOW_FRAME_INTERVAL :
                                                                                   std::chrono::duration_cast<std::chrono::milliseconds>(NOW - lastRealtimePreviewFrame);
    const auto DELAY   = OVERVIEW_WINDOW_FRAME_INTERVAL - std::min(ELAPSED, OVERVIEW_WINDOW_FRAME_INTERVAL);
    schedulePreviewFrameAfter(DELAY);
}

// THE IDLE PULSE, and what it is actually FOR: keeping the clients whose tiles
// are on screen drawing, so a preview is a live window and not a still. It used
// to do that by damaging the output at misc:render_unfocused_fps -- a full
// repaint of every overview on every monitor, 15 times a second, forever, for a
// screen on which nothing was moving.
//
// A repaint is only the DELIVERY MECHANISM for the frame callbacks the render
// tail sends. When there is nothing queued and nothing animating, they can be
// sent straight from here instead: the tiles that then commit are routed back
// to a real repaint by shouldHandleSurfaceDamage -> scheduleRealtimePreviewFrame,
// which is the same path a client commit has always taken. Nothing in
// sendOverviewFrameCallbacks touches the render pass; it reads the overview's
// own geometry and calls presentFeedback, both of which are as valid here as
// they are at the end of render().
int COverview::realtimePreviewTimerCallback(void* data) {
    const auto OVERVIEW = sc<COverview*>(data);
    if (!OVERVIEW)
        return 0;

    OVERVIEW->realtimePreviewTimerArmed  = false;
    OVERVIEW->realtimePreviewTimerDue    = {};
    const bool WASQUEUED                 = OVERVIEW->realtimePreviewFrameQueued;
    OVERVIEW->realtimePreviewFrameQueued = false;

    const bool WANTSREPAINT              = OVERVIEW->previewRepaintPending;
    OVERVIEW->previewRepaintPending      = false;

    const auto NOW = Time::steadyNow();

    // THE FLOOR, and it is a deliberate one. A handful of things the overview
    // draws are POLLED rather than announced -- the dock's favourites file, the
    // trash icon's full/empty name -- and they used to land on the next of
    // these pulses because every pulse was a full repaint. One repaint a second
    // keeps them landing; it is the 15/s that was never earned.
    const bool FLOOR = OVERVIEW->lastIdleRepaint.time_since_epoch().count() == 0 || NOW - OVERVIEW->lastIdleRepaint >= OVERVIEW_IDLE_REPAINT_FLOOR;

    if (WASQUEUED || WANTSREPAINT || FLOOR || OVERVIEW->needsIdleRepaint()) {
        OVERVIEW->lastIdleRepaint = NOW;
        OVERVIEW->damage();
    } else
        OVERVIEW->sendOverviewFrameCallbacks(NOW);

    OVERVIEW->wakeDrawnWindows(NOW);
    OVERVIEW->prunePreviewPulses();
    OVERVIEW->scheduleMinimumPreviewFrame();
    return 0;
}

// Is anything on this overview actually moving? Every animated value the render
// pass reads, plus the two wall-clock eases that are not animated values at all
// (the grid reflow, which onPreRender re-arms through reflowPacer, and the tile
// chrome). If none of them is running, the pixels this frame would produce are
// the pixels already on the screen.
bool COverview::needsIdleRepaint() const {
    if (closing || drag.activeWindow || resize.activeWindow || pan.pointerDown)
        return true;

    if (inputFramePending || selectedWorkspaceFramePending || rebuildPending)
        return true;

    if (scale->isBeingAnimated() || viewOffset->isBeingAnimated() || reflowPacer->isBeingAnimated() || resultsBand->isBeingAnimated() || resultsWidth->isBeingAnimated() ||
        searchDock->isBeingAnimated() || pillNudge->isBeingAnimated() || gridNudge->isBeingAnimated() || stripTipAnimating() || stripCleanupAnimating() || workspaceInsertProgress->isBeingAnimated() || workspaceInsertFadeProgress->isBeingAnimated())
        return true;

    if (scratchpadReveal->isBeingAnimated() || altStripsReveal->isBeingAnimated())
        return true;

    // Per OUTPUT, the same way onPreRender asks: a hover easing on the other
    // monitor is not a reason for this one to repaint.
    if (TileChrome::animatingOn(pMonitor.lock()))
        return true;

    if (searchHoverEasing() || pillPlaying())
        return true;

    if (Hyprdrive::BorderFade::animatingOn(pMonitor.lock()))
        return true;

    // THE TWO HAND-TIMED ANIMATIONS OF THE SEARCH FIELD (user, 2026-09-19: the
    // letters "freeze mid-frame ... once I get past a certain number").
    //
    // The letter bounce and the field's width/scroll springs are timestamped
    // rather than PHLANIMVARs, so not one of the tests above can see them, and
    // this gate is what the idle pulse asks before it repaints at all. Under the
    // width cap that was invisible: every keystroke re-aimed resultsWidth and
    // searchDock, whose animation kept the frames coming for the letters as a
    // side effect. Once the query passes the cap the field stops resizing, those
    // vars settle, and the only things still moving were these two -- so the
    // pulse stopped repainting and a letter stuck wherever its pop had reached
    // until the next key.
    if (SearchLetters::g_searchLetters.busy(Time::steadyNow()))
        return true;

    if (const auto MONITOR = pMonitor.lock(); MONITOR && g_search.fieldOn(MONITOR->m_id).busy())
        return true;

    if (WindowPill::animating() && isOverviewPointerOnMonitor(pMonitor.lock()))
        return true;

    // The ones onPreRender paces that no test above can see (B1): a closing
    // tile, the strip's drag glide, the ripple, and the easter egg's video
    // (pumped by the letter tick, which the idle pulse must not undercut).
    if (!closingTiles.empty() || deskGlideMoving || Hyprdrive::Ripple::activeOn(pMonitor.lock()) || Hyprdrive::KeyboardCat::busy())
        return true;

    return false;
}

bool COverview::hasRunningWorkspaceAnimation() const {
    return viewOffset->isBeingAnimated() || workspaceInsertProgress->isBeingAnimated() || workspaceInsertFadeProgress->isBeingAnimated();
}

bool COverview::shouldSuppressRenderDamage() const {
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
        {
            const auto fullscreenWindow = overviewFullscreenPathWindow(workspace);
            if (fullscreenWindow) {
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

namespace OverviewDetail {

// The Quick Look (Sushi) preview the overview opened, if it is up on this
// monitor. It is a real toplevel drawn at its native box over the overview, not
// a tile, so every tile gate in this file says no to it -- and two of those
// gates were routes it needed: its frame callbacks and its damage. The places
// that must treat it as live ask this instead of repeating the lookup.
// THE SCRATCHPAD OVER THE OVERVIEW (user, 2026-09-30: "when triggering the
// scratchpad from the overview, show it appearing over the overview"). A
// special workspace shown on this monitor is drawn natively on top of the
// overview -- its own slide, fade and dim -- the way the desktop draws it over
// the windows. The minimize stash is special too, and is never shown.
//
// AND WHILE IT GOES (user, 2026-09-30: "toggling the overview while the
// scratchpad is open should show it closing"). Hyprland clears
// m_activeSpecialWorkspace the moment a special starts hiding and animates it
// out afterwards, so the last one shown on each monitor is remembered and drawn
// for as long as its fade or slide is still running.
PHLWORKSPACE overviewShownSpecialOn(const PHLMONITOR& monitor) {
    static std::unordered_map<MONITORID, PHLWORKSPACEREF> s_last;
    if (!monitor)
        return nullptr;
    const auto SPECIAL = monitor->m_activeSpecialWorkspace;
    if (SPECIAL && SPECIAL->m_name != "special:minimized") {
        s_last[monitor->m_id] = SPECIAL;
        return SPECIAL;
    }
    const auto IT = s_last.find(monitor->m_id);
    if (IT == s_last.end())
        return nullptr;
    const auto LEAVING = IT->second.lock();
    if (!LEAVING || LEAVING->m_monitor != monitor || LEAVING->m_alpha->value() < 0.01F ||
        !(LEAVING->m_alpha->isBeingAnimated() || LEAVING->m_renderOffset->isBeingAnimated())) {
        s_last.erase(IT);
        return nullptr;
    }
    return LEAVING;
}

bool onShownSpecial(const PHLWINDOW& window, const PHLMONITOR& monitor) {
    const auto SPECIAL = overviewShownSpecialOn(monitor);
    return SPECIAL && window && window->m_workspace == SPECIAL && Desktop::View::validMapped(window) && !window->isHidden();
}

PHLWINDOW overviewSushiWindowOn(const PHLMONITOR& monitor) {
    if (!monitor || !SearchBackend::isSushiActive())
        return nullptr;

    for (const auto& w : Desktop::windowState()->windows()) {
        if (Desktop::View::validMapped(w) && SearchBackend::isOverviewSushiWindow(w->m_class, w->getPID()))
            return w->m_monitor == monitor ? w : nullptr;
    }

    return nullptr;
}
}

void COverview::sendOverviewFrameCallbacks(const Time::steady_tp& now) {
    // The repaint this stood in for (or the one that just happened) is done
    // with. Anything that wants another one will set the flag again below, or
    // through hkScheduleFrame.
    previewRepaintPending = false;

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

        // A SUSPENDED CLIENT CANNOT ASK FOR THE FRAME THAT WOULD WAKE IT (user,
        // 2026-09-19: a fullscreen YouTube video in Zen on another desktop
        // "shows static when in fact it's playing").
        //
        // Hyprland marks windows whose workspace is not active as suspended over
        // xdg-toplevel, and Firefox honours that literally: it stops painting AND
        // stops decoding. The window then never calls CWLSurfaceResource::frame,
        // so it never reaches shouldAllowSurfaceFrame -- measured, its class was
        // absent from the gate trace entirely while every terminal was in it. No
        // amount of adaptive polling can fix that, because the poll is a frame
        // callback and the client is not listening for one.
        //
        // A tile on screen is a claim that this window is being looked at, which
        // is the same claim the dock makes for a preview (Dock.cpp), and it is
        // undone by Hyprland itself as soon as the overview stops drawing the
        // tile. The frame RATE is still the adaptive gate's decision downstream;
        // this only puts the client back in a state where it can be asked.
        if (window->m_suspended)
            window->setSuspended(false);

        if (Prof::enabled())
            ++g_frameSent[window->m_class].first;

        if (!realtime && !ISDRAGGED && !CANFRAMETHROTTLEDWINDOWS) {
            scheduleRealtimePreviewFrame();
            return;
        }

        if (Prof::enabled())
            ++g_frameSent[window->m_class].second;

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

    // The Sushi preview is the window in front of everything, so it gets a frame
    // every overview frame, unthrottled, exactly as it would on the desktop. It
    // used to get none: it is not a tile, so frameWindow() refused it, and
    // hkSendFrameEventsToWorkspace had already taken Hyprland's own dispatch
    // away. GTK paces its commits on frame callbacks, so the preview kept showing
    // the buffer it mapped with -- centred by Hyprland inside a window that popin
    // was already growing, the border visibly outside the app -- until a late
    // commit snapped it into place. Measured 2026-09-10 in a screen recording:
    // border tracking popin frame by frame, surface inset ~70px, then a jump.
    if (const auto SUSHI = overviewSushiWindowOn(MONITOR))
        surfaceTreePresent(SUSHI->wlSurface() ? SUSHI->wlSurface()->resource() : nullptr, MONITOR, now);
    if (overviewShownSpecialOn(MONITOR)) {
        for (const auto& W : Desktop::windowState()->windows())
            if (onShownSpecial(W, MONITOR))
                surfaceTreePresent(W->wlSurface() ? W->wlSurface()->resource() : nullptr, MONITOR, now);
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
        {
            const auto fullscreenWindow = overviewFullscreenPathWindow(workspace);
            if (fullscreenWindow) {
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

    // An OVERLAY client asking for frames is animating over the overview (a
    // notification toast being swiped away, the OSD): keep the frames coming
    // while it asks. Nothing else would -- its commits alone do not reliably
    // come back as a repaint here, so Qt sat waiting on the callback and a
    // swiped toast froze half off the screen until the pointer moved again.
    bool overlayWantsFrames = false;
    for (const auto LAYER :
         {ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM, ZWLR_LAYER_SHELL_V1_LAYER_TOP, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY}) {
        for (const auto& layerRef : MONITOR->m_layerSurfaceLayers[LAYER]) {
            const auto layer = layerRef.lock();
            if (Desktop::View::validMapped(layer) && surfaceTreeHasFrameCallbacks(layer->wlSurface() ? layer->wlSurface()->resource() : nullptr)) {
                surfaceTreePresent(layer->wlSurface() ? layer->wlSurface()->resource() : nullptr, MONITOR, now);
                overlayWantsFrames = overlayWantsFrames || LAYER == ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY;
            }
        }
    }
    if (overlayWantsFrames)
        requestInputFrame();
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
}

bool COverview::shouldAllowSurfaceFrame(SP<CWLSurfaceResource> surface, const Time::steady_tp& now) {
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

    if (!window || window->m_monitor != MONITOR) {
        noteFrameGate(window, "other-monitor/no-window");
        return true;
    }

    // Drawn live over the overview: it paces like any window on the desktop.
    if (onShownSpecial(window, MONITOR)) {
        noteFrameGate(window, "shown-special");
        return true;
    }

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
    if (Hyprdrive::Dock::isPreviewing(window)) {
        noteFrameGate(window, "dock-preview");
        return true;
    }

    // ...and the same argument for a window that was just retiled onto a
    // desktop nobody is looking at: it needs a few real frames to redraw at its
    // new size, or the strip shows its old buffer inside the new frame.
    if (Hyprdrive::Dock::isRetiling(window)) {
        noteFrameGate(window, "retiling");
        return true;
    }

    // ASKED FOR BY THE PASS THAT IS DRAWING IT (user, 2026-09-19: the live
    // window on the previous desktop stops painting after switching desktop
    // from the strip).
    //
    // A window appears TWICE in this overview: as a tile in the big card grid,
    // and live inside its strip thumbnail (renderStripCell -> renderWindowLive).
    // The tests below describe only the first of those, so a window whose card
    // has scrolled away -- measured at idx=0 box=-7066,98 against a 1440x900
    // monitor -- was refused frames while its thumbnail sat in plain view being
    // redrawn every frame. That is why it depended on WHICH desktop you switched
    // to: the same window was live or frozen depending on how far its card had
    // scrolled.
    //
    // When the request comes from the draw itself, the geometry question is
    // already answered -- we are painting it right now -- so only the poll/
    // stream gate has anything left to decide.
    if (askingForDrawnWindow || drawnSet.contains(window.get())) {
        if (!previewWantsFrame(window, now)) {
            noteFrameGate(window, "poll-wait(drawn)");
            return false;
        }

        noteFrameGate(window, "allowed(drawn)");
        return true;
    }

    if (shouldShowPinnedFloatingOverviewWindow(window)) {
        if (sendingOverviewFrameCallbacks)
            return true;

        scheduleRealtimePreviewFrame();
        return false;
    }

    if (!shouldShowOverviewWindow(window) || !window->m_workspace) {
        noteFrameGate(window, "not-shown-in-overview");
        return true;
    }

    const auto ACTIVEIDX = activeWorkspaceIndex();
    const auto SCALE     = scale->value();
    const auto PITCH     = getWorkspaceRenderedPitch(MONITOR, SCALE, layout);

    bool sawCard = false;
    for (size_t workspaceIdx = 0; workspaceIdx < images.size(); ++workspaceIdx) {
        const auto& workspaceImage = images[workspaceIdx];
        if (!workspaceImage || workspaceImage->pWorkspace != window->m_workspace)
            continue;

        sawCard = true;
        const auto FULLSCREENWINDOW = overviewFullscreenPathWindow(workspaceImage->pWorkspace);
        if (FULLSCREENWINDOW && FULLSCREENWINDOW != window && !window->m_isFloating) {
            noteFrameGate(window, "occluded-by-fullscreen");
            return false;
        }

        const auto WORKSPACEOFFSET = workspaceOverviewOffset(workspaceIdx, ACTIVEIDX, PITCH);

        // A FULLSCREEN WINDOW HAS NO TILE (user, 2026-09-19: a fullscreen
        // YouTube video shows a still even while its card is plainly on screen).
        // It is drawn filling the card through the fullscreen path, so asking
        // getOverviewWindowBox where its TILE is answers about a layout it is
        // not part of, the intersect test fails, and the frame is refused as
        // off-screen. Measured: the card visible in the strip and the tile 0
        // changed pixels across five captures. This test runs BEFORE the
        // selected-workspace check, which is why even the selected desktop's
        // video was frozen. Ask about the card instead, because the card is
        // what it is drawn into.
        // The card's VISIBLE box, which is what renderWorkspaceLive tests -- the
        // raw workspace box can sit off-screen for a card the scrolled strip is
        // nonetheless showing, so testing it disagreed with the pass that draws.
        const auto WINDOWBOX = FULLSCREENWINDOW == window ?
            workspaceOverviewVisibleBox(workspaceIdx, getOverviewWorkspaceBox(MONITOR, SCALE, viewOffset->value(), WORKSPACEOFFSET, layout), SCALE, MONITOR) :
            getOverviewWindowBox(window, MONITOR, SCALE, viewOffset->value(), WORKSPACEOFFSET, layout);
        if (!overviewBoxIntersectsMonitor(WINDOWBOX, MONITOR)) {
            noteFrameGate(window, "card-offscreen");
            return false;
        }

        // The selected desktop is the one being looked at: it stays live and is
        // never polled.
        if (isSelectedWorkspace(workspaceImage->pWorkspace)) {
            noteFrameGate(window, "selected-ws");
            return true;
        }

        // Every other tile is adaptive. A refused poll returns WITHOUT asking for
        // a preview frame: the whole point is not to wake for a window that has
        // nothing to show.
        if (!previewWantsFrame(window, now)) {
            noteFrameGate(window, "poll-wait");
            return false;
        }

        // A STREAMING TILE IS A DOCK PREVIEW BY ANOTHER NAME (user, 2026-09-19:
        // the video "lags/is choppy painting, until I load the dock previews by
        // hovering over the app icon"). Dock::isPreviewing returns true above
        // without consulting either throttle, so a hovered preview runs at the
        // monitor's rate; a streaming tile was going through the idle pulse
        // instead, which runs at misc:render_unfocused_fps -- 15 by default, and
        // 15fps of a 30fps video is exactly the judder described. Polling tiles
        // still pay the 2s interval, which is where the leanness actually lives.
        if (previewIsStreaming(window, now)) {
            noteFrameGate(window, "allowed(streaming)");
            return true;
        }

        if (sendingOverviewFrameCallbacks) {
            noteFrameGate(window, "allowed(pulse)");
            return true;
        }

        noteFrameGate(window, "throttled");
        scheduleRealtimePreviewFrame();
        return false;
    }

    noteFrameGate(window, sawCard ? "no-card-match" : "no-card-for-workspace");
    return false;
}

bool COverview::shouldHandleSurfaceDamage(SP<CWLSurfaceResource> surface) {
    const auto MONITOR = pMonitor.lock();
    const auto OWNER   = resolveSurfaceOwner(MONITOR, closing, surface);
    if (!OWNER)
        return true;

    const auto layerOwner  = OWNER->layer;
    const auto windowOwner = OWNER->windowOwner;

    if (layerOwner) {
        if (layerOwner->m_monitor != MONITOR)
            return true;

        // OVERLAY is transient chrome drawn ABOVE the overview (notification
        // toasts, the OSD, Omapager) and the user works it with the pointer
        // here: a toast being hovered or swiped away must repaint as it moves.
        // Its damage was refused with TOP's, so a swipe froze the toast where it
        // was. AQ_SCHEDULE_DAMAGE is throttled while the overview is up, hence
        // the input frame. TOP (the bar, which can animate nonstop) stays out.
        if (layerOwner->m_layer == ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY) {
            requestInputFrame();
            return true;
        }

        if (layerOwner->m_layer > ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM)
            return false;

        markBlurDirty();
        // Y17 (GPU, 2026-09-14). The two wallpaper caches are no longer redrawn
        // every frame, so this hook is what tells them a wallpaper changed.
        // updateCardWallpaperFB composites BACKGROUND *and* BOTTOM (Omarchy's
        // wallpaper is often a BOTTOM layer); renderGlobalWallpaper, and so the
        // backdrop, draws BACKGROUND only.
        frost.cardWallpaperDirty = true;
        // plugin:hyprdrive:backdrop: only LIVE re-blurs while the overview is
        // up. Every other mode keeps the frame it opened with, so a moving
        // wallpaper never re-runs the full-screen kawase (the cards above still
        // stream it).
        if (resolvedBackdrop(MONITOR->m_id) == EBackdrop::LIVE)
            markBackdropBlurDirty();
        return true;
    }

    if (!windowOwner)
        return true;

    auto window = getOverviewWindowToShow(windowOwner);

    // A window on another monitor is that monitor's to damage -- but while its
    // strip is out on THIS one, this one shows it too, and nothing else would
    // repaint it here. Throttled like every other off-card preview. Never
    // refuses: the owning monitor's own damage must still go through.
    if (window && window->m_monitor != MONITOR && altStripsRevealValue() > 0.01F)
        scheduleRealtimePreviewFrame();

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

    // The Sushi preview's commits repaint NOW, like the selected desktop's --
    // the tile gate below would drop them, and the new buffer would sit unseen
    // until the idle frame. See overviewSushiWindowOn.
    if (window && window == overviewSushiWindowOn(MONITOR)) {
        selectedWorkspaceFramePending = true;
        return true;
    }
    if (onShownSpecial(window, MONITOR)) {
        selectedWorkspaceFramePending = true;
        return true;
    }

    if (!shouldShowOverviewWindow(window) || !window->m_workspace) {
        noteDamageGate(window, "not-shown");
        return false;
    }

    // The mirror of askingForDrawnWindow: a commit from a window this overview
    // has been drawing is worth a repaint even when its card is off in the grid,
    // because the thumbnail showing it is not.
    if (previewPulses.contains(window.get())) {
        noteDamageGate(window, "ACCEPTED(drawn)");
        const auto NOWDRAWN = Time::steadyNow();
        notePreviewDamage(window, NOWDRAWN);
        if (previewIsStreaming(window, NOWDRAWN))
            return true;
    }

    noteDamageGate(window, "reached-loop");
    const auto ACTIVEIDX = activeWorkspaceIndex();
    const auto SCALE     = scale->value();
    const auto PITCH     = getWorkspaceRenderedPitch(MONITOR, SCALE, layout);

    for (size_t workspaceIdx = 0; workspaceIdx < images.size(); ++workspaceIdx) {
        const auto& workspaceImage = images[workspaceIdx];
        if (!workspaceImage || workspaceImage->pWorkspace != window->m_workspace)
            continue;

        const auto FULLSCREENWINDOW = overviewFullscreenPathWindow(workspaceImage->pWorkspace);
        if (FULLSCREENWINDOW && FULLSCREENWINDOW != window && !window->m_isFloating) {
            noteDamageGate(window, "occluded-by-fullscreen");
            return false;
        }

        const auto WORKSPACEOFFSET = workspaceOverviewOffset(workspaceIdx, ACTIVEIDX, PITCH);

        // Same substitution as the frame gate above, and for the same reason:
        // the commit that proves a fullscreen window is alive was being dropped
        // against a tile box it has no part in, so even when it did paint the
        // overview never repainted to show it.
        // The card's VISIBLE box, which is what renderWorkspaceLive tests -- the
        // raw workspace box can sit off-screen for a card the scrolled strip is
        // nonetheless showing, so testing it disagreed with the pass that draws.
        const auto WINDOWBOX = FULLSCREENWINDOW == window ?
            workspaceOverviewVisibleBox(workspaceIdx, getOverviewWorkspaceBox(MONITOR, SCALE, viewOffset->value(), WORKSPACEOFFSET, layout), SCALE, MONITOR) :
            getOverviewWindowBox(window, MONITOR, SCALE, viewOffset->value(), WORKSPACEOFFSET, layout);
        if (!overviewBoxIntersectsMonitor(WINDOWBOX, MONITOR)) {
            noteDamageGate(window, "card-offscreen");
            return false;
        }

        noteDamageGate(window, "ACCEPTED");
        // THE DETECTOR. The client painted something, which is the only evidence
        // that this preview is not a still -- so the tile starts streaming and
        // keeps streaming until it goes quiet for OVERVIEW_PREVIEW_QUIET_TIMEOUT.
        const auto NOWDAMAGE = Time::steadyNow();
        notePreviewDamage(window, NOWDAMAGE);

        if (isSelectedWorkspace(workspaceImage->pWorkspace)) {
            selectedWorkspaceFramePending = true;
            return true;
        }

        // ...and the commit that proved it is worth painting NOW. The 33ms
        // throttle below is right for a tile that damages once; for one that is
        // playing video it is a second cap on top of the pulse's, and the two
        // together are what made a dock hover look like a different feature.
        if (previewIsStreaming(window, NOWDAMAGE))
            return true;

        if (!realtimePreviewFrameQueued && shouldAllowRealtimePreviewFrame())
            return true;

        scheduleRealtimePreviewFrame();
        return false;

    }

    return false;
}
