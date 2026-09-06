#define WLR_USE_UNSTABLE

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <optional>
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
#include <hyprland/src/devices/IKeyboard.hpp>
#include <hyprland/src/devices/IPointer.hpp>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-compose.h>
#include <unordered_set>
#include <sys/eventfd.h>
#include <hyprland/src/managers/input/trackpad/GestureTypes.hpp>
#include <hyprland/src/managers/input/trackpad/TrackpadGestures.hpp>

#include <hyprutils/string/ConstVarList.hpp>
using namespace Hyprutils::String;

#include "globals.hpp"
#include "SearchLetters.hpp"

#include "Compat.hpp"
#include "AppSearch.hpp"
#include "OmarchySearch.hpp"
#include "Config.hpp"
#include "Dock.hpp"
#include "DockStacks.hpp"
#include "FlightController.hpp"
#include "DockInput.hpp"
#include "LauncherEntry.hpp"
#include "Minimize.hpp"
#include "PluginVersion.hpp"
#include "scrollOverview.hpp"
#include "SearchSession.hpp"
#include "OverviewGesture.hpp"
#include "OverviewRender.hpp"
#include "SearchProviders.hpp"
#include "WindowPill.hpp"

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

static bool openOverview(PHLMONITOR monitor) {
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

static bool overviewAcceptsSearch() {
    return std::ranges::any_of(scrollOverviews(), [](const auto& overview) { return overview && !overview->isClosing(); });
}

// G1: the compose sequence table, built once from the locale. A guest with no
// Compose file leaves this null and every path below falls through to the plain
// utf8 one, so compose is a bonus rather than a dependency.
// ponytail: context and table are deliberately never unref'd -- they live as
// long as the plugin does, and freeing them at unload would race the key hook.
static xkb_compose_state* composeState() {
    static xkb_compose_state* STATE = []() -> xkb_compose_state* {
        const char* LOCALE = nullptr;
        for (const char* VAR : {"LC_ALL", "LC_CTYPE", "LANG"}) {
            if (const char* V = getenv(VAR); V && *V) {
                LOCALE = V;
                break;
            }
        }
        if (!LOCALE)
            LOCALE = "C";

        xkb_context* CTX = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
        if (!CTX)
            return nullptr;

        xkb_compose_table* TABLE = xkb_compose_table_new_from_locale(CTX, LOCALE, XKB_COMPOSE_COMPILE_NO_FLAGS);
        if (!TABLE)
            return nullptr;

        xkb_compose_state* S = xkb_compose_state_new(TABLE, XKB_COMPOSE_STATE_NO_FLAGS);
        xkb_compose_table_unref(TABLE);
        return S;
    }();

    return STATE;
}

// A sequence half-typed when the overview closes, or when ESC/BACKSPACE cuts it
// short, must not swallow the first key of the next one.
static void resetCompose() {
    if (auto* C = composeState(); C)
        xkb_compose_state_reset(C);
}

// The query is UTF-8 now (it has to be -- a composed 'e' is two bytes), so
// BACKSPACE removes a CODEPOINT. pop_back() would leave a severed lead byte and
// hand the providers a string that is not valid UTF-8.
static void popCodepoint(std::string& s) {
    size_t n = s.size();
    while (n > 0 && (static_cast<unsigned char>(s[n - 1]) & 0xC0) == 0x80)
        --n;
    s.resize(n > 0 ? n - 1 : 0);
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

    const auto STATE = activeXkbState();
    if (!STATE)
        return {};

    const xkb_keysym_t SYM = xkb_state_key_get_one_sym(STATE, keycode + 8);

    // G1 last clause: Compose and dead keys START a search. xkb_compose_state_feed
    // IGNOREs modifier keysyms itself, so a SHIFT held for the second character of
    // `Compose " o` does not cancel the sequence -- do not pre-filter them here and
    // do not feed a key we are not going to consume.
    if (auto* COMPOSE = composeState(); COMPOSE && SYM != XKB_KEY_NoSymbol && xkb_compose_state_feed(COMPOSE, SYM) == XKB_COMPOSE_FEED_ACCEPTED) {
        switch (xkb_compose_state_get_status(COMPOSE)) {
            case XKB_COMPOSE_COMPOSING:
                // Mid-sequence. Types nothing YET, but the bar is live from the
                // Compose press onward -- that is the whole requirement.
                return {.consume = true, .starts = true};

            case XKB_COMPOSE_COMPOSED: {
                char      buf[64] = {};
                const int N       = xkb_compose_state_get_utf8(COMPOSE, buf, sizeof(buf));
                resetCompose();
                return {.consume = true, .starts = true, .text = N > 0 ? std::string(buf, N) : std::string()};
            }

            case XKB_COMPOSE_CANCELLED:
                // An invalid sequence types nothing at all, GNOME included. The
                // keys are still ours -- they were typed AT the search bar.
                resetCompose();
                return {.consume = true, .starts = true};

            case XKB_COMPOSE_NOTHING: break; // ordinary key, fall through
        }
    }

    char      buf[8] = {};
    const int N      = xkb_state_key_get_utf8(STATE, keycode + 8, buf, sizeof(buf));
    if (N <= 0)
        return {};

    // ENTER, TAB, ESC and friends all yield exactly one control byte and type
    // nothing. Anything wider is a real character from the layout and is now
    // allowed in -- the query stopped being ASCII the moment compose landed.
    if (N == 1 && (buf[0] < 0x20 || buf[0] == 0x7F))
        return {};

    return {.consume = true, .starts = true, .text = std::string(buf, N)};
}

// The trailing work every edit to the query owes, whatever made the edit.
static void afterQueryEdit() {
    // The gate reads the query twice -- "is it empty" for the Y9 minimized rule
    // and the text itself for the match -- so every edit changes its answer.
    Membership::bump("query edit");

    // X52. The moment being complained about starts HERE, not at the frame.
    if (Prof::enabled()) {
        Prof::reset();
        Prof::keyAt   = Prof::clock::now();
        Prof::pending = true;
    }

    // X20: every keystroke starts a new provider generation. Non-blocking, so
    // this costs the keystroke nothing; results land over the next frames.
    SearchProviders::query(g_search.query.text);
    OmarchySearch::query(g_search.query.text);
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
static CHyprSignalListener g_pMonitorGoneHook;
static bool                g_tapSuperCandidate = false;
static uint32_t            g_tapSuperPressedAt = 0;

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
                if (event.keycode == EVDEV_KEY_SPACE && ScrollOverview::Stacks::previewHovered()) {
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
                if (overviewAcceptsSearch() && !isModifierKeycode(event.keycode) &&
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

        if (!ISSUPER) {
            if (PRESSED && !isModifierKeycode(event.keycode))
                g_tapSuperCandidate = false; // SUPER is being held as a modifier
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

// shared core: register / unregister the overview trackpad gesture. used by both the hyprlang
// keyword and the Lua `hyprdrive.gesture` function so both configure the same gesture system.
static std::expected<void, std::string> applyOverviewGesture(size_t fingerCount, eTrackpadGestureDirection direction, const std::string& action, uint32_t modMask,
                                                             float deltaScale, bool disableInhibit) {
    if (fingerCount <= 1 || fingerCount >= 10)
        return std::unexpected(std::format("Invalid value {} for finger count", fingerCount));

    if (direction == TRACKPAD_GESTURE_DIR_NONE)
        return std::unexpected("Invalid direction");

    if (action == "overview")
        return g_pTrackpadGestures->addGesture(makeUnique<COverviewGesture>(direction), fingerCount, direction, modMask, deltaScale, disableInhibit);

    if (action == "unset")
        return g_pTrackpadGestures->removeGesture(fingerCount, direction, modMask, deltaScale, disableInhibit);

    return std::unexpected(std::format("Invalid gesture: {}", action));
}

// Lua-facing registrar (hyprdrive.gesture). takes the direction/mods as strings and resolves
// them the same way the keyword does, then defers to applyOverviewGesture.
static SDispatchResult onRegisterOverviewGesture(size_t fingerCount, const std::string& directionStr, const std::string& action, const std::string& mods, float deltaScale,
                                                 bool disableInhibit) {
    if (g_unloading)
        return {};

    const auto direction = g_pTrackpadGestures->dirForString(directionStr);
    if (direction == TRACKPAD_GESTURE_DIR_NONE)
        return {.success = false, .error = std::format("Invalid direction: {}", directionStr)};

    uint32_t modMask = 0;
    if (!mods.empty() && g_pKeybindManager)
        modMask = g_pKeybindManager->stringToModMask(mods);

    const auto res = applyOverviewGesture(fingerCount, direction, action, modMask, std::clamp(deltaScale, 0.1F, 10.F), disableInhibit);
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

    const auto direction = g_pTrackpadGestures->dirForString(data[1]);

    if (direction == TRACKPAD_GESTURE_DIR_NONE) {
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

    const auto resultFromGesture = applyOverviewGesture(fingerCount, direction, std::string{data[startDataIdx]}, modMask, deltaScale, disableInhibit);

    if (!resultFromGesture)
        result.setError(resultFromGesture.error().c_str());

    return result;
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

    if (VERB == "view" || VERB == "sort") {
        const auto CUT2 = rest.find(' ');
        if (CUT2 == std::string::npos)
            return {.success = false, .error = "expected a mode and a path"};

        const auto MODE = rest.substr(0, CUT2);
        auto       path = rest.substr(CUT2 + 1);
        while (!path.empty() && path.front() == ' ')
            path.erase(path.begin());

        if (VERB == "view") {
            if (MODE != "fan" && MODE != "grid")
                return {.success = false, .error = "expected fan or grid"};
            ScrollOverview::Stacks::setView(path, MODE == "grid");
            return {};
        }

        if (MODE != "name" && MODE != "mtime")
            return {.success = false, .error = "expected name or mtime"};
        ScrollOverview::Stacks::setSort(path, MODE == "mtime");
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

    g_pRenderPreHook = Event::bus()->m_events.render.pre.listen([](PHLMONITOR monitor) {
        if (const auto overview = scrollOverviewForMonitor(monitor))
            overview->onPreRender();

        ScrollOverview::Dock::damageIfChanged(monitor);
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

    g_pDockConfigHook = Event::bus()->m_events.config.reloaded.listen([] {
        // FIRST, before anything in this plugin reads a config value this pass:
        // a value the user set under the old plugin:scrolloverview: name is
        // copied onto its plugin:hyprdrive: twin here and nowhere else, so the
        // rest of the codebase only ever knows the new name. ONE RELEASE.
        ScrollOverview::Compat::adoptLegacyValues();

        // A reload rebuilds the dock's whole layout under whatever is flying,
        // and a flight's boxes are the OLD layout's -- its icon target can move,
        // shrink, or stop existing. Land them first, with the landing every
        // other end of a flight gets, rather than let one chase an icon that is
        // no longer there. Cheap: with nothing in the air it returns at once.
        ScrollOverview::Flight::abortAll("config-reload");

        for (const auto& MONITOR : State::monitorState()->monitors())
            ScrollOverview::Dock::damageIfChanged(MONITOR);
    });

    g_pDockHoverHook = Event::bus()->m_events.input.mouse.move.listen([](Vector2D, Event::SCallbackInfo&) {
        const auto HIT = monitorUnderCursor();
        if (!HIT) {
            ScrollOverview::Dock::releasePointerFocus();
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
    });

    // X24 dock-side. The overview installs its own button hook; this one is the
    // plain desktop, where nothing else claims a click on the pill. Both edges
    // are swallowed over the pill: the dock is not a window, so a press that
    // reached the window underneath would focus it, and a release that leaked
    // after a swallowed press would leave the pointer stuck.
    g_pDockClickHook = Event::bus()->m_events.input.mouse.button.listen([](IPointer::SButtonEvent event, Event::SCallbackInfo& info) {
        const auto HIT = monitorUnderCursor();
        if (!HIT)
            return;
        const auto& [MONITOR, LOCAL] = *HIT;

        // The plain desktop's button map is fixed: right opens the menu, middle
        // launches, left drives the pill. (The overview honours the left-handed
        // config; the desktop dock never has.) The ladder and the reasons for its
        // order live in DockInput -- this is the same dock the overview drives.
        const auto ROLE = event.button == BTN_RIGHT  ? ScrollOverview::DockInput::EButton::Menu
                        : event.button == BTN_MIDDLE ? ScrollOverview::DockInput::EButton::Launch
                        : event.button == BTN_LEFT   ? ScrollOverview::DockInput::EButton::Main
                        : ScrollOverview::DockInput::EButton::Other;
        const bool PRESSED = event.state == WL_POINTER_BUTTON_STATE_PRESSED;

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
    g_pMonitorGoneHook = Event::bus()->m_events.monitor.removed.listen([](PHLMONITOR monitor) {
        ScrollOverview::Flight::abortAll("monitor-removed");
        dropScrollOverviewsForMonitor(monitor);
        if (monitor)
            ScrollOverview::Dock::forgetMonitor(monitor->m_id);
    });

    ScrollOverview::Config::registerDispatcher("overview", ::onOverviewDispatcher);
    ScrollOverview::Config::registerDispatcher("navigate", ::onNavigateDispatcher);
    ScrollOverview::Config::registerDispatcher("window", ::onWindowDispatcher);
    ScrollOverview::Config::registerDispatcher("minimize", ::onMinimizeDispatcher);
    ScrollOverview::Config::registerDispatcher("hotkey", ::onHotkeyDispatcher);
    ScrollOverview::Config::registerDispatcher("stack", ::onStackDispatcher);
    ScrollOverview::Config::registerGesture(::onRegisterOverviewGesture, ::overviewGestureKeyword);
    ScrollOverview::Config::registerConfig();
    registerRawKeyHooks();

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
        }
    }

    // X51/X52. Build the icon index at LOAD, where a one-off ~10ms pass is
    // invisible, rather than letting the first search pay for it inside a frame
    // that is already trying to paint results.
    // Y13. ASYNC. Building this inline cost 4237ms of PARSE TIME, which is
    // time the compositor spends before exec-once and therefore before the
    // wallpaper can even be asked for.
    AppSearch::warmIconIndexAsync();

    return {ScrollOverview::Compat::NAME, "Mission-control overview + dock for Hyprland", "Vaxry, yayuuu, dragos", SCROLLOVERVIEW_VERSION};
}

APICALL EXPORT void PLUGIN_EXIT() {
    stopSearchRepeat();
    if (g_searchRepeatTimer) {
        wl_event_source_remove(g_searchRepeatTimer);
        g_searchRepeatTimer = nullptr;
    }
    OmarchySearch::setResultsCallback(nullptr);
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
    g_pDockScrollHook.reset();
    g_pDockRenderHook.reset();
    g_pMonitorGoneHook.reset();

    g_unloading = true;
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
    // The text textures outlive a close now (X51), so unload is where they go --
    // same moment the last overview's destructor used to drop them, and hyprpm
    // reloads this plugin in-process.
    OverviewRender::clearTextCache();
    disableScrollOverviewHooks();

    if (g_pTrackpadGestures)
        g_pTrackpadGestures->clearGestures();

    HyprlandAPI::reloadConfig(); // re-adds built-in gestures cleared above
}
