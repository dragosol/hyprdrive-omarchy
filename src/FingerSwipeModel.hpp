#pragma once

// A SWIPE THAT OUTLIVES ITS FINGERS, AND A SWIPE UNDER A HELD DRAG
// (user, 2026-10-07). Pure: no Hyprland, so scripts/finger_swipe_test.cpp can
// drive it with made-up touch frames. FingerSwipe.cpp is the glue.
//
// Why it has to exist. libinput reports a touchpad gesture as a finger COUNT,
// never as fingers:
//   * lift one finger of a 3/4-finger swipe and, after a 100 ms debounce, the
//     swipe ENDS (cancelled) and a fresh swipe / scroll / pointer state starts
//     from scratch -- the desktop switch snaps back or commits half-way;
//   * hold the clickpad button (dragging a file) and every gesture is
//     suppressed: all fingers become ONE pointer whose motion is the SUM of
//     every touch, so three swiping fingers fling the dragged file instead.
// So the plugin reads the touchpad's own multitouch slots (TouchpadRaw.cpp)
// and decides with real touch identities:
//   Continue   a swipe keeps going while ANY finger that started it is down.
//              libinput's cancel and whatever it starts next are swallowed;
//              the updates come from the remaining original fingers.
//   DragArm    the button is held and 3+ OTHER fingers are down: pointer
//              motion is swallowed (no fling) until they travel armMm,
//   DragSwipe  then they are a swipe of their own, the held finger ignored.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <set>
#include <vector>

namespace ScrollOverview::FingerSwipeModel {

    struct SVec {
        double x = 0, y = 0;
    };

    struct STouch {
        int    id = -1; // the kernel's tracking id
        double x = 0, y = 0; // mm
    };

    // One evdev frame (SYN_REPORT): the touches that are down, and the button.
    struct SFrame {
        std::vector<STouch> touches;
        bool                button = false;
    };

    enum class EMode : uint8_t {
        Idle,
        Native,    // libinput's own swipe, watched
        Continue,  // libinput's swipe, carried on by us after a finger lifted
        DragArm,   // button held + 3/4 other fingers down, not yet moved
        DragSwipe, // ...moved: our swipe
    };

    // What the glue does after one input.
    struct SOut {
        bool              cancel = false; // swallow the libinput event just fed in
        bool              begin  = false; // start a swipe of `fingers` (DragSwipe)
        uint32_t          fingers = 0;
        std::vector<SVec> updates;        // libinput delta units
        bool              end = false;    // end the swipe (not cancelled)
        // Clients saw libinput's begin (pointer-gestures protocol); the end that
        // was just swallowed is theirs too: tell them it was cancelled.
        bool              endClients = false;
    };

    struct SConfig {
        bool   keepSwipe = true;
        bool   dragSwipe = true;
        double armMm     = 1.5; // DragArm -> DragSwipe travel
    };

    class CModel {
      public:
        SConfig cfg;

        EMode mode() const {
            return m_mode;
        }
        uint32_t fingers() const {
            return m_fingers;
        }
        // Pointer motion and finger scroll from the touchpad belong to us.
        bool swallowPointer() const {
            return m_mode == EMode::Continue || m_mode == EMode::DragArm || m_mode == EMode::DragSwipe;
        }
        // libinput delta units per mm, learned from libinput's own swipes.
        double unitsPerMm() const {
            return m_unitsPerMm;
        }

        SOut onSwipeBegin(uint32_t fingers) {
            SOut out;
            if (ours()) {
                m_swallowSwipe = true; // its update/end are ours to drop too
                out.cancel     = true;
                return out;
            }
            m_mode       = EMode::Native;
            m_clientOpen = true;
            m_fingers    = fingers;
            m_original = idsExcept(m_prev, m_buttonIds);
            m_libSum = m_rawSum = 0;
            return out;
        }

        SOut onSwipeUpdate(uint32_t, SVec delta) {
            SOut out;
            if (ours() || m_swallowSwipe) {
                out.cancel = true;
                return out;
            }
            if (m_mode == EMode::Native)
                m_libSum += std::hypot(delta.x, delta.y);
            return out;
        }

        SOut onSwipeEnd(bool cancelled) {
            SOut out;
            if (m_swallowSwipe) {
                m_swallowSwipe = false;
                out.cancel     = true;
                return out;
            }
            if (ours()) {
                out.cancel     = true;
                out.endClients = m_clientOpen;
                m_clientOpen   = false;
                return out;
            }
            if (m_mode == EMode::Native) {
                // libinput gave up on a count change before a frame showed the
                // lift (should not happen: it debounces 100 ms). Same answer.
                if (cancelled && cfg.keepSwipe && !intersect(m_original, m_prev).empty()) {
                    m_mode         = EMode::Continue;
                    out.cancel     = true;
                    out.endClients = m_clientOpen;
                    m_clientOpen   = false;
                    return out;
                }
                m_clientOpen = false;
                learn();
                m_mode = EMode::Idle;
            }
            return out;
        }

        // Any pinch event: a 3->2 lift can turn into one.
        SOut onPinch(bool isEnd) {
            SOut out;
            if (ours() || m_swallowPinch) {
                m_swallowPinch = !isEnd && true;
                out.cancel     = true;
            }
            return out;
        }

        SOut onFrame(const SFrame& f) {
            SOut out;

            // The finger(s) that pressed the button are the drag, whatever
            // happens next; a release frees them.
            if (f.button && !m_prevButton)
                m_buttonIds = ids(f);
            else if (!f.button)
                m_buttonIds.clear();
            m_prevButton = f.button;

            switch (m_mode) {
                case EMode::Native: {
                    const auto LEFT = intersect(m_original, f);
                    m_rawSum += len(delta(LEFT, f));
                    if (cfg.keepSwipe && !LEFT.empty() && LEFT.size() < m_original.size()) {
                        m_mode = EMode::Continue;
                        push(out, delta(LEFT, f));
                    }
                    break;
                }
                case EMode::Continue:
                case EMode::DragSwipe: {
                    const auto LEFT = intersect(m_original, f);
                    if (LEFT.empty()) {
                        out.end = true;
                        m_mode  = EMode::Idle;
                        break;
                    }
                    push(out, delta(LEFT, f));
                    break;
                }
                case EMode::DragArm: {
                    const auto EXTRA = idsExcept(f, m_buttonIds);
                    if (!f.button || EXTRA.size() < 3) {
                        m_mode = EMode::Idle;
                        break;
                    }
                    if (EXTRA.size() > m_original.size()) {
                        m_original = EXTRA; // a 4th finger landed: a 4-finger swipe
                        m_fingers  = sc32(EXTRA.size());
                    }
                    const auto D = delta(intersect(m_original, f), f);
                    m_arm.x += D.x;
                    m_arm.y += D.y;
                    if (len(m_arm) >= cfg.armMm) {
                        m_mode      = EMode::DragSwipe;
                        out.begin   = true;
                        out.fingers = m_fingers;
                        push(out, m_arm);
                    }
                    break;
                }
                case EMode::Idle: {
                    const auto EXTRA = idsExcept(f, m_buttonIds);
                    if (cfg.dragSwipe && f.button && !m_buttonIds.empty() && EXTRA.size() >= 3) {
                        m_mode     = EMode::DragArm;
                        m_original = EXTRA;
                        m_fingers  = sc32(std::min<size_t>(EXTRA.size(), 4));
                        m_arm      = {};
                    }
                    break;
                }
            }

            m_prev = f;
            out.fingers = out.fingers ? out.fingers : m_fingers;
            return out;
        }

        // The raw stream went away (device lost, watchdog): finish what we hold.
        SOut abort() {
            SOut out;
            if (m_mode == EMode::Continue || m_mode == EMode::DragSwipe)
                out.end = true;
            m_mode = EMode::Idle;
            m_prev = {};
            m_buttonIds.clear();
            m_prevButton = false;
            return out;
        }

      private:
        EMode         m_mode    = EMode::Idle;
        uint32_t      m_fingers = 0;
        std::set<int> m_original, m_buttonIds;
        SFrame        m_prev;
        bool          m_prevButton   = false;
        bool          m_swallowSwipe = false, m_swallowPinch = false;
        bool          m_clientOpen   = false; // clients are inside libinput's swipe
        SVec          m_arm;
        double        m_libSum = 0, m_rawSum = 0;
        double        m_unitsPerMm = 30.0;

        bool ours() const {
            return m_mode == EMode::Continue || m_mode == EMode::DragArm || m_mode == EMode::DragSwipe;
        }
        static uint32_t sc32(size_t n) {
            return static_cast<uint32_t>(n);
        }
        static double len(SVec v) {
            return std::hypot(v.x, v.y);
        }
        static std::set<int> ids(const SFrame& f) {
            std::set<int> s;
            for (const auto& T : f.touches)
                s.insert(T.id);
            return s;
        }
        static std::set<int> idsExcept(const SFrame& f, const std::set<int>& skip) {
            std::set<int> s;
            for (const auto& T : f.touches)
                if (!skip.contains(T.id))
                    s.insert(T.id);
            return s;
        }
        static std::set<int> intersect(const std::set<int>& a, const SFrame& f) {
            std::set<int> s;
            for (const auto& T : f.touches)
                if (a.contains(T.id))
                    s.insert(T.id);
            return s;
        }
        // The mean motion of `which` between the previous frame and `f`, mm.
        SVec delta(const std::set<int>& which, const SFrame& f) const {
            SVec   sum;
            size_t n = 0;
            for (const auto& T : f.touches) {
                if (!which.contains(T.id))
                    continue;
                const auto P = std::ranges::find_if(m_prev.touches, [&](const STouch& p) { return p.id == T.id; });
                if (P == m_prev.touches.end())
                    continue;
                sum.x += T.x - P->x;
                sum.y += T.y - P->y;
                ++n;
            }
            if (n)
                sum = {sum.x / n, sum.y / n};
            return sum;
        }
        void push(SOut& out, SVec mm) const {
            if (mm.x == 0 && mm.y == 0)
                return;
            out.updates.push_back({mm.x * m_unitsPerMm, mm.y * m_unitsPerMm});
        }
        // A finished native swipe teaches the scale: libinput's (accelerated)
        // units over the raw millimetres, smoothed so one odd swipe cannot
        // throw a continued one off.
        void learn() {
            if (m_rawSum < 8.0 || m_libSum <= 0)
                return;
            const double R = std::clamp(m_libSum / m_rawSum, 5.0, 200.0);
            m_unitsPerMm   = 0.7 * m_unitsPerMm + 0.3 * R;
        }
    };
}
