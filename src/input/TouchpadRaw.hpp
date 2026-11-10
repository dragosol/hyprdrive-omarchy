#pragma once

// THE TOUCHPAD'S OWN FINGERS. libinput hands the compositor a finger COUNT for
// a gesture and nothing about which fingers they are; FingerSwipe needs the
// fingers. So every touchpad Hyprland has is opened a SECOND time, read-only,
// as an ordinary evdev client: the kernel gives each open file its own event
// queue, so this sees the full multitouch stream (slots, tracking ids,
// positions, BTN_LEFT) without taking a single event from libinput. Never
// EVIOCGRAB, and no ioctl on libinput's own fd (EVIOCGKEY flushes the queue it
// is asked on).
//
// Permission: the device node carries the uaccess tag, so the logged-in seat
// user has an ACL on it while the session is active. Opening fails cleanly
// otherwise, and FingerSwipe then falls back to libinput's plain behaviour.

#include "FingerSwipeModel.hpp"

#include <functional>
#include <hyprland/src/devices/IPointer.hpp>

namespace ScrollOverview::TouchpadRaw {
    // Called once per evdev frame (SYN_REPORT) of any touchpad.
    using FrameFn = std::function<void(const FingerSwipeModel::SFrame&)>;
    // Called when a device the frames came from goes away mid-stream.
    using LostFn = std::function<void()>;

    void init(FrameFn onFrame, LostFn onLost);
    // Open any touchpad not open yet (device hotplug, config reload, resume).
    void rescan();
    // Is this pointer one of the touchpads being read?
    bool reads(const SP<IPointer>& pointer);
    bool active();
    void shutdown();
}
