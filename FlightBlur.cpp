// See FlightBlur.hpp.
#include "FlightBlur.hpp"

#include "Config.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/pass/Pass.hpp>
#include <hyprland/src/render/pass/SurfacePassElement.hpp>
#include <hyprland/src/render/pass/TransformedWindowPassElement.hpp>
#include <hyprland/src/render/transformer/Transformer.hpp>

#include <algorithm>
#include <cmath>

namespace ScrollOverview::FlightBlur {
    namespace {
        // Room around the window for what the flight draws outside it: the card
        // shadow and the border. Logical px. The composite only samples inside
        // the box it is given, so anything past this is cut, not blurred.
        constexpr double MARGIN = 48.0;

        bool moved(const CBox& a, const CBox& b) {
            return std::abs(a.x - b.x) > 0.5 || std::abs(a.y - b.y) > 0.5 || std::abs(a.w - b.w) > 0.5 || std::abs(a.h - b.h) > 0.5;
        }

        bool valid(const CBox& b) {
            return b.w >= 1.0 && b.h >= 1.0;
        }

        // The renderer's own recipe (MotionBlurTransformer::amendTransformedRenderData):
        // `drawn` is the box it is about to composite, `cur` the geometry that box
        // was built around; the previous box is the same box, placed and scaled the
        // way `prev` relates to `cur`.
        SMotionBlurData motionFor(const CBox& drawn, const CBox& prev, const CBox& cur, int n) {
            const Vector2D RELPOS = drawn.pos() - cur.pos();
            const Vector2D SCALE  = prev.size() / cur.size();

            SMotionBlurData mb;
            mb.enabled  = true;
            mb.previous = CBox{prev.pos() + RELPOS * SCALE, drawn.size() * SCALE};
            mb.current  = drawn;
            mb.samples  = n;
            return mb;
        }

        CBox logical(const CBox& renderPx, float scale) {
            return renderPx.copy().scale(1.0 / std::max(scale, 0.01F));
        }

        class CFlightBlurTransformer : public Render::IWindowTransformer {
          public:
            SP<Render::IFramebuffer> transform(SP<Render::IFramebuffer> in) override {
                return in;
            }

            void amendTransformedRenderData(const CBox& currentBox, SMotionBlurData* data) override {
                const int N = samples();
                if (!data || !m_armed || N < 2 || !valid(m_prev) || !valid(m_cur))
                    return;
                *data = motionFor(currentBox, m_prev, m_cur, N);
            }

            // monitor-local LOGICAL, like the currentBox the renderer passes
            CBox m_prev, m_cur;
            bool m_armed = false;
        };

        CFlightBlurTransformer* transformerOf(const PHLWINDOW& window) {
            for (const auto& t : window->m_transformers)
                if (auto* mine = dynamic_cast<CFlightBlurTransformer*>(t.get()))
                    return mine;
            return nullptr;
        }

        void detach(const PHLWINDOW& window) {
            std::erase_if(window->m_transformers, [](const auto& t) { return dynamic_cast<CFlightBlurTransformer*>(t.get()) != nullptr; });
        }
    }

    int samples() {
        return std::clamp(Config::getValue<int>("plugin:hyprdrive:dock:minimize_motion_blur"), 0, 32);
    }

    bool drawBlurred(PHLMONITOR monitor, const PHLWINDOW& window, const CBox& prevBox, const CBox& box, const std::function<void()>& draw) {
        const int N = samples();
        if (N < 2 || !monitor || !window || !valid(prevBox) || !valid(box) || !moved(prevBox, box) || monitor->m_transform != WL_OUTPUT_TRANSFORM_NORMAL)
            return false;

        auto nested = makeUnique<Render::CRenderPass>();
        {
            auto restore = g_pHyprRenderer->redirectPass(nested.get());
            draw();
        }
        if (nested->empty())
            return true;

        const float SCALE = monitor->m_scale;
        const CBox  PREV  = logical(prevBox, SCALE);
        const CBox  CUR   = logical(box, SCALE);
        CBox        drawn = CUR.copy().expand(MARGIN);

        g_pHyprRenderer->addPassElement(makeUnique<CTransformedWindowPassElement>(CTransformedWindowPassElement::SData{
            .pass       = std::move(nested),
            .window     = window,
            .currentBox = drawn,
            .blurBox    = box.copy().round(),
            .blur       = false, // the surfaces frost themselves, inside the pass
            .motionBlur = motionFor(drawn, PREV, CUR, N),
        }));
        return true;
    }

    void recordDriven(PHLMONITOR monitor, const PHLWINDOW& window, const CBox& prevBox, const CBox& box) {
        if (!monitor || !window)
            return;

        const bool WANT = samples() >= 2 && valid(prevBox) && valid(box) && monitor->m_transform == WL_OUTPUT_TRANSFORM_NORMAL;
        auto*      t    = transformerOf(window);
        if (!WANT) {
            if (t)
                t->m_armed = false;
            return;
        }
        if (!t) {
            window->m_transformers.emplace_back(makeUnique<CFlightBlurTransformer>());
            t = sc<CFlightBlurTransformer*>(window->m_transformers.back().get());
        }

        const float SCALE = monitor->m_scale;
        t->m_prev         = logical(prevBox, SCALE);
        t->m_cur          = logical(box, SCALE);
        t->m_armed        = moved(prevBox, box);

        // The smear reaches back to where the window was: repaint that too.
        CBox reach = t->m_prev.copy();
        reach      = CBox{std::min(reach.x, t->m_cur.x), std::min(reach.y, t->m_cur.y), 0, 0};
        reach.w    = std::max(t->m_prev.x + t->m_prev.w, t->m_cur.x + t->m_cur.w) - reach.x;
        reach.h    = std::max(t->m_prev.y + t->m_prev.h, t->m_cur.y + t->m_cur.h) - reach.y;
        g_pHyprRenderer->damageBox(reach.expand(MARGIN).translate(monitor->m_position));
    }

    void sweep(const std::function<bool(const PHLWINDOW&)>& stillFlying) {
        for (const auto& window : Desktop::windowState()->windows()) {
            if (window && !window->m_transformers.empty() && transformerOf(window) && !stillFlying(window))
                detach(window);
        }
    }

    void detachAll() {
        for (const auto& window : Desktop::windowState()->windows())
            if (window && !window->m_transformers.empty())
                detach(window);
    }
}
