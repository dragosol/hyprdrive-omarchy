// FingerSwipeModel: a swipe that outlives lifted fingers, and a swipe made
// under a held drag. Frames are made up; the event ORDER follows libinput 1.31
// (a count change ends the swipe as cancelled 100 ms after the raw lift).
#include "../FingerSwipeModel.hpp"

#include <cstdio>
#include <string>

using namespace ScrollOverview::FingerSwipeModel;

static int g_fail = 0, g_checks = 0;
static void check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) {
        ++g_fail;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

static SFrame frame(std::initializer_list<STouch> t, bool button = false) {
    return SFrame{std::vector<STouch>(t), button};
}

static double sumX(const SOut& o) {
    double s = 0;
    for (const auto& U : o.updates)
        s += U.x;
    return s;
}

int main() {
    // 1. Plain 3-finger swipe, all fingers lift together: untouched.
    {
        CModel m;
        m.onFrame(frame({{1, 10, 30}, {2, 20, 30}, {3, 30, 30}}));
        check(!m.onSwipeBegin(3).cancel, "1: begin passes");
        m.onFrame(frame({{1, 15, 30}, {2, 25, 30}, {3, 35, 30}}));
        check(!m.onSwipeUpdate(3, {150, 0}).cancel, "1: update passes");
        check(m.mode() == EMode::Native, "1: native");
        m.onFrame(frame({}));
        check(m.mode() == EMode::Native, "1: all lifted at once stays native");
        check(!m.onSwipeEnd(false).cancel, "1: end passes");
        check(m.mode() == EMode::Idle, "1: idle after");
        check(!m.swallowPointer(), "1: pointer free");
    }

    // 2. 4-finger swipe, one finger lifts, the rest carry on, then lift.
    {
        CModel m;
        m.onFrame(frame({{1, 10, 30}, {2, 20, 30}, {3, 30, 30}, {4, 40, 30}}));
        m.onSwipeBegin(4);
        m.onFrame(frame({{1, 12, 30}, {2, 22, 30}, {3, 32, 30}, {4, 42, 30}}));
        m.onSwipeUpdate(4, {60, 0});
        auto o = m.onFrame(frame({{1, 14, 30}, {2, 24, 30}, {3, 34, 30}})); // 4 lifts
        check(m.mode() == EMode::Continue, "2: continue after a lift");
        check(sumX(o) > 0, "2: the lift frame still moves");
        check(m.swallowPointer(), "2: pointer swallowed while continuing");
        {
            const auto E = m.onSwipeEnd(true);
            check(E.cancel, "2: libinput's cancel is swallowed");
            check(E.endClients, "2: ...but clients are told their swipe ended");
        }
        check(m.onSwipeBegin(3).cancel, "2: libinput's re-detected 3-finger begin is swallowed");
        check(m.onSwipeUpdate(3, {10, 0}).cancel, "2: and its updates");
        o = m.onFrame(frame({{1, 20, 30}}));                     // down to one original
        check(m.mode() == EMode::Continue && sumX(o) > 0, "2: one original finger still drives it");
        check(o.fingers == 4, "2: updates carry the original count");
        o = m.onFrame(frame({}));
        check(o.end && m.mode() == EMode::Idle, "2: last original lifts -> end");
        {
            const auto E = m.onSwipeEnd(true);
            check(E.cancel && !E.endClients, "2: the swallowed swipe's end is swallowed too, clients never saw it");
        }
        check(!m.swallowPointer(), "2: pointer free again");
    }

    // 3. A NEW finger landing during a continued swipe does not keep it alive.
    {
        CModel m;
        m.onFrame(frame({{1, 10, 30}, {2, 20, 30}, {3, 30, 30}}));
        m.onSwipeBegin(3);
        m.onFrame(frame({{1, 11, 30}, {2, 21, 30}}));
        check(m.mode() == EMode::Continue, "3: continue");
        m.onFrame(frame({{1, 12, 30}, {9, 50, 30}}));
        auto o = m.onFrame(frame({{9, 55, 30}}));
        check(o.end && m.mode() == EMode::Idle, "3: only a newcomer left -> end");
    }

    // 4. keepSwipe off: native behaviour.
    {
        CModel m;
        m.cfg.keepSwipe = false;
        m.onFrame(frame({{1, 10, 30}, {2, 20, 30}, {3, 30, 30}}));
        m.onSwipeBegin(3);
        m.onFrame(frame({{1, 11, 30}, {2, 21, 30}}));
        check(m.mode() == EMode::Native, "4: no continue when off");
        check(!m.onSwipeEnd(true).cancel, "4: cancel passes through");
    }

    // 5. Held drag + 3 more fingers swiping = a 3-finger swipe, drag kept.
    {
        CModel m;
        m.onFrame(frame({{1, 50, 60}}, false));
        m.onFrame(frame({{1, 50, 60}}, true)); // click and hold: finger 1 is the drag
        check(m.mode() == EMode::Idle && !m.swallowPointer(), "5: a plain drag is left alone");
        m.onFrame(frame({{1, 50, 60}, {2, 20, 20}, {3, 30, 20}}, true));
        check(m.mode() == EMode::Idle, "5: two extra fingers are not a swipe");
        m.onFrame(frame({{1, 50, 60}, {2, 20, 20}, {3, 30, 20}, {4, 40, 20}}, true));
        check(m.mode() == EMode::DragArm && m.swallowPointer(), "5: three extra fingers arm (pointer swallowed)");
        auto o = m.onFrame(frame({{1, 50, 60}, {2, 20.5, 20}, {3, 30.5, 20}, {4, 40.5, 20}}, true));
        check(!o.begin && m.mode() == EMode::DragArm, "5: below the arm travel");
        o = m.onFrame(frame({{1, 51, 60}, {2, 23, 20}, {3, 33, 20}, {4, 43, 20}}, true));
        check(o.begin && o.fingers == 3 && m.mode() == EMode::DragSwipe, "5: begins a 3-finger swipe");
        check(sumX(o) > 0, "5: with the travel so far");
        o = m.onFrame(frame({{1, 51, 60}, {2, 30, 20}, {3, 40, 20}}, true)); // one swiping finger lifts
        check(m.mode() == EMode::DragSwipe && sumX(o) > 0, "5: continues past a lift");
        o = m.onFrame(frame({{1, 51, 60}}, true));
        check(o.end && m.mode() == EMode::Idle, "5: swipe fingers gone -> end, drag finger ignored");
        check(!m.swallowPointer(), "5: the drag moves the pointer again");
        check(m.onSwipeEnd(false).cancel == false, "5: nothing of libinput's was swallowed");
    }

    // 6. Held drag + 4 extra fingers = a 4-finger swipe.
    {
        CModel m;
        m.onFrame(frame({{1, 50, 60}}, true));
        m.onFrame(frame({{1, 50, 60}, {2, 20, 20}, {3, 30, 20}, {4, 40, 20}}, true));
        m.onFrame(frame({{1, 50, 60}, {2, 20, 20}, {3, 30, 20}, {4, 40, 20}, {5, 10, 25}}, true));
        auto o = m.onFrame(frame({{1, 50, 60}, {2, 20, 24}, {3, 30, 24}, {4, 40, 24}, {5, 10, 29}}, true));
        check(o.begin && o.fingers == 4, "6: 4-finger swipe under a drag");
    }

    // 7. dragSwipe off, and releasing the button while armed.
    {
        CModel m;
        m.cfg.dragSwipe = false;
        m.onFrame(frame({{1, 50, 60}}, true));
        m.onFrame(frame({{1, 50, 60}, {2, 20, 20}, {3, 30, 20}, {4, 40, 20}}, true));
        check(m.mode() == EMode::Idle, "7: off");
        CModel n;
        n.onFrame(frame({{1, 50, 60}}, true));
        n.onFrame(frame({{1, 50, 60}, {2, 20, 20}, {3, 30, 20}, {4, 40, 20}}, true));
        n.onFrame(frame({{1, 50, 60}, {2, 20, 20}, {3, 30, 20}, {4, 40, 20}}, false));
        check(n.mode() == EMode::Idle && !n.swallowPointer(), "7: button released while armed -> idle");
    }

    // 8. abort() ends what we hold.
    {
        CModel m;
        m.onFrame(frame({{1, 10, 30}, {2, 20, 30}, {3, 30, 30}}));
        m.onSwipeBegin(3);
        m.onFrame(frame({{1, 11, 30}, {2, 21, 30}}));
        check(m.abort().end && m.mode() == EMode::Idle, "8: abort ends a continued swipe");
    }

    // 9. The scale is learned from native swipes.
    {
        CModel m;
        const double BEFORE = m.unitsPerMm();
        m.onFrame(frame({{1, 0, 30}, {2, 10, 30}, {3, 20, 30}}));
        m.onSwipeBegin(3);
        for (int i = 1; i <= 10; ++i) {
            m.onFrame(frame({{1, 2.0 * i, 30}, {2, 10 + 2.0 * i, 30}, {3, 20 + 2.0 * i, 30}}));
            m.onSwipeUpdate(3, {120, 0}); // 60 units per mm
        }
        m.onFrame(frame({}));
        m.onSwipeEnd(false);
        check(m.unitsPerMm() > BEFORE, "9: learned toward 60 u/mm");
    }

    std::printf("finger_swipe_test: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
