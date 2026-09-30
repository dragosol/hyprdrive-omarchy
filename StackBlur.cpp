// See StackBlur.hpp.
#include "StackBlur.hpp"

#include "DockTilt.hpp"

#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/gl/GLFramebuffer.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace ScrollOverview::StackBlur {
    namespace {
        SP<Render::GL::CGLFramebuffer> g_fb;
        SP<Render::GL::CGLFramebuffer> g_tmp; // one instant, composited, before it is summed

        // HYPRDRIVE_STACKBLUR_LOG=<file>: per-frame cost of the blur pass, GPU
        // included (it waits for the GPU with glFinish, so only while measuring).
        const char* logPath() {
            static const char* const P = std::getenv("HYPRDRIVE_STACKBLUR_LOG");
            return P;
        }
    }

    bool usable(PHLMONITOR monitor) {
        return monitor && monitor->m_transform == WL_OUTPUT_TRANSFORM_NORMAL;
    }

    void queue(std::vector<SSample> samples, const CBox& bounds) {
        if (samples.empty() || bounds.width < 2 || bounds.height < 2)
            return;
        g_pHyprRenderer->m_renderPass.add(makeUnique<CStackBlurPassElement>(std::move(samples), bounds));
    }

    void shutdown() {
        if (g_fb && Render::GL::g_pHyprOpenGL) {
            Render::GL::g_pHyprOpenGL->makeEGLCurrent();
            g_fb->release();
            if (g_tmp)
                g_tmp->release();
        }
        g_fb.reset();
        g_tmp.reset();
    }

    CStackBlurPassElement::CStackBlurPassElement(std::vector<SSample> samples, const CBox& bounds) : m_samples(std::move(samples)), m_bounds(bounds) {}

    std::optional<CBox> CStackBlurPassElement::boundingBox() {
        const auto MONITOR = g_pHyprRenderer->m_renderData.pMonitor;
        if (!MONITOR)
            return std::nullopt;
        return m_bounds.copy().scale(1.F / MONITOR->m_scale).round();
    }

    std::vector<UP<IPassElement>> CStackBlurPassElement::draw() {
        const auto MONITOR = g_pHyprRenderer->m_renderData.pMonitor.lock();
        const auto PREV    = g_pHyprRenderer->m_renderData.currentFB;
        if (!MONITOR || !PREV || !usable(MONITOR))
            return {};

        if (logPath())
            glFinish(); // drain what was queued before us, so only the blur is timed
        const auto T0 = std::chrono::steady_clock::now();

        const auto SIZE = MONITOR->m_transformedSize;
        if (!g_fb || g_fb->m_size != SIZE) {
            if (g_fb)
                g_fb->release();
            g_fb = makeShared<Render::GL::CGLFramebuffer>("hyprdrive-stack-blur");
            if (!g_fb->alloc(sc<int>(SIZE.x), sc<int>(SIZE.y))) {
                g_fb.reset();
                return {};
            }
        }

        // Sum the samples offscreen...
        g_fb->bind();
        Render::GL::g_pHyprOpenGL->scissor(nullptr);
        glClearColor(0.F, 0.F, 0.F, 0.F);
        glClear(GL_COLOR_BUFFER_BIT);
        // Per instant: composite its pieces (text OVER its pill -- summing them
        // made dark ink vanish on a light pill), then add that instant into the
        // accumulator at 1/N: the mean of N composited frames, the same blur as
        // before on a dark theme. Both passes clipped to the fan's bounds.
        if (!g_tmp || g_tmp->m_size != SIZE) {
            if (g_tmp)
                g_tmp->release();
            g_tmp = makeShared<Render::GL::CGLFramebuffer>("hyprdrive-stack-blur-sample");
            if (!g_tmp->alloc(sc<int>(SIZE.x), sc<int>(SIZE.y))) {
                g_tmp.reset();
                PREV->bind();
                return {};
            }
        }
        int groups = 0;
        for (const auto& S : m_samples)
            groups = std::max(groups, S.group + 1);
        const float WEIGHT = 1.F / sc<float>(std::max(groups, 1));
        const CBox  CLIP   = m_bounds.copy().round();
        for (int g = 0; g < groups; ++g) {
            g_tmp->bind();
            Render::GL::g_pHyprOpenGL->scissor(CLIP);
            glClearColor(0.F, 0.F, 0.F, 0.F);
            glClear(GL_COLOR_BUFFER_BIT);
            for (const auto& S : m_samples)
                if (S.group == g)
                    DockTilt::drawAdditive(S.tex, S.box, S.alpha, false, &CLIP);
            g_fb->bind();
            if (const auto TEX = g_tmp->getTexture(); TEX && TEX->ok())
                DockTilt::drawAdditive(TEX, CBox{0, 0, SIZE.x, SIZE.y}, WEIGHT, true, &CLIP);
        }
        Render::GL::g_pHyprOpenGL->scissor(nullptr);

        // ...then lay the average over the scene, where the fan is.
        PREV->bind();
        CRegion damage{g_pHyprRenderer->m_renderData.damage};
        damage.intersect(m_bounds);
        if (!damage.empty() && g_fb->getTexture() && g_fb->getTexture()->ok())
            Render::GL::g_pHyprOpenGL->renderTexture(g_fb->getTexture(), CBox{0, 0, SIZE.x, SIZE.y}, {.damage = &damage, .a = 1.F});

        if (logPath()) {
            glFinish();
            const float MS = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - T0).count();
            if (FILE* f = std::fopen(logPath(), "a")) {
                std::fprintf(f, "blur samples=%zu ms=%.3f\n", m_samples.size(), MS);
                std::fclose(f);
            }
        }
        return {};
    }
}
