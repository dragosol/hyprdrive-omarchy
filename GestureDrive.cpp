// Driving Hyprland's trackpad gesture manager with swipes libinput never sent
// (FingerSwipe: a swipe continued past lifted fingers, a swipe under a held
// drag). gestureBegin/Update/End are public; the one thing they cannot do is
// pick a gesture while a modifier is held -- a Super+drag of a window holds
// Super, and every configured swipe is bound with no modifiers -- so for a
// drag swipe the pick is made here, the same way gestureUpdate makes it, minus
// the modifier test.
//
// Standard headers first: the access hack must not reach them.
#include <cmath>
#include <hyprland/src/devices/IPointer.hpp>
#include <hyprland/src/managers/input/trackpad/gestures/ITrackpadGesture.hpp>
#define private public
#include <hyprland/src/managers/input/trackpad/TrackpadGestures.hpp>
#undef private

#include "GestureDrive.hpp"

namespace ScrollOverview::GestureDrive {

    void begin(uint32_t timeMs, uint32_t fingers) {
        if (g_pTrackpadGestures)
            g_pTrackpadGestures->gestureBegin(IPointer::SSwipeBeginEvent{.timeMs = timeMs, .fingers = fingers});
    }

    void update(uint32_t timeMs, uint32_t fingers, const Vector2D& delta, bool ignoreMods) {
        auto* T = g_pTrackpadGestures.get();
        if (!T)
            return;
        const IPointer::SSwipeUpdateEvent E{.timeMs = timeMs, .fingers = fingers, .delta = delta};
        if (!ignoreMods || T->m_activeGesture || T->m_gestureFindFailed) {
            T->gestureUpdate(E);
            return;
        }

        T->m_currentTotalDelta += delta;
        const auto TOTAL = T->m_currentTotalDelta;
        if (std::abs(TOTAL.x) < 5 && std::abs(TOTAL.y) < 5)
            return;

        const auto AXIS      = std::abs(TOTAL.x) > std::abs(TOTAL.y) ? TRACKPAD_GESTURE_DIR_HORIZONTAL : TRACKPAD_GESTURE_DIR_VERTICAL;
        const auto DIRECTION = AXIS == TRACKPAD_GESTURE_DIR_HORIZONTAL ? (TOTAL.x < 0 ? TRACKPAD_GESTURE_DIR_LEFT : TRACKPAD_GESTURE_DIR_RIGHT) :
                                                                          (TOTAL.y < 0 ? TRACKPAD_GESTURE_DIR_UP : TRACKPAD_GESTURE_DIR_DOWN);
        for (const auto& G : T->m_gestures) {
            if (G->direction != AXIS && G->direction != DIRECTION && G->direction != TRACKPAD_GESTURE_DIR_SWIPE)
                continue;
            if (G->fingerCount != fingers)
                continue;
            T->m_activeGesture     = G;
            G->currentDirection    = G->gesture->isDirectionSensitive() ? G->direction : DIRECTION;
            G->gesture->begin({.swipe = &E, .direction = DIRECTION, .scale = G->deltaScale});
            break;
        }
        if (!T->m_activeGesture) {
            T->m_gestureFindFailed = true;
            return;
        }
        T->m_activeGesture->gesture->update({.swipe = &E, .direction = T->m_activeGesture->currentDirection, .scale = T->m_activeGesture->deltaScale});
    }

    void end(uint32_t timeMs, bool cancelled) {
        if (g_pTrackpadGestures)
            g_pTrackpadGestures->gestureEnd(IPointer::SSwipeEndEvent{.timeMs = timeMs, .cancelled = cancelled});
    }

    bool active() {
        return g_pTrackpadGestures && g_pTrackpadGestures->m_activeGesture;
    }
}
