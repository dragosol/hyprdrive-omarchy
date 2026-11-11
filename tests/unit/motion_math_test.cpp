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

#include "MotionClock.hpp"
#include "MotionMath.hpp"

#include <chrono>
#include <optional>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using Hyprutils::Math::CBox;
using Hyprutils::Math::Vector2D;

static long g_compared = 0, g_failed = 0;
static int  g_checks = 0, g_checkFailed = 0;

// Every golden copy is [[gnu::noinline]] and every value goes through a
// compiler barrier before it is compared: with both sides inlined, GCC sees two
// identical expressions, folds the comparison to "equal" and the test measures
// nothing (it did -- 63 million comparisons in a millisecond).
template <typename T>
static T launder(T v) {
    asm volatile("" : "+m"(v));
    return v;
}

// Bit equality, so -0 != +0 and two NaNs compare by payload.
template <typename T>
static bool sameBits(T a, T b) {
    a = launder(a);
    b = launder(b);
    return std::memcmp(&a, &b, sizeof(T)) == 0;
}
static bool sameBox(CBox a, CBox b) {
    return sameBits(a.x, b.x) && sameBits(a.y, b.y) && sameBits(a.w, b.w) && sameBits(a.h, b.h);
}
static bool sameVec(Vector2D a, Vector2D b) {
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
    [[gnu::noinline]] CBox dockLerpBox(const CBox& a, const CBox& b, float t) {
        return CBox{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.w + (b.w - a.w) * t, a.h + (b.h - a.h) * t};
    }
    // motion/FlightController.cpp:334
    [[gnu::noinline]] CBox flightLerpBox(const CBox& a, const CBox& b, float t) {
        return CBox{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.w + (b.w - a.w) * t, a.h + (b.h - a.h) * t};
    }
    // overview/OverviewGeometry.cpp:259 (overview/GridLayout.cpp:570 forwarded to it)
    [[gnu::noinline]] CBox geometryLerpBox(const CBox& from, const CBox& to, float t) {
        return CBox{
            from.x + (to.x - from.x) * t,
            from.y + (to.y - from.y) * t,
            from.w + (to.w - from.w) * t,
            from.h + (to.h - from.h) * t,
        };
    }
    // overview/OverviewGeometry.cpp:268 (overview/OverviewInternal.hpp:2600 forwarded to it)
    [[gnu::noinline]] float hyprlerp(float from, float to, float perc) {
        return (to - from) * perc + from;
    }
    // overview/OverviewGeometry.cpp:272 (overview/OverviewInternal.hpp:2604 forwarded to it)
    [[gnu::noinline]] Vector2D hyprlerp(const Vector2D& from, const Vector2D& to, float perc) {
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

// ============================================================================
// A2 -- the clock reads. Each deleted helper read the clock itself; here the
// read is a parameter (`now`), so the arithmetic can be compared on a grid.
// The sites that read Time::steadyNow() (== std::chrono::steady_clock::now(),
// Hyprland's helpers/time/Time.cpp:35) inline now call Config::realElapsedMs,
// whose body (core/Config.cpp:798) is unchanged and kept below as well.
// ============================================================================
namespace old_a2 {
    using clock = std::chrono::steady_clock;
    template <typename T>
    [[gnu::noinline]] float sc(T v) {
        return static_cast<float>(v);
    }
    // core/Config.cpp:798 realElapsedMs (unchanged; the sites below now call it)
    [[gnu::noinline]] float configReal(clock::time_point now, const clock::time_point& since) {
        return sc<float>(std::chrono::duration<float, std::milli>(now - since).count());
    }
    // dock/DockPreview.cpp:68 realElapsedMs (a file-local copy)
    [[gnu::noinline]] float dockPreviewReal(clock::time_point now, const clock::time_point& since) {
        return sc<float>(std::chrono::duration<float, std::milli>(now - since).count());
    }
    // stacks/StackFan.cpp:158 elapsed
    [[gnu::noinline]] float stackFanElapsed(clock::time_point now, const clock::time_point& since) {
        return std::chrono::duration<float, std::milli>(now - since).count();
    }
    // dock/DockTilt.cpp:77 msSince
    [[gnu::noinline]] float dockTiltMsSince(clock::time_point now, const clock::time_point& t) {
        return std::chrono::duration<float, std::milli>(now - t).count();
    }
    // dock/DockSpringLoad.cpp:8 springSinceMs (its body; the function stays, on realElapsedMs)
    [[gnu::noinline]] float springSinceMs(clock::time_point now, const std::optional<clock::time_point>& t) {
        return t ? std::chrono::duration<float, std::milli>(now - *t).count() : 0.F;
    }
    // search/SearchLetters.hpp:116 msSince
    [[gnu::noinline]] float searchLettersMsSince(clock::time_point now, clock::time_point then) {
        return std::chrono::duration<float, std::milli>(now - then).count();
    }
    // search/KeyboardCat.cpp:55 msSince
    [[gnu::noinline]] float keyboardCatMsSince(clock::time_point now, clock::time_point t) {
        return std::chrono::duration<float, std::milli>(now - t).count();
    }
    // The inline reads the sites spelled out (BorderFade, FlightBlur, TileChrome,
    // OverviewStrip, OverviewCards, MosaicGestures, MosaicParts, FullscreenDesktop,
    // ShowDesktop, DesktopStrip, SpringLoad, ScratchpadExit, StackFan, SearchSession,
    // KeyboardCat, Ripple), one per representation.
    [[gnu::noinline]] float inlineMsF(clock::time_point now, clock::time_point then) {
        return std::chrono::duration<float, std::milli>(now - then).count();
    }
    [[gnu::noinline]] double inlineMsD(clock::time_point now, clock::time_point then) {
        return std::chrono::duration<double, std::milli>(now - then).count();
    }
    [[gnu::noinline]] float inlineSF(clock::time_point now, clock::time_point then) {
        return std::chrono::duration<float>(now - then).count();
    }
    [[gnu::noinline]] double inlineSD(clock::time_point now, clock::time_point then) {
        return std::chrono::duration<double>(now - then).count();
    }
}

static void testA2() {
    using clock = std::chrono::steady_clock;
    using ns    = std::chrono::nanoseconds;
    // Where `then` sits on the clock: boot, an hour, days and a year of uptime
    // (a float's ms resolution coarsens with the size of the DIFFERENCE, not of
    // the base, but the base is kept honest anyway).
    const long long BASES[] = {0LL, 1LL, 999'999LL, 3'600'000'000'000LL, 259'200'123'456'789LL, 31'536'000'987'654'321LL};
    // The differences: every ns up to 4096, then a geometric sweep to ~2 days,
    // each with its neighbours, and all of it negated (a stamp in the future).
    std::vector<long long> d;
    for (long long i = 0; i <= 4096; ++i)
        d.push_back(i);
    for (double x = 4097; x < 2e14; x *= 1.0123)
        for (long long k = -2; k <= 2; ++k)
            d.push_back(static_cast<long long>(x) + k);
    const size_t N = d.size();
    for (size_t i = 0; i < N; ++i)
        d.push_back(-d[i]);

    long n = 0, bad[12] = {};
    for (long long base : BASES) {
        const clock::time_point THEN{ns{base}};
        for (long long dd : d) {
            const clock::time_point NOW{ns{base + dd}};
            ++n;
            const float REAL = old_a2::configReal(NOW, THEN);
            bad[0] += !sameBits(REAL, old_a2::dockPreviewReal(NOW, THEN));
            bad[1] += !sameBits(REAL, old_a2::stackFanElapsed(NOW, THEN));
            bad[2] += !sameBits(REAL, old_a2::dockTiltMsSince(NOW, THEN));
            bad[3] += !sameBits(REAL, old_a2::inlineMsF(NOW, THEN));
            // springSinceMs: the new body is `t ? realElapsedMs(*t) : 0.F`
            const std::optional<clock::time_point> OPT = (dd & 1) ? std::optional{THEN} : std::nullopt;
            bad[4] += !sameBits(OPT ? old_a2::configReal(NOW, *OPT) : 0.F, old_a2::springSinceMs(NOW, OPT));
            bad[5] += !sameBits(Hyprdrive::Motion::msBetween(NOW, THEN), old_a2::searchLettersMsSince(NOW, THEN));
            bad[6] += !sameBits(Hyprdrive::Motion::msBetween(NOW, THEN), old_a2::keyboardCatMsSince(NOW, THEN));
            bad[7] += !sameBits(Hyprdrive::Motion::msBetween(NOW, THEN), old_a2::inlineMsF(NOW, THEN));
            bad[8] += !sameBits(Hyprdrive::Motion::msBetween<double>(NOW, THEN), old_a2::inlineMsD(NOW, THEN));
            bad[9] += !sameBits(Hyprdrive::Motion::secondsBetween(NOW, THEN), old_a2::inlineSF(NOW, THEN));
            bad[10] += !sameBits(Hyprdrive::Motion::secondsBetween<double>(NOW, THEN), old_a2::inlineSD(NOW, THEN));
            // Config::realElapsedMs is what the steadyNow() sites call now.
            bad[11] += !sameBits(REAL, old_a2::inlineMsF(NOW, THEN));
        }
    }
    report("A2 Config::realElapsedMs == DockPreview.cpp realElapsedMs", n, bad[0]);
    report("A2 Config::realElapsedMs == StackFan.cpp elapsed", n, bad[1]);
    report("A2 Config::realElapsedMs == DockTilt.cpp msSince", n, bad[2]);
    report("A2 Config::realElapsedMs == inline steadyNow() duration<float, milli>", n, bad[3]);
    report("A2 springSinceMs on realElapsedMs == its old body", n, bad[4]);
    report("A2 msBetween == SearchLetters.hpp msSince", n, bad[5]);
    report("A2 msBetween == KeyboardCat.cpp msSince", n, bad[6]);
    report("A2 msBetween == inline duration<float, milli>", n, bad[7]);
    report("A2 msBetween<double> == inline duration<double, milli>", n, bad[8]);
    report("A2 secondsBetween == inline duration<float>", n, bad[9]);
    report("A2 secondsBetween<double> == inline duration<double>", n, bad[10]);
    report("A2 Config::realElapsedMs body == the inline form it replaced", n, bad[11]);
}

int main() {
    testA1();
    testA2();

    std::printf("motion_math_test: %d checks, %ld bitwise comparisons, %ld differ\n", g_checks, g_compared, g_failed);
    return g_checkFailed ? 1 : 0;
}
