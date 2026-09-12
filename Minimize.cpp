#include "Minimize.hpp"

#include <algorithm>
#include <sstream>
#include <string>

#define private public
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/state/GlobalWindowController.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/layout/target/Target.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/state/WorkspaceState.hpp>
#undef private

namespace ScrollOverview::Minimize {

namespace {

struct SEntry {
    PHLWINDOWREF window;
    WORKSPACEID  workspace = WORKSPACE_INVALID;
    std::string  workspaceName;
    MONITORID    monitor = MONITOR_INVALID;
    CBox         box;
    bool         floating = false;
};

std::vector<SEntry> g_entries;

// The workspace tracker holds only weak refs (see ensureNoWorkspaceGaps), so
// this strong ref is what keeps the stash alive while it is empty.
PHLWORKSPACE g_stash;

constexpr const char* STASH_NAME = "special:minimized";

// A special workspace that is never shown keeps its windows MAPPED, which is
// the whole reason the spec picks one: an unmapped window cannot be previewed
// live (X25).
PHLWORKSPACE stash(PHLMONITOR monitor) {
    if (g_stash)
        return g_stash;

    for (const auto& w : State::workspaceState()->workspaces()) {
        const auto WORKSPACE = w.lock();
        if (WORKSPACE && WORKSPACE->m_name == STASH_NAME) {
            g_stash = WORKSPACE;
            return g_stash;
        }
    }

    if (!monitor)
        return nullptr;

    g_stash = State::workspaceState()->create(State::workspaceState()->newSpecialID(), monitor->m_id, STASH_NAME);
    return g_stash;
}

PHLWORKSPACE workspaceByID(WORKSPACEID id) {
    for (const auto& w : State::workspaceState()->workspaces()) {
        const auto WORKSPACE = w.lock();
        if (WORKSPACE && WORKSPACE->m_id == id)
            return WORKSPACE;
    }

    return nullptr;
}

std::vector<SEntry>::iterator entryFor(PHLWINDOW window) {
    return std::ranges::find_if(g_entries, [&window](const auto& e) { return e.window.lock() == window; });
}

void dropDeadEntries() {
    std::erase_if(g_entries, [](const auto& e) { return !e.window.lock(); });
}

}

bool isStash(const PHLWORKSPACE& workspace) {
    return workspace && workspace->m_name == STASH_NAME;
}

void adoptStashedWindows() {
    dropDeadEntries();

    for (const auto& WINDOW : Desktop::windowState()->windows()) {
        if (!WINDOW || !WINDOW->m_isMapped || !isStash(WINDOW->m_workspace) || entryFor(WINDOW) != g_entries.end())
            continue;

        // What it was minimized FROM died with the old ledger, so there is no
        // honest answer -- the desktop its monitor is showing is the sane home,
        // and TILED the sane state: the window comes back into the layout rather
        // than floating over it. Its current box is kept for the floating case,
        // where restore reads it.
        const auto MONITOR = WINDOW->m_monitor.lock();
        const auto HOME    = MONITOR ? MONITOR->m_activeWorkspace : nullptr;
        if (!HOME || HOME->m_isSpecialWorkspace)
            continue;

        const auto TARGET = WINDOW->layoutTarget();
        g_entries.emplace_back(SEntry{WINDOW, HOME->m_id, HOME->m_name, MONITOR->m_id, TARGET ? TARGET->position() : CBox{}, false});
    }
}

bool isMinimized(PHLWINDOW window) {
    return window && entryFor(window) != g_entries.end();
}

WORKSPACEID originalWorkspace(PHLWINDOW window) {
    if (!window)
        return WORKSPACE_INVALID;

    const auto IT = entryFor(window);
    return IT == g_entries.end() ? WORKSPACE_INVALID : IT->workspace;
}

std::vector<PHLWINDOW> minimized() {
    dropDeadEntries();

    std::vector<PHLWINDOW> out;
    for (const auto& e : g_entries)
        out.emplace_back(e.window.lock());

    return out;
}

bool minimize(PHLWINDOW window, bool handoffFocus) {
    dropDeadEntries();
    Membership::bump("minimize");

    if (!window || isMinimized(window))
        return false;

    const auto WORKSPACE = window->m_workspace;
    const auto TARGET    = window->layoutTarget();
    if (!WORKSPACE || !TARGET || WORKSPACE->m_isSpecialWorkspace)
        return false;

    const auto STASHWS = stash(window->m_monitor.lock());
    if (!STASHWS)
        return false;

    // X41/X42: a workspace move is exactly where geometry gets destroyed
    // silently, so capture the real box first and put it back on restore.
    g_entries.emplace_back(SEntry{window, WORKSPACE->m_id, WORKSPACE->m_name, WORKSPACE->m_monitor ? WORKSPACE->m_monitor->m_id : MONITOR_INVALID,
                                  TARGET->position(), window->m_isFloating});

    const CBox STASHBOX = TARGET->position();

    Desktop::globalWindowController()->moveWindowToWorkspace(window, STASHWS);

    // THE STASH IS A TILED WORKSPACE, and that is visible to the user: every new
    // minimize retiles everything already stashed, so each window there shrinks
    // as more are added -- and the dock preview, which renders the REAL window,
    // shrinks with it. A preview that changes size because of something that
    // happened to an unrelated window is indefensible.
    //
    // So a stashed window floats and is pinned to the box it had the instant
    // before it was minimized. Floating takes it out of the stash's layout
    // entirely, which is what stops the geometry depending on how many
    // neighbours it has; the pinned box is what makes a TILED window keep the
    // size it was tiled at rather than collapsing to some floating default.
    //
    // The ledger entry above still records what it REALLY was, so restore puts
    // the tiled ones back as tiled -- this float is confined to the stash.
    if (const auto STASHTARGET = window->layoutTarget()) {
        STASHTARGET->setFloating(true);
        if (!STASHBOX.empty()) {
            // rememberFloatingSize BEFORE the position write, and not instead of
            // it: the target carries a remembered floating size from whenever it
            // last floated, and a later layout pass will happily reassert that
            // over a box we merely set once. Measured: a stashed window pinned
            // at 1420x854 was found at 710x427 a few actions later -- exactly
            // half, i.e. the remembered size winning. Setting both means the
            // pin and the memory agree and there is nothing left to drift to.
            STASHTARGET->rememberFloatingSize(STASHBOX.size());
            STASHTARGET->setPositionGlobal(STASHBOX);
        }
    }

    // A stashed window must not keep the keyboard. Measured 2026-08-03 by X33's
    // rig: minimize left focus on the window it had just taken off screen, so
    // typing went to something invisible and the dock read the app as still
    // focused -- which is the test `cycle-windows` branches on. Hand the focus
    // to whatever is left on the workspace it came from.
    //
    // A dock FLIGHT defers this (handoffFocus=false): it keeps the window
    // focused so the flying proxy renders bright, and calls handoffMinimizedFocus
    // when the flight lands. The origin workspace is captured in the ledger
    // entry above, so the landing can find it even though the window is stashed.
    if (handoffFocus && Desktop::focusState()->isWindowActive(window)) {
        PHLWINDOW next;
        for (const auto& CANDIDATE : Desktop::windowState()->windows()) {
            if (CANDIDATE != window && CANDIDATE->m_isMapped && CANDIDATE->m_workspace == WORKSPACE)
                next = CANDIDATE;
        }

        if (next)
            Desktop::focusState()->fullWindowFocus(next, Desktop::FOCUS_REASON_OTHER);
        else
            Desktop::focusState()->resetWindowFocus();
    }

    return true;
}

void handoffMinimizedFocus(PHLWINDOW window) {
    if (!window || !Desktop::focusState()->isWindowActive(window))
        return;

    // The window is stashed now; its origin workspace is the ledger's, the same
    // one minimize() would have handed off to. Reap-safe: if that workspace is
    // gone (last window minimized), nothing there to focus -> reset.
    const auto ORIGIN = originalWorkspace(window);
    PHLWINDOW  next;
    if (ORIGIN != WORKSPACE_INVALID) {
        for (const auto& CANDIDATE : Desktop::windowState()->windows()) {
            if (CANDIDATE != window && CANDIDATE->m_isMapped && CANDIDATE->m_workspace && CANDIDATE->m_workspace->m_id == ORIGIN)
                next = CANDIDATE;
        }
    }

    if (next)
        Desktop::focusState()->fullWindowFocus(next, Desktop::FOCUS_REASON_OTHER);
    else
        Desktop::focusState()->resetWindowFocus();
}

CBox originalBox(PHLWINDOW window) {
    const auto IT = entryFor(window);
    return IT == g_entries.end() ? CBox{} : IT->box;
}

bool showOriginalWorkspace(PHLWINDOW window) {
    const auto IT = entryFor(window);
    if (IT == g_entries.end())
        return false;

    auto WORKSPACE = workspaceByID(IT->workspace);
    if (!WORKSPACE)
        WORKSPACE = State::workspaceState()->create(IT->workspace, IT->monitor, IT->workspaceName);
    if (!WORKSPACE)
        return false;

    if (const auto MONITOR = WORKSPACE->m_monitor.lock(); MONITOR && MONITOR->m_activeWorkspace != WORKSPACE)
        MONITOR->changeWorkspace(WORKSPACE, false, true, true);

    return true;
}

bool restore(PHLWINDOW window) {
    dropDeadEntries();
    Membership::bump("restore");

    if (!window)
        return false;

    const auto IT = entryFor(window);
    if (IT == g_entries.end())
        return false;

    const auto ENTRY = *IT;
    g_entries.erase(IT);

    // Minimizing the last window of a workspace lets Hyprland reap it the
    // moment the user looks away, and without this the window is stranded on
    // the stash with no way back. Measured: leave the workspace and it is gone.
    auto WORKSPACE = workspaceByID(ENTRY.workspace);
    if (!WORKSPACE)
        WORKSPACE = State::workspaceState()->create(ENTRY.workspace, ENTRY.monitor, ENTRY.workspaceName);

    if (!WORKSPACE)
        return false;

    Desktop::globalWindowController()->moveWindowToWorkspace(window, WORKSPACE);

    // ponytail: only floating windows own their box. A tiled window's geometry
    // belongs to the layout, and writing it back would fight the retile.
    if (const auto TARGET = window->layoutTarget(); TARGET) {
        // Undo the stash's float -- through the LAYOUT MANAGER, not the target.
        //
        // ITarget::setFloating is a bare flag write (CWindowTarget::setFloating
        // in v0.56.2 sets m_isFloating, clears layout flags, unpins, and nothing
        // else); it never tells the workspace's layout space. The window arrived
        // here still floating -- the stash floats everything -- so the move above
        // filed it in this workspace's FLOATING algorithm, and flipping the flag
        // afterwards left dwindle never knowing it existed: zen came back at full
        // size on top of or behind foot instead of the tiling opening a slot
        // (user, 2026-09-12). Target.cpp's own swap has to follow setFloating
        // with onUpdateSpace() for exactly this reason.
        //
        // changeFloatingMode -> CSpace::toggleTargetFloating does the real work:
        // the algorithm re-files the target, then recalculate() retiles. It is a
        // TOGGLE, hence the comparison -- a floating restore is already floating
        // and must not be flipped. An earlier attempt here called
        // workspace->updateWindows(), which is only the side step inside that
        // toggle and retiles nothing.
        if (TARGET->floating() != ENTRY.floating && TARGET->space())
            g_layoutManager->changeFloatingMode(TARGET);

        if (ENTRY.floating && !ENTRY.box.empty())
            TARGET->setPositionGlobal(ENTRY.box);

        // Z4: warp for TILED restores too. The re-entered window otherwise
        // draws its first frames easing from a transient pre-retile box
        // (measured: 30k of 427k px) -- but the flight already animated the
        // return, so the real window must APPEAR at its place, not animate
        // to it. GNOME's restore does not replay an open animation either.
        TARGET->warpPositionSize();
    }

    if (const auto MONITOR = WORKSPACE->m_monitor.lock(); MONITOR && MONITOR->m_activeWorkspace != WORKSPACE)
        MONITOR->changeWorkspace(WORKSPACE, false, true, true);

    // X43: focus and stacking are two different things. A restored window that
    // is merely focused can come back underneath whatever was on top of it.
    Desktop::windowState()->raise(window);
    Desktop::focusState()->fullWindowFocus(window, Desktop::FOCUS_REASON_KEYBIND);

    return true;
}

}
