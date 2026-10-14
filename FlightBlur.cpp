// See FlightBlur.hpp.
// FIRST, as in OverviewInternal.hpp: CAlgorithm::m_tiled is private, and the
// layout blur has to ask whether a desktop scrolls. std headers clean first so
// the define covers only the header whose guard it claims.
#include <sstream>
#include <functional>
#include <memory>
#include <chrono>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>
#include <any>
#include <cstdio>
#include <optional>
#include <expected>
#include <unordered_map>
#include <map>
#include <set>
#include <variant>
#include <span>
#include <format>
#define private public
#define protected public // IHyprRenderer::shouldBlur, as Window.cpp reads it
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/pass/Pass.hpp> // drawBlurred reads/moves the nested pass's elements
#include <hyprland/src/layout/algorithm/Algorithm.hpp>
#undef protected
#undef private
#include "FlightBlur.hpp"
#include <string_view>
#include <array>

#include "Config.hpp"
#include "MotionSpacing.hpp"
#include "OverviewPassElement.hpp"

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
#include <hyprland/src/managers/input/UnifiedWorkspaceSwipeGesture.hpp>
#include <hyprland/src/layout/space/Space.hpp>
#include <hyprland/src/layout/algorithm/tiled/scrolling/ScrollingAlgorithm.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/event/EventBus.hpp>
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

        // Over a pixel, not half of one: boxes are rounded, and rounding x and
        // w separately wobbles a still box by 1px from frame to frame -- which
        // sent single frames down the blurred path (2026-10-10 recording).
        bool moved(const CBox& a, const CBox& b) {
            return std::abs(a.x - b.x) > 1.0 || std::abs(a.y - b.y) > 1.0 || std::abs(a.w - b.w) > 1.0 || std::abs(a.h - b.h) > 1.0;
        }

        bool valid(const CBox& b) {
            return b.w >= 1.0 && b.h >= 1.0;
        }

        // The renderer's own recipe (MotionBlurTransformer::amendTransformedRenderData):
        // `drawn` is the box it is about to composite, `cur` the geometry that box
        // was built around; the previous box is the same box, placed and scaled the
        // way `prev` relates to `cur`.
        //
        // EVERY smear goes through here, and here it is held to MotionSpacing's
        // rule: copies never more than 2 device px apart, on any edge -- more
        // samples for a long motion, a shorter trail past the shader's 64. `n`
        // is the configured count (now the minimum), `maxN` a cost ceiling,
        // `scale` the monitor's (the rule is in device px).
        SMotionBlurData motionFor(const CBox& drawn, const CBox& prevIn, const CBox& cur, int n, float scale, int maxN = MotionSpacing::MAX_SAMPLES) {
            const auto PLAN = MotionSpacing::plan(MotionSpacing::SBox{prevIn.x, prevIn.y, prevIn.w, prevIn.h}, MotionSpacing::SBox{cur.x, cur.y, cur.w, cur.h}, scale, n, maxN);
            const CBox prev{PLAN.previous.x, PLAN.previous.y, PLAN.previous.w, PLAN.previous.h};

            const Vector2D RELPOS = drawn.pos() - cur.pos();
            const Vector2D SCALE  = prev.size() / cur.size();

            SMotionBlurData mb;
            mb.enabled  = true;
            mb.previous = CBox{prev.pos() + RELPOS * SCALE, drawn.size() * SCALE};
            mb.current  = drawn;
            mb.samples  = PLAN.samples;
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
                *data = motionFor(currentBox, m_prev, m_cur, m_samples, m_scale, m_maxSamples);
            }

            // monitor-local LOGICAL, like the currentBox the renderer passes
            CBox            m_prev, m_cur;
            bool            m_armed   = false;
            int             m_samples    = 0;
            int             m_maxSamples = MotionSpacing::MAX_SAMPLES;
            float           m_scale      = 1.F; // the monitor's, for the spacing rule
            Time::steady_tp m_fedAt   = {};
        };

        // Nobody fed it for this long: the motion is over, take it off (the
        // window would otherwise keep rendering through an offscreen buffer).
        constexpr float STALE_MS = 150.F;

        // The layout tracker's memory: where each moving window was drawn at
        // the last frame of its monitor.
        struct SLastBox {
            PHLWINDOWREF    window;
            CBox            box;
            Time::steady_tp at;
        };
        std::vector<SLastBox> g_lastLayoutBox;

        // Every window's last floating state, and when it last flipped. A
        // float<->tile toggle animates the window from its floating box into a
        // tile slot (or back): a change of MODE, not layout motion, and smearing
        // it drew its trail behind/between the neighbours as glitchy strips
        // (user, 2026-10-01: "a motion blur artifact in that mpv retiling").
        struct SModeSeen {
            PHLWINDOWREF    window;
            bool            floating = false;
            Time::steady_tp flippedAt{};
        };
        std::vector<SModeSeen> g_modeSeen;
        constexpr float        MODE_FLIP_QUIET_MS = 1000.F;

        bool justFlippedMode(const PHLWINDOW& window, Time::steady_tp now) {
            auto it = std::ranges::find_if(g_modeSeen, [&](const auto& e) { return e.window.lock() == window; });
            if (it == g_modeSeen.end()) {
                g_modeSeen.push_back({window, window->m_isFloating, {}});
                return false;
            }
            if (it->floating != window->m_isFloating) {
                it->floating  = window->m_isFloating;
                it->flippedAt = now;
            }
            return it->flippedAt.time_since_epoch().count() != 0 &&
                std::chrono::duration<float, std::milli>(now - it->flippedAt).count() < MODE_FLIP_QUIET_MS;
        }

        // A scrolling desktop's tape is blurred only above this speed: a
        // flick, not a drift. Logical px per second.
        constexpr double FLICK_PX_PER_S = 12000.0; // user, 2026-09-30: 2500 then 5000 still blurred too early -- an ordinary snap of the tape peaks near 9000

        // (How much of a scroll or swipe is smeared -- half, user 2026-09-30:
        // "halve the blur for the tape and desktop swipes" -- is now the
        // flick / swipe / scratchpad defaults in FlightBlur.hpp's SITES.)

        CFlightBlurTransformer* transformerOf(const PHLWINDOW& window) {
            for (const auto& t : window->m_transformers)
                if (auto* mine = dynamic_cast<CFlightBlurTransformer*>(t.get()))
                    return mine;
            return nullptr;
        }

        // While our transformer is on a window, Hyprland renders it through its
        // transformed-window path, and that path draws the window's blur in one
        // combined textured draw with the window (blur + matte + motion blur)
        // that veils it: half-strength text, no border, for every frame the
        // transformer was attached (user, 2026-10-10: "literally every time
        // there's a movement ... there's a fucking glitch"). So Hyprland's blur
        // is held off for the window (noBlur at LAYOUT priority, the lowest: a
        // user's own rule still wins) and RENDER_PRE_WINDOW queues the same
        // frost as a separate element behind it (onPreWindow).
        void holdHyprlandBlur(const PHLWINDOW& window, bool hold) {
            if (!window || !window->m_ruleApplicator)
                return;
            if (hold)
                window->m_ruleApplicator->noBlur().set(true, Desktop::Types::PRIORITY_LAYOUT);
            else
                window->m_ruleApplicator->noBlur().unset(Desktop::Types::PRIORITY_LAYOUT);
        }

        CFlightBlurTransformer* attach(const PHLWINDOW& window) {
            window->m_transformers.emplace_back(makeUnique<CFlightBlurTransformer>());
            holdHyprlandBlur(window, true);
            return sc<CFlightBlurTransformer*>(window->m_transformers.back().get());
        }

        void detach(const PHLWINDOW& window) {
            std::erase_if(window->m_transformers, [](const auto& t) { return dynamic_cast<CFlightBlurTransformer*>(t.get()) != nullptr; });
            holdHyprlandBlur(window, false);
        }

        // The frost a window gets behind it while it moves: Hyprland's cached
        // background blur where Hyprland itself would read that for this window
        // standing still, a live blur where hyprdrive blocks the cache (a
        // dragged or floating tile) or Hyprland would blur live -- so moving
        // and still look the same. box: monitor-local physical px.
        void queueFrost(const PHLWINDOW& window, const CBox& box, int rounding, float roundingPower, float alpha, bool live) {
            if (box.w < 2 || box.h < 2 || alpha <= 0.F)
                return;
            COverviewBlurPassElement::SData frost;
            frost.box           = box;
            frost.rounding      = rounding;
            frost.roundingPower = roundingPower;
            frost.alpha         = std::clamp(alpha, 0.F, 1.F);
            frost.xray          = false;
            frost.hyprlandCache = !live && window && g_pHyprRenderer->shouldUseNewBlurOptimizations(nullptr, window);
            g_pHyprRenderer->addPassElement(makeUnique<COverviewBlurPassElement>(frost));
        }

        // Windows already given their frost this frame. RENDER_PRE_WINDOW fires
        // for each pass a window is rendered in (main, then popups); a frost
        // queued in the popup pass would land on top of the windows after it.
        std::vector<Desktop::View::CWindow*> g_frostedThisFrame;
        std::vector<CHyprSignalListener>     g_frostListeners;

        void onPreWindow() {
            const auto WINDOW  = g_pHyprRenderer->m_renderData.currentWindow.lock();
            const auto MONITOR = g_pHyprRenderer->m_renderData.pMonitor.lock();
            if (!WINDOW || !MONITOR || !transformerOf(WINDOW))
                return;
            if (std::ranges::find(g_frostedThisFrame, WINDOW.get()) != g_frostedThisFrame.end())
                return;
            g_frostedThisFrame.push_back(WINDOW.get());

            // Would Hyprland have blurred it, our hold aside?
            holdHyprlandBlur(WINDOW, false);
            const bool WANT = g_pHyprRenderer->shouldBlur(WINDOW);
            holdHyprlandBlur(WINDOW, true);
            if (!WANT)
                return;

            // renderWindow's own box and fade (Renderer.cpp, renderWindow).
            const auto WS     = WINDOW->m_workspace;
            const auto OFFSET = WS && !WINDOW->m_pinned ? WS->m_renderOffset->value() : Vector2D{};
            const auto POS    = WINDOW->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT) + OFFSET + WINDOW->m_floatingOffset - MONITOR->m_position;
            CBox       box{POS, WINDOW->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT)};
            box.scale(MONITOR->m_scale).round();
            const float FADE  = WINDOW->alphaValue(Desktop::View::WINDOW_ALPHA_FADE) * WINDOW->alphaValue(Desktop::View::WINDOW_ALPHA_FULLSCREEN) *
                WINDOW->alphaValue(Desktop::View::WINDOW_ALPHA_LAYOUT) * (WINDOW->m_pinned || !WS ? 1.F : WS->m_alpha->value());
            const int ROUND = sc<int>(std::max(WINDOW->rounding() * MONITOR->m_scale - 1.F, 0.F));
            queueFrost(WINDOW, box, ROUND, WINDOW->roundingPower(), FADE, false);
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

        // BLUR FLICKER (user, 2026-10-09: "when I am in overview or using 3
        // finger gesture to switch desktops, blur glitches on and off"). The
        // nested pass is drawn into an offscreen buffer cleared to transparent,
        // so any blur INSIDE it frosts nothing: a moving window came out clear
        // on every frame the smear was armed and frosted on every frame it was
        // not. Blur has to see the real frame, outside the nested pass:
        //   - a window's own surface blur becomes a frost element of its own,
        //     queued BEHIND the element at the window's box. NOT the element's
        //     blur/blurBox: Hyprland composites those with the window in one
        //     textured draw (blur + matte + motion blur), and that draw veiled
        //     the window -- text at half strength, border gone, for exactly the
        //     frames the smear was on (user's recording, 2026-10-10). The frost
        //     reads what the window would have read standing still: Hyprland's
        //     cached background blur, or live where hyprdrive blocks that.
        //   - a frosted plate (the search pane, dock chrome) moves out to the
        //     main pass, ahead of the element, so it frosts what is under it.
        bool  surfaceBlur = false;
        bool  blurLive    = false;
        int   blurRound   = 0;
        float blurPower   = 2.F;
        float blurA       = 1.F;
        auto& elements    = nested->m_passElements;
        for (auto it = elements.begin(); it != elements.end();) {
            auto* EL = it->element.get();
            if (auto* surface = dynamic_cast<CSurfacePassElement*>(EL); surface && surface->m_data.blur) {
                surface->m_data.blur = false;
                blurLive             = blurLive || surface->m_data.blockBlurOptimization;
                if (!surfaceBlur || surface->m_data.mainSurface) {
                    blurRound = std::max(surface->m_data.rounding - 1, 0);
                    blurPower = surface->m_data.roundingPower;
                    blurA     = surface->m_data.fadeAlpha;
                }
                surfaceBlur = true;
            } else if (dynamic_cast<COverviewBlurPassElement*>(EL)) {
                g_pHyprRenderer->addPassElement(std::move(it->element));
                it = elements.erase(it);
                continue;
            }
            ++it;
        }
        if (nested->empty())
            return true;

        const float SCALE = monitor->m_scale;
        const CBox  PREV  = logical(prevBox, SCALE);
        const CBox  CUR   = logical(box, SCALE);
        CBox        drawn = CUR.copy().expand(MARGIN);

        if (surfaceBlur)
            queueFrost(window, box.copy().round(), blurRound, blurPower, blurA, blurLive);

        g_pHyprRenderer->addPassElement(makeUnique<CTransformedWindowPassElement>(CTransformedWindowPassElement::SData{
            .pass       = std::move(nested),
            .window     = window,
            .currentBox = drawn,
            .blurBox    = box.copy().round(),
            .blur       = false, // see queueFrost: never Hyprland's combined blur
            .motionBlur = motionFor(drawn, PREV, CUR, N, SCALE),
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
        if (!t)
            t = attach(window);

        const float SCALE = monitor->m_scale;
        t->m_prev         = logical(prevBox, SCALE);
        t->m_cur          = logical(box, SCALE);
        t->m_armed        = moved(prevBox, box);
        t->m_samples      = samples();
        t->m_maxSamples   = MotionSpacing::MAX_SAMPLES;
        t->m_scale        = SCALE;
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
            auto* T = transformerOf(window);
            if (!T || (stillFlying && stillFlying(window)))
                continue;
            const float AGE = std::chrono::duration<float, std::milli>(NOW - T->m_fedAt).count();
            // Not fed this frame: no smear from here on, whatever it held.
            if (AGE > 20.F)
                T->m_armed = false;
            if (AGE > STALE_MS)
                detach(window);
        }
    }

    void reset(const PHLWINDOW& window) {
        if (window)
            if (auto* T = transformerOf(window))
                T->m_armed = false;
    }

    namespace {
        float smoothstep01(float t) {
            const float T = std::clamp(t, 0.F, 1.F);
            return T * T * (3.F - 2.F * T);
        }

        // How much of the smear is left at `p` for each fade shape. "early" is
        // the house fade (HALF AS LONG, user 2026-09-30); unknown names read as
        // it too, so a typo in a config never switches a blur off.
        float envelope(std::string_view fade, float p) {
            if (fade == "constant")
                return 1.F;
            if (fade == "full")
                return 1.F - smoothstep01(p);
            if (fade == "late")
                return p < 0.5F ? 1.F : 1.F - smoothstep01((p - 0.5F) / 0.5F);
            if (fade == "linear")
                return 1.F - std::clamp(p, 0.F, 1.F);
            return 1.F - smoothstep01(p / 0.5F);
        }

        constexpr size_t SITE_COUNT = std::size(SITES);
    }

    float amount(ESite site, float progress) {
        const auto I = sc<size_t>(site);
        if (I >= SITE_COUNT)
            return 0.F;
        if (!Config::g_pluginKeysRegistered)
            return SITES[I].strength * envelope(SITES[I].fade, progress);

        static auto& ON = Config::valueRef<Hyprlang::INT>("plugin:hyprdrive:motion_blur:enabled");
        if (!*ON)
            return 0.F;

        // One cached handle per key: this runs per window per frame.
        static std::array<CConfigValue<Hyprlang::FLOAT>*, SITE_COUNT>  STRENGTH{};
        static std::array<CConfigValue<std::string>*, SITE_COUNT>      FADE{}; // std::string: see Motion.cpp
        if (!STRENGTH[I]) {
            STRENGTH[I] = &Config::valueRef<Hyprlang::FLOAT>(std::format("plugin:hyprdrive:motion_blur:{}:strength", SITES[I].name));
            FADE[I]     = &Config::valueRef<std::string>(std::format("plugin:hyprdrive:motion_blur:{}:fade", SITES[I].name));
        }
        const float STRENGTHV = std::clamp(sc<float>(**STRENGTH[I]), 0.F, 1.F);
        const std::string& FADEV = **FADE[I];
        return STRENGTHV * envelope(FADEV.empty() ? std::string_view{SITES[I].fade} : std::string_view{FADEV}, progress);
    }

    CBox shape(ESite site, const CBox& prev, const CBox& cur, float progress) {
        const float KEEP = amount(site, progress);
        if (KEEP < 0.03F)
            return cur;
        return CBox{cur.x + (prev.x - cur.x) * KEEP, cur.y + (prev.y - cur.y) * KEEP, cur.w + (prev.w - cur.w) * KEEP, cur.h + (prev.h - cur.h) * KEEP};
    }

    CBox fadeMotion(const CBox& prev, const CBox& cur, float progress) {
        const float T    = std::clamp(progress / 0.5F, 0.F, 1.F);
        const float KEEP = 1.F - T * T * (3.F - 2.F * T);
        if (KEEP < 0.03F)
            return cur;
        return CBox{cur.x + (prev.x - cur.x) * KEEP, cur.y + (prev.y - cur.y) * KEEP, cur.w + (prev.w - cur.w) * KEEP, cur.h + (prev.h - cur.h) * KEEP};
    }

    int layoutSamples() {
        return std::clamp(Config::getValue<int>("plugin:hyprdrive:layout_motion_blur"), 0, 32);
    }

    void trackLayoutMotion(PHLMONITOR monitor, const std::function<bool(const PHLWINDOW&)>& skip) {
        if (!monitor)
            return;
        std::erase_if(g_lastLayoutBox, [](const auto& e) { return !e.window.lock(); });
        std::erase_if(g_modeSeen, [](const auto& e) { return !e.window.lock(); });

        // A TRACKPAD WORKSPACE SWIPE (user, 2026-09-30: "add motion blur when
        // swiping through workspaces with the trackpad"): while the fingers
        // are down, and while the release animation carries the workspaces home
        // after them. An ordinary workspace switch stays sharp.
        static bool            s_wasSwiping = false;
        static Time::steady_tp s_swipeEndedAt{};
        const bool             SWIPING = g_pUnifiedWorkspaceSwipe && g_pUnifiedWorkspaceSwipe->isGestureInProgress();
        if (s_wasSwiping && !SWIPING)
            s_swipeEndedAt = Time::steadyNow();
        // The frame a swipe starts or ends can re-base the workspaces' offsets
        // in one step: nothing moved, and it must not be smeared as if it had.
        // Start every window's history over and take any smear off.
        if (s_wasSwiping != SWIPING) {
            for (const auto& e : g_lastLayoutBox)
                if (const auto W = e.window.lock())
                    if (auto* t = transformerOf(W))
                        t->m_armed = false;
            g_lastLayoutBox.clear();
        }
        s_wasSwiping         = SWIPING;
        const bool SWIPEHOME = !SWIPING && s_swipeEndedAt.time_since_epoch().count() != 0 &&
            std::chrono::duration<float, std::milli>(Time::steadyNow() - s_swipeEndedAt).count() < 1000.F;

        // A SCRATCHPAD UNDER THE FINGERS (user, 2026-10-08: "when i do the
        // gesture (4 finger up/down) to show scratchpad there is no motion
        // blur"). Hyprland's special-workspace gesture drives the render offset
        // by hand each update -- set, not animated -- so isBeingAnimated() was
        // false for the whole finger-driven part and only the release could
        // blur. A special workspace whose offset MOVED since this monitor's last
        // frame is sliding too, exactly as a desktop swipe is under the fingers.
        struct SOffsetSeen {
            PHLWORKSPACEREF ws;
            Vector2D        off;
            uint64_t        call  = 0;
            bool            moved = false;
        };
        static std::vector<SOffsetSeen> s_offsets;
        static uint64_t                 s_call = 0;
        ++s_call;
        std::erase_if(s_offsets, [](const auto& e) { return !e.ws.lock(); });
        const auto offsetMoved = [&](const PHLWORKSPACE& ws) {
            auto it = std::ranges::find_if(s_offsets, [&](const auto& e) { return e.ws.lock() == ws; });
            if (it == s_offsets.end()) {
                s_offsets.push_back({ws, ws->m_renderOffset->value(), s_call, false});
                return false;
            }
            if (it->call != s_call) {
                const auto OFF = ws->m_renderOffset->value();
                it->moved      = OFF.distance(it->off) > 0.01;
                it->off        = OFF;
                it->call       = s_call;
            }
            return it->moved;
        };

        const int N = layoutSamples();
        if (N >= 2 && monitor->m_transform == WL_OUTPUT_TRANSFORM_NORMAL) {
            // An interactive move/resize is the user's hand, not the layout's.
            const auto DRAGTARGET = g_layoutManager && g_layoutManager->dragController() ? g_layoutManager->dragController()->target() : nullptr;
            const auto NOW        = Time::steadyNow();

            for (const auto& window : Desktop::windowState()->windows()) {
                if (!window || !window->m_isMapped || window->isHidden() || window->m_monitor != monitor || !window->m_workspace)
                    continue;
                const auto& WS        = window->m_workspace;
                // A special workspace hiding is still drawn while it fades or
                // slides out (the scratchpad closing).
                const bool  SHOWN     = WS->isVisible() || WS->m_forceRendering ||
                    (WS->m_isSpecialWorkspace && WS->m_alpha->value() > 0.01F && (WS->m_alpha->isBeingAnimated() || WS->m_renderOffset->isBeingAnimated()));
                const auto  IT        = std::ranges::find_if(g_lastLayoutBox, [&](const auto& e) { return e.window.lock() == window; });
                const auto& WSOFFSET  = WS->m_renderOffset;
                // A SPECIAL workspace sliding in (the scratchpad) moves its
                // windows by the workspace's render offset, not their geometry.
                const bool  HANDMOVED = WS->m_isSpecialWorkspace && offsetMoved(WS); // asked every frame, so its last offset is never stale
                const bool  SLIDING   = WS->m_isSpecialWorkspace && !window->m_pinned && (WSOFFSET->isBeingAnimated() || HANDMOVED);
                // An ordinary workspace moved by a trackpad swipe, fingers down
                // or on the release.
                const bool  SWIPED    = !WS->m_isSpecialWorkspace && !window->m_pinned && (SWIPING || (SWIPEHOME && WSOFFSET->isBeingAnimated()));
                const bool  OFFSETS   = SLIDING || SWIPED;
                const auto* SPACE     = WS->m_space.get();
                const bool  SCROLLING = SPACE && SPACE->algorithm() && dynamic_cast<Layout::Tiled::CScrollingAlgorithm*>(SPACE->algorithm()->m_tiled.get());
                const bool  RESIZING  = window->sizeAnimation()->isBeingAnimated();
                const bool  MOVING    = window->positionAnimation()->isBeingAnimated();
                const bool  MODEFLIP  = justFlippedMode(window, NOW);
                if (!SHOWN || !(MOVING || RESIZING || OFFSETS) || MODEFLIP || (skip && skip(window)) || (DRAGTARGET && window->layoutTarget() == DRAGTARGET)) {
                    if (IT != g_lastLayoutBox.end()) {
                        g_lastLayoutBox.erase(IT);
                        // It WAS smeared last frame: take that smear off now,
                        // not when sweep() notices 20 ms later (user,
                        // 2026-09-30, recording 21-58-49: a desktop swipe let
                        // go mid-motion snaps back in one frame, and that frame
                        // was drawn with the swipe's last smear on it). A flying
                        // window (skip) feeds its own transformer: leave it.
                        if (!(skip && skip(window)))
                            if (auto* t = transformerOf(window))
                                t->m_armed = false;
                    }
                    continue;
                }

                // Monitor-local logical, like the renderer's currentBox.
                const CBox NOWBOX{window->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT) - monitor->m_position + (OFFSETS ? WSOFFSET->value() : Vector2D{}),
                                  window->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT)};
                if (IT == g_lastLayoutBox.end()) {
                    g_lastLayoutBox.push_back({window, NOWBOX, NOW});
                    continue;
                }
                const CBox  RAWPREV = IT->box;
                const float DTMS    = std::chrono::duration<float, std::milli>(NOW - IT->at).count();
                IT->box             = NOWBOX;
                IT->at              = NOW;
                // Whatever this frame decides, a window that is not smeared
                // below must not keep LAST frame's smear: the transformer would
                // hold it until it went stale (user, 2026-09-30: the terminal
                // stayed streaked across the screen after it had stopped).
                const auto DISARM = [&] {
                    if (auto* t = transformerOf(window))
                        t->m_armed = false;
                };
                if (!valid(RAWPREV) || !valid(NOWBOX) || !moved(RAWPREV, NOWBOX)) {
                    DISARM();
                    continue;
                }
                // A JUMP is not motion: when a swipe ends Hyprland re-bases the
                // workspaces' offsets, and a window's box leaps a whole screen
                // in one frame. More than a third of the monitor per frame is
                // never a hand or an animation.
                if (std::abs(NOWBOX.x - RAWPREV.x) > monitor->m_size.x / 3.0 || std::abs(NOWBOX.y - RAWPREV.y) > monitor->m_size.y / 3.0) {
                    DISARM();
                    continue;
                }

                // A SCROLLING desktop's tape carrying a window sideways is the
                // user scrolling (user, 2026-09-30): blurred only while it goes
                // FAST -- a flick -- and never while it drifts. A size change
                // there is a real retile and is blurred as anywhere else.
                if (SCROLLING && MOVING && !RESIZING && !OFFSETS) {
                    const double SPEED = RAWPREV.pos().distance(NOWBOX.pos()) / std::max(1.0F, DTMS) * 1000.0; // logical px/s
                    if (SPEED < FLICK_PX_PER_S) {
                        DISARM();
                        continue;
                    }
                }

                // Which of the user's blur settings this motion is: a swipe
                // under the fingers, the scratchpad sliding, the scrolling
                // tape flicked, or an ordinary retile / open. A swipe has no
                // animation clock (progress 0: its smear is just its speed).
                const float PCT  = SWIPED ? 0.F :
                                            std::max({MOVING ? window->positionAnimation()->getPercent() : 0.F, RESIZING ? window->sizeAnimation()->getPercent() : 0.F,
                                                      // Under the fingers there is no clock: like a swipe, 0.
                                                      SLIDING && WSOFFSET->isBeingAnimated() ? WSOFFSET->getPercent() : 0.F});
                const ESite SITE = SWIPED                                          ? ESite::SWIPE :
                    SLIDING                                                        ? ESite::SCRATCHPAD :
                    (SCROLLING && MOVING && !RESIZING && !OFFSETS)                 ? ESite::FLICK :
                                                                                     ESite::LAYOUT;
                const CBox PREV = shape(SITE, RAWPREV, NOWBOX, PCT);
                if (!moved(PREV, NOWBOX)) {
                    DISARM();
                    continue;
                }

                auto* t = transformerOf(window);
                if (!t)
                    t = attach(window);
                t->m_prev    = PREV;
                t->m_cur     = NOWBOX;
                t->m_armed   = true;
                // A swipe moves every window of two desktops at once, each
                // drawn offscreen: a lower ceiling keeps it at frame rate. Under
                // the spacing rule a ceiling only shortens the trail.
                t->m_samples    = SWIPED ? std::min(N, 8) : N;
                t->m_maxSamples = SWIPED ? 16 : MotionSpacing::MAX_SAMPLES;
                t->m_scale      = monitor->m_scale;
                t->m_fedAt   = NOW;

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
        // Drawn without the transformer, the window blurs the ordinary way.
        if (!m_held.empty())
            holdHyprlandBlur(window, false);
    }

    CSuspend::~CSuspend() {
        if (m_held.empty())
            return;
        if (const auto WINDOW = m_window.lock()) {
            for (auto& t : m_held)
                WINDOW->m_transformers.emplace_back(std::move(t));
            holdHyprlandBlur(WINDOW, true);
        }
    }

    void init() {
        g_frostListeners.push_back(Event::bus()->m_events.render.stage.listen([](eRenderStage stage) {
            if (stage == RENDER_BEGIN)
                g_frostedThisFrame.clear();
            else if (stage == RENDER_PRE_WINDOW)
                onPreWindow();
        }));
    }

    void shutdown() {
        g_frostListeners.clear();
        g_frostedThisFrame.clear();
    }

    void detachAll() {
        for (const auto& window : Desktop::windowState()->windows())
            if (window && !window->m_transformers.empty())
                detach(window);
    }
}
