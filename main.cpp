#define WLR_USE_UNSTABLE

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

#include "Compat.hpp"
#include "AppSearch.hpp"
#include "OmarchySearch.hpp"
#include "Config.hpp"
#include "DockTilt.hpp"
#include "Dock.hpp"
#include "DockStacks.hpp"
#include <hyprland/src/layout/LayoutManager.hpp>

#include "FlightController.hpp"
#include "FlightBlur.hpp"
#include "ConfigureHold.hpp"
#include "DockRoom.hpp"
#include "Scratchpad.hpp"
#include <hyprland/src/layout/target/Target.hpp>
#include "DockInput.hpp"
#include "LauncherEntry.hpp"
#include "Minimize.hpp"
#include "PluginVersion.hpp"
#include "scrollOverview.hpp"
#include "SearchSession.hpp"
#include "OverviewGesture.hpp"
#include "OverviewRender.hpp"
#include "SearchProviders.hpp"
#include "Window.hpp"
#include "WindowPill.hpp"
#include "Placement.hpp"

// Methods
static CFunctionHook* g_pScrollRenderWorkspaceHook = nullptr;
static CFunctionHook* g_pScrollAddDamageHookA      = nullptr;
static CFunctionHook* g_pScrollAddDamageHookB      = nullptr;
static CFunctionHook* g_pScrollDamageSurfaceHook   = nullptr;
static CFunctionHook* g_pScrollScheduleFrameHook   = nullptr;
static CFunctionHook* g_pScrollSendFrameEventsHook = nullptr;
static CFunctionHook* g_pScrollSurfaceFrameHook    = nullptr;
static CFunctionHook* g_pScrollMouseMovedHook      = nullptr;
typedef void (*origRenderWorkspace)(void*, PHLMONITOR, PHLWORKSPACE, const Time::steady_tp&, const CBox&);
typedef void (*origAddDamageA)(void*, const CBox&);
typedef void (*origAddDamageB)(void*, const pixman_region32_t*);
typedef void (*origDamageSurface)(void*, SP<CWLSurfaceResource>, double, double, double);
typedef void (*origScheduleFrame)(void*, Aquamarine::IOutput::scheduleFrameReason);
typedef void (*origSendFrameEventsToWorkspace)(void*, PHLMONITOR, PHLWORKSPACE, const Time::steady_tp&);
typedef void (*origSurfaceFrame)(void*, const Time::steady_tp&);
typedef void (*origOnMouseMoved)(void*, IPointer::SMotionEvent);

static bool g_unloading = false;

// Do NOT change this function.
APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

static bool renderingOverview = false;
static bool damageFromSurface = false;
static PHLMONITORREF renderingOverviewMonitor;
static bool g_scrollOverviewHooksActive = false;

static void failNotif(const std::string& reason);

bool ensureScrollOverviewHooks() {
    if (g_scrollOverviewHooksActive)
        return true;

    bool success = g_pScrollRenderWorkspaceHook->hook();
    success      = success && g_pScrollScheduleFrameHook->hook();
    success      = success && g_pScrollDamageSurfaceHook->hook();
    success      = success && g_pScrollSendFrameEventsHook->hook();
    success      = success && g_pScrollSurfaceFrameHook->hook();
    success      = success && g_pScrollAddDamageHookA->hook();
    success      = success && g_pScrollAddDamageHookB->hook();

    if (!success) {
        disableScrollOverviewHooks();
        failNotif("Failed enabling overview hooks (is other overview plugin enabled?)");
        return false;
    }

    g_scrollOverviewHooksActive = true;
    return true;
}

void disableScrollOverviewHooks() {
    if (g_pScrollAddDamageHookB)
        g_pScrollAddDamageHookB->unhook();
    if (g_pScrollAddDamageHookA)
        g_pScrollAddDamageHookA->unhook();
    if (g_pScrollSurfaceFrameHook)
        g_pScrollSurfaceFrameHook->unhook();
    if (g_pScrollSendFrameEventsHook)
        g_pScrollSendFrameEventsHook->unhook();
    if (g_pScrollDamageSurfaceHook)
        g_pScrollDamageSurfaceHook->unhook();
    if (g_pScrollScheduleFrameHook)
        g_pScrollScheduleFrameHook->unhook();
    if (g_pScrollRenderWorkspaceHook)
        g_pScrollRenderWorkspaceHook->unhook();

    g_scrollOverviewHooksActive = false;
}

// X30. The autohide barrier needs the RELATIVE motion, which only this call
// carries: the bus's mouse.move gives the absolute position, and at the bottom
// edge the cursor is clamped, so the position stops changing while the user is
// still pushing. Fed AFTER the original, so the position the barrier tests
// against is the one this motion produced.
// The arrow over the dock's chrome on the desktop (see g_pDockHoverHook).
static bool g_dockCursor = false;
static void setDockCursor(bool on) {
    if (on == g_dockCursor || !Pointer::Cursor::overrideController)
        return;
    g_dockCursor = on;
    if (on)
        Pointer::Cursor::overrideController->setOverride("left_ptr", Pointer::Cursor::CURSOR_OVERRIDE_UNKNOWN);
    else
        Pointer::Cursor::overrideController->unsetOverride(Pointer::Cursor::CURSOR_OVERRIDE_UNKNOWN);
}

// ConfigureHold.hpp: a gesture's in-between sizes never reach the client.
static CFunctionHook* g_pSendWindowSizeHook = nullptr;
typedef void (*origSendWindowSize)(void*, bool);
static void hkSendWindowSize(void* thisptr, bool force) {
    if (ScrollOverview::ConfigureHold::intercept(sc<Desktop::View::CWindow*>(thisptr), force))
        return;
    rc<origSendWindowSize>(g_pSendWindowSizeHook->m_original)(thisptr, force);
}

// DockRoom.hpp: tiled windows make room for a dock that does not hide.
static CFunctionHook* g_pTargetPlaceHook = nullptr;
static void hkTargetPlace(void* thisptr, const Layout::STargetBox& box, uint8_t flags) {
    ScrollOverview::DockRoom::place(thisptr, box, flags, [](void* t, const Layout::STargetBox& b, uint8_t f) {
        rc<void (*)(void*, const Layout::STargetBox&, uint8_t)>(g_pTargetPlaceHook->m_original)(t, b, f);
    });
}

static void hkOnMouseMoved(void* thisptr, IPointer::SMotionEvent e) {
    rc<origOnMouseMoved>(g_pScrollMouseMovedHook->m_original)(thisptr, e);
    ScrollOverview::Dock::feedPointerMotion(e.delta);
}

static void hkScheduleFrame(void* thisptr, Aquamarine::IOutput::scheduleFrameReason reason) {
    const auto OVERVIEW = scrollOverviewForMonitor(sc<Monitor::CMonitor*>(thisptr)->m_self.lock());
    if (OVERVIEW) {
        using enum Aquamarine::IOutput::scheduleFrameReason;

        const bool THROTTLEDREASON =
            reason == AQ_SCHEDULE_UNKNOWN || reason == AQ_SCHEDULE_CLIENT_UNKNOWN || reason == AQ_SCHEDULE_NEEDS_FRAME || reason == AQ_SCHEDULE_RENDER_MONITOR ||
            reason == AQ_SCHEDULE_DAMAGE;

        if (THROTTLEDREASON && !OVERVIEW->blockDamageReporting && !OVERVIEW->shouldAllowRealtimePreviewSchedule())
            return;
    }

    rc<origScheduleFrame>(g_pScrollScheduleFrameHook->m_original)(thisptr, reason);
}

//
static void hkRenderWorkspace(void* thisptr, PHLMONITOR pMonitor, PHLWORKSPACE pWorkspace, const Time::steady_tp& now, const CBox& geometry) {
    const auto OVERVIEW = scrollOverviewForMonitor(pMonitor);
    if (!OVERVIEW || renderingOverview)
        rc<origRenderWorkspace>(g_pScrollRenderWorkspaceHook->m_original)(thisptr, pMonitor, pWorkspace, now, geometry);
    else {
        const bool PREVRENDERINGOVERVIEW = renderingOverview;
        const auto PREVRENDERINGMONITOR  = renderingOverviewMonitor;
        renderingOverview                = true;
        renderingOverviewMonitor         = pMonitor;
        OVERVIEW->render();
        renderingOverviewMonitor = PREVRENDERINGMONITOR;
        renderingOverview = PREVRENDERINGOVERVIEW;
    }
}

static void hkDamageSurface(void* thisptr, SP<CWLSurfaceResource> surface, double x, double y, double scale) {
    // Y17 (GPU, 2026-09-14). The overview's U2 prefilter keeps a mipmapped copy
    // of each surface's texture and no longer rebuilds it per draw; this is the
    // "that copy is stale" signal. Unconditional, and BEFORE the gate below:
    // whether a given overview wants the damage reported is a different
    // question from whether the client's pixels changed.
    OverviewWindow::notifySurfaceCommit(surface);
    if (scrollOverviews().empty())
        ScrollOverview::noteWallpaperCommit(surface);

    const bool HANDLED = scrollOverviews().empty() ||
        std::ranges::any_of(scrollOverviews(), [](const auto& overview) { return overview && overview->blockDamageReporting; }) ||
        std::ranges::all_of(scrollOverviews(), [&surface](const auto& overview) { return !overview || overview->shouldHandleSurfaceDamage(surface); });
    if (HANDLED) {
        const bool PREVDAMAGEFROMSURFACE = damageFromSurface;
        damageFromSurface                = !scrollOverviews().empty();
        rc<origDamageSurface>(g_pScrollDamageSurfaceHook->m_original)(thisptr, surface, x, y, scale);
        damageFromSurface = PREVDAMAGEFROMSURFACE;
    }
}

static void hkSendFrameEventsToWorkspace(void* thisptr, PHLMONITOR monitor, PHLWORKSPACE workspace, const Time::steady_tp& now) {
    if (scrollOverviewForMonitor(monitor))
        return;

    rc<origSendFrameEventsToWorkspace>(g_pScrollSendFrameEventsHook->m_original)(thisptr, monitor, workspace, now);
}

static void hkSurfaceFrame(void* thisptr, const Time::steady_tp& now) {
    const auto SURFACE = sc<CWLSurfaceResource*>(thisptr)->m_self.lock();

    if (std::ranges::any_of(scrollOverviews(), [&SURFACE, &now](const auto& overview) { return overview && !overview->shouldAllowSurfaceFrame(SURFACE, now); }))
        return;

    rc<origSurfaceFrame>(g_pScrollSurfaceFrameHook->m_original)(thisptr, now);
}

// CMonitor::addDamage has two overloads (a CBox, a pixman region); we hook both
// and the suppress decision is identical for either payload. It is the ONE thing
// the two hooks share, so it lives here and the hooks are one line each. Only the
// overview's own render is ever suppressed; everything else passes straight to the
// original -- the old code's `!OVERVIEW || blockDamageReporting || damageFromSurface`
// branch and the fall-through both called the original with the same args, so it
// was dead weight and is gone.
static bool suppressAddDamage(void* thisptr) {
    const auto PMONITOR = sc<Monitor::CMonitor*>(thisptr);
    const auto OVERVIEW = scrollOverviewForMonitor(PMONITOR->m_self.lock());
    return OVERVIEW && renderingOverviewMonitor.lock() == PMONITOR->m_self.lock() && !damageFromSurface && OVERVIEW->shouldSuppressRenderDamage();
}

static void hkAddDamageA(void* thisptr, const CBox& box) {
    if (suppressAddDamage(thisptr))
        return;
    rc<origAddDamageA>(g_pScrollAddDamageHookA->m_original)(thisptr, box);
}

static void hkAddDamageB(void* thisptr, const pixman_region32_t* rg) {
    if (suppressAddDamage(thisptr))
        return;
    rc<origAddDamageB>(g_pScrollAddDamageHookB->m_original)(thisptr, rg);
}

// The desktop dock's pointer hooks all begin the same way: take the cursor, find
// the monitor it is over, SKIP any monitor whose overview is up (that one has its
// own input path -- letting the desktop hook fire there too is a double-handle),
// and hand back the pill-local point. Five hooks pasted this preamble; the one
// that forgot the overview-skip would be the drift this phase exists to kill. Now
// there is one copy. Returns nullopt when the cursor is over no plain-desktop
// monitor (an empty result means "not my click", same as the old loop falling
// through without a match).
static std::optional<std::pair<PHLMONITOR, Vector2D>> monitorUnderCursor() {
    const auto POINT = g_pInputManager->getMouseCoordsInternal();
    for (const auto& MONITOR : State::monitorState()->monitors()) {
        if (!MONITOR || scrollOverviewForMonitor(MONITOR) || !MONITOR->logicalBox().containsPoint(POINT))
            continue;
        return std::make_pair(MONITOR, (POINT - MONITOR->m_position) * MONITOR->m_scale);
    }
    return std::nullopt;
}

static std::pair<std::string, std::string> splitOverviewArg(const std::string& arg) {
    const auto FIRST = arg.find_first_not_of(" \t");
    if (FIRST == std::string::npos)
        return {"on", ""};

    const auto SEPARATOR = arg.find_first_of(" \t", FIRST);
    if (SEPARATOR == std::string::npos)
        return {arg.substr(FIRST), ""};

    const auto TARGET = arg.find_first_not_of(" \t", SEPARATOR);
    if (TARGET == std::string::npos)
        return {arg.substr(FIRST, SEPARATOR - FIRST), ""};

    const auto LAST = arg.find_last_not_of(" \t");
    return {arg.substr(FIRST, SEPARATOR - FIRST), arg.substr(TARGET, LAST - TARGET + 1)};
}

static std::vector<PHLMONITOR> overviewTargetMonitors(const std::string& target) {
    if (target == "all")
        return State::monitorState()->monitors();

    if (!target.empty()) {
        const auto& MONITORS = State::monitorState()->monitors();
        const auto  IT       = std::ranges::find_if(MONITORS, [&target](const auto& monitor) { return monitor && monitor->m_name == target; });
        return IT == MONITORS.end() ? std::vector<PHLMONITOR>{} : std::vector<PHLMONITOR>{*IT};
    }

    const auto MONITOR = Desktop::focusState()->monitor();
    return MONITOR ? std::vector<PHLMONITOR>{MONITOR} : std::vector<PHLMONITOR>{};
}

static bool openOverviewOne(PHLMONITOR monitor);

// ONE STATE ACROSS MONITORS (user, 2026-09-27), the opening half of the rule
// CScrollOverview::close() keeps: the monitor asked for opens first, then every
// other one, so no path -- the dock's Show Overview, a query open -- leaves a
// monitor on the desktop while another shows the overview.
static bool openOverview(PHLMONITOR monitor) {
    OpenProf::begin(monitor ? monitor->m_name.c_str() : "?");
    // Cheap (two config reads) and catches a `hyprctl keyword` toggle that no
    // reload announced.
    ScrollOverview::FramePacing::apply();
    if (!openOverviewOne(monitor))
        return false;
    OpenProf::mark("first monitor open");
    for (const auto& M : State::monitorState()->monitors())
        if (M && M != monitor && M->m_enabled)
            openOverviewOne(M);
    OpenProf::mark("all monitors open");
    return true;
}

static bool openOverviewOne(PHLMONITOR monitor) {
    if (!monitor)
        return true;

    if (const auto EXISTING = scrollOverviewForMonitor(monitor)) {
        // Already open is a no-op, but still CLOSING is not: reversing the
        // animation is what makes a second tap of SUPER feel immediate instead
        // of landing on an overview that is on its way out and being dropped.
        //
        // Exception: a close that never started its animation (the swipe
        // gesture used to mark `closing` without tearing down) is already
        // gone on screen. Reopening it would resurrect a ghost. Finish it.
        if (EXISTING->isClosing()) {
            EXISTING->close();
            if (!scrollOverviewForMonitor(monitor))
                return true;
            if (const auto STILL = scrollOverviewForMonitor(monitor))
                STILL->reopen();
            return true;
        }
        return true;
    }

    if (!ensureScrollOverviewHooks())
        return false;

    // A brand-new overview, not a reopen of one that is still closing.
    if (scrollOverviews().empty())
        resetSearchSession();

    const bool PREVRENDERINGOVERVIEW = renderingOverview;
    renderingOverview                = true;
    auto overview                    = makeShared<CScrollOverview>(monitor->m_activeWorkspace, false, monitor);
    registerScrollOverview(overview);
    renderingOverview = PREVRENDERINGOVERVIEW;
    return true;
}

// X32. The dock's Show Applications button opens the overview, and the hook
// setup above is not something a second implementation should duplicate.
bool openScrollOverview(PHLMONITOR monitor) {
    return openOverview(monitor);
}

static void afterQueryEdit();

bool openScrollOverviewWithQuery(PHLMONITOR monitor, const std::string& query) {
    if (!monitor)
        return false;
    if (!openScrollOverviewForMonitor(monitor)) {
        resetSearchSession();
        if (!openOverview(monitor))
            return false;
    }

    g_search.query.text             = query;
    g_search.query.active           = !query.empty();
    g_search.nav.zone               = EOverviewNavZone::Search;
    g_search.nav.selection          = -1;
    g_search.nav.explicitSelection  = false;
    g_search.nav.selectFirstPending = false;
    g_search.nav.intent             = ESearchIntent::Typing;
    g_search.nav.hoverIndex         = -1;
    afterQueryEdit();
    // After the edit, which clears it. With no results there is nothing for
    // the search zone to hold: the keyboard starts on the tiles.
    g_search.query.windowsOnly      = !query.empty();
    if (g_search.query.windowsOnly) {
        g_search.nav.zone               = EOverviewNavZone::Tile;
        g_search.nav.selection          = -1;
        g_search.nav.selectFirstPending = false;
    }
    for (const auto& overview : scrollOverviews())
        if (overview)
            overview->damage();
    return true;
}

// Declared in OverviewInternal.hpp, which this file does not include.
namespace OvInternal {
    void overviewQueryEdited();
    void cancelPointerHandoff(); // scrollOverview.cpp
    bool pointerOnTopLayer(PHLMONITOR monitor); // OverviewInput.cpp
}
void OvInternal::overviewQueryEdited() {
    afterQueryEdit();
    for (const auto& overview : scrollOverviews())
        if (overview)
            overview->damage();
}

void overviewAdmitWindow(const PHLWINDOW& window) {
    if (!window || (g_search.query.text.empty() && !g_search.query.active))
        return;
    if (!overviewSearchHides(window))
        return;

    g_search.query.text.clear();
    g_search.query.active  = false;
    g_search.nav.zone      = EOverviewNavZone::Tile;
    g_search.nav.selection = -1;
    afterQueryEdit();
    for (const auto& overview : scrollOverviews())
        if (overview)
            overview->damage();
}

static void resetCompose();

// Every open starts unfiltered, on the tiles, holding nothing. ONE call: this
// used to be six hand-listed fields and the list was wrong twice (X61 added the
// nav zone, G12 the result drag, neither reached every site).
//
// And ONE function, for the same reason at the next level up. The dispatcher
// was the only caller until X32 put a second door on the overview -- the dock's
// Show Applications button -- and that door went straight to openOverview(),
// past all three lines below. The result was user-visible: type a query, click
// a tile to close, press Show Applications, and the "fresh" overview came back
// still filtered by the query. Anything a new session has to forget goes here,
// not into either caller.
void resetSearchSession() {
    OmarchySearch::closeSushi();
    g_search.reset();
    SearchLetters::g_searchLetters.reset();
    OmarchySearch::resetMenuRoute();
    Membership::bump("search session reset");
    resetCompose(); // a sequence abandoned last time must not eat the first key
    SearchProviders::query("");
}

static SDispatchResult onOverviewDispatcher(std::string arg) {
    // Ahead of parsing the action, which is deliberate and is load-bearing for
    // the CLOSE actions too: a toggle-off with a query typed drops the filter
    // first, so the windows it was hiding are back in the grid for the close
    // animation instead of popping in after it.
    // Tab is a step INSIDE the session: it must not reset it (it runs first).
    if (splitOverviewArg(arg).first == "tab") {
        if (const auto ACTIVE = activeScrollOverview())
            if (auto* S = dynamic_cast<CScrollOverview*>(ACTIVE.get()))
                S->tabAcrossMonitors();
        return {};
    }
    resetSearchSession();
    const auto [ACTION, TARGET] = splitOverviewArg(arg);
    const auto ACTIVE           = activeScrollOverview();

    if (ACTIVE && ACTIVE->m_isSwiping)
        return {.success = false, .error = "already swiping"};

    if (ACTION == "select") {
        if (ACTIVE)
            ACTIVE->selectHoveredWorkspace();
        return {};
    }

    // The overview's Tab step, for a keybind -- and for tests that must not
    // type into the user's session (2026-09-27).
    if (ACTION == "tab") {
        if (auto* S = ACTIVE ? dynamic_cast<CScrollOverview*>(ACTIVE.get()) : nullptr)
            S->tabAcrossMonitors();
        return {};
    }

    if (ACTION == "off" || ACTION == "close" || ACTION == "disable") {
        if (TARGET.empty() || TARGET == "all") {
            const auto OPEN = scrollOverviews();
            for (const auto& overview : OPEN) {
                if (overview)
                    overview->close();
            }
            return {};
        }

        const auto MONITORS = overviewTargetMonitors(TARGET);
        if (MONITORS.empty())
            return {.success = false, .error = "monitor not found: " + TARGET};
        if (const auto overview = scrollOverviewForMonitor(MONITORS.front()))
            overview->close();
        return {};
    }

    if (ACTION != "toggle" && ACTION != "on" && ACTION != "open" && ACTION != "enable")
        return {.success = false, .error = "invalid arg. expected toggle|open|close|select [monitor|all]"};

    const auto MONITORS = overviewTargetMonitors(TARGET);
    if (MONITORS.empty())
        return {.success = false, .error = TARGET.empty() ? "no active monitor" : "monitor not found: " + TARGET};

    const bool ALL_OPEN = std::ranges::all_of(MONITORS, [](const auto& monitor) {
        const auto OVERVIEW = scrollOverviewForMonitor(monitor);
        return OVERVIEW && !OVERVIEW->isClosing();
    });
    if (ACTION == "toggle" && ALL_OPEN) {
        for (const auto& monitor : MONITORS) {
            if (const auto overview = scrollOverviewForMonitor(monitor))
                overview->close();
        }
        return {};
    }

    // X51. The open pays for whatever the first keystroke no longer does, so it
    // is armed exactly like a keystroke -- same counters, same three frames.
    if (Prof::enabled()) {
        Prof::reset();
        Prof::keyAt   = Prof::clock::now();
        Prof::pending = true;
    }

    // X54. Search providers are D-Bus activated and go cold again after well
    // under a minute of idle, so without this the first keystroke waits on a
    // GTK app starting: median settle 1085ms cold vs 342ms warmed (9 samples
    // each, 45s idle between). Launching them here overlaps the open animation
    // and the reach for the keyboard. Costs a respawn per open-after-idle;
    // that is the price of the results being up when the user looks at them.
    SearchProviders::warm();
    OmarchySearch::init();

    for (const auto& monitor : MONITORS) {
        if (!openOverview(monitor))
            return {.success = false, .error = "failed enabling overview hooks (is other overview plugin enabled?)"};
    }
    return {};
}

// Tap-SUPER toggles the overview.
//
// This cannot live in a Lua bind: measured on 0.56.1, every bind form on
// SUPER_L fires identically whether SUPER was tapped alone or held as a
// modifier for another key, so a bind would open the overview on every
// SUPER+<key> shortcut. The tap/hold distinction only exists at the raw key
// event level.
// ponytail: raw evdev keycodes; a remapped SUPER will not be recognised.
static constexpr uint32_t EVDEV_KEY_LEFTMETA  = 125;
static constexpr uint32_t EVDEV_KEY_RIGHTMETA = 126;

// Double-tap SHIFT in the search field launches a NEW instance of the selected
// app. Same argument as tap-SUPER above: a bind on SHIFT_L cannot tell a tap
// from the SHIFT that is holding down a capital letter, and this is a field the
// user types capitals into. A tap is a press and release with no other real key
// between them, so typing "Zen" cancels on the 'z' and never reaches here.
static constexpr uint32_t EVDEV_KEY_LEFTSHIFT  = 42;
static constexpr uint32_t EVDEV_KEY_RIGHTSHIFT = 54;
// Ctrl x2 (set the browsed AI agent as Omarchy's default) is the same gesture
// with the same timing -- a tap is a tap -- so it shares both constants below.
static constexpr uint32_t EVDEV_KEY_LEFTCTRL   = 29;
static constexpr uint32_t EVDEV_KEY_RIGHTCTRL  = 97;
// A tap must RELEASE within this, or it was a hold.
static constexpr uint32_t TAP_SHIFT_HOLD_MS = 250;
// ...and the second tap must land within this of the first, or it is the start
// of a new pair rather than the end of one.
static constexpr uint32_t TAP_SHIFT_GAP_MS = 450;

// Only a real key cancels the tap. Modifier and lock keycodes must be ignored:
// this compositor emits a synthetic CAPSLOCK (58) press/release pair around
// every key event, which would otherwise cancel every tap before it completed.
static bool isModifierKeycode(uint32_t keycode) {
    switch (keycode) {
        case 29:  // LEFTCTRL
        case 42:  // LEFTSHIFT
        case 54:  // RIGHTSHIFT
        case 56:  // LEFTALT
        case 58:  // CAPSLOCK
        case 69:  // NUMLOCK
        case 70:  // SCROLLLOCK
        case 97:  // RIGHTCTRL
        case 100: // RIGHTALT
        case EVDEV_KEY_LEFTMETA:
        case EVDEV_KEY_RIGHTMETA: return true;
        default: return false;
    }
}

static constexpr uint32_t EVDEV_KEY_ESC       = 1;
static constexpr uint32_t EVDEV_KEY_BACKSPACE = 14;
static constexpr uint32_t EVDEV_KEY_SPACE     = 57;   // V5, the sushi preview

static xkb_state* activeXkbState() {
    const auto KEEB = g_pSeatManager ? g_pSeatManager->m_keyboard.lock() : nullptr;
    return KEEB ? KEEB->m_xkbState : nullptr;
}

// SUPER, CTRL and ALT must keep their chords working while the overview is
// open, so a key pressed with one of them held is never search input.
//
// Tracked from the raw key stream rather than read from xkb: xkb was measured
// reporting logo=0 for the Z press of a SUPER+Z chord in this VM (see
// share/f_guard_fix.py). Shift is deliberately absent -- it types.
static bool isGuardModifierKeycode(uint32_t keycode) {
    switch (keycode) {
        case 29:  // LEFTCTRL
        case 56:  // LEFTALT
        case 97:  // RIGHTCTRL
        case 100: // RIGHTALT
        case EVDEV_KEY_LEFTMETA:
        case EVDEV_KEY_RIGHTMETA: return true;
        default: return false;
    }
}

// Keycodes, not a count: Super-tap key-repeat used to increment a counter
// without a matching release, after which searchKeyFor refused every letter
// for the rest of the session.
static std::unordered_set<uint32_t> g_heldGuardKeys;

// Keys that belong to the whole desktop, never to the search field: PRINT (and
// SYSRQ, which is what ALT+PRINT resolves to) and the XF86 family -- volume,
// brightness, media, mic mute, keyboard light. The overview-owns-the-keyboard
// catch-all lets these past so Omarchy's own binds still fire; the overview
// borrows those binds into its submap while it holds it (scrollOverview.cpp,
// borrowGlobalPassthroughBinds).
static bool isGlobalPassthroughKeycode(uint32_t keycode) {
    const auto STATE = activeXkbState();
    if (!STATE)
        return false;

    const xkb_keysym_t SYM = xkb_state_key_get_one_sym(STATE, keycode + 8);
    return SYM == XKB_KEY_Print || SYM == XKB_KEY_Sys_Req || (SYM >= 0x1008FF00 && SYM <= 0x1008FFFF);
}

xkb_state* overviewActiveXkbState() {
    return activeXkbState();
}

static bool overviewAcceptsSearch() {
    return std::ranges::any_of(scrollOverviews(), [](const auto& overview) { return overview && !overview->isClosing(); });
}

// A sequence half-typed when the overview closes, or when ESC/BACKSPACE cuts it
// short, must not swallow the first key of the next one.
static void resetCompose() {
    ScrollOverview::TextInput::resetCompose();
}

// The query is UTF-8 now (it has to be -- a composed 'e' is two bytes), so
// BACKSPACE removes a CODEPOINT. pop_back() would leave a severed lead byte and
// hand the providers a string that is not valid UTF-8.
static void popCodepoint(std::string& s) {
    ScrollOverview::TextInput::popCodepoint(s);
}

struct SSearchKey {
    bool        consume = false; // ours: cancel the event so it cannot reach the window below
    bool        starts  = false; // counts as starting/continuing a search, even with no text
    std::string text;            // what to append; EMPTY while a compose sequence is mid-flight
};

// What this keycode contributes to the query. Returns nothing whenever a guard
// modifier is held, so SUPER+Q stays a keybind instead of becoming the letter q.
static SSearchKey searchKeyFor(uint32_t keycode) {
    if (!g_heldGuardKeys.empty())
        return {};
    const auto K = ScrollOverview::TextInput::keyFor(activeXkbState(), keycode);
    return {.consume = K.consume, .starts = K.starts, .text = K.text};
}

// ---------------------------------------------------------------------------
// THE LETTER TICK (user, 2026-09-19: the pop is "sometimes smooth sometimes
// it's not", worst on "the trailing characters bouncing").
//
// The field's animation was kept alive from INSIDE the render pass: renderSearch
// ended in `damage(); requestInputFrame();`, and requestInputFrame is a
// scheduleFrame issued while a frame is already being composited, where it is
// liable to be absorbed by the one in flight instead of booking the next one.
//
// MEASURED, not guessed (49s screen recording plus the in-compositor profile,
// 3140 frames). Frames dropped, by position in the pop:
//
//     f=2-4   (first 65ms)      0.0 - 0.5%
//     f=6-8   (100-135ms)      15   - 17%
//     f=9-12                   22   - 29%
//     f=15-21 (250-350ms)      36   - 39%
//
// The head of every pop is clean because the compositor's OWN animation
// variables -- resultsWidth, searchDock, resultsBand -- are still running and
// schedule those frames as a side effect. Once they settle, the letters are the
// only thing still moving and the in-pass request is all that is left, so the
// tail of every pop loses better than one frame in three. That is why it got
// worse the more letters were on screen (more tail), why it was periodic rather
// than constant, and why a held backspace stayed smooth until the end -- the
// same coupling scrollOverview.cpp's frame-policy comment records from the
// earlier incarnation, where letters froze outright once the animvars settled.
//
// Ticking from the event loop instead: half a 60Hz frame, so a request is
// always pending before the next vblank, self-sustaining while anything in the
// field still moves and stopping on its own when nothing does. renderSearch
// keeps its own call as a belt-and-braces fallback; it is harmless when a tick
// has already booked the frame.
static wl_event_source*  g_letterTickTimer = nullptr;
static constexpr int     LETTER_TICK_MS    = 8;

static int letterTickCb(void*) {
    const auto NOW     = SearchLetters::clock::now();
    const bool LETTERS = SearchLetters::g_searchLetters.busy(NOW);

    bool again = false;
    for (const auto& OVERVIEW : scrollOverviews()) {
        if (!OVERVIEW)
            continue;

        const auto MONITOR = OVERVIEW->pMonitor.lock();
        if (!MONITOR)
            continue;

        // The letters are one query shared by every view; the width and slide
        // springs are per monitor, so both questions get asked.
        if (!LETTERS && !g_search.fieldOn(MONITOR->m_id).busy())
            continue;

        OVERVIEW->damage();
        OVERVIEW->requestInputFrame();
        again = true;
    }

    if (again && g_letterTickTimer)
        wl_event_source_timer_update(g_letterTickTimer, LETTER_TICK_MS);

    return 0;
}

// Armed by the edit, not by the render: the edit is what starts something
// moving, and it happens off the render pass.
static void armLetterTick() {
    if (!g_pCompositor)
        return;

    if (!g_letterTickTimer)
        g_letterTickTimer = wl_event_loop_add_timer(g_pCompositor->m_wlEventLoop, letterTickCb, nullptr);

    wl_event_source_timer_update(g_letterTickTimer, LETTER_TICK_MS);
}

// The trailing work every edit to the query owes, whatever made the edit.
static void afterQueryEdit() {
    g_search.query.windowsOnly = false;

    // The gate reads the query twice -- "is it empty" for the Y9 minimized rule
    // and the text itself for the match -- so every edit changes its answer.
    Membership::bump("query edit");

    // X52. The moment being complained about starts HERE, not at the frame.
    if (Prof::enabled()) {
        Prof::reset();
        Prof::keyAt   = Prof::clock::now();
        Prof::pending = true;
    }

    // The pop starts now, so the frames that carry it start now too -- from
    // here rather than from the render pass. See armLetterTick.
    armLetterTick();

    // X20: every keystroke starts a new provider generation. Non-blocking, so
    // this costs the keystroke nothing; results land over the next frames.
    SearchProviders::query(g_search.query.text);
    OmarchySearch::query(g_search.query.text);
    if (!g_search.query.text.empty())
        OmarchySearch::resetMenuRoute();
    if (OmarchySearch::isSushiActive())
        OmarchySearch::closeSushi();

    // The ranking changes under any edit, so a held index would point at a
    // different app than the one the user saw highlighted.
    g_search.nav.selection         = -1;
    g_search.nav.explicitSelection = false;
    g_search.nav.intent            = ESearchIntent::Typing;
    g_search.nav.hoverIndex        = -1;

    // Typing arms first-app selection (spent once results exist) and puts
    // SPACE back to "character". Auto-select is for Enter, not Quick Look.
    g_search.nav.selectFirstPending = !g_search.query.text.empty();
    g_search.nav.zone               = g_search.query.text.empty() ? EOverviewNavZone::Tile : EOverviewNavZone::Search;

    for (const auto& overview : scrollOverviews()) {
        if (!overview)
            continue;
        overview->damage();
        overview->requestInputFrame();
    }
}

// OVERVIEW_CPP_SPEC 5 F5. The overview is not a wayland client. Seat keyboard
// focus is cleared while it is open, and the query is assembled from the raw
// key stream with those events cancelled so they cannot reach a window or bar.
//
// This used to be a pure isSearchKey(keycode) predicate, consulted separately
// for the press and the release on the promise that both would agree. COMPOSE
// IS STATEFUL AND BREAKS THAT PROMISE: a sequence can complete between a press
// and its release, so the predicate flips underneath and leaks a stray release
// into the window below. The decision is made ONCE, on the press, and the
// release is consumed iff that press was.
static std::unordered_set<uint32_t> g_consumedPresses;

static bool handleSearchPress(uint32_t keycode) {
    if (!overviewAcceptsSearch())
        return false;

    if (g_pSeatManager)
        g_pSeatManager->setKeyboardFocus(nullptr);

    // X5: ESC is always ours while the overview is open -- it either clears the
    // query or exits -- so it must never fall through to the window underneath.
    if (keycode == EVDEV_KEY_ESC) {
        if (OmarchySearch::isSushiActive()) {
            OmarchySearch::closeSushi();
            for (const auto& overview : scrollOverviews())
                if (overview)
                    overview->damage();
            return true;
        }
        resetCompose();
        // If in Result zone and in a subroute, ESC goes back up to parent
        if (g_search.nav.zone == EOverviewNavZone::Result && OmarchySearch::canNavigateMenuBack()) {
            OmarchySearch::navigateMenuBack();
            g_search.nav.selection = 0;
            afterQueryEdit();
            return true;
        }
        // X5, GNOME-style: the first ESC drops the filter, the next one leaves.
        if (!g_search.query.active && g_search.query.text.empty()) {
            for (const auto& overview : scrollOverviews())
                if (overview)
                    overview->close();
            return true;
        }
        g_search.query.text.clear();
        g_search.query.active = false;
        g_search.nav.zone = EOverviewNavZone::Tile;
        g_search.nav.selection = -1;
        afterQueryEdit();
        return true;
    }

    if (keycode == EVDEV_KEY_BACKSPACE) {
        if (OmarchySearch::isSushiActive()) {
            OmarchySearch::closeSushi();
            for (const auto& overview : scrollOverviews())
                if (overview)
                    overview->damage();
        }
        resetCompose();
        if (g_search.query.windowsOnly) {
            g_search.query.text.clear();
            g_search.query.active = false;
            afterQueryEdit();
            return true;
        }
        if (g_search.nav.zone == EOverviewNavZone::Result && g_search.query.text.empty()) {
            if (OmarchySearch::navigateMenuBack()) {
                g_search.nav.selection = 0;
                afterQueryEdit();
                return true;
            }
        }
        if (!g_search.query.active && g_search.query.text.empty())
            return true;
        if (g_search.nav.zone == EOverviewNavZone::Result) {
            g_search.nav.zone              = EOverviewNavZone::Search;
            g_search.nav.selection         = -1;
            g_search.nav.explicitSelection = false;
        }
        if (g_search.query.text.empty()) {
            g_search.query.active = false;
            return true;
        }
        popCodepoint(g_search.query.text);
        if (g_search.query.text.empty())
            g_search.query.active = false;
        afterQueryEdit();
        return true;
    }

    const SSearchKey KEY = searchKeyFor(keycode);
    if (!KEY.consume)
        return false;

    if (OmarchySearch::isSushiActive()) {
        OmarchySearch::closeSushi();
        for (const auto& overview : scrollOverviews())
            if (overview)
                overview->damage();
    }

    // Typing any character while in Result zone shifts focus back to Search bar
    if (g_search.nav.zone == EOverviewNavZone::Result) {
        g_search.nav.zone              = EOverviewNavZone::Search;
        g_search.nav.selection         = -1;
        g_search.nav.explicitSelection = false;
    }

    if (KEY.starts)
        g_search.query.active = true;

    if (g_search.query.windowsOnly)
        g_search.query.text.clear();
    g_search.query.text += KEY.text;
    afterQueryEdit();
    return true;
}

// Hyprland's key-repeat timer only fires for focused clients. Search steals
// seat focus, so Backspace would delete once and then stall until another
// physical press. Drive repeats ourselves off the compositor loop.
static wl_event_source* g_searchRepeatTimer = nullptr;
static uint32_t         g_searchRepeatKey   = ~0u;

static void stopSearchRepeat() {
    g_searchRepeatKey = ~0u;
    if (g_searchRepeatTimer)
        wl_event_source_timer_update(g_searchRepeatTimer, 0);
}

static int searchRepeatCb(void*) {
    if (g_searchRepeatKey == ~0u || !overviewAcceptsSearch() || g_consumedPresses.count(g_searchRepeatKey) == 0) {
        stopSearchRepeat();
        return 0;
    }
    handleSearchPress(g_searchRepeatKey);
    if (g_searchRepeatKey == EVDEV_KEY_BACKSPACE && g_search.query.text.empty()) {
        stopSearchRepeat();
        return 0;
    }
    int rate = 25;
    if (const auto KB = g_pSeatManager ? g_pSeatManager->m_keyboard.lock() : nullptr; KB && KB->m_repeatRate > 0)
        rate = KB->m_repeatRate;
    if (g_searchRepeatTimer)
        wl_event_source_timer_update(g_searchRepeatTimer, std::max(20, 1000 / std::max(1, rate)));
    return 0;
}

static void startSearchRepeat(uint32_t keycode) {
    if (!g_pCompositor)
        return;
    if (!g_searchRepeatTimer)
        g_searchRepeatTimer = wl_event_loop_add_timer(g_pCompositor->m_wlEventLoop, searchRepeatCb, nullptr);
    g_searchRepeatKey = keycode;
    int delay = 600;
    if (const auto KB = g_pSeatManager ? g_pSeatManager->m_keyboard.lock() : nullptr; KB && KB->m_repeatDelay > 0)
        delay = KB->m_repeatDelay;
    wl_event_source_timer_update(g_searchRepeatTimer, std::max(200, delay));
}

static int              g_fileWakeFd     = -1;
static wl_event_source* g_fileWakeSource = nullptr;

static int onFileResultsReady(int fd, uint32_t mask, void* data) {
    uint64_t val = 0;
    ::read(fd, &val, sizeof(val));
    for (const auto& overview : scrollOverviews()) {
        if (!overview)
            continue;
        overview->damage();
        overview->requestInputFrame();
    }
    ScrollOverview::Stacks::onReaderReady();
    return 0;
}

static CHyprSignalListener g_pRawKeyHook;
static CHyprSignalListener g_pTapSuperButtonHook;
static CHyprSignalListener g_pRenderPreHook;
static CHyprSignalListener g_pMembOpenHook;
static CHyprSignalListener g_pMembLateHook;
static CHyprSignalListener g_pMembCloseHook;
static CHyprSignalListener g_pMembKillHook;
static CHyprSignalListener g_pMembGoneHook;
static CHyprSignalListener g_pMembPinHook;
static CHyprSignalListener g_pMembFloatHook;
static CHyprSignalListener g_pMembTitleHook;
static CHyprSignalListener g_pMembClassHook;
static CHyprSignalListener g_pMembWsHook;
static CHyprSignalListener g_pMembFullHook;
static CHyprSignalListener g_pDockConfigHook;
static CHyprSignalListener g_pDockHoverHook;
static CHyprSignalListener g_pDockClickHook;
static CHyprSignalListener g_pDockScrollHook;
static CHyprSignalListener g_pDockRenderHook;
static CHyprSignalListener g_pScratchExitRenderHook;
static CHyprSignalListener g_pMonitorGoneHook;
static CHyprSignalListener g_pScrollCameraActiveHook;
static CHyprSignalListener g_pScrollCameraMovedHook;
static CHyprSignalListener g_pClickPreReloadHook;
static bool                g_tapSuperCandidate = false;
static uint32_t            g_tapSuperPressedAt = 0;
static bool                g_tapShiftCandidate = false;
static uint32_t            g_tapShiftPressedAt = 0;
// Release time of the last completed SHIFT tap; 0 = no pair in flight.
static uint32_t            g_tapShiftLastTapMs = 0;
static bool                g_tapCtrlCandidate  = false;
static uint32_t            g_tapCtrlPressedAt  = 0;
static uint32_t            g_tapCtrlLastTapMs  = 0;

// ONE raw-key listener, and it owns the guard-modifier count, V5's SPACE
// preview, the search keys, and the SUPER tap. The order is load-bearing at
// every step (each branch says why), and they are one listener because they
// read one key stream and each has to decide whether the key survives to the
// next -- two listeners could not.
static void                registerRawKeyHooks() {
    g_pRawKeyHook = Event::bus()->m_events.input.keyboard.key.listen([](IKeyboard::SKeyEvent event, Event::SCallbackInfo& info) {
        const bool PRESSED = event.state == WL_KEYBOARD_KEY_STATE_PRESSED;
        const bool ISSUPER = event.keycode == EVDEV_KEY_LEFTMETA || event.keycode == EVDEV_KEY_RIGHTMETA;

        if (isGuardModifierKeycode(event.keycode)) {
            if (PRESSED)
                g_heldGuardKeys.insert(event.keycode);
            else
                g_heldGuardKeys.erase(event.keycode);
        }

        // A REAL KEY BREAKS EVERY TAP IN FLIGHT -- and it has to be recorded
        // HERE, above every consume path, not after them.
        //
        // The same bookkeeping lives further down inside the !ISSUPER branch,
        // where it only ever ran for keys that fell all the way through. A chord
        // whose letter one of the branches below swallowed returned first, so
        // the SUPER tap candidate was still standing when SUPER came back up and
        // the release was read as a tap: SUPER+W closed the window and then
        // toggled the overview shut on the way out (user, 2026-09-23). Every
        // Super chord over an open overview had it; W is just the first one
        // bound to something.
        if (PRESSED && !isModifierKeycode(event.keycode)) {
            g_tapSuperCandidate = false;
            g_tapShiftCandidate = false;
            g_tapShiftLastTapMs = 0;
            g_tapCtrlCandidate  = false;
            g_tapCtrlLastTapMs  = 0;
        }

        if (!ISSUPER) {
            if (PRESSED) {
                // V11 SPEC V5. SPACE PREVIEWS THE HOVERED FAN/GRID ITEM.
                //
                // THE SCOPE IS THE FEATURE. previewHovered() claims the key
                // only when a stack popup is open AND the pointer is over one
                // of its file rows -- two conditions that are false in every
                // other state this compositor is ever in, including with the
                // dock right there and a stack pinned. Everywhere else the
                // call is a null check and a walk of an empty vector, and the
                // key falls through to the focused window exactly as before.
                // A space that went missing in a text editor would be a far
                // bigger bug than this is a feature, so the claim is decided
                // by the same read that decides what to preview: no item, no
                // claim (see DockStacks::previewHovered).
                //
                // AHEAD of handleSearchPress, and that ordering is not an
                // accident: while the overview is open handleSearchPress
                // consumes SPACE unconditionally -- G1's rule that whitespace
                // must never START a search still keeps the key rather than
                // typing it into a window the user cannot see. A fan opened
                // over the overview would otherwise never see the press.
                // Dock stacks A: an open fan/pane owns ITS keys (arrows, Enter,
                // Esc, Space; the pane also types). handleKey claims SPACE only
                // when there is an item to preview, so everywhere else the key
                // falls through untouched -- the scope rule above still holds.
                // Neither a stack popup nor the Rename field takes a CHORD (Super/
                // Ctrl/Alt held) or a desktop key (Print, volume, brightness):
                // those always reach their binds (review 2026-09-24).
                const bool PLAINKEY = g_heldGuardKeys.empty() && !isGlobalPassthroughKeycode(event.keycode);

                // The dock menu's Rename field, while it is up, owns the keyboard.
                if (PLAINKEY && ScrollOverview::Dock::menuWantsKeyboard() && ScrollOverview::Dock::menuHandleKey(event.keycode)) {
                    g_consumedPresses.insert(event.keycode);
                    info.cancelled = true;
                    return;
                }
                if (PLAINKEY && ScrollOverview::Stacks::keyboardActive() && ScrollOverview::Stacks::handleKey(event.keycode)) {
                    g_consumedPresses.insert(event.keycode);
                    info.cancelled = true;
                    return;
                }

                if (event.keycode == EVDEV_KEY_SPACE) {
                    if (overviewAcceptsSearch()) {
                        if (overviewHandleSearchSpace()) {
                            for (const auto& overview : scrollOverviews())
                                if (overview)
                                    overview->damage();
                            g_consumedPresses.insert(event.keycode);
                            info.cancelled = true;
                            return;
                        }
                        // Not a preview: fall through to handleSearchPress and
                        // type the space into the query.
                    } else if (OmarchySearch::isSushiActive()) {
                        OmarchySearch::closeSushi();
                        for (const auto& overview : scrollOverviews())
                            if (overview)
                                overview->damage();
                        g_consumedPresses.insert(event.keycode);
                        info.cancelled = true;
                        return;
                    }
                }

                if (handleSearchPress(event.keycode)) {
                    g_consumedPresses.insert(event.keycode);
                    if (event.keycode == EVDEV_KEY_BACKSPACE && !g_search.query.text.empty())
                        startSearchRepeat(event.keycode);
                    info.cancelled = true;
                    return;
                }

                // Overview owns the keyboard: a key that is not search input
                // still must not reach the window or a layer-shell bar.
                // Arrows/Tab/Return are left for CScrollOverview::onKeyboardKey.
                // PRINT and the XF86 keys are the exception: they are desktop
                // keys, not query keys, and Omarchy's binds must still see them.
                if (overviewAcceptsSearch() && !isModifierKeycode(event.keycode) && !isGlobalPassthroughKeycode(event.keycode) &&
                    event.keycode != 15 && event.keycode != 28 && event.keycode != 96 &&
                    event.keycode != 103 && event.keycode != 105 && event.keycode != 106 &&
                    event.keycode != 108) {
                    g_consumedPresses.insert(event.keycode);
                    info.cancelled = true;
                    return;
                }
            }
            else if (g_consumedPresses.erase(event.keycode) > 0) {
                if (event.keycode == g_searchRepeatKey)
                    stopSearchRepeat();
                info.cancelled = true;
                return;
            }
        }

        // DOUBLE-TAP SHIFT -> new instance of the selected app. overviewHandleShiftTap
        // was written but never wired to a caller, so the gesture had no way to
        // fire; the timing half of it belongs here, where tap and hold are still
        // distinguishable.
        if (event.keycode == EVDEV_KEY_LEFTSHIFT || event.keycode == EVDEV_KEY_RIGHTSHIFT) {
            if (PRESSED) {
                g_tapShiftCandidate = true;
                g_tapShiftPressedAt = event.timeMs;
            } else if (g_tapShiftCandidate) {
                g_tapShiftCandidate  = false;
                const bool WASTAP    = event.timeMs - g_tapShiftPressedAt <= TAP_SHIFT_HOLD_MS;
                const bool PAIRED    = WASTAP && g_tapShiftLastTapMs != 0 && event.timeMs - g_tapShiftLastTapMs <= TAP_SHIFT_GAP_MS;
                g_tapShiftLastTapMs  = PAIRED || !WASTAP ? 0 : event.timeMs;
                if (PAIRED && overviewAcceptsSearch() && overviewHandleShiftTap()) {
                    info.cancelled = true;
                    return;
                }
            }
        }

        // DOUBLE-TAP CTRL -> the browsed AI agent becomes Omarchy's default. The
        // Shift pair's timing and its cancel rule (a real key between the taps
        // breaks it), so Ctrl+Backspace and every other chord stay chords.
        if (event.keycode == EVDEV_KEY_LEFTCTRL || event.keycode == EVDEV_KEY_RIGHTCTRL) {
            if (PRESSED) {
                g_tapCtrlCandidate = true;
                g_tapCtrlPressedAt = event.timeMs;
            } else if (g_tapCtrlCandidate) {
                g_tapCtrlCandidate = false;
                const bool WASTAP  = event.timeMs - g_tapCtrlPressedAt <= TAP_SHIFT_HOLD_MS;
                const bool PAIRED  = WASTAP && g_tapCtrlLastTapMs != 0 && event.timeMs - g_tapCtrlLastTapMs <= TAP_SHIFT_GAP_MS;
                g_tapCtrlLastTapMs = PAIRED || !WASTAP ? 0 : event.timeMs;
                if (PAIRED && overviewAcceptsSearch() && overviewHandleCtrlTap()) {
                    info.cancelled = true;
                    return;
                }
            }
        }

        if (!ISSUPER) {
            if (PRESSED && !isModifierKeycode(event.keycode)) {
                g_tapSuperCandidate = false; // SUPER is being held as a modifier
                // ...and so is SHIFT. A real key also breaks a pair in flight,
                // so tap, type, tap is not a double tap.
                g_tapShiftCandidate = false;
                g_tapShiftLastTapMs = 0;
                g_tapCtrlCandidate  = false;
                g_tapCtrlLastTapMs  = 0;
            }
            return;
        }

        if (PRESSED) {
            g_tapSuperCandidate = true;
            g_tapSuperPressedAt = event.timeMs;
            return;
        }

        if (!g_tapSuperCandidate)
            return;
        g_tapSuperCandidate = false;

        if (!ScrollOverview::Config::getTapSuper())
            return;

        const auto TIMEOUT = ScrollOverview::Config::getTapSuperTimeoutMs();
        if (TIMEOUT > 0 && event.timeMs - g_tapSuperPressedAt > static_cast<uint32_t>(TIMEOUT))
            return;

        onOverviewDispatcher("toggle all");
    });

    // SUPER+click/drag (move or resize a window) must not count as a tap.
    g_pTapSuperButtonHook = Event::bus()->m_events.input.mouse.button.listen([](IPointer::SButtonEvent event, Event::SCallbackInfo& info) { g_tapSuperCandidate = false; });
}

static SDispatchResult onNavigateDispatcher(std::string arg) {
    const auto OVERVIEW = activeScrollOverview();
    if (!OVERVIEW)
        return {};

    if (arg != "left" && arg != "right" && arg != "up" && arg != "down")
        return {.success = false, .error = "invalid arg. expected left|right|up|down"};

    OVERVIEW->moveSelection(arg);
    return {};
}

static SDispatchResult onWindowDispatcher(std::string arg) {
    const auto OVERVIEW = activeScrollOverview();
    if (!OVERVIEW)
        return {};

    if (arg == "click") {
        // Only reached when onMouseButton let the press through, i.e. over a
        // TOP/OVERLAY layer: the press is that layer's (the bind is
        // non-consuming), so the overview must not act on it too.
        if (OvInternal::pointerOnTopLayer(Desktop::focusState()->monitor()))
            return {};
        OVERVIEW->performPrimaryClickAction();
        return {};
    }

    if (arg != "select" && arg != "close")
        return {.success = false, .error = "invalid arg. expected select|close|click"};

    OVERVIEW->windowDispatcherAction(arg);
    return {};
}

static void failNotif(const std::string& reason) {
    HyprlandAPI::addNotification(SCROLLOVERVIEW_HANDLE, "[hyprdrive] Failure in initialization: " + reason, CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
}

// Helper function to find a function by name and ensure it contains one of the requested substrings in its demangled name (to disambiguate overloads).
static void* findFnOrThrow(const std::string& name, std::initializer_list<std::string_view> demangledNeedles) {
    auto fns = HyprlandAPI::findFunctionsByName(SCROLLOVERVIEW_HANDLE, name);
    if (fns.empty()) {
        failNotif(std::format("no fns for hook {}", name));
        throw std::runtime_error(std::format("[hyprdrive] No fns for hook {}", name));
    }

    if (demangledNeedles.size() == 0 || (demangledNeedles.size() == 1 && demangledNeedles.begin()->empty()))
        return fns[0].address;

    std::vector<SFunctionMatch> matches;
    matches.reserve(fns.size());
    for (const auto& fn : fns) {
        for (const auto& needle : demangledNeedles) {
            if (needle.empty() || fn.demangled.find(needle) != std::string::npos) {
                matches.push_back(fn);
                break;
            }
        }
    }

    if (matches.empty()) {
        failNotif(std::format("no matching overload for hook {}", name));
        throw std::runtime_error(std::format("[hyprdrive] No matching overload for hook {}", name));
    }

    if (matches.size() > 1) {
        failNotif(std::format("ambiguous overload for hook {} ({} matches)", name, matches.size()));
        throw std::runtime_error(std::format("[hyprdrive] Ambiguous overload for hook {}", name));
    }

    return matches[0].address;
}

// Register / unregister a trackpad gesture. Used by the hyprlang keyword, the
// Lua `hyprdrive.gesture` function, and the `hyprdrive:gesture` dispatcher.
static std::expected<void, std::string> applyTrackpadGesture(size_t fingerCount, eTrackpadGestureDirection direction, const std::string& action, uint32_t modMask, float deltaScale,
                                                             bool disableInhibit, const std::string& arg) {
    if (fingerCount <= 1 || fingerCount >= 10)
        return std::unexpected(std::format("Invalid value {} for finger count", fingerCount));

    if (direction == TRACKPAD_GESTURE_DIR_NONE)
        return std::unexpected("Invalid direction");

    const auto add = [&](UP<ITrackpadGesture>&& gesture) {
        return g_pTrackpadGestures->addGesture(std::move(gesture), fingerCount, direction, modMask, deltaScale, disableInhibit);
    };

    if (action == "unset")
        return g_pTrackpadGestures->removeGesture(fingerCount, direction, modMask, deltaScale, disableInhibit);

    if (action == "overview")
        return add(makeUnique<COverviewGesture>(direction));

    if (action == "workspace")
        return add(makeUnique<COverviewWorkspaceGesture>());

    if (action == "workspace_all")
        return add(makeUnique<COverviewWorkspaceGesture>(true));

    if (action == "scroll")
        return add(makeUnique<COverviewScrollGesture>());

    if (action == "scratchpad_show" || action == "scratchpad_hide")
        return add(makeUnique<CScratchpadGesture>(action == "scratchpad_show"));

    if (action == "fullscreen_on" || action == "unfullscreen_or_close")
        return add(makeUnique<CFullscreenOrCloseGesture>(action == "fullscreen_on"));

    if (action == "close")
        return add(makeUnique<CCloseTrackpadGesture>());

    if (action == "fullscreen" || action == "maximize")
        return add(makeUnique<CFullscreenTrackpadGesture>(action == "maximize" ? "maximize" : (arg.empty() ? "fullscreen" : arg)));

    if (action == "float" || action == "tile")
        return add(makeUnique<CFloatTrackpadGesture>(action == "tile" ? "tile" : (arg.empty() ? "toggle" : arg)));

    if (action == "move")
        return add(makeUnique<CMoveTrackpadGesture>());

    if (action == "resize")
        return add(makeUnique<CResizeTrackpadGesture>());

    if (action == "special")
        return add(makeUnique<CSpecialWorkspaceGesture>(arg.empty() ? "special" : arg));

    if (action == "zoom") {
        std::string level = "2";
        std::string mode  = arg.empty() ? "live" : arg;
        if (const auto SP = mode.find(' '); SP != std::string::npos) {
            level = mode.substr(0, SP);
            mode  = mode.substr(SP + 1);
        } else if (mode != "live" && mode != "toggle" && mode != "mult") {
            level = mode;
            mode  = "live";
        }
        return add(makeUnique<CCursorZoomTrackpadGesture>(level, mode));
    }

    if (action == "dispatcher" || action == "dispatch") {
        if (arg.empty())
            return std::unexpected("dispatcher gesture needs an arg, e.g. exec,notify-send hi");
        std::string dispatcher = arg;
        std::string data;
        const auto  SEP = arg.find_first_of(", ");
        if (SEP != std::string::npos) {
            dispatcher = arg.substr(0, SEP);
            data       = arg.substr(SEP + 1);
            while (!data.empty() && (data.front() == ' ' || data.front() == ','))
                data.erase(data.begin());
        }
        return add(makeUnique<CDispatcherTrackpadGesture>(dispatcher, data));
    }

    if (action == "keys" || action == "key" || action == "shortcut" || action == "chord") {
        if (arg.empty())
            return std::unexpected("keys gesture needs a chord, e.g. CTRL+T or Print");
        return add(makeUnique<CKeysTrackpadGesture>(arg));
    }

    return std::unexpected(std::format("Invalid gesture: {}", action));
}

// Lua-facing registrar (hyprdrive.gesture). takes the direction/mods as strings and resolves
// them the same way the keyword does, then defers to applyTrackpadGesture.
static SDispatchResult onRegisterOverviewGesture(size_t fingerCount, const std::string& directionStr, const std::string& action, const std::string& mods, float deltaScale,
                                                 bool disableInhibit, const std::string& arg) {
    if (g_unloading)
        return {};

    std::string directionNorm = directionStr;
    if (directionNorm == "pinch_in" || directionNorm == "pinch-in")
        directionNorm = "pinchin";
    else if (directionNorm == "pinch_out" || directionNorm == "pinch-out")
        directionNorm = "pinchout";

    uint32_t clickMods = 0;
    if (!mods.empty() && g_pKeybindManager)
        clickMods = g_pKeybindManager->stringToModMask(mods);

    if (directionNorm == "click" || directionNorm == "tap") {
        if (action == "unset")
            removeClickGesture(fingerCount, clickMods);
        else
            addClickGesture(fingerCount, action, clickMods, disableInhibit, arg);
        return {};
    }

    const auto direction = g_pTrackpadGestures->dirForString(directionNorm);
    if (direction == TRACKPAD_GESTURE_DIR_NONE)
        return {.success = false, .error = std::format("Invalid direction: {}", directionStr)};

    const uint32_t modMask = clickMods;

    const auto res = applyTrackpadGesture(fingerCount, direction, action, modMask, std::clamp(deltaScale, 0.1F, 10.F), disableInhibit, arg);
    if (!res)
        return {.success = false, .error = res.error()};

    return {};
}

static Hyprlang::CParseResult overviewGestureKeyword(const char* LHS, const char* RHS) {
    Hyprlang::CParseResult result;

    if (g_unloading)
        return result;

    CConstVarList             data(RHS);

    size_t                    fingerCount = 0;

    try {
        fingerCount = std::stoul(std::string{data[0]});
    } catch (...) {
        result.setError(std::format("Invalid value {} for finger count", data[0]).c_str());
        return result;
    }

    const std::string DIRSTR = std::string{data[1]};
    const bool        ISCLICK = DIRSTR == "click" || DIRSTR == "tap";
    const auto direction = ISCLICK ? TRACKPAD_GESTURE_DIR_NONE : g_pTrackpadGestures->dirForString(data[1]);

    if (!ISCLICK && direction == TRACKPAD_GESTURE_DIR_NONE) {
        result.setError(std::format("Invalid direction: {}", data[1]).c_str());
        return result;
    }

    int      startDataIdx   = 2;
    uint32_t modMask        = 0;
    float    deltaScale     = 1.F;
    bool     disableInhibit = false;

    while (true) {

        if (data[startDataIdx].starts_with("mod:")) {
            modMask = g_pKeybindManager->stringToModMask(std::string{data[startDataIdx].substr(4)});
            startDataIdx++;
            continue;
        } else if (data[startDataIdx].starts_with("scale:")) {
            try {
                deltaScale = std::clamp(std::stof(std::string{data[startDataIdx].substr(6)}), 0.1F, 10.F);
                startDataIdx++;
                continue;
            } catch (...) {
                result.setError(std::format("Invalid delta scale: {}", std::string{data[startDataIdx].substr(6)}).c_str());
                return result;
            }
        } else if (data[startDataIdx] == "disable_inhibit") {
            disableInhibit = true;
            startDataIdx++;
            continue;
        }

        break;
    }

    const std::string ACTION = std::string{data[startDataIdx]};
    std::string       ARG;
    for (int i = startDataIdx + 1; !data[i].empty(); ++i) {
        if (!ARG.empty())
            ARG += ",";
        ARG += std::string{data[i]};
    }

    if (ISCLICK) {
        if (ACTION == "unset")
            removeClickGesture(fingerCount, modMask);
        else
            addClickGesture(fingerCount, ACTION, modMask, disableInhibit, ARG);
        return result;
    }

    const auto resultFromGesture = applyTrackpadGesture(fingerCount, direction, ACTION, modMask, deltaScale, disableInhibit, ARG);

    if (!resultFromGesture)
        result.setError(resultFromGesture.error().c_str());

    return result;
}

// `hyprctl dispatch 'hl.plugin.hyprdrive.debug("frames")'` -- why is each
// monitor being asked to repaint right now?
//
// Why this is permanent rather than another probe build: a monitor that
// repaints when nothing moves is the most expensive bug class in this plugin
// (measured 2026-09-15: DP-1 held near 100 fps by a settled overview), and
// every hunt for one so far has cost a stress rig, a custom build and -- twice
// -- the user's session. Reading the answer out of the live compositor costs
// nothing when nobody asks.
//
// The report comes back as `.error` so plain `hyprctl dispatch` prints it; a
// dispatcher has no other way to say anything back. It is also written to the
// Hyprland log for whoever reads that instead.
static SDispatchResult onDebugDispatcher(std::string arg) {
    CConstVarList     data(arg, 0, 's', true);
    const std::string TOPIC = std::string{data[0]};
    // `activate <class>`: exactly a left click on that app's dock icon, for the
    // nested test rig (whose dock no real pointer can reach).
    if (TOPIC == "activate") {
        const std::string CLASS = data.size() > 1 ? std::string{data[1]} : std::string{};
        if (!ScrollOverview::Dock::debugActivate(Desktop::focusState()->monitor(), CLASS))
            return {.success = false, .error = std::format("hyprdrive:debug activate: no dock item for class {}", CLASS)};
        return {};
    }
    // `scratchpad`: exactly a click on the overview's Scratchpad cell -- show
    // the default scratchpad, then close the overview (the test rig's trigger).
    if (TOPIC == "scratchpad") {
        const auto MONITOR = Desktop::focusState()->monitor();
        ScrollOverview::Scratchpad::show(ScrollOverview::Scratchpad::ensureDefault(MONITOR));
        if (const auto OV = scrollOverviewForMonitor(MONITOR))
            OV->close();
        return {};
    }
    // `type <text>` / `type`: set the open overview's search query as if typed
    // (empty clears it) -- the rig has no keyboard that reaches the overview.
    // `mem`: what hyprdrive holds on the GPU, as far as it can count it.
    if (TOPIC == "mem") {
        const auto [N, BYTES] = OverviewWindow::texturePrefilterCacheBytes();
        std::string report    = std::format("hyprdrive mem: prefilter {} entries {:.1f} MiB", N, BYTES / 1048576.0);
        for (const auto& M : State::monitorState()->monitors())
            if (M)
                report += std::format("  | {} {}x{} full-screen RGBA8 = {:.1f} MiB", M->m_name, (int)M->m_transformedSize.x, (int)M->m_transformedSize.y,
                                      M->m_transformedSize.x * M->m_transformedSize.y * 4.0 / 1048576.0);
        return {.success = false, .error = report};
    }
    if (TOPIC == "type") {
        const std::string Q = data.size() > 1 ? std::string{data[1]} : std::string{};
        g_search.query.text   = Q;
        g_search.query.active = !Q.empty();
        g_search.nav.zone     = EOverviewNavZone::Search;
        g_search.nav.intent   = ESearchIntent::Typing;
        afterQueryEdit();
        for (const auto& overview : scrollOverviews())
            if (overview)
                overview->damage();
        return {};
    }
    if (TOPIC != "frames")
        return {.success = false, .error = "hyprdrive:debug frames|mem|activate <class>|scratchpad|type [text]"};

    std::string report = "hyprdrive frames:";
    for (const auto& monitor : State::monitorState()->monitors()) {
        if (!monitor)
            continue;

        report += "\n  " + monitor->m_name + std::format(" @{:.2f}Hz: ", monitor->m_refreshRate) + CScrollOverview::frameCadence(monitor->m_id);

        if (const auto OVERVIEW = scrollOverviewForMonitor(monitor)) {
            report += "\n  " + OVERVIEW->frameDebugLine();
            continue;
        }

        std::string line = "\n  " + monitor->m_name + " [desktop]";
        if (const auto DOCK = ScrollOverview::Dock::frameReason(monitor))
            line += std::string(" dock:") + DOCK;
        else
            line += " idle";
        report += line;
    }

    // Also to a file: the report is multi-line and a Lua error goes through
    // hyprctl's own formatting, so this is the copy that is always readable.
    const std::string STATEDIR = std::string{std::getenv("HOME") ? std::getenv("HOME") : "/tmp"} + "/.local/state/hyprdrive";
    std::filesystem::create_directories(STATEDIR);
    if (std::ofstream out(STATEDIR + "/frames.txt", std::ios::trunc); out.is_open())
        out << report << "\n";

    return {.success = false, .error = report};
}

static SDispatchResult onGestureDispatcher(std::string arg) {
    CConstVarList data(arg, 0, 's', true);
    const std::string CMD = std::string{data[0]};
    if (CMD.empty() || CMD == "help") {
        return {.success = false,
                .error   = "hyprdrive:gesture add|remove <fingers> <direction> <action> [arg] [mod:KEYS] [scale:N] [disable_inhibit]"};
    }

    const bool REMOVE = CMD == "remove" || CMD == "unset" || CMD == "del";
    const bool ADD    = CMD == "add" || CMD == "set";
    if (!ADD && !REMOVE)
        return {.success = false, .error = std::format("hyprdrive:gesture: unknown command {}", CMD)};

    size_t fingerCount = 0;
    try {
        fingerCount = std::stoul(std::string{data[1]});
    } catch (...) {
        return {.success = false, .error = "hyprdrive:gesture: fingers must be an integer"};
    }

    const std::string DIRSTR = std::string{data[2]};
    const bool        ISCLICK = DIRSTR == "click" || DIRSTR == "tap";
    const auto direction = ISCLICK ? TRACKPAD_GESTURE_DIR_NONE : g_pTrackpadGestures->dirForString(data[2]);
    if (!ISCLICK && direction == TRACKPAD_GESTURE_DIR_NONE)
        return {.success = false, .error = std::format("Invalid direction: {}", DIRSTR)};

    int      idx            = 3;
    uint32_t modMask        = 0;
    float    deltaScale     = 1.F;
    bool     disableInhibit = false;
    while (!data[idx].empty()) {
        const auto TOK = std::string{data[idx]};
        if (TOK.starts_with("mod:")) {
            modMask = g_pKeybindManager->stringToModMask(TOK.substr(4));
            idx++;
            continue;
        }
        if (TOK.starts_with("scale:")) {
            try {
                deltaScale = std::clamp(std::stof(TOK.substr(6)), 0.1F, 10.F);
            } catch (...) {
                return {.success = false, .error = "invalid scale"};
            }
            idx++;
            continue;
        }
        if (TOK == "disable_inhibit") {
            disableInhibit = true;
            idx++;
            continue;
        }
        break;
    }

    const std::string ACTION = REMOVE ? "unset" : std::string{data[idx]};
    std::string       extra;
    if (!REMOVE) {
        for (int i = idx + 1; !data[i].empty(); ++i) {
            const auto TOK = std::string{data[i]};
            if (TOK.starts_with("mod:") || TOK.starts_with("scale:") || TOK == "disable_inhibit")
                continue;
            if (!extra.empty())
                extra += " ";
            extra += TOK;
        }
    }

    if (ISCLICK) {
        if (REMOVE)
            removeClickGesture(fingerCount, modMask);
        else
            addClickGesture(fingerCount, ACTION, modMask, disableInhibit, extra);
        return {};
    }

    const auto res = applyTrackpadGesture(fingerCount, direction, ACTION, modMask, deltaScale, disableInhibit, extra);
    if (!res)
        return {.success = false, .error = res.error()};
    return {};
}

// OVERVIEW_CPP_SPEC §13.9 X24. hyprgnomedock does not exist yet, so this is how
// the minimize engine is driven and tested until its icons can call it.
static SDispatchResult onMinimizeDispatcher(std::string arg) {
    if (arg == "restore") {
        const auto MINIMIZED = ScrollOverview::Minimize::minimized();
        if (MINIMIZED.empty())
            return {.success = false, .error = "nothing is minimized"};

        // X54: back out through the DOCK, so it flies from its icon instead of
        // reappearing. The dock owns the animation because it owns the target.
        ScrollOverview::Dock::restoreWithFlight(Desktop::focusState()->monitor(), MINIMIZED.back());
        return {};
    }

    const auto WINDOW = Desktop::focusState()->window();
    if (!WINDOW)
        return {.success = false, .error = "no focused window"};

    ScrollOverview::Dock::minimizeWithFlight(Desktop::focusState()->monitor(), WINDOW);
    return {};
}

// OVERVIEW_CPP_SPEC §13.9 X34. All thirty accelerators and `<Super>q` land
// here; the compositor's `bind` lines carry which is which.
static SDispatchResult onHotkeyDispatcher(std::string arg) {
    if (arg == "overlay") {
        ScrollOverview::Dock::showOverlay();
        return {};
    }

    bool shift = false;
    bool ctrl  = false;
    if (arg.starts_with("shift ")) {
        shift = true;
        arg   = arg.substr(6);
    } else if (arg.starts_with("ctrl ")) {
        ctrl = true;
        arg  = arg.substr(5);
    }

    ScrollOverview::Dock::hotkey(std::stoi(arg), shift, ctrl);
    return {};
}

// V11 SPEC V4. THE STACK DISPATCHER, and the reason there is one at all.
//
// A stack is meant to be created by dragging a file out of a file manager and
// onto the dock. That gesture cannot reach this plugin: in Hyprland 0.56 the
// drop target of an XDG drag is always a wl_surface (CSeatManager's
// dndPointerFocus is a WP<CWLSurfaceResource>) and this dock is not a surface
// -- it is drawn straight into the render pass -- while every member that
// would let a non-surface claim the offer is private on CWLDataDeviceProtocol
// with `friend` limited to CSeatManager. The full finding is in DockStacks.hpp.
//
// So the pin is a DISPATCHER, which is the same shape every other scriptable
// thing in this plugin has, and a Nautilus script wraps it for the file
// manager's own right-click menu (scripts/install-nautilus-pin.sh).
//
//     hyprdrive:stack pin /home/you/Downloads
//     hyprdrive:stack unpin /home/you/Downloads
//     hyprdrive:stack toggle /home/you/Downloads     -- pin if not pinned, else unpin
//     hyprdrive:stack view grid /home/you/Downloads
//     hyprdrive:stack sort mtime /home/you/Downloads
//     hyprdrive:stack clear                          -- every stack, for gate_slate
//
// A path is taken RAW to the end of the line: paths contain spaces, and any
// splitting cleverer than "the verb, then the rest" would refuse the ones that
// need this most.
static SDispatchResult onStackDispatcher(std::string arg) {
    const auto CUT  = arg.find(' ');
    const auto VERB = arg.substr(0, CUT);
    auto       rest = CUT == std::string::npos ? std::string{} : arg.substr(CUT + 1);
    while (!rest.empty() && rest.front() == ' ')
        rest.erase(rest.begin());

    if (VERB == "clear") {
        ScrollOverview::Stacks::clearAll();
        return {};
    }

    // `open <path>`: open (or, if it is open, fold) that stack's fan or pane on
    // the focused monitor's dock, as a click on its cell would -- for keybinds,
    // and for testing without touching the pointer.
    if (VERB == "open") {
        const auto MONITOR = Desktop::focusState()->monitor();
        if (!MONITOR)
            return {.success = false, .error = "no focused monitor"};
        std::string want = rest;
        while (want.size() > 1 && want.back() == '/')
            want.pop_back();
        for (const auto& ITEM : ScrollOverview::Dock::layout(MONITOR).items) {
            if (ITEM.stack && !ITEM.placeholder && ITEM.stackPath == want) {
                ScrollOverview::Stacks::open(MONITOR, ITEM, ITEM.iconBox);
                ScrollOverview::Dock::damageIfChanged(MONITOR);
                g_pHyprRenderer->damageMonitor(MONITOR);
                return {};
            }
        }
        return {.success = false, .error = "no such stack on this dock"};
    }

    if (VERB == "view" || VERB == "sort") {
        const auto CUT2 = rest.find(' ');
        if (CUT2 == std::string::npos)
            return {.success = false, .error = "expected a mode and a path"};

        const auto MODE = rest.substr(0, CUT2);
        auto       path = rest.substr(CUT2 + 1);
        while (!path.empty() && path.front() == ' ')
            path.erase(path.begin());

        if (VERB == "view") {
            if (MODE != "fan" && MODE != "grid" && MODE != "auto")
                return {.success = false, .error = "expected auto, fan or grid"};
            ScrollOverview::Stacks::setViewMode(path, ScrollOverview::StackModel::viewFromName(MODE));
            return {};
        }

        const auto SORT = MODE == "mtime" ? std::optional{ScrollOverview::StackModel::ESort::Modified} : ScrollOverview::StackModel::sortFromName(MODE);
        if (!SORT)
            return {.success = false, .error = "expected name, added, modified (mtime) or kind"};
        ScrollOverview::Stacks::setSort(path, *SORT);
        return {};
    }

    if (rest.empty())
        return {.success = false, .error = "expected a path"};

    if (VERB == "pin") {
        if (!ScrollOverview::Stacks::pin(rest))
            return {.success = false, .error = "not pinned: the path does not exist, or is already a stack"};
        return {};
    }

    if (VERB == "unpin") {
        if (!ScrollOverview::Stacks::unpin(rest))
            return {.success = false, .error = "not a stack"};
        return {};
    }

    if (VERB == "toggle") {
        if (!ScrollOverview::Stacks::unpin(rest) && !ScrollOverview::Stacks::pin(rest))
            return {.success = false, .error = "the path does not exist"};
        return {};
    }

    return {.success = false, .error = "expected pin, unpin, toggle, view, sort or clear"};
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    SCROLLOVERVIEW_HANDLE = handle;

    // Before anything can record a use of a pre-rename name. ONE RELEASE.
    ScrollOverview::Compat::openLog();

    const std::string HASH        = __hyprland_api_get_hash();
    const std::string CLIENT_HASH = __hyprland_api_get_client_hash();

    if (HASH != CLIENT_HASH) {
        failNotif("Version mismatch (headers ver is not equal to running hyprland ver)");
        throw std::runtime_error("[he] Version mismatch");
    }

    g_pScrollRenderWorkspaceHook = HyprlandAPI::createFunctionHook(
        SCROLLOVERVIEW_HANDLE,
        findFnOrThrow("renderWorkspace", {"CHyprRenderer::renderWorkspace(", "IHyprRenderer::renderWorkspace("}),
        rc<void*>(hkRenderWorkspace));

    g_pScrollScheduleFrameHook = HyprlandAPI::createFunctionHook(
        SCROLLOVERVIEW_HANDLE, 
        findFnOrThrow("_ZN7Monitor8CMonitor13scheduleFrameEN10Aquamarine7IOutput19scheduleFrameReasonE", {""}),
        rc<void*>(hkScheduleFrame));

    g_pScrollDamageSurfaceHook = HyprlandAPI::createFunctionHook(
        SCROLLOVERVIEW_HANDLE,
        findFnOrThrow("damageSurface", {"CHyprRenderer::damageSurface(", "IHyprRenderer::damageSurface("}),
        rc<void*>(hkDamageSurface));

    g_pScrollSendFrameEventsHook = HyprlandAPI::createFunctionHook(
        SCROLLOVERVIEW_HANDLE,
        findFnOrThrow("sendFrameEventsToWorkspace", {"CHyprRenderer::sendFrameEventsToWorkspace(", "IHyprRenderer::sendFrameEventsToWorkspace("}),
        rc<void*>(hkSendFrameEventsToWorkspace));

    g_pScrollSurfaceFrameHook = HyprlandAPI::createFunctionHook(
        SCROLLOVERVIEW_HANDLE,
        findFnOrThrow("_ZN18CWLSurfaceResource5frameERKNSt6chrono10time_pointINS0_3_V212steady_clockENS0_8durationIlSt5ratioILl1ELl1000000000EEEEEE", {""}),
        rc<void*>(hkSurfaceFrame));

    g_pScrollAddDamageHookB = HyprlandAPI::createFunctionHook(
        SCROLLOVERVIEW_HANDLE,
        findFnOrThrow("addDamageEPK15pixman_region32", {"CMonitor::addDamage"}),
        rc<void*>(hkAddDamageB));

    g_pScrollAddDamageHookA = HyprlandAPI::createFunctionHook(
        SCROLLOVERVIEW_HANDLE,
        findFnOrThrow("_ZN7Monitor8CMonitor9addDamageERKN9Hyprutils4Math4CBoxE", {""}),
        rc<void*>(hkAddDamageA));

    // X30. Hooked HERE and not through ensureScrollOverviewHooks(): those go on
    // and off with the overview, and the autohide barrier lives on the plain
    // desktop, which is exactly where they are not installed.
    g_pScrollMouseMovedHook = HyprlandAPI::createFunctionHook(
        SCROLLOVERVIEW_HANDLE,
        findFnOrThrow("onMouseMoved", {"CInputManager::onMouseMoved("}),
        rc<void*>(hkOnMouseMoved));
    g_pScrollMouseMovedHook->hook();

    g_pSendWindowSizeHook = HyprlandAPI::createFunctionHook(SCROLLOVERVIEW_HANDLE, findFnOrThrow("sendWindowSize", {"CWindow::sendWindowSize("}), rc<void*>(hkSendWindowSize));
    g_pSendWindowSizeHook->hook();

    g_pTargetPlaceHook = HyprlandAPI::createFunctionHook(SCROLLOVERVIEW_HANDLE, findFnOrThrow("setPositionGlobal", {"CWindowTarget::setPositionGlobal("}), rc<void*>(hkTargetPlace));
    g_pTargetPlaceHook->hook();
    ScrollOverview::ConfigureHold::setSender([](Desktop::View::CWindow* window, bool force) {
        if (g_pSendWindowSizeHook)
            rc<origSendWindowSize>(g_pSendWindowSizeHook->m_original)(window, force);
    });

    CursorShape::init();
    OpenProf::init();
    // NOT FramePacing::apply() here: plugin keys are registered further down
    // (registerConfig), and reading an unregistered key aborts Hyprland in
    // CConfigValue::bind -- which is exactly how 2026-09-29's build crash-looped
    // every login. The config.reloaded hook and every overview open apply it.
    g_pRenderPreHook = Event::bus()->m_events.render.pre.listen([](PHLMONITOR monitor) {
        // Throttled inside to a few stat()s every couple of seconds.
        AppSearch::pollForChanges();

        if (const auto overview = scrollOverviewForMonitor(monitor))
            overview->onPreRender();

        ScrollOverview::Dock::damageIfChanged(monitor);

        // Layout motion blur (FlightBlur.hpp). The overview draws its own
        // windows; there, only the sweep runs.
        ScrollOverview::DockRoom::watch(monitor);

        const auto FLYING = [](const PHLWINDOW& w) { return ScrollOverview::Flight::hasFlight(w); };
        if (!scrollOverviewForMonitor(monitor))
            ScrollOverview::FlightBlur::trackLayoutMotion(monitor, FLYING);
        else {
            // The overview draws its own tiles; only a special workspace (the
            // scratchpad opening or closing over it) is drawn natively, through
            // the renderer the blur rides (user, 2026-09-30).
            ScrollOverview::FlightBlur::trackLayoutMotion(monitor, [&FLYING](const PHLWINDOW& w) {
                return FLYING(w) || !w->m_workspace || !w->m_workspace->m_isSpecialWorkspace;
            });
        }
    });

    // X27. NOT the renderWorkspace hook: that one is installed on demand by
    // ensureScrollOverviewHooks() and dropped again by removeOverview(), so it
    // does not exist on a plain desktop -- which is exactly where a dock lives.
    // POST_WINDOWS also puts the dock above windows and below the bar's top
    // layer.
    //
    // §10 clause 6, pinned-in-overview: this stage fires far below the overview's
    // own pass, so the pill was buried under it. The overview now draws the dock
    // itself at the end of its render, and this hook stands down for that monitor
    // -- otherwise the pill is drawn twice and its translucent background blends
    // over itself.
    // The dock's hover highlight on a PLAIN desktop. render.pre only fires when a
    // frame is already happening, so on an idle desktop a pointer move over the
    // pill damages nothing and the highlight never appears. The overview has its
    // own mouse hook and calls this from there.
    // X30. Flipping dock-fixed at runtime damages nothing by itself: with the
    // pointer still and no window repainting, there is no frame for the dock to
    // be drawn into and the change would not show until something else moved.
    // P6i1 SHADOW. Everything the compositor tells us that can change the
    // membership gate's answer: what windows exist, and the four properties the
    // gate reads off one. The plugin's own mutators -- setHidden for a flight,
    // minimize/restore, the search query -- bump at their own call sites, since
    // no event announces them.
    g_pMembOpenHook  = Event::bus()->m_events.window.open.listen([](PHLWINDOW) { Membership::bump("window.open"); });
    g_pMembLateHook  = Event::bus()->m_events.window.openLate.listen([](PHLWINDOW) { Membership::bump("window.openLate"); });
    g_pMembCloseHook = Event::bus()->m_events.window.close.listen([](PHLWINDOW) { Membership::bump("window.close"); });
    g_pMembKillHook  = Event::bus()->m_events.window.kill.listen([](PHLWINDOW) { Membership::bump("window.kill"); });
    g_pMembGoneHook  = Event::bus()->m_events.window.destroy.listen([](PHLWINDOWREF) { Membership::bump("window.destroy"); });
    g_pMembPinHook   = Event::bus()->m_events.window.pin.listen([](PHLWINDOW) { Membership::bump("window.pin"); });
    g_pMembFloatHook = Event::bus()->m_events.window.floating.listen([](PHLWINDOW) { Membership::bump("window.floating"); });
    g_pMembTitleHook = Event::bus()->m_events.window.title.listen([](PHLWINDOW) { Membership::bump("window.title"); });
    g_pMembClassHook = Event::bus()->m_events.window.class_.listen([](PHLWINDOW) { Membership::bump("window.class"); });
    g_pMembWsHook    = Event::bus()->m_events.window.moveToWorkspace.listen([](PHLWINDOW, PHLWORKSPACE) { Membership::bump("window.moveToWorkspace"); });
    g_pMembFullHook  = Event::bus()->m_events.window.fullscreen.listen([](PHLWINDOW) { Membership::bump("window.fullscreen"); });

    // The dock's keys as they stood BEFORE this reload parsed anything. See the
    // abortAll below for what it is compared against.
    static uint64_t s_dockKeysBeforeReload = 0;

    g_pClickPreReloadHook = Event::bus()->m_events.config.preReload.listen([] {
        clearClickGestures();
        s_dockKeysBeforeReload = ScrollOverview::Config::dockKeyFingerprint();
    });

    g_pDockConfigHook = Event::bus()->m_events.config.reloaded.listen([] {
        // A no-op unless the pointer set changed -- a reload is not a reason to
        // drop a touchpad gesture that is still under the hand.
        attachPadFingerListeners();
        // FIRST, before anything in this plugin reads a config value this pass:
        // a value the user set under the old plugin:scrolloverview: name is
        // copied onto its plugin:hyprdrive: twin here and nowhere else, so the
        // rest of the codebase only ever knows the new name. ONE RELEASE.
        ScrollOverview::Compat::adoptLegacyValues();
        // A reload writes the user's render:new_render_scheduling back.
        ScrollOverview::FramePacing::apply();

        // `omarchy theme set` writes colors.toml and reloads Hyprland, so the
        // reload IS the theme change as far as this plugin is concerned. Drop
        // the palette and font caches here and the new theme is on the next
        // frame rather than up to half a second later.
        ScrollOverview::Config::invalidateThemeCache();

        // A reload rebuilds the dock's whole layout under whatever is flying,
        // and a flight's boxes are the OLD layout's -- its icon target can move,
        // shrink, or stop existing. Land them first, with the landing every
        // other end of a flight gets, rather than let one chase an icon that is
        // no longer there.
        //
        // ONLY when a dock key actually moved, though. Every theme switch and
        // every unrelated `hyprctl reload` came through here and snapped an
        // in-flight minimize to its end, for a reload that cannot have touched
        // the layout the flight is aiming at. Taken AFTER adoptLegacyValues so a
        // value arriving under the old name counts as the change it is.
        if (const auto DOCKKEYS = ScrollOverview::Config::dockKeyFingerprint(); DOCKKEYS != s_dockKeysBeforeReload) {
            s_dockKeysBeforeReload = DOCKKEYS;
            ScrollOverview::Flight::abortAll("config-reload");
        }

        for (const auto& MONITOR : State::monitorState()->monitors())
            ScrollOverview::Dock::damageIfChanged(MONITOR);

        // The reload just overwrote the overrides an OPEN overview installed
        // (workspace animations, follow_mouse, no_warps) -- without this, a
        // workspace switch inside the overview plays Hyprland's slide again.
        // Only the shared-state owner acts; the method self-guards.
        for (const auto& OVERVIEW : scrollOverviews()) {
            if (const auto SCROLL = OVERVIEW ? dynamic_cast<CScrollOverview*>(OVERVIEW.get()) : nullptr)
                SCROLL->reapplyConfigOverridesAfterReload();
        }
    });

    g_pDockHoverHook = Event::bus()->m_events.input.mouse.move.listen([](Vector2D, Event::SCallbackInfo& info) {
        const auto HIT = monitorUnderCursor();

        // The scratchpad's drag-out zone rides the same motion (it is a desktop
        // surface like the dock, and a drag is nothing but motion).
        ScrollOverview::ScratchpadExit::onPointerMove(HIT ? HIT->first : PHLMONITOR{});
        // Dock stacks B: a drag of files over the dock (prefetch + target).
        ScrollOverview::DockDrop::onPointerMove(HIT ? HIT->first : PHLMONITOR{}, HIT ? HIT->second : Vector2D{});
        // A fan item pulled out becomes a real drag (DockDrop::startFileDrag).
        ScrollOverview::Stacks::onPointerMove();

        if (!HIT) {
            ScrollOverview::Dock::releasePointerFocus();
            setDockCursor(false);
            return;
        }
        const auto& [MONITOR, LOCAL] = *HIT;

        // X32. The desktop dock reorders too, and its drag needs motion for
        // the same reason the highlight does: on an idle desktop nothing
        // else is asking for a frame.
        if (ScrollOverview::Dock::dragPending())
            ScrollOverview::Dock::dragMove(MONITOR, LOCAL);

        ScrollOverview::Dock::syncPointerFocus(MONITOR, LOCAL);
        ScrollOverview::Dock::damageIfChanged(MONITOR);

        // HOVER MUST NOT PASS THROUGH (user, 2026-09-29): over the dock's
        // chrome -- pill, hover previews, menu, a stack's fan/pane -- the window
        // underneath got the motion anyway (link highlights, hover states),
        // because this is a listener and Hyprland went on to focus the surface
        // under the cursor. Cancelling here returns from mouseMoveUnified before
        // that (the cursor itself has already moved). Not with the overview up
        // (it owns its own input), and not during a drag-and-drop, whose target
        // Hyprland must keep tracking.
        const bool ON_CHROME = !scrollOverviewForMonitor(MONITOR) && !ScrollOverview::DockDrop::dndInProgress() && ScrollOverview::Dock::occupiesPointer(MONITOR, LOCAL);
        if (ON_CHROME)
            info.cancelled = true;
        // The cancel also skips Hyprland's cursor-shape update, so a pointer that
        // came off a text field kept the I-beam over the dock. The dock owns the
        // lowest override group (UNKNOWN -- Hyprland itself never sets it), so a
        // resize edge, a drag or the overview's own override still win.
        setDockCursor(ON_CHROME);
    });

    // X24 dock-side. The overview installs its own button hook; this one is the
    // plain desktop, where nothing else claims a click on the pill. Both edges
    // are swallowed over the pill: the dock is not a window, so a press that
    // reached the window underneath would focus it, and a release that leaked
    // after a swallowed press would leave the pointer stuck.
    g_pDockClickHook = Event::bus()->m_events.input.mouse.button.listen([](IPointer::SButtonEvent event, Event::SCallbackInfo& info) {
        if (handleClickGestureButton(event)) {
            info.cancelled = true;
            return;
        }

        const auto HIT = monitorUnderCursor();
        if (!HIT)
            return;
        const auto& [MONITOR, LOCAL] = *HIT;

        // Dock stacks B: a drag of files released over the dock is the dock's.
        // Runs before Hyprland's own drop listener (registered at drag start).
        if (ScrollOverview::DockDrop::onButton(MONITOR, LOCAL, event.state == WL_POINTER_BUTTON_STATE_PRESSED)) {
            info.cancelled = true;
            return;
        }

        // The plain desktop's button map is fixed: right opens the menu, middle
        // launches, left drives the pill. (The overview honours the left-handed
        // config; the desktop dock never has.) The ladder and the reasons for its
        // order live in DockInput -- this is the same dock the overview drives.
        const auto ROLE = event.button == BTN_RIGHT  ? ScrollOverview::DockInput::EButton::Menu
                        : event.button == BTN_MIDDLE ? ScrollOverview::DockInput::EButton::Launch
                        : event.button == BTN_LEFT   ? ScrollOverview::DockInput::EButton::Main
                        : ScrollOverview::DockInput::EButton::Other;
        const bool PRESSED = event.state == WL_POINTER_BUTTON_STATE_PRESSED;

        // A window dragged back onto the dock goes home the way it does in the
        // overview: the whole minimize FLIGHT -- shrink into the icon, bounce on
        // landing -- and not a silent snap. This is new because the premise
        // below used to hold: before the dock tear-out there was no such thing
        // as a window drag on the plain desktop, so there was nothing to catch.
        if (!PRESSED && ROLE == ScrollOverview::DockInput::EButton::Main && ScrollOverview::Dock::dockOwnsNativeDrag() && g_layoutManager &&
            g_layoutManager->dragController()) {
            const auto TARGET  = g_layoutManager->dragController()->target();
            const auto DRAGGED = TARGET ? TARGET->window() : PHLWINDOW{};
            const bool ONDOCK  = ScrollOverview::Dock::pillContains(MONITOR, LOCAL);

            // END IT OURSELVES. The dock swallowed the press that started this
            // drag, so Hyprland never entered its own button state machine for
            // it and nothing on its side will let go -- the button came up and
            // the window kept following the pointer until the NEXT click.
            g_layoutManager->dragController()->dragEnd();
            ScrollOverview::Dock::clearDockNativeDrag();
            const auto ANCHOR = ScrollOverview::Placement::takeDragAnchor();

            // Dropped back onto the dock: the whole minimize flight, same as the
            // overview does, rather than a silent snap.
            if (ONDOCK && DRAGGED)
                ScrollOverview::Flight::minimize(MONITOR, DRAGGED, true); // bounce at the drop, Z10
            // Anywhere else: dragEnd just tiled it off the CURSOR (the window under
            // it, the side it was on). Re-file it the way opening it would --
            // AFTER dragEnd, which is also what has lowered the drag flag.
            else if (DRAGGED)
                ScrollOverview::Placement::retileAsNew(DRAGGED, ANCHOR);

            info.cancelled = true;
            return;
        }

        // Desktop never has a window drag in flight, and lets a menu miss through
        // (its menu may have no drawn geometry to hit-test); a let-through miss
        // then falls into the pill ladder, exactly as the overview's own dock does.
        if (ScrollOverview::DockInput::handleMenu(MONITOR, LOCAL, ROLE, PRESSED, false, true).claimed) {
            info.cancelled = true;
            return;
        }
        if (ScrollOverview::DockInput::handlePill(MONITOR, LOCAL, ROLE, PRESSED, false).claimed) {
            info.cancelled = true;
            return;
        }

        // Hold-Super window pills are disabled on this fork.
    });

    // X33. The wheel over the pill, on the PLAIN desktop only.
    //
    // Deliberate divergence, and the one place X33 does not copy the reference:
    // with the overview open the reference scrolls the dash's icon to
    // `this.app.activate()`, which would close the overview on a wheel graze.
    // The pill is pinned OVER the overview here (§10 clause 6) and the wheel is
    // the overview's primary navigation, so inside it the overview keeps the
    // tick. Recorded rather than silently dropped.
    g_pDockScrollHook = Event::bus()->m_events.input.mouse.axis.listen([](IPointer::SAxisEvent event, Event::SCallbackInfo& info) {
        // Vertical only. A horizontal wheel has no reference behaviour.
        // Wheel, finger, and continuous (touchpad) all count: SOURCE_WHEEL
        // alone left laptop two-finger scrolls going to the window under the
        // pill. Dead time in Dock::scroll turns a finger stream into steps.
        if (event.axis != WL_POINTER_AXIS_VERTICAL_SCROLL || event.delta == 0.0)
            return;
        if (event.source == WL_POINTER_AXIS_SOURCE_WHEEL_TILT)
            return;

        const auto HIT = monitorUnderCursor();
        if (!HIT)
            return;
        const auto& [MONITOR, LOCAL] = *HIT;

        if (ScrollOverview::Dock::occupiesPointer(MONITOR, LOCAL))
            ScrollOverview::Dock::syncPointerFocus(MONITOR, LOCAL);

        if (ScrollOverview::Dock::scroll(MONITOR, LOCAL, event.delta < 0.0))
            info.cancelled = true;
    });

    g_pDockRenderHook = Event::bus()->m_events.render.stage.listen([](eRenderStage stage) {
        if (stage != RENDER_POST_WINDOWS)
            return;

        const auto MONITOR = g_pHyprRenderer->m_renderData.pMonitor.lock();
        if (scrollOverviewForMonitor(MONITOR))
            return;

        // Y7. No overview means no window drag, and this is also what clears
        // the flag if the overview closed while one was in flight.
        ScrollOverview::Dock::setWindowDragActive(false);

        ScrollOverview::Dock::render(MONITOR);
        // A file dragged out of a fan: its icon under the pointer, over the dock.
        ScrollOverview::DockDrop::renderGhost(MONITOR);
    });

    // The scratchpad's drag-out zone draws at the LAST moment, not with the
    // dock: it sits at the very top of the screen (user, 2026-09-24: "bump it
    // all the way to the top"), where the bar lives, and POST_WINDOWS is below
    // the top layer -- the bar would paint over it.
    g_pScratchExitRenderHook = Event::bus()->m_events.render.stage.listen([](eRenderStage stage) {
        if (stage != RENDER_LAST_MOMENT)
            return;
        const auto MONITOR = g_pHyprRenderer->m_renderData.pMonitor.lock();
        if (!MONITOR || scrollOverviewForMonitor(MONITOR))
            return;
        ScrollOverview::ScratchpadExit::render(MONITOR);
    });

    // P0. An overview must not outlive its output. Nothing used to tear one
    // down when its monitor went away, so it stayed in the registry holding a
    // dead PHLMONITORREF and the next `overview off` dereferenced it inside
    // close() -- a SIGSEGV, which gdm then papered over by bringing the session
    // straight back WITHOUT the user's config, so the damage outlived the crash.
    // `hyprctl output remove` and a user unplugging a display raise this same
    // event, and it is the one Hyprland's own monitor-reference holders listen
    // to. It is deliberately NOT emitted while the compositor is shutting down,
    // which is what keeps this from queueing a deferred destroy that would never
    // run -- PLUGIN_EXIT owns teardown there.
    //
    // Flights first: they are settled while the overview is still registered, so
    // an unhidden window is re-added to the draw list it is about to leave with
    // rather than after it.
    // Before the desktop is drawn, not after: corrected on the first frame it
    // would slide the window across the screen instead of simply being right.
    // See normalizeFittingScrollingOffset.
    g_pScrollCameraActiveHook = Event::bus()->m_events.workspace.active.listen([](PHLWORKSPACE workspace) { normalizeScrollingCamera(workspace); });
    g_pScrollCameraMovedHook  = Event::bus()->m_events.workspace.moveToMonitor.listen([](PHLWORKSPACE workspace, PHLMONITOR) { normalizeScrollingCamera(workspace); });
    // And whatever is already stuck when the plugin comes up.
    normalizeAllScrollingCameras();

    g_pMonitorGoneHook = Event::bus()->m_events.monitor.removed.listen([](PHLMONITOR monitor) {
        ScrollOverview::Flight::abortAll("monitor-removed");
        dropScrollOverviewsForMonitor(monitor);
        if (monitor) {
            ScrollOverview::Dock::forgetMonitor(monitor->m_id);
            if (const auto OPENGL = g_pHyprRenderer ? g_pHyprRenderer->glBackend() : WP<Render::GL::CHyprOpenGLImpl>{})
                OPENGL->makeEGLCurrent();
            ScrollOverview::releaseWallpaperStash(monitor->m_id);
        }
        // A stack popup on the monitor that went away must not keep the keyboard.
        if (monitor && ScrollOverview::Stacks::popupMonitor() == monitor->m_id)
            ScrollOverview::Stacks::close();
    });

    ScrollOverview::Config::registerDispatcher("overview", ::onOverviewDispatcher);
    ScrollOverview::Config::registerDispatcher("navigate", ::onNavigateDispatcher);
    ScrollOverview::Config::registerDispatcher("window", ::onWindowDispatcher);
    ScrollOverview::Config::registerDispatcher("minimize", ::onMinimizeDispatcher);
    ScrollOverview::Config::registerDispatcher("hotkey", ::onHotkeyDispatcher);
    ScrollOverview::Config::registerDispatcher("stack", ::onStackDispatcher);
    ScrollOverview::Config::registerDispatcher("gesture", ::onGestureDispatcher);
    ScrollOverview::Config::registerDispatcher("debug", ::onDebugDispatcher);
    ScrollOverview::Config::registerGesture(::onRegisterOverviewGesture, ::overviewGestureKeyword);
    ScrollOverview::Config::registerConfig();
    // After a reload, windows are still parked on the stash with no ledger entry
    // behind them; without this they can never be restored.
    ScrollOverview::Minimize::adoptStashedWindows();
    registerRawKeyHooks();
    attachPadFingerListeners();

    // Reload can drop live window alphas until decorations are recomputed.
    // Re-apply default-opacity (0.985 / 0.85) to already-mapped windows.
    if (Desktop::globalWindowController())
        Desktop::globalWindowController()->updateAllWindowsDecorations();

    // ONE RELEASE. Pinned dock apps are user data; they do not get to disappear
    // because the plugin changed its name. Both are one-shot and no-ops once
    // the config and the store have been migrated.
    ScrollOverview::Compat::adoptLegacyFavorites();
    ScrollOverview::Compat::adoptLegacyValues();

    if (g_pCompositor && g_pCompositor->m_wlEventLoop) {
        g_fileWakeFd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        if (g_fileWakeFd >= 0) {
            g_fileWakeSource = wl_event_loop_add_fd(g_pCompositor->m_wlEventLoop, g_fileWakeFd, WL_EVENT_READABLE, onFileResultsReady, nullptr);
            OmarchySearch::setResultsCallback([]() {
                if (g_fileWakeFd >= 0) {
                    uint64_t one = 1;
                    ::write(g_fileWakeFd, &one, sizeof(one));
                }
            });
            // Stack folder reads and trash jobs land off-thread too; same wake.
            ScrollOverview::Thumbs::setReadyCallback([]() {
                if (g_fileWakeFd >= 0) {
                    uint64_t one = 1;
                    ::write(g_fileWakeFd, &one, sizeof(one));
                }
            });
            ScrollOverview::StackReader::setReadyCallback([]() {
                if (g_fileWakeFd >= 0) {
                    uint64_t one = 1;
                    ::write(g_fileWakeFd, &one, sizeof(one));
                }
            });
            // The dictionary/translator answers off-thread too; same wake.
            ToolSearch::setReadyCallback([]() {
                if (g_fileWakeFd >= 0) {
                    uint64_t one = 1;
                    ::write(g_fileWakeFd, &one, sizeof(one));
                }
            });
        }
    }

    // X51/X52. Build the icon index at LOAD, where a one-off ~10ms pass is
    // invisible, rather than letting the first search pay for it inside a frame
    // that is already trying to paint results.
    // Y13. ASYNC. Building this inline cost 4237ms of PARSE TIME, which is
    // time the compositor spends before exec-once and therefore before the
    // wallpaper can even be asked for.
    AppSearch::warmIconIndexAsync();
    // Decode icons ahead of the first open / first keystroke (OverviewRender.hpp).
    OverviewRender::startIconWarmer();

    return {ScrollOverview::Compat::NAME, "Mission-control overview + dock for Hyprland", "Vaxry, yayuuu, dragos", SCROLLOVERVIEW_VERSION};
}

APICALL EXPORT void PLUGIN_EXIT() {
    stopSearchRepeat();
    if (g_searchRepeatTimer) {
        wl_event_source_remove(g_searchRepeatTimer);
        g_searchRepeatTimer = nullptr;
    }
    if (g_letterTickTimer) {
        wl_event_source_remove(g_letterTickTimer);
        g_letterTickTimer = nullptr;
    }
    OmarchySearch::setResultsCallback(nullptr);
    ToolSearch::shutdown();
    // BEFORE the wake fd below is closed: the reader's worker writes to it when
    // a job lands, and a job landing after the close would write to a reused fd
    // (review 2026-09-24).
    ScrollOverview::Stacks::shutdown();
    ScrollOverview::StackReader::shutdown();
    ScrollOverview::Thumbs::shutdown(); // before the wake fd closes: its worker writes to it
    if (g_fileWakeSource) {
        wl_event_source_remove(g_fileWakeSource);
        g_fileWakeSource = nullptr;
    }
    if (g_fileWakeFd >= 0) {
        close(g_fileWakeFd);
        g_fileWakeFd = -1;
    }
    g_pRawKeyHook.reset();
    g_pTapSuperButtonHook.reset();
    g_pRenderPreHook.reset();
    CursorShape::shutdown();
    ScrollOverview::FramePacing::shutdown();
    OpenProf::shutdown();
    OvInternal::cancelPointerHandoff();
    g_pMembOpenHook.reset();
    g_pMembLateHook.reset();
    g_pMembCloseHook.reset();
    g_pMembKillHook.reset();
    g_pMembGoneHook.reset();
    g_pMembPinHook.reset();
    g_pMembFloatHook.reset();
    g_pMembTitleHook.reset();
    g_pMembClassHook.reset();
    g_pMembWsHook.reset();
    g_pMembFullHook.reset();
    g_pDockConfigHook.reset();
    g_pDockHoverHook.reset();
    g_pDockClickHook.reset();
    ScrollOverview::DockDrop::shutdown();
    g_pDockScrollHook.reset();
    g_pDockRenderHook.reset();
    g_pScratchExitRenderHook.reset();
    g_pMonitorGoneHook.reset();
    g_pScrollCameraActiveHook.reset();
    g_pScrollCameraMovedHook.reset();

    g_unloading = true;
    setDockCursor(false);
    ScrollOverview::Dock::releasePointerFocus();
    // FIRST: a flight in the air at unload is a window that is hidden, stashed,
    // or mid-geometry with nothing left to finish it -- the plugin's code IS the
    // thing that would have landed it. hyprpm reloads this plugin in-process, so
    // "the session is ending anyway" is not true here.
    ScrollOverview::Flight::abortAll("plugin-exit");
    // The D-Bus worker outlives the overview otherwise, and hyprpm reloads the
    // plugin in-process.
    AppSearch::shutdownIconIndex();
    SearchProviders::shutdown();
    OmarchySearch::shutdown();
    ScrollOverview::LauncherEntry::stop();
    clearScrollOverviews();
    // AGAIN, after the overviews are gone: their teardown hands focus back and
    // can queue work (a cursor warp, a flight, a dock-seat refresh) after the
    // first round of cancels above -- a doLater left in Hyprland's event loop
    // is a call into unloaded code when that loop is destroyed.
    OvInternal::cancelPointerHandoff();
    ScrollOverview::Flight::abortAll("plugin-exit");
    // A flying window may hold the flight's blur transformer; its destructor is
    // this plugin's code, so it goes before the dlclose (Hyprland's own exit too).
    ScrollOverview::FlightBlur::detachAll();
    OverviewRender::stopIconWarmer();
    // Nothing may stay held once the hook goes with the plugin.
    ScrollOverview::ConfigureHold::flush();
    ScrollOverview::DockRoom::shutdown();
    ScrollOverview::Dock::releasePointerFocus();
    // Outlives every overview on purpose (Window.cpp), so it is freed here.
    if (const auto OPENGL = g_pHyprRenderer ? g_pHyprRenderer->glBackend() : WP<Render::GL::CHyprOpenGLImpl>{})
        OPENGL->makeEGLCurrent();
    OverviewWindow::releaseTexturePrefilterCache();
    ScrollOverview::releaseWallpaperStash(std::nullopt);
    // The text textures outlive a close now (X51), so unload is where they go --
    // same moment the last overview's destructor used to drop them, and hyprpm
    // reloads this plugin in-process.
    OverviewRender::clearTextCache();
    // The drag-out zone's drift timer and stripe textures: the timer would fire
    // into unloaded code, and the textures need the context made current above.
    ScrollOverview::ScratchpadExit::shutdown();
    shutdownClickGestures();
    // Get Info: its poll timer would fire into unloaded code, and its worker
    // must be joined (with any package query killed) before the code goes.
    ScrollOverview::Dock::stopInfoPoll();
    ScrollOverview::Dock::stopUrgentHopTimer();
    AppInfo::shutdown();
    disableScrollOverviewHooks();

    if (g_pTrackpadGestures)
    {
        // The swipe in progress first: clearGestures() does not touch it, and it
        // may be one of ours (session abort mid-swipe, 2026-09-29).
        ScrollOverview::endActiveTrackpadGesture();
        g_pTrackpadGestures->clearGestures();
    }

    // LAST, and it is the difference between unloading and killing the session.
    //
    // The types below are defined IN THIS SHARED OBJECT (OverviewPassElement.hpp, DockTilt.hpp).
    // A queued one that outlives the dlclose leaves Hyprland's render pass holding a
    // vtable in freed memory, and the next frame's CRenderPass::clear() runs its
    // destructor through that vtable. `hyprctl plugin unload` SEGV'd the compositor
    // twice on 2026-09-12 doing exactly this -- both cores identical, and both name
    // it outright: beginRender -> Render::CRenderPass::clear() -> a call to an
    // address in no mapped region, with hyprdrive.so already gone from the map.
    //
    // Hyprland cannot know these types, so the plugin has to take them back out
    // before its code goes away. The trackpad gestures above are the same hazard,
    // and clearGestures() is what already answers it for them.
    if (g_pHyprRenderer) {
        for (const char* TYPE : {"COverviewBlurPassElement", "COverviewSplitBorderPassElement", "COverviewShadowPassElement", "CDockLivePreviewPassElement",
                                 "CDockTiltPassElement", "CStackBlurPassElement"})
            g_pHyprRenderer->m_renderPass.removeAllOfType(TYPE);
    }

    // After the purge: nothing queued can reach the tilt shader any more, and
    // its GL program has to go while this object's code is still mapped.
    ScrollOverview::DockTilt::shutdown();
    ScrollOverview::StackBlur::shutdown();

    HyprlandAPI::reloadConfig(); // re-adds built-in gestures cleared above
}
