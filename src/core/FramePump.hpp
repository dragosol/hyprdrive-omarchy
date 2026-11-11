#pragma once

// ASK FOR ANOTHER FRAME, ONCE PER OUTPUT PER EVENT-LOOP ITERATION.
//
// The coalesced frame request every self-driven animation on the desktop rides
// (the dock's reveal, bounces, flights, previews...), and the list of foreign
// modules that may still be moving. It used to live in the dock, which meant a
// new animation in ANY module had to edit Dock.cpp to be asked about.
//
// WHERE TO ADD A FRAME SOURCE: a module whose animation runs with no pointer
// motion and no client commit left to repaint it adds ONE line to the table in
// FramePump.cpp (`SOURCES`): a reason name, a pure-as-possible predicate, and
// the slot in Dock::needsFramesReason's chain it is asked at. Nothing else:
// the dock asks the table at those slots, from its two call sites, and the
// name is what `hyprdrive:debug frames` prints.

#include <hyprland/src/SharedDefs.hpp>
#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/helpers/math/Math.hpp>

#include <cstdint>
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

    // Where in Dock::needsFramesReason's chain a source is asked. The chain
    // interleaves the dock's own terms with these, and some terms on both sides
    // are NOT pure predicates (Stacks::needsFrames retires finished hops,
    // the dock's arrangeAnimating retires finished exits, BorderFade refreshes
    // its curve cache), so the order they are asked in is kept exactly as it
    // was: each slot is named for the dock term it follows.
    enum class ESlot : uint8_t {
        AFTER_TOOLTIP,      // after "tooltip-fade", before "ghost-flight"
        AFTER_GHOST_FLIGHT, // after "ghost-flight", before "arrange"
        AFTER_ARRANGE,      // after "arrange", before "hover-panel"
    };

    struct SSource {
        const char* name;                          // what `hyprdrive:debug frames` prints
        bool (*active)(const PHLMONITOR& monitor); // never null; monitor is never null
        ESlot       slot;
    };

    // The first source in `slot`, in table order, whose predicate is true --
    // or nullptr. Sources after the first true one are not asked.
    const char* foreignReason(const PHLMONITOR& monitor, ESlot slot);
}
