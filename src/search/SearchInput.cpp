#include "PluginParts.hpp"
using namespace PluginDetail;

static constexpr uint32_t EVDEV_KEY_ESC       = 1;

static xkb_state* activeXkbState() {
    const auto KEEB = g_pSeatManager ? g_pSeatManager->m_keyboard.lock() : nullptr;
    return KEEB ? KEEB->m_xkbState : nullptr;
}

namespace PluginDetail {
// SUPER, CTRL and ALT must keep their chords working while the overview is
// open, so a key pressed with one of them held is never search input.
//
// Tracked from the raw key stream rather than read from xkb: xkb was measured
// reporting logo=0 for the Z press of a SUPER+Z chord in this VM (see
// share/f_guard_fix.py). Shift is deliberately absent -- it types.
bool isGuardModifierKeycode(uint32_t keycode) {
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

// Keys that belong to the whole desktop, never to the search field: PRINT (and
// SYSRQ, which is what ALT+PRINT resolves to) and the XF86 family -- volume,
// brightness, media, mic mute, keyboard light. The overview-owns-the-keyboard
// catch-all lets these past so Omarchy's own binds still fire; the overview
// borrows those binds into its submap while it holds it (Overview.cpp,
// borrowGlobalPassthroughBinds).
bool isGlobalPassthroughKeycode(uint32_t keycode) {
    const auto STATE = activeXkbState();
    if (!STATE)
        return false;

    const xkb_keysym_t SYM = xkb_state_key_get_one_sym(STATE, keycode + 8);
    return SYM == XKB_KEY_Print || SYM == XKB_KEY_Sys_Req || (SYM >= 0x1008FF00 && SYM <= 0x1008FFFF);
}
} // namespace PluginDetail

xkb_state* overviewActiveXkbState() {
    return activeXkbState();
}

namespace PluginDetail {
bool overviewAcceptsSearch() {
    return std::ranges::any_of(scrollOverviews(), [](const auto& overview) { return overview && !overview->isClosing(); });
}

// A sequence half-typed when the overview closes, or when ESC/BACKSPACE cuts it
// short, must not swallow the first key of the next one.
void resetCompose() {
    Hyprdrive::TextInput::resetCompose();
}
} // namespace PluginDetail

// The query is UTF-8 now (it has to be -- a composed 'e' is two bytes), so
// BACKSPACE removes a CODEPOINT. pop_back() would leave a severed lead byte and
// hand the providers a string that is not valid UTF-8.
static void popCodepoint(std::string& s) {
    Hyprdrive::TextInput::popCodepoint(s);
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
    const auto K = Hyprdrive::TextInput::keyFor(activeXkbState(), keycode);
    return {.consume = K.consume, .starts = K.starts, .text = K.text};
}

static constexpr int     LETTER_TICK_MS    = 8;

static int letterTickCb(void*) {
    const auto NOW     = SearchLetters::clock::now();
    // The easter egg is a video: it wants every frame while it plays, and the
    // tick stays up while it does -- with or without an overview, so its
    // watchdog can stop the sound if the overview went away under it.
    Hyprdrive::KeyboardCat::watchdog();
    const bool CAT     = Hyprdrive::KeyboardCat::busy();
    const bool LETTERS = SearchLetters::g_searchLetters.busy(NOW) || CAT;

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

    if ((again || CAT) && g_letterTickTimer)
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

namespace PluginDetail {
// The trailing work every edit to the query owes, whatever made the edit.
void afterQueryEdit() {
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

bool handleSearchPress(uint32_t keycode) {
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
        if (Hyprdrive::KeyboardCat::noteBackspace())
            armLetterTick(); // its frames start with the trigger
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
    Hyprdrive::KeyboardCat::noteTyped();
    afterQueryEdit();
    return true;
}

void stopSearchRepeat() {
    g_searchRepeatKey = ~0u;
    if (g_searchRepeatTimer)
        wl_event_source_timer_update(g_searchRepeatTimer, 0);
}
} // namespace PluginDetail

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

namespace PluginDetail {
void startSearchRepeat(uint32_t keycode) {
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

int onFileResultsReady(int fd, uint32_t mask, void* data) {
    uint64_t val = 0;
    ::read(fd, &val, sizeof(val));
    for (const auto& overview : scrollOverviews()) {
        if (!overview)
            continue;
        overview->damage();
        overview->requestInputFrame();
    }
    Hyprdrive::Stacks::onReaderReady();
    return 0;
}

// Locking closes every overview at once and drops the search with it: no
// query, no half-typed compose sequence, no provider work still running, no
// key repeating (hyprdriveSessionLocked).
void onSessionLock() {
    stopSearchRepeat();
    g_consumedPresses.clear();
    resetSearchSession();
    const auto OPEN = scrollOverviews(); // close() unregisters: walk a copy
    for (const auto& OVERVIEW : OPEN)
        if (OVERVIEW)
            OVERVIEW->close();
}
} // namespace PluginDetail
