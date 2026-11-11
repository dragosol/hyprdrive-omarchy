#include "DesktopSwitch.hpp"

#include "Config.hpp"
#include "DesktopRowModel.hpp"
#include "DesktopStepModel.hpp"
#include "DesktopStrip.hpp"
#include "FunctionHooks.hpp"
#include "IOverview.hpp"
#include "Mosaic.hpp"
#include "globals.hpp"
#include "Overview.hpp"

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

#include <algorithm>
#include <format>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace Hyprdrive::DesktopSwitch {
    namespace {
        using namespace Hyprdrive::DesktopStep;

        constexpr const char* KEY_SCROLL      = "plugin:hyprdrive:binds:scroll_desktops";
        constexpr const char* KEY_SCROLL_MODS = "plugin:hyprdrive:binds:scroll_desktops_mods";
        constexpr const char* KEY_ANIM_KEYS   = "plugin:hyprdrive:animation:desktop_keyboard";
        constexpr const char* KEY_ANIM_FOCUS  = "plugin:hyprdrive:animation:desktop_focus";
        // The desktop strip (DesktopStrip.hpp).
        constexpr const char* KEY_SMOOTH      = "plugin:hyprdrive:binds:scroll_desktops_smooth";
        constexpr const char* KEY_TICKS       = "plugin:hyprdrive:binds:scroll_desktops_ticks";
        constexpr const char* KEY_STICKY      = "plugin:hyprdrive:binds:scroll_desktops_sticky";
        constexpr const char* KEY_STOP_EMPTY  = "plugin:hyprdrive:binds:scroll_desktops_stop_at_empty";
        constexpr const char* KEY_POPULATED   = "plugin:hyprdrive:binds:scroll_desktops_populated_only";
        constexpr const char* KEY_BOUNCE      = "plugin:hyprdrive:animation:desktop_row_bounce";
        constexpr const char* KEY_JUMP        = "plugin:hyprdrive:animation:desktop_jump_through";

        // Depth counters, not flags: a focus change can nest inside another
        // (changeWorkspace focuses the desktop's last window on the way out).
        int g_ownDispatch = 0; // hyprdrive:desktop running
        int g_focusDepth  = 0; // Hyprland's rawWindowFocus, or the dock's activation

        SNotchAccumulator   g_notches; // a named desktop shown: the wheel steps the old way
        CHyprSignalListener g_axisHook, g_activeHook;
        // The desktop each monitor showed last: where a switch comes FROM.
        std::vector<std::pair<PHLMONITORREF, int>> g_shown;
        CFunctionHook*      g_focusHook       = nullptr;
        CFunctionHook*      g_focusActionHook = nullptr;

        using origRawWindowFocus = void (*)(Desktop::CFocusState*, PHLWINDOW, Desktop::eFocusReason, SP<CWLSurfaceResource>);
        using origFocusAction    = ::Config::Actions::ActionResult (*)(PHLWINDOW);

        // Hyprland's focus entry point. When the window it is handed sits on a
        // desktop that is not shown, THIS is what calls changeWorkspace --
        // app activation (focus_on_activate), a notification's activate come through
        // here -- so a desktop change seen while it runs is a focus change.
        void hkRawWindowFocus(Desktop::CFocusState* self, PHLWINDOW window, Desktop::eFocusReason reason, SP<CWLSurfaceResource> surface) {
            // Focus following the mouse onto a window that only peeks in at
            // the edge, or through the dock, focuses nothing
            // (Mosaic::vetoHoverFocus).
            if (reason == Desktop::FOCUS_REASON_FFM && Mosaic::vetoHoverFocus(window))
                return;
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

        // The FIRST function of `name` whose demangled signature contains
        // `demangledNeedle`, hooked. None found -> throw (no toast); found but
        // not created, or hook() failed -> throw. Same texts as before.
        CFunctionHook* hookOrThrow(const std::string& name, const std::string& demangledNeedle, void* hook, const char* label) {
            bool found = false;
            auto* h    = FunctionHooks::firstMatch("DesktopSwitch", name, hook, demangledNeedle, &found);
            if (!found)
                throw std::runtime_error(std::format("[hyprdrive] No fns for hook {}", name));
            if (!h || !h->hook())
                throw std::runtime_error(std::format("[hyprdrive] Could not hook {}", label));
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
            return g_pKeybindManager->stringToModMask(Hyprdrive::Config::getValue<std::string>(KEY_SCROLL_MODS));
        }

        // THE ROW the wheel walks on `monitor` (DesktopRowModel.hpp), `current`
        // shown: binds:scroll_desktops_stop_at_empty / _populated_only.
        std::vector<int> rowOn(const PHLMONITOR& monitor, int current) {
            DesktopRow::SFacts f{.current       = current,
                                 .stopAtEmpty   = Hyprdrive::Config::getValue<bool>(KEY_STOP_EMPTY),
                                 .populatedOnly = Hyprdrive::Config::getValue<bool>(KEY_POPULATED)};
            for (const auto& REF : State::workspaceState()->workspaces()) {
                const auto WS = REF.lock();
                if (!WS || WS->m_isSpecialWorkspace || WS->m_id < DesktopRow::FIRST)
                    continue;
                if (WS->m_monitor.lock() != monitor) {
                    f.elsewhere.push_back(sc<int>(WS->m_id));
                    continue;
                }
                f.existing.push_back(sc<int>(WS->m_id));
                if (WS->getWindowCount() > 0)
                    f.populated.push_back(sc<int>(WS->m_id));
            }
            return DesktopRow::row(f);
        }

        StripModel::SSettings stripSettings() {
            return {.smooth = Hyprdrive::Config::getValue<bool>(KEY_SMOOTH),
                    .ticks  = std::clamp(sc<int>(Hyprdrive::Config::getValue<Hyprlang::INT>(KEY_TICKS)), 1, 10),
                    .sticky = Hyprdrive::Config::getValue<bool>(KEY_STICKY),
                    .bounce = Hyprdrive::Config::getValue<bool>(KEY_BOUNCE)};
        }

        // The step itself: along the row on `monitor` -- never onto a desktop
        // of another monitor (going there would pull the focus across).
        SDispatchResult stepDesktop(const PHLMONITOR& monitor, int dir) {
            if (!monitor)
                return {.success = false, .error = "no focused monitor"};

            // The overview owns the desktops while it is up: move its focused
            // card, exactly as its own wheel and arrow keys do.
            if (const auto OVERVIEW = openScrollOverviewForMonitor(monitor)) {
                if (auto* S = dynamic_cast<COverview*>(OVERVIEW.get()))
                    S->stepDesktopCard(dir > 0);
                return {};
            }

            const auto ACTIVE = monitor->m_activeWorkspace;
            if (!ACTIVE)
                return {.success = false, .error = "no active desktop"};


            const int CURRENT = sc<int>(ACTIVE->m_id);
            const int TARGET  = DesktopRow::neighbour(rowOn(monitor, CURRENT), CURRENT, dir);
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
            if (!Hyprdrive::Config::getValue<bool>(KEY_SCROLL))
                return;
            const uint32_t MODS = g_pInputManager->getModsFromAllKBs() & ~(HL_MODIFIER_CAPS | HL_MODIFIER_MOD2);
            const uint32_t WANT = scrollMods();
            if (WANT == 0 || MODS != WANT)
                return;
            // The overview's wheel is its own (it steps its cards per notch,
            // throttled by input:scroll_event_delay) and it swallows the event.
            if (const auto MONITOR = monitorAtCursor(); MONITOR && scrollOverviewForMonitor(MONITOR))
                return;
            // THE WHEEL MOVES THE ROW (DesktopStrip.hpp; user, 2026-11-02: "if I
            // do two ticks it needs to be able to quickly go through two
            // desktops or more"). Taken here, the event swallowed: the bind it
            // would have fired is throttled by binds:scroll_event_delay (300
            // ms), which threw the second of two quick ticks away. Down = next,
            // as the bind.
            info.cancelled     = true;
            const auto MONITOR = Desktop::focusState()->monitor();
            if (DesktopStrip::onWheel(MONITOR, sc<int>(e.deltaDiscrete), stripSettings()))
                return;
            // A named desktop shown (not in the row): a desktop per notch.
            const int N = g_notches.feed(e.deltaDiscrete, e.timeMs);
            for (int i = 0; i < std::abs(N); ++i)
                stepDesktop(MONITOR, N > 0 ? 1 : -1);
        }

        // FROM WHERE IT IS (user, 2026-11-02: "I always want stuff to be
        // reversible"). Hyprland starts a switch by sliding the desktop going
        // out from wherever it is, but WARPS the one coming in a whole screen
        // away first: scrolling back mid-slide, or on to the next one, jumped.
        // The one coming in now starts right beside the one going out, where
        // it is this frame: back mid-way, it is the desktop that was leaving,
        // turned around where it stood; on to a third, it follows the second
        // in like the next card in a strip. The same slide, toward the same
        // place (setValue: no warp, so the one going out is still drawn).
        void continueFromHere(const PHLWORKSPACE& in, const PHLMONITOR& monitor) {
            if (!in->m_renderOffset)
                return;
            const Vector2D START = in->m_renderOffset->value(); // the side it comes from, a screen away
            if (std::abs(START.x) < 0.5 && std::abs(START.y) < 0.5)
                return; // not a slide (fade, or no animation)
            // The one going out heads exactly the other way, a screen off. Two
            // can (a third notch: the first desktop is still sliding away too):
            // it is the one most on screen.
            PHLWORKSPACE out;
            for (const auto& REF : State::workspaceState()->workspaces()) {
                const auto WS = REF.lock();
                if (!WS || WS == in || WS->m_isSpecialWorkspace || WS->m_monitor != monitor || !WS->m_renderOffset || !WS->m_renderOffset->isBeingAnimated())
                    continue;
                const Vector2D GOAL = WS->m_renderOffset->goal();
                if (std::abs(GOAL.x + START.x) > 1.0 || std::abs(GOAL.y + START.y) > 1.0)
                    continue;
                if (!out || WS->m_renderOffset->value().size() < out->m_renderOffset->value().size())
                    out = WS;
            }
            if (!out)
                return;
            const Vector2D NOW = out->m_renderOffset->value();
            if (std::abs(NOW.x) < 0.5 && std::abs(NOW.y) < 0.5)
                return; // it was at rest: an ordinary switch
            in->m_renderOffset->setValue(NOW + START);
        }

        // Finish every slide on `monitor` now: Hyprland's own `instant` path
        // (startAnimation(..., true)) is exactly a warp of these two values.
        void warpSlides(const PHLMONITOR& monitor) {
            for (const auto& REF : State::workspaceState()->workspaces()) {
                const auto WS = REF.lock();
                if (!WS || WS->m_isSpecialWorkspace || WS->m_monitor != monitor)
                    continue;
                if (WS->m_renderOffset && WS->m_renderOffset->isBeingAnimated())
                    WS->m_renderOffset->warp();
                if (WS->m_alpha && WS->m_alpha->isBeingAnimated())
                    WS->m_alpha->warp();
            }
        }

        // `monitor` now shows `id`: -> the desktop it showed before (0 unknown).
        int remember(const PHLMONITOR& monitor, int id) {
            std::erase_if(g_shown, [](const auto& e) { return !e.first.lock(); });
            for (auto& [MON, ID] : g_shown)
                if (MON.lock() == monitor)
                    return std::exchange(ID, id);
            g_shown.emplace_back(monitor, id);
            return 0;
        }

        PHLWORKSPACE desktopOn(const PHLMONITOR& monitor, int id) {
            for (const auto& REF : State::workspaceState()->workspaces()) {
                const auto WS = REF.lock();
                if (WS && !WS->m_isSpecialWorkspace && WS->m_id == id && WS->m_monitor == monitor)
                    return WS;
            }
            return nullptr;
        }

        // A desktop has just been shown: Hyprland started both slides inside
        // changeWorkspace a moment ago. Whose slide it is:
        //   * the strip's own switch, or one the strip claims (it is moving,
        //     or a jump through): Hyprland's slide ends now, the strip draws;
        //   * a cause whose animation switch is off: it lands now;
        //   * otherwise Hyprland's, continued from where things are.
        void onActive(PHLWORKSPACE workspace) {
            if (!workspace || workspace->m_isSpecialWorkspace)
                return;
            const auto MONITOR = workspace->m_monitor.lock();
            if (!MONITOR)
                return;
            const int TO   = sc<int>(workspace->m_id);
            const int FROM = remember(MONITOR, TO);
            if (scrollOverviewForMonitor(MONITOR))
                return;

            if (DesktopStrip::switching()) {
                warpSlides(MONITOR);
                return;
            }

            const auto CAUSE = classify(g_ownDispatch > 0, g_pKeybindManager && g_pKeybindManager->m_currentKeybind, g_focusDepth > 0);
            if (snaps(CAUSE, Hyprdrive::Config::getValue<bool>(KEY_ANIM_KEYS), Hyprdrive::Config::getValue<bool>(KEY_ANIM_FOCUS))) {
                DesktopStrip::finishNow();
                warpSlides(MONITOR);
                return;
            }

            // ANIMATION:DESKTOP_JUMP_THROUGH (user, 2026-11-02): a key or the
            // dock jumping several desktops slides past the ones between.
            const bool JUMP = (CAUSE == ECause::Keyboard || CAUSE == ECause::Focus) && Hyprdrive::Config::getValue<bool>(KEY_JUMP);
            if (DesktopStrip::claims(MONITOR, FROM, TO, JUMP)) {
                // Where the one going away was before Hyprland's slide moved it.
                const auto     OUT = desktopOn(MONITOR, FROM);
                const Vector2D WAS = OUT && OUT->m_renderOffset ? OUT->m_renderOffset->value() : Vector2D{};
                warpSlides(MONITOR);
                DesktopStrip::take(MONITOR, FROM, TO, WAS, JUMP);
                return;
            }

            continueFromHere(workspace, MONITOR);
        }
    }

    CFocusScope::CFocusScope() {
        ++g_focusDepth;
    }

    CFocusScope::~CFocusScope() {
        --g_focusDepth;
    }

    void preRender(PHLMONITOR monitor) {
        if (!monitor)
            return;
        // The overview opening owns the desktops: the row lands first.
        if (DesktopStrip::driving(monitor) && scrollOverviewForMonitor(monitor))
            DesktopStrip::finishNow();
        DesktopStrip::preRender(monitor);
    }

    std::string debugReport() {
        return DesktopStrip::debugReport();
    }

    void init() {
        Hyprdrive::Config::registerDispatcher("desktop", onDesktopDispatcher);
        DesktopStrip::init(rowOn, [](const PHLMONITOR&, int id) { (void)::Config::Actions::changeWorkspace(std::to_string(id)); });

        g_focusHook = hookOrThrow("rawWindowFocus", "CFocusState::rawWindowFocus(", rc<void*>(hkRawWindowFocus), "rawWindowFocus");
        g_focusActionHook =
            hookOrThrow("focus", "Config::Actions::focus(Hyprutils::Memory::CSharedPointer<Desktop::View::CWindow>)", rc<void*>(hkFocusAction), "Config::Actions::focus");

        g_axisHook   = Event::bus()->m_events.input.mouse.axis.listen([](IPointer::SAxisEvent e, Event::SCallbackInfo& info) { onAxis(e, info); });
        g_activeHook = Event::bus()->m_events.workspace.active.listen([](PHLWORKSPACE workspace) { onActive(workspace); });
    }

    void shutdown() {
        DesktopStrip::shutdown();
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
        g_shown.clear();
    }
}
