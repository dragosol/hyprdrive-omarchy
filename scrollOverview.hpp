#pragma once

#define WLR_USE_UNSTABLE

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
#include <string>
#include <unordered_map>
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
    struct SLayer {
        PHLLSREF layer;
        bool     aboveFullscreen = true;
        float    alpha           = 1.F;
    };

    std::vector<SSurface> m_surfaces;
    std::vector<SWindow>  m_windows;
    std::vector<SLayer>   m_layers;
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
    bool     overviewStateValid  = false;
    float    lastOverviewScale   = 1.F;
    int      lastBackdropWallpaperMode = -1;
    Vector2D lastOverviewViewOffset    = Vector2D{};

    SP<Render::IFramebuffer> backdropFB;
    SP<Render::IFramebuffer> cardWallpaperFB;

    void releaseGL() {
        if (backdropFB)
            backdropFB->release();
        backdropFB.reset();
    }
};

class CScrollOverview : public IOverview {
  public:
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
    bool   hasVisiblePrecomputedBlurWindow(PHLMONITOR monitor, size_t activeIdx, float workspacePitch, float renderScale) const;
    void   renderWindowLive(PHLMONITOR monitor, PHLWINDOW window, const CBox& windowBox, float renderScale, const Time::steady_tp& now, const CBox* workspaceBox = nullptr,
                             bool dragged = false, float alphaMul = 1.F);
    void   renderDraggedWindow(PHLMONITOR monitor, size_t activeIdx, float workspacePitch, float renderScale, const Time::steady_tp& now);
    void   renderPinnedFloatingWindows(PHLMONITOR monitor, float overviewScale, const Time::steady_tp& now);
    float        workspaceScrollingSpanRatio(const PHLWORKSPACE& workspace, PHLMONITOR monitor) const;
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
    void         renderSearch(PHLMONITOR monitor, float renderScale);
    void   moveViewportWorkspace(bool up);
    void   trackpadSwipeLayout(const PHLWORKSPACE target, const double delta);
    void   trackpadSwipeWorkspace(const double delta);
    void   finishWorkspaceScrollFollow();
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
    void   suspendInputListeners();
    void   useAnimDirection(bool opening);
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
    SP<Hyprutils::Animation::SAnimationPropertyConfig> workspaceInsertFadeConfig;
    SP<Hyprutils::Animation::SAnimationPropertyConfig> workspaceRemoveFadeConfig;
    SP<Hyprutils::Animation::SAnimationPropertyConfig> openAnimConfig;
    SP<Hyprutils::Animation::SAnimationPropertyConfig> closeAnimConfig;
    // X75. Used when a toggle catches the previous one mid-flight; see
    // useAnimDirection for why a reversal cannot use either directional curve.
    SP<Hyprutils::Animation::SAnimationPropertyConfig> reverseAnimConfig;
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

};

bool overviewToggleSushiForSelection();
