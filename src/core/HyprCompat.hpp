#pragma once
// Hyprland 0.57 port: the names Hyprdrive used on 0.56 that no longer exist.
// Workspace ids were one signed integer (special workspaces negative); 0.57 has
// a variant (numbered | special). Hyprdrive keeps its integer keys.

// Every standard header first: the access hack below must never reach them.
#include <bits/stdc++.h>

// Force-included before any TU's own includes, so a per-file access hack is dead
// for every header pulled in here. The headers whose private members Hyprdrive
// reaches (docs/port057) are therefore opened here, once, for every TU.
#define private public
#define protected public
#include <hyprutils/animation/AnimatedVariable.hpp>
#include <hyprland/src/helpers/AnimatedVariable.hpp>
#include <hyprland/src/workspace/HLWorkspace.hpp>
#include <hyprland/src/workspace/SpecialWorkspace.hpp>
#include <hyprland/src/desktop/view/window/Window.hpp>
#include <hyprland/src/desktop/view/window/WindowMetadata.hpp>
#include <hyprland/src/desktop/view/window/WindowGroupMembership.hpp>
#include <hyprland/src/desktop/view/window/WindowPresentation.hpp>
#include <hyprland/src/desktop/view/window/WindowBackend.hpp>
#include <hyprland/src/desktop/view/window/WindowEffectsController.hpp>
#include <hyprland/src/desktop/view/window/WindowFullscreenPolicy.hpp>
#include <hyprland/src/desktop/view/window/X11Backend.hpp>
#include <hyprland/src/desktop/view/window/WaylandBackend.hpp>
#include <hyprland/src/xwayland/XSurface.hpp>
#include <hyprland/src/render/decorations/IHyprWindowDecoration.hpp>
#include <hyprland/src/render/decorations/AnimatedDecorationGradient.hpp>
#include <hyprland/src/render/decorations/CHyprBorderDecoration.hpp>
#include <hyprland/src/render/decorations/CHyprDropShadowDecoration.hpp>
#include <hyprland/src/render/transformer/TransformerList.hpp>
#include <hyprland/src/render/pass/Pass.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/keybinds/Bind.hpp>
#include <hyprland/src/keybinds/Manager.hpp>
#include <hyprland/src/managers/input/trackpad/TrackpadGestures.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/render/Shader.hpp>
#include <hyprland/src/render/types.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/input/Keys.hpp>
#include <hyprland/src/ipc/s2/S2.hpp>
#include <hyprland/src/state/WorkspaceState.hpp>
#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/config/lua/ConfigManager.hpp>
#include <hyprland/src/config/shared/actions/ConfigActions.hpp>
#include <hyprland/src/SharedDefs.hpp>
#undef protected
#undef private

using enum Input::eKeyboardModifiers;
using WORKSPACEID = int64_t;
inline constexpr WORKSPACEID WORKSPACE_INVALID = -1;

namespace Hyprdrive::Compat {
    inline bool isSpecial(const Workspace::CHLWorkspace* ws) {
        return ws && ws->type() == Workspace::eWorkspaceType::SPECIAL;
    }
    // A numbered workspace: its number. A special one: a stable negative id from
    // its name (0.56 handed out -99, -98, ... in creation order).
    inline WORKSPACEID id(const Workspace::CHLWorkspace* ws) {
        if (!ws)
            return WORKSPACE_INVALID;
        const auto ID = ws->id();
        if (const auto* N = std::get_if<Workspace::SWorkspaceNumberedID>(&ID))
            return sc<WORKSPACEID>(N->value);
        return -100 - sc<WORKSPACEID>(std::hash<std::string>{}(ws->addressableName()) % 1000000);
    }
    inline WORKSPACEID id(const PHLWORKSPACE& ws) { return id(ws.get()); }
    inline WORKSPACEID id(const PHLWORKSPACEREF& ws) { return id(ws.lock().get()); }
    template <typename T>
        requires requires(const T& t) { t.get(); }
    bool isSpecial(const T& ws) { return isSpecial(ws.get()); }
    inline bool pinned(const PHLWINDOW& w) { return w && (w->m_state & Desktop::View::WINDOW_STATE_PINNED); }
    inline bool urgent(const PHLWINDOW& w) { return w && (w->m_hints & Desktop::View::WINDOW_HINT_URGENT); }

    /// The live workspace whose Compat::id is `id` (numbered or special), or nullptr.
    inline PHLWORKSPACE workspaceByID(WORKSPACEID id) {
        for (const auto& w : State::workspaceState()->workspaces()) {
            if (const auto WS = w.lock(); WS && Compat::id(WS) == id)
                return WS;
        }
        return nullptr;
    }

    /// Socket2 event, as 0.56's g_pEventManager->postEvent(SHyprIPCEvent{event, data}); no-op without a socket.
    inline void postEvent(std::string event, std::string data) {
        if (const auto& SOCK = IPC::Socket2::sock())
            SOCK->postEvent({.event = std::move(event), .data = std::move(data)});
    }

    /// Mark an animated variable as animating (or not) without giving it a goal: what 0.56 did with
    /// `av->m_bIsBeingAnimated = b`, so CSurfacePassElement scales subsurfaces of a hand-driven size.
    inline void setBeingAnimated(Hyprutils::Animation::CBaseAnimatedVariable* variable, bool animated) {
        if (variable)
            variable->m_bIsBeingAnimated = animated;
    }

    // ---- Dispatchers by name. 0.57's addDispatcherV2 is a stub (no name -> function table).

    using TOwnDispatcher = std::function<SDispatchResult(std::string)>;
    /// Hyprdrive's own name -> dispatcher table (full names, e.g. "hyprdrive:overview" and the legacy spelling);
    /// Config::registerDispatcher fills it next to addDispatcherV2.
    std::unordered_map<std::string, TOwnDispatcher>& ownDispatchers();
    /// Run Hyprdrive's dispatcher `name` with `arg`; nullopt when `name` is not one of Hyprdrive's.
    std::optional<SDispatchResult> dispatchOwn(const std::string& name, std::string arg);
    /// Fallback for Hyprland's own dispatchers: evaluates `return hl.dispatch(<luaExpr>)` (e.g.
    /// "hl.dsp.window.close()"), as `hyprctl dispatch` does. Lua's result/error text; nullopt when not on a Lua config.
    std::optional<std::string> dispatchLua(const std::string& luaExpr);
    /// A dispatcher string that is already Lua ("hl.dsp.window.close()"): split it into name/arg never, run it whole.
    bool isLuaDispatch(std::string_view s);
    /// 0.56's dispatch by name: Hyprdrive's own, then Hyprland's legacy names (exec, workspace, ... as 0.56's
    /// DispatcherTranslator parsed them), cursorzoom, or a Lua expression in `name`. Anything else: an error, notified once per name.
    SDispatchResult dispatchNamed(const std::string& name, const std::string& arg);

    // ---- The bind Hyprland is running. 0.57 keeps no m_currentKeybind; a hook on
    // CKeybindManager::invokeBind records it for the duration of the call.

    /// Hook invokeBind (once, from plugin init). False when the function was not found.
    bool hookInvokeBind();
    /// The bind whose action is running right now (0.56's g_pKeybindManager->m_currentKeybind), or nullptr.
    Keybinds::PBind currentBind();
}

// Private members with no 0.57 accessor (reachable through the access block above).
namespace Hyprdrive::Priv {
    /// The window's XWayland surface (0.56's w->m_xwaylandSurface); nullptr for a Wayland window.
    inline SP<CXWaylandSurface> xsurface(const PHLWINDOW& w) {
        if (!w || w->backend().type() != Desktop::View::eBackendType::WINDOW_BACKEND_X11)
            return nullptr;
        return sc<Desktop::View::CX11Backend&>(w->backend()).m_xwaylandSurface.lock();
    }
    /// Size last sent in a configure and not yet acked, either backend (0.56's w->m_pendingReportedSize).
    inline std::optional<Vector2D>& pendingReportedSize(Desktop::View::CWindow& w) {
        if (w.backend().type() == Desktop::View::eBackendType::WINDOW_BACKEND_X11)
            return sc<Desktop::View::CX11Backend&>(w.backend()).m_pendingReportedSize;
        return sc<Desktop::View::CWaylandBackend&>(w.backend()).m_pendingReportedSize;
    }
    /// The raw hidden flag, written without setHidden()'s side effects (0.56's w->m_hidden).
    inline bool& hidden(Desktop::View::CWindow& w) { return w.m_hidden; }
    /// The raw suspended flag (0.56's w->m_suspended).
    inline bool& suspended(Desktop::View::CWindow& w) { return w.m_suspended; }
    /// The dim animation (0.56's w->m_dimPercent).
    inline PHLANIMVAR<float>& dimPercent(Desktop::View::CWindow& w) { return w.presentation().m_dimPercent; }
    /// The border's gradient state (0.56's m_realBorderColor{,Previous}, m_border{Fade,Angle}AnimationProgress); nullptr without a border.
    inline CAnimatedDecorationGradient* borderGradient(const PHLWINDOW& w) {
        const auto D = w ? w->presentation().decoration(DECORATION_BORDER) : nullptr;
        return D ? &sc<CHyprBorderDecoration*>(D.get())->m_gradient : nullptr;
    }
    /// The shadow's gradient state (0.56's m_realShadowColor, m_shadowFadeAnimationProgress); nullptr without a shadow.
    inline CAnimatedDecorationGradient* shadowGradient(const PHLWINDOW& w) {
        const auto D = w ? w->presentation().decoration(DECORATION_SHADOW) : nullptr;
        return D ? &sc<CHyprDropShadowDecoration*>(D.get())->m_gradient : nullptr;
    }
    /// The window's transformer vector (0.56's w->m_transformers); call sortTransformers(w) after inserting.
    inline std::vector<UP<Render::IWindowTransformer>>& transformers(Desktop::View::CWindow& w) { return w.effects().transformers()->m_transformers; }
    /// Re-sort the transformer list by priority after a hand insert.
    inline void sortTransformers(Desktop::View::CWindow& w) { w.effects().transformers()->sort(); }
    /// Set or clear one flag of a live bind (0.56 wrote k->nonConsuming / k->submapUniversal); Hyprland reads flags live.
    inline void setBindFlag(Keybinds::CBind& bind, Keybinds::eBindFlags flag, bool on) {
        if (on)
            bind.m_flags |= flag;
        else
            bind.m_flags &= sc<Keybinds::BindFlags>(~flag);
    }
}
