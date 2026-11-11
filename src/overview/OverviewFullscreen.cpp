#include "OverviewFullscreen.hpp"
#include "FunctionHooks.hpp"
#include "HoverTarget.hpp"
#include "globals.hpp"

#include <hyprland/src/config/shared/actions/ConfigActions.hpp>
#include <hyprland/src/debug/log/Logger.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>

#include <optional>

namespace Hyprdrive::OverviewFullscreen {
    namespace {
        using ::Config::Actions::ActionResult;
        using TMode  = ActionResult (*)(::Fullscreen::eFullscreenMode, bool, std::optional<PHLWINDOW>);
        using TModes = ActionResult (*)(::Fullscreen::eFullscreenMode, ::Fullscreen::eFullscreenMode, bool, std::optional<PHLWINDOW>);

        constexpr const char* FULLSCREEN_MODE =
            "_ZN6Config7Actions16fullscreenWindowEN10Fullscreen15eFullscreenModeEbSt8optionalIN9Hyprutils6Memory14CSharedPointerIN7Desktop4View7CWindowEEEE";
        constexpr const char* FULLSCREEN_MODES =
            "_ZN6Config7Actions16fullscreenWindowEN10Fullscreen15eFullscreenModeES2_bSt8optionalIN9Hyprutils6Memory14CSharedPointerIN7Desktop4View7CWindowEEEE";

        CFunctionHook* g_modeHook  = nullptr;
        CFunctionHook* g_modesHook = nullptr;

        // The window the action acts on (OverviewFullscreen.hpp): the one it
        // was called with, unless the user's keybinds mean another one right
        // now (HoverTarget: the overview's selected tile).
        std::optional<PHLWINDOW> target(const std::optional<PHLWINDOW>& w) {
            if (w.has_value())
                return w;
            const auto TILE = HoverTarget::window();
            if (!retarget(false, !!TILE, !!TILE, TILE == Desktop::focusState()->window()))
                return w;
            return TILE;
        }

        ActionResult hkMode(::Fullscreen::eFullscreenMode mode, bool layoutAware, std::optional<PHLWINDOW> w) {
            return rc<TMode>(g_modeHook->m_original)(mode, layoutAware, target(w));
        }

        ActionResult hkModes(::Fullscreen::eFullscreenMode internal, ::Fullscreen::eFullscreenMode client, bool layoutAware, std::optional<PHLWINDOW> w) {
            return rc<TModes>(g_modesHook->m_original)(internal, client, layoutAware, target(w));
        }

        CFunctionHook* hookOne(const char* name, void* destination) {
            bool found = false;
            auto* hook = FunctionHooks::firstMatch("OverviewFullscreen", name, destination, {}, &found);
            if (!found)
                Log::logger->log(Log::WARN, "[hyprdrive] {} not found: Super+F in the overview acts on the focused window", name);
            if (hook && !hook->hook())
                hook = nullptr;
            return hook;
        }
    }

    void init() {
        g_modeHook  = hookOne(FULLSCREEN_MODE, rc<void*>(hkMode));
        g_modesHook = hookOne(FULLSCREEN_MODES, rc<void*>(hkModes));
    }
}
