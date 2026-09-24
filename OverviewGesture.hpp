#pragma once

#include <hyprland/src/devices/IPointer.hpp>
#include <hyprland/src/managers/input/trackpad/gestures/ITrackpadGesture.hpp>
#include <hyprland/src/managers/input/trackpad/gestures/WorkspaceSwipeGesture.hpp>
#include <hyprland/src/managers/input/trackpad/gestures/ScrollMoveGesture.hpp>
#include <hyprland/src/managers/input/trackpad/gestures/SpecialWorkspaceGesture.hpp>
#include <hyprland/src/managers/input/trackpad/gestures/CloseGesture.hpp>
#include <hyprland/src/managers/input/trackpad/gestures/FullscreenGesture.hpp>
#include "IOverview.hpp"

#include <memory>
#include <optional>
#include <vector>

class COverviewGesture : public ITrackpadGesture {
  public:
    explicit COverviewGesture(eTrackpadGestureDirection direction);
    ~COverviewGesture() override = default;

    void begin(const ITrackpadGesture::STrackpadGestureBegin& e) override;
    void update(const ITrackpadGesture::STrackpadGestureUpdate& e) override;
    void end(const ITrackpadGesture::STrackpadGestureEnd& e) override;

  private:
    eTrackpadGestureDirection m_direction   = TRACKPAD_GESTURE_DIR_NONE;
    float                     m_lastDelta   = 0.F;
    bool                      m_firstUpdate = false;
    // Every overview this swipe drives: one per monitor, exactly as tap-SUPER's
    // "toggle all" opens them (user, 2026-09-14).
    std::vector<WP<IOverview>> m_overviews;
};

// 3-finger workspace swipe. Inside an open overview the cards follow the
// fingers; on the desktop this is Hyprland's native workspace swipe.
//
// `workspace_all` (user, 2026-09-23) is the same gesture stepping through EVERY
// workspace number, empty ones included, rather than only the occupied ones.
// Hyprland already has that mode -- gestures:workspace_swipe_use_r, `r+1`
// instead of `m+1` -- but it is global, and turning it on would change the plain
// `workspace` swipe too. So the all-workspaces variant switches it on for the
// length of ONE swipe and puts it back at the end: the native gesture reads it
// on every update and again on release, so begin..end is the window it needs.
class COverviewWorkspaceGesture : public ITrackpadGesture {
  public:
    explicit COverviewWorkspaceGesture(bool allWorkspaces = false) : m_allWorkspaces(allWorkspaces) {}
    ~COverviewWorkspaceGesture() override = default;

    void begin(const ITrackpadGesture::STrackpadGestureBegin& e) override;
    void update(const ITrackpadGesture::STrackpadGestureUpdate& e) override;
    void end(const ITrackpadGesture::STrackpadGestureEnd& e) override;
    bool isDirectionSensitive() override;

  private:
    CWorkspaceSwipeGesture m_native;
    bool                   m_inOverview = false;
    WP<IOverview>          m_overview;

    bool                   m_allWorkspaces = false;
    bool                   m_forcedUseR    = false; // this swipe set use_r, and end() owes it back
    int                    m_prevUseR      = 0;
};

// Swipe through a SCROLLING workspace's columns (user, 2026-09-14). On the
// desktop this is Hyprland's own scroll-move gesture -- momentum, snap to a
// column, focus follows -- on the focused monitor. With the overview up it pans
// the tape of the desktop in view instead, under the fingers, snapping on
// release, because the native one only knows the monitor's active desktop and
// nothing about the card the user is looking at.
class COverviewScrollGesture : public ITrackpadGesture {
  public:
    COverviewScrollGesture()           = default;
    ~COverviewScrollGesture() override = default;

    void begin(const ITrackpadGesture::STrackpadGestureBegin& e) override;
    void update(const ITrackpadGesture::STrackpadGestureUpdate& e) override;
    void end(const ITrackpadGesture::STrackpadGestureEnd& e) override;

  private:
    CScrollMoveTrackpadGesture m_native;
    bool                       m_inOverview = false;
    WP<IOverview>              m_overview;
};

// Fires a keyboard chord (CTRL+T, Print, ...) on gesture end.
class CKeysTrackpadGesture : public ITrackpadGesture {
  public:
    explicit CKeysTrackpadGesture(std::string chord) : m_chord(std::move(chord)) {}
    ~CKeysTrackpadGesture() override = default;

    void begin(const ITrackpadGesture::STrackpadGestureBegin&) override {}
    void update(const ITrackpadGesture::STrackpadGestureUpdate&) override {}
    void end(const ITrackpadGesture::STrackpadGestureEnd&) override;

  private:
    std::string m_chord;
};

void clearClickGestures();
// Plugin unload: the deferred hold-click timer would fire into unloaded code.
void shutdownClickGestures();
void addClickGesture(size_t fingers, const std::string& action, uint32_t mods, bool disableInhibit, const std::string& arg);
void removeClickGesture(size_t fingers, uint32_t mods);
void attachPadFingerListeners();
bool handleClickGestureButton(const IPointer::SButtonEvent& event);
void sendGestureChord(const std::string& chord);

// ONE-WAY GESTURES (user, 2026-09-24: "4f up to reveal and 4f down to close
// scratchpad ... 4f pinchout to fullscreen and 4f pinchin to exit fullscreen
// ... and if not, then close the app (show it gradually closing same thing as
// the regular close window gesture)").
//
// Hyprland's own special-workspace and fullscreen gestures are TOGGLES: the same
// swipe that reveals the scratchpad hides it again. These wrap them and decide
// once, at begin, whether this gesture applies at all -- and when it does, every
// event goes to the native gesture, so the reveal, the fullscreen grow and the
// close shrink all follow the fingers exactly as the native ones do. A gesture
// that does not apply is inert for its whole length. All of them stand down
// while an overview is up, where the fingers belong to the overview.
class CHyprdriveForwardGesture : public ITrackpadGesture {
  public:
    void begin(const ITrackpadGesture::STrackpadGestureBegin& e) override;
    void update(const ITrackpadGesture::STrackpadGestureUpdate& e) override;
    void end(const ITrackpadGesture::STrackpadGestureEnd& e) override;

  protected:
    // The native gesture to run for this begin, or null to do nothing.
    virtual ITrackpadGesture* pick() = 0;

  private:
    ITrackpadGesture* m_active = nullptr;
};

// `scratchpad_show` / `scratchpad_hide`: reveal the default scratchpad when none
// is up; put away the one that is.
class CScratchpadGesture : public CHyprdriveForwardGesture {
  public:
    explicit CScratchpadGesture(bool show) : m_show(show) {}

  protected:
    ITrackpadGesture* pick() override;

  private:
    bool                                      m_show = true;
    std::unique_ptr<CSpecialWorkspaceGesture> m_native;
};

// Hyprland's close gesture with a longer road and a way back (user,
// 2026-09-24: "4f pinchin trajectory needs to be longer for closing window, or
// give the user time to change their mind. if 4f is not released from trackpad
// it shouldn't close the window"). The native one shrinks the window over a
// short travel and closes on ANY end past 20% of it -- including an end
// libinput marks CANCELLED, which is what a pinch that loses a finger
// produces. This runs the native gesture for its animation, but:
//   - the travel is CLOSE_TRAVEL times longer (its delta scale is cut to match);
//   - on end it closes only on a real release (not cancelled) with the
//     fingers still at least CLOSE_COMMIT of the way in -- easing back out
//     before lifting is changing your mind;
//   - otherwise it rewinds the native gesture to zero with one synthetic update,
//     so the native end plays its own spring-back instead of closing.
class CGuardedCloseGesture : public ITrackpadGesture {
  public:
    void begin(const ITrackpadGesture::STrackpadGestureBegin& e) override;
    void update(const ITrackpadGesture::STrackpadGestureUpdate& e) override;
    void end(const ITrackpadGesture::STrackpadGestureEnd& e) override;

  private:
    CCloseTrackpadGesture              m_native;
    float                              m_delta = 0.F; // the native gesture's own running total
    eTrackpadGestureDirection          m_dir   = TRACKPAD_GESTURE_DIR_NONE;
    std::optional<IPointer::SPinchUpdateEvent> m_lastPinch;
    std::optional<IPointer::SSwipeUpdateEvent> m_lastSwipe;
    float                              m_userScale = 1.F;
};

// `fullscreen_on`: fullscreen the focused window, never out of it.
// `unfullscreen_or_close`: out of fullscreen if it is in it, otherwise Hyprland's
// close gesture -- the window shrinks and fades under the fingers and closes
// past the threshold.
class CFullscreenOrCloseGesture : public CHyprdriveForwardGesture {
  public:
    explicit CFullscreenOrCloseGesture(bool enter) : m_enter(enter) {}

  protected:
    ITrackpadGesture* pick() override;

  private:
    bool                       m_enter = true;
    CFullscreenTrackpadGesture m_fullscreen{"fullscreen"};
    CGuardedCloseGesture       m_close;
};
