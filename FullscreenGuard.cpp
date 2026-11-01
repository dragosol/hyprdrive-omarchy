// See FullscreenGuard.hpp.
#include "FullscreenGuard.hpp"
#include "Config.hpp"
#include "IOverview.hpp"
#include "Minimize.hpp"

#include <algorithm>
#include <cctype>
#include <string>

#include <hyprland/src/desktop/view/WLSurface.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/protocols/core/Compositor.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/protocols/PointerConstraints.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/protocols/types/ContentType.hpp>

namespace ScrollOverview::FullscreenGuard {
    namespace {
        bool g_inside = false; // minimizing: its own focus handoff is not another pick

        bool startsWith(const std::string& s, const char* p) {
            return s.rfind(p, 0) == 0;
        }

        bool endsWithExe(std::string s) {
            std::ranges::transform(s, s.begin(), [](unsigned char c) { return std::tolower(c); });
            return s.size() > 4 && s.ends_with(".exe");
        }
    }

    bool isGame(const PHLWINDOW& w) {
        if (!w)
            return false;
        if (w->getContentType() == NContentType::CONTENT_TYPE_GAME)
            return true;
        for (const auto& CLASS : {w->m_class, w->m_initialClass}) {
            if (startsWith(CLASS, "steam_app_") || CLASS == "gamescope")
                return true;
            if (w->m_isX11 && endsWithExe(CLASS))
                return true;
        }
        return false;
    }

    PHLWINDOW gameCover(const PHLWINDOW& target) {
        if (!target || !target->m_isMapped || target->m_isFloating || !target->m_workspace)
            return nullptr;
        // Mode 0: the fullscreen window keeps the focus, nothing leaves fullscreen.
        if (Config::getValue<int>("misc:on_focus_under_fullscreen") == 0)
            return nullptr;
        const auto COVER = Fullscreen::controller()->getFullscreenWindow(target->m_workspace);
        if (!COVER || COVER == target || Fullscreen::controller()->getFullscreenModes(COVER).internal != Fullscreen::FSMODE_FULLSCREEN ||
            Minimize::isMinimized(COVER) || !isGame(COVER))
            return nullptr;
        return COVER;
    }

    void resumePointerLock() {
        if (!g_pInputManager || !g_pSeatManager || g_pInputManager->m_constraints.empty())
            return;
        // The overview owns the pointer while one is up (or closing).
        if (!scrollOverviews().empty())
            return;
        const auto SURF = Desktop::focusState()->surface();
        if (!SURF || g_pSeatManager->m_state.pointerFocus != SURF)
            return;
        const auto HL = Desktop::View::CWLSurface::fromResource(SURF);
        const auto C  = HL ? HL->constraint() : nullptr;
        if (!C || C->isActive())
            return;
        C->activate();
    }

    void beforeFocus(const PHLWINDOW& target, Desktop::eFocusReason reason) {
        (void)reason;
        if (g_inside)
            return;
        const auto COVER = gameCover(target);
        if (!COVER)
            return;
        g_inside = true;
        Minimize::minimize(COVER, false);
        g_inside = false;
    }

    namespace {
        CFunctionHook* g_focusHook = nullptr;
        using origFullWindowFocus  = void (*)(void*, PHLWINDOW, Desktop::eFocusReason, SP<CWLSurfaceResource>, bool);
        void hkFullWindowFocus(void* thisptr, PHLWINDOW w, Desktop::eFocusReason reason, SP<CWLSurfaceResource> surface, bool forceFSCycle) {
            beforeFocus(w, reason);
            rc<origFullWindowFocus>(g_focusHook->m_original)(thisptr, w, reason, surface, forceFSCycle);
        }
    }

    void installFocusHook() {
        const auto FNS = HyprlandAPI::findFunctionsByName(
            SCROLLOVERVIEW_HANDLE, "_ZN7Desktop11CFocusState15fullWindowFocusEN9Hyprutils6Memory14CSharedPointerINS_4View7CWindowEEENS_12eFocusReasonENS3_I18CWLSurfaceResourceEEb");
        if (FNS.empty())
            return; // no game guard: Hyprland's own exit-fullscreen stands
        g_focusHook = HyprlandAPI::createFunctionHook(SCROLLOVERVIEW_HANDLE, FNS[0].address, rc<void*>(hkFullWindowFocus));
        if (g_focusHook && !g_focusHook->hook())
            g_focusHook = nullptr;
    }
}
