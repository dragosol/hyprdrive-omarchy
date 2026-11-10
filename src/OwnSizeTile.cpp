#include "OwnSizeTile.hpp"
#include "DebugPaths.hpp"

#include <hyprland/src/config/shared/actions/ConfigActions.hpp>
#include <hyprland/src/desktop/rule/windowRule/WindowRuleApplicator.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/types/OverridableVar.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/layout/target/Target.hpp>
#include <hyprland/src/layout/space/Space.hpp>
#include <hyprland/src/protocols/XDGShell.hpp>
#include <hyprland/src/xwayland/XSurface.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <format>
#include <optional>
#include <string>
#include <vector>

namespace ScrollOverview::OwnSizeTile {
    namespace {
        using ::Config::Actions::ActionResult;
        using ::Config::Actions::eTogglableAction;
        using TFloatWindow = ActionResult (*)(eTogglableAction, std::optional<PHLWINDOW>);

        constexpr const char* FLOAT_WINDOW =
            "_ZN6Config7Actions11floatWindowENS0_16eTogglableActionESt8optionalIN9Hyprutils6Memory14CSharedPointerIN7Desktop4View7CWindowEEEE";

        constexpr const char* ON_X11_CONFIGURE = "_ZN7Desktop4View7CWindow21onX11ConfigureRequestEN9Hyprutils4Math4CBoxE";
        using TOnX11Configure                  = void (*)(void*, CBox);

        CFunctionHook*            g_hook          = nullptr;
        CFunctionHook*            g_configureHook = nullptr;
        std::vector<PHLWINDOWREF> g_owned;

        void trace(const std::string& s) {
            if (!DebugPaths::flagOn("hyprdrive-mosaic.on"))
                return;
            if (FILE* const F = DebugPaths::openLog("hyprdrive-mosaic.log")) {
                std::fprintf(F, "ownsize %s\n", s.c_str());
                std::fclose(F);
            }
        }

        // Into a tile at its own size: pseudotiled at it (centred, never
        // stretched), and no_max_size so dwindle does not turn it away. The
        // pseudo box is the window's whole box: the layout takes the reserved
        // edge (the border) off it again, and 4 px short of its own size is
        // exactly what a program that insists on its size fights (rig: Wine's
        // Minesweeper tiled at 150x197, not its 154x201).
        void own(const PHLWINDOW& w, const Vector2D& size) {
            const auto T = w->layoutTarget();
            if (!T)
                return;
            const auto R = w->getFullWindowReservedArea();
            T->setPseudoSize(size + R.topLeft + R.bottomRight);
            T->setPseudo(true);
            w->m_ruleApplicator->noMaxSize().set(true, Desktop::Types::PRIORITY_SET_PROP);
            // And never under it: a tile smaller than the window (a mosaic
            // packed tight, a crowded dwindle) shrank the pseudo box, and the
            // program fought the smaller size at ~3600/s (rig). With
            // misc:size_limits_tiled (20-layout.lua) the layout clamps a tile
            // to its window's min size, centred -- it overflows its slot, it
            // is never squeezed.
            w->m_ruleApplicator->minSize().set(size, Desktop::Types::PRIORITY_SET_PROP);
            if (!owned(w))
                g_owned.emplace_back(w);
            trace(std::format("{} tiles at its own size {:.0f}x{:.0f}", w->m_class, size.x, size.y));
        }

        // Floated again: nothing of ours stays on it.
        void release(const PHLWINDOW& w) {
            std::erase_if(g_owned, [&](const PHLWINDOWREF& r) { return !r || r.lock() == w; });
            if (const auto T = w->layoutTarget())
                T->setPseudo(false);
            w->m_ruleApplicator->noMaxSize().unset(Desktop::Types::PRIORITY_SET_PROP);
            w->m_ruleApplicator->minSize().unset(Desktop::Types::PRIORITY_SET_PROP);
            trace(std::format("{} floats again", w->m_class));
        }

        ActionResult hkFloatWindow(eTogglableAction action, std::optional<PHLWINDOW> w) {
            // The window floatWindow acts on: the one named, else the focused
            // one (Actions' xtract, exactly).
            const PHLWINDOW WINDOW = w.value_or(Desktop::focusState()->window());
            bool            wantFloat = false;
            if (WINDOW) {
                switch (action) {
                    case ::Config::Actions::TOGGLE_ACTION_TOGGLE: wantFloat = !WINDOW->m_isFloating; break;
                    case ::Config::Actions::TOGGLE_ACTION_ENABLE: wantFloat = true; break;
                    case ::Config::Actions::TOGGLE_ACTION_DISABLE: wantFloat = false; break;
                }
            }
            // A fixed-size window on its way into a tile is set up first, so the
            // layout places it at its own size from the first configure on.
            Vector2D size;
            if (WINDOW && WINDOW->m_isMapped && WINDOW->m_isFloating && !wantFloat && fixedSize(WINDOW, size))
                own(WINDOW, size);

            const auto RESULT = rc<TFloatWindow>(g_hook->m_original)(action, w);

            // Floated by this call -- or the tile turned it away after all.
            if (WINDOW && WINDOW->m_isFloating && owned(WINDOW))
                release(WINDOW);
            return RESULT;
        }

        // A tiled X11 window's every ConfigureRequest is answered -- a synthetic
        // ConfigureNotify and a refocus (CWindow::onX11ConfigureRequest's tiled
        // path) -- and Wine answers that with the same request again: 1230/s
        // on 2026-11-04 with geometry that never changed, which is what starves
        // GTK apps of the wheel. A floating window's no-op request gets no
        // answer; an own-size tile now gets the same. A request for another size
        // is the program changing the size it fixes (a bigger board, a login
        // turning into its main window): the tile follows it, or lets go of it
        // when the window can be resized now.
        void hkOnX11Configure(void* thisptr, CBox box) {
            auto* const SELF = sc<Desktop::View::CWindow*>(thisptr);
            const auto  W    = SELF ? SELF->m_self.lock() : PHLWINDOW{};
            if (W && W->m_isMapped && !W->m_isFloating && owned(W)) {
                if (std::abs(box.w - W->m_reportedSize.x) < 1 && std::abs(box.h - W->m_reportedSize.y) < 1)
                    return;
                Vector2D size;
                if (fixedSize(W, size))
                    own(W, size);
                else
                    release(W);
                if (const auto T = W->layoutTarget(); T && T->space())
                    T->space()->recalculate();
                trace(std::format("{} asked for {:.0f}x{:.0f}: the tile follows", W->m_class, box.w, box.h));
            }
            rc<TOnX11Configure>(g_configureHook->m_original)(thisptr, box);
        }
    }

    bool fixedSize(const PHLWINDOW& w, Vector2D& size) {
        if (!w)
            return false;
        if (w->m_isX11) {
            const auto XS = w->m_xwaylandSurface.lock();
            if (!XS || !XS->m_sizeHints)
                return false;
            const auto& H = *XS->m_sizeHints;
            if (H.min_width < 1 || H.min_height < 1 || H.min_width != H.max_width || H.min_height != H.max_height)
                return false;
            size = w->xwaylandSizeToReal(Vector2D{sc<double>(H.min_width), sc<double>(H.min_height)});
            return true;
        }
        if (!w->m_xdgSurface || !w->m_xdgSurface->m_toplevel)
            return false;
        const Vector2D MIN = w->m_xdgSurface->m_toplevel->layoutMinSize();
        const Vector2D MAX = w->m_xdgSurface->m_toplevel->layoutMaxSize();
        if (MIN.x < 1 || MIN.y < 1 || std::abs(MAX.x - MIN.x) >= 1 || std::abs(MAX.y - MIN.y) >= 1)
            return false;
        size = MIN;
        return true;
    }

    bool owned(const PHLWINDOW& w) {
        return w && std::ranges::any_of(g_owned, [&](const PHLWINDOWREF& r) { return r && r.lock() == w; });
    }

    void init() {
        const auto FNS = HyprlandAPI::findFunctionsByName(SCROLLOVERVIEW_HANDLE, FLOAT_WINDOW);
        if (FNS.empty()) {
            trace("floatWindow not found: Hyprland's own toggle stands");
            return;
        }
        g_hook = HyprlandAPI::createFunctionHook(SCROLLOVERVIEW_HANDLE, FNS[0].address, rc<void*>(hkFloatWindow));
        if (g_hook && !g_hook->hook())
            g_hook = nullptr;
        // Only with the toggle hooked: no own-size tile exists without it.
        const auto CFG = g_hook ? HyprlandAPI::findFunctionsByName(SCROLLOVERVIEW_HANDLE, ON_X11_CONFIGURE) : std::vector<SFunctionMatch>{};
        if (!CFG.empty()) {
            g_configureHook = HyprlandAPI::createFunctionHook(SCROLLOVERVIEW_HANDLE, CFG[0].address, rc<void*>(hkOnX11Configure));
            if (g_configureHook && !g_configureHook->hook())
                g_configureHook = nullptr;
        }
    }

    void shutdown() {
        // Floated first, while the hooks still stand: without the plugin a
        // tiled X11 window's every request is answered again, and Wine's no-op
        // requests turn back into the loop (a plugin reload included -- the new
        // one would not know them). Floating is the state that cannot loop;
        // Super+T puts them back. The hooks go with the plugin (HyprlandAPI
        // removes them at unload).
        std::vector<PHLWINDOW> tiled;
        for (const auto& r : g_owned)
            if (const auto W = r.lock(); W && W->m_isMapped && !W->m_isFloating)
                tiled.push_back(W);
        for (const auto& W : tiled)
            (void)::Config::Actions::floatWindow(::Config::Actions::TOGGLE_ACTION_ENABLE, W);
        g_owned.clear();
        g_hook          = nullptr;
        g_configureHook = nullptr;
    }
}
