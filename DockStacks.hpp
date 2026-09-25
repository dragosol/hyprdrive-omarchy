#pragma once

#include "globals.hpp"
#include "Dock.hpp"
#include "StackModel.hpp"

#include <hyprland/src/render/Texture.hpp>

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

// V11 SPEC V4. DOCK STACKS -- a file or folder pinned to the dock as a
// first-class cell beside the app icons, opening into a FAN or a GRID of its
// contents. The concept is dragosol/dock-stacks (a GNOME Shell extension staged
// at /mnt/host/v11-ref/); none of its code is here, and several of its choices
// are deliberately not followed. Those are marked DEVIATION below.
//
// WHY A FILE OF ITS OWN. Dock.cpp is 188 KB and every one of its plates,
// menus and hit-tests is already threaded through it. A stack popup is a
// second plate with its own contents, its own hit rects and its own state
// file; put inline it would add a third meaning to a dozen existing branches.
// The Makefile globs *.cpp, so this file needs no build change -- and the
// symcheck rule applies with full force: everything declared here is defined
// OUTSIDE any anonymous namespace in DockStacks.cpp, or the first click on a
// stack is a lazy-bind SIGSEGV in the compositor.
//
// THE PIN ROUTE, AND WHY IT IS NOT A DRAG FROM NAUTILUS. Investigated against
// the Hyprland 0.56 headers this plugin builds on, not assumed:
//
//   * the drop target of an XDG drag is ALWAYS a wl_surface --
//     CSeatManager::m_state.dndPointerFocus is a WP<CWLSurfaceResource>, and
//     the routing that fills it (CWLDataDeviceProtocol::updateDrag) sends
//     enter/motion/drop to a CWLSurfaceResource and nothing else;
//   * this dock is not a surface. It is drawn straight into the monitor's
//     render pass. There is no object the compositor could focus;
//   * and every member that would let a non-surface claim an offer --
//     m_dnd.currentSource, initiateDrag, updateDrag, dropDrag -- is PRIVATE on
//     CWLDataDeviceProtocol, with `friend` granted only to CSeatManager and
//     the protocol's own resource classes. A plugin is neither.
//
// What IS public is renderDND(), dndActive() and abortDndIfPresent(): enough
// to know a foreign drag exists, not enough to receive one. So a file dragged
// out of Nautilus cannot reach this dock, and no amount of plugin hooking
// changes that in 0.56. The honest routes, which is what is implemented:
//
//   * `hyprdrive:stack pin <path>` / `unpin` / `toggle` / `view` / `sort`
//     dispatchers -- scriptable, and what the gates drive;
//   * a Nautilus script dropped in ~/.local/share/nautilus/scripts, so the pin
//     is a right-click away in the file manager's own UI;
//   * and for MOVING a file into a stack, the gesture that CAN reach us: drag
//     an item out of an open fan/grid and onto another stack's cell. That
//     drag begins inside the plugin's own input machinery, so it is ours end
//     to end. Same gio semantics, same bounce, same shake.
namespace ScrollOverview::Stacks {

    // EVERY box handed to the render pass goes through this (2026-09-24: a
    // stack cell animating IN from zero size gave the pile outline a negative
    // box, and Hyprland's renderBorder RASSERTs width/height > 0 -- it aborted
    // the compositor). Rects and textures share the assert family; none of
    // them is worth drawing below 2 px anyway.
    inline bool drawableBox(const CBox& b) {
        return std::isfinite(b.x) && std::isfinite(b.y) && std::isfinite(b.width) && std::isfinite(b.height) && b.width >= 2.0 && b.height >= 2.0;
    }


    struct SStack {
        std::string          path; // absolute, no trailing slash
        StackModel::ESort    sort    = StackModel::ESort::Name;
        StackModel::EDisplay display = StackModel::EDisplay::Stack;
        StackModel::EView    view    = StackModel::EView::Auto;
        std::string          customLabel; // the dock label the user chose; "" = the basename

        // WHAT THE CELL DRAWS, resolved ONCE when the store is read rather than
        // on every build of the dock's item list (user, 2026-09-14): iconFor()
        // stats the path and asks gio to guess a content type.
        std::string icon;          // themed icon NAME for the cell
        std::string label;         // customLabel, or the basename
        bool        isDir = false; // a folder fans; a file just opens
    };

    // The store, cached and poll-refreshed exactly as favourites() is -- the
    // dock asks this once per frame through dockItems(), and the file changes
    // about as often as a user pins something.
    const std::vector<SStack>& all();

    // Mutators. Each writes the store. `pin` refuses a path that does not
    // exist and one that is already pinned; both return whether anything
    // changed, which is what the dispatcher reports.
    bool pin(const std::string& path);
    bool unpin(const std::string& path);
    bool setSort(const std::string& path, StackModel::ESort sort);
    bool setDisplay(const std::string& path, StackModel::EDisplay display);
    bool setViewMode(const std::string& path, StackModel::EView view);
    bool setLabel(const std::string& path, const std::string& label); // dock label only; "" = basename
    bool moveTo(const std::string& path, size_t index);                // reorder among stacks
    void clearAll(); // gate_slate: no stacks means no shifted cell predictions

    // What the dock draws in the cell, and what the tooltip says.
    std::string iconFor(const std::string& path);
    std::string labelFor(const std::string& path);

    // THE STORE, AS A NUMBER. The dock memoizes the item list it builds out of
    // all() and needs one cheap thing to key it on (user, 2026-09-14). Moves
    // when the store is re-read into something different, and when this process
    // writes it.
    uint64_t    generation();

    // ---- the popup -------------------------------------------------------
    // Opened by a click on a stack cell, and modelled on the Y1 hover panel:
    // one frosted plate per row, grown out of the icon it belongs to, hit-
    // tested against the box that was actually DRAWN last frame.
    // A StackReader job landed (main thread, from the wake fd or inotify):
    // refresh what the popup and the cells show, and damage what changed.
    void onReaderReady();
    // Plugin unload: the inotify fd source, the popup, a pending item drag.
    void shutdown();

    void open(PHLMONITOR monitor, const Dock::SItem& item, const CBox& anchor);
    void close();
    // Drop a pending item drag on the floor (the overview closing under a held
    // button, a monitor going away). Deliberately not a release: nothing moves.
    void abandonItemDrag();
    bool isOpen();
    const std::string& openPath();
    // Which output the popup is on, or MONITOR_INVALID. The dock's frame pump
    // needs it to decide how much of a monitor one of its animations has to
    // damage (Dock.cpp, dockDamageRegion).
    MONITORID popupMonitor();

    // Does the popup hold the dock out? (autohide, exactly as panelHoldsDock.)
    bool holdsDock(PHLMONITOR monitor);
    // Is a monitor-local RENDER px point over the popup?
    bool contains(PHLMONITOR monitor, const Vector2D& point);
    // A press or release over the popup. Returns whether it was claimed.
    bool click(PHLMONITOR monitor, const Vector2D& point, bool release);
    // A wheel tick over the popup: the grid scrolls, the fan swallows.
    bool scroll(PHLMONITOR monitor, const Vector2D& point, bool up);

    // ---- V11 SPEC V5, the spacebar preview --------------------------------
    // The fan/grid ITEM under the pointer right now, or empty -- read off the
    // boxes DRAWN last frame, exactly as contains() and click() do, so the
    // preview and the click can never disagree about what is hovered. The
    // action row (Open in Files, N more...) is not an item and answers
    // empty.
    std::string hoveredItemPath();

    // Toggle a gnome-sushi preview of that item, and report whether SPACE was
    // CLAIMED. False whenever there is no popup or nothing under the pointer,
    // and that is the ENTIRE scope of the claim: everywhere else -- every app,
    // the bare desktop, the overview's own search field -- the key falls
    // through untouched. A key that ate space anywhere else would be a
    // regression far larger than the feature.
    bool previewHovered();

    // Dock stacks A. While a fan or pane is open it owns its keys: arrows,
    // Enter, Esc, Space (and, the pane, typing). handleKey takes a PRESS and
    // says whether it claimed it; Space is claimed only when previewHovered()
    // would preview something, so it never goes missing elsewhere.
    bool keyboardActive();
    bool handleKey(uint32_t keycode);
    bool sushiAvailable();
    // Frames this MONITOR needs for the stacks: the popup's entrance and exit,
    // a drag ghost following the pointer, and the hop/shake that acknowledges a
    // move. An open, settled fan asks for nothing -- it does not move, and it
    // has no hover state of its own (user, 2026-09-14). The popup lives on one
    // monitor; the hop and the shake ride a CELL, which every monitor's dock
    // draws, so those are answered for all of them.
    bool needsFrames(MONITORID monitor);
    // A signature of everything drawn, for the dock's damage comparison.
    std::string visualState();

    // Drawn from Dock::render, after the icons and before the menu.
    void render(PHLMONITOR monitor, const Dock::SLayout& layout);

    // ---- the move ---------------------------------------------------------
    // gio semantics and nothing hand-rolled: g_file_move, which does the
    // cross-filesystem fallback itself and which this code must never
    // reimplement as copy-then-unlink. Returns false and fills `err` on
    // failure, having touched nothing.
    bool moveInto(const std::string& destDir, const std::string& srcPath, std::string& err);

    // The error acknowledgement V4 asks for and the dock did not have: a short
    // horizontal shake of one cell. Keyed on the stack's PATH, because a stack
    // has no window class -- which is also why armBounce could not be reused
    // as it stood.
    void armShake(const std::string& path);
    void armBounce(const std::string& path);
    // Applied by the dock's icon loop, beside bounceLift/wiggleRadians.
    float bounceLift(const std::string& path, float iconHeight);
    float shakeOffset(const std::string& path, float iconHeight);
    bool  animating();

    // ---- the dock cell (StackIcon.cpp) --------------------------------------
    // One item of the pile: what it is, where it sits in the cell, its angle.
    struct SPileItem {
        StackModel::SEntry entry;
        CBox               box; // monitor-local RENDER px
        float              rot = 0.F;
    };
    // The pile's items for a stack cell at `iconBox`, bottom first (empty for
    // Display as Folder, a file, an empty, missing or unread folder).
    std::vector<SPileItem> pileLayout(const std::string& stackPath, const CBox& iconBox);
    // Draw a stack's dock cell. False: the caller draws its plain icon (a pinned FILE).
    bool drawCell(PHLMONITOR monitor, const Dock::SItem& item, const CBox& iconBox, float alpha);
    // A thumbnail (cached, or the image itself) or the themed icon.
    SP<Render::ITexture> iconTexture(const StackModel::SEntry& e, int px);
    // The first of a '|'-joined list of icon names the user's theme has.
    std::string pickIcon(const std::string& names);
    // Is this stack's fan/pane open or closing? (its pile items are in flight)
    bool popupShowsStack(const std::string& stackPath);
    // Move `paths` to the Trash off the compositor thread, remembering whose
    // they were: the stack bounces when it lands, or shakes with the error.
    void trashFrom(const std::string& stackPath, std::vector<std::string> paths);
    // Appended to a stack's tooltip: "Folder not found", a recent trash error, or "".
    std::string tooltipSuffix(const std::string& stackPath);

    // ---- the pane's geometry (StackPane.cpp) ---------------------------------
    namespace Pane {
        inline constexpr float CELL_W = 104.F, CELL_H = 112.F, SPACING = 10.F, PAD = 14.F, HEADER_H = 30.F, FIELD_H = 34.F, GAP = 10.F;

        struct SPaneLayout {
            CBox  plate, header, field, grid; // monitor-local RENDER px
            int   cols  = 1;
            float cellW = 0.F, cellH = 0.F;
        };
        SPaneLayout layout(PHLMONITOR monitor, const CBox& anchor, const CBox& pill, size_t items, float progress, bool leaving);
    }
}
