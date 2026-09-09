#pragma once

#include <hyprland/src/devices/IPointer.hpp>
#include <hyprland/src/managers/input/trackpad/gestures/ITrackpadGesture.hpp>
#include <hyprland/src/managers/input/trackpad/gestures/WorkspaceSwipeGesture.hpp>
#include "IOverview.hpp"

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
    WP<IOverview>             m_overview;
};

// 3-finger workspace swipe. Inside an open overview the cards follow the
// fingers; on the desktop this is Hyprland's native workspace swipe.
class COverviewWorkspaceGesture : public ITrackpadGesture {
  public:
    COverviewWorkspaceGesture()  = default;
    ~COverviewWorkspaceGesture() override = default;

    void begin(const ITrackpadGesture::STrackpadGestureBegin& e) override;
    void update(const ITrackpadGesture::STrackpadGestureUpdate& e) override;
    void end(const ITrackpadGesture::STrackpadGestureEnd& e) override;
    bool isDirectionSensitive() override;

  private:
    CWorkspaceSwipeGesture m_native;
    bool                   m_inOverview = false;
    WP<IOverview>          m_overview;
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
