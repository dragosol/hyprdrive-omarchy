// The frame pump and the foreign frame sources (FramePump.hpp says what each
// is for and where to add one). Moved out of the dock, unchanged.

#include "FramePump.hpp"
#include "MotionClock.hpp"

#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/render/Renderer.hpp>

#include <algorithm>
#include <array>
#include <unordered_map>

#include "BorderFade.hpp"
#include "DockStacks.hpp"
#include "FlightController.hpp"
#include "Mosaic.hpp"
#include "WindowPill.hpp"

namespace Hyprdrive::FramePump {
namespace {
// ONE OUTSTANDING FRAME REQUEST PER MONITOR, and what it has to damage.
//
// The region is accumulated rather than taken from the first caller: within one
// event-loop iteration the dock's own animation and a live preview on another
// output can both ask, and a request that kept only the first box would leave
// the second one's pixels stale. `full` wins over any box, which is what a
// caller that cannot describe its damage asks for.
struct SPumpRequest {
    bool full    = false;
    bool haveBox = false;
    CBox box; // monitor-local RENDER px, the union of every region asked for
};
std::unordered_map<MONITORID, SPumpRequest> g_pumpPending;
std::unordered_map<MONITORID, uint64_t>     g_pumpSeqs;

// A PUMP THAT OUTLIVES THE PLUGIN IS A CALL INTO UNMAPPED MEMORY.
//
// The doLater a pump queues runs on the NEXT event-loop iteration, and
// PLUGIN_EXIT is followed straight away by the dlclose. releasePointerFocus()
// used to take every outstanding request out, which is part of why main.cpp
// calls it first at unload -- but it is also called from the pointer hook and
// from an overview opening, where dropping another monitor's pump froze its
// dock mid-animation (user, 2026-09-14). So the guarantee moves here, where no
// input event can trigger it: a static whose destructor runs when this object is
// unloaded. Declared AFTER the two maps, so it is destroyed BEFORE them.
//
// Sound whichever way the host unloads: with a dlclose this runs and the
// requests are cancelled; without one the code stays mapped and a late callback
// is merely a damaged monitor.
struct SPumpReaper {
    ~SPumpReaper() {
        if (!g_pEventLoopManager)
            return;

        for (auto [id, seq] : g_pumpSeqs) {
            if (seq)
                g_pEventLoopManager->removeDoLater(seq);
        }
        g_pumpSeqs.clear();
        g_pumpPending.clear();
    }
};
SPumpReaper g_pumpReaper;

// THE FOREIGN FRAME SOURCES, in the order Dock::needsFramesReason asks them.
// The dock's own terms (autohide, menu, press, tilt, tooltip, ghost flight,
// arrangement, hover panel, bounce, spring, launch zoom) stay in that function;
// this is every term that belongs to another module. A new one is one line
// here, in the slot whose neighbours it should be asked between -- order within
// a slot is the order asked, and the first true one names the reason.
constexpr std::array SOURCES{
    // V2. The SUPER-hold pill eases in and out over windows on a desktop where
    // nothing else is repainting, exactly like the dock's own terms. It is
    // asked from the dock's chain rather than pumped from its own module for
    // the reason that chain exists at all: one list, in one place, so a new
    // animation cannot join two of the three pumps and be forgotten by the
    // third.
    SSource{"window-pill", [](const PHLMONITOR&) { return WindowPill::animating(); }, ESlot::AFTER_TOOLTIP},

    // The titles Super shows over windows (Mosaic::renderZoomLabels) fade on
    // the tooltip's clock, over a pointer that may have stopped.
    SSource{"super-labels", [](const PHLMONITOR&) { return Mosaic::labelsAnimating(); }, ESlot::AFTER_TOOLTIP},

    // V11 V4. The stack popup's entrance, its drag ghost, and the hop or the
    // shake that acknowledges a move. Asked from the dock's chain rather than
    // pumped from its own module for the reason stated there: ONE list.
    // NOT PURE: Stacks::animating() retires finished hops and shakes.
    SSource{"stacks", [](const PHLMONITOR& monitor) { return Stacks::needsFrames(monitor->m_id); }, ESlot::AFTER_GHOST_FLIGHT},

    SSource{"flight", [](const PHLMONITOR& monitor) { return Flight::anyOn(monitor); }, ESlot::AFTER_ARRANGE},

    // The pill's and the preview rings' border fade (BorderFade.hpp): a
    // pointer resting on the pill emits no motion to repaint it.
    SSource{"border-fade", [](const PHLMONITOR& monitor) { return BorderFade::animatingOn(monitor); }, ESlot::AFTER_ARRANGE},
};
}

// ASK FOR ANOTHER FRAME. Every animation the dock owns runs on a desktop where
// nothing else is moving, so nothing else damages the monitor -- and a damage
// requested from INSIDE the render pass is cleared with that pass, so it has to
// be posted for after it. doLater is that, and it both starts and sustains the
// loop.
//
// ONE REQUEST PER FRAME, and that is not tidiness either. damageIfChanged runs
// on every pointer MOTION event, and motion events arrive far faster than
// frames do -- so a predicate that is true while the pointer merely rests on a
// dock icon (the hover panel is up, a bounce is running) posts a doLater per
// motion, each of which damages the monitor, each of which causes a frame,
// whose render.pre calls damageIfChanged again. Measured 2026-08-14: without
// this guard zflight goes from 8/8 green to z4/z4c/z5/z6 red with the dock
// missing clicks outright (zrig/danglingC2/zflight-pump-flood.log). The
// collapse is exact rather than approximate: two damageMonitor calls in one
// event-loop iteration are the same region twice, so dropping the second
// changes nothing but the flood.
//
// THE MARK IS ERASED, NOT SET FALSE, and the difference is a latch. `PENDING`
// is the only thing standing between a motion event and another doLater, so a
// mark that survives its callback is a dock that never asks for a frame again:
// the entry reads true, every request() returns early, and the reveal, the
// bounce and the flights all stop mid-animation on that output with nothing in
// any log. The callback erases, and forget() erases on the way out --
// because a doLater queued against a monitor that is then removed is dropped
// with the loop, and its callback is exactly what would have cleared the mark.
void request(PHLMONITOR monitor, std::optional<CBox> regionRenderPx) {
    if (!monitor)
        return;

    const bool ALREADY = g_pumpPending.contains(monitor->m_id);

    auto&      REQ     = g_pumpPending[monitor->m_id];
    if (!regionRenderPx)
        REQ.full = true;
    else if (!REQ.full && !regionRenderPx->empty()) {
        if (!REQ.haveBox) {
            REQ.box     = *regionRenderPx;
            REQ.haveBox = true;
        } else {
            // The union, spelled out: CBox has no "grow to contain" of its own.
            const double X0 = std::min(REQ.box.x, regionRenderPx->x);
            const double Y0 = std::min(REQ.box.y, regionRenderPx->y);
            const double X1 = std::max(REQ.box.x + REQ.box.width, regionRenderPx->x + regionRenderPx->width);
            const double Y1 = std::max(REQ.box.y + REQ.box.height, regionRenderPx->y + regionRenderPx->height);
            REQ.box         = CBox{X0, Y0, X1 - X0, Y1 - Y0};
        }
    }

    if (ALREADY)
        return;

    if (g_pEventLoopManager) {
        if (const auto IT = g_pumpSeqs.find(monitor->m_id); IT != g_pumpSeqs.end() && IT->second) {
            g_pEventLoopManager->removeDoLater(IT->second);
            g_pumpSeqs.erase(IT);
        }
        g_pumpSeqs[monitor->m_id] = g_pEventLoopManager->doLater([MONITOR = PHLMONITORREF{monitor}, ID = monitor->m_id] {
            SPumpRequest request;
            if (const auto IT = g_pumpPending.find(ID); IT != g_pumpPending.end())
                request = IT->second;
            g_pumpPending.erase(ID);
            g_pumpSeqs.erase(ID);
            if (!MONITOR)
                return;

            const auto MON = MONITOR.lock();
            if (request.full || !request.haveBox || request.box.empty()) {
                g_pHyprRenderer->damageMonitor(MON);
                return;
            }

            // damageBox wants GLOBAL LOGICAL coordinates and everything the dock
            // holds is monitor-local render px: divide out the scale, add the
            // output's position. Rounded OUTWARD, because a box that lands half
            // a pixel inside its own damage leaves a stale row.
            const double SCALE = std::max(MON->m_scale, 0.01F);
            CBox         box{request.box.x / SCALE + MON->m_position.x, request.box.y / SCALE + MON->m_position.y, request.box.width / SCALE, request.box.height / SCALE};
            box.expand(1.0);
            g_pHyprRenderer->damageBox(box.round());
        });
    }
}

void forget(MONITORID id) {
    g_pumpPending.erase(id);
    if (g_pEventLoopManager) {
        if (const auto IT = g_pumpSeqs.find(id); IT != g_pumpSeqs.end() && IT->second) {
            g_pEventLoopManager->removeDoLater(IT->second);
            g_pumpSeqs.erase(IT);
        }
    }
}

const char* foreignReason(const PHLMONITOR& monitor, ESlot slot) {
    for (const auto& SOURCE : SOURCES) {
        if (SOURCE.slot == slot && SOURCE.active(monitor))
            return SOURCE.name;
    }
    return nullptr;
}
}
