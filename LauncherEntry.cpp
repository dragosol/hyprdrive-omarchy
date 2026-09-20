#include "LauncherEntry.hpp"

#include <systemd/sd-bus.h>
#include <wayland-server.h>

#include <string>
#include <unordered_map>

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/state/MonitorState.hpp>

// The badge change is told to the DOCK, which decides whether it is visible at
// all before anything is damaged.
#include "Dock.hpp"

namespace ScrollOverview::LauncherEntry {

namespace {

    std::unordered_map<std::string, SState> g_states;
    sd_bus*                                 g_bus    = nullptr;
    wl_event_source*                        g_source = nullptr;

    // "application://org.gnome.Nautilus.desktop" -> "org.gnome.Nautilus".
    // Anything that is not that shape is not addressed at an app we can draw.
    std::string idFromUri(const char* uri) {
        static constexpr std::string_view PREFIX = "application://";
        static constexpr std::string_view SUFFIX = ".desktop";

        if (!uri)
            return {};

        std::string_view s{uri};
        if (!s.starts_with(PREFIX))
            return {};
        s.remove_prefix(PREFIX.size());

        if (!s.ends_with(SUFFIX))
            return {};
        s.remove_suffix(SUFFIX.size());

        return std::string{s};
    }

    // The protocol's own types are x/b/d, but senders are not uniform about the
    // integer width -- reading the variant's ACTUAL type is cheaper than being
    // wrong for one app.
    bool readIntVariant(sd_bus_message* m, long long& out) {
        const char* contents = nullptr;
        char        type     = 0;
        if (sd_bus_message_peek_type(m, &type, &contents) <= 0 || type != 'v' || !contents || contents[0] == '\0' || contents[1] != '\0')
            return false;

        const char T = contents[0];
        if (sd_bus_message_enter_container(m, 'v', contents) <= 0)
            return false;

        bool ok = true;
        switch (T) {
            case 'x': {
                int64_t v = 0;
                ok        = sd_bus_message_read_basic(m, 'x', &v) >= 0;
                out       = v;
                break;
            }
            case 't': {
                uint64_t v = 0;
                ok         = sd_bus_message_read_basic(m, 't', &v) >= 0;
                out        = static_cast<long long>(v);
                break;
            }
            case 'i': {
                int32_t v = 0;
                ok        = sd_bus_message_read_basic(m, 'i', &v) >= 0;
                out       = v;
                break;
            }
            case 'u': {
                uint32_t v = 0;
                ok         = sd_bus_message_read_basic(m, 'u', &v) >= 0;
                out        = v;
                break;
            }
            default: ok = false; break;
        }

        sd_bus_message_exit_container(m);
        return ok;
    }

    bool readBoolVariant(sd_bus_message* m, bool& out) {
        int v = 0;
        if (sd_bus_message_enter_container(m, 'v', "b") <= 0)
            return false;

        const bool OK = sd_bus_message_read_basic(m, 'b', &v) >= 0;
        sd_bus_message_exit_container(m);

        out = v != 0;
        return OK;
    }

    // Update(s app_uri, a{sv} properties). A sender may send ONE property, so
    // this starts from what is already known rather than from a blank state --
    // an app that pushes only `count` must not have its `urgent` cleared.
    int onUpdate(sd_bus_message* m, void*, sd_bus_error*) {
        const char* uri = nullptr;
        if (sd_bus_message_read_basic(m, 's', &uri) < 0)
            return 0;

        const auto ID = idFromUri(uri);
        if (ID.empty())
            return 0;

        SState       state  = g_states.contains(ID) ? g_states[ID] : SState{};
        const SState BEFORE = state;

        if (sd_bus_message_enter_container(m, 'a', "{sv}") <= 0)
            return 0;

        while (sd_bus_message_enter_container(m, 'e', "sv") > 0) {
            const char* key = nullptr;
            if (sd_bus_message_read_basic(m, 's', &key) < 0 || !key) {
                sd_bus_message_exit_container(m);
                break;
            }

            const std::string_view K{key};
            bool                   handled = false;
            if (K == "count")
                handled = readIntVariant(m, state.count);
            else if (K == "count-visible")
                handled = readBoolVariant(m, state.countVisible);
            else if (K == "urgent")
                handled = readBoolVariant(m, state.urgent);

            if (!handled)
                sd_bus_message_skip(m, "v");

            sd_bus_message_exit_container(m);
        }
        sd_bus_message_exit_container(m);

        if (BEFORE.count == state.count && BEFORE.countVisible == state.countVisible && BEFORE.urgent == state.urgent)
            return 0;

        g_states[ID] = state;

        // An idle desktop produces no frames, so the dock would not notice this
        // until something else happened to repaint -- measured 2026-08-03: an
        // urgent app did not pull the dock out until an unrelated config reload
        // damaged the screen. The dock's own damageIfChanged decides what
        // actually changes; this only guarantees it gets asked.
        //
        // Through damageIfChanged and not a bare damageMonitor (user,
        // 2026-09-14): a count that moved on an app NOT on the dock, or one
        // whose badge is switched off, used to repaint both whole outputs
        // anyway. damageIfChanged compares the pill's appearance and damages
        // only the monitors where something actually changed.
        for (const auto& MONITOR : State::monitorState()->monitors()) {
            if (MONITOR)
                Dock::damageIfChanged(MONITOR);
        }

        return 0;
    }

    int onBusReadable(int, uint32_t, void*) {
        while (sd_bus_process(g_bus, nullptr) > 0) { }
        return 0;
    }

}

SState state(const std::string& desktopId) {
    if (desktopId.empty())
        return {};

    if (const auto IT = g_states.find(desktopId); IT != g_states.end())
        return IT->second;

    return {};
}

sd_bus* sessionBus() {
    // start() is idempotent and cheap once the bus is up, so this is an
    // ensure rather than a getter: the preview path can be the first thing in
    // a session to want a bus (badges off, or nothing has broadcast yet).
    if (!g_bus)
        start();
    return g_bus;
}

void start() {
    if (g_bus)
        return;

    // Blocking, but only here: sd_bus_open_user's handshake and AddMatch are
    // both round trips on a local socket, and this runs once, when the dock is
    // first drawn.
    if (sd_bus_open_user(&g_bus) < 0) {
        g_bus = nullptr;
        return;
    }

    // No well-known name is taken: this only LISTENS. Taking
    // com.canonical.Unity would make the dock the Unity API's owner, which is a
    // claim about the whole session, not about drawing a badge.
    if (sd_bus_add_match(g_bus, nullptr, "type='signal',interface='com.canonical.Unity.LauncherEntry',member='Update'", onUpdate, nullptr) < 0) {
        stop();
        return;
    }

    // On the compositor's OWN event loop rather than a worker thread: the
    // handler's whole job is to damage monitors, which is a main-thread act.
    //
    // READABLE only, still, and V5 is the method call the old note said to
    // revisit this for. It does not change the answer: the preview call sets
    // expect_reply=0 and flushes itself, so nothing this connection sends ever
    // waits on POLLOUT. Add the writable half the day something here wants an
    // answer back.
    g_source = wl_event_loop_add_fd(g_pCompositor->m_wlEventLoop, sd_bus_get_fd(g_bus), WL_EVENT_READABLE, onBusReadable, nullptr);
}

void stop() {
    if (g_source) {
        wl_event_source_remove(g_source);
        g_source = nullptr;
    }

    if (g_bus) {
        sd_bus_flush_close_unref(g_bus);
        g_bus = nullptr;
    }

    g_states.clear();
}

}
