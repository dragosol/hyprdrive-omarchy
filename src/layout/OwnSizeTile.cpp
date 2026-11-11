#include "OwnSizeTile.hpp"
#include "DebugPaths.hpp"

#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/config/shared/actions/ConfigActions.hpp>
#include <hyprland/src/config/shared/complex/ComplexDataTypes.hpp>
#include <hyprland/src/desktop/rule/windowRule/WindowRuleApplicator.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/types/OverridableVar.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/layout/target/Target.hpp>
#include <hyprland/src/layout/space/Space.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/protocols/XDGShell.hpp>
#include <hyprland/src/xwayland/XSurface.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <format>
#include <optional>
#include <string>
#include <vector>

namespace Hyprdrive::OwnSizeTile {
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
            // program fought the smaller size at ~3600/s (rig). place() gives
            // such a tile room -- it overflows its slot, it is never squeezed;
            // this min size keeps a resize (scrolling's, a drag) above it too.
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

        // `b` grown about its centre to at least `size`, then moved by `by`.
        CBox grown(const CBox& b, const Vector2D& size, const Vector2D& by) {
            const double W = std::max(b.w, size.x);
            const double H = std::max(b.h, size.y);
            return CBox{b.x - (W - b.w) / 2.0 + by.x, b.y - (H - b.h) / 2.0 + by.y, W, H};
        }

        // The box a too-small own-size tile is placed through, or `box` itself
        // when it has the room: grown about its centre until the window's
        // pseudo box fits in what updatePos keeps of it (a logical box loses
        // gaps_in on its inner sides; a visual box is used as it is), plus
        // `extra`. While the tile is on screen the window is kept inside the
        // work area (a crowded dwindle's corner tile); a tile past the screen's
        // edge -- the infinite canvas's, a scrolled-away column -- keeps its
        // window out there with it.
        Layout::STargetBox roomFor(Layout::ITarget* T, const PHLWINDOW& W, const Layout::STargetBox& box, const Vector2D& extra) {
            static auto    PGAPSIN = CConfigValue<Config::IComplexConfigValue>("general:gaps_in");
            const auto*    GAPS    = sc<Config::CCssGapData*>(PGAPSIN.ptr());
            const bool     VISUAL  = !box.visualBox.empty();
            const CBox     TILE    = VISUAL ? box.visualBox : box.logicalBox;
            const Vector2D PSEUDO  = T->pseudoSize();
            const Vector2D GAPROOM =
                VISUAL || !GAPS ? Vector2D{} : Vector2D{sc<double>(GAPS->m_left + GAPS->m_right), sc<double>(GAPS->m_top + GAPS->m_bottom)};
            const Vector2D NEED = PSEUDO + GAPROOM + extra;
            if (TILE.w >= NEED.x && TILE.h >= NEED.y)
                return box;

            Vector2D by;
            if (const auto SPACE = T->space()) {
                const CBox&      WA    = SPACE->workArea();
                constexpr double SLACK = 2.0;
                if (TILE.x >= WA.x - SLACK && TILE.y >= WA.y - SLACK && TILE.x + TILE.w <= WA.x + WA.w + SLACK && TILE.y + TILE.h <= WA.y + WA.h + SLACK) {
                    // Where updatePos puts it: the pseudo box centred in the
                    // grown tile, less the border.
                    const auto     R   = W->getFullWindowReservedArea();
                    const Vector2D OWN = PSEUDO - (R.topLeft + R.bottomRight);
                    const Vector2D AT  = TILE.middle() - PSEUDO / 2.0 + R.topLeft;
                    by = Vector2D{std::clamp(AT.x, WA.x, std::max(WA.x, WA.x + WA.w - OWN.x)), std::clamp(AT.y, WA.y, std::max(WA.y, WA.y + WA.h - OWN.y))} - AT;
                }
            }
            Layout::STargetBox big = box;
            big.logicalBox         = grown(box.logicalBox, NEED, by);
            if (VISUAL)
                big.visualBox = grown(box.visualBox, NEED, by);
            return big;
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

    void place(void* target, const Layout::STargetBox& box, uint8_t flags, PlaceFn next) {
        auto* const T = sc<Layout::ITarget*>(target);
        const auto  W = T && !g_owned.empty() ? T->window() : PHLWINDOW{};
        if (!W || T->floating() || !T->isPseudo() || !owned(W) || Fullscreen::controller()->isFullscreen(W)) {
            next(target, box, flags);
            return;
        }
        const Layout::STargetBox BIG = roomFor(T, W, box, {});
        next(target, BIG, flags);
        if (BIG.logicalBox == box.logicalBox && BIG.visualBox == box.visualBox)
            return;
        // Squeezed even so (a workspace rule's own gaps, an aspect-ratio pad):
        // again, with its whole size to spare. Inside DockRoom's ConfigureHold,
        // so the program hears only the last size.
        const auto     R   = W->getFullWindowReservedArea();
        const Vector2D OWN = T->pseudoSize() - (R.topLeft + R.bottomRight);
        const Vector2D GOT = W->size(Desktop::View::IGeometric::GEOMETRIC_GOAL);
        if (GOT.x < OWN.x - 0.5 || GOT.y < OWN.y - 0.5)
            next(target, roomFor(T, W, box, T->pseudoSize()), flags);
        // The layout's own tile back (no updatePos): what it and Mosaic read.
        T->Layout::ITarget::setPositionGlobal(box, flags);
        trace(std::format("{} placed at its own size in a {:.0f}x{:.0f} tile", W->m_class, box.logicalBox.w, box.logicalBox.h));
    }

    void init() {
        const auto FNS = HyprlandAPI::findFunctionsByName(HYPRDRIVE_HANDLE, FLOAT_WINDOW);
        if (FNS.empty()) {
            trace("floatWindow not found: Hyprland's own toggle stands");
            return;
        }
        g_hook = HyprlandAPI::createFunctionHook(HYPRDRIVE_HANDLE, FNS[0].address, rc<void*>(hkFloatWindow));
        if (g_hook && !g_hook->hook())
            g_hook = nullptr;
        // Only with the toggle hooked: no own-size tile exists without it.
        const auto CFG = g_hook ? HyprlandAPI::findFunctionsByName(HYPRDRIVE_HANDLE, ON_X11_CONFIGURE) : std::vector<SFunctionMatch>{};
        if (!CFG.empty()) {
            g_configureHook = HyprlandAPI::createFunctionHook(HYPRDRIVE_HANDLE, CFG[0].address, rc<void*>(hkOnX11Configure));
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
