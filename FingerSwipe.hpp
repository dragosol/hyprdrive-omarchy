#pragma once

#include <hyprland/src/devices/IPointer.hpp>

// The glue between the touchpad's raw fingers (TouchpadRaw), the decisions
// (FingerSwipeModel) and Hyprland: libinput's swipe/pinch/scroll events are
// swallowed while a swipe is ours, and ours are fed to the gesture manager.
namespace ScrollOverview::FingerSwipe {
    void init();
    // Config reload / device hotplug: open touchpads that appeared.
    void rescan();
    // hkOnMouseMoved asks before the motion reaches Hyprland: touchpad motion
    // while a swipe is ours would move the pointer (and fling a held drag).
    bool swallowMotion(const IPointer::SMotionEvent& e);
    void shutdown();
}
