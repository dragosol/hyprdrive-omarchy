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
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/helpers/time/Time.hpp>

#include <chrono>
#include <ranges>

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
                if (!data || !m_armed || m_samples < 2 || !valid(m_prev) || !valid(m_cur))
                    return;
                *data = motionFor(currentBox, m_prev, m_cur, m_samples);
            }

            // monitor-local LOGICAL, like the currentBox the renderer passes
            CBox            m_prev, m_cur;
            bool            m_armed   = false;
            int             m_samples = 0;
            Time::steady_tp m_fedAt   = {};
        };

        // Nobody fed it for this long: the motion is over, take it off (the
        // window would otherwise keep rendering through an offscreen buffer).
        constexpr float STALE_MS = 150.F;

        // The layout tracker's memory: where each moving window was drawn at
        // the last frame of its monitor.
        std::vector<std::pair<PHLWINDOWREF, CBox>> g_lastLayoutBox;

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

    bool drawBlurred(PHLMONITOR monitor, const PHLWINDOW& window, const CBox& prevBox, const CBox& box, const std::function<void()>& draw, int samplesOverride) {
        const int N = samplesOverride >= 0 ? samplesOverride : samples();
        if (N < 2 || !monitor || !valid(prevBox) || !valid(box) || !moved(prevBox, box) || monitor->m_transform != WL_OUTPUT_TRANSFORM_NORMAL)
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
        t->m_samples      = samples();
        t->m_fedAt        = Time::steadyNow();

        // The smear reaches back to where the window was: repaint that too.
        CBox reach = t->m_prev.copy();
        reach      = CBox{std::min(reach.x, t->m_cur.x), std::min(reach.y, t->m_cur.y), 0, 0};
        reach.w    = std::max(t->m_prev.x + t->m_prev.w, t->m_cur.x + t->m_cur.w) - reach.x;
        reach.h    = std::max(t->m_prev.y + t->m_prev.h, t->m_cur.y + t->m_cur.h) - reach.y;
        g_pHyprRenderer->damageBox(reach.expand(MARGIN).translate(monitor->m_position));
    }

    void sweep(const std::function<bool(const PHLWINDOW&)>& stillFlying) {
        const auto NOW = Time::steadyNow();
        for (const auto& window : Desktop::windowState()->windows()) {
            if (!window || window->m_transformers.empty())
                continue;
            const auto* T = transformerOf(window);
            if (!T || (stillFlying && stillFlying(window)))
                continue;
            if (std::chrono::duration<float, std::milli>(NOW - T->m_fedAt).count() > STALE_MS)
                detach(window);
        }
    }

    int layoutSamples() {
        return std::clamp(Config::getValue<int>("plugin:hyprdrive:layout_motion_blur"), 0, 32);
    }

    void trackLayoutMotion(PHLMONITOR monitor, const std::function<bool(const PHLWINDOW&)>& skip) {
        if (!monitor)
            return;
        std::erase_if(g_lastLayoutBox, [](const auto& e) { return !e.first.lock(); });

        const int N = layoutSamples();
        if (N >= 2 && monitor->m_transform == WL_OUTPUT_TRANSFORM_NORMAL) {
            // An interactive move/resize is the user's hand, not the layout's.
            const auto DRAGTARGET = g_layoutManager && g_layoutManager->dragController() ? g_layoutManager->dragController()->target() : nullptr;

            for (const auto& window : Desktop::windowState()->windows()) {
                if (!window || !window->m_isMapped || window->isHidden() || window->m_monitor != monitor || !window->m_workspace || !window->m_workspace->isVisible())
                    continue;
                const auto IT = std::ranges::find_if(g_lastLayoutBox, [&](const auto& e) { return e.first.lock() == window; });
                const bool ANIMATING = window->positionAnimation()->isBeingAnimated() || window->sizeAnimation()->isBeingAnimated();
                if (!ANIMATING || (skip && skip(window)) || (DRAGTARGET && window->layoutTarget() == DRAGTARGET)) {
                    if (IT != g_lastLayoutBox.end())
                        g_lastLayoutBox.erase(IT);
                    continue;
                }

                // Monitor-local logical, like the renderer's currentBox.
                const CBox NOWBOX{window->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT) - monitor->m_position,
                                  window->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT)};
                if (IT == g_lastLayoutBox.end()) {
                    g_lastLayoutBox.emplace_back(window, NOWBOX);
                    continue;
                }
                const CBox RAWPREV = IT->second;
                IT->second         = NOWBOX;
                if (!valid(RAWPREV) || !valid(NOWBOX) || !moved(RAWPREV, NOWBOX))
                    continue;

                // HALF AS LONG (user, 2026-09-30: "make that one half as long
                // lasting, the fade starting a lot earlier"). The smear is full
                // at the start of the animation, where the window moves fastest,
                // and shrinks to nothing by its halfway point: the previous box
                // is pulled toward the current one by the animation's own
                // progress, smoothstepped so it eases out rather than stopping.
                const float PCT  = std::max(window->positionAnimation()->isBeingAnimated() ? window->positionAnimation()->getPercent() : 0.F,
                                            window->sizeAnimation()->isBeingAnimated() ? window->sizeAnimation()->getPercent() : 0.F);
                const float T    = std::clamp(PCT / 0.5F, 0.F, 1.F);
                const float KEEP = 1.F - T * T * (3.F - 2.F * T);
                if (KEEP < 0.03F)
                    continue;
                const CBox PREV{NOWBOX.x + (RAWPREV.x - NOWBOX.x) * KEEP, NOWBOX.y + (RAWPREV.y - NOWBOX.y) * KEEP, NOWBOX.w + (RAWPREV.w - NOWBOX.w) * KEEP,
                                NOWBOX.h + (RAWPREV.h - NOWBOX.h) * KEEP};
                if (!moved(PREV, NOWBOX))
                    continue;

                auto* t = transformerOf(window);
                if (!t) {
                    window->m_transformers.emplace_back(makeUnique<CFlightBlurTransformer>());
                    t = sc<CFlightBlurTransformer*>(window->m_transformers.back().get());
                }
                t->m_prev    = PREV;
                t->m_cur     = NOWBOX;
                t->m_armed   = true;
                t->m_samples = N;
                t->m_fedAt   = Time::steadyNow();

                CBox reach{std::min(PREV.x, NOWBOX.x), std::min(PREV.y, NOWBOX.y), 0, 0};
                reach.w = std::max(PREV.x + PREV.w, NOWBOX.x + NOWBOX.w) - reach.x;
                reach.h = std::max(PREV.y + PREV.h, NOWBOX.y + NOWBOX.h) - reach.y;
                g_pHyprRenderer->damageBox(reach.expand(MARGIN).translate(monitor->m_position));
            }
        } else
            g_lastLayoutBox.clear();

        // Anything nobody fed this frame and the last few comes off.
        sweep(skip);
    }

    CSuspend::CSuspend(const PHLWINDOW& window) : m_window(window) {
        if (!window)
            return;
        auto& list = window->m_transformers;
        for (auto it = list.begin(); it != list.end();) {
            if (dynamic_cast<CFlightBlurTransformer*>(it->get())) {
                m_held.emplace_back(std::move(*it));
                it = list.erase(it);
            } else
                ++it;
        }
    }

    CSuspend::~CSuspend() {
        if (m_held.empty())
            return;
        if (const auto WINDOW = m_window.lock())
            for (auto& t : m_held)
                WINDOW->m_transformers.emplace_back(std::move(t));
    }

    void detachAll() {
        for (const auto& window : Desktop::windowState()->windows())
            if (window && !window->m_transformers.empty())
                detach(window);
    }
}
