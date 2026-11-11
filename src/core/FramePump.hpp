#pragma once

// ASK FOR ANOTHER FRAME, ONCE PER OUTPUT PER EVENT-LOOP ITERATION.
//
// The coalesced frame request every self-driven animation on the desktop rides
// (the dock's reveal, bounces, flights, previews...). It used to live in the
// dock.

#include <hyprland/src/SharedDefs.hpp>
#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/helpers/math/Math.hpp>

#include <optional>

namespace Hyprdrive::FramePump {
    // Damage `regionRenderPx` (monitor-local RENDER px) on `monitor` after the
    // current event-loop iteration, or the whole output for nullopt. Calls in
    // one iteration collapse into one damage: the union of their boxes, or the
    // whole output if any of them asked for it. See FramePump.cpp.
    void request(PHLMONITOR monitor, std::optional<CBox> regionRenderPx = std::nullopt);

    // An output went away: drop its pending mark and its queued doLater.
    // Called from Dock::forgetMonitor (the monitor.removed handler).
    void forget(MONITORID id);
}
