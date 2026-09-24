#include "OverviewGesture.hpp"

#include "Config.hpp"
#include "IOverview.hpp"
#include "scrollOverview.hpp"
#include "OmarchySearch.hpp"
#include "SearchProviders.hpp"
#include "Scratchpad.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <optional>
#include <ranges>
#include <string_view>
#include <vector>
#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon.h>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/devices/IKeyboard.hpp>
#include <hyprland/src/managers/KeybindManager.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/config/shared/actions/ConfigActions.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>

COverviewGesture::COverviewGesture(eTrackpadGestureDirection direction) : m_direction(direction) {}

// THE SWIPE OPENS AND CLOSES EVERY MONITOR, like tap-SUPER (user, 2026-09-14:
// the gesture only opened the monitor under the pointer). Each monitor's
// overview gets the same delta, so every `scale` holds the same value and each
// onSwipeEnd reaches the same commit-or-cancel decision: they finish together
// without having to be told to.
void COverviewGesture::begin(const ITrackpadGesture::STrackpadGestureBegin& e) {
    ITrackpadGesture::begin(e);

    m_lastDelta   = 0.F;
    m_firstUpdate = true;
    m_overviews.clear();

    const auto FOCUSED = Desktop::focusState()->monitor();
    if (!FOCUSED)
        return;

    const bool WANT_CLOSE = m_direction == TRACKPAD_GESTURE_DIR_DOWN;

    // An overview still animating out cannot be swiped (onSwipeUpdate reads
    // `closing` and would run backwards). Drop them first, on every monitor --
    // and before ensureScrollOverviewHooks, because dropping the last one tears
    // the hooks down.
    for (const auto& MONITOR : State::monitorState()->monitors()) {
        if (const auto OVERVIEW = scrollOverviewForMonitor(MONITOR); OVERVIEW && OVERVIEW->isClosing())
            dropScrollOverviewsForMonitor(MONITOR);
    }

    if (!WANT_CLOSE) {
        // Already open where the fingers are: swipe-up does not close.
        if (scrollOverviewForMonitor(FOCUSED))
            return;
        if (!ensureScrollOverviewHooks())
            return;

        // Super-tap and the dock button reset the search session; swipe-open
        // was the remaining door, so a query survived close and came back.
        resetSearchSession();
        SearchProviders::warm();
        OmarchySearch::init();

        for (const auto& MONITOR : State::monitorState()->monitors()) {
            if (!MONITOR || !MONITOR->m_enabled || !MONITOR->m_activeWorkspace || scrollOverviewForMonitor(MONITOR))
                continue;

            const auto OVERVIEW = makeShared<CScrollOverview>(MONITOR->m_activeWorkspace, true, MONITOR);
            registerScrollOverview(OVERVIEW);
            OVERVIEW->m_swipeClosing = false;
            OVERVIEW->m_isSwiping    = true;
            m_overviews.emplace_back(OVERVIEW);
        }
        return;
    }

    // A copy: nothing here should mutate the registry, but close paths do, and
    // iterating the live vector through one would be a use-after-free.
    const auto OPEN = scrollOverviews();
    for (const auto& OVERVIEW : OPEN) {
        if (!OVERVIEW || OVERVIEW->isClosing())
            continue;

        // Only the pointer's monitor picks the desktop under it. On the others
        // the pointer is off-screen, and a side card reaching past the edge
        // could still be "hovered" and switch that monitor's desktop.
        if (OVERVIEW->pMonitor.lock() == FOCUSED)
            OVERVIEW->selectHoveredWorkspace();
        OVERVIEW->m_swipeClosing = true;
        OVERVIEW->m_isSwiping    = true;
        m_overviews.emplace_back(OVERVIEW);
    }
}

void COverviewGesture::update(const ITrackpadGesture::STrackpadGestureUpdate& e) {
    if (m_overviews.empty())
        return;

    if (m_firstUpdate) {
        m_firstUpdate = false;
        return;
    }

    // Signed follow: reversing the fingers undoes the motion. Clamp to the
    // gesture length so extra travel does not wind up a reverse.
    m_lastDelta += distance(e);
    const float MAX = sc<float>(ScrollOverview::Config::getGestureDistance());
    m_lastDelta     = std::clamp(m_lastDelta, 0.F, MAX);

    for (const auto& REF : m_overviews) {
        if (const auto OVERVIEW = REF.lock())
            OVERVIEW->onSwipeUpdate(m_lastDelta);
    }
}

void COverviewGesture::end(const ITrackpadGesture::STrackpadGestureEnd& e) {
    // Locked up front: onSwipeEnd may close, and closing unregisters.
    std::vector<SP<IOverview>> driven;
    for (const auto& REF : m_overviews) {
        if (const auto OVERVIEW = REF.lock())
            driven.emplace_back(OVERVIEW);
    }
    m_overviews.clear();

    for (const auto& OVERVIEW : driven)
        OVERVIEW->onSwipeEnd();
}

void COverviewWorkspaceGesture::begin(const ITrackpadGesture::STrackpadGestureBegin& e) {
    ITrackpadGesture::begin(e);
    m_inOverview = false;
    m_overview.reset();

    const auto MONITOR  = Desktop::focusState()->monitor();
    const auto OVERVIEW = openScrollOverviewForMonitor(MONITOR);
    if (OVERVIEW) {
        m_inOverview = true;
        m_overview   = OVERVIEW;
        OVERVIEW->onWorkspaceSwipeBegin();
        return;
    }

    if (m_allWorkspaces && !m_forcedUseR) {
        m_prevUseR   = ScrollOverview::Config::getValue<int>("gestures:workspace_swipe_use_r");
        m_forcedUseR = true;
        ScrollOverview::Config::setValue("gestures:workspace_swipe_use_r", 1);
    }

    m_native.begin(e);
}

void COverviewWorkspaceGesture::update(const ITrackpadGesture::STrackpadGestureUpdate& e) {
    if (m_inOverview) {
        const auto OVERVIEW = m_overview.lock();
        if (!OVERVIEW)
            return;
        // Negate so the cards stick to the fingers: swipe right, content moves right.
        OVERVIEW->onWorkspaceSwipeUpdate(-distance(e));
        return;
    }

    m_native.update(e);
}

void COverviewWorkspaceGesture::end(const ITrackpadGesture::STrackpadGestureEnd& e) {
    if (m_inOverview) {
        if (const auto OVERVIEW = m_overview.lock())
            OVERVIEW->onWorkspaceSwipeEnd();
        m_overview.reset();
        m_inOverview = false;
        return;
    }

    m_native.end(e);

    // AFTER the native end: the release is where it picks the workspace to land
    // on, and it reads use_r to do it.
    if (m_forcedUseR) {
        ScrollOverview::Config::setValue("gestures:workspace_swipe_use_r", m_prevUseR);
        m_forcedUseR = false;
    }
}

void COverviewScrollGesture::begin(const ITrackpadGesture::STrackpadGestureBegin& e) {
    ITrackpadGesture::begin(e);
    m_inOverview = false;
    m_overview.reset();

    if (const auto OVERVIEW = openScrollOverviewForMonitor(Desktop::focusState()->monitor())) {
        m_inOverview = true;
        m_overview   = OVERVIEW;
        return;
    }

    m_native.begin(e);
}

void COverviewScrollGesture::update(const ITrackpadGesture::STrackpadGestureUpdate& e) {
    if (m_inOverview) {
        const auto OVERVIEW = m_overview.lock();
        if (auto* const SCROLL = OVERVIEW ? dynamic_cast<CScrollOverview*>(OVERVIEW.get()) : nullptr)
            // Negated like the workspace swipe: the columns stick to the fingers.
            SCROLL->onTapeSwipeUpdate(-distance(e));
        return;
    }

    m_native.update(e);
}

void COverviewScrollGesture::end(const ITrackpadGesture::STrackpadGestureEnd& e) {
    if (m_inOverview) {
        if (const auto OVERVIEW = m_overview.lock()) {
            if (auto* SCROLL = dynamic_cast<CScrollOverview*>(OVERVIEW.get()))
                SCROLL->onTapeSwipeEnd();
        }
        m_overview.reset();
        m_inOverview = false;
        return;
    }

    m_native.end(e);
}

bool COverviewWorkspaceGesture::isDirectionSensitive() {
    return true;
}

namespace {

struct SClickGesture {
    size_t      fingers         = 3;
    std::string action;
    uint32_t    mods            = 0;
    bool        disableInhibit  = false;
    std::string arg;
};

std::vector<SClickGesture>       g_clickGestures;
std::vector<CHyprSignalListener> g_padListeners;
size_t                           g_attachedPointerCount = 0;
uint32_t                         g_padFingers           = 0;
uint32_t                         g_lastPadFingers       = 0;
uint32_t                         g_eatButton             = 0;
bool                             g_padButtonFromTouchpad = false;
bool                             g_sawSwipeOrPinch       = false;
Time::steady_tp                  g_lastPadFingersAt{};
Time::steady_tp                  g_holdBeganAt{};

// OFF unless HYPRDRIVE_CLICK_LOG is set to something other than 0. Every line
// is an open/write/close of a file on /tmp, and the loudest callers are on the
// reload and button paths: a config reload alone wrote one line per click
// gesture re-added and one per pointer device, for a trace nobody reads outside
// a debugging session. Same shape as FlightController's zdbg, but an env var
// rather than a key -- the first lines are written before a config exists.
void clickLog(const char* fmt, ...) {
    static const bool ENABLED = [] {
        const char* const V = std::getenv("HYPRDRIVE_CLICK_LOG");
        return V && *V && std::string_view{V} != "0";
    }();

    if (!ENABLED)
        return;

    if (FILE* F = fopen("/tmp/hyprdrive-click.log", "a")) {
        va_list ap;
        va_start(ap, fmt);
        vfprintf(F, fmt, ap);
        va_end(ap);
        fclose(F);
    }
}

void notePadFingers(uint32_t n) {
    g_padFingers = n;
    if (n > 0) {
        g_lastPadFingers   = n;
        g_lastPadFingersAt = Time::steadyNow();
    }
}

// Hold/swipe end. A physical click cancels the hold, then the button event
// arrives — keep the last count but restart the clock from the cancel, not
// from when the fingers first landed (rest-then-click is seconds, not 160ms).
void notePadFingersEnd(bool keepLast) {
    g_padFingers = 0;
    if (keepLast && g_lastPadFingers > 0)
        g_lastPadFingersAt = Time::steadyNow();
    else
        g_lastPadFingers = 0;
}

uint32_t resolvedPadFingers() {
    if (g_padFingers >= 2)
        return g_padFingers;
    if (g_lastPadFingers >= 2) {
        const float ago = sc<float>(std::chrono::duration<float, std::milli>(Time::steadyNow() - g_lastPadFingersAt).count());
        if (ago < 220.F)
            return g_lastPadFingers;
    }
    return 0;
}

void dispatchNamed(const std::string& name, const std::string& arg) {
    if (!g_pKeybindManager)
        return;
    const auto IT = g_pKeybindManager->m_dispatchers.find(name);
    if (IT != g_pKeybindManager->m_dispatchers.end())
        IT->second(arg);
}

std::string trimCopy(std::string s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.erase(s.begin());
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
        s.pop_back();
    return s;
}

std::string aliasKey(std::string key) {
    std::string lower = key;
    std::ranges::transform(lower, lower.begin(), ::tolower);
    if (lower == "printscreen" || lower == "print_screen" || lower == "prtsc" || lower == "prntscrn" || lower == "print")
        return "Print";
    if (lower == "enter" || lower == "return")
        return "Return";
    if (lower == "esc" || lower == "escape")
        return "Escape";
    if (lower == "space" || lower == "spacebar")
        return "space";
    if (lower == "tab")
        return "TAB";
    return key;
}

std::vector<std::string> splitChord(const std::string& spec) {
    std::string s = spec;
    for (char& c : s) {
        if (c == '+' || c == ',')
            c = ' ';
    }
    std::vector<std::string> parts;
    std::string              cur;
    for (char c : s) {
        if (c == ' ' || c == '\t') {
            if (!cur.empty()) {
                parts.push_back(trimCopy(cur));
                cur.clear();
            }
        } else
            cur.push_back(c);
    }
    if (!cur.empty())
        parts.push_back(trimCopy(cur));
    return parts;
}

uint32_t resolveXkbKeycode(const std::string& key) {
    if (!g_pSeatManager)
        return 0;
    const auto KB = g_pSeatManager->m_keyboard.lock();
    if (!KB || !KB->m_xkbKeymap || !KB->m_xkbState)
        return 0;

    const xkb_keysym_t KEYSYM = xkb_keysym_from_name(key.c_str(), XKB_KEYSYM_CASE_INSENSITIVE);
    if (KEYSYM == XKB_KEY_NoSymbol)
        return 0;

    const xkb_keycode_t MIN = xkb_keymap_min_keycode(KB->m_xkbKeymap);
    const xkb_keycode_t MAX = xkb_keymap_max_keycode(KB->m_xkbKeymap);
    for (xkb_keycode_t kc = MIN; kc <= MAX; ++kc) {
        if (xkb_state_key_get_one_sym(KB->m_xkbState, kc) == KEYSYM)
            return kc;
    }
    return 0;
}

// Run compositor binds that match this chord (Print → screenshot __lua, etc).
bool fireMatchingKeybind(uint32_t modmask, const std::string& key) {
    if (!g_pKeybindManager)
        return false;
    const xkb_keysym_t WANT = xkb_keysym_from_name(key.c_str(), XKB_KEYSYM_CASE_INSENSITIVE);
    if (WANT == XKB_KEY_NoSymbol)
        return false;

    bool fired = false;
    for (const auto& k : g_pKeybindManager->m_keybinds) {
        if (!k || !k->enabled || k->mouse || k->release)
            continue;
        if (!k->submap.name.empty())
            continue;
        if (k->modmask != modmask)
            continue;

        xkb_keysym_t bindSym = XKB_KEY_NoSymbol;
        if (!k->key.empty())
            bindSym = xkb_keysym_from_name(k->key.c_str(), XKB_KEYSYM_CASE_INSENSITIVE);
        if (bindSym != WANT)
            continue;

        const auto IT = g_pKeybindManager->m_dispatchers.find(k->handler);
        if (IT == g_pKeybindManager->m_dispatchers.end())
            continue;
        clickLog("bind %s mods=%u handler=%s arg=%s\n", key.c_str(), modmask, k->handler.c_str(), k->arg.c_str());
        IT->second(k->arg);
        fired = true;
        if (!k->nonConsuming)
            break;
    }
    return fired;
}

void fireClickAction(const SClickGesture& g) {
    const auto& A = g.action;
    if (A == "overview") {
        dispatchNamed("hyprdrive:overview", "toggle all"); // every monitor, like tap-SUPER
        return;
    }
    if (A == "workspace") {
        dispatchNamed("workspace", "e+1");
        return;
    }
    if (A == "close") {
        dispatchNamed("killactive", "");
        return;
    }
    if (A == "fullscreen") {
        dispatchNamed("fullscreen", g.arg.empty() ? "0" : (g.arg == "maximize" ? "1" : "0"));
        return;
    }
    if (A == "maximize") {
        dispatchNamed("fullscreen", "1");
        return;
    }
    if (A == "float" || A == "tile") {
        dispatchNamed("togglefloating", "");
        return;
    }
    if (A == "special") {
        dispatchNamed("togglespecialworkspace", g.arg.empty() ? "special" : g.arg);
        return;
    }
    if (A == "dispatcher" || A == "dispatch") {
        std::string name = g.arg;
        std::string data;
        const auto  SEP = g.arg.find_first_of(", ");
        if (SEP != std::string::npos) {
            name = g.arg.substr(0, SEP);
            data = g.arg.substr(SEP + 1);
            while (!data.empty() && (data.front() == ' ' || data.front() == ','))
                data.erase(data.begin());
        }
        if (!name.empty())
            dispatchNamed(name, data);
        return;
    }
    if (A == "zoom")
        dispatchNamed("cursorzoom", g.arg.empty() ? "2" : g.arg);
    if (A == "keys" || A == "key" || A == "shortcut" || A == "chord")
        sendGestureChord(g.arg);
}

bool tryFireClick(uint32_t fingers, uint32_t eatButton) {
    if (fingers < 2 || g_clickGestures.empty())
        return false;
    const uint32_t mods = g_pInputManager ? g_pInputManager->getModsFromAllKBs() : 0;
    for (const auto& g : g_clickGestures) {
        if (g.fingers != fingers)
            continue;
        if (g.mods != 0 && (mods & g.mods) != g.mods)
            continue;
        clickLog("match %zuf %s arg=%s\n", g.fingers, g.action.c_str(), g.arg.c_str());
        fireClickAction(g);
        g_eatButton      = eatButton;
        g_padFingers     = 0;
        g_lastPadFingers = 0;
        return true;
    }
    return false;
}

// Clickfinger only maps 1/2/3-finger clicks to left/right/middle. A 4-finger
// physical click never becomes a button; libinput reports a short hold instead.
//
// A CANCELLED hold is also how every 4-finger swipe and pinch STARTS: libinput
// ends the hold the moment the fingers move and only then begins the swipe or
// pinch, so at this point g_sawSwipeOrPinch is still false. Firing here closed
// the window at the start of a 4-finger pinch, fingers still down (user,
// 2026-09-24: "window closing before fingers lifted from trackpad"). A cancelled
// hold therefore waits HOLD_CANCEL_GRACE_MS and fires only if no swipe or pinch
// began in the meantime -- they begin in the same input frame as the cancel.
constexpr int    HOLD_CANCEL_GRACE_MS = 80;
wl_event_source* g_holdClickTimer     = nullptr;
uint32_t         g_holdClickFingers   = 0;

int holdClickTimerCb(void*) {
    clickLog("holdClick deferred: swipe=%d\n", g_sawSwipeOrPinch ? 1 : 0);
    if (!g_sawSwipeOrPinch)
        tryFireClick(g_holdClickFingers, 0);
    return 0;
}

void maybeClickFromHold(bool cancelled) {
    const uint32_t fingers = g_padFingers >= 2 ? g_padFingers : g_lastPadFingers;
    const float    dur     = sc<float>(std::chrono::duration<float, std::milli>(Time::steadyNow() - g_holdBeganAt).count());
    clickLog("holdClick? fingers=%u dur=%.0f swipe=%d cancelled=%d\n", fingers, dur, g_sawSwipeOrPinch ? 1 : 0, cancelled ? 1 : 0);
    if (g_sawSwipeOrPinch || fingers < 4)
        return;
    if (dur < 0.F || dur > 800.F)
        return;

    if (!cancelled) {
        tryFireClick(fingers, 0);
        return;
    }

    if (!g_holdClickTimer && g_pCompositor)
        g_holdClickTimer = wl_event_loop_add_timer(g_pCompositor->m_wlEventLoop, holdClickTimerCb, nullptr);
    if (!g_holdClickTimer)
        return; // no way to wait: a missed click beats a closed window
    g_holdClickFingers = fingers;
    wl_event_source_timer_update(g_holdClickTimer, HOLD_CANCEL_GRACE_MS);
}

} // namespace

void sendGestureChord(const std::string& chord) {
    auto parts = splitChord(chord);
    if (parts.empty())
        return;

    std::string key = aliasKey(parts.back());
    parts.pop_back();

    uint32_t modmask = 0;
    if (g_pKeybindManager) {
        for (auto& p : parts)
            modmask |= g_pKeybindManager->stringToModMask(p);
    }

    clickLog("chord '%s' key=%s mods=%u\n", chord.c_str(), key.c_str(), modmask);

    // Print (and any other chord that is a compositor bind) must run the bind,
    // not get passed into the focused app.
    if (fireMatchingKeybind(modmask, key))
        return;

    const uint32_t keycode = resolveXkbKeycode(key);
    if (!keycode) {
        clickLog("chord: no keycode for %s\n", key.c_str());
        return;
    }
    clickLog("pass mods=%u keycode=%u\n", modmask, keycode);
    const auto passResult = Config::Actions::pass(modmask, keycode, std::nullopt);
    if (!passResult)
        clickLog("pass failed: %s\n", passResult.error().message.c_str());
}

void CKeysTrackpadGesture::end(const ITrackpadGesture::STrackpadGestureEnd&) {
    sendGestureChord(m_chord);
}

void clearClickGestures() {
    g_clickGestures.clear();
}

void shutdownClickGestures() {
    if (g_holdClickTimer) {
        wl_event_source_remove(g_holdClickTimer);
        g_holdClickTimer = nullptr;
    }
}

void addClickGesture(size_t fingers, const std::string& action, uint32_t mods, bool disableInhibit, const std::string& arg) {
    removeClickGesture(fingers, mods);
    g_clickGestures.push_back(SClickGesture{fingers, action, mods, disableInhibit, arg});
    clickLog("addClick %zu %s arg=%s mods=%u\n", fingers, action.c_str(), arg.c_str(), mods);
}

void removeClickGesture(size_t fingers, uint32_t mods) {
    std::erase_if(g_clickGestures, [&](const SClickGesture& g) { return g.fingers == fingers && g.mods == mods; });
}

// Idempotent, and deliberately so: this is called from the config.reloaded
// listener, and a reload is not a reason to rebuild anything here. Tearing the
// listeners down and putting them back cost ~10 signal registrations per
// pointer device AND zeroed g_padFingers, which threw away a 4-finger hold that
// was still under the hand -- a theme switch mid-gesture cancelled the gesture.
// The listeners are attached to the POINTER SET, so that is the only thing that
// can make them stale; handleClickGestureButton watches the same count.
void attachPadFingerListeners() {
    if (!g_pInputManager) {
        g_padListeners.clear();
        g_attachedPointerCount = 0;
        return;
    }

    if (!g_padListeners.empty() && g_pInputManager->m_pointers.size() == g_attachedPointerCount)
        return;

    g_padListeners.clear();
    g_attachedPointerCount = g_pInputManager->m_pointers.size();
    for (auto& ptr : g_pInputManager->m_pointers) {
        if (!ptr)
            continue;
        g_padListeners.emplace_back(ptr->m_pointerEvents.holdBegin.listen([](IPointer::SHoldBeginEvent e) {
            clickLog("holdBegin fingers=%u\n", e.fingers);
            g_sawSwipeOrPinch = false;
            g_holdBeganAt     = Time::steadyNow();
            notePadFingers(e.fingers);
        }));
        g_padListeners.emplace_back(ptr->m_pointerEvents.holdEnd.listen([](IPointer::SHoldEndEvent e) {
            clickLog("holdEnd cancelled=%d last=%u\n", e.cancelled ? 1 : 0, g_lastPadFingers);
            maybeClickFromHold(e.cancelled);
            notePadFingersEnd(e.cancelled);
        }));
        g_padListeners.emplace_back(ptr->m_pointerEvents.swipeBegin.listen([](IPointer::SSwipeBeginEvent e) {
            g_sawSwipeOrPinch = true;
            notePadFingers(e.fingers);
        }));
        g_padListeners.emplace_back(ptr->m_pointerEvents.swipeUpdate.listen([](IPointer::SSwipeUpdateEvent e) { notePadFingers(e.fingers); }));
        g_padListeners.emplace_back(ptr->m_pointerEvents.swipeEnd.listen([](IPointer::SSwipeEndEvent e) { notePadFingersEnd(e.cancelled); }));
        g_padListeners.emplace_back(ptr->m_pointerEvents.pinchBegin.listen([](IPointer::SPinchBeginEvent e) {
            g_sawSwipeOrPinch = true;
            notePadFingers(e.fingers);
        }));
        g_padListeners.emplace_back(ptr->m_pointerEvents.pinchUpdate.listen([](IPointer::SPinchUpdateEvent e) { notePadFingers(e.fingers); }));
        g_padListeners.emplace_back(ptr->m_pointerEvents.pinchEnd.listen([](IPointer::SPinchEndEvent e) { notePadFingersEnd(e.cancelled); }));
        // Clickfinger (Omarchy default, and what user.trackpad-gestures used) maps
        // 1/2/3-finger physical clicks to left/right/middle. The old plugin bound
        // o.bind("mouse:274", screenshot). Hyprland sets event.mouse=true on pads,
        // so we have to recognize BTN_MIDDLE on the touchpad device itself.
        const bool isPad = ptr->m_isTouchpad;
        g_padListeners.emplace_back(ptr->m_pointerEvents.button.listen([isPad](IPointer::SButtonEvent e) {
            if (e.state != WL_POINTER_BUTTON_STATE_PRESSED)
                return;
            g_padButtonFromTouchpad = isPad;
            if (!isPad || e.button != BTN_MIDDLE)
                return;
            if (resolvedPadFingers() >= 4)
                return;
            notePadFingers(3);
        }));
    }
    clickLog("attachPadFingerListeners pointers=%zu listeners=%zu\n", g_attachedPointerCount, g_padListeners.size());
}

bool handleClickGestureButton(const IPointer::SButtonEvent& event) {
    if (g_pInputManager && g_pInputManager->m_pointers.size() != g_attachedPointerCount)
        attachPadFingerListeners();

    if (event.state != WL_POINTER_BUTTON_STATE_PRESSED) {
        if (g_eatButton && event.button == g_eatButton) {
            g_eatButton = 0;
            return true;
        }
        return false;
    }
    if (g_clickGestures.empty())
        return false;

    uint32_t fingers = resolvedPadFingers();
    // Same mapping as user.trackpad-gestures: clickfinger 3-finger click is
    // BTN_MIDDLE (mouse:274). Keep a 4-finger hold if we already saw one.
    if (g_padButtonFromTouchpad && event.button == BTN_MIDDLE && fingers < 3)
        fingers = 3;

    clickLog("button btn=%u mouse=%d pad=%d fingers=%u current=%u last=%u gestures=%zu\n", event.button, event.mouse ? 1 : 0, g_padButtonFromTouchpad ? 1 : 0, fingers,
             g_padFingers, g_lastPadFingers, g_clickGestures.size());

    return tryFireClick(fingers, event.button);
}

void CHyprdriveForwardGesture::begin(const ITrackpadGesture::STrackpadGestureBegin& e) {
    ITrackpadGesture::begin(e);
    m_active = nullptr;
    if (scrollOverviewForMonitor(Desktop::focusState()->monitor()))
        return;
    m_active = pick();
    if (m_active)
        m_active->begin(e);
}

void CHyprdriveForwardGesture::update(const ITrackpadGesture::STrackpadGestureUpdate& e) {
    if (m_active)
        m_active->update(e);
}

void CHyprdriveForwardGesture::end(const ITrackpadGesture::STrackpadGestureEnd& e) {
    if (m_active)
        m_active->end(e);
    m_active = nullptr;
}

ITrackpadGesture* CScratchpadGesture::pick() {
    const auto MONITOR = Desktop::focusState()->monitor();
    if (!MONITOR)
        return nullptr;

    const auto SHOWN  = ScrollOverview::Scratchpad::shownOn(MONITOR);
    const auto TARGET = m_show ? (SHOWN ? PHLWORKSPACE{} : ScrollOverview::Scratchpad::ensureDefault(MONITOR)) : SHOWN;
    if (!TARGET)
        return nullptr;

    // The native gesture looks the workspace up as "special:" + name.
    std::string name = TARGET->m_name;
    if (name.starts_with("special:"))
        name = name.substr(8);
    m_native = std::make_unique<CSpecialWorkspaceGesture>(name);
    return m_native.get();
}

ITrackpadGesture* CFullscreenOrCloseGesture::pick() {
    const auto WINDOW = Desktop::focusState()->window();
    if (!WINDOW)
        return nullptr;

    const bool FULL = Fullscreen::controller()->isFullscreen(WINDOW);
    if (m_enter)
        return FULL ? nullptr : &m_fullscreen;
    return FULL ? sc<ITrackpadGesture*>(&m_fullscreen) : sc<ITrackpadGesture*>(&m_close);
}

// The native close gesture's travel is 200 of its units; this is how many times
// further the fingers go, and what fraction of THAT they must still be past on
// release for the window to close.
static constexpr float CLOSE_NATIVE_TRAVEL = 200.F;
static constexpr float CLOSE_TRAVEL        = 1.75F; // user, 2026-09-24: 2.5 was too hard; halfway to native 1.0
static constexpr float CLOSE_COMMIT        = 0.5F;  // halfway between native 0.2 and 0.8

void CGuardedCloseGesture::begin(const ITrackpadGesture::STrackpadGestureBegin& e) {
    auto scaled  = e;
    scaled.scale = e.scale / CLOSE_TRAVEL;
    m_userScale  = e.scale;
    ITrackpadGesture::begin(scaled); // this object tracks the same total the native one does
    m_native.begin(scaled);
    m_delta = 0.F;
    m_dir   = e.direction;
    m_lastPinch.reset();
    m_lastSwipe.reset();
}

void CGuardedCloseGesture::update(const ITrackpadGesture::STrackpadGestureUpdate& e) {
    auto scaled  = e;
    scaled.scale = m_userScale / CLOSE_TRAVEL;
    m_delta += distance(scaled);
    m_dir = e.direction;
    if (e.pinch)
        m_lastPinch = *e.pinch;
    if (e.swipe)
        m_lastSwipe = *e.swipe;
    m_native.update(scaled);
}

void CGuardedCloseGesture::end(const ITrackpadGesture::STrackpadGestureEnd& e) {
    const bool CANCELLED = (e.pinch && e.pinch->cancelled) || (e.swipe && e.swipe->cancelled);
    const bool COMMIT    = !CANCELLED && m_delta >= CLOSE_NATIVE_TRAVEL * CLOSE_COMMIT;

    if (!COMMIT && m_delta != 0.F) {
        // One update that takes the native total back to exactly zero, built
        // from the same formula ITrackpadGesture::distance applies to it.
        const float MS = m_userScale / CLOSE_TRAVEL; // the native gesture's m_scale
        ITrackpadGesture::STrackpadGestureUpdate rewind{.direction = m_dir, .scale = MS};
        IPointer::SPinchUpdateEvent              pinch;
        IPointer::SSwipeUpdateEvent              swipe;
        bool                                     built = false;

        if (m_lastPinch && (m_dir == TRACKPAD_GESTURE_DIR_PINCH || m_dir == TRACKPAD_GESTURE_DIR_PINCH_IN || m_dir == TRACKPAD_GESTURE_DIR_PINCH_OUT)) {
            // distance = MS * 400 * (PINCH_IN ? -Δ : 1.6Δ), Δ = lastScale - newScale
            const float PER = MS * 400.F * (m_dir == TRACKPAD_GESTURE_DIR_PINCH_IN ? -1.F : 1.6F);
            pinch           = *m_lastPinch;
            pinch.delta     = {};
            pinch.scale     = m_lastPinch->scale + sc<double>(m_delta / PER);
            rewind.pinch    = &pinch;
            built           = true;
        } else if (m_lastSwipe) {
            swipe       = *m_lastSwipe;
            swipe.delta = {};
            switch (m_dir) {
                case TRACKPAD_GESTURE_DIR_UP: swipe.delta.y = m_delta / MS; built = true; break;
                case TRACKPAD_GESTURE_DIR_DOWN:
                case TRACKPAD_GESTURE_DIR_VERTICAL: swipe.delta.y = -m_delta / MS; built = true; break;
                case TRACKPAD_GESTURE_DIR_LEFT: swipe.delta.x = m_delta / MS; built = true; break;
                case TRACKPAD_GESTURE_DIR_RIGHT:
                case TRACKPAD_GESTURE_DIR_HORIZONTAL: swipe.delta.x = -m_delta / MS; built = true; break;
                default: break; // a SWIPE's distance is a length and cannot go back
            }
            rewind.swipe = &swipe;
        }

        if (built)
            m_native.update(rewind);
    }

    m_native.end(e);
}
