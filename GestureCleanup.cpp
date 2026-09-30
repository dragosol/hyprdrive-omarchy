// A SWIPE IN FLIGHT AT UNLOAD (2026-09-29: the session aborted in
// CTrackpadGestures::gestureUpdate). clearGestures() empties the registered
// list, but the gesture being performed lives on in m_activeGesture -- an
// object whose code is this plugin's -- so the next finger motion after a
// live plugin reload called into unloaded code. It is ended here as cancelled
// (so it cleans up: a half-done desktop switch springs back, a held
// follow_mouse is restored) and then dropped.
//
// Standard headers first: the access hack must not reach them.
#include <hyprland/src/devices/IPointer.hpp>
#include <hyprland/src/managers/input/trackpad/gestures/ITrackpadGesture.hpp>
#define private public
#include <hyprland/src/managers/input/trackpad/TrackpadGestures.hpp>
#undef private

#include "OverviewGesture.hpp"

namespace ScrollOverview {
    void endActiveTrackpadGesture() {
        if (!g_pTrackpadGestures)
            return;
        auto& active = g_pTrackpadGestures->m_activeGesture;
        if (!active)
            return;
        if (active->gesture) {
            // Both halves set: a swipe gesture reads .swipe, a pinch one .pinch.
            const IPointer::SSwipeEndEvent SWIPE{.timeMs = 0, .cancelled = true};
            const IPointer::SPinchEndEvent PINCH{.timeMs = 0, .cancelled = true};
            active->gesture->end({.swipe = &SWIPE, .pinch = &PINCH, .direction = active->currentDirection, .scale = active->deltaScale});
        }
        active.reset();
    }
}
