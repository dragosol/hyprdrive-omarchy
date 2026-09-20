#pragma once

#define WLR_USE_UNSTABLE

#include "StripLayout.hpp"
#include "globals.hpp"
#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/helpers/AnimatedVariable.hpp>
#include <hyprland/src/helpers/signal/Signal.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/render/Framebuffer.hpp>
#include <hyprland/src/render/types.hpp>
#include <chrono>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Config.hpp"
#include "DropIndicator.hpp"
#include "DropDecision.hpp"
#include "IOverview.hpp"

class CMonitor;
struct wl_event_source;

// The live half of a released drag -- the window, the destination overview, the
// anchor under the cursor. Defined in scrollOverview.cpp beside the only code
// that fills it in; declared here because the gather/commit steps are private
// members. The DECISION taken from it is the pure half, in DropDecision.hpp.
struct SOverviewDropSite;

// One thing an arrow key can land on: a strip cell, a search result, or a tile.
// Same shape, same collect-then-pick split as the drop above.
struct SOverviewNavTarget;

// Everything the overview makes visible while it is up, and puts back when it
// is not: surface visible-regions it clears, windows it unhides, layers it
// lifts above fullscreen. Three save-and-restore vectors that used to be three
// members of the god object with three force* and three restore* methods, and
// the restores were three consecutive lines of an eight-call destructor litany
// -- a shape that only stays correct while nobody adds a fourth thing or an
// early return.
//
// The destructor is the guarantee, not the mechanism: ~CScrollOverview still
// calls restoreAll() at exactly the point the three restores used to run,
// because their position in that litany is load-bearing (they run before
// images.clear() drops the textures and before the cursor override comes off).
// restoreAll() empties the vectors, so the destructor that follows finds
// nothing to do -- and a future path that forgets to call it no longer leaks a
// hidden window.
class CForcedVisibility {
  public:
    ~CForcedVisibility() { restoreAll(); }

    void forceSurface(SP<CWLSurfaceResource> surface);
    void forceWindowSurfaces(PHLWINDOW window);
    // Tell the client its surface is ON THIS OUTPUT while its tile is drawn.
    // Undone by restoreAll, so a client is never left believing it is visible
    // on a monitor that has stopped drawing it.
    void enterWindowSurfaces(PHLWINDOW window, PHLMONITOR monitor);
    void forceWindow(PHLWINDOW window);
    void forceLayersAboveFullscreen(PHLMONITOR monitor);
    void restoreAll();

  private:
    void restoreSurfaces();
    void restoreWindows();
    void restoreLayers();

    struct SSurface {
        WP<CWLSurfaceResource> surface;
        CRegion                visibleRegion;
    };
    struct SWindow {
        PHLWINDOWREF window;
        bool         hidden = false;
    };
    struct SEntered {
        WP<CWLSurfaceResource> surface;
        PHLMONITORREF          monitor;
    };
    std::vector<SEntered>           m_entered;
    std::unordered_set<void*>       m_enteredSeen;
    struct SLayer {
        PHLLSREF layer;
        bool     aboveFullscreen = true;
        float    alpha           = 1.F;
    };

    std::vector<SSurface> m_surfaces;
    std::vector<SWindow>  m_windows;
    std::vector<SLayer>   m_layers;

    // MEMBERSHIP, SEPARATELY FROM THE SAVED STATE. Both force* entry points run
    // once per surface and once per window per renderWindowLive -- i.e. per
    // tile, per mini, per dock preview, every frame -- and each one was a linear
    // scan of the vector beside it looking for "have I already saved this".
    // The vectors stay the record of what has to be PUT BACK, in order; these
    // only answer the question the scan was asking.
    std::unordered_set<const void*> m_surfaceSeen;
    std::unordered_set<const void*> m_windowSeen;
};

// The overview's blur/frost caches and the state that says whether they are
// still good for this frame. Grouped so "is the frost cache valid" is one
// object's question instead of eight loose members of the god object.
//
// releaseGL() is explicit and NOT the destructor: dropping a framebuffer needs
// the EGL context current, which only ~CScrollOverview knows it has made. The
// destructor drops the shared pointers and nothing else, which is exactly what
// the old code did to cardWallpaperFB.
struct COverviewFrostCache {
    bool     overviewDirty       = true;
    bool     backdropDirty       = true;
    // Y17 (GPU, 2026-09-14). The card-wallpaper FB had no dirty tracking at all
    // ("ponytail: add a dirty flag if profiling ever cares") and the backdrop
    // had a flag that was cleared but never READ, so both re-rendered the whole
    // wallpaper -- and the backdrop a forced 4-pass full-monitor kawase on top
    // of it -- once per monitor per frame, forever.
    bool     cardWallpaperDirty  = true;
    bool     overviewStateValid  = false;
    float    lastOverviewScale   = 1.F;
    int      lastBackdropWallpaperMode = -1;
    Vector2D lastOverviewViewOffset    = Vector2D{};

    // Y17. Signature of everything the two wallpaper caches read that a SURFACE
    // COMMIT does not report: monitor pixel size/transform/scale, the target
    // format and image description, the wallpaper texture and its fade-in
    // opacity, each wallpaper layer's geometry/alpha/texture, and the blur
    // knobs blurFramebufferWithDamage reads globally. 0 = never computed, so
    // the first frame after a reset always renders.
    uint64_t lastBackdropSig      = 0;
    uint64_t lastCardWallpaperSig = 0;

    SP<Render::IFramebuffer> backdropFB;
    SP<Render::IFramebuffer> cardWallpaperFB;

    void releaseGL() {
        // Y17: cardWallpaperFB was dropped without release() while backdropFB
        // was released -- the same kind of object, two lifetimes. Both are ours
        // and both are dropped here, which is the one place that knows the EGL
        // context is current.
        if (backdropFB)
            backdropFB->release();
        backdropFB.reset();
        if (cardWallpaperFB)
            cardWallpaperFB->release();
        cardWallpaperFB.reset();
        backdropDirty        = true;
        cardWallpaperDirty   = true;
        lastBackdropSig      = 0;
        lastCardWallpaperSig = 0;
    }
};

struct SSearchLayout;

// See normalizeFittingScrollingOffset (OverviewInternal.hpp): a scrolling
// desktop whose columns fit the screen is put back to its centred camera.
void normalizeScrollingCamera(PHLWORKSPACE workspace);
void normalizeAllScrollingCameras();

class CScrollOverview : public IOverview {
  public:
    // The recorded frame cadence for an output, kept across closes, for
    // `hyprdrive:debug frames`. See the note in scrollOverview.cpp.
    static std::string frameCadence(MONITORID monitor);

    // This monitor's slot in the per-monitor search session state (SearchSession.hpp).
    MONITORID      searchMonitorId() const;
    SSearchLayout& searchLayoutHere() const;

    // A config reload rebuilt Hyprland's config from disk, which wipes the
    // overrides this overview installed while open (workspace animations off,
    // follow_mouse/no_warps parked) AND makes the saved "previous" values
    // stale -- the fresh config IS what a later close must restore. Drop the
    // stale snapshots and install the overrides again from the reloaded
    // values. Self-guarding: a no-op on any overview that is not the shared
    // state owner or never overrode anything (user, 2026-09-14).
    void reapplyConfigOverridesAfterReload();

    CScrollOverview(PHLWORKSPACE startedOn_, bool swipe = false, PHLMONITOR monitor = {});
    ~CScrollOverview() override;

    void         render() override;
    void         damage() override;
    void         requestInputFrame() override;
    void         markBlurDirty();
    void         markBackdropBlurDirty();
    bool         shouldHandleSurfaceDamage(SP<CWLSurfaceResource> surface) override;
    bool         shouldAllowSurfaceFrame(SP<CWLSurfaceResource> surface, const Time::steady_tp& now) override;
    bool         shouldAllowRealtimePreviewSchedule() override;
    bool         shouldSuppressRenderDamage() const override;
    void         onPreRender() override;

    void           setClosing(bool closing) override;
    EOverviewPhase phase() const override;
    void           reopen() override;

    void         resetSwipe() override;
    void         onSwipeUpdate(double delta) override;
    void         onSwipeEnd() override;
    void         onWorkspaceSwipeBegin() override;
    void         onWorkspaceSwipeUpdate(double delta) override;
    void         onWorkspaceSwipeEnd() override;
    // The "scroll" gesture: pan the scrolling tape of the desktop in view with
    // the fingers, snapping to a column on release (the same follow the
    // touchpad's own two-finger scroll gets inside the overview). False when
    // that desktop is not a scrolling one, so the gesture can say so.
    bool         onTapeSwipeUpdate(double delta);
    void         onTapeSwipeEnd();
    float        openProgress() const override;

    // close without a selection
    void         close() override;
    void         selectHoveredWorkspace() override;
    void         performPrimaryClickAction() override;
    bool         moveSelection(const std::string& direction) override;
    bool         windowDispatcherAction(const std::string& action) override;

  private:
    void   ensureTrailingWorkspace();
    void   ensureNoWorkspaceGaps();
    void   rebuildWorkspaceImages();
    void   seedRememberedSelections();
    void   redrawAll(bool forcelowres = false);
    void   renderListShadow();
    void   onWorkspaceChange();
    void   renderGlobalWallpaper(PHLMONITOR monitor, const Time::steady_tp& now);
    void   renderWallpaperLayers(PHLMONITOR monitor, const CBox& workspaceBox, float renderScale, const Time::steady_tp& now, float alpha = 1.F);
    void   updateBackdropBlurCache(PHLMONITOR monitor, int wallpaperMode, const Time::steady_tp& now);
    void   updateCardWallpaperFB(PHLMONITOR monitor, const Time::steady_tp& now);
    void   renderBackdropBlurCache(PHLMONITOR monitor, float alpha = 1.F);
    void   renderWorkspaceBackground(PHLMONITOR monitor, size_t workspaceIdx, size_t activeIdx, float workspacePitch, float renderScale, int wallpaperMode, const Time::steady_tp& now);
    void   renderWorkspaceLive(PHLMONITOR monitor, size_t workspaceIdx, size_t activeIdx, float workspacePitch, float renderScale, int wallpaperMode, const Time::steady_tp& now);
    void   renderDetentBadge(PHLMONITOR monitor, float renderScale);
    // Whether renderDetentBadge will put a number on the strip this frame. The
    // resting numbers read it too, so they cannot disagree about it.
    bool   detentBadgeVisible() const;
    // Re-aims the resting-number fade at whichever cell the badge is on now.
    // Mutates the fade's clock, so: once per frame.
    void   updateRestingFade();
    // Alpha for ONE cell's resting number. Only the badged cell fades; its
    // predecessor eases back in behind the badge as it moves along the strip.
    float  restingNumberFade(size_t workspaceIdx) const;
    bool   hasVisiblePrecomputedBlurWindow(PHLMONITOR monitor, size_t activeIdx, float workspacePitch, float renderScale) const;
    void   renderWindowLive(PHLMONITOR monitor, PHLWINDOW window, const CBox& windowBox, float renderScale, const Time::steady_tp& now, const CBox* workspaceBox = nullptr,
                             bool dragged = false, float alphaMul = 1.F);
    void   renderDraggedWindow(PHLMONITOR monitor, size_t activeIdx, float workspacePitch, float renderScale, const Time::steady_tp& now);
    void   renderPinnedFloatingWindows(PHLMONITOR monitor, float overviewScale, const Time::steady_tp& now);
    float        workspaceScrollingSpanRatio(const PHLWORKSPACE& workspace, PHLMONITOR monitor) const;
    float        workspaceScrollingSpanRatioLive(const PHLWORKSPACE& workspace, PHLMONITOR monitor) const;
    CBox         stripCellLogicalBox(size_t workspaceIdx) const;
    CBox         stripCellBox(size_t workspaceIdx, PHLMONITOR monitor, float renderScale) const;
    PHLWORKSPACE workspaceAtStripPoint(const Vector2D& point, size_t* hoveredWorkspaceIdx = nullptr) const;
    // V3. IOverviewGeometry's name for the line above -- the dock reaches it
    // through the interface, and a defaulted out-parameter cannot be a virtual.
    PHLWORKSPACE stripWorkspaceAt(const Vector2D& point) const override;
    CBox         stripMiniBox(const CBox& cell, PHLWINDOW window, PHLMONITOR monitor) const;
    PHLWINDOW    windowAtStripPoint(const Vector2D& point, CBox* miniBox = nullptr) const;
    void         focusOverviewWorkspace(const PHLWORKSPACE& workspace, size_t workspaceIdx);
    // Everything the strip's cells and its container panel share: the two radii
    // (outer corners large, facing corners tight), the rounding power, and the
    // open/close fade every one of them multiplies its alpha by.
    struct SStripStyle {
        int   round      = 0;
        int   innerRound = 0;
        float power      = 2.F;
        float fade       = 1.F;
    };
    void         renderStrip(PHLMONITOR monitor, float renderScale, const Time::steady_tp& now);
    void         renderStripPanel(PHLMONITOR monitor, float renderScale, const SStripStyle& style);
    void         renderStripCellRect(const CBox& cell, const CHyprColor& color, bool largeLeft, bool largeRight, const SStripStyle& style) const;
    void         renderStripCellBorder(PHLMONITOR monitor, const CBox& cell, bool largeLeft, bool largeRight, const SStripStyle& style) const;
    void         renderStripCell(PHLMONITOR monitor, size_t workspaceIdx, const CBox& cell, const PHLWINDOW& dragged, bool hovered, float renderScale,
                                 const Time::steady_tp& now, const SStripStyle& style);
    void         collectNavTargets(PHLMONITOR monitor, std::vector<SOverviewNavTarget>& targets) const;
    CBox         currentNavBox(const std::vector<SOverviewNavTarget>& targets, PHLMONITOR monitor) const;
    PHLWINDOW    firstNavTile() const;
    bool         isNavTile(const PHLWINDOW& window) const;
    void         renderSearch(PHLMONITOR monitor, float renderScale);
    void   moveViewportWorkspace(bool up);
    void   trackpadSwipeLayout(const PHLWORKSPACE target, const double delta);
    void   trackpadSwipeWorkspace(const double delta);
    void   finishWorkspaceScrollFollow();
    void   maybeTickDetent(size_t workspaceIdx);
    double trackpadWorkspaceScrollOffset(PHLMONITOR monitor, float renderScale);
    bool   scrollStepAllowed(uint32_t timeMs);
    bool   selectOverviewWindow(PHLWINDOW window, size_t workspaceIdx, bool syncFocus = false);
    bool   selectWindowAtOverviewCursor(bool syncFocus = false);
    PHLWINDOW windowClosestToWorkspaceCenter(size_t workspaceIdx) const;
    void   rememberSelection(PHLWINDOW window);
    size_t matchingWindowCount() const;
    void   syncSelectionToViewport();
    void   syncFocusedSelection();
    size_t dragWorkspaceIndex(PHLWINDOW window) const;
    void   updateWorkspaceOverflow();
    CBox   workspaceOverviewVisibleBox(size_t workspaceIdx, const CBox& workspaceBox, float renderScale, PHLMONITOR monitor) const;
    float      workspaceOverviewOffset(size_t workspaceIdx, size_t activeIdx, float workspacePitch) const;
    float      workspaceOverviewLogicalOffset(size_t workspaceIdx, size_t activeIdx, float workspacePitch) const;
    float      workspaceOverviewAlpha(size_t workspaceIdx) const;
    PHLWINDOW windowAtOverviewPoint(const Vector2D& point, size_t* workspaceIdx = nullptr) const;
    PHLWINDOW windowAtOverviewCursor(size_t* workspaceIdx = nullptr);
    CBox      tileBox(const PHLWINDOW& window) const;
    CBox      overviewBoxForGlobal(WORKSPACEID workspaceId, const CBox& globalBox) const;
    void      refreshWindows() override;
    bool      selectAndClose(const PHLWINDOW& window);
    CBox      tileCloseButtonBox(const PHLWINDOW& window) const;
    void      updateTileHover();

    // EVERYTHING A BARE POINTER MOVE CAN CHANGE ON SCREEN, as one comparable
    // value. AQ_SCHEDULE_CURSOR_MOVE is not throttled, so the unconditional
    // frame request the motion handler used to make repainted the whole
    // overview at the output's refresh rate for a cursor crossing the gap
    // between two tiles. Nothing here follows the pointer CONTINUOUSLY -- the
    // drag proxy, the resize and the pan do, and they ask for their own frames
    // -- so a frame is owed only when one of these discrete states flips.
    struct SHoverSignature {
        Desktop::View::CWindow* tile            = nullptr;
        bool                    closeButton     = false;
        int                     searchHover     = -1;
        int                     searchSelection = -1;
        int                     searchZone      = 0;
        bool                    scratchZone     = false; // the invisible reveal zone
        bool                    scratchCell     = false; // the cell itself, which lights when hovered
        bool                    altZone         = false;
        const void*             altCellOverview = nullptr;
        size_t                  altCellIdx      = 0;

        bool operator==(const SHoverSignature&) const = default;
    };
    SHoverSignature hoverSignature() const;
    bool            hoverSignatureChanged();
    SHoverSignature lastHoverSignature;
    bool            lastHoverSignatureValid = false;
    CDropIndicator::SDropAnchor dropAnchorAtOverviewCursorOnWorkspace(size_t workspaceIdx, const PHLWINDOW& ignoredWindow = nullptr,
                                                                      CScrollOverview* dragContext = nullptr);
    PHLWORKSPACE workspaceAtOverviewPoint(const Vector2D& point, size_t* workspaceIdx = nullptr) const;
    PHLWORKSPACE workspaceAtOverviewDropPoint(const Vector2D& point, size_t* workspaceIdx = nullptr, const PHLWINDOW& draggedWindow = nullptr) const;
    PHLWORKSPACE workspaceAtOverviewCursor(size_t* workspaceIdx = nullptr) const;
    Vector2D  overviewPointToGlobal(size_t workspaceIdx, const Vector2D& pointLocal) const;
    CBox      draggedWindowBox(size_t workspaceIdx) const;
    CBox      draggedWindowBoxFor(PHLWINDOW window, size_t workspaceIdx, const Vector2D& pointLocal, const Vector2D& grabRatio) const;
    CBox      draggedWindowGlobalBox() const;
    void      refreshDragOriginalOverviewBoxes();
    void      clearDragPending();
    void      beginWindowDrag(PHLWINDOW window, const CBox* stripMiniOrigin = nullptr);
    void      beginWindowDragFrom(const PHLWINDOW& window, const CBox& miniOrigin) override;
    void      updateWindowDrag();
    void      endWindowDrag();
    // The three gathering stages of a drop, in the order endWindowDrag() runs
    // them: each one only asks what the stage before it did not already settle,
    // because a release on the dock pill has to return before a workspace is
    // queried and the anchor query writes the destination's cursor.
    void      gatherDropSite(SOverviewDropSite& site);
    void      gatherDropWorkspace(SOverviewDropSite& site);
    void      gatherDropAnchor(SOverviewDropSite& site);
    void      commitDropMinimize(const SOverviewDropSite& site);
    void      commitDropCancel(const SOverviewDropSite& site);
    void      commitDrop(const SOverviewDropSite& site, const ScrollOverview::Drop::SDecision& decision);
    float     dragMiniProgress() const;
    float     dragRecentreProgress() const;
    Vector2D  dragEffectiveGrabRatio() const;
    CBox      resizedWindowBox() const;
    void      beginWindowResize();
    void      updateWindowResize();
    void      endWindowResize();
    void      updateScrollingPan();
    void      beginScrollingPan(PHLWORKSPACE workspace);
    void      endScrollingPan();
    void      focusMostVisibleScrollingWindow(const PHLWORKSPACE& workspace);
    bool      moveScrollingColumnSelection(bool next);
    void   applyWorkspaceAnimationOverrides();
    void   restoreWorkspaceAnimationOverrides();
    void   forceWorkspaceAlphaVisible();
    void   forceWorkspaceAlphaVisible(const PHLWORKSPACE& workspace);
    void   forceWorkspaceWindowsDecoRecalc(const PHLWORKSPACE& workspace);
    void   emitFullscreenVisibilityState(PHLWINDOW window, bool hideFullscreen);
    void   applyInputConfigOverrides();
    void   restoreInputConfigOverrides();
    void   transferSharedStateOwnership();
    size_t activeWorkspaceIndex() const;
    bool   isSelectedWorkspace(const PHLWORKSPACE& workspace) const;
    void   sendOverviewFrameCallbacks(const Time::steady_tp& now);
    bool   hasRunningWorkspaceAnimation() const;
    bool   shouldAllowRealtimePreviewFrame() const;
    void   scheduleMinimumPreviewFrame();
    void   schedulePreviewFrameAfter(std::chrono::milliseconds delay);
    void   scheduleRealtimePreviewFrame();
    // Whether the idle pulse owes a REPAINT rather than just frame callbacks.
    bool   needsIdleRepaint() const;
    // One line of "what on this overview is still asking for frames", for
    // `hyprdrive:debug frames`. Reads state only (2026-09-15).
    std::string frameDebugLine() const override;

    // ADAPTIVE PREVIEW PULSE. One entry per window whose tile this overview has
    // gated, keyed by the raw pointer and holding a ref purely so the pulse can
    // drop entries whose window is gone. See OVERVIEW_PREVIEW_POLL_INTERVAL.
    struct SPreviewPulse {
        PHLWINDOWREF    ref;
        bool            streaming         = false;
        Time::steady_tp lastPoll          = {};  // last frame callback handed out while polling
        Time::steady_tp lastChange        = {};  // last damage the client committed
        Time::steady_tp graceUntil        = {};  // forced streaming after a move
        WORKSPACEID     lastSeenWorkspace = WORKSPACE_INVALID;
    };
    std::unordered_map<Desktop::View::CWindow*, SPreviewPulse> previewPulses;

    // May this window's client have a frame callback now? Polling windows are
    // told no until their two seconds are up. Mutates the state, so it is the
    // one place the poll clock advances.
    bool            previewWantsFrame(const PHLWINDOW& window, const Time::steady_tp& now);
    // The client committed: it drew something, so it is worth streaming.
    void            notePreviewDamage(const PHLWINDOW& window, const Time::steady_tp& now);
    bool            anyPreviewStreaming(const Time::steady_tp& now) const;
    // Is THIS window streaming right now? Const, unlike previewWantsFrame: the
    // frame and damage gates both ask, and neither should advance the poll clock.
    bool            previewIsStreaming(const PHLWINDOW& window, const Time::steady_tp& now) const;
    // How long the idle pulse may sleep: a frame interval while anything streams,
    // otherwise until the next window is due a poll.
    std::chrono::milliseconds previewPulseInterval(const Time::steady_tp& now) const;
    void            prunePreviewPulses();

    // Something asked for a repaint and was throttled; the next pulse owes it.
    bool   previewRepaintPending = false;
    // When the idle pulse last repainted outright, for OVERVIEW_IDLE_REPAINT_FLOOR.
    Time::steady_tp lastIdleRepaint = {};
    void   suspendInputListeners();
    void   useAnimDirection(bool opening);
    void   syncPluginAnimEnabled();
    // THE DETACHED SCRATCHPAD CELL (Scratchpad.hpp). Deliberately NOT one of
    // `images`: a swipe walks that vector, and the requirement is that a swipe
    // can never land on the scratchpad. It is revealed by a window drag or by
    // the invisible zone it will occupy, and reached only through these -- which
    // is also why workspaceAtStripPoint still knows nothing about it.
    float        scratchpadRevealValue() const;
    CBox         stripSlotLogicalBox(size_t workspaceIdx, bool scratchSlot, float revealOverride) const;
    CBox         scratchpadCellLogicalBox(float revealOverride) const;
    CBox         scratchpadCellBox(PHLMONITOR monitor, float renderScale, float revealOverride) const;
    CBox         scratchpadZoneLogicalBox() const;
    bool         scratchpadCellContains(const Vector2D& point) const;
    PHLWORKSPACE scratchpadCellWorkspace() const;
    void         updateScratchpadReveal();
    void         renderScratchpadCell(PHLMONITOR monitor, float renderScale, const Time::steady_tp& now, const SStripStyle& style);

    // THE OTHER MONITORS' STRIPS (user, 2026-09-14). One detached strip per other
    // monitor, left of this one, each a live view of THAT monitor's overview strip:
    // its desktops, its windows, its active desktop. Revealed exactly like the
    // scratchpad cell -- a window drag, or an invisible zone left of the strip --
    // and never at all when this is the only monitor. Like the scratchpad cell
    // they are not `images`, so a swipe can never walk onto another monitor.
    struct SAltStrip {
        CScrollOverview* overview = nullptr; // that monitor's overview; alive for the frame it was read in
        PHLMONITOR       monitor;
    };
    struct SAltHit {
        CScrollOverview* overview = nullptr;
        PHLMONITOR       monitor;
        size_t           workspaceIdx = 0; // into overview->images
        PHLWORKSPACE     workspace;
        CBox             cell; // on THIS monitor, render px
    };
    // BY REFERENCE, into the memo below. No caller holds it across a call that
    // could rebuild it.
    const std::vector<SAltStrip>&           altStrips() const;
    float                                   altStripsRevealValue() const;
    // BY REFERENCE, into the memo below: the cell-box accessor asks for one
    // cell at a time and the output carries three vectors. Safe for every
    // caller because none of them holds the reference across another stripRow.
    const ScrollOverview::StripLayout::SOutput& stripRow(float scratchRevealOverride, float altRevealOverride) const;

    // THE STRIP ROW, MEMOISED FOR THE FRAME IT WAS DRAWN IN. Every cell box,
    // every mini, the scratchpad cell, both reveal zones and the alt strips all
    // come out of stripRow, so one frame rebuilt the whole row a few dozen
    // times -- and each rebuild walked every workspace's windows once per image
    // to work out its scrolling span ratio. The key is this output's frame
    // counter plus every scalar the layout reads, so a mid-frame change (the
    // search row publishes its height after the strip has drawn) still misses.
    // Held between frames on purpose: a hit test belongs against the row that
    // is on screen.
    struct SStripRowMemo {
        uint64_t frame        = 0;
        float    scratchArg   = -1.F;
        float    altArg       = -1.F;
        float    scratch      = 0.F;
        float    alt          = 0.F;
        float    top          = 0.F;
        float    band         = 0.F;
        float    gap          = 0.F;
        double   monX         = 0.0;
        double   monY         = 0.0;
        double   monW         = 0.0;
        double   aspect       = 0.0;
        size_t   images       = 0;
        size_t   altSignature = 0;
        bool     filled       = false;

        ScrollOverview::StripLayout::SOutput out;
    };
    mutable SStripRowMemo stripRowMemo;

    // altStrips()' answer for the frame it was last built in. See the function.
    mutable std::vector<SAltStrip> altStripsBuf;
    mutable uint64_t               altStripsFrame  = 0;
    mutable size_t                 altStripsCount  = 0;
    mutable bool                   altStripsFilled = false;

    // The span ratio is asked for once per image by stripRow and once per MINI
    // by stripMiniBox, and each answer walks that workspace's window list, does
    // a dynamic_cast and searches `images` linearly. It cannot change inside a
    // drawn frame, so it is answered once per frame per workspace.
    mutable uint64_t                          spanRatioFrame = 0;
    mutable MONITORID                         spanRatioMonitor = MONITOR_INVALID;
    mutable std::unordered_map<WORKSPACEID, float> spanRatioMemo;

    // The X7/X8 chrome queue renderWorkspaceLive fills and flushes within one
    // workspace pass. Held here so the pass does not allocate one per card per
    // frame; see the comment at its only use.
    std::vector<std::pair<PHLWINDOW, CBox>> tileChromeBuf;

    // matchingWindowCount's answer, keyed on the query and the membership
    // generation. See the function for why those two are the whole key.
    mutable std::string matchCountQuery;
    mutable uint64_t    matchCountMembers = 0;
    mutable size_t      matchCountImages  = 0;
    mutable size_t      matchCountValue   = 0;
    mutable bool        matchCountFilled  = false;
    CBox                                    altZoneLogicalBox() const;
    std::optional<SAltHit>                  altCellAt(const Vector2D& point) const;
    void                                    updateAltStripsReveal();
    void                                    renderAltStrips(PHLMONITOR monitor, float renderScale, const Time::steady_tp& now, const SStripStyle& style);

    void   activateSubmapIfConfigured();
    void   restoreSubmapIfActive();
    bool   dispatchSubmapMouseClick(uint32_t button);
    static int realtimePreviewTimerCallback(void* data);

    size_t viewportCurrentWorkspace = 0;
    bool   rebuildPending           = false;

    // Y9. Whether the LAST frame ran with an empty query. shouldShowOverviewWindow
    // answers differently for a MINIMIZED window either side of that edge, and
    // the window list is not rebuilt on its own, so the edge has to be noticed.
    // Starts true because every open starts unfiltered (main.cpp:282).
    bool   lastQueryEmpty           = true;

    COverviewFrostCache frost;

    struct SWorkspaceImage {
        PHLWORKSPACE              pWorkspace;
        std::vector<PHLWINDOWREF> windows;
        float                     overflowLeft   = 0.F;
        float                     overflowRight  = 0.F;
        float                     overflowTop    = 0.F;
        float                     overflowBottom = 0.F;
    };

    struct SWorkspaceInsertTransition {
        bool                             active              = false;
        WORKSPACEID                      transitionWorkspaceID = WORKSPACE_INVALID;
        bool                             transitionFadeIn   = true;
        std::unordered_map<WORKSPACEID, float> oldRelativeOffsets;
        std::unordered_map<WORKSPACEID, float> newRelativeOffsets;
        float                            transitionOldRelativeOffset = 0.F;
    };

    Vector2D                         lastMousePosLocal = Vector2D{}; // monitor-local pixel space

    PHLWINDOWREF                     closeOnWindow;
    WORKSPACEID                      focusSyncedFromWorkspaceID = WORKSPACE_INVALID;

    // ---------------------------------------------------------------- gestures
    // The pointer gestures the overview owns, one struct each, because a
    // field's name no longer has to carry its owner: `drag.originalBox` and
    // `resize.originalBox` used to be `dragOriginalBox` and `resizeOriginalBox`
    // in one flat list of 31, and which of the three a given member belonged to
    // was a prefix convention rather than a fact the compiler knew.
    //
    // NOT a std::variant, deliberately, and this is the phase's one scope-down:
    // the three are not mutually exclusive here. drag and resize hang off
    // DIFFERENT mouse buttons (MAIN vs RESIZE, OverviewInput.cpp:309/454) and
    // neither begin path guards on the other, so holding both buttons runs
    // beginWindowDrag() and beginWindowResize() over the same window and
    // onMouseMove() then updates both in one event (OverviewInput.cpp:154-163).
    // A variant makes that state unrepresentable -- which is the point of a
    // variant, and is also a behaviour change, which this phase is not allowed
    // to make. Grouping is the part that is free.

    struct SWindowDrag {
        PHLWINDOWREF    activeWindow;
        PHLWORKSPACEREF originalWorkspace;
        // Also the SUBMAP click's press point (beginSubmapMouseClickPending):
        // one press starts either gesture and the threshold is measured from
        // the same coordinate, so there is one field, not two.
        Vector2D        startMouseLocal         = Vector2D{};
        Vector2D        grabOffsetLocal         = Vector2D{};
        Vector2D        grabRatio               = Vector2D{0.5, 0.5};
        // Torn out of a dock preview rather than grabbed off a card. The proxy
        // is then sized by the slot it WOULD get on the card being looked at,
        // not by the box it had on the desktop it came from -- a window that was
        // alone on its own desktop would otherwise grow to the whole card, i.e.
        // bigger than anything else in the overview.
        bool            fromDockPreview         = false;
        bool            miniOver                = false;
        float           miniFrom                = 0.F;
        Time::steady_tp miniChangedAt           = {};
        bool            recentreOver            = false;
        float           recentreFrom            = 0.F;
        Time::steady_tp recentreChangedAt       = {};
        Vector2D        originalFloatSize       = Vector2D{};
        Vector2D        originalTapeTranslation = Vector2D{};
        CBox            originalBox             = CBox{};
        CBox            originalVisualBox       = CBox{};
        CBox            originalOverviewBox     = CBox{};
        CBox            originalOverviewHitbox  = CBox{};
        bool            pendingPrimary          = false;
        bool            startedTiled            = false;
        // Where the proxy was on the PREVIOUS motion, in global px. The frame
        // that rubs the proxy off an output is the frame after it left it, so
        // the cross-monitor damage gate has to look at both.
        CBox            lastGlobalBox           = CBox{};
    };

    struct SWindowResize {
        PHLWINDOWREF        pendingWindow;
        PHLWINDOWREF        activeWindow;
        Vector2D            startMouseLocal = Vector2D{};
        Vector2D            lastMouseLocal  = Vector2D{};
        CBox                originalBox     = CBox{};
        size_t              workspaceIdx    = 0;
        Layout::eRectCorner corner          = Layout::CORNER_NONE;
        bool                pointerDown     = false;
    };

    struct SPan {
        PHLWORKSPACEREF workspace;
        PHLWINDOWREF    initialWindow;
        Vector2D        lastMouseLocal = Vector2D{};
        bool            pointerDown    = false;
    };

    SWindowDrag                      drag;
    SWindowResize                    resize;
    SPan                             pan;

    bool                             submapMouseClickPending = false;
    bool                             emittingFullscreenVisibilityState = false;
    bool                             inputConfigOverridden = false;
    bool                             realtimePreviewTimerArmed = false;
    bool                             realtimePreviewFrameQueued = false;
    bool                             selectedWorkspaceFramePending = false;
    bool                             inputFramePending = false;
    bool                             sendingOverviewFrameCallbacks = false;
    // Set while renderWindowLive is asking for the next frame of the window it
    // is drawing. The draw has already answered "is this on screen"; see
    // shouldAllowSurfaceFrame.
    bool                             askingForDrawnWindow = false;

    // WHAT THE LAST PASS DREW. The render pass records; the event loop wakes.
    // Membership is also the gate's answer to "is this window on screen": the
    // pass that drew it knows, and it knows about the strip thumbnail as well as
    // the card, which is the distinction the card-grid geometry could not make.
    std::vector<PHLWINDOWREF>        drawnWindows;
    std::unordered_set<void*>        drawnSet;
    void                             wakeDrawnWindows(const Time::steady_tp& now);
    bool                             wantsWakes() const;
    bool                             usesSubmapKeybinds = false;
    bool                             submapActive = false;
    uint32_t                         submapMouseClickButton = 0;
    std::string                      previousSubmapName;
    int                              previousNoWarps = 0;
    int                              previousWarpOnChangeWorkspace = 0;
    int                              previousWarpOnToggleSpecial = 0;
    int                              previousWarpBackAfterNonMouseInput = 0;
    int                              previousFollowMouse = 0;

    std::vector<SP<SWorkspaceImage>> images;
    std::vector<PHLWINDOWREF>        pinnedFloatingWindows;
    std::unordered_map<WORKSPACEID, PHLWINDOWREF> rememberedSelection;
    SWorkspaceInsertTransition       workspaceInsertTransition;
    PHLWORKSPACEREF                  pendingRemovedWorkspace;
    PHLWORKSPACE                     trailingWorkspace;
    std::vector<PHLWORKSPACE>        gapWorkspaces;
    ScrollOverview::Config::ELayout  layout = ScrollOverview::Config::ELayout::VERTICAL;

    CForcedVisibility forced;

    struct SWorkspaceAnimationConfig {
        std::string                                      name;
        Hyprutils::Animation::SAnimationPropertyConfig   config;
    };
    std::vector<SWorkspaceAnimationConfig> savedWorkspaceAnimationConfigs;
    bool                                   workspaceAnimationsOverridden = false;
    bool                                   sharedStateOwner = false;

    PHLWORKSPACE                     startedOn;

    PHLANIMVAR<float>                scale;
    PHLANIMVAR<Vector2D>             viewOffset;
    PHLANIMVAR<float>                workspaceInsertProgress;
    PHLANIMVAR<float>                workspaceInsertFadeProgress;
    PHLANIMVAR<float>                reflowPacer;
    // X21/X23: eased height of the search results band in the layout stack.
    PHLANIMVAR<float>                resultsBand;
    // Eased width of the unified search results container block.
    PHLANIMVAR<float>                resultsWidth;
    // X50: eased 0..1 dock factor for the search pill. 1 = results are on
    // screen and the pill has left the scaled card for the top of the monitor.
    PHLANIMVAR<float>                searchDock;
    // How far the detached scratchpad cell is out (0 hidden, 1 shown). The
    // strip's own layout reads it, which is what slides the strip left to make
    // room rather than having the cell overlap it.
    PHLANIMVAR<float>                scratchpadReveal;
    // The same for every other monitor's strip at once (0 hidden, 1 shown).
    PHLANIMVAR<float>                altStripsReveal;
    SP<Hyprutils::Animation::SAnimationPropertyConfig> workspaceInsertFadeConfig;
    SP<Hyprutils::Animation::SAnimationPropertyConfig> workspaceRemoveFadeConfig;
    SP<Hyprutils::Animation::SAnimationPropertyConfig> openAnimConfig;
    SP<Hyprutils::Animation::SAnimationPropertyConfig> closeAnimConfig;
    // X75. Used when a toggle catches the previous one mid-flight; see
    // useAnimDirection for why a reversal cannot use either directional curve.
    SP<Hyprutils::Animation::SAnimationPropertyConfig> reverseAnimConfig;
    // Search panel (band / width / dock). windowsMove is ~400ms and retargets
    // as columns appear; this is a single ~180ms ease-out to a stable span.
    SP<Hyprutils::Animation::SAnimationPropertyConfig> searchAnimConfig;
    Time::steady_tp                  lastRealtimePreviewFrame = {};
    Time::steady_tp                  realtimePreviewTimerDue = {};
    wl_event_source*                 realtimePreviewTimer = nullptr;

    // The overview's whole lifecycle is two states: Open and Closing. `closing`
    // is the single source of truth for both -- set at the top of close() (which
    // is also the double-invocation guard) and cleared by reopen() via
    // setClosing(false). It used to be shadowed by a second `closeApplied` bool
    // that carried the same fact; two bools for one state is a drift waiting to
    // happen (the Z18 zombie class is exactly "which flag is authoritative"), so
    // there is one now. `closing` is read directly by every input handler, hence
    // a bool rather than an enum -- with two states there is nothing to make
    // unrepresentable.
    bool                             closing = false;

    // event handlers -- moved out of the constructor, defined at the bottom of scrollOverview.cpp
    void onMouseMove(Vector2D, Event::SCallbackInfo& info);
    void onTouchMove(ITouch::SMotionEvent, Event::SCallbackInfo& info);
    void onMouseButton(IPointer::SButtonEvent event, Event::SCallbackInfo& info);
    void onCursorSelect(ITouch::SDownEvent, Event::SCallbackInfo& info);
    void onMouseAxis(IPointer::SAxisEvent e, Event::SCallbackInfo& info);
    void onWindowOpen(PHLWINDOW);
    void onWindowClose(PHLWINDOW);
    void onWindowMove(PHLWINDOW, PHLWORKSPACE);
    void onWindowActive(PHLWINDOW window, Desktop::eFocusReason);
    void onWindowFullscreen(PHLWINDOW window);
    void onWorkspaceLifecycle(PHLWORKSPACEREF);
    void onKeyboardKey(IKeyboard::SKeyEvent event, Event::SCallbackInfo& info);

    CHyprSignalListener             mouseMoveHook;
    CHyprSignalListener             mouseButtonHook;
    CHyprSignalListener             touchMoveHook;
    CHyprSignalListener             touchDownHook;
    CHyprSignalListener             mouseAxisHook;
    CHyprSignalListener             windowOpenHook;
    CHyprSignalListener             windowCloseHook;
    CHyprSignalListener             windowMoveHook;
    CHyprSignalListener             windowActiveHook;
    CHyprSignalListener             windowFullscreenHook;
    CHyprSignalListener             keyboardKeyHook;
    CHyprSignalListener             workspaceCreatedHook;
    CHyprSignalListener             workspaceRemovedHook;

    bool                             swipe = false;

    uint32_t                         lastScrollStepTimeMs = 0;      // event time of the last discrete scroll step, for scroll_event_delay throttling

    double                           trackpadScrollAccum          = 0.0;
    bool                             trackpadWorkspaceFollowing   = false;
    bool                             trackpadTapeFollowing        = false;
    bool                             trackpadGestureSettlePending = false;
    double                           trackpadGestureSettleOffset  = 0.0;

    // DETENT STATE for the 3-finger workspace drag.
    //
    // trackpadDetentRaw is the UNWARPED accumulator and is the only thing the
    // finger drives. trackpadScrollAccum keeps holding the warped value, because
    // finishWorkspaceScrollFollow() and every other reader want the offset the
    // cards are actually at. Accumulating into the warped value instead would
    // feed the curve its own output every frame and compound it.
    double                           trackpadDetentRaw       = 0.0;
    bool                             trackpadDetentActive    = false;
    // Card the drag is currently settling on, and how settled: 1 at a card
    // centre, 0 at the midpoint between two. Drives both the highlight and the
    // pill, so they share one number and cannot disagree.
    size_t                           trackpadDetentWorkspace = 0;
    float                            trackpadDetentFocus     = 0.F;
    // When trackpadDetentWorkspace last CHANGED. The strip badge spawns off
    // this with the search field's per-letter entrance, so landing on a new
    // desktop re-pops the number instead of cross-fading it.
    Time::steady_tp                  trackpadDetentBornAt;
    // Cross-fade state for the resting strip numbers. Only two cells are ever
    // in motion: the one the badge just landed on (fading OUT) and the one it
    // just left (fading back IN). Everything else is simply lit.
    size_t                           restingFadeCell     = std::numeric_limits<size_t>::max();
    size_t                           restingFadePrevCell = std::numeric_limits<size_t>::max();
    Time::steady_tp                  restingFadeAt;
    // Index the last tick fired for. size_t max = none yet, so the first
    // settle after a swipe begins does not fire a spurious tick.
    size_t                           trackpadDetentTickedIdx = std::numeric_limits<size_t>::max();
    Time::steady_tp                  trackpadDetentLastTick;

};

bool overviewToggleSushiForSelection();
// SPACE while the overview search is up: true if consumed as Quick Look,
// false if the key should type a space into the query.
bool overviewHandleSearchSpace();
// Double-tap Shift: launch a new instance of the selected app even if one
// is already open. True if the tap was consumed as that launch.
bool overviewHandleShiftTap();
bool overviewHandleCtrlTap();
