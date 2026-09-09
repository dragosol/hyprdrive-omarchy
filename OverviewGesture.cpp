#include "OverviewGesture.hpp"

#include "Config.hpp"
#include "IOverview.hpp"
#include "scrollOverview.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdarg>
#include <optional>
#include <ranges>
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

COverviewGesture::COverviewGesture(eTrackpadGestureDirection direction) : m_direction(direction) {}

void COverviewGesture::begin(const ITrackpadGesture::STrackpadGestureBegin& e) {
    ITrackpadGesture::begin(e);

    m_lastDelta   = 0.F;
    m_firstUpdate = true;

    const auto MONITOR = Desktop::focusState()->monitor();
    if (!MONITOR)
        return;

    const bool WANT_CLOSE = m_direction == TRACKPAD_GESTURE_DIR_DOWN;
    const bool WANT_OPEN  = !WANT_CLOSE; // up, vertical, swipe: open if nothing is up

    auto       overview = scrollOverviewForMonitor(MONITOR);

    if (overview && overview->isClosing()) {
        dropScrollOverviewsForMonitor(MONITOR);
        overview.reset();
        if (WANT_CLOSE) {
            m_overview.reset();
            return;
        }
    }

    if (WANT_OPEN) {
        if (overview) {
            // Already open: swipe-up does not close. Swipe-down does.
            m_overview.reset();
            return;
        }
        if (!ensureScrollOverviewHooks())
            return;

        // Super-tap and the dock button reset the search session; swipe-open
        // was the remaining door, so a query survived close and came back.
        resetSearchSession();
        overview = makeShared<CScrollOverview>(MONITOR->m_activeWorkspace, true, MONITOR);
        registerScrollOverview(overview);
        overview->m_swipeClosing = false;
    } else {
        if (!overview) {
            m_overview.reset();
            return;
        }
        overview->selectHoveredWorkspace();
        overview->m_swipeClosing = true;
    }

    overview->m_isSwiping = true;
    m_overview            = overview;
}

void COverviewGesture::update(const ITrackpadGesture::STrackpadGestureUpdate& e) {
    const auto OVERVIEW = m_overview.lock();
    if (!OVERVIEW)
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

    OVERVIEW->onSwipeUpdate(m_lastDelta);
}

void COverviewGesture::end(const ITrackpadGesture::STrackpadGestureEnd& e) {
    const auto OVERVIEW = m_overview.lock();
    if (OVERVIEW)
        OVERVIEW->onSwipeEnd();
    m_overview.reset();
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

void clickLog(const char* fmt, ...) {
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
        dispatchNamed("hyprdrive:overview", "toggle");
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
void maybeClickFromHold(bool cancelled) {
    const uint32_t fingers = g_padFingers >= 2 ? g_padFingers : g_lastPadFingers;
    const float    dur     = sc<float>(std::chrono::duration<float, std::milli>(Time::steadyNow() - g_holdBeganAt).count());
    clickLog("holdClick? fingers=%u dur=%.0f swipe=%d cancelled=%d\n", fingers, dur, g_sawSwipeOrPinch ? 1 : 0, cancelled ? 1 : 0);
    if (g_sawSwipeOrPinch || fingers < 4)
        return;
    if (dur < 0.F || dur > 800.F)
        return;
    tryFireClick(fingers, 0);
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

void addClickGesture(size_t fingers, const std::string& action, uint32_t mods, bool disableInhibit, const std::string& arg) {
    removeClickGesture(fingers, mods);
    g_clickGestures.push_back(SClickGesture{fingers, action, mods, disableInhibit, arg});
    clickLog("addClick %zu %s arg=%s mods=%u\n", fingers, action.c_str(), arg.c_str(), mods);
}

void removeClickGesture(size_t fingers, uint32_t mods) {
    std::erase_if(g_clickGestures, [&](const SClickGesture& g) { return g.fingers == fingers && g.mods == mods; });
}

void attachPadFingerListeners() {
    g_padListeners.clear();
    g_padFingers            = 0;
    g_attachedPointerCount  = 0;
    if (!g_pInputManager)
        return;
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
