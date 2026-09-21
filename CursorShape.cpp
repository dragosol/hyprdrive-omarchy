// THE REQUESTED CURSOR SHAPE, PUSHED TO IPC (user, 2026-09-21): the Claude-cursor overlay
// (~/Projects/claude-computer-use/cursor) draws its own animated cursor while Claude drives
// the desktop, and it should look like whatever the app under the pointer ASKED for -- the
// I-beam over text, the hand over a link, a resize arrow on a border. Hyprland knows that
// (CInputManager::m_cursorSurfaceInfo: a shape name set through cursor-shape-v1, or a
// client surface), but no IPC exposes it. This posts `cursorshape>>NAME` on socket2 whenever
// it changes: push, not poll -- no timer, no process, one string compare per event.
//
//   NAME  a cursor-shape-v1 name ("default", "text", "pointer", "grabbing", "ew-resize", ...),
//         "surface" when the client draws its own cursor image, "hidden" when it hid it.
#include "CursorShape.hpp"

#include <any>
#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>
// standard headers first: the access hack below must not reach them
#define private public
#define protected public
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/protocols/CursorShape.hpp>
#include <hyprland/src/managers/EventManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#undef protected
#undef private
#include <hyprland/src/event/EventBus.hpp>

namespace CursorShape {

namespace {
    std::string          g_last;
    bool                 g_pending = false;
    uint64_t             g_seq     = 0;
    CHyprSignalListener  g_setShape;
    CHyprSignalListener  g_move;

    std::string current() {
        if (!g_pInputManager)
            return {};
        const auto& I = g_pInputManager->m_cursorSurfaceInfo;
        if (I.hidden)
            return "hidden";
        if (!I.name.empty())
            return I.name;
        return I.wlSurface ? "surface" : "default";
    }

    void check() {
        g_pending = false;
        g_seq     = 0;
        const auto NOW = current();
        if (NOW.empty() || NOW == g_last || !g_pEventManager)
            return;
        g_last = NOW;
        g_pEventManager->postEvent(SHyprIPCEvent{.event = "cursorshape", .data = NOW});
    }

    // Read AFTER the event is handled: the input manager applies a shape in its own listener,
    // and a moved pointer's new surface sets its cursor in reply. One deferred check per burst.
    void schedule() {
        if (g_pending || !g_pEventLoopManager)
            return;
        g_pending = true;
        g_seq     = g_pEventLoopManager->doLater([] { check(); });
    }
}

void init() {
    if (PROTO::cursorShape)
        g_setShape = PROTO::cursorShape->m_events.setShape.listen([](const CCursorShapeProtocol::SSetShapeEvent&) { schedule(); });
    g_move = Event::bus()->m_events.input.mouse.move.listen([](Vector2D, Event::SCallbackInfo&) { schedule(); });
    g_last.clear();
}

void shutdown() {
    g_setShape.reset();
    g_move.reset();
    // a queued doLater would call into this plugin after dlclose: cancel it
    if (g_seq && g_pEventLoopManager)
        g_pEventLoopManager->removeDoLater(g_seq);
    g_seq     = 0;
    g_pending = false;
}

} // namespace CursorShape
