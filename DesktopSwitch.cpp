#include "DesktopSwitch.hpp"

#include "Config.hpp"
#include "DesktopStepModel.hpp"
#include "IOverview.hpp"
#include "globals.hpp"
#include "scrollOverview.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/config/shared/actions/ConfigActions.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/managers/KeybindManager.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/state/WorkspaceState.hpp>

#include <format>
#include <stdexcept>
#include <string>

namespace ScrollOverview::DesktopSwitch {
    namespace {
        using namespace ScrollOverview::DesktopStep;

        constexpr const char* KEY_SCROLL      = "plugin:hyprdrive:binds:scroll_desktops";
        constexpr const char* KEY_SCROLL_MODS = "plugin:hyprdrive:binds:scroll_desktops_mods";
        constexpr const char* KEY_ANIM_KEYS   = "plugin:hyprdrive:animation:desktop_keyboard";
        constexpr const char* KEY_ANIM_FOCUS  = "plugin:hyprdrive:animation:desktop_focus";

        // Depth counters, not flags: a focus change can nest inside another
        // (changeWorkspace focuses the desktop's last window on the way out).
        int g_ownDispatch = 0; // hyprdrive:desktop running
        int g_focusDepth  = 0; // Hyprland's rawWindowFocus, or the dock's activation

        SNotchAccumulator   g_notches;
        CHyprSignalListener g_axisHook, g_activeHook;
        CFunctionHook*      g_focusHook       = nullptr;
        CFunctionHook*      g_focusActionHook = nullptr;

        using origRawWindowFocus = void (*)(Desktop::CFocusState*, PHLWINDOW, Desktop::eFocusReason, SP<CWLSurfaceResource>);
        using origFocusAction    = ::Config::Actions::ActionResult (*)(PHLWINDOW);

        // Hyprland's focus entry point. When the window it is handed sits on a
        // desktop that is not shown, THIS is what calls changeWorkspace --
        // app activation (focus_on_activate), a notification's activate come through
        // here -- so a desktop change seen while it runs is a focus change.
        void hkRawWindowFocus(Desktop::CFocusState* self, PHLWINDOW window, Desktop::eFocusReason reason, SP<CWLSurfaceResource> surface) {
            ++g_focusDepth;
            rc<origRawWindowFocus>(g_focusHook->m_original)(self, window, reason, surface);
            --g_focusDepth;
        }

        // The focuswindow dispatcher (`hl.dsp.focus({ window = ... })`, hyprctl
        // dispatch focuswindow) switches the desktop ITSELF, before it focuses
        // -- rawWindowFocus then finds the window already shown -- so it is
        // marked at its own entry.
        ::Config::Actions::ActionResult hkFocusAction(PHLWINDOW window) {
            ++g_focusDepth;
            auto result = rc<origFocusAction>(g_focusActionHook->m_original)(window);
            --g_focusDepth;
            return result;
        }

        void* findFn(const std::string& name, const std::string& demangledNeedle) {
            for (const auto& FN : HyprlandAPI::findFunctionsByName(SCROLLOVERVIEW_HANDLE, name)) {
                if (FN.demangled.find(demangledNeedle) != std::string::npos)
                    return FN.address;
            }
            throw std::runtime_error(std::format("[hyprdrive] No fns for hook {}", name));
        }

        CFunctionHook* hookOrThrow(void* address, void* hook, const char* name) {
            auto* h = HyprlandAPI::createFunctionHook(SCROLLOVERVIEW_HANDLE, address, hook);
            if (!h || !h->hook())
                throw std::runtime_error(std::format("[hyprdrive] Could not hook {}", name));
            return h;
        }

        struct SOwnDispatch {
            SOwnDispatch() {
                ++g_ownDispatch;
            }
            ~SOwnDispatch() {
                --g_ownDispatch;
            }
        };

        uint32_t scrollMods() {
            if (!g_pKeybindManager)
                return 0;
            return g_pKeybindManager->stringToModMask(ScrollOverview::Config::getValue<std::string>(KEY_SCROLL_MODS));
        }

        // The step itself: by number on `monitor`, skipping desktops that live
        // on another monitor (going there would pull the focus across).
        SDispatchResult stepDesktop(const PHLMONITOR& monitor, int dir) {
            if (!monitor)
                return {.success = false, .error = "no focused monitor"};

            // The overview owns the desktops while it is up: move its focused
            // card, exactly as its own wheel and arrow keys do.
            if (const auto OVERVIEW = openScrollOverviewForMonitor(monitor)) {
                if (auto* S = dynamic_cast<CScrollOverview*>(OVERVIEW.get()))
                    S->stepDesktopCard(dir > 0);
                return {};
            }

            const auto ACTIVE = monitor->m_activeWorkspace;
            if (!ACTIVE)
                return {.success = false, .error = "no active desktop"};

            int highest = 0;
            for (const auto& REF : State::workspaceState()->workspaces()) {
                const auto WS = REF.lock();
                if (WS && !WS->m_isSpecialWorkspace && WS->m_monitor == monitor && WS->m_id > highest)
                    highest = sc<int>(WS->m_id);
            }

            const auto ELSEWHERE = [&monitor](int id) {
                for (const auto& REF : State::workspaceState()->workspaces()) {
                    const auto WS = REF.lock();
                    if (WS && !WS->m_isSpecialWorkspace && WS->m_id == id)
                        return WS->m_monitor != monitor;
                }
                return false;
            };

            const int CURRENT = sc<int>(ACTIVE->m_id);
            const int TARGET  = target(CURRENT, dir, highest, ELSEWHERE);
            if (TARGET == CURRENT)
                return {};

            // Hyprland's own `workspace N`: creates an empty desktop on the
            // focused monitor if need be, with its usual animation.
            SOwnDispatch own;
            (void)::Config::Actions::changeWorkspace(std::to_string(TARGET));
            return {};
        }

        SDispatchResult onDesktopDispatcher(std::string arg) {
            const auto FIRST = arg.find_first_not_of(" \t");
            arg              = FIRST == std::string::npos ? "" : arg.substr(FIRST, arg.find_last_not_of(" \t") - FIRST + 1);
            int dir          = 0;
            if (arg == "next" || arg == "+1")
                dir = 1;
            else if (arg == "prev" || arg == "previous" || arg == "-1")
                dir = -1;
            else
                return {.success = false, .error = "expected next or prev"};
            return stepDesktop(Desktop::focusState()->monitor(), dir);
        }

        PHLMONITOR monitorAtCursor() {
            const auto POINT = g_pInputManager->getMouseCoordsInternal();
            for (const auto& MONITOR : State::monitorState()->monitors()) {
                if (MONITOR && MONITOR->logicalBox().containsPoint(POINT))
                    return MONITOR;
            }
            return nullptr;
        }

        // ONE STEP PER NOTCH, ahead of the binds (the event bus runs before
        // CKeybindManager::onAxisEvent). On the configured chord a fraction of
        // a notch is swallowed: it must not fire the mouse_down bind, and it
        // must not restart binds:scroll_event_delay's clock either, or the
        // event that completes the notch would be thrown away. A whole notch
        // passes through to the bind untouched.
        void onAxis(IPointer::SAxisEvent e, Event::SCallbackInfo& info) {
            if (info.cancelled || e.source != WL_POINTER_AXIS_SOURCE_WHEEL || e.axis != WL_POINTER_AXIS_VERTICAL_SCROLL || e.deltaDiscrete == 0)
                return;
            if (!ScrollOverview::Config::getValue<bool>(KEY_SCROLL))
                return;
            const uint32_t MODS = g_pInputManager->getModsFromAllKBs() & ~(HL_MODIFIER_CAPS | HL_MODIFIER_MOD2);
            const uint32_t WANT = scrollMods();
            if (WANT == 0 || MODS != WANT)
                return;
            // The overview's wheel is its own (it steps its cards per notch,
            // throttled by input:scroll_event_delay) and it swallows the event.
            if (const auto MONITOR = monitorAtCursor(); MONITOR && scrollOverviewForMonitor(MONITOR))
                return;
            if (g_notches.feed(e.deltaDiscrete, e.timeMs) == 0)
                info.cancelled = true;
        }

        // A desktop has just been shown: Hyprland started both slides inside
        // changeWorkspace a moment ago. If this cause's switch is off, finish
        // them now -- Hyprland's own `instant` path (startAnimation(..., true))
        // is exactly a warp of these two values.
        void onActive(PHLWORKSPACE workspace) {
            if (!workspace || workspace->m_isSpecialWorkspace)
                return;
            const auto MONITOR = workspace->m_monitor.lock();
            if (!MONITOR || scrollOverviewForMonitor(MONITOR))
                return;

            const auto CAUSE = classify(g_ownDispatch > 0, g_pKeybindManager && g_pKeybindManager->m_currentKeybind, g_focusDepth > 0);
            if (!snaps(CAUSE, ScrollOverview::Config::getValue<bool>(KEY_ANIM_KEYS), ScrollOverview::Config::getValue<bool>(KEY_ANIM_FOCUS)))
                return;

            // The desktop coming in and the one going out: whatever on this
            // monitor is mid-slide. Nothing moving (the workspaces animation is
            // off, Omarchy's default) = nothing to do.
            for (const auto& REF : State::workspaceState()->workspaces()) {
                const auto WS = REF.lock();
                if (!WS || WS->m_isSpecialWorkspace || WS->m_monitor != MONITOR)
                    continue;
                if (WS->m_renderOffset && WS->m_renderOffset->isBeingAnimated())
                    WS->m_renderOffset->warp();
                if (WS->m_alpha && WS->m_alpha->isBeingAnimated())
                    WS->m_alpha->warp();
            }
        }
    }

    CFocusScope::CFocusScope() {
        ++g_focusDepth;
    }

    CFocusScope::~CFocusScope() {
        --g_focusDepth;
    }

    void init() {
        ScrollOverview::Config::registerDispatcher("desktop", onDesktopDispatcher);

        g_focusHook = hookOrThrow(findFn("rawWindowFocus", "CFocusState::rawWindowFocus("), rc<void*>(hkRawWindowFocus), "rawWindowFocus");
        g_focusActionHook =
            hookOrThrow(findFn("focus", "Config::Actions::focus(Hyprutils::Memory::CSharedPointer<Desktop::View::CWindow>)"), rc<void*>(hkFocusAction), "Config::Actions::focus");

        g_axisHook   = Event::bus()->m_events.input.mouse.axis.listen([](IPointer::SAxisEvent e, Event::SCallbackInfo& info) { onAxis(e, info); });
        g_activeHook = Event::bus()->m_events.workspace.active.listen([](PHLWORKSPACE workspace) { onActive(workspace); });
    }

    void shutdown() {
        g_axisHook.reset();
        g_activeHook.reset();
        if (g_focusHook) {
            // Unhooked here; Hyprland frees it with the plugin's other hooks.
            g_focusHook->unhook();
            g_focusHook = nullptr;
        }
        if (g_focusActionHook) {
            g_focusActionHook->unhook();
            g_focusActionHook = nullptr;
        }
        g_notches.reset();
    }
}
