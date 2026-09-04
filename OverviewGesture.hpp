#pragma once

#include <hyprland/src/managers/input/trackpad/gestures/ITrackpadGesture.hpp>
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
