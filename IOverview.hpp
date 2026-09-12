#pragma once
#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/SharedDefs.hpp> // WORKSPACEID
#include <hyprland/src/helpers/memory/Memory.hpp>
#include <hyprland/src/helpers/math/Math.hpp>
#include <hyprland/src/helpers/time/Time.hpp>

#include <string>
#include <vector>

class CWLSurfaceResource;

// An overview object exists in one of two phases, and "exists" is not one of
// them: a closing overview stays in the registry until its animation ends
// (the Z18 zombie class is exactly "which flag is authoritative"). Two states,
// so there is nothing here to make unrepresentable that a bool would not --
// what the type buys is that every consumer asks phase() instead of inventing
// its own predicate over the registry.
enum class EOverviewPhase {
    Open,
    Closing,
};

// ROLE: FRAME POLICY. What the compositor's render and damage paths are allowed
// to do while an overview owns the monitor. This is main.cpp's seven hooks and
// nothing else -- they never ask an overview where a tile is, and the dock and
// the minimize flights never ask it whether a surface may have a frame.
class IOverviewFramePolicy {
  public:
    IOverviewFramePolicy()          = default;
    virtual ~IOverviewFramePolicy() = default;

    virtual void render()                                                                          = 0;
    virtual void damage()                                                                          = 0;
    virtual void requestInputFrame()                                                               = 0;
    virtual void onPreRender()                                                                     = 0;
    virtual bool shouldHandleSurfaceDamage(SP<CWLSurfaceResource> surface)                         = 0;
    virtual bool shouldAllowSurfaceFrame(SP<CWLSurfaceResource> surface, const Time::steady_tp& now) = 0;
    virtual bool shouldAllowRealtimePreviewSchedule()                                              = 0;
    virtual bool shouldSuppressRenderDamage() const                                                = 0;

    // Set while the overview is drawing itself, so its own damage does not come
    // back round as a reason to draw again.
    bool blockDamageReporting = false;
};

// ROLE: GEOMETRY. Where the overview is DRAWING things, for the code that has
// to aim at them: the dock's minimize and restore flights, and the tile
// bookkeeping their landings need.
class IOverviewGeometry {
  public:
    IOverviewGeometry()          = default;
    virtual ~IOverviewGeometry() = default;

    // X24. Where a window is DRAWN in the overview, monitor-local render px.
    // The dock is only ever on screen while the overview is, so a minimize
    // started from it has to shrink from the tile the user is looking at, not
    // from the window's real geometry underneath it.
    virtual CBox tileBox(const PHLWINDOW& window) const = 0;
    // Z8. A GLOBAL LOGICAL box transformed into the overview's rendering of a
    // given desktop's card. The restore flight aims a MINIMIZED window here:
    // it has no tile (Y9 reveals minimized windows only under a filter), and
    // the offscreen fallback aimed at the NEXT desktop for a window whose home
    // IS the active one.
    virtual CBox overviewBoxForGlobal(WORKSPACEID workspaceId, const CBox& globalBox) const = 0;
    // V3. The desktop whose STRIP CARD is under a monitor-local render-px point,
    // or null. Asked by the dock's icon drop, which is why it is here rather
    // than left private to the overview: while the overview is up the cards ARE
    // the desktops, and a card is the only thing an icon can be dropped onto.
    // Same point space `tileBox` answers in and `Dock::pillContains` tests, so
    // the drop can ask both without converting anything.
    virtual PHLWORKSPACE stripWorkspaceAt(const Vector2D& point) const = 0;
    // Z20. Rebuild the per-card window lists from live state. setHidden fires
    // no compositor event, so a window unhidden at a flight's landing is
    // otherwise never re-added to workspaceImage->windows -- the grid and
    // hit-test see it (live iteration) while the render (list walk) does not:
    // a clickable, invisible tile.
    virtual void refreshWindows() = 0;
};

class IOverview : public IOverviewFramePolicy, public IOverviewGeometry {
  public:
    IOverview()          = default;
    ~IOverview() override = default;

    // ---- ROLE: LIFECYCLE ---------------------------------------------------
    virtual EOverviewPhase phase() const = 0;

    // Everything that decides whether to open or to close asks this -- otherwise
    // a tap during the close animation reads as "already open" and is thrown
    // away. Not virtual: there is one phase, and one way to read it off.
    bool isClosing() const {
        return phase() == EOverviewPhase::Closing;
    }

    virtual void setClosing(bool closing) = 0;

    // Reverse a close that is still animating. Cheap on purpose: the window
    // images, the blur framebuffers and the input hooks are all still alive at
    // this point, so this re-arms them instead of rebuilding them.
    virtual void reopen() = 0;

    virtual void close() = 0;

    // ---- ROLE: GESTURE -----------------------------------------------------
    virtual void resetSwipe()                = 0;
    virtual void onSwipeUpdate(double delta) = 0;
    virtual void onSwipeEnd()                = 0;
    // 3-finger workspace swipe while the overview is open: cards follow the
    // fingers, then settle onto the nearest desktop. Outside overview this is
    // Hyprland's native workspace swipe.
    virtual void onWorkspaceSwipeBegin()           = 0;
    virtual void onWorkspaceSwipeUpdate(double delta) = 0;
    virtual void onWorkspaceSwipeEnd()             = 0;

    // 0 at the desktop, 1 when the overview is fully open. The 3-finger
    // swipe warps this every update; Super-tap animates it. The dock's
    // autohide slide reads it so the pill cannot run on a second clock.
    virtual float openProgress() const = 0;

    // ---- ROLE: SELECTION ---------------------------------------------------
    virtual void performPrimaryClickAction()                        = 0;
    virtual void selectHoveredWorkspace()                           = 0;
    virtual bool moveSelection(const std::string& direction)        = 0;
    virtual bool windowDispatcherAction(const std::string& action)  = 0;

    // X28. Commit the overview's selection to one window and close, the way a
    // tile click does. The dock's right-click window list needs exactly this:
    // focusing the window directly would be overwritten by close()'s own
    // selection, which is also where X43's raise lives.
    virtual bool selectAndClose(const PHLWINDOW& window) = 0;

    // Start a live window drag from a box that is ALREADY at mini scale -- a
    // dock preview cell. The window grows out of that box on the way out
    // instead of popping to full size for a frame, and from then on the gesture
    // is the ordinary overview window drag: the same shrink at the dock
    // boundary, the same minimize on a drop back into it.
    virtual void beginWindowDragFrom(const PHLWINDOW& window, const CBox& miniOrigin) = 0;

    PHLMONITORREF pMonitor;
    bool          m_isSwiping    = false;
    // This swipe is dismissing an already-open overview. Distinct from
    // `closing`: that flag is the lifecycle (close() has started and will
    // destroy us). The gesture used to set `closing` at swipe-begin so the
    // follow math ran in reverse, and then onSwipeEnd's close() saw the flag
    // and returned without tearing anything down -- overview invisible,
    // still owning the monitor, clicks and menus dead.
    bool          m_swipeClosing = false;
};

const std::vector<SP<IOverview>>& scrollOverviews();
SP<IOverview>                     scrollOverviewForMonitor(PHLMONITOR monitor);
// The registry holds overviews in every phase, including ones still animating
// their close (a "zombie": exists but not open -- see Z18 in Dock.cpp). This is
// the query almost every caller actually wants: the overview on `monitor` only
// if it is OPEN, i.e. registered and not closing. Ask this instead of pairing
// scrollOverviewForMonitor with a hand-written !isClosing() at each call site.
SP<IOverview>                     openScrollOverviewForMonitor(PHLMONITOR monitor);
SP<IOverview>                     scrollOverviewAt(const Vector2D& point);
SP<IOverview>                     activeScrollOverview();
void                              registerScrollOverview(const SP<IOverview>& overview);
void                              unregisterScrollOverview(IOverview* overview);
void                              clearScrollOverviews();

// P0. Drop every overview standing on a monitor that is going away -- and any
// whose monitor has already gone, so this is idempotent and self-healing rather
// than one output's special case.
//
// An overview used to outlive its output: nothing tore it down, so it stayed in
// the registry holding a dead PHLMONITORREF and the next close() walked it
// (`closeOnWindow->m_workspace == pMonitor->m_activeWorkspace`) and took the
// compositor with it. `hyprctl output remove` and a user pulling a DisplayPort
// cable arrive at the same place. Wired to monitor.removed in PLUGIN_INIT.
void                              dropScrollOverviewsForMonitor(PHLMONITOR monitor);

// X32. Open the overview on a monitor -- the Show Applications button is the
// first caller outside the dispatcher itself. Already open, or no monitor: a
// no-op that reports success.
//
// This is the OPEN and nothing else. A fresh open also needs the session reset
// below; they are separate because openOverview() is the reopen path for a
// toggle that lands on a closing overview, and resetting there would drop the
// filter mid-close-animation.
bool                              openScrollOverview(PHLMONITOR monitor);

// Start a NEW search session: no query, no selection, nothing held, no
// half-typed compose sequence. What the dispatcher does before every open, for
// the entry points that are not the dispatcher.
void                              resetSearchSession();
