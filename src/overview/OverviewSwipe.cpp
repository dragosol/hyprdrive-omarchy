// CScrollOverview: trackpad tape swipe, selection/viewport sync, workspace swipe gestures and detents.
// Cut from scrollOverview.cpp by pure code motion.
#include "OverviewParts.hpp"

using namespace OverviewDetail;


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
    const uint32_t DELAY = sc<uint32_t>(Hyprdrive::Config::getScrollEventDelay());

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
    ALGO->moveTape(sc<float>(-1 * delta * Hyprdrive::Config::getTouchpadScrollFactor() / SCALE));
    damage();
}

bool CScrollOverview::onTapeSwipeUpdate(double delta) {
    if (closing || images.empty() || viewportCurrentWorkspace >= images.size() || !images[viewportCurrentWorkspace])
        return false;

    const auto WORKSPACE = images[viewportCurrentWorkspace]->pWorkspace;
    if (!isWorkspaceScrolling(WORKSPACE))
        return false;

    // 0 is trackpadSwipeLayout's "fingers lifted"; a real update never asks for it.
    if (delta != 0.0)
        trackpadSwipeLayout(WORKSPACE, delta);
    return true;
}

void CScrollOverview::onTapeSwipeEnd() {
    if (images.empty() || viewportCurrentWorkspace >= images.size() || !images[viewportCurrentWorkspace])
        return;
    trackpadSwipeLayout(images[viewportCurrentWorkspace]->pWorkspace, 0.0);
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
    trackpadScrollAccum         += delta * Hyprdrive::Config::getTouchpadScrollFactor();

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

    // Browsing must not OPEN anything: focusing a window on a special
    // workspace makes Hyprland show that workspace, so a search whose first
    // match lived in the scratchpad pulled the scratchpad open on the first
    // keystroke (user, 2026-09-30: "empty desktop, type 'm', it opens the
    // scratchpad"). It stays the selection; picking it (close()) focuses it.
    if (window->m_workspace && window->m_workspace->m_isSpecialWorkspace)
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

namespace {

    // Detent warp across one card-to-card interval, u in [0,1].
    //
    //   g(u)  = u - (K/2pi) * sin(2pi*u)
    //   g'(u) = 1 - K * cos(2pi*u)
    //
    // g(0) = 0 and g(1) = 1 EXACTLY, because sin(0) = sin(2pi) = 0. That is the
    // load-bearing property: an interval costs the same finger travel warped or
    // unwarped, so the detent buys stickiness without spending pad runway. The
    // travel is merely redistributed -- slow at the two ends (the card centres),
    // fast through the middle (the gap).
    //
    // Monotonic while |K| < 1. Config clamps to 0.9; at K = 1, g'(0) = 0 and the
    // cards stop dead under a moving finger.
    double detentWarp(double u, double k) {
        if (k <= 0.0)
            return u;

        constexpr double TWO_PI = 6.283185307179586;
        return u - (k / TWO_PI) * std::sin(TWO_PI * u);
    }

    // A flick can cross several boundaries between two frames. One spawn per
    // crossing then overlaps into a rattle instead of reading as separate
    // detents, so ticks are floored to this spacing and the extras are dropped.
    constexpr int64_t DETENT_TICK_MIN_MS = 55;
}

void CScrollOverview::onWorkspaceSwipeBegin() {
    trackpadWorkspaceFollowing = true;
    trackpadTapeFollowing      = false;
    // Logical units, same as viewOffset — a slow swipe must not have to cover
    // a full card width on the pad.
    trackpadScrollAccum = viewOffset ? axisValue(viewOffset->value(), layout) : 0.0;

    // The raw accumulator starts where the cards already are. Seeding it from
    // the warped value is correct here and only here: at swipe start the two
    // agree, because the previous swipe ended settled on a card.
    trackpadDetentRaw       = trackpadScrollAccum;
    trackpadDetentActive    = true;
    trackpadDetentFocus     = 0.F;
    trackpadDetentWorkspace = viewportCurrentWorkspace;
    trackpadDetentBornAt    = Time::steadyNow();
    // Seeded to the card under the finger, NOT to "none": otherwise the first
    // update fires a tick for a boundary the user has not crossed yet.
    trackpadDetentTickedIdx = viewportCurrentWorkspace;
}

void CScrollOverview::maybeTickDetent(size_t workspaceIdx) {
    if (workspaceIdx == trackpadDetentTickedIdx)
        return;

    trackpadDetentTickedIdx = workspaceIdx;

    const auto CMD = Hyprdrive::Config::getGestureDetentSound();
    // Missing or non-executable is silent, not an error: the default names a
    // path that only some machines have, and a detent is not worth a failed
    // spawn per boundary on the ones that do not.
    if (CMD.empty() || ::access(CMD.c_str(), X_OK) != 0)
        return;

    const auto NOW = Time::steadyNow();
    if (trackpadDetentLastTick.time_since_epoch().count() != 0 &&
        std::chrono::duration_cast<std::chrono::milliseconds>(NOW - trackpadDetentLastTick).count() < DETENT_TICK_MIN_MS)
        return;

    trackpadDetentLastTick = NOW;

    // runAsync, and a binary rather than a shell string: this runs on the
    // compositor thread inside a gesture update, so it may not fork and it may
    // not wait. The config value is an executable path, not a command line --
    // a shell here would be a process that cannot be killed promptly, which is
    // the same reason OmarchySearch spawns the way it does.
    Hyprutils::OS::CProcess process(CMD, {});
    process.runAsync();
}

void CScrollOverview::onWorkspaceSwipeUpdate(double delta) {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR || !viewOffset)
        return;

    const float SCALE = std::max<float>(scale->value(), 0.01F);
    const float PITCH = getWorkspaceLogicalPitch(MONITOR, SCALE, layout);
    // gesture_distance px on the pad = one desktop. Linear in finger travel,
    // not velocity, so a slow swipe still reaches the next workspace.
    const float DIST  = std::max(80.F, sc<float>(Hyprdrive::Config::getGestureDistance()));

    trackpadWorkspaceFollowing = true;

    // The card offsets are NOT a uniform lattice. workspaceOverviewLogicalOffset
    // adds each neighbour pair's overflow (EXTRAINTERVAL) on top of the pitch,
    // so cells genuinely differ in width. Snapping to multiples of PITCH would
    // therefore walk off the cards the moment one workspace overflows, and the
    // drift would grow with distance from the active one. Snap to the real
    // offsets instead, and carry the image index along so the highlight knows
    // which card it landed on.
    std::vector<std::pair<double, size_t>> stops;
    stops.reserve(images.size());
    for (size_t i = 0; i < images.size(); ++i) {
        if (!images[i] || !images[i]->pWorkspace)
            continue;
        stops.emplace_back(sc<double>(workspaceOverviewLogicalOffset(i, viewportCurrentWorkspace, PITCH)), i);
    }

    if (stops.empty()) {
        // No cards to detent against. Keep the old linear behaviour rather than
        // dividing by an empty span.
        trackpadScrollAccum += delta * (PITCH / DIST);
        trackpadDetentRaw   = trackpadScrollAccum;
        trackpadDetentFocus = 0.F;
        viewOffset->setValueAndWarp(axisOffsetVector(sc<float>(trackpadScrollAccum), layout));
        damage();
        return;
    }

    std::sort(stops.begin(), stops.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

    // The finger drives the RAW accumulator; the warp is applied on the way out.
    // Accumulating into the warped value would feed the curve its own output
    // every frame and compound it into a stall.
    trackpadDetentRaw = std::clamp(trackpadDetentRaw + delta * (PITCH / DIST), stops.front().first, stops.back().first);

    // Bracket the raw offset by the two cards it lies between.
    size_t hi = 0;
    while (hi < stops.size() && stops[hi].first < trackpadDetentRaw)
        ++hi;

    const size_t HIIDX = std::min(hi, stops.size() - 1);
    const size_t LOIDX = HIIDX == 0 ? 0 : HIIDX - 1;
    const double LO    = stops[LOIDX].first;
    const double HI    = stops[HIIDX].first;
    const double SPAN  = HI - LO;

    double u = 0.0;
    if (SPAN > 1e-6) {
        u                   = std::clamp((trackpadDetentRaw - LO) / SPAN, 0.0, 1.0);
        trackpadScrollAccum = LO + SPAN * detentWarp(u, sc<double>(Hyprdrive::Config::getGestureDetent()));
    } else
        trackpadScrollAccum = trackpadDetentRaw;

    viewOffset->setValueAndWarp(axisOffsetVector(sc<float>(trackpadScrollAccum), layout));

    // The nearer card takes the highlight. Focus is 1 on a centre and 0 at the
    // midpoint, so the ring and the pill fade with the motion instead of
    // snapping on at the crossing.
    const size_t NEAREST = u > 0.5 ? HIIDX : LOIDX;
    // Re-arm the pop ONLY on a change: re-stamping every frame would hold the
    // entrance at t=0 and the number would never finish growing.
    if (stops[NEAREST].second != trackpadDetentWorkspace)
        trackpadDetentBornAt = Time::steadyNow();
    trackpadDetentWorkspace = stops[NEAREST].second;
    trackpadDetentFocus     = sc<float>(1.0 - 2.0 * std::min(u, 1.0 - u));
    trackpadDetentActive    = true;

    maybeTickDetent(stops[NEAREST].second);

    damage();
}

void CScrollOverview::onWorkspaceSwipeEnd() {
    // Drop the highlight and the pill with the gesture. finishWorkspaceScrollFollow
    // animates the settle from trackpadScrollAccum, which is already the warped
    // value the cards are sitting at, so the release does not jump.
    trackpadDetentActive    = false;
    trackpadDetentFocus     = 0.F;
    trackpadDetentTickedIdx = std::numeric_limits<size_t>::max();

    finishWorkspaceScrollFollow();
}

void CScrollOverview::onSwipeUpdate(double delta) {
    const int DISTANCE = Hyprdrive::Config::getGestureDistance();

    m_isSwiping = true;

    const bool  DISMISS = m_swipeClosing || closing;
    const float PERC    = DISMISS ? 1.0 - std::clamp(delta / sc<double>(DISTANCE), 0.0, 1.0) : std::clamp(delta / sc<double>(DISTANCE), 0.0, 1.0);

    // MOTION OFF: this path warps every frame to follow the finger, so it is not
    // a Hyprland animated assignment and syncPluginAnimEnabled has nothing here
    // to disable -- the progressive scale IS the animation, and it survived
    // anim_scale 0 for exactly that reason.
    //
    // Snap to whichever end the gesture is past rather than committing on the
    // first event: the swipe stays cancellable (reverse under halfway and it
    // snaps back to the desktop), while the overview itself only ever appears
    // fully open or fully gone. onSwipeEnd's 0.35/0.65 thresholds then see a
    // clean 0 or 1 and resolve the same way they always did.
    if (!Hyprdrive::Config::motionEnabled()) {
        scale->setValueAndWarp(PERC >= 0.5F ? Hyprdrive::Config::getScale() : 1.F);
        return;
    }

    scale->setValueAndWarp(hyprlerp(1.F, Hyprdrive::Config::getScale(), PERC));
}

void CScrollOverview::onSwipeEnd() {
    const bool DISMISS = m_swipeClosing;
    m_isSwiping        = false;
    m_swipeClosing     = false;

    // Scale 1 is the desktop, the configured overview scale is fully open.
    // A reverse mid-gesture is just this number coming back the other way.
    const float OPEN     = Hyprdrive::Config::getScale();
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
