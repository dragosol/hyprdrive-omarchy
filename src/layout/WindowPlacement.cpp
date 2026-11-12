#include "WindowPlacement.hpp"
#include "PlacementModel.hpp"
#include "FullscreenDesktop.hpp"
#include "OwnSizeTile.hpp"
#include "XGeometry.hpp"
#include "Intent.hpp"
#include "Mosaic.hpp"
#include "Config.hpp"
#include "DebugPaths.hpp"
#include "FunctionHooks.hpp"
#include "Minimize.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/desktop/rule/windowRule/WindowRuleApplicator.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/layout/target/WindowTarget.hpp>
#include <hyprland/src/layout/space/Space.hpp>
#include <hyprland/src/config/shared/actions/ConfigActions.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/debug/log/Logger.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/xwayland/XSurface.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <format>
#include <optional>
#include <string>
#include <vector>

namespace Hyprdrive::WindowPlacement {
    namespace {
        using namespace PlacementModel;

        CHyprSignalListener g_openEarly, g_open, g_close, g_floating;

        using clock = std::chrono::steady_clock;
        int64_t nowMs() {
            return std::chrono::duration_cast<std::chrono::milliseconds>(clock::now().time_since_epoch()).count();
        }

        // THE USER'S INTENT per window: what he last floated or tiled it to.
        // An X11 window keeps it across a Wine remap (an unmap and a map is a
        // new CWindow for the same X window): kept by X window and process for
        // REMAP_S after it closed, and taken by the window that maps as it.
        constexpr int64_t REMAP_MS = 10000;
        struct SIntentRec {
            PHLWINDOWREF           win;
            EIntent                intent = EIntent::None;
            uint32_t               xid    = 0;
            pid_t                  pid    = 0;
            std::optional<int64_t> closedAt;
        };
        std::vector<SIntentRec> g_intents;

        EIntent intentOf(const PHLWINDOW& w) {
            for (const auto& I : g_intents)
                if (!I.closedAt && I.win.lock() == w)
                    return I.intent;
            const auto XS = w->m_isX11 ? w->m_xwaylandSurface.lock() : nullptr;
            if (!XS || !XS->m_xID)
                return EIntent::None;
            for (auto& I : g_intents)
                if (I.closedAt && I.xid == XS->m_xID && I.pid == w->getPID() && nowMs() - *I.closedAt < REMAP_MS) {
                    I.win = w; // remapped: the same window to him
                    I.closedAt.reset();
                    return I.intent;
                }
            return EIntent::None;
        }
        void stamp(const PHLWINDOW& w, EIntent intent) {
            std::erase_if(g_intents, [&](const SIntentRec& i) { return !i.closedAt && i.win.lock() == w; });
            const auto XS = w->m_isX11 ? w->m_xwaylandSurface.lock() : nullptr;
            g_intents.push_back({.win = w, .intent = intent, .xid = XS ? XS->m_xID : 0, .pid = w->getPID()});
        }

        // THE FUNNEL. Every floating change after the map goes through
        // CWindowTarget::setFloating -- the user's toggles (through
        // changeFloatingMode, which then says window.floating), and what
        // nobody announces: dwindle and master refusing a window too small
        // for its tile, a group, a swap. A window that became floating here
        // and was not announced as the user's is decided again once the event
        // is over (reconcile), never inside the layout's own call.
        using TSetFloating                  = void (*)(void*, bool);
        constexpr const char* SET_FLOATING  = "_ZN6Layout13CWindowTarget11setFloatingEb";
        CFunctionHook*        g_floatHook   = nullptr;
        int                   g_ours        = 0; // our own re-tile: not the user, not a cause
        std::vector<PHLWINDOWREF> g_due;          // floated, cause not known yet
        wl_event_source*          g_timer = nullptr;
        struct SBudgetRec {
            PHLWINDOWREF win;
            CBudget      budget;
        };
        std::vector<SBudgetRec> g_budgets;
        void arm(int ms);

        void hkSetFloating(void* self, bool x) {
            auto* const     T   = sc<Layout::CWindowTarget*>(self);
            const PHLWINDOW W   = T ? T->window() : nullptr;
            const bool      WAS = W && W->m_isFloating;
            rc<TSetFloating>(g_floatHook->m_original)(self, x);
            if (!W || g_ours || WAS || !W->m_isFloating)
                return;
            if (std::ranges::none_of(g_due, [&](const PHLWINDOWREF& r) { return r.lock() == W; }))
                g_due.emplace_back(W);
            arm(1);
        }

        // What was decided for each window as it mapped.
        struct SPlaced {
            PHLWINDOWREF win;
            SOutcome     outcome;
            bool         ownSize = false; // set up to tile at its own size: again at window.open
        };
        std::vector<SPlaced> g_placed;

        void trace(const std::string& s) {
            if (!DebugPaths::flagOn("hyprdrive-mosaic.on"))
                return;
            if (FILE* const F = DebugPaths::openLog("hyprdrive-mosaic.log")) {
                std::fprintf(F, "%lld placement %s\n",
                             (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(), s.c_str());
                std::fclose(F);
            }
        }

        bool openTiled() {
            return Config::getValue<bool>("plugin:hyprdrive:general:open_tiled");
        }

        bool enforcedOn(const PHLWORKSPACE& ws) {
            return ws && (openTiled() || Mosaic::isMosaicWorkspace(ws));
        }

        // An X11 window's _NET_WM_WINDOW_TYPE, as far as placing it goes.
        enum class EXType { Normal, Passing, Unplaceable };
        EXType xType(const PHLWINDOW& w) {
            const auto XS = w->m_xwaylandSurface.lock();
            if (!XS)
                return EXType::Normal;
            // Never placed: menus, tooltips, notifications, docks -- tiling
            // them is physically wrong (and Hyprland never manages most).
            static constexpr const char* UNPLACEABLE[] = {"_NET_WM_WINDOW_TYPE_MENU",         "_NET_WM_WINDOW_TYPE_POPUP_MENU", "_NET_WM_WINDOW_TYPE_DROPDOWN_MENU",
                                                          "_NET_WM_WINDOW_TYPE_TOOLTIP",      "_NET_WM_WINDOW_TYPE_NOTIFICATION", "_NET_WM_WINDOW_TYPE_COMBO",
                                                          "_NET_WM_WINDOW_TYPE_DND",          "_NET_WM_WINDOW_TYPE_DOCK",       "_KDE_NET_WM_WINDOW_TYPE_OVERRIDE"};
            // Placed as Hyprland places them (ruling 5): a splash, a palette.
            static constexpr const char* PASSING[] = {"_NET_WM_WINDOW_TYPE_SPLASH", "_NET_WM_WINDOW_TYPE_UTILITY", "_NET_WM_WINDOW_TYPE_TOOLBAR"};
            for (const uint32_t A : XS->m_atoms) {
                if (!A)
                    continue;
                for (const char* N : UNPLACEABLE)
                    if (A == XGeometry::atom(N))
                        return EXType::Unplaceable;
                for (const char* N : PASSING)
                    if (A == XGeometry::atom(N))
                        return EXType::Passing;
            }
            return EXType::Normal;
        }

        SWindowFacts factsOf(const PHLWINDOW& w) {
            SWindowFacts f;
            const auto   WS = w->m_workspace;
            const auto   XT = w->m_isX11 ? xType(w) : EXType::Normal;
            f.placeable     = WS && !WS->m_isSpecialWorkspace && !w->m_pinned && XT != EXType::Unplaceable && !(w->m_isX11 && w->isX11OverrideRedirect());
            f.passing       = XT == EXType::Passing;
            f.floating      = w->m_isFloating;
            f.tagged        = w->m_ruleApplicator && w->m_ruleApplicator->m_tagKeeper.isTagged("hyprdrive-float");
            f.ruleFloat     = w->m_ruleApplicator && w->m_ruleApplicator->static_.floating.value_or(false);
            Vector2D size;
            f.fixedSize = OwnSizeTile::fixedSize(w, size);
            f.child     = w->parent() || (w->m_isX11 && w->x11Parent());
            f.modal     = w->isModal();
            return f;
        }

        bool sameApp(const PHLWINDOW& a, const PHLWINDOW& b) {
            return a && b && ((a->getPID() > 0 && a->getPID() == b->getPID()) || (!a->m_class.empty() && a->m_class == b->m_class));
        }

        SPlaceFacts placeFacts(const PHLWINDOW& w, const PHLWORKSPACE& ws) {
            const auto APP = FullscreenDesktop::fullscreenAppOn(ws);
            return {.enforce       = enforcedOn(ws),
                    .fullscreenApp = APP && APP != w,
                    .sameApp       = APP && APP != w && sameApp(APP, w),
                    .stay          = Config::getValue<std::string>("plugin:hyprdrive:general:fullscreen_desktop") == "stay"};
        }

        // Tile, before the layout files it: the target's floating() reads this
        // flag (CWindowTarget::floating), so newTarget takes it into the tiles
        // and the open animation starts from its tile.
        bool tile(const PHLWINDOW& w, bool ownSize) {
            const bool OWN = ownSize && OwnSizeTile::tileAtOwnSize(w);
            w->m_isFloating = false;
            return OWN;
        }

        void remember(const PHLWINDOW& w, const SOutcome& o, bool ownSize) {
            std::erase_if(g_placed, [&](const SPlaced& p) { return !p.win || p.win.lock() == w; });
            g_placed.push_back({w, o, ownSize});
        }

        void onOpenEarly(const PHLWINDOW& w) {
            if (!w || !g_pCompositor || g_pCompositor->m_isShuttingDown)
                return;
            const char*        WHY      = Intent::judge(w);
            const bool         PROMPTED = WHY != nullptr;
            Intent::opened(w);
            const SWindowFacts FACTS  = factsOf(w);
            const EIntent      INTENT = intentOf(w);
            SOutcome           o      = decide(FACTS, INTENT, placeFacts(w, w->m_workspace), PROMPTED);
            const auto         FROM  = w->m_workspace;
            if (o.kind == EOutcome::Beside) {
                const auto TO = FullscreenDesktop::beside(w, PROMPTED);
                // Decided again where it went (no fullscreen app there); if it
                // could not go, as though there were none to go beside.
                const SOutcome THERE = decide(FACTS, INTENT, {.enforce = enforcedOn(TO ? TO : FROM)}, PROMPTED);
                if (!TO) {
                    // No place for it (refused, logged): OVERLAY -- on top of
                    // the app as Hyprland has it, the only float nobody asked
                    // for.
                    o = SOutcome{.kind = EOutcome::Leave, .why = EWhy::OverFullscreen};
                } else
                    o = {.kind = THERE.kind, .why = THERE.why, .ownSize = THERE.ownSize, .prompted = PROMPTED};
                if (TO && THERE.kind == EOutcome::Tile && w->m_isFloating)
                    remember(w, o, tile(w, THERE.ownSize));
                else
                    remember(w, o, false);
            } else {
                FullscreenDesktop::openedOn(w, PROMPTED);
                bool own = false;
                if (o.kind == EOutcome::Tile && (w->m_isFloating || o.ownSize))
                    own = tile(w, o.ownSize);
                // His float, remembered across a remap: it maps floating again.
                if (o.kind == EOutcome::Float && o.why == EWhy::Intent && INTENT == EIntent::Float)
                    w->m_isFloating = true;
                remember(w, o, own);
            }
            trace(std::format("{} {:x} on {}: {} ({}){}{}", w->m_class, (uintptr_t)w.get(), FROM ? FROM->m_id : -1,
                              o.kind == EOutcome::Tile        ? "tile" :
                                  o.kind == EOutcome::Float  ? "float" :
                                  o.kind == EOutcome::Beside ? "beside" :
                                                               "leave",
                              whyName(o.why), o.ownSize ? ", own size" : "", w->m_workspace != FROM ? std::format(", moved to {}", w->m_workspace->m_id) : ""));
        }

        // window.open: Hyprland set every tiled window's pseudo size to its
        // first box after the layout filed it; an own-size tile's is its own.
        void onOpen(const PHLWINDOW& w) {
            for (const auto& P : g_placed)
                if (P.ownSize && P.win.lock() == w && !w->m_isFloating && OwnSizeTile::tileAtOwnSize(w))
                    if (const auto T = w->layoutTarget(); T && T->space())
                        T->space()->recalculate();
        }
    }

    namespace {
        // A window floated by nobody we know of: tiled back where the
        // enforcer is on and nothing else holds it afloat -- within its
        // budget; past that it is left floating for good, said once.
        int reconcile(void*) {
            if (!g_pCompositor || g_pCompositor->m_isShuttingDown)
                return 0;
            for (const auto& REF : std::exchange(g_due, {})) {
                const auto W = REF.lock();
                if (!W || !W->m_isMapped || !W->m_isFloating || !W->m_workspace || Fullscreen::controller()->isFullscreen(W) || Minimize::isMinimized(W))
                    continue;
                if (!retile(factsOf(W), intentOf(W), {.enforce = enforcedOn(W->m_workspace)}))
                    continue;
                auto IT = std::ranges::find_if(g_budgets, [&](const SBudgetRec& b) { return b.win.lock() == W; });
                if (IT == g_budgets.end()) {
                    g_budgets.push_back({W, {}});
                    IT = std::prev(g_budgets.end());
                }
                if (IT->budget.spent())
                    continue;
                if (!IT->budget.spend(nowMs())) {
                    trace(std::format("{} {:x} keeps floating itself: left floating (gave up after {} re-tiles in {} ms)", W->m_class, (uintptr_t)W.get(), CBudget::RETILES,
                                      CBudget::SPAN));
                    Log::logger->log(Log::WARN, "[hyprdrive] placement: {} keeps floating itself, left floating", W->m_class);
                    continue;
                }
                trace(std::format("{} {:x} floated by its layout or the app: tiled again", W->m_class, (uintptr_t)W.get()));
                ++g_ours;
                (void)::Config::Actions::floatWindow(::Config::Actions::TOGGLE_ACTION_DISABLE, W);
                --g_ours;
            }
            return 0;
        }
        void arm(int ms) {
            if (!g_timer && g_pCompositor)
                g_timer = wl_event_loop_add_timer(g_pCompositor->m_wlEventLoop, reconcile, nullptr);
            if (g_timer)
                wl_event_source_timer_update(g_timer, ms);
        }

        // window.floating: changeFloatingMode, after its change -- Super+T,
        // hyprctl, Omarchy's menu, the window pill, the trackpad float, a
        // Super+drag. The user's, unless it is ours or the minimize stash's.
        void onFloating(const PHLWINDOW& w) {
            if (!w || g_ours || !w->m_isMapped || (w->m_workspace && w->m_workspace->m_isSpecialWorkspace) || Minimize::isMinimized(w))
                return;
            std::erase_if(g_due, [&](const PHLWINDOWREF& r) { return !r || r.lock() == w; });
            stamp(w, w->m_isFloating ? EIntent::Float : EIntent::Tile);
            trace(std::format("{} {:x}: the user {} it", w->m_class, (uintptr_t)w.get(), w->m_isFloating ? "floated" : "tiled"));
        }
    }

    bool mainWindow(const PHLWINDOW& w) {
        return w && !w->m_isFloating && !w->parent() && !w->isModal() && !(w->m_workspace && w->m_workspace->m_isSpecialWorkspace);
    }

    SOutcome outcomeOf(const PHLWINDOW& w) {
        for (const auto& P : g_placed)
            if (P.win.lock() == w)
                return P.outcome;
        return {};
    }

    void init() {
        g_openEarly = Event::bus()->m_events.window.openEarly.listen([](PHLWINDOW w) { onOpenEarly(w); });
        g_open      = Event::bus()->m_events.window.open.listen([](PHLWINDOW w) { onOpen(w); });
        g_close     = Event::bus()->m_events.window.close.listen([](PHLWINDOW w) {
            std::erase_if(g_placed, [&](const SPlaced& p) { return !p.win || p.win.lock() == w; });
            std::erase_if(g_budgets, [&](const SBudgetRec& b) { return !b.win || b.win.lock() == w; });
            const int64_t NOW = nowMs();
            std::erase_if(g_intents, [&](SIntentRec& i) {
                if (i.closedAt)
                    return NOW - *i.closedAt >= REMAP_MS;
                if (i.win.lock() != w)
                    return !i.win;
                if (!i.xid)
                    return true;
                i.closedAt = NOW; // an X11 window: kept for its remap
                return false;
            });
        });
        g_floating = Event::bus()->m_events.window.floating.listen([](PHLWINDOW w) { onFloating(w); });
        // Not fatal when missing: the decision at openEarly still stands, a
        // later float by a layout is then left as Hyprland has it.
        bool found  = false;
        g_floatHook = FunctionHooks::firstMatch("WindowPlacement", SET_FLOATING, rc<void*>(hkSetFloating), {}, &found);
        if (g_floatHook && !g_floatHook->hook())
            g_floatHook = nullptr;
        if (!g_floatHook) {
            trace("CWindowTarget::setFloating not hooked: decided at the map only");
            Log::logger->log(Log::WARN, "[hyprdrive] placement: CWindowTarget::setFloating not found ({}): a layout's later float is left alone", found ? "hook failed" : "missing");
        }
    }

    void shutdown() {
        g_openEarly.reset();
        g_open.reset();
        g_close.reset();
        g_floating.reset();
        if (g_timer) {
            wl_event_source_remove(g_timer);
            g_timer = nullptr;
        }
        // Unhooked first: OwnSizeTile's shutdown (after this one) floats its
        // windows through setFloating, and so can Hyprland's exit; a hook left
        // standing would call into a trampoline nobody keeps (the rig's
        // SIGSEGV at exit, 2026-11-11).
        if (g_floatHook) {
            g_floatHook->unhook();
            g_floatHook = nullptr;
        }
        g_placed.clear();
        g_due.clear();
        g_budgets.clear();
        g_intents.clear();
    }
}
