// Host-side unit test for MotionMath.hpp -- the motion arithmetic that used to
// be copied into each module. Needs no compositor: hyprutils maths types only.
//
//   g++ -std=c++2b -O2 -Isrc/motion -o /tmp/motion_math_test
//       tests/unit/motion_math_test.cpp $(pkg-config --cflags --libs hyprutils)
//
// `make check` runs exactly that (tests/host_tests.sh).
//
// WHAT IT DEFENDS. Each shared function replaced one or more copies that lived
// at call sites, and the merge was allowed only because the shared one returns
// THE SAME BITS. So every old copy is kept below VERBATIM (a golden reference,
// named after the file and line it was deleted from) and compared to the
// shared function over a dense grid, with exact equality on the bit patterns --
// never a tolerance. A shared function that rounds one ULP differently from a
// copy it replaced fails here.

#include "MotionMath.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using Hyprutils::Math::CBox;
using Hyprutils::Math::Vector2D;

static long g_compared = 0, g_failed = 0;
static int  g_checks = 0, g_checkFailed = 0;

// Bit equality, so -0 != +0 and two NaNs compare by payload.
template <typename T>
static bool sameBits(const T& a, const T& b) {
    return std::memcmp(&a, &b, sizeof(T)) == 0;
}
static bool sameBox(const CBox& a, const CBox& b) {
    return sameBits(a.x, b.x) && sameBits(a.y, b.y) && sameBits(a.w, b.w) && sameBits(a.h, b.h);
}
static bool sameVec(const Vector2D& a, const Vector2D& b) {
    return sameBits(a.x, b.x) && sameBits(a.y, b.y);
}

// One named comparison run: `bad` is how many of `n` inputs differed.
static void report(const char* what, long n, long bad) {
    ++g_checks;
    g_compared += n;
    if (bad) {
        ++g_checkFailed;
        g_failed += bad;
        std::printf("FAIL %s: %ld of %ld differ\n", what, bad, n);
    }
}

// The input grids. Values: zeros of both signs, tiny, fractional, the pixel
// range, huge, and ragged decimals that do not round-trip through float.
static std::vector<double> valueGrid() {
    std::vector<double> v = {0.0, -0.0, 1e-30, -1e-30, 0.1, -0.1, 0.3333333333333333, 1.0, -1.0, 2.5, 7.77777, 99.999,
                             1920.0, -1080.0, 2560.5, 3840.25, 12345.678901234, -98765.4321, 1e7, -1e7, 1e15, 0.7071067811865476};
    for (int i = -40; i <= 40; ++i)
        v.push_back(i * 37.123456789);
    return v;
}
// t: a dense sweep of 0..1 plus the far side of both ends (an ease that
// overshoots hands lerp t outside 0..1).
static std::vector<float> tGrid() {
    std::vector<float> t;
    for (int i = -64; i <= 1088; ++i)
        t.push_back(static_cast<float>(i) / 1024.F);
    t.push_back(-0.F);
    t.push_back(1e-7F);
    t.push_back(0.99999994F);
    t.push_back(1.0000001F);
    return t;
}

// ============================================================================
// A1 -- lerp. Golden copies of the deleted definitions, verbatim.
// ============================================================================
namespace old_a1 {
    // dock/Dock.cpp:60
    CBox dockLerpBox(const CBox& a, const CBox& b, float t) {
        return CBox{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.w + (b.w - a.w) * t, a.h + (b.h - a.h) * t};
    }
    // motion/FlightController.cpp:334
    CBox flightLerpBox(const CBox& a, const CBox& b, float t) {
        return CBox{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.w + (b.w - a.w) * t, a.h + (b.h - a.h) * t};
    }
    // overview/OverviewGeometry.cpp:259 (overview/GridLayout.cpp:570 forwarded to it)
    CBox geometryLerpBox(const CBox& from, const CBox& to, float t) {
        return CBox{
            from.x + (to.x - from.x) * t,
            from.y + (to.y - from.y) * t,
            from.w + (to.w - from.w) * t,
            from.h + (to.h - from.h) * t,
        };
    }
    // overview/OverviewGeometry.cpp:268 (overview/OverviewInternal.hpp:2600 forwarded to it)
    float hyprlerp(float from, float to, float perc) {
        return (to - from) * perc + from;
    }
    // overview/OverviewGeometry.cpp:272 (overview/OverviewInternal.hpp:2604 forwarded to it)
    Vector2D hyprlerp(const Vector2D& from, const Vector2D& to, float perc) {
        return Vector2D{hyprlerp(static_cast<float>(from.x), static_cast<float>(to.x), perc), hyprlerp(static_cast<float>(from.y), static_cast<float>(to.y), perc)};
    }
}

static void testA1() {
    const auto V = valueGrid();
    const auto T = tGrid();
    long       n = 0, badDock = 0, badFlight = 0, badGeom = 0;
    for (size_t i = 0; i < V.size(); ++i) {
        for (size_t j = 0; j < V.size(); ++j) {
            // Four different edges per box, so x/y/w/h are not all one case.
            const CBox A{V[i], V[j], V[(i + 3) % V.size()], V[(j + 7) % V.size()]};
            const CBox B{V[j], V[(i + 5) % V.size()], V[(j + 11) % V.size()], V[i]};
            for (float t : T) {
                const CBox NEW = Hyprdrive::Motion::lerp(A, B, t);
                ++n;
                badDock += !sameBox(NEW, old_a1::dockLerpBox(A, B, t));
                badFlight += !sameBox(NEW, old_a1::flightLerpBox(A, B, t));
                badGeom += !sameBox(NEW, old_a1::geometryLerpBox(A, B, t));
            }
        }
    }
    report("A1 lerp(CBox) == Dock.cpp lerpBox", n, badDock);
    report("A1 lerp(CBox) == FlightController.cpp lerpBox", n, badFlight);
    report("A1 lerp(CBox) == OverviewGeometry.cpp lerpBox", n, badGeom);

    long nf = 0, badF = 0, nv = 0, badV = 0;
    for (size_t i = 0; i < V.size(); ++i) {
        for (size_t j = 0; j < V.size(); ++j) {
            const float    A = static_cast<float>(V[i]), B = static_cast<float>(V[j]);
            const Vector2D P{V[i], V[j]}, Q{V[j], V[(i + 3) % V.size()]};
            for (float t : T) {
                ++nf;
                badF += !sameBits(Hyprdrive::Motion::lerp(A, B, t), old_a1::hyprlerp(A, B, t));
                ++nv;
                badV += !sameVec(Hyprdrive::Motion::lerp(P, Q, t), old_a1::hyprlerp(P, Q, t));
            }
        }
    }
    report("A1 lerp(float) == OverviewGeometry.cpp hyprlerp(float)", nf, badF);
    report("A1 lerp(Vector2D) == OverviewGeometry.cpp hyprlerp(Vector2D)", nv, badV);
}

int main() {
    testA1();

    std::printf("motion_math_test: %d checks, %ld bitwise comparisons, %ld differ\n", g_checks, g_compared, g_failed);
    return g_checkFailed ? 1 : 0;
}
