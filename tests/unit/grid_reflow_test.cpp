// Host test for GridReflowModel.hpp: the overview grid's reflow ease and the
// size filter in front of it (the "tiles shake at rest" reports, 2026-11).
//
// THE FORCED REPRODUCTION. The bug never reproduced on the rig, so the test
// builds it: a tile at rest, owner frames 16 ms apart, and between two of them
// ONE box query that sees the window 15 px smaller. The old gridReflowBox is
// kept below VERBATIM in shape (oldReflowBox: the same statements, with the
// globals it read handed in) as the CONTROL -- on it, the next frame draws the
// tile off its slot and it eases back; on the model, nothing moves. A control
// that stopped failing would mean this test can no longer see the bug.
//
// The ease is the house decel curve: its steep start is why a retarget's
// first frame already shows a good part of the jump.
#include "GridReflowModel.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <unordered_map>

using namespace Hyprdrive::GridReflow;

static int g_checks = 0, g_failed = 0;
#define CHECK(c)                                                                                                                                                                   \
    do {                                                                                                                                                                           \
        ++g_checks;                                                                                                                                                                \
        if (!(c)) {                                                                                                                                                                \
            ++g_failed;                                                                                                                                                            \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c);                                                                                                               \
        }                                                                                                                                                                          \
    } while (0)

// ---------------------------------------------------------------- harness
// Time is a double in ms; the animation clock runs at anim_scale 1.
using TP           = double;
static double g_now = 0.0;
static const auto ELAPSED = [](const TP& since) { return static_cast<float>(g_now - since); };
// The house pair (MotionPresets.hpp "hyprdrive": decel 0.16,1 0.3,1, accel
// 0.7,0 0.84,0), solved here by bisection: the steep start is the point.
static float bezier(float x1, float y1, float x2, float y2, float x) {
    const auto AT = [](float p1, float p2, float u) { return 3 * (1 - u) * (1 - u) * u * p1 + 3 * (1 - u) * u * u * p2 + u * u * u; };
    float      lo = 0, hi = 1;
    for (int i = 0; i < 40; ++i) {
        const float MID = (lo + hi) / 2;
        (AT(x1, x2, MID) < x ? lo : hi) = MID;
    }
    return AT(y1, y2, (lo + hi) / 2);
}
static const auto EASE = [](float t, bool exiting) {
    const float T = std::clamp(t, 0.F, 1.F);
    if (T >= 1.F)
        return 1.F;
    return exiting ? bezier(0.7F, 0.F, 0.84F, 0.F, T) : bezier(0.16F, 1.F, 0.3F, 1.F, T);
};

static constexpr float DURATION = 300.F; // mission_control_reflow_ms default
static constexpr double FRAME_MS = 16.0;

static double maxEdgeDelta(const CBox& a, const CBox& b) {
    return std::max({std::abs(a.x - b.x), std::abs(a.y - b.y), std::abs(a.w - b.w), std::abs(a.h - b.h)});
}

// A two-window row, as the solver lays one out: each window scaled by the
// same factor, centred in `usable`, `gap` apart. Enough to show what a size
// does to the window's own slot AND to its neighbour's.
struct SRow {
    CBox a, b;
};
static SRow layRow(const Vector2D& sa, const Vector2D& sb) {
    const double USABLE_W = 1800, USABLE_H = 900, GAP = 24, X0 = 60, Y0 = 120;
    const double SCALE    = std::min((USABLE_W - GAP) / (sa.x + sb.x), USABLE_H / std::max(sa.y, sb.y));
    const double ROWW     = (sa.x + sb.x) * SCALE + GAP;
    const double X        = X0 + (USABLE_W - ROWW) / 2.0;
    return SRow{CBox{X, Y0, sa.x * SCALE, sa.y * SCALE}, CBox{X + sa.x * SCALE + GAP, Y0, sb.x * SCALE, sb.y * SCALE}};
}

// ---------------------------------------------------------------- the control
// The OLD gridReflowBox (GridLayout.cpp at a4d6892, lines 597-661), verbatim in
// its statements. What it read from globals -- the reflow map entry, the frame
// counter, the clock, the "needs another frame" flag -- is a parameter here.
struct SOldReflow {
    bool     alive = false; // stands in for `window.lock() == window`
    CBox     from;
    CBox     to;
    double   start        = 0;
    uint64_t lastFrame    = 0;
    bool     promotedLast = false;
};
static bool gridBoxesDiffer(const CBox& a, const CBox& b) {
    return std::abs(a.x - b.x) > 0.5 || std::abs(a.y - b.y) > 0.5 || std::abs(a.w - b.w) > 0.5 || std::abs(a.h - b.h) > 0.5;
}
static CBox oldReflowBox(SOldReflow& entry, uint64_t FRAME, const CBox& slot, const CBox* enterFrom, bool promoted, bool exiting) {
    const int  DURATION_ = static_cast<int>(DURATION);
    const auto NOW       = g_now;
    const bool KNOWN     = entry.alive;
    const bool INFLIGHT  = KNOWN && (g_now - entry.start) < static_cast<float>(DURATION_);
    const bool UNDRAWN   = KNOWN && !INFLIGHT && FRAME - entry.lastFrame > 1;
    const bool JOINED    = KNOWN && promoted && !entry.promotedLast;

    if (!KNOWN || UNDRAWN || JOINED) {
        const CBox START = enterFrom ? *enterFrom : slot;
        entry            = SOldReflow{true, START, slot, NOW, FRAME, promoted};
        return START;
    }

    entry.lastFrame    = FRAME;
    entry.promotedLast = promoted;

    const float ELAPSED_ = static_cast<float>(g_now - entry.start);
    const float T        = EASE(ELAPSED_ / static_cast<float>(DURATION_), exiting);
    const CBox  CURRENT  = Hyprdrive::Motion::lerp(entry.from, entry.to, T);

    if (gridBoxesDiffer(slot, entry.to)) {
        entry.from  = CURRENT; // retarget from where the tile actually is
        entry.to    = slot;
        entry.start = NOW;
        return CURRENT;
    }

    return CURRENT;
}

// ---------------------------------------------------------------- the model, as GridLayout.cpp drives it
struct SNewTile {
    SEase<TP>      ease;
    bool           known = false;
    SSizeTrack<TP> size;
    bool           sizeKnown = false;
};
static CBox ownerStep(SNewTile& t, uint64_t frame, const CBox& slot) {
    CBox out;
    step(t.ease, t.known, SAsk{slot, nullptr, false, false, frame, DURATION}, g_now, ELAPSED, EASE, out);
    t.known = true;
    return out;
}
static CBox query(const SNewTile& t, uint64_t frame, const CBox& slot) {
    return peek(t.known ? &t.ease : nullptr, t.known, SAsk{slot, nullptr, false, false, frame, DURATION}, ELAPSED, EASE);
}
// The size the grid lays the window out with this frame. `value` is the size
// animation's current value (default: no animation, value == goal).
static Vector2D ownerSize(SNewTile& t, uint64_t frame, const Vector2D& goal, ESize* result = nullptr, const Vector2D* value = nullptr) {
    const auto R = observe(t.size, t.sizeKnown, goal, value ? *value : goal, frame, g_now, ELAPSED);
    t.sizeKnown  = true;
    if (result)
        *result = R;
    return t.size.input;
}

// Hyprland's size animation, as the grid sees it: from `from` to `to` over
// `ms`, eased; on its last tick the value is warped onto the goal exactly.
static Vector2D animValue(const Vector2D& from, const Vector2D& to, double startMs, double ms) {
    const double T = (g_now - startMs) / ms;
    if (T >= 1.0)
        return to;
    const float E = EASE(static_cast<float>(T), false);
    return Vector2D{from.x + (to.x - from.x) * E, from.y + (to.y - from.y) * E};
}

int main() {
    const Vector2D SA{900, 600}, SB{700, 500};
    const Vector2D SA_SMALL{SA.x - 15, SA.y - 15}; // the transient a query sees
    const SRow     REST  = layRow(SA, SB);
    const SRow     BLIP  = layRow(SA_SMALL, SB);
    CHECK(maxEdgeDelta(REST.a, BLIP.a) > 5.0); // the transient really is a different slot
    CHECK(maxEdgeDelta(REST.b, BLIP.b) > 3.0); // ...and moves the row neighbour too

    // ---------------- 1. A query between two frames, transient size: the forced reproduction
    {
        // OLD: settle both tiles at rest, then one query with the small size.
        SOldReflow oa, ob;
        uint64_t   frame = 1;
        g_now            = 0;
        oldReflowBox(oa, frame, REST.a, nullptr, false, false);
        oldReflowBox(ob, frame, REST.b, nullptr, false, false);
        for (int i = 0; i < 40; ++i) { // 640 ms of frames at rest
            g_now += FRAME_MS;
            ++frame;
            oldReflowBox(oa, frame, REST.a, nullptr, false, false);
            oldReflowBox(ob, frame, REST.b, nullptr, false, false);
        }
        g_now += 5; // a commit handler / hit test, 5 ms after the frame
        oldReflowBox(oa, frame, BLIP.a, nullptr, false, false);
        oldReflowBox(ob, frame, BLIP.b, nullptr, false, false);
        g_now += FRAME_MS - 5;
        ++frame;
        const CBox DRAWNA = oldReflowBox(oa, frame, REST.a, nullptr, false, false);
        const CBox DRAWNB = oldReflowBox(ob, frame, REST.b, nullptr, false, false);
        // CONTROL: the old logic draws the tile off its slot on the next frame --
        // the reporters' "shrinks for a frame, then eases back".
        CHECK(maxEdgeDelta(DRAWNA, REST.a) > 1.0);
        CHECK(maxEdgeDelta(DRAWNB, REST.b) > 0.5);
        // ...and it is still off two frames later: an EASED return, not a blip.
        g_now += FRAME_MS;
        ++frame;
        CHECK(maxEdgeDelta(oldReflowBox(oa, frame, REST.a, nullptr, false, false), REST.a) > 0.5);
        std::printf("control: old logic, one transient query -> next frame off by %.2f px (tile), %.2f px (neighbour)\n", maxEdgeDelta(DRAWNA, REST.a),
                    maxEdgeDelta(DRAWNB, REST.b));

        // NEW: the same sequence; the query is a peek.
        SNewTile na, nb;
        frame = 1;
        g_now = 0;
        ownerStep(na, frame, REST.a);
        ownerStep(nb, frame, REST.b);
        for (int i = 0; i < 40; ++i) {
            g_now += FRAME_MS;
            ++frame;
            ownerStep(na, frame, REST.a);
            ownerStep(nb, frame, REST.b);
        }
        g_now += 5;
        const CBox QA = query(na, frame, BLIP.a);
        const CBox QB = query(nb, frame, BLIP.b);
        CHECK(maxEdgeDelta(QA, REST.a) == 0.0); // the query reads where the tile IS
        CHECK(maxEdgeDelta(QB, REST.b) == 0.0);
        g_now += FRAME_MS - 5;
        ++frame;
        CHECK(maxEdgeDelta(ownerStep(na, frame, REST.a), REST.a) == 0.0);
        CHECK(maxEdgeDelta(ownerStep(nb, frame, REST.b), REST.b) == 0.0);
        CHECK(maxEdgeDelta(na.ease.to, REST.a) == 0.0);
    }

    // ---------------- 2. The size filter: a transient size SEEN BY THE OWNER never reaches the grid
    {
        SNewTile t;
        uint64_t frame = 1;
        g_now          = 0;
        ESize    r{};
        CHECK(ownerSize(t, frame, SA, &r) == SA && r == ESize::FIRST);
        for (int i = 0; i < 10; ++i) {
            g_now += FRAME_MS;
            CHECK(ownerSize(t, ++frame, SA, &r) == SA && r == ESize::STEADY);
        }
        // the client shrinks itself by 15 px for two frames (32 ms), then back
        g_now += FRAME_MS;
        CHECK(ownerSize(t, ++frame, SA_SMALL, &r) == SA && r == ESize::PENDING);
        g_now += FRAME_MS;
        CHECK(ownerSize(t, ++frame, SA_SMALL, &r) == SA && r == ESize::PENDING);
        g_now += FRAME_MS;
        CHECK(ownerSize(t, ++frame, SA, &r) == SA && r == ESize::REJECTED);
        // CONTROL: the old input was the size as read, so the solver saw SA_SMALL
        // on those two frames -- and every query in between.

        // ...at 144 Hz, a 45 ms flicker is six owner frames; still never taken
        for (int i = 0; i < 6; ++i) {
            g_now += 7;
            CHECK(ownerSize(t, ++frame, SA_SMALL, &r) == SA);
        }
        g_now += 7;
        CHECK(ownerSize(t, ++frame, SA, &r) == SA && r == ESize::REJECTED);

        // rounding noise at fractional scale: 1 device px is 0.8 logical px at
        // 1.25 and 0.625 at 1.6 -- never even pending
        for (const double D : {0.8, 0.625, 1.0}) {
            g_now += FRAME_MS;
            CHECK(ownerSize(t, ++frame, Vector2D{SA.x - D, SA.y + D}, &r) == SA && r == ESize::STEADY);
        }
        g_now += FRAME_MS;
        CHECK(ownerSize(t, ++frame, SA, &r) == SA && r == ESize::STEADY);

        // a REAL resize: taken once it has held for SIZE_SETTLE_MS
        const Vector2D BIG{1100, 700};
        double         firstSeen = 0, adoptedAt = -1;
        for (int i = 0; i < 10 && adoptedAt < 0; ++i) {
            g_now += FRAME_MS;
            ownerSize(t, ++frame, BIG, &r);
            if (r == ESize::PENDING && firstSeen == 0)
                firstSeen = g_now;
            if (r == ESize::ADOPTED)
                adoptedAt = g_now;
        }
        CHECK(adoptedAt > 0 && t.size.adopted == BIG);
        CHECK(adoptedAt - firstSeen >= SIZE_SETTLE_MS && adoptedAt - firstSeen < SIZE_SETTLE_MS + FRAME_MS);

        // a continuous change (a size that keeps moving) is still taken, every
        // SIZE_SETTLE_MS, not starved
        int adoptions = 0;
        for (int i = 0; i < 30; ++i) {
            g_now += FRAME_MS;
            ownerSize(t, ++frame, Vector2D{BIG.x + 4.0 * (i + 1), BIG.y}, &r);
            adoptions += r == ESize::ADOPTED;
        }
        CHECK(adoptions >= 6);

        // the overview's own resize handle: taken on the frame it happens
        g_now += FRAME_MS;
        CHECK(observe(t.size, true, Vector2D{800, 500}, Vector2D{800, 500}, ++frame, g_now, ELAPSED, true) == ESize::ADOPTED && t.size.input == Vector2D(800, 500));

        // an absence (the overview did not draw this window for a while) takes
        // the size as it is: no settle wait on the frame it comes back
        frame += 5;
        g_now += 500;
        CHECK(ownerSize(t, frame, SA, &r) == SA && r == ESize::FIRST);
    }

    // ---------------- 2b. A flicker WITH its size animation: out and back never reach the grid
    {
        // The goal goes 15 px smaller for 30 ms; Hyprland animates the value
        // toward it (400 ms) and, when the goal comes back, back again. The old
        // input -- the value -- moves the whole time; the grid's must not.
        SNewTile       t;
        uint64_t       frame = 1;
        g_now                = 0;
        ownerSize(t, frame, SA);
        g_now += FRAME_MS;
        ownerSize(t, ++frame, SA);
        const double OUT  = g_now;
        Vector2D     from = SA;
        bool         still = true, oldMoved = false;
        double       back  = -1;
        for (int i = 0; i < 40; ++i) {
            g_now += FRAME_MS;
            const bool AWAY = g_now - OUT < 30;
            if (!AWAY && back < 0) {
                back = g_now;
                from = animValue(SA, SA_SMALL, OUT, 400);
            }
            const Vector2D GOAL  = AWAY ? SA_SMALL : SA;
            const Vector2D VALUE = AWAY ? animValue(SA, SA_SMALL, OUT, 400) : animValue(from, SA, back, 400);
            still                = still && ownerSize(t, ++frame, GOAL, nullptr, &VALUE) == SA;
            oldMoved             = oldMoved || sizesDiffer(VALUE, SA) || std::abs(VALUE.x - SA.x) > 0.5;
        }
        CHECK(still);
        CHECK(oldMoved); // CONTROL: the old input (the value) did move
    }

    // ---------------- 3a. A real resize keeps the OLD motion: it follows the animation, then eases
    {
        // The goal goes to BIG at frame 20 and stays; Hyprland animates the value
        // there over 450 ms. Old: the grid reads the value every frame. New: the
        // change is taken SIZE_SETTLE_MS later, then the value is followed to the
        // end. Following from the first frame would follow a flicker just the
        // same (2b) -- the twitch -- so the first SIZE_SETTLE_MS cannot match;
        // from the adoption on the grid's INPUT is the old one exactly, and the
        // tile comes to rest on the same frame.
        const Vector2D BIG{1100, 700};
        SOldReflow     old;
        SNewTile       neu;
        g_now = 0;
        bool   sameBefore = true, sameInputAfter = true;
        int    adoptFrame = -1, oldFirst = -1, newFirst = -1, oldRest = -1, newRest = -1;
        double changeAt   = 0;
        const CBox FINAL  = layRow(BIG, SB).a;
        CBox       prevO, prevN;
        for (int frame = 1; frame <= 90; ++frame) {
            if (frame == 20)
                changeAt = g_now;
            const Vector2D GOAL  = frame < 20 ? SA : BIG;
            const Vector2D VALUE = frame < 20 ? SA : animValue(SA, BIG, changeAt, 450);
            ESize          r{};
            const Vector2D IN = ownerSize(neu, frame, GOAL, &r, &VALUE);
            if (r == ESize::ADOPTED && adoptFrame < 0)
                adoptFrame = frame;
            if (adoptFrame >= 0)
                sameInputAfter = sameInputAfter && IN == VALUE;
            const CBox O = oldReflowBox(old, frame, layRow(VALUE, SB).a, nullptr, false, false);
            const CBox N = ownerStep(neu, frame, layRow(IN, SB).a);
            if (frame < 20)
                sameBefore = sameBefore && O.x == N.x && O.y == N.y && O.w == N.w && O.h == N.h;
            if (frame > 1) {
                if (oldFirst < 0 && maxEdgeDelta(O, prevO) > 0)
                    oldFirst = frame;
                if (newFirst < 0 && maxEdgeDelta(N, prevN) > 0)
                    newFirst = frame;
            }
            if (maxEdgeDelta(O, FINAL) > 0.5)
                oldRest = frame + 1;
            if (maxEdgeDelta(N, FINAL) > 0.5)
                newRest = frame + 1;
            prevO = O;
            prevN = N;
            g_now += FRAME_MS;
        }
        CHECK(sameBefore);
        CHECK(adoptFrame > 20 && (adoptFrame - 20) * FRAME_MS >= SIZE_SETTLE_MS && (adoptFrame - 21) * FRAME_MS < SIZE_SETTLE_MS);
        CHECK(sameInputAfter);       // the old input, to the bit, from the adoption on
        CHECK(newRest == oldRest);   // and the tile comes to rest on the same frame
        CHECK(newFirst - oldFirst == adoptFrame - 21); // the value first moves on frame 21
        std::printf("real resize: old moves frames %d..%d, new %d..%d (starts %d frames late, ends on the same frame)\n", oldFirst, oldRest, newFirst, newRest,
                    newFirst - oldFirst);
    }

    // ---------------- 3. A real, lasting change reflows with the SAME timing and curve as before
    {
        // The same slot sequence through both: rest at A, the slot becomes B at
        // frame 20 and stays. Every drawn box must be bit-identical.
        const CBox A = REST.a, B = layRow(Vector2D{1100, 700}, SB).a;
        SOldReflow old;
        SNewTile   neu;
        g_now          = 0;
        bool identical = true;
        int  movingFrames = 0;
        for (uint64_t frame = 1; frame <= 60; ++frame) {
            const CBox SLOT = frame < 20 ? A : B;
            const CBox O    = oldReflowBox(old, frame, SLOT, nullptr, false, false);
            const CBox N    = ownerStep(neu, frame, SLOT);
            identical       = identical && O.x == N.x && O.y == N.y && O.w == N.w && O.h == N.h;
            movingFrames += frame >= 20 && maxEdgeDelta(N, B) > 0.5;
            g_now += FRAME_MS;
        }
        CHECK(identical);
        // it really travelled: more than half a pixel off B for several frames
        CHECK(movingFrames >= 5);
        std::printf("real change: old and new drew the same %d travelling frames, bit for bit\n", movingFrames);

        // a continuously moving slot (the search band easing) is followed every
        // frame, exactly as the old logic did
        SOldReflow old2;
        SNewTile   neu2;
        g_now     = 0;
        identical = true;
        for (uint64_t frame = 1; frame <= 40; ++frame) {
            CBox SLOT = A;
            SLOT.y += std::min<double>(frame, 20) * 7.3;
            const CBox O = oldReflowBox(old2, frame, SLOT, nullptr, false, false);
            const CBox N = ownerStep(neu2, frame, SLOT);
            identical    = identical && O.x == N.x && O.y == N.y && O.w == N.w && O.h == N.h;
            g_now += FRAME_MS;
        }
        CHECK(identical);

        // a search promotion seeded from off its card, and JOINED -- same seeds
        SNewTile   p;
        SOldReflow po;
        const CBox FROM{A.x + 2000, A.y, A.w, A.h};
        CBox       out;
        g_now = 0;
        CHECK(step(p.ease, false, SAsk{A, &FROM, true, false, 1, DURATION}, g_now, ELAPSED, EASE, out) == EStep::SEEDED);
        CHECK(out.x == FROM.x && oldReflowBox(po, 1, A, &FROM, true, false).x == FROM.x);
        // a peek before any frame answers with the seed the owner will use
        CHECK(peek<TP>(nullptr, false, SAsk{A, &FROM, true, false, 1, DURATION}, ELAPSED, EASE).x == FROM.x);
    }

    // ---------------- 4. The retarget rule: half a pixel is the same slot
    {
        SNewTile t;
        g_now = 0;
        ownerStep(t, 1, REST.a);
        g_now += 400;
        CBox NEAR = REST.a;
        NEAR.x += 0.4;
        NEAR.w -= 0.4;
        CBox out;
        CHECK(step(t.ease, true, SAsk{NEAR, nullptr, false, false, 2, DURATION}, g_now, ELAPSED, EASE, out) == EStep::SETTLED);
        CBox FAR = REST.a;
        FAR.x += 0.6;
        CHECK(step(t.ease, true, SAsk{FAR, nullptr, false, false, 3, DURATION}, g_now, ELAPSED, EASE, out) == EStep::RETARGETED);
    }

    std::printf("grid_reflow_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
