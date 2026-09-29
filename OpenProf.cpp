#include "OpenProf.hpp"

#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/SharedDefs.hpp>

#include <chrono>
#include <cstdio>
#include <format>
#include <string>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace OpenProf {
    namespace {
        using clock                       = std::chrono::steady_clock;
        constexpr int OPENPROF_WINDOW_MS  = 1500;

        CHyprSignalListener      g_pre, g_stage;
        bool                     g_on = false;
        clock::time_point        g_t0{};
        std::vector<std::string> g_lines;
        MONITORID                g_frameMon = MONITOR_INVALID;
        clock::time_point        g_frameStart{};
        uint64_t                 g_overviewNs = 0;
        std::unordered_map<MONITORID, clock::time_point> g_lastPre;

        double msSince(clock::time_point t) {
            return std::chrono::duration<double, std::milli>(clock::now() - t).count();
        }

        void flush() {
            if (g_lines.empty())
                return;
            if (FILE* f = std::fopen("/tmp/hyprdrive-openprof.log", "a")) {
                for (const auto& l : g_lines)
                    std::fprintf(f, "%s\n", l.c_str());
                std::fclose(f);
            }
            g_lines.clear();
        }

        // Closes the window once it has run out; the next frame event after
        // that writes the block out.
        bool live() {
            if (!g_on)
                return false;
            if (msSince(g_t0) <= OPENPROF_WINDOW_MS)
                return true;
            g_on = false;
            flush();
            return false;
        }
    }

    void init() {
        g_pre = Event::bus()->m_events.render.pre.listen([](PHLMONITOR monitor) {
            if (!live() || !monitor)
                return;
            const auto NOW = clock::now();
            double     gap = -1;
            if (const auto IT = g_lastPre.find(monitor->m_id); IT != g_lastPre.end())
                gap = std::chrono::duration<double, std::milli>(NOW - IT->second).count();
            g_lastPre[monitor->m_id] = NOW;
            g_frameMon               = monitor->m_id;
            g_frameStart             = NOW;
            g_overviewNs             = 0;
            g_lines.push_back(std::format("  +{:7.1f}  mon {} gap {:6.1f}", msSince(g_t0), monitor->m_name, gap));
        });
        g_stage = Event::bus()->m_events.render.stage.listen([](eRenderStage stage) {
            if (stage != RENDER_POST || !live() || g_frameMon == MONITOR_INVALID || g_lines.empty())
                return;
            g_lines.back() += std::format("  frame {:6.2f} ms  overview {:6.2f} ms", msSince(g_frameStart), g_overviewNs / 1e6);
            g_frameMon = MONITOR_INVALID;
        });
    }

    void shutdown() {
        g_pre.reset();
        g_stage.reset();
        flush();
    }

    bool active() {
        return live();
    }

    void begin(const char* what) {
        // Read per open, not per load: touch/rm the flag with no reload.
        if (::access("/tmp/hyprdrive-openprof.on", F_OK) != 0)
            return;
        flush(); // a previous window cut short by this open
        g_on    = true;
        g_t0    = clock::now();
        g_lastPre.clear();
        g_frameMon = MONITOR_INVALID;
        g_lines.push_back(std::format("=== open ({})", what));
    }

    void mark(const char* what) {
        if (live())
            g_lines.push_back(std::format("  +{:7.1f}  {}", msSince(g_t0), what));
    }

    void addOverviewRender(uint64_t ns) {
        if (live())
            g_overviewNs += ns;
    }
}
