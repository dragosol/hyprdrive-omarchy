#include <hyprland/src/includes.hpp>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/state/MonitorState.hpp>
// The two scheduler flags a switch must clear (see FramePacing.hpp).
#define private public
#include <hyprland/src/output/MonitorFrameScheduler.hpp>
#undef private

#include "FramePacing.hpp"

#include "Config.hpp"

#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <systemd/sd-bus.h>
#include <unordered_map>

#include "globals.hpp"

namespace ScrollOverview::FramePacing {
    namespace {
        constexpr const char* KEY_SCHED = "render:new_render_scheduling";

        bool g_applied = false; // smooth_frames is holding the scheduler on
        int  g_saved   = 0;     // the user's own value, put back on release

        // ---- hint ----
        // A session counts once it has this many animated frames; it is "bad"
        // at this late fraction; the hint needs HINT_BAD of the last HINT_OF.
        constexpr int   MIN_FRAMES = 12;
        constexpr float BAD_RATIO  = 0.25F;
        constexpr int   HINT_OF    = 5;
        constexpr int   HINT_BAD   = 3;

        struct SRun {
            std::chrono::steady_clock::time_point last{};
            bool                                  inRun = false;
        };
        std::unordered_map<MONITORID, SRun> g_runs;
        int                                 g_frames = 0, g_late = 0;
        std::array<bool, HINT_OF>           g_recent{};
        int                                 g_recentCount = 0, g_recentNext = 0;
        bool                                g_hinted      = false; // this load

        std::filesystem::path hintMarker() {
            const char* STATE = std::getenv("XDG_STATE_HOME");
            const char* HOME  = std::getenv("HOME");
            std::filesystem::path base = STATE && *STATE ? std::filesystem::path(STATE) : std::filesystem::path(HOME ? HOME : "/tmp") / ".local/state";
            return base / "hyprdrive" / "smooth-frames-hinted";
        }

        // Clear the flags a late frame can leave behind, then switch. Safe at
        // any moment: a held-back frame that is dropped here is simply redrawn
        // at the next refresh.
        void cleanSchedulers() {
            for (const auto& M : State::monitorState()->monitors()) {
                if (!M || !M->m_frameScheduler)
                    continue;
                M->m_frameScheduler->m_renderAtFrame = true;
                M->m_frameScheduler->m_pendingThird  = false;
            }
        }

        void setScheduler(bool on) {
            if (on)
                cleanSchedulers();
            Config::setValue(KEY_SCHED, on ? 1 : 0);
            if (!g_pHyprRenderer)
                return;
            for (const auto& M : State::monitorState()->monitors())
                if (M)
                    g_pHyprRenderer->damageMonitor(M);
        }

        // org.freedesktop.Notifications.Notify, fire and forget -- no process to
        // spawn or reap inside the compositor (the Quick Look path does the same).
        // Hyprland's own overlay if the session bus or daemon is not there.
        void notify() {
            const char* TITLE = "Hyprdrive: dropped frames";
            const char* BODY  = "The overview dropped frames while animating. Turn on Smooth frames in the Hyprdrive panel "
                                "(Textures) to let Hyprland render ahead when the GPU runs late.";
            sd_bus*         bus = nullptr;
            sd_bus_message* m   = nullptr;
            bool            ok  = sd_bus_default_user(&bus) >= 0 &&
                sd_bus_message_new_method_call(bus, &m, "org.freedesktop.Notifications", "/org/freedesktop/Notifications", "org.freedesktop.Notifications", "Notify") >= 0 &&
                sd_bus_message_append(m, "susssasa{sv}i", "Hyprdrive", 0u, "", TITLE, BODY, 0, 0, 15000) >= 0;
            if (ok) {
                sd_bus_message_set_expect_reply(m, 0);
                ok = sd_bus_send(bus, m, nullptr) >= 0;
                if (ok)
                    sd_bus_flush(bus);
            }
            if (m)
                sd_bus_message_unref(m);
            if (bus)
                sd_bus_unref(bus);
            if (!ok)
                HyprlandAPI::addNotification(SCROLLOVERVIEW_HANDLE, std::string("[hyprdrive] ") + BODY, CHyprColor{0.9F, 0.7F, 0.2F, 1.F}, 15000);
        }

        void maybeHint() {
            if (g_hinted || g_applied)
                return;
            if (Config::getValue<int>(KEY_SCHED) != 0 || !g_pHyprRenderer->explicitSyncSupported())
                return; // already rendering ahead, or the new scheduler cannot run here
            int bad = 0;
            for (int i = 0; i < g_recentCount; ++i)
                bad += g_recent[i] ? 1 : 0;
            if (bad < HINT_BAD)
                return;
            g_hinted = true;
            std::error_code ec;
            const auto      MARK = hintMarker();
            if (std::filesystem::exists(MARK, ec))
                return; // once per user, not once per login
            std::filesystem::create_directories(MARK.parent_path(), ec);
            std::ofstream(MARK) << "shown\n";
            notify();
        }
    }

    void apply() {
        const bool WANT = Config::getValue<bool>("plugin:hyprdrive:smooth_frames");
        if (WANT) {
            // A config reload writes the user's value back; reassert (cleanly).
            if (!g_applied)
                g_saved = Config::getValue<int>(KEY_SCHED);
            g_applied = true;
            if (Config::getValue<int>(KEY_SCHED) != 1)
                setScheduler(true);
        } else if (g_applied) {
            g_applied = false;
            // Off: the flags left behind are ignored by the classic path, and any
            // later switch on goes through cleanSchedulers() first.
            setScheduler(g_saved != 0);
        }
    }

    // Unload -- which is ALSO Hyprland's own exit, when monitors and the
    // renderer may already be gone. Only the config value goes back: no
    // monitor walk, no damage (a reload redraws anyway, an exit needs nothing).
    void shutdown() {
        if (!g_applied)
            return;
        g_applied = false;
        // Exiting: the value is runtime-only and dies with the compositor.
        if (!g_pCompositor || g_pCompositor->m_isShuttingDown)
            return;
        Config::setValue(KEY_SCHED, g_saved != 0 ? 1 : 0);
    }

    void noteAnimatedFrame(const SP<Monitor::CMonitor>& monitor) {
        if (!monitor)
            return;
        const auto NOW    = std::chrono::steady_clock::now();
        auto&      run    = g_runs[monitor->m_id];
        const float PERIOD = monitor->m_refreshRate > 1.F ? 1000.F / monitor->m_refreshRate : 16.667F;
        if (run.inRun) {
            const float GAP = std::chrono::duration<float, std::milli>(NOW - run.last).count();
            if (GAP < 250.F) { // longer is a stall of a different kind, or a pause
                ++g_frames;
                if (GAP > PERIOD * 1.5F)
                    ++g_late;
            }
        }
        run.inRun = true;
        run.last  = NOW;
    }

    void noteIdleFrame(const SP<Monitor::CMonitor>& monitor) {
        if (monitor)
            g_runs[monitor->m_id].inRun = false;
    }

    void endSession() {
        g_runs.clear();
        if (g_frames >= MIN_FRAMES) {
            g_recent[g_recentNext] = sc<float>(g_late) / sc<float>(g_frames) >= BAD_RATIO;
            g_recentNext           = (g_recentNext + 1) % HINT_OF;
            g_recentCount          = std::min(g_recentCount + 1, HINT_OF);
            maybeHint();
        }
        g_frames = 0;
        g_late   = 0;
    }
}
