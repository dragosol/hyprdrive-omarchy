#include "IOverview.hpp"
#include "globals.hpp"

#include <algorithm>

#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/managers/EventManager.hpp>

#include "Compat.hpp"
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/output/Monitor.hpp>

static std::vector<SP<IOverview>> g_scrollOverviews;

// The most recently registered overview, and the third of activeScrollOverview's
// four tiers. It used to be `g_pScrollOverview`, an inline mutable global in
// IOverview.hpp that any translation unit could assign to -- and three did, one
// of them saving and restoring it around every render. Nothing reachable from
// render() ever read it (the only readers are this file and the keyboard hook,
// and onPreRender runs outside that window), so the render-time override was
// protecting a question nobody asked from inside the answer. It is a file
// static now: same tier, same value, no public seat.
static SP<IOverview> g_lastRegistered;

// Tells the shell (quickshell ii) to reveal bar + dock while the overview is up.
// ponytail: one bool over the existing event socket, no new IPC channel.
static void emitOverviewState() {
    static bool lastOpen = false;
    const bool  OPEN     = !g_scrollOverviews.empty();
    if (OPEN == lastOpen || !g_pEventManager)
        return;
    lastOpen = OPEN;
    g_pEventManager->postEvent(SHyprIPCEvent{.event = ScrollOverview::Compat::NAME, .data = OPEN ? "1" : "0"});
    // ONE RELEASE. A shell still listening for the pre-rename event name keeps
    // revealing itself; the plugin does not get to decide when the user's
    // quickshell config is updated.
    g_pEventManager->postEvent(SHyprIPCEvent{.event = ScrollOverview::Compat::LEGACY_NAME, .data = OPEN ? "1" : "0"});
}

const std::vector<SP<IOverview>>& scrollOverviews() {
    return g_scrollOverviews;
}

SP<IOverview> scrollOverviewForMonitor(PHLMONITOR monitor) {
    if (!monitor)
        return {};

    const auto IT = std::ranges::find_if(g_scrollOverviews, [&monitor](const auto& overview) {
        return overview && overview->pMonitor.lock() == monitor;
    });
    return IT == g_scrollOverviews.end() ? SP<IOverview>{} : *IT;
}

// The registry serves overviews in every phase; a closing one is a zombie
// (Z18). This is the filtered query -- exists AND not closing -- that consumers
// want instead of re-deriving the predicate. Lives here, beside the registry.
SP<IOverview> openScrollOverviewForMonitor(PHLMONITOR monitor) {
    const auto OVERVIEW = scrollOverviewForMonitor(monitor);
    return (OVERVIEW && !OVERVIEW->isClosing()) ? OVERVIEW : SP<IOverview>{};
}

SP<IOverview> scrollOverviewAt(const Vector2D& point) {
    const auto IT = std::ranges::find_if(g_scrollOverviews, [&point](const auto& overview) {
        const auto monitor = overview ? overview->pMonitor.lock() : PHLMONITOR{};
        return monitor && monitor->logicalBox().containsPoint(point);
    });
    return IT == g_scrollOverviews.end() ? SP<IOverview>{} : *IT;
}

// Tab's keyboard owner, and where the pointer was when Tab chose it: the
// pointer moving is the user reaching for another monitor, and then the pointer
// rules again, exactly as before.
static WP<IOverview> g_keyOwner;
static Vector2D      g_keyOwnerMouse;

void setKeyboardOverview(const SP<IOverview>& overview) {
    g_keyOwner      = overview;
    g_keyOwnerMouse = g_pInputManager ? g_pInputManager->getMouseCoordsInternal() : Vector2D{};
}

SP<IOverview> activeScrollOverview() {
    if (const auto OWNER = g_keyOwner.lock()) {
        if (g_pInputManager && g_pInputManager->getMouseCoordsInternal().distanceSq(g_keyOwnerMouse) < 1.0 &&
            std::ranges::find(g_scrollOverviews, OWNER) != g_scrollOverviews.end())
            return OWNER;
        g_keyOwner.reset();
    }
    if (g_pInputManager) {
        if (const auto overview = scrollOverviewAt(g_pInputManager->getMouseCoordsInternal()))
            return overview;
    }

    if (const auto monitor = Desktop::focusState()->monitor()) {
        if (const auto overview = scrollOverviewForMonitor(monitor))
            return overview;
    }

    if (g_lastRegistered && std::ranges::find(g_scrollOverviews, g_lastRegistered) != g_scrollOverviews.end())
        return g_lastRegistered;

    return g_scrollOverviews.empty() ? SP<IOverview>{} : g_scrollOverviews.front();
}

void registerScrollOverview(const SP<IOverview>& overview) {
    if (!overview || std::ranges::find(g_scrollOverviews, overview) != g_scrollOverviews.end())
        return;

    g_scrollOverviews.emplace_back(overview);
    g_lastRegistered = overview;
    emitOverviewState();
}

// Overviews die from inside their own close animation: the last thing close() does
// is scale->setCallbackOnEnd([this]{ removeOverview(this); }). This vector holds the
// only strong reference, so erasing it there ran ~CScrollOverview *while the animation
// manager was standing in that variable's callback* -- destroying `scale` out from
// under its own caller. The manager then walked freed memory on the next event, which
// is why the crash landed on a later mouse move and not on the keystroke that closed
// the overview: "segfault at X ip X", a call through a dangling function pointer.
//
// ponytail: park the reference and let the event loop drop it one tick later. Nothing
// else about teardown has to move, and it fixes every caller rather than the one that
// happened to reproduce. Same doLater the dock already uses.
static std::vector<SP<IOverview>> g_dyingOverviews;

static uint64_t g_dyingOverviewsSeq = 0;

void unregisterScrollOverview(IOverview* overview) {
    if (!overview)
        return;

    for (const auto& candidate : g_scrollOverviews)
        if (candidate.get() == overview)
            g_dyingOverviews.emplace_back(candidate);

    std::erase_if(g_scrollOverviews, [overview](const auto& candidate) { return candidate.get() == overview; });
    g_lastRegistered = activeScrollOverview();
    emitOverviewState();

    if (!g_dyingOverviews.empty() && g_pEventLoopManager) {
        if (g_dyingOverviewsSeq)
            g_pEventLoopManager->removeDoLater(g_dyingOverviewsSeq);
        g_dyingOverviewsSeq = g_pEventLoopManager->doLater([] {
            g_dyingOverviewsSeq = 0;
            g_dyingOverviews.clear();
        });
    }
}

// P0. See the declaration in IOverview.hpp for the defect. The teardown goes
// through unregisterScrollOverview -- the park above -- and NOT through a direct
// erase, because this runs inside the monitor.removed emit: dropping the last
// reference here would run ~CScrollOverview, destroying animated variables and
// event listeners, from inside a signal callback. That is the same shape as the
// close-animation crash the park was added for, so it gets the same answer, and
// an overview whose monitor died mid-open needs it most: `scale` is still live.
//
// No close ANIMATION, deliberately: the animation is a thing drawn on an output,
// and the output is what just went away.
void dropScrollOverviewsForMonitor(PHLMONITOR monitor) {
    if (!monitor)
        return;

    // Collected first: unregisterScrollOverview mutates the vector being walked.
    std::vector<IOverview*> doomed;
    for (const auto& overview : g_scrollOverviews) {
        if (!overview)
            continue;
        const auto OVERVIEWMONITOR = overview->pMonitor.lock();
        if (!OVERVIEWMONITOR || OVERVIEWMONITOR == monitor)
            doomed.emplace_back(overview.get());
    }

    if (doomed.empty())
        return;

    for (auto* const overview : doomed)
        unregisterScrollOverview(overview);

    // The render hooks go on with the first overview and have to come off with
    // the last -- removeOverview()'s tail, minus the damage: the monitor this
    // ran for is the one leaving.
    if (g_scrollOverviews.empty())
        disableScrollOverviewHooks();
}

void clearScrollOverviews() {
    if (g_dyingOverviewsSeq && g_pEventLoopManager) {
        g_pEventLoopManager->removeDoLater(g_dyingOverviewsSeq);
        g_dyingOverviewsSeq = 0;
    }
    g_dyingOverviews.clear();
    auto overviews = std::move(g_scrollOverviews);
    g_scrollOverviews.clear();
    g_lastRegistered.reset();
    emitOverviewState();
    overviews.clear();
}
