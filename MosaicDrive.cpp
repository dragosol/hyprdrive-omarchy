// See MosaicDrive.hpp.
// The std headers clean first, so private=public covers only Hyprland's.
#include <algorithm>
#include <any>
#include <chrono>
#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#define private public
#define protected public
#include <hyprutils/animation/AnimatedVariable.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#undef protected
#undef private

#include "MosaicDrive.hpp"
#include "FlightController.hpp"

namespace ScrollOverview::MosaicDrive {
    void drawAt(const PHLWINDOW& w, const CBox& box) {
        if (!w || !w->m_realPosition || !w->m_realSize)
            return;
        // value(), not the goal: the goal is what the client is sized to
        // (sendWindowSize reads it), and it stays.
        w->m_realPosition->value() = Vector2D{box.x, box.y};
        w->m_realSize->value()     = Vector2D{std::max(4.0, box.w), std::max(4.0, box.h)};
        // AS ANIMATING, or Hyprland will not scale sub-surfaces with the
        // window, and "animating in", or it crops a surface drawn smaller than
        // its client instead of scaling it (FlightController.cpp, the same two
        // bits). Neither variable is handed to the animation manager, so
        // nothing else moves them until release() warps them home.
        w->m_realPosition->m_bIsBeingAnimated = true;
        w->m_realSize->m_bIsBeingAnimated     = true;
        w->m_animatingIn                      = true;
        // What Hyprland's own size callback does on every animated step: the
        // border and shadow follow the drawn box. Without it they stayed at
        // the size of the previous zoom step, an outline inside every window.
        if (w->m_isMapped)
            w->updateWindowDecos();
    }

    CBox layoutBox(const PHLWINDOW& w) {
        if (!w || !w->m_realPosition || !w->m_realSize)
            return {};
        const auto P = w->m_realPosition->goal(), S = w->m_realSize->goal();
        return CBox{P.x, P.y, S.x, S.y};
    }

    void release(const PHLWINDOW& w) {
        if (!w || !w->m_realPosition || !w->m_realSize)
            return;
        w->m_animatingIn                      = false;
        w->m_realPosition->m_bIsBeingAnimated = true;
        w->m_realSize->m_bIsBeingAnimated     = true;
        w->m_realPosition->warp();
        w->m_realSize->warp();
    }

    bool followPan(const PHLWINDOW& w) {
        if (!w || !w->m_realPosition || !w->m_realSize || !w->m_realSize->isBeingAnimated())
            return false;
        // Only the open animation. A restore from the dock flies the window in
        // with its size animating too (and sets m_animatingIn); centring it here
        // took its flight over (user, 2026-10-29: "you replaced my maximize from
        // dock animation").
        if (!w->m_animatingIn || ScrollOverview::Flight::hasFlight(w))
            return false;
        const auto GP = w->m_realPosition->goal(), GS = w->m_realSize->goal(), S = w->m_realSize->value();
        w->m_realPosition->setValue(GP + (GS - S) / 2.0);
        if (w->m_isMapped)
            w->updateWindowDecos();
        return true;
    }
}
