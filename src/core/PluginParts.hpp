#pragma once

// Internal to the plugin wiring that main.cpp was split into (Hooks.cpp,
// OverviewOpen.cpp, SearchInput.cpp, KeyRouter.cpp, GestureBinds.cpp,
// Dispatchers.cpp, DebugCommands.cpp). The state itself is DEFINED in main.cpp,
// in its original order; these are only declarations.

#define WLR_USE_UNSTABLE

#include "FullscreenGuard.hpp"
#include "XGeometry.hpp"
#include "OpenProf.hpp"
#include "FramePacing.hpp"
#include <unistd.h>
#include <hyprland/src/pointer/cursor/CursorShapeOverrideController.hpp>

#include <algorithm>
#include <chrono>
#include <expected>
#include <optional>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/state/GlobalWindowController.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/protocols/core/Compositor.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/managers/KeybindManager.hpp>
#include <hyprland/src/devices/IKeyboard.hpp>
#include <hyprland/src/devices/IPointer.hpp>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-compose.h>
#include <unordered_set>
#include <sys/eventfd.h>
#include <hyprland/src/managers/input/trackpad/GestureTypes.hpp>
#include <hyprland/src/managers/input/trackpad/TrackpadGestures.hpp>
#include <hyprland/src/managers/input/trackpad/gestures/CloseGesture.hpp>
#include <hyprland/src/managers/input/trackpad/gestures/CursorZoomGesture.hpp>
#include <hyprland/src/managers/input/trackpad/gestures/DispatcherGesture.hpp>
#include <hyprland/src/managers/input/trackpad/gestures/FloatGesture.hpp>
#include <hyprland/src/managers/input/trackpad/gestures/FullscreenGesture.hpp>
#include <hyprland/src/managers/input/trackpad/gestures/MoveGesture.hpp>
#include <hyprland/src/managers/input/trackpad/gestures/ResizeGesture.hpp>
#include <hyprland/src/managers/input/trackpad/gestures/SpecialWorkspaceGesture.hpp>
#include <hyprland/src/managers/input/trackpad/gestures/WorkspaceSwipeGesture.hpp>

#include <hyprutils/string/ConstVarList.hpp>
using namespace Hyprutils::String;

#include "ScratchpadExit.hpp"
#include "ToolSearch.hpp"
#include "TextInput.hpp"
#include "StackReader.hpp"
#include "DockDrop.hpp"
#include "StackBlur.hpp"
#include "Thumbs.hpp"
#include "DockStacks.hpp"
#include "AppInfo.hpp"
#include "CursorShape.hpp"
#include "globals.hpp"
#include "SearchLetters.hpp"
#include "KeyboardCat.hpp"
#include "Intent.hpp"
#include "GameApps.hpp"
#include "FullscreenDesktop.hpp"
#include "ShowDesktop.hpp"
#include "OwnSizeTile.hpp"
#include "Mosaic.hpp"
#include <hyprland/src/managers/SessionLockManager.hpp>

#include "Compat.hpp"
#include "AppSearch.hpp"
#include "SearchBackend.hpp"
#include "Config.hpp"
#include "DockTilt.hpp"
#include "Dock.hpp"
#include "DockStacks.hpp"
#include <hyprland/src/layout/LayoutManager.hpp>

#include "FlightController.hpp"
#include "FlightBlur.hpp"
#include "Motion.hpp"
#include "ConfigureHold.hpp"
#include "SizeAck.hpp"
#include "DockRoom.hpp"
#include "Scratchpad.hpp"
#include <hyprland/src/layout/target/Target.hpp>
#include "DockInput.hpp"
#include "LauncherEntry.hpp"
#include "Minimize.hpp"
#include "PluginVersion.hpp"
#include "Overview.hpp"
#include "DesktopStrip.hpp"
#include "DesktopSwitch.hpp"
#include "SearchSession.hpp"
#include "OverviewGesture.hpp"
#include "OverviewRender.hpp"
#include "SearchProviders.hpp"
#include "Window.hpp"
#include "WindowPill.hpp"
#include "Placement.hpp"
#include "FingerSwipe.hpp"
#include "Spring.hpp"
#include "HotCorner.hpp"
#include "Ripple.hpp"
#include "BorderFade.hpp"

typedef void (*origSendWindowSize)(void*, bool);
// Declared in OverviewInternal.hpp, which this file does not include.
namespace OvInternal {
    void overviewQueryEdited();
    void cancelPointerHandoff(); // Overview.cpp
    bool pointerOnTopLayer(PHLMONITOR monitor); // OverviewInput.cpp
}

// Tap-SUPER toggles the overview.
//
// This cannot live in a Lua bind: measured on 0.56.1, every bind form on
// SUPER_L fires identically whether SUPER was tapped alone or held as a
// modifier for another key, so a bind would open the overview on every
// SUPER+<key> shortcut. The tap/hold distinction only exists at the raw key
// event level.
// ponytail: raw evdev keycodes; a remapped SUPER will not be recognised.
constexpr uint32_t EVDEV_KEY_LEFTMETA  = 125;

constexpr uint32_t EVDEV_KEY_RIGHTMETA = 126;

constexpr uint32_t EVDEV_KEY_BACKSPACE = 14;

namespace PluginDetail {
    extern CFunctionHook* g_pScrollRenderWorkspaceHook;
    extern CFunctionHook* g_pScrollAddDamageHookA;
    extern CFunctionHook* g_pScrollAddDamageHookB;
    extern CFunctionHook* g_pScrollDamageSurfaceHook;
    extern CFunctionHook* g_pScrollScheduleFrameHook;
    extern CFunctionHook* g_pScrollSendFrameEventsHook;
    extern CFunctionHook* g_pScrollSurfaceFrameHook;
    extern CFunctionHook* g_pScrollMouseMovedHook;
    extern bool g_unloading;
    extern bool renderingOverview;
    extern bool damageFromSurface;
    extern PHLMONITORREF renderingOverviewMonitor;
    extern bool g_scrollOverviewHooksActive;
    extern bool g_dockCursor;
    extern CFunctionHook* g_pSendWindowSizeHook;
    extern CFunctionHook* g_pShouldRenderWindowHook;
    extern CFunctionHook* g_pTargetPlaceHook;
    extern std::unordered_set<uint32_t> g_heldGuardKeys;
    extern wl_event_source* g_letterTickTimer;
    extern std::unordered_set<uint32_t> g_consumedPresses;
    extern wl_event_source* g_searchRepeatTimer;
    extern uint32_t g_searchRepeatKey;
    extern CHyprSignalListener g_pRawKeyHook;
    extern CHyprSignalListener g_pTapSuperButtonHook;
    extern bool g_tapSuperCandidate;
    extern uint32_t g_tapSuperPressedAt;
    extern bool g_tapShiftCandidate;
    extern uint32_t g_tapShiftPressedAt;
    extern uint32_t g_tapShiftLastTapMs;
    extern bool g_tapCtrlCandidate;
    extern uint32_t g_tapCtrlPressedAt;
    extern uint32_t g_tapCtrlLastTapMs;

    extern void setDockCursor(bool on);
    extern void hkSendWindowSize(void* thisptr, bool force);
    extern bool hkShouldRenderWindow(void* thisptr, PHLWINDOW window, PHLMONITOR monitor);
    extern void hkOnAck(void* thisptr, uint32_t serial);
    extern void hkTargetPlace(void* thisptr, const Layout::STargetBox& box, uint8_t flags);
    extern void hkOnMouseMoved(void* thisptr, IPointer::SMotionEvent e);
    extern void hkScheduleFrame(void* thisptr, Aquamarine::IOutput::scheduleFrameReason reason);
    extern void hkRenderWorkspace(void* thisptr, PHLMONITOR pMonitor, PHLWORKSPACE pWorkspace, const Time::steady_tp& now, const CBox& geometry);
    extern void hkDamageSurface(void* thisptr, SP<CWLSurfaceResource> surface, double x, double y, double scale);
    extern void hkSendFrameEventsToWorkspace(void* thisptr, PHLMONITOR monitor, PHLWORKSPACE workspace, const Time::steady_tp& now);
    extern void hkSurfaceFrame(void* thisptr, const Time::steady_tp& now);
    extern void hkAddDamageA(void* thisptr, const CBox& box);
    extern void hkAddDamageB(void* thisptr, const pixman_region32_t* rg);
    extern std::optional<std::pair<PHLMONITOR, Vector2D>> monitorUnderCursor();
    extern SDispatchResult onOverviewDispatcher(std::string arg);
    extern bool isGuardModifierKeycode(uint32_t keycode);
    extern bool isGlobalPassthroughKeycode(uint32_t keycode);
    extern bool overviewAcceptsSearch();
    extern void resetCompose();
    extern void afterQueryEdit();
    extern bool handleSearchPress(uint32_t keycode);
    extern void stopSearchRepeat();
    extern void startSearchRepeat(uint32_t keycode);
    extern int onFileResultsReady(int fd, uint32_t mask, void* data);
    extern void onSessionLock();
    extern void registerRawKeyHooks();
    extern SDispatchResult onNavigateDispatcher(std::string arg);
    extern SDispatchResult onWindowDispatcher(std::string arg);
    extern void failNotif(const std::string& reason);
    extern void* findFnOrThrow(const std::string& name, std::initializer_list<std::string_view> demangledNeedles);
    extern std::expected<void, std::string> applyTrackpadGesture(size_t fingerCount, eTrackpadGestureDirection direction, const std::string& action, uint32_t modMask, float deltaScale, bool disableInhibit, const std::string& arg);
    extern SDispatchResult onRegisterOverviewGesture(size_t fingerCount, const std::string& directionStr, const std::string& action, const std::string& mods, float deltaScale, bool disableInhibit, const std::string& arg);
    extern Hyprlang::CParseResult overviewGestureKeyword(const char* LHS, const char* RHS);
    extern SDispatchResult onDebugDispatcher(std::string arg);
    extern SDispatchResult onGestureDispatcher(std::string arg);
    extern SDispatchResult onMinimizeDispatcher(std::string arg);
    extern SDispatchResult onHotkeyDispatcher(std::string arg);
    extern SDispatchResult onStackDispatcher(std::string arg);
} // namespace PluginDetail
