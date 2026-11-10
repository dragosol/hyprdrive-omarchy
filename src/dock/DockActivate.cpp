// What a click, a hotkey, a menu row or a drop on the pill DOES: window
// queries, the seat, focus history, activation, launching, cycling and
// workspace moves. activateRow/hotkey/dragRelease live here because they
// call launchApp/activateItem with default arguments.
#include "DockParts.hpp"

namespace ScrollOverview::Dock {
using namespace DockDetail;
namespace DockDetail {

// [FLIGHT] revealNow STAYS in the dock -- it drives g_reveal, which autohide
// owns -- but the flight module calls it, so it needs external linkage: the
// definition now sits with the other cross-module entry points at the bottom
// of this file, in the named namespace.

// [FLIGHT] windowRenderBox / offscreenAimBox / flightAnchorBox moved to
// FlightController.cpp -- they answer "where does this flight end", which is
// the state machine's question.

// [FLIGHT] overviewOpenFor, restoreForFlight, the flight log's zdbg,
// reverseFlight and the minimize/restore impls moved to
// FlightController.cpp -- they are Flight::minimize / Flight::restore and the
// FSM's own reverse() now. dismissHoverPanelNow STAYS in the dock (it drives
// g_panel / g_tip) and is declared in Dock.hpp for the flight module.

// X24's click policy, straight off the reference config: `click-action`
// minimize, `activate-single-window` true, `minimize-shift` true,
// `shift-click-action` minimize.
//
// X32 adds the LAUNCH branch, which X27 could not have: a pinned favourite owns
// no window, so `click-action` has nothing to minimize and the reference falls
// through to launching the app.
// A dock icon is a favourite desktop id, a live window class, or both. Exact
// class match is not enough: a pinned Chromium webapp's window is
// `chrome-{host}_{path}-Default` while the favourite's id is the .desktop
// stem, and a second Chrome profile is a second class of the same id. Clicking
// the icon with those windows on THIS desktop must find them -- otherwise the
// ladder sees "none here" and launches a duplicate.
bool windowMatchesItem(const PHLWINDOW& window, const SItem& item, const SInterestCfg& cfg) {
    if (!interesting(window, cfg))
        return false;

    const auto CLASS = AppSearch::identityClass(window->m_class, window->m_title);

    if (!item.wmClass.empty() && CLASS == item.wmClass)
        return true;

    if (!item.desktopId.empty() && AppSearch::idForClass(CLASS) == item.desktopId)
        return true;

    return false;
}

// A LOCATION's windows (user, 2026-10-13: the Trash's menu "is lacking a lot"
// next to Files'): the file manager's windows showing it. Files says WHICH
// locations it has open (FileManager1.OpenWindowsWithLocations, Locations.cpp)
// but not in compositor windows, and titles a window after its location's
// display name -- the same name the dock gives the item. So: a window titled
// that while Files reports the location open.
std::vector<PHLWINDOW> locationWindows(const SItem& item) {
    if (item.uri.empty() || item.stack || item.locationName.empty() || !Locations::openWindows(item.uri))
        return {};
    std::vector<PHLWINDOW> out;
    for (const auto& WINDOW : Desktop::windowState()->windows())
        if (WINDOW->m_isMapped && WINDOW->m_title == item.locationName)
            out.emplace_back(WINDOW);
    return out;
}

// What a menu's window rows act on: an app's windows, or a location's.
std::vector<PHLWINDOW> menuWindows(const SItem& item) {
    return itemWindows(item);
}

std::vector<PHLWINDOW> itemWindows(const SItem& item) {
    // A location's windows are the file manager's windows showing it, so a
    // click, the menu's window rows and the hover previews treat the Trash the
    // way they treat an app (user, 2026-10-13).
    if (!item.uri.empty())
        return item.stack ? std::vector<PHLWINDOW>{} : locationWindows(item);
    if (item.wmClass.empty() && item.desktopId.empty())
        return {};

    const auto             CFG = interestCfg();
    std::vector<PHLWINDOW> out;
    for (const auto& WINDOW : Desktop::windowState()->windows()) {
        if (windowMatchesItem(WINDOW, item, CFG))
            out.emplace_back(WINDOW);
    }

    return out;
}

// The app's windows on the ACTIVE DESKTOP. A minimized window counts toward
// the desktop it will COME BACK TO: its own m_workspace is the stash
// (`special:minimized`) and answers no useful question, so this goes through
// the overview's one home-desktop helper rather than inventing a second answer.
//
// SCOPED IS THE CLICK, not the dock. runningClasses/dockItems stay global: an
// app running on another desktop keeps its icon and its running DOT.
std::vector<PHLWINDOW> itemWindowsHere(const SItem& item, PHLMONITOR monitor) {
    const auto ACTIVE = monitor ? monitor->m_activeWorkspace : nullptr;
    auto       all    = itemWindows(item);
    if (!ACTIVE)
        return all;

    std::vector<PHLWINDOW> out;
    for (const auto& WINDOW : all) {
        if (OvInternal::overviewGridHomeWorkspace(WINDOW) == ACTIVE->m_id)
            out.emplace_back(WINDOW);
    }

    return out;
}

std::string itemCycleKey(const SItem& item) {
    if (!item.uri.empty())
        return item.uri;
    return !item.wmClass.empty() ? item.wmClass : item.desktopId;
}

PHLWINDOW frontWindow(const std::vector<PHLWINDOW>& windows) {
    for (const auto& WINDOW : windows) {
        if (WINDOW && !Minimize::isMinimized(WINDOW))
            return WINDOW;
    }

    return windows.empty() ? nullptr : windows.front();
}

// The one of `windows` focused most recently, by Hyprland's own focus history
// (newest last); frontWindow when none of them is in it yet.
PHLWINDOW recentWindow(const std::vector<PHLWINDOW>& windows) {
    const auto& HISTORY = Desktop::History::windowTracker()->fullHistory();
    for (auto it = HISTORY.rbegin(); it != HISTORY.rend(); ++it) {
        if (const auto WINDOW = it->lock(); WINDOW && std::ranges::find(windows, WINDOW) != windows.end())
            return WINDOW;
    }
    return frontWindow(windows);
}

void stealDockSeat() {
    g_dockSeatHeld = true;
    if (!g_pSeatManager)
        return;
    g_pSeatManager->setPointerFocus(nullptr, {});
    g_pSeatManager->setKeyboardFocus(nullptr);
}

void restoreDockSeat() {
    if (!g_dockSeatHeld)
        return;
    g_dockSeatHeld = false;

    const auto WINDOW = Desktop::focusState()->window();
    if (!WINDOW || !WINDOW->m_isMapped)
        return;

    SP<CWLSurfaceResource> surf;
    if (WINDOW->wlSurface())
        surf = WINDOW->wlSurface()->resource();
    if (g_pSeatManager && surf)
        g_pSeatManager->setKeyboardFocus(surf);
}
}


void noteFocus(const PHLWINDOW& window, Desktop::eFocusReason reason) {
    const auto NOW   = std::chrono::steady_clock::now();
    const bool MEANT = reason != Desktop::FOCUS_REASON_FFM;
    if (window == g_focusNow.lock()) {
        g_focusMeant |= MEANT;
        return;
    }
    if (g_focusNow.lock() && (g_focusMeant || NOW - g_focusSince >= PASSING))
        g_focusSettled = g_focusNow;
    g_focusNow   = window;
    g_focusSince = NOW;
    g_focusMeant = MEANT;
}
namespace DockDetail {


static PHLWINDOW intentFocus() {
    const auto CUR = Desktop::focusState()->window();
    if (CUR && CUR == g_focusNow.lock() && !g_focusMeant && std::chrono::steady_clock::now() - g_focusSince < PASSING)
        if (const auto SETTLED = g_focusSettled.lock(); SETTLED && Desktop::View::validMapped(SETTLED))
            return SETTLED;
    return CUR;
}

// Go to a window on the desktop it belongs to. Preview clicks and "show me
// that window" must switch there -- never fly the window onto the current
// desktop (the restore flight comes up from the dock) and never toggle
// special:minimized (Omarchy slides that workspace in vertically).
void activateWindow(const PHLWINDOW& window) {
    if (!window)
        return;

    // A desktop change from here is FOCUS moving (animation:desktop_focus),
    // unless a bind is running it -- Super+1..0 with dock:hot_keys.
    DesktopSwitch::CFocusScope focusChange;

    if (Minimize::isMinimized(window)) {
        Minimize::showOriginalWorkspace(window);
        Minimize::restore(window);
        return;
    }

    // A window parked on a SCRATCHPAD: pull that workspace up first. Focusing
    // it with the scratchpad down left the window focused but invisible, so the
    // icon (and a preview click) looked like it had done nothing at all.
    if (const auto SCRATCH = Scratchpad::workspaceFor(window))
        Scratchpad::show(SCRATCH);

    if (const auto WORKSPACE = window->m_workspace; WORKSPACE && !WORKSPACE->m_isSpecialWorkspace) {
        if (const auto MONITOR = WORKSPACE->m_monitor.lock(); MONITOR && MONITOR->m_activeWorkspace != WORKSPACE)
            MONITOR->changeWorkspace(WORKSPACE, false, true, false);
    }

    Desktop::windowState()->raise(window);
    Desktop::focusState()->fullWindowFocus(window, Desktop::FOCUS_REASON_SWITCH_TO_WINDOW_HARD);

    // Activate restores seat onto the window; take it back if the pointer is
    // still on the dock so the next tick cycles instead of scrolling the client.
    if (g_dockSeatHeld)
        stealDockSeat();
}

// Scroll (and CTRL+click) cycle. The list is explicit so the click can hand
// this desktop's windows and the wheel can hand the whole app. Walking to
// another desktop changes the count, which drops the memory and restarts the
// cycle at the top of the new set.
void cycleThroughWindows(const std::string& wmClass, bool reversed, const std::vector<PHLWINDOW>& WINDOWS) {
    if (WINDOWS.empty())
        return;

    const bool STALE = !g_cycle || g_cycle->wmClass != wmClass || g_cycle->windows.size() != WINDOWS.size() || elapsedMs(g_cycle->at) > CYCLE_MEMORY_MS;

    if (STALE) {
        g_cycle = SCycle{.wmClass = wmClass, .index = 0};
        g_cycle->windows.reserve(WINDOWS.size());
        for (const auto& WINDOW : WINDOWS)
            g_cycle->windows.emplace_back(WINDOW);
    }

    g_cycle->at = Time::steadyNow();

    const int COUNT = sc<int>(g_cycle->windows.size());
    g_cycle->index  = reversed ? (g_cycle->index - 1 + COUNT) % COUNT : (g_cycle->index + 1) % COUNT;

    // A window can die while the memory still holds it. Dropping the memory
    // rather than skipping the slot keeps the index and the list in step.
    const auto WINDOW = g_cycle->windows[g_cycle->index].lock();
    if (!WINDOW) {
        g_cycle.reset();
        return;
    }

    activateWindow(WINDOW);
}

// The reference's shared entry point for a cycle: if the app already has focus
// and nothing of it is shouting, step to its next window; otherwise the gesture
// is simply "bring this app forward", which is what an urgent window wants.
// Scroll on the icon uses this (global, every window of the app).
void cycleOrActivate(const SItem& item, bool reversed) {
    const auto WINDOWS = itemWindows(item);
    if (WINDOWS.empty())
        return;

    const auto FOCUSED   = Desktop::focusState()->window();
    const bool OWNSFOCUS = FOCUSED && windowMatchesItem(FOCUSED, item, interestCfg());

    if (OWNSFOCUS && !classUrgent(item.wmClass))
        cycleThroughWindows(itemCycleKey(item), reversed, WINDOWS);
    else
        activateWindow(frontWindow(WINDOWS));
}

// The overview gets out of the way, because a window that maps behind an open
// overview is a window the user cannot see they just started.
//
// X33 gave this a second caller and with it a second way to be asked: a RUNNING
// app middle-clicked for a new window owns a class but need not own a favourite,
// so `desktopId` can be empty where `idForClass` still answers.
// Z11 (user): spam-clicking a launcher must REPLAY the blow-up but exec ONE
// instance. The vulnerable window is exec -> first map: once a window exists,
// clicks take the toggle branch and never reach here. 30s covers a cold app;
// a slower one gets a deliberate second chance.
// ponytail: plain timestamps, no startup-notification protocol.
// (g_pendingLaunches and its window live above buildDockItems, which clears
// an entry once its launch has landed.)

// V3 widened this in two places, both in its signature, and both because a
// DRAG is a different statement from a click:
//   `onWorkspace`  where the app is to start. Null is "here", which is every
//                  caller that existed before -- a plain spawn lands on the
//                  active desktop and the overview gets out of its way.
//                  Non-null goes out through the exec rule and is SILENT, so
//                  the overview stays up and the session does not move.
//   `deliberate`   skip Z11's spam guard. Z11 exists because a launcher can be
//                  double-CLICKED faster than an app can map, and the second
//                  click is not a second intention. A drag is not that: it is a
//                  press, a journey and a release aimed at a named desktop, and
//                  "drop kitty on 3, then drop kitty on 5" must give two
//                  kittys. The guard's other half is untouched -- an app with
//                  windows already never reaches it (FIRSTINSTANCE), so this
//                  only differs for the app that is still coming up.
void launchApp(PHLMONITOR monitor, const SItem& item, PHLWORKSPACE onWorkspace = nullptr, bool deliberate = false, const std::string& execRule = "") {
    // Y6. Armed here rather than in activateItem so every path that actually
    // starts something gets the blow-up -- a click on a closed app, a hotkey,
    // and `middle-click-action=launch` alike. GNOME calls animateLaunch() from
    // the same place, on activation rather than on the branch that chose it.
    if (!item.icon.empty() && item.iconBox.width >= 1 && item.iconBox.height >= 1)
        g_flight.launchZooms.emplace_back(SLaunchZoom{.icon = item.icon, .monitor = monitor ? monitor->m_id : MONITOR_INVALID, .from = item.iconBox, .startedAt = Time::steadyNow()});

    const bool  FIRSTINSTANCE = item.uri.empty() ? itemWindows(item).empty() : true;
    const auto  PENDKEY       = pendingLaunchKey(item);
    const float PENDING_MS    = item.uri.empty() ? LAUNCH_PENDING_MS : LOCATION_PENDING_MS;
    if (!deliberate && FIRSTINSTANCE && !PENDKEY.empty()) {
        if (const auto IT = g_pendingLaunches.find(PENDKEY); IT != g_pendingLaunches.end() && elapsedMs(IT->second) < PENDING_MS)
            return; // the zoom above replays; the exec does not
        g_pendingLaunches[PENDKEY] = Time::steadyNow();
    }

    // A SILENT launch leaves the overview where it is. The close below exists
    // because "a window that maps behind an open overview is a window the user
    // cannot see they just started" -- and that is exactly backwards for a drop
    // onto another desktop's card, where the overview IS how they see it happen.
    const bool SILENT = onWorkspace && onWorkspace != (monitor ? monitor->m_activeWorkspace : PHLWORKSPACE{});
    const auto closeOverview = [&] {
        if (SILENT)
            return;
        if (const auto OVERVIEW = scrollOverviewForMonitor(monitor); OVERVIEW)
            OVERVIEW->close();
    };

    // X35. `new-window` on a location is opening it -- the reference's fake
    // ShellApp answers `open_new_window` with the same `launch` either way.
    if (!item.uri.empty()) {
        Locations::activate(item.uri);
        closeOverview();
        return;
    }

    const auto  ID    = item.desktopId.empty() ? AppSearch::idForClass(item.wmClass) : item.desktopId;
    const auto* ENTRY = AppSearch::entryForId(ID);
    if (!ENTRY)
        return;

    // `workspace <name> silent`: Hyprland's own exec rule, matched against the
    // window when it maps through a token in the child's environment. The NAME
    // rather than the id, because that is what a workspace rule matches on and
    // what a named desktop would need.
    AppSearch::launchWithRule(*ENTRY, Config::getTerminal(), !execRule.empty() ? execRule : SILENT ? "workspace " + onWorkspace->m_name + " silent" : "");

    closeOverview();
}

// OPEN IN EMPTY WORKSPACE (user, 2026-09-29): SUPER/ALT/CTRL+click on an app
// that is not running, and the context menu's "Open in Empty Workspace". The
// lowest-numbered desktop with no windows on this monitor (Hyprland's own
// `emptym` -- which is this one, when this one is empty): switch to it (created
// if need be, with the usual animation), and launch pinned to it, so an app
// that maps slowly still lands there if the user has moved on.
void launchInEmptyWorkspace(PHLMONITOR monitor, const SItem& item) {
    if (!monitor || item.stack || item.scratchpad)
        return;
    const auto TARGET = getWorkspaceIDNameFromString("emptym");
    // A location (the Trash): its file manager window, opened on the empty
    // desktop -- it opens where the user is, so go there first.
    if (!item.uri.empty()) {
        if (const auto OVERVIEW = scrollOverviewForMonitor(monitor); OVERVIEW)
            OVERVIEW->close();
        if (TARGET.id != WORKSPACE_INVALID && (!monitor->m_activeWorkspace || monitor->m_activeWorkspace->m_id != TARGET.id))
            (void)::Config::Actions::changeWorkspace(std::to_string(TARGET.id));
        Locations::activate(item.uri);
        return;
    }
    if (TARGET.id == WORKSPACE_INVALID) {
        launchApp(monitor, item, nullptr, true);
        return;
    }
    // The overview first: a desktop switch under an open overview is a switch
    // the user cannot see.
    if (const auto OVERVIEW = scrollOverviewForMonitor(monitor); OVERVIEW)
        OVERVIEW->close();
    if (!monitor->m_activeWorkspace || monitor->m_activeWorkspace->m_id != TARGET.id)
        (void)::Config::Actions::changeWorkspace(std::to_string(TARGET.id));
    launchApp(monitor, item, nullptr, true, "workspace " + std::to_string(TARGET.id));
}

// OPEN IN SCRATCHPAD (user, 2026-10-13): a new window of the app, born on the
// scratchpad -- Omarchy's own `special:scratchpad` when there is none yet --
// and the scratchpad pulled up so the user sees it arrive. A location (the
// Trash) cannot take a launch rule, so its window opens on the scratchpad by
// opening it while the scratchpad is the shown, focused workspace.
void launchInScratchpad(PHLMONITOR monitor, const SItem& item) {
    if (!monitor || item.stack || item.scratchpad)
        return;
    const auto SCRATCH = Scratchpad::ensureDefault(monitor);
    if (!SCRATCH)
        return;
    if (const auto OVERVIEW = scrollOverviewForMonitor(monitor); OVERVIEW)
        OVERVIEW->close();
    if (!item.uri.empty()) {
        Scratchpad::show(SCRATCH);
        Locations::activate(item.uri);
        return;
    }
    launchApp(monitor, item, nullptr, true, "workspace " + SCRATCH->m_name);
    Scratchpad::show(SCRATCH);
}

// The empty desktop closest by id to `from`: `emptym`'s own test of empty -- no
// such desktop yet, or one on `monitor` with no windows -- but searched outward
// from `from` rather than up from 1, because a window being given room of its
// own should land beside where it was, not back on desktop 1 from desktop 7. A
// tie goes up, the direction Hyprland's own `emptyn` looks. Always answers: an
// id past every existing desktop is empty by definition.
WORKSPACEID nearestEmptyWorkspaceID(const PHLMONITOR& monitor, WORKSPACEID from) {
    const auto EMPTY = [&monitor](WORKSPACEID id) {
        for (const auto& CANDIDATE : State::workspaceState()->workspaces()) {
            if (const auto WORKSPACE = CANDIDATE.lock(); WORKSPACE && WORKSPACE->m_id == id)
                return WORKSPACE->getWindowCount() == 0 && WORKSPACE->m_monitor.lock() == monitor;
        }
        return true;
    };

    for (WORKSPACEID step = 1;; ++step) {
        if (EMPTY(from + step))
            return from + step;
        if (from - step >= 1 && EMPTY(from - step))
            return from - step;
    }
}

// MOVE TO EMPTY WORKSPACE (user, 2026-10-08): the context menu's sibling of
// "Open in Empty Workspace", offered under each of the app's windows -- that
// window goes, rather than a new one coming. Hyprland's own movetoworkspace
// moves it AND follows it, so the user lands on the new desktop with it
// focused, the way Open in Empty Workspace lands them beside the app it
// launched.
// MOVE TO THIS WORKSPACE (user, 2026-10-15): the window joins the desktop the
// dock's monitor is showing, and is focused there. Off a scratchpad or another
// desktop alike; the menu only offers it when the window is elsewhere.
void moveToThisWorkspace(PHLMONITOR monitor, const PHLWINDOW& WINDOW) {
    if (!monitor || !WINDOW)
        return;
    const auto TARGET = monitor->m_activeWorkspace;
    if (!TARGET || TARGET->m_isSpecialWorkspace || WINDOW->m_workspace == TARGET)
        return;
    // Off the minimize stash first, for moveToEmptyWorkspace's reason.
    if (Minimize::isMinimized(WINDOW))
        Minimize::restore(WINDOW);
    if (const auto OVERVIEW = scrollOverviewForMonitor(monitor); OVERVIEW)
        OVERVIEW->close();
    (void)::Config::Actions::moveToWorkspace(TARGET, true, WINDOW);
    activateWindow(WINDOW);
    if (g_dockSeatHeld)
        stealDockSeat();
}

void moveToEmptyWorkspace(PHLMONITOR monitor, const PHLWINDOW& WINDOW) {
    if (!monitor || !WINDOW)
        return;

    // A stashed window comes off the stash FIRST, for Scratchpad::moveWindowTo's
    // reason: otherwise Minimize goes on owning it, and a later restore pulls it
    // back off the desktop it was just given. Restoring also puts it back on its
    // own desktop, which is then where "nearest" is measured from.
    if (Minimize::isMinimized(WINDOW))
        Minimize::restore(WINDOW);

    // Measured from the window's desktop, on the monitor that desktop is on. A
    // window with no ordinary desktop (one parked on a scratchpad, or on a named
    // desktop with a negative id) has no "near" of its own: the dock's monitor
    // and what it is showing stand in, and a non-positive anchor degrades to
    // `emptym`'s lowest empty desktop.
    const auto  FROM      = WINDOW->m_workspace;
    const bool  ONDESKTOP = FROM && !FROM->m_isSpecialWorkspace;
    const auto  HOME      = ONDESKTOP ? FROM->m_monitor.lock() : PHLMONITOR{};
    const auto  MONITOR   = HOME ? HOME : monitor;
    WORKSPACEID anchor    = ONDESKTOP ? FROM->m_id : MONITOR->m_activeWorkspace ? MONITOR->m_activeWorkspace->m_id : 0;
    anchor                = std::max<WORKSPACEID>(anchor, 0);

    const auto   ID = nearestEmptyWorkspaceID(MONITOR, anchor);
    PHLWORKSPACE target;
    for (const auto& CANDIDATE : State::workspaceState()->workspaces()) {
        if (const auto WORKSPACE = CANDIDATE.lock(); WORKSPACE && WORKSPACE->m_id == ID)
            target = WORKSPACE;
    }
    if (!target)
        target = State::workspaceState()->create(ID, MONITOR->m_id, std::to_string(ID));
    if (!target)
        return;

    // The overview first: a desktop switch under an open overview is a switch
    // the user cannot see.
    if (const auto OVERVIEW = scrollOverviewForMonitor(monitor); OVERVIEW)
        OVERVIEW->close();
    (void)::Config::Actions::moveToWorkspace(target, false, WINDOW);

    // activateWindow's reason: the focus restored the seat onto the window while
    // the pointer is still over the dock.
    if (g_dockSeatHeld)
        stealDockSeat();
}

void activateItem(PHLMONITOR monitor, const SItem& item, bool shift, bool ctrl = false, bool emptyWorkspace = false) {
    // X35. A location has no windows to minimize: it is opened. Nautilus raises
    // the window it already has for a location rather than opening a second
    // one, so this is the reference's "focus it" too.
    // V11 V4. A STACK opens its fan or its grid rather than launching --
    // tested before the location branch, because a stack carries a uri too.
    // A pinned FILE has nothing to fan, and Stacks::open opens it instead.
    // The scratchpad's own icon: toggling the workspace is the WHOLE action.
    // There is no app to launch, nothing to stash, and no second instance to
    // spawn -- tested first because it carries neither a class nor a uri and
    // would otherwise fall through every branch below to launchApp.
    if (item.scratchpad) {
        const auto WORKSPACE = Scratchpad::byShortName(item.scratchpadName);

        // ...unless the overview is up, when it is NOT a toggle. The overview
        // is covering the scratchpad, so a click on the icon is the user asking
        // to SEE it -- and toggling would answer that by pulling it down, which
        // reads as the click doing nothing at all (user, 2026-09-22).
        //
        // The overview has to COMMIT to a scratchpad window, not merely close
        // with the scratchpad shown. Closing hands focus back to its own
        // selection twice -- once in close(), and again when the close animation
        // ends and restoreOverviewKeyboard refocuses closeOnWindow -- and
        // focusing a window outside an open special workspace makes Hyprland
        // hide it. The first version did show()-then-close() and the scratchpad
        // flashed up for the length of the animation and went (user,
        // 2026-09-23: "just closes overview"). selectAndClose makes a window on
        // the scratchpad the thing both refocuses land on.
        if (const auto OVERVIEW = openScrollOverviewForMonitor(monitor); OVERVIEW) {
            Scratchpad::show(WORKSPACE);
            if (const auto WINDOWS = Scratchpad::windowsOn(WORKSPACE); !WINDOWS.empty())
                OVERVIEW->selectAndClose(WINDOWS.front());
            else
                OVERVIEW->close();
            return;
        }

        Scratchpad::toggle(WORKSPACE);
        return;
    }

    if (item.stack) {
        Stacks::open(monitor, item, item.iconBox);
        return;
    }

    // A location with no window showing it is opened; with one, it is an app
    // like any other below: switch back to it, minimize it, restore it.
    if (!item.uri.empty() && itemWindows(item).empty()) {
        launchApp(monitor, item);
        return;
    }

    // A modifier click on an app that is not running opens it on an empty
    // desktop -- and so does any click on one set to always open there.
    // Running, the modifier changes nothing (CTRL still cycles below).
    if ((emptyWorkspace || opensInEmptyDesktop(itemDesktopId(item))) && itemWindows(item).empty()) {
        launchInEmptyWorkspace(monitor, item);
        return;
    }

    // Scroll cycles. Click does not. CTRL+click is the leftover cycle from Y4
    // (scoped to this desktop when the app is here, otherwise the whole app).
    const auto here = itemWindowsHere(item, monitor);
    if (ctrl) {
        const auto CYCLE = !here.empty() ? here : itemWindows(item);
        cycleThroughWindows(itemCycleKey(item), false, CYCLE);
        return;
    }

    // SWITCH BACK FIRST (user, 2026-09-24: "I have multiple ghostty windows
    // open. I switch to zen. I then click on the ghostty icon in dock. First
    // click on that should immediately switch me back to the last used window
    // of that process, same with every other app"). While another app has the
    // focus, a click is "take me back to this app": its most recently used
    // window, wherever that is -- another desktop, the scratchpad, minimized.
    // Only once the app HAS the focus do the rules below (minimize/restore the
    // windows here) apply, so a second click still puts it away.
    // ...but NOT when every window of the app on this desktop is minimized
    // (user, 2026-09-25: spam-clicking two tiled Ghostty windows left one
    // restored full-width and the other stashed). Minimizing them is exactly
    // what takes the focus off the app, so the next click read as "switch back"
    // and restored ONE window. With all of them away here, the click is the
    // toggle below: restore them all.
    const bool ALLHERESTASHED = !here.empty() && std::ranges::all_of(here, [](const PHLWINDOW& w) { return Minimize::isMinimized(w); });
    {
        std::string st = "app click " + item.wmClass + " here=" + std::to_string(here.size()) + " allStashed=" + (ALLHERESTASHED ? "1" : "0") + " focus=" +
            (Desktop::focusState()->window() ? Desktop::focusState()->window()->m_class : std::string("none")) + " intent=" +
            (intentFocus() ? intentFocus()->m_class : std::string("none"));
        for (const auto& W : here)
            st += std::format(" [{:x} min={} flight={} restoreFlight={} ws={}]", (uintptr_t)W.get(), Minimize::isMinimized(W) ? 1 : 0, Flight::hasFlight(W) ? 1 : 0,
                              Flight::isRestoreFlight(W) ? 1 : 0, W->m_workspace ? W->m_workspace->m_name : "-");
        Stacks::traceEvent(st);
    }
    if (const auto ALL = itemWindows(item); !ALL.empty() && !ALLHERESTASHED) {
        const auto FOCUSED = intentFocus(); // not a window the pointer passed over
        if (!FOCUSED || std::ranges::find(ALL, FOCUSED) == ALL.end()) {
            const auto TARGET = recentWindow(ALL);
            Stacks::traceEvent("  -> switch back to one window");
            if (const auto OVERVIEW = scrollOverviewForMonitor(monitor); OVERVIEW && !Minimize::isMinimized(TARGET))
                OVERVIEW->selectAndClose(TARGET);
            else
                activateWindow(TARGET);
            return;
        }
    }

    // "Click it again and it goes away", for an app that lives in the
    // scratchpad. Its windows are not on any desktop, so `here` is empty and
    // the branches below would focus a window that is already up -- the click
    // read as doing nothing. Scoped to the scratchpad SHOWN on this monitor and
    // to an app entirely inside it: an app with a window out on a desktop too
    // keeps the ordinary rules, because that window is what a click is about.
    if (here.empty()) {
        if (const auto SHOWN = Scratchpad::shownOn(monitor)) {
            const auto ALL = itemWindows(item);
            if (!ALL.empty() && std::ranges::all_of(ALL, [&](const auto& window) { return Scratchpad::workspaceFor(window) == SHOWN; })) {
                Scratchpad::hide(SHOWN);
                return;
            }
        }
    }

    // Window on THIS desktop: minimize/maximize that window. Never spawn a
    // second instance of an app you can already see (or just hid) here.
    // Visible -> minimize; hidden (or mid-restore flight) -> restore.
    // Scroll, not click, is what cycles when there is more than one.
    if (!here.empty()) {
        // The whole app goes (or comes back) as one: each window is told its
        // size once, not once per sibling leaving the layout (ConfigureHold.hpp).
        ConfigureHold::CHold      oneResize;
        Minimize::CGestureBoxes   boxesBeforeAnyMoves(here);
        std::vector<PHLWINDOW> visible;
        for (const auto& WINDOW : here) {
            if (!Minimize::isMinimized(WINDOW))
                visible.emplace_back(WINDOW);
        }

        // Z5. A window mid-RESTORE-flight still reads as minimized (it stays
        // stashed until landing), so without this a spam of clicks re-restored
        // forever instead of toggling. Count it as visible; the next minimize
        // then reverses its flight in place.
        for (const auto& WINDOW : here) {
            if (Flight::hasFlight(WINDOW) && Flight::isRestoreFlight(WINDOW) && Minimize::isMinimized(WINDOW))
                visible.emplace_back(WINDOW);
        }

        if (visible.empty()) {
            Stacks::traceEvent("  -> restore " + std::to_string(here.size()));
            {
                Minimize::CRestoreSlots ownSlots(here);
                for (const auto& WINDOW : here)
                    Flight::restore(monitor, WINDOW);
            }
            Flight::retargetRestores();
            return;
        }

        Stacks::traceEvent("  -> minimize " + std::to_string(visible.size()));
        for (const auto& WINDOW : visible)
            Flight::minimize(monitor, WINDOW);
        return;
    }

    // Running only on another desktop: go there and focus it. Launching a
    // fresh window here is what left the original stashed on special:minimized
    // after a desktop switch ("locked regardless of desktop").
    if (const auto ALL = itemWindows(item); !ALL.empty()) {
        activateWindow(frontWindow(ALL));
        return;
    }

    launchApp(monitor, item);
}

// X33. `middle-click-action` and `shift-middle-click-action`, both `launch` in
// the reference.
//
// The structure is the reference's, not a switch of my own: the whole action
// table lives inside `if (this.running)`, and a NOT-running app ignores the
// setting entirely and just launches. That is why the empty check comes first
// and takes no notice of `action`.
//
// `minimize` routes back through activateItem so the two buttons cannot drift
// apart -- it is the left button's action, and a user who sets middle to
// `minimize` means that one.
//
// V6 scoped the left button, and this table is deliberately NOT scoped with it.
// The amendment is about "i click its icon"; these are the reference's actions
// over an app, and `quit` that killed only the windows on this desktop, or
// `skip` that could not reach the window you can see running elsewhere, would be
// a different feature nobody asked for. `minimize` is the one action V6 names,
// and it inherits the scoping through activateItem, which is the point of
// routing it there.
void applyClickAction(PHLMONITOR monitor, const SItem& item, const std::string& action, bool shift) {
    // X35. Every one of the reference's five actions is about an app's windows,
    // and a location has none -- `launch` is the only one it can answer, and
    // opening it is what that means here.
    if (!item.uri.empty()) {
        activateItem(monitor, item, shift);
        return;
    }

    const auto WINDOWS = itemWindows(item);

    if (WINDOWS.empty()) {
        launchApp(monitor, item);
        return;
    }

    if (action == "launch")
        launchApp(monitor, item);
    else if (action == "minimize")
        activateItem(monitor, item, shift);
    else if (action == "cycle-windows")
        cycleOrActivate(item, false);
    else if (action == "quit") {
        for (const auto& WINDOW : WINDOWS)
            WINDOW->sendClose();
    } else if (action == "skip")
        activateWindow(WINDOWS.front());
}

void activateRow(PHLMONITOR monitor, const SItem& item, const SRow& row) {
    const auto& wmClass = item.wmClass;

    switch (row.kind) {
        case ERow::Separator: return;

        // V11 V4. Per-STACK, and written straight to the store: the toggle is
        // the setting, so there is nowhere else for it to live.
        case ERow::StackRecent: Locations::activate("file://" + row.exec); return;
        case ERow::StackSortKey:
            if (const auto K = StackModel::sortFromName(row.exec))
                Stacks::setSort(item.stackPath, *K);
            return;
        case ERow::StackDisplayKey: Stacks::setDisplay(item.stackPath, row.exec == "folder" ? StackModel::EDisplay::Folder : StackModel::EDisplay::Stack); return;
        case ERow::StackViewKey: Stacks::setViewMode(item.stackPath, StackModel::viewFromName(row.exec)); return;
        case ERow::StackShowParent:
            // The parent folder, this one selected, in the user's file manager.
            Locations::reveal(item.stackPath);
            return;
        case ERow::StackTrashNewest: {
            auto L = StackReader::listing(item.stackPath);
            std::erase_if(L.entries, [](const StackModel::SEntry& e) { return e.dir; });
            StackModel::sortEntries(L.entries, StackModel::ESort::Added);
            if (!L.entries.empty())
                Stacks::trashFrom(item.stackPath, {L.entries.front().path});
            return;
        }
        case ERow::StackEmptyConfirm: {
            std::vector<std::string> paths;
            for (const auto& E : StackReader::listing(item.stackPath).entries)
                paths.push_back(E.path);
            Stacks::trashFrom(item.stackPath, std::move(paths));
            return;
        }
        // In-place views: handled in menuClick, never activated.
        case ERow::StackRename:
        case ERow::StackRenameField:
        case ERow::StackEmptyAsk:
        case ERow::StackEmptyCancel: return;
        case ERow::StackOpen: Locations::activate(item.uri); return;
        case ERow::StackUnpin:
            unpin(IconOverrides::folderId(item.stackPath)); // its place among the favourites, if it had one
            Stacks::unpin(item.stackPath);                  // "Remove from Dock": the folder is untouched
            return;

        case ERow::EmptyTrash: Locations::emptyTrash(); return;
        case ERow::EmptyTrashConfirm: Locations::emptyTrash(); return;
        case ERow::Mount: Locations::mount(item.uri); return;
        case ERow::Unmount: Locations::unmount(item.uri, false); return;
        case ERow::Eject: Locations::unmount(item.uri, true); return;

        case ERow::Action: AppSearch::launchExec(row.exec); return;

        case ERow::Window: {
            const auto WINDOW = row.window.lock();
            if (!WINDOW)
                return;

            // Same as clicking its hover-panel preview: go to that window on
            // its desktop. Inside the overview, commit it as the selection so
            // close() does not overwrite the focus.
            if (const auto OVERVIEW = scrollOverviewForMonitor(monitor); OVERVIEW)
                OVERVIEW->selectAndClose(WINDOW);
            else
                activateWindow(WINDOW);
            return;
        }

        // Get Info and Back swap the menu's rows in place (menuClick); they only
        // reach here if something activates a row without a menu to swap.
        case ERow::Details:
        case ERow::InfoBack: return;

        case ERow::InfoLine:
            if (!row.exec.empty())
                Hyprutils::OS::CProcess("/usr/bin/wl-copy", {row.exec}).runAsync();
            return;

        case ERow::InfoCopyAll: {
            std::string all;
            for (const auto& R : infoRows(item))
                if (R.kind == ERow::InfoLine && !R.exec.empty())
                    all += R.label + ": " + R.exec + "\n";
            if (!all.empty())
                Hyprutils::OS::CProcess("/usr/bin/wl-copy", {all}).runAsync();
            return;
        }

        case ERow::InfoIcon:
            // The Omarchy shell's IPC: open the Hyprdrive panel on Icons with
            // this app chosen (Panel.qml, `icons`). argv, no shell: the id is
            // one argument whatever it contains.
            // The overview first: it owns the keyboard and draws over the
            // shell, so the panel would open unseen and close again at once.
            if (row.exec.empty())
                return;
            if (const auto OVERVIEW = scrollOverviewForMonitor(monitor); OVERVIEW)
                OVERVIEW->close();
            Hyprutils::OS::CProcess("/usr/bin/qs", {"-p", "/usr/share/omarchy/shell", "ipc", "call", "dragos.hyprdrive", "icons", row.exec}).runAsync();
            return;

        case ERow::InfoReveal:
            // In the user's file manager (Locations::reveal).
            if (!row.exec.empty())
                Locations::reveal(row.exec);
            return;

        case ERow::Favorite: toggleFavorite(itemDesktopId(item)); return;
        case ERow::AlwaysEmptyWorkspace: toggleEmptyDesktop(itemDesktopId(item)); return;

        case ERow::ShowWindows: {
            if (!item.uri.empty()) {
                openScrollOverviewWithQuery(monitor, item.locationName);
                return;
            }
            const auto* ENTRY = AppSearch::entryForId(itemDesktopId(item));
            openScrollOverviewWithQuery(monitor, ENTRY && !ENTRY->name.empty() ? ENTRY->name : wmClass);
            return;
        }

        case ERow::Quit:
            for (const auto& WINDOW : menuWindows(item))
                WINDOW->sendClose();
            return;

        case ERow::NewWindow: launchApp(monitor, item, nullptr, true); return;

        case ERow::OpenEmptyWorkspace: launchInEmptyWorkspace(monitor, item); return;

        case ERow::MoveEmptyWorkspace: moveToEmptyWorkspace(monitor, row.window.lock()); return;

        case ERow::MoveHere: moveToThisWorkspace(monitor, row.window.lock()); return;

        case ERow::OpenScratchpad: launchInScratchpad(monitor, item); return;

        case ERow::MoveScratchpad: {
            const auto WINDOW = row.window.lock();
            if (!WINDOW)
                return;
            // Off the minimize stash first, for the reason moveToEmptyWorkspace
            // gives: otherwise a later restore pulls it back out.
            if (Minimize::isMinimized(WINDOW))
                Minimize::restore(WINDOW);
            if (const auto SCRATCH = Scratchpad::ensureDefault(monitor))
                Scratchpad::moveWindowTo(WINDOW, SCRATCH);
            return;
        }

        case ERow::MinimizeAll: {
            ConfigureHold::CHold    oneResize;
            Minimize::CGestureBoxes boxesBeforeAnyMoves(menuWindows(item));
            for (const auto& WINDOW : menuWindows(item))
                if (!Minimize::isMinimized(WINDOW))
                    Flight::minimize(monitor, WINDOW, false);
            return;
        }

        case ERow::RestoreAll: {
            ConfigureHold::CHold oneResize;
            {
                Minimize::CRestoreSlots ownSlots(menuWindows(item));
                for (const auto& WINDOW : menuWindows(item))
                    if (Minimize::isMinimized(WINDOW))
                        Flight::restore(monitor, WINDOW);
            }
            Flight::retargetRestores();
            return;
        }

        case ERow::ForceQuit: {
            // Each process once; never pid 0/1 and never the compositor itself,
            // whatever a client claims its pid is.
            std::unordered_set<pid_t> pids;
            for (const auto& WINDOW : itemWindows(item))
                if (const pid_t PID = WINDOW->getPID(); PID > 1 && PID != getpid())
                    pids.insert(PID);
            for (const pid_t PID : pids)
                kill(PID, SIGKILL);
            return;
        }
    }
}
}

void hotkey(int number, bool shift, bool ctrl) {
    if (!Config::getValue<bool>(KEY_HOT_KEYS))
        return;

    // `_showOverlay()` runs beside `_activateApp`, not after a successful one,
    // and `_activateApp` no-ops past the end of the dash. An empty slot
    // therefore still draws the numbers -- which is how you see it is empty.
    showOverlay();

    const auto MONITOR = Desktop::focusState()->monitor();
    if (!MONITOR)
        return;

    const auto LAYOUT = layout(MONITOR);
    const int  INDEX  = number - 1;
    if (INDEX < 0 || INDEX >= sc<int>(LAYOUT.items.size()))
        return;

    const auto& ITEM = LAYOUT.items[INDEX];

    // The three modifiers are three branches of ONE `AppIcon.activate(button=1)`
    // reading the KEY event's state, not three behaviours of their own -- which
    // is why the reference binds all thirty accelerators to one callback.
    // CONTROL short-circuits to GNOME's own activate (a new window of a running
    // app, a launch of a stopped one); SHIFT picks `shift-click-action`; neither
    // reaches `clickAction`.
    if (ctrl)
        launchApp(MONITOR, ITEM);
    else
        activateItem(MONITOR, ITEM, shift);
}

// V3. The release, resolved. Everything below the ladder is the SAME code X32
// ran; what is new is that a drop which leaves the pill now has somewhere to go
// other than nowhere.
//
// Returns whether the gesture was a DROP. False means "this was a click" and
// the caller runs its click ladder -- which is the X16 recovery X32 wrote, kept
// where it was in the order.
bool dragRelease(PHLMONITOR monitor, const Vector2D& point) {
    if (g_dockDrag.panel)
        return panelDragRelease(monitor, point);

    if (!g_dockDrag.drag)
        return false;

    const auto DRAG = *g_dockDrag.drag;
    g_dockDrag.drag.reset();

    // A STACK being reordered (dock stacks A): only among the stacks, only a
    // live drag that ends on the pill. A release near the press is a click --
    // it opens the stack.
    if (!DRAG.stackPath.empty()) {
        const float CLICKTOL = 3.F * dragThresholdPx(monitor);
        if (!DRAG.live || DRAG.start.distanceSq(point) <= CLICKTOL * CLICKTOL)
            return false;
        // Folders anywhere: among the favourites, by the apps' own reorder.
        if (foldersAnywhere()) {
            if (const int AT = dropIndexAt(monitor, point); AT >= 0)
                pinAt(IconOverrides::folderId(DRAG.stackPath), AT);
            return true;
        }
        const int INDEX = stackDropIndexAt(monitor, point);
        if (INDEX < 0)
            return true; // off the pill: nothing moves (removing is the menu's job)
        // INDEX counts the dragged stack itself; a later slot lands one short otherwise.
        size_t from = 0;
        for (const auto& S : Stacks::all()) {
            if (S.path == DRAG.stackPath)
                break;
            ++from;
        }
        const size_t TO = sc<size_t>(INDEX) > from ? sc<size_t>(INDEX) - 1 : sc<size_t>(INDEX);
        Stacks::moveTo(DRAG.stackPath, TO);
        return true;
    }

    // USER-REPORTED P0 2026-08-06: "I cant use the dock clicking it does
    // nothing". This is X16's lesson arriving on the dock, and it is why the
    // scripted rig could not see it -- that rig jitters 4px, under the 10px
    // getDragThreshold, so it never went down this path at all.
    //
    // A drag goes live the moment the pointer passes that threshold, and a real
    // hand wobbles 10-20px between press and release. The release then landed
    // HERE, reordered the favourite, and returned true -- which swallowed the
    // click, so the app never minimized or launched. To the user that is a dead
    // dock, and Y4 made it worse by making the click the primary gesture.
    //
    // A release that lands near where the press did is a CLICK, whatever the
    // pointer did in between. Same 3x tolerance X16 settled on for window tiles.
    const float CLICKTOL = 3.F * dragThresholdPx(monitor);

    // IN TWO STAGES, for the reason DropDecision.hpp gives for the window
    // drop's own ladder: the gathering is not free, and resolveIcon is total at
    // every stage. Stage one is what the dock already knew, and the two
    // conclusions it can reach on its own -- a click and a reorder -- are the
    // COMMON path. Every dock click in the session comes through here, so
    // walking the overview's strip and rebuilding the item model to answer
    // "was that a click" would be a cost paid by the gesture that needs none
    // of it.
    const int        INDEX = dropIndexAt(monitor, point);
    Drop::SIconFacts facts{
        .haveItem  = !DRAG.id.empty(),
        .live      = DRAG.live,
        .nearStart = DRAG.start.distanceSq(point) <= CLICKTOL * CLICKTOL,
        .onPill    = INDEX >= 0,
    };

    switch (Drop::resolveIcon(facts)) {
        case Drop::EIconAction::Click: return false;

        // X32's reorder, unchanged: a favourite moved to the slot the caret drew.
        case Drop::EIconAction::Reorder: pinAt(DRAG.id, INDEX); return true;

        default: break;
    }

    // Stage two: the overview, and where in it the button came up.
    const auto OVERVIEW = openScrollOverviewForMonitor(monitor);
    const auto STRIPWS  = OVERVIEW ? OVERVIEW->stripWorkspaceAt(point) : PHLWORKSPACE{};
    const auto ACTIVEWS = monitor ? monitor->m_activeWorkspace : PHLWORKSPACE{};

    facts.overviewOpen      = OVERVIEW != nullptr;
    facts.onStripCard       = STRIPWS != nullptr;
    facts.stripCardIsActive = STRIPWS && STRIPWS == ACTIVEWS;

    // The dragged item, found again by the id the press recorded. The layout has
    // reflowed since (an app can have opened or closed mid-drag), so this is a
    // fresh lookup and not the SItem copy the press held -- what is carried over
    // from the press is the drag's own memory: the icon and the cell it left.
    const auto LAYOUT = layout(monitor);
    const auto ITEM   = std::ranges::find_if(LAYOUT.items, [&](const SItem& item) { return item.desktopId == DRAG.id; });

    switch (Drop::resolveIcon(facts)) {
        // V3 (spec V3): "anywhere on the active desktop -> launch on the active
        // workspace". Focused, and the overview (if this was its own card) closes
        // behind it -- it is where the user is, and they have just said so twice.
        case Drop::EIconAction::LaunchHere:
            if (ITEM != LAYOUT.items.end())
                launchApp(monitor, *ITEM, nullptr, true);
            return true;

        // "...onto any workspace card in the overview strip -> launch on THAT
        // workspace". SILENT, because the user is looking at a strip of desktops
        // and pointed at one that is not this one: stealing the session to it
        // would be answering a different question.
        case Drop::EIconAction::LaunchThere:
            if (ITEM != LAYOUT.items.end())
                launchApp(monitor, *ITEM, STRIPWS, true);
            return true;

        // Missed everything. X26's near miss, in the other direction: the icon
        // goes home rather than being lost or silently unpinned.
        default:
            if (!DRAG.icon.empty())
                g_dockDrag.ghostHome = SDockGhostFlight{.icon      = DRAG.icon,
                                                        .monitor   = DRAG.monitor,
                                                        .from      = ghostBoxAt(monitor, DRAG.home, DRAG.grab, point),
                                                        .home      = ITEM != LAYOUT.items.end() ? ITEM->iconBox : DRAG.home,
                                                        .startedAt = Time::steadyNow()};
            return true;
    }
}

bool debugActivate(PHLMONITOR monitor, const std::string& wmClass) {
    if (!monitor)
        return false;
    // The base layout: an autohidden dock's live layout has no items at all.
    const auto& LAYOUT = baseLayout(monitor);
    for (const auto& item : LAYOUT.items) {
        // A pinned app that is not running has no class yet: its desktop id.
        if (item.wmClass == wmClass || (!wmClass.empty() && (item.desktopId == wmClass || item.desktopId == wmClass + ".desktop"))) {
            activateItem(monitor, item, false);
            return true;
        }
    }
    return false;
}
}
