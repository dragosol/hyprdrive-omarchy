#include "OverviewGesture.hpp"

#include "Config.hpp"
#include "IOverview.hpp"
#include "scrollOverview.hpp"

#include <algorithm>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/output/Monitor.hpp>

COverviewGesture::COverviewGesture(eTrackpadGestureDirection direction) : m_direction(direction) {}

void COverviewGesture::begin(const ITrackpadGesture::STrackpadGestureBegin& e) {
    ITrackpadGesture::begin(e);

    m_lastDelta   = 0.F;
    m_firstUpdate = true;

    const auto MONITOR = Desktop::focusState()->monitor();
    if (!MONITOR)
        return;

    const bool WANT_CLOSE = m_direction == TRACKPAD_GESTURE_DIR_DOWN;
    const bool WANT_OPEN  = !WANT_CLOSE; // up, vertical, swipe: open if nothing is up

    auto       overview = scrollOverviewForMonitor(MONITOR);

    if (overview && overview->isClosing()) {
        dropScrollOverviewsForMonitor(MONITOR);
        overview.reset();
        if (WANT_CLOSE) {
            m_overview.reset();
            return;
        }
    }

    if (WANT_OPEN) {
        if (overview) {
            // Already open: swipe-up does not close. Swipe-down does.
            m_overview.reset();
            return;
        }
        if (!ensureScrollOverviewHooks())
            return;

        // Super-tap and the dock button reset the search session; swipe-open
        // was the remaining door, so a query survived close and came back.
        resetSearchSession();
        overview = makeShared<CScrollOverview>(MONITOR->m_activeWorkspace, true, MONITOR);
        registerScrollOverview(overview);
        overview->m_swipeClosing = false;
    } else {
        if (!overview) {
            m_overview.reset();
            return;
        }
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

    // Signed follow: reversing the fingers undoes the motion. Clamp to the
    // gesture length so extra travel does not wind up a reverse.
    m_lastDelta += distance(e);
    const float MAX = sc<float>(ScrollOverview::Config::getGestureDistance());
    m_lastDelta     = std::clamp(m_lastDelta, 0.F, MAX);

    OVERVIEW->onSwipeUpdate(m_lastDelta);
}

void COverviewGesture::end(const ITrackpadGesture::STrackpadGestureEnd& e) {
    const auto OVERVIEW = m_overview.lock();
    if (OVERVIEW)
        OVERVIEW->onSwipeEnd();
    m_overview.reset();
}
