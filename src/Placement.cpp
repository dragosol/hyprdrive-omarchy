#include "Placement.hpp"

#include <algorithm>
#include <optional>

#define private public
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/layout/algorithm/Algorithm.hpp>
#include <hyprland/src/layout/algorithm/tiled/scrolling/ScrollingAlgorithm.hpp>
#include <hyprland/src/layout/space/Space.hpp>
#include <hyprland/src/layout/target/Target.hpp>
#undef private

namespace ScrollOverview::Placement {

namespace {

PHLWINDOWREF g_dragAnchor;

bool tiledOn(const PHLWINDOW& window, const PHLWORKSPACE& workspace, const PHLWINDOW& except) {
    return window && window != except && Desktop::View::validMapped(window) && !window->m_isFloating && window->m_workspace == workspace &&
        window->layoutTarget();
}

PHLWINDOW anchorFor(const PHLWINDOW& window, const PHLWINDOW& preferred) {
    const auto WORKSPACE = window->m_workspace;

    if (tiledOn(preferred, WORKSPACE, window))
        return preferred;

    // The window last focused ON THIS DESKTOP -- the one a drop from elsewhere
    // most plausibly belongs beside, when the drag started somewhere else.
    if (const auto LAST = WORKSPACE->getLastFocusedWindow(); tiledOn(LAST, WORKSPACE, window))
        return LAST;

    for (const auto& CANDIDATE : Desktop::windowState()->windows()) {
        if (tiledOn(CANDIDATE, WORKSPACE, window))
            return CANDIDATE;
    }

    return nullptr;
}

}

void noteDragAnchor(const PHLWINDOW& window) {
    g_dragAnchor = window;
}

PHLWINDOW takeDragAnchor() {
    const auto WINDOW = g_dragAnchor.lock();
    g_dragAnchor.reset();
    return WINDOW;
}

void retileAsNew(const PHLWINDOW& window, const PHLWINDOW& preferredAnchor) {
    if (!window || !Desktop::View::validMapped(window) || window->m_isFloating)
        return;

    const auto WORKSPACE = window->m_workspace;
    const auto TARGET    = window->layoutTarget();
    if (!WORKSPACE || WORKSPACE->m_isSpecialWorkspace || !TARGET || !WORKSPACE->m_space || !WORKSPACE->m_space->algorithm())
        return;

    const auto ALGO = WORKSPACE->m_space->algorithm();

    // A scrolling workspace has no dwindle to hand the decision to; its drop
    // placements (tape edge, next to the anchor along the tape) stay its own.
    if (dc<Layout::Tiled::CScrollingAlgorithm*>(ALGO->m_tiled.get()))
        return;

    // Only a target the tiled algorithm actually holds can be taken out of it --
    // removeTarget logs a Hyprland BUG line for anything else.
    if (!std::ranges::contains(ALGO->m_tiledTargets, TARGET))
        return;

    std::optional<Vector2D> focal;
    if (const auto ANCHOR = anchorFor(window, preferredAnchor)) {
        const auto BOX = ANCHOR->layoutTarget()->position();
        focal          = BOX.pos() + BOX.size() / 2.0;
    }

    // With no anchor the window is alone on the desktop, and a plain re-add
    // (no focal point) fills it -- nothing there for the cursor to influence.
    ALGO->removeTarget(TARGET);
    ALGO->moveTarget(TARGET, focal, false);
    WORKSPACE->m_space->recalculate();
}

}
