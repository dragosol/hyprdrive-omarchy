#include "WindowPlacement.hpp"
#include "PlacementModel.hpp"
#include "FullscreenDesktop.hpp"
#include "OwnSizeTile.hpp"
#include "XGeometry.hpp"
#include "Intent.hpp"
#include "Mosaic.hpp"
#include "Config.hpp"
#include "DebugPaths.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/desktop/rule/windowRule/WindowRuleApplicator.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/xwayland/XSurface.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <format>
#include <string>
#include <vector>

namespace Hyprdrive::WindowPlacement {
    namespace {
        using namespace PlacementModel;

        CHyprSignalListener g_openEarly, g_open, g_close;

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
            const SWindowFacts FACTS = factsOf(w);
            SOutcome           o     = decide(FACTS, EIntent::None, placeFacts(w, w->m_workspace), PROMPTED);
            const auto         FROM  = w->m_workspace;
            if (o.kind == EOutcome::Beside) {
                const auto TO = FullscreenDesktop::beside(w, PROMPTED);
                // Decided again where it went (no fullscreen app there); if it
                // could not go, as though there were none to go beside.
                const SOutcome THERE = decide(FACTS, EIntent::None, {.enforce = enforcedOn(TO ? TO : FROM)}, PROMPTED);
                if (!TO) {
                    // (A float or a dialog over a fullscreen app: its
                    // Beside comes with the way back; until then, on top as
                    // Hyprland has it.)
                    o = FACTS.floating ? SOutcome{.kind = EOutcome::Leave, .why = EWhy::OverFullscreen} : THERE;
                } else
                    o = {.kind = THERE.kind, .why = THERE.why, .ownSize = THERE.ownSize, .prompted = PROMPTED};
                if (TO && THERE.kind == EOutcome::Tile && w->m_isFloating)
                    remember(w, o, tile(w, THERE.ownSize));
                else
                    remember(w, o, false);
            } else {
                bool own = false;
                if (o.kind == EOutcome::Tile && (w->m_isFloating || o.ownSize))
                    own = tile(w, o.ownSize);
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
                if (P.ownSize && P.win.lock() == w && !w->m_isFloating)
                    (void)OwnSizeTile::tileAtOwnSize(w);
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
        });
    }

    void shutdown() {
        g_openEarly.reset();
        g_open.reset();
        g_close.reset();
        g_placed.clear();
    }
}
