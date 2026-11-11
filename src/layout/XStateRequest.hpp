#pragma once
// WHAT TO DO WITH AN X11 _NET_WM_STATE MAXIMIZE / FULLSCREEN REQUEST
// (XGeometry.hpp, "A GAME FLIPPING MAXIMIZED"), kept apart from Hyprland so a
// host test can hold it (tests/unit/x_state_request_test.cpp).
//
// Hyprland 0.56 (CXWM::handleClientMessage + CWindow::onUpdateState) turns
// any message naming _NET_WM_STATE_MAXIMIZED_VERT/HORZ into a TOGGLE between
// maximized and not -- ADD on a maximized window un-maximizes it, REMOVE on a
// window that is not maximized maximizes it. FULLSCREEN is honoured as ADD /
// REMOVE / TOGGLE already, so only the maximize half of a message is judged
// here: it is stripped when it asks for the state the window already has, so
// Hyprland's toggle never sees it. A window whose messages keep flipping its
// state (more than FLIPS in WINDOW) has its maximize and fullscreen requests
// stripped for BLOCK; nothing else is touched.
#include <cstdint>
#include <deque>

namespace Hyprdrive::XStateRequest {
    // Hyprland's eFullscreenMode for the client side, as far as this cares.
    enum class eMode : uint8_t {
        NONE,
        MAXIMIZED,
        FULLSCREEN,
    };

    // _NET_WM_STATE's data32[0].
    constexpr uint32_t REMOVE = 0, ADD = 1, TOGGLE = 2;

    struct SRequest {
        uint32_t action     = ADD;
        bool     maximize   = false; // data32[1] or [2] names MAXIMIZED_VERT/HORZ
        bool     fullscreen = false; // ... names FULLSCREEN
    };

    struct SVerdict {
        bool stripMaximize   = false; // clear the MAXIMIZED atoms before Hyprland reads it
        bool stripFullscreen = false; // clear the FULLSCREEN atom
        bool blocked         = false; // the storm guard is why
    };

    // The client mode once Hyprland has applied the FULLSCREEN half of `r`
    // (onUpdateState does FULLSCREEN first, then maximize).
    inline eMode afterFullscreen(const SRequest& r, eMode current) {
        if (!r.fullscreen)
            return current;
        switch (r.action) {
            case ADD: return eMode::FULLSCREEN;
            case REMOVE: return current == eMode::FULLSCREEN ? eMode::NONE : current;
            case TOGGLE: return current == eMode::FULLSCREEN ? eMode::NONE : eMode::FULLSCREEN;
            default: // Hyprland reads an unknown action as "off"
                return current == eMode::FULLSCREEN ? eMode::NONE : current;
        }
    }

    // Would Hyprland's maximize toggle do the wrong thing with this request?
    // ADD on a maximized window, REMOVE on one that is not, and an action
    // that is none of the three. TOGGLE stays a toggle. A fullscreen window
    // is left to Hyprland (it ignores maximize there).
    inline bool maximizeIsNoop(const SRequest& r, eMode current) {
        if (!r.maximize)
            return false;
        const eMode MID = afterFullscreen(r, current);
        if (MID == eMode::FULLSCREEN)
            return false;
        switch (r.action) {
            case ADD: return MID == eMode::MAXIMIZED;
            case REMOVE: return MID == eMode::NONE;
            case TOGGLE: return false;
            default: return true;
        }
    }

    // One window's recent flips: a flip is a client message that changed its
    // client mode. Times are milliseconds on any monotonic clock.
    class CStormGuard {
      public:
        static constexpr int     FLIPS  = 6;    // more than this many...
        static constexpr int64_t WINDOW = 1000; // ...within this long is a storm
        static constexpr int64_t BLOCK  = 4000; // its requests are ignored this long

        bool                     blocked(int64_t now) const {
            return m_blockedUntil != 0 && now < m_blockedUntil;
        }

        // A client message changed the window's mode at `now`. True when this
        // flip starts a block (log it then, once).
        bool noteFlip(int64_t now) {
            while (!m_flips.empty() && now - m_flips.front() >= WINDOW)
                m_flips.pop_front();
            m_flips.push_back(now);
            if (blocked(now) || count() <= FLIPS)
                return false;
            m_blockedUntil = now + BLOCK;
            m_flips.clear();
            return true;
        }

        // Nothing has happened for a while: the entry can go.
        bool idle(int64_t now) const {
            return !blocked(now) && (m_flips.empty() || now - m_flips.back() >= WINDOW);
        }

      private:
        int count() const {
            return static_cast<int>(m_flips.size());
        }
        std::deque<int64_t> m_flips;
        int64_t             m_blockedUntil = 0;
    };

    inline SVerdict decide(const SRequest& r, eMode current, const CStormGuard& guard, int64_t now) {
        SVerdict v;
        if (!r.maximize && !r.fullscreen)
            return v;
        if (guard.blocked(now)) {
            v.stripMaximize   = r.maximize;
            v.stripFullscreen = r.fullscreen;
            v.blocked         = true;
            return v;
        }
        v.stripMaximize = maximizeIsNoop(r, current);
        return v;
    }
}
