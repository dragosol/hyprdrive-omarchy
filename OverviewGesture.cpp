#include "OverviewGesture.hpp"

#include "scrollOverview.hpp"

#include <algorithm>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/output/Monitor.hpp>

void COverviewGesture::begin(const ITrackpadGesture::STrackpadGestureBegin& e) {
    ITrackpadGesture::begin(e);

    m_lastDelta   = 0.F;
    m_firstUpdate = true;

    const auto MONITOR  = Desktop::focusState()->monitor();
    if (!MONITOR)
        return;

    auto       overview = scrollOverviewForMonitor(MONITOR);

    if (overview && overview->isClosing()) {
        // Mid-close, or the zombie this gesture used to leave behind (marked
        // closing, never destroyed). A second swipe must be able to get out.
        dropScrollOverviewsForMonitor(MONITOR);
        m_overview.reset();
        return;
    }

    if (!overview) {
        if (!ensureScrollOverviewHooks())
            return;

        overview = makeShared<CScrollOverview>(MONITOR->m_activeWorkspace, true, MONITOR);
        registerScrollOverview(overview);
        overview->m_swipeClosing = false;
    } else {
        overview->selectHoveredWorkspace();
        overview->m_swipeClosing = true;
    }

    overview->m_isSwiping = true;
    m_overview            = overview;
}

void COverviewGesture::update(const ITrackpadGesture::STrackpadGestureUpdate& e) {
    const auto OVERVIEW = m_overview.lock();
    if (!OVERVIEW)
        return;

    if (m_firstUpdate) {
        m_firstUpdate = false;
        return;
    }

    m_lastDelta += distance(e);

    if (m_lastDelta <= 0.01) // plugin will crash if swipe ends at <= 0
        m_lastDelta = 0.01;

    OVERVIEW->onSwipeUpdate(m_lastDelta);
}

void COverviewGesture::end(const ITrackpadGesture::STrackpadGestureEnd& e) {
    const auto OVERVIEW = m_overview.lock();
    if (OVERVIEW)
        OVERVIEW->onSwipeEnd();
    m_overview.reset();
}
