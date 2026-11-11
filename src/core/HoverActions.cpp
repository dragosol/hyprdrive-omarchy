#include "HoverActions.hpp"
#include "FunctionHooks.hpp"
#include "HoverTarget.hpp"
#include "globals.hpp"

#include <hyprland/src/config/shared/actions/ConfigActions.hpp>
#include <hyprland/src/debug/log/Logger.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>

#include <optional>

namespace Hyprdrive::HoverActions {
    namespace {
        using ::Config::Actions::ActionResult;
        using TMode  = ActionResult (*)(::Fullscreen::eFullscreenMode, bool, std::optional<PHLWINDOW>);
        using TModes = ActionResult (*)(::Fullscreen::eFullscreenMode, ::Fullscreen::eFullscreenMode, bool, std::optional<PHLWINDOW>);
        using TClose = ActionResult (*)(std::optional<PHLWINDOW>);

        constexpr const char* FULLSCREEN_MODE =
            "_ZN6Config7Actions16fullscreenWindowEN10Fullscreen15eFullscreenModeEbSt8optionalIN9Hyprutils6Memory14CSharedPointerIN7Desktop4View7CWindowEEEE";
        constexpr const char* FULLSCREEN_MODES =
            "_ZN6Config7Actions16fullscreenWindowEN10Fullscreen15eFullscreenModeES2_bSt8optionalIN9Hyprutils6Memory14CSharedPointerIN7Desktop4View7CWindowEEEE";
        constexpr const char* CLOSE_WINDOW = "_ZN6Config7Actions11closeWindowESt8optionalIN9Hyprutils6Memory14CSharedPointerIN7Desktop4View7CWindowEEEE";

        CFunctionHook* g_modeHook  = nullptr;
        CFunctionHook* g_modesHook = nullptr;
        CFunctionHook* g_closeHook = nullptr;

        // The window the action acts on (HoverActions.hpp): the one it was
        // called with, unless the user's keybinds mean another one right now
        // (HoverTarget: the overview's selected tile, else the dock preview
        // under the pointer).
        std::optional<PHLWINDOW> target(const std::optional<PHLWINDOW>& w) {
            if (w.has_value())
                return w;
            const auto HOVERED = HoverTarget::window();
            if (!HoverTarget::retarget(false, !!HOVERED, HOVERED == Desktop::focusState()->window()))
                return w;
            return HOVERED;
        }

        ActionResult hkMode(::Fullscreen::eFullscreenMode mode, bool layoutAware, std::optional<PHLWINDOW> w) {
            return rc<TMode>(g_modeHook->m_original)(mode, layoutAware, target(w));
        }

        ActionResult hkModes(::Fullscreen::eFullscreenMode internal, ::Fullscreen::eFullscreenMode client, bool layoutAware, std::optional<PHLWINDOW> w) {
            return rc<TModes>(g_modesHook->m_original)(internal, client, layoutAware, target(w));
        }

        ActionResult hkClose(std::optional<PHLWINDOW> w) {
            return rc<TClose>(g_closeHook->m_original)(target(w));
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

    void init() {
        g_modeHook  = hookOne(FULLSCREEN_MODE, rc<void*>(hkMode), "Super+F");
        g_modesHook = hookOne(FULLSCREEN_MODES, rc<void*>(hkModes), "Super+F");
        g_closeHook = hookOne(CLOSE_WINDOW, rc<void*>(hkClose), "Super+W");
    }
}
