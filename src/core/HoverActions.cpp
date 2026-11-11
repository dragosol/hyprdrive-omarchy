#include "HoverActions.hpp"
#include "FunctionHooks.hpp"
#include "HoverTarget.hpp"
#include "globals.hpp"

#include <hyprland/src/config/shared/actions/ConfigActions.hpp>
#include <hyprland/src/debug/log/Logger.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/managers/KeybindManager.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>

#include <optional>

namespace Hyprdrive::HoverActions {
    namespace {
        using ::Config::Actions::ActionResult;
        using TMode  = ActionResult (*)(::Fullscreen::eFullscreenMode, bool, std::optional<PHLWINDOW>);
        using TModes = ActionResult (*)(::Fullscreen::eFullscreenMode, ::Fullscreen::eFullscreenMode, bool, std::optional<PHLWINDOW>);
        using TClose = ActionResult (*)(std::optional<PHLWINDOW>);
        using TPin   = ActionResult (*)(::Config::Actions::eTogglableAction, std::optional<PHLWINDOW>);
        using TMove  = ActionResult (*)(PHLWORKSPACE, bool, std::optional<PHLWINDOW>);

        constexpr const char* FULLSCREEN_MODE =
            "_ZN6Config7Actions16fullscreenWindowEN10Fullscreen15eFullscreenModeEbSt8optionalIN9Hyprutils6Memory14CSharedPointerIN7Desktop4View7CWindowEEEE";
        constexpr const char* FULLSCREEN_MODES =
            "_ZN6Config7Actions16fullscreenWindowEN10Fullscreen15eFullscreenModeES2_bSt8optionalIN9Hyprutils6Memory14CSharedPointerIN7Desktop4View7CWindowEEEE";
        constexpr const char* CLOSE_WINDOW = "_ZN6Config7Actions11closeWindowESt8optionalIN9Hyprutils6Memory14CSharedPointerIN7Desktop4View7CWindowEEEE";
        constexpr const char* PIN_WINDOW =
            "_ZN6Config7Actions9pinWindowENS0_16eTogglableActionESt8optionalIN9Hyprutils6Memory14CSharedPointerIN7Desktop4View7CWindowEEEE";
        constexpr const char* MOVE_TO_WORKSPACE =
            "_ZN6Config7Actions15moveToWorkspaceEN9Hyprutils6Memory14CSharedPointerI10CWorkspaceEEbSt8optionalINS3_IN7Desktop4View7CWindowEEEE";

        CFunctionHook* g_modeHook  = nullptr;
        CFunctionHook* g_modesHook = nullptr;
        CFunctionHook* g_closeHook = nullptr;
        CFunctionHook* g_pinHook   = nullptr;
        CFunctionHook* g_moveHook  = nullptr;

        // SOwnCall's depth: the plugin's own code, or a hooked action's
        // original, is running.
        int g_own = 0;

        // Each hook: the window from target(), then Hyprland's own action
        // with every call it makes in turn left alone (SOwnCall).
        ActionResult hkMode(::Fullscreen::eFullscreenMode mode, bool layoutAware, std::optional<PHLWINDOW> w) {
            w = target(w);
            SOwnCall own;
            return rc<TMode>(g_modeHook->m_original)(mode, layoutAware, w);
        }

        ActionResult hkModes(::Fullscreen::eFullscreenMode internal, ::Fullscreen::eFullscreenMode client, bool layoutAware, std::optional<PHLWINDOW> w) {
            w = target(w);
            SOwnCall own;
            return rc<TModes>(g_modesHook->m_original)(internal, client, layoutAware, w);
        }

        ActionResult hkClose(std::optional<PHLWINDOW> w) {
            w = target(w);
            SOwnCall own;
            return rc<TClose>(g_closeHook->m_original)(w);
        }

        ActionResult hkPin(::Config::Actions::eTogglableAction action, std::optional<PHLWINDOW> w) {
            w = target(w);
            SOwnCall own;
            return rc<TPin>(g_pinHook->m_original)(action, w);
        }

        ActionResult hkMove(PHLWORKSPACE ws, bool silent, std::optional<PHLWINDOW> w) {
            bool swapped = false;
            w            = target(w, &swapped);
            SOwnCall own;
            return rc<TMove>(g_moveHook->m_original)(ws, HoverTarget::moveSilently(swapped, silent), w);
        }

        CFunctionHook* hookOne(const char* name, void* destination, const char* what) {
            bool found = false;
            auto* hook = FunctionHooks::firstMatch("HoverActions", name, destination, {}, &found);
            if (!found)
                Log::logger->log(Log::WARN, "[hyprdrive] {} not found: {} on a hovered tile or dock preview acts on the focused window", name, what);
            if (hook && !hook->hook())
                hook = nullptr;
            return hook;
        }
    }

    SOwnCall::SOwnCall() {
        ++g_own;
    }
    SOwnCall::~SOwnCall() {
        --g_own;
    }

    // The window the action acts on (HoverActions.hpp): the one it was
    // called with, unless the user's keybinds mean another one right now
    // (HoverTarget: the overview's selected tile, else the dock preview
    // under the pointer).
    std::optional<PHLWINDOW> target(const std::optional<PHLWINDOW>& w, bool* swapped) {
        if (swapped)
            *swapped = false;
        if (g_own > 0)
            return w;
        const auto FOCUSED = Desktop::focusState()->window();
        const bool INBIND  = g_pKeybindManager && g_pKeybindManager->m_currentKeybind;
        const bool NAMED   = w.has_value();
        if (NAMED && !(INBIND && *w && *w == FOCUSED))
            return w; // named, and not "the active window" from a bind: the caller's
        const auto HOVERED = HoverTarget::window();
        if (!HoverTarget::retarget(NAMED, NAMED && INBIND && *w == FOCUSED, g_own > 0, !!HOVERED, HOVERED == FOCUSED))
            return w;
        if (swapped)
            *swapped = true;
        return HOVERED;
    }

    void init() {
        g_modeHook  = hookOne(FULLSCREEN_MODE, rc<void*>(hkMode), "Super+F");
        g_modesHook = hookOne(FULLSCREEN_MODES, rc<void*>(hkModes), "Super+F");
        g_closeHook = hookOne(CLOSE_WINDOW, rc<void*>(hkClose), "Super+W");
        g_pinHook   = hookOne(PIN_WINDOW, rc<void*>(hkPin), "pin");
        g_moveHook  = hookOne(MOVE_TO_WORKSPACE, rc<void*>(hkMove), "Super+Shift+N");
    }
}
