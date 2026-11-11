// See BorderFade.hpp.
#include "BorderFade.hpp"
#include "MotionClock.hpp"
#include "MotionTween.hpp"
#include "MotionRetarget.hpp"
#include "Config.hpp"

#include <hyprland/src/animation/AnimationManager.hpp>
#include <hyprland/src/config/shared/animation/AnimationTree.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprutils/animation/BezierCurve.hpp>

#include <algorithm>
#include <chrono>
#include <functional>
#include <unordered_map>

namespace Hyprdrive::BorderFade {
    namespace {
        using Clock = std::chrono::steady_clock;

        struct SFade {
            MONITORID         monitor = MONITOR_INVALID; // where it was last drawn: animatingOn()
            // to = 1 lit, 0 resting; from = where the fade started (a reversal
            // starts mid-way).
            Hyprdrive::Motion::STween<Clock::time_point> fade;
            // A fade turned while it was still moving carries on at the speed
            // it had (MotionRetarget.hpp); 0 for every other leg.
            double            kick = 0.0;
            Clock::time_point seen;
        };

        std::unordered_map<uint64_t, SFade> g_fades;
        Clock::time_point                   g_lastPrune;

        // Forgotten after this long undrawn: a plate that comes back later
        // starts settled rather than replaying a fade from before.
        constexpr auto FORGET = std::chrono::seconds(2);

        struct SCurve {
            float                                                ms = 0.F;
            Hyprutils::Memory::CSharedPointer<Hyprutils::Animation::CBezierCurve> bezier;
            Hyprdrive::Motion::SKickRoom                          room; // of `bezier`, measured when it is read
        };
        float ease(const SCurve& c, float t);

        // The `border` leaf. Read once and kept until the next config reload:
        // the reload listener bumps Config::themeGeneration() -- the same
        // signal the palette and font caches re-read on -- and a theme switch
        // is a reload. Same read as closingTileDurationMs: the animation tree,
        // never Config::getValue.
        SCurve   g_curve;
        uint64_t g_curveGeneration = 0;

        const SCurve& curve() {
            auto& cached = g_curve;
            if (g_curveGeneration == Config::themeGeneration())
                return cached;
            g_curveGeneration = Config::themeGeneration();
            cached            = {};

            const auto& TREE = ::Config::animationTree();
            if (!TREE)
                return cached;
            const auto CONFIG = TREE->getAnimationPropertyConfig("border");
            const auto VALUES = CONFIG && CONFIG->pValues ? CONFIG->pValues.lock() : CONFIG;
            if (!VALUES || !VALUES->internalEnabled || VALUES->internalSpeed <= 0.01F)
                return cached;
            cached.ms = VALUES->internalSpeed * 100.F;
            if (const auto& MGR = Animation::mgr(); MGR && !VALUES->internalBezier.empty())
                cached.bezier = MGR->getBezier(VALUES->internalBezier);
            cached.room = Hyprdrive::Motion::kickRoom([&](float t) { return ease(cached, t); });
            return cached;
        }

        float ease(const SCurve& c, float t) {
            if (t >= 1.F)
                return 1.F;
            return c.bezier ? c.bezier->getYForPoint(t) : t;
        }

        void prune(Clock::time_point now) {
            if (now - g_lastPrune < FORGET)
                return;
            g_lastPrune = now;
            std::erase_if(g_fades, [&](const auto& kv) { return now - kv.second.seen > FORGET; });
        }

        float progressAt(const SFade& f, const SCurve& c, Clock::time_point now) {
            // The animation clock: the speed setting stretches the `border`
            // leaf's time, and a snapping clock lands the ring on its colour.
            return std::clamp(Hyprdrive::Motion::Clock::animMsBetween(now, f.fade.start) / c.ms, 0.F, 1.F);
        }

        float valueAt(const SFade& f, const SCurve& c, Clock::time_point now) {
            if (c.ms <= 0.F)
                return f.fade.to;
            // The animation clock: the speed setting stretches the `border`
            // leaf's time, and a snapping clock lands the ring on its colour.
            const float T = progressAt(f, c, now);
            const float PLAIN = f.fade.at(ease(c, T));
            return f.kick == 0.0 ? PLAIN : static_cast<float>(Hyprdrive::Motion::withKick(PLAIN, f.kick, T));
        }

        // A new leg toward `target` from the value as drawn; if the old leg was
        // still moving, at the speed it had (same `border` time both legs, so
        // the velocity per unit progress carries over unscaled).
        void turn(SFade& f, const SCurve& c, float target, Clock::time_point now) {
            const float CURRENT = valueAt(f, c, now);
            double      kick    = 0.0;
            if (c.ms > 0.F) {
                const float T = progressAt(f, c, now);
                if (T < 1.F) {
                    const double VEL = Hyprdrive::Motion::legSlope(f.fade.to - f.fade.from, Hyprdrive::Motion::curveSlope([&](float t) { return ease(c, t); }, T), f.kick, T);
                    kick             = Hyprdrive::Motion::matchKick(target - CURRENT, VEL, c.room);
                }
            }
            f.fade.retarget(CURRENT, target, now);
            f.kick = kick;
        }
    }

    uint64_t key(const void* p, uint64_t salt) {
        return std::hash<const void*>{}(p) ^ (salt * 0x9E3779B97F4A7C15ULL);
    }

    uint64_t key(std::string_view tag, uint64_t salt) {
        return std::hash<std::string_view>{}(tag) ^ (salt * 0x9E3779B97F4A7C15ULL);
    }

    float mix(const PHLMONITOR& monitor, uint64_t k, bool lit) {
        const auto NOW = Clock::now();
        prune(NOW);
        const auto& C = curve();

        auto [it, fresh] = g_fades.try_emplace(k);
        auto& f          = it->second;
        if (fresh) {
            // From RESTING, always: a ring first seen lit fades in. The
            // overview does not redraw an idle tile, so a tile's key is
            // forgotten while it sits there, and "first sight settles" made
            // every grid hover snap to lit (user, 2026-10-16).
            f.fade = {.from = 0.F, .to = lit ? 1.F : 0.F, .start = NOW};
        } else if ((f.fade.to == 1.F) != lit) {
            turn(f, C, lit ? 1.F : 0.F, NOW);
        }
        f.seen    = NOW;
        f.monitor = monitor ? monitor->m_id : MONITOR_INVALID;

        return valueAt(f, C, NOW);
    }

    CHyprColor color(const PHLMONITOR& monitor, uint64_t k, bool lit, const CHyprColor& resting, const CHyprColor& active) {
        const float M = mix(monitor, k, lit);
        if (M <= 0.F)
            return resting;
        if (M >= 1.F)
            return active;
        const auto L = [M](double a, double b) { return sc<float>(a + ((b - a) * M)); };
        return CHyprColor{L(resting.r, active.r), L(resting.g, active.g), L(resting.b, active.b), L(resting.a, active.a)};
    }

    bool animatingOn(const PHLMONITOR& monitor) {
        if (!monitor || g_fades.empty())
            return false;
        const auto& C   = curve();
        const auto  NOW = Clock::now();
        return std::ranges::any_of(g_fades, [&](const auto& kv) {
            const auto& F = kv.second;
            return F.monitor == monitor->m_id && NOW - F.seen < FORGET && valueAt(F, C, NOW) != F.fade.to;
        });
    }

    bool fading(uint64_t k) {
        const auto IT = g_fades.find(k);
        if (IT == g_fades.end())
            return false;
        return valueAt(IT->second, curve(), Clock::now()) != IT->second.fade.to;
    }

    void clear() {
        g_fades.clear();
        g_curve           = {};
        g_curveGeneration = 0;
    }
}
