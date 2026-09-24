#pragma once

#include <hyprland/src/devices/IPointer.hpp>
#include <hyprland/src/managers/input/trackpad/gestures/ITrackpadGesture.hpp>
#include <hyprland/src/managers/input/trackpad/gestures/WorkspaceSwipeGesture.hpp>
#include <hyprland/src/managers/input/trackpad/gestures/ScrollMoveGesture.hpp>
#include "IOverview.hpp"

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
void addClickGesture(size_t fingers, const std::string& action, uint32_t mods, bool disableInhibit, const std::string& arg);
void removeClickGesture(size_t fingers, uint32_t mods);
void attachPadFingerListeners();
bool handleClickGestureButton(const IPointer::SButtonEvent& event);
void sendGestureChord(const std::string& chord);
