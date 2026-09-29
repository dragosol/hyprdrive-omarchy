#include "OpenProf.hpp"

#include <hyprland/src/includes.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/SharedDefs.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/pass/PassElement.hpp>

#include <EGL/egl.h>
#include <GLES2/gl2ext.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <format>
#include <string>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace OpenProf {
    namespace {
        using clock                      = std::chrono::steady_clock;
        constexpr int OPENPROF_WINDOW_MS = 1500;
        constexpr int OPENPROF_DRAIN_MS  = 400; // GPU answers arrive a frame or two late

        CHyprSignalListener      g_pre, g_stage;
        bool                     g_on = false, g_draining = false;
        clock::time_point        g_t0{};
        std::vector<std::string> g_lines;
        MONITORID                g_frameMon = MONITOR_INVALID;
        clock::time_point        g_frameStart{};
        uint64_t                 g_overviewNs = 0;
        std::unordered_map<MONITORID, clock::time_point> g_lastPre;

        // GPU time per frame: a timestamp at RENDER_BEGIN and one at
        // RENDER_LAST_MOMENT (EXT_disjoint_timer_query), read back later.
        PFNGLQUERYCOUNTEREXTPROC        pQueryCounter = nullptr;
        PFNGLGETQUERYOBJECTUI64VEXTPROC pGetU64       = nullptr;
        bool                            g_glTried     = false;
        struct SPending {
            GLuint begin = 0, end = 0;
            size_t line  = 0;
            bool   ended = false;
            bool   read  = false; // a later frame has begun: no more marks
            std::vector<std::pair<const char*, GLuint>> marks;
        };
        std::vector<SPending> g_pending;
        std::vector<GLuint>   g_freeQueries;

        double msSince(clock::time_point t) {
            return std::chrono::duration<double, std::milli>(clock::now() - t).count();
        }

        int gpuMHz() {
            static const char* PATH = "/sys/class/drm/card0/device/tile0/gt0/freq0/act_freq";
            int                mhz  = -1;
            if (FILE* f = std::fopen(PATH, "r")) {
                if (std::fscanf(f, "%d", &mhz) != 1)
                    mhz = -1;
                std::fclose(f);
            }
            return mhz;
        }

        bool glReady() {
            if (!g_glTried) {
                g_glTried     = true;
                pQueryCounter = rc<PFNGLQUERYCOUNTEREXTPROC>(eglGetProcAddress("glQueryCounterEXT"));
                pGetU64       = rc<PFNGLGETQUERYOBJECTUI64VEXTPROC>(eglGetProcAddress("glGetQueryObjectui64vEXT"));
            }
            return pQueryCounter && pGetU64;
        }

        GLuint takeQuery() {
            if (g_freeQueries.empty()) {
                std::array<GLuint, 32> ids{};
                glGenQueries(ids.size(), ids.data());
                g_freeQueries.insert(g_freeQueries.end(), ids.begin(), ids.end());
            }
            const GLuint Q = g_freeQueries.back();
            g_freeQueries.pop_back();
            return Q;
        }

        // Context is current: read whatever the GPU has finished.
        void poll() {
            if (!glReady())
                return;
            std::erase_if(g_pending, [](SPending& p) {
                if (!p.ended || !p.read)
                    return false;
                GLuint avail = 0;
                glGetQueryObjectuiv(p.end, GL_QUERY_RESULT_AVAILABLE, &avail);
                if (avail && !p.marks.empty())
                    glGetQueryObjectuiv(p.marks.back().second, GL_QUERY_RESULT_AVAILABLE, &avail);
                if (!avail)
                    return false;
                GLuint64 a = 0, b = 0;
                pGetU64(p.begin, GL_QUERY_RESULT, &a);
                pGetU64(p.end, GL_QUERY_RESULT, &b);
                std::string split;
                GLuint64    prev = a;
                for (const auto& [name, q] : p.marks) {
                    GLuint64 t = 0;
                    pGetU64(q, GL_QUERY_RESULT, &t);
                    split += std::format(" {}={:.2f}", name, (t - prev) / 1e6);
                    prev = t;
                    g_freeQueries.push_back(q);
                }
                // Marks past the end (sections drawn after LAST_MOMENT) extend it.
                const GLuint64 LAST = std::max(b, prev);
                if (!p.marks.empty() && b > prev)
                    split += std::format(" rest={:.2f}", (b - prev) / 1e6);
                if (p.line < g_lines.size())
                    g_lines[p.line] += std::format("  gpu {:6.2f} ms [{} ]", (LAST - a) / 1e6, split);
                g_freeQueries.push_back(p.begin);
                g_freeQueries.push_back(p.end);
                return true;
            });
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
            // Unread queries are abandoned with their lines; the ids go back.
            for (const auto& p : g_pending) {
                g_freeQueries.push_back(p.begin);
                if (p.end)
                    g_freeQueries.push_back(p.end);
                for (const auto& m : p.marks)
                    g_freeQueries.push_back(m.second);
            }
            g_pending.clear();
        }

        // Recording while the window is open, then draining GPU answers for a
        // little longer, then the block is written.
        bool recording() {
            if (!g_on)
                return false;
            if (msSince(g_t0) <= OPENPROF_WINDOW_MS)
                return true;
            if (!g_draining) {
                g_draining = true;
                g_frameMon = MONITOR_INVALID;
            }
            if (!g_pending.empty() && msSince(g_t0) <= OPENPROF_WINDOW_MS + OPENPROF_DRAIN_MS)
                return false;
            g_on       = false;
            g_draining = false;
            flush();
            return false;
        }
    }

    void init() {
        g_pre = Event::bus()->m_events.render.pre.listen([](PHLMONITOR monitor) {
            if (!recording() || !monitor)
                return;
            const auto NOW = clock::now();
            double     gap = -1;
            if (const auto IT = g_lastPre.find(monitor->m_id); IT != g_lastPre.end())
                gap = std::chrono::duration<double, std::milli>(NOW - IT->second).count();
            g_lastPre[monitor->m_id] = NOW;
            g_frameMon               = monitor->m_id;
            g_frameStart             = NOW;
            g_overviewNs             = 0;
            g_lines.push_back(std::format("  +{:7.1f}  mon {} gap {:6.1f}  gpuclk {:4d}", msSince(g_t0), monitor->m_name, gap, gpuMHz()));
        });
        g_stage = Event::bus()->m_events.render.stage.listen([](eRenderStage stage) {
            if (!g_on)
                return;
            if (stage == RENDER_BEGIN) {
                for (auto& p : g_pending)
                    p.read = p.ended;
                poll();
                if (g_draining || g_frameMon == MONITOR_INVALID || g_lines.empty() || !glReady())
                    return;
                SPending p;
                p.begin = takeQuery();
                p.line  = g_lines.size() - 1;
                pQueryCounter(p.begin, GL_TIMESTAMP_EXT);
                g_pending.push_back(p);
            } else if (stage == RENDER_LAST_MOMENT) {
                if (!g_pending.empty() && !g_pending.back().ended && glReady()) {
                    g_pending.back().end   = takeQuery();
                    g_pending.back().ended = true;
                    pQueryCounter(g_pending.back().end, GL_TIMESTAMP_EXT);
                }
            } else if (stage == RENDER_POST) {
                if (g_draining || g_frameMon == MONITOR_INVALID || g_lines.empty())
                    return;
                g_lines.back() += std::format("  frame {:6.2f} ms  overview {:6.2f} ms", msSince(g_frameStart), g_overviewNs / 1e6);
                g_frameMon = MONITOR_INVALID;
            }
        });
    }

    void shutdown() {
        g_pre.reset();
        g_stage.reset();
        // No GL context here: pending answers are dropped, not read.
        flush();
    }

    bool active() {
        return g_on && !g_draining && msSince(g_t0) <= OPENPROF_WINDOW_MS;
    }

    void begin(const char* what) {
        // Read per open, not per load: touch/rm the flag with no reload.
        if (::access("/tmp/hyprdrive-openprof.on", F_OK) != 0)
            return;
        flush(); // a previous window cut short by this open
        g_on       = true;
        g_draining = false;
        g_t0       = clock::now();
        g_lastPre.clear();
        g_frameMon = MONITOR_INVALID;
        g_lines.push_back(std::format("=== open ({}) gpuclk {}", what, gpuMHz()));
    }

    void mark(const char* what) {
        if (active())
            g_lines.push_back(std::format("  +{:7.1f}  {}", msSince(g_t0), what));
    }

    namespace {
        class CGpuMarkElement : public IPassElement {
          public:
            explicit CGpuMarkElement(const char* name) : m_name(name) {}
            std::vector<UP<IPassElement>> draw() override {
                // The frame's pending entry is the last one begun. The overview's
                // later sections draw after RENDER_LAST_MOMENT, so "ended" does
                // not close it to marks; the next RENDER_BEGIN does.
                if (!g_pending.empty() && !g_pending.back().read && glReady()) {
                    const GLuint Q = takeQuery();
                    pQueryCounter(Q, GL_TIMESTAMP_EXT);
                    g_pending.back().marks.emplace_back(m_name, Q);
                }
                return {};
            }
            bool needsLiveBlur() override {
                return false;
            }
            bool needsPrecomputeBlur() override {
                return false;
            }
            const char* passName() override {
                return "CGpuMarkElement";
            }
            ePassElementType type() override {
                return EK_CUSTOM;
            }
            bool undiscardable() override {
                return true;
            }
            bool disableSimplification() override {
                return true;
            }

          private:
            const char* m_name;
        };
    }

    void gpuMark(const char* name) {
        if (active() && g_pHyprRenderer)
            g_pHyprRenderer->m_renderPass.add(makeUnique<CGpuMarkElement>(name));
    }

    void addOverviewRender(uint64_t ns) {
        if (active())
            g_overviewNs += ns;
    }
}
