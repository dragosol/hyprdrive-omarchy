#include "PluginParts.hpp"
using namespace PluginDetail;

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
// COverview::close() keeps: the monitor asked for opens first, then every
// other one, so no path -- the dock's Show Overview, a query open -- leaves a
// monitor on the desktop while another shows the overview.
static bool openOverview(PHLMONITOR monitor) {
    OpenProf::begin(monitor ? monitor->m_name.c_str() : "?");
    // Cheap (two config reads) and catches a `hyprctl keyword` toggle that no
    // reload announced.
    Hyprdrive::FramePacing::apply();
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

    if (hyprdriveSessionLocked())
        return false;

    if (!ensureScrollOverviewHooks())
        return false;

    // A brand-new overview, not a reopen of one that is still closing.
    if (scrollOverviews().empty())
        resetSearchSession();

    const bool PREVRENDERINGOVERVIEW = renderingOverview;
    renderingOverview                = true;
    auto overview                    = makeShared<COverview>(monitor->m_activeWorkspace, false, monitor);
    registerScrollOverview(overview);
    renderingOverview = PREVRENDERINGOVERVIEW;
    return true;
}

// X32. The dock's Show Applications button opens the overview, and the hook
// setup above is not something a second implementation should duplicate.
bool openScrollOverview(PHLMONITOR monitor) {
    return openOverview(monitor);
}

// THE LOCK SCREEN OWNS THE KEYBOARD (marketplace review, #11392). Hyprland
// hands plugins every key before the seat does, the lock screen's included:
// with an overview up, keys meant for the password field became search text
// and reached the search providers. While the session is locked no overview
// opens, the raw key listener consumes nothing, and locking closes whatever
// was open and drops its search (onSessionLock).
//
// FROM THE LOCK SIGNAL ON, not only once Hyprland calls the session locked.
// The signal comes first: onSessionLock closes the overviews in it, and a
// close that hands the keyboard back at once did so in that gap -- the window
// focused before the lock got the keyboard for a moment as the lock started
// (gate lock_inert, 2026-11-13). noteSessionLockSignal() covers the gap; it
// lapses by itself if the lock never completes, so a lock that was refused
// cannot leave the overview unable to open.
namespace {
    std::chrono::steady_clock::time_point g_lockSignalAt{};
    bool                                  g_lockSignalled = false;
}

void noteSessionLockSignal() {
    g_lockSignalled = true;
    g_lockSignalAt  = std::chrono::steady_clock::now();
}

bool hyprdriveSessionLocked() {
    if (g_pSessionLockManager && g_pSessionLockManager->isSessionLocked()) {
        g_lockSignalled = false; // Hyprland's own answer from here on
        return true;
    }
    if (g_lockSignalled && std::chrono::steady_clock::now() - g_lockSignalAt > std::chrono::seconds(5))
        g_lockSignalled = false;
    return g_lockSignalled;
}

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
    SearchBackend::closeSushi();
    g_search.reset();
    SearchLetters::g_searchLetters.reset();
    SearchBackend::resetMenuRoute();
    Membership::bump("search session reset");
    resetCompose(); // a sequence abandoned last time must not eat the first key
    SearchProviders::query("");
}

namespace PluginDetail {
SDispatchResult onOverviewDispatcher(std::string arg) {
    // Ahead of parsing the action, which is deliberate and is load-bearing for
    // the CLOSE actions too: a toggle-off with a query typed drops the filter
    // first, so the windows it was hiding are back in the grid for the close
    // animation instead of popping in after it.
    // Tab is a step INSIDE the session: it must not reset it (it runs first).
    if (splitOverviewArg(arg).first == "tab") {
        if (const auto ACTIVE = activeScrollOverview())
            if (auto* S = dynamic_cast<COverview*>(ACTIVE.get()))
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
        if (auto* S = ACTIVE ? dynamic_cast<COverview*>(ACTIVE.get()) : nullptr)
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

    // X54, reversed. Search providers are D-Bus activated and go cold again
    // after well under a minute of idle. Warming them here (StartServiceByName
    // on every provider) cut a cold first search from ~1085ms to ~342ms, but
    // it cold-started Nautilus, GNOME Software, gvfs, the portal and Sushi on
    // EVERY open -- most of which never search -- and did it during the open
    // animation. On a 16 GB laptop under memory pressure each of those GTK
    // starts allocates GPU buffers through compaction and stalled the
    // compositor 0.3-1.4 s (measured 2026-10-23). The first keystroke's own
    // GetInitialResultSet auto-starts a cold provider instead: app rows are
    // in-memory and stay instant, provider rows land when the provider is up.
    SearchBackend::init();

    for (const auto& monitor : MONITORS) {
        if (!openOverview(monitor))
            return {.success = false, .error = "failed enabling overview hooks (is other overview plugin enabled?)"};
    }
    return {};
}
} // namespace PluginDetail
