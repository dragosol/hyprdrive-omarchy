#pragma once

// DROPPING INTO THE DOCK (dock stacks B, docs/superpowers/specs/2026-09-24-dock-stacks-b-design.md).
//
// The dock is not a Wayland surface, so another app's drag cannot target it
// through the protocol. Instead, while a drag hovers the dock, this reads the
// drag source's text/uri-list (the source is the private
// CWLDataDeviceProtocol::m_dnd.currentSource; IDataSource::send is public) on
// the event loop -- and on release over the dock it takes the drop: acts on the
// files itself (StackReader's worker) and CANCELS the drag, so the source never
// thinks it was moved and the window under the dock never receives it.
// Proven in a spike before this was built (the spec's "spike" section).

#include "globals.hpp"

#include <string>
#include <vector>

namespace ScrollOverview::DockDrop {

    // Pointer motion on the plain desktop (null monitor: off every monitor).
    void onPointerMove(PHLMONITOR monitor, const Vector2D& local);
    // A button on the plain desktop; true = the drop was ours (cancel the event).
    bool onButton(PHLMONITOR monitor, const Vector2D& local, bool pressed);

    // The Moses gap: while ONE folder hovers between cells, a placeholder for it
    // is inserted among the stacks at this index. -1 = none.
    int                placeholderIndex();
    const std::string& placeholderPath();
    uint64_t           placeholderGeneration(); // for the dock's item cache key

    // What the drag is over, for the dock's hover ring and lift.
    const std::string& hoveredStack(); // "" = none
    bool               hoveringTrash();

    // Start a real Wayland drag of `paths` (a fan item dragged out), as the
    // plugin's own data source. False if another drag is running.
    bool startFileDrag(const std::vector<std::string>& paths, const std::string& icon);
    bool draggingOut();
    // Any Wayland drag-and-drop in flight (ours or a client's): pointer motion
    // must keep reaching Hyprland then, it moves the drag target.
    bool dndInProgress();
    // The drag's icon under the pointer (RENDER_POST_WINDOWS).
    void renderGhost(PHLMONITOR monitor);

    void shutdown();
}
