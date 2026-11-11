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
#include "MotionCurves.hpp"
#include "SearchLetters.hpp"
#include "ShowDesktopMath.hpp"
#include "DesktopStripModel.hpp"
#include "MotionMath.hpp"
#include "MotionPresets.hpp"
#include "MotionSites.hpp"
#include "MotionTween.hpp"
#include "MotionRetarget.hpp"
#include "MotionTimings.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <limits>
#include <optional>
#include <string_view>
#include <random>

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

// ============================================================================
// A3 -- the named curves. Each golden copy is the deleted definition (or the
// inline expression a site spelled out), verbatim, as a function of t; the
// "new" side is what the site now calls, clamp included, as the site writes it.
// ============================================================================
namespace old_a3 {
    // dock/Dock.cpp:45 easeOutQuad
    [[gnu::noinline]] float dockEaseOutQuad(float t) {
        return 1.F - (1.F - t) * (1.F - t);
    }
    // dock/DockLayout.cpp:23 revealEase, the house branch
    [[gnu::noinline]] float dockLayoutReveal(float t) {
        const float INV = 1.F - std::clamp(t, 0.F, 1.F);
        return 1.F - (INV * INV);
    }
    // overview/TileChrome.cpp:241 easedHover, the house branch (T arrives clamped)
    [[gnu::noinline]] float tileChromeHover(float T) {
        const float INV = 1.F - T;
        return 1.F - INV * INV;
    }
    // dock/DockLayout.cpp:89 arrangeEase, the house branch
    [[gnu::noinline]] float dockLayoutArrange(float t) {
        t             = std::clamp(t, 0.F, 1.F);
        const float U = 1.F - t;
        return 1.F - U * U * U;
    }
    // stacks/StackFan.cpp:127-133 (StackRender.cpp:485 called the first two)
    [[gnu::noinline]] float stackOutQuad(float t) { return 1.F - (1.F - t) * (1.F - t); }
    [[gnu::noinline]] float stackOutCubic(float t) { return 1.F - std::pow(1.F - t, 3.F); }
    [[gnu::noinline]] float stackOutQuint(float t) { return 1.F - std::pow(1.F - t, 5.F); }
    [[gnu::noinline]] float stackOutBack(float t, float s) { // Qt's Easing.OutBack
        t -= 1.F;
        return t * t * ((s + 1.F) * t + s) + 1.F;
    }
    // search/SearchLetters.hpp:46 / :56 (clamp01 is unchanged, SearchLetters.hpp:41)
    [[gnu::noinline]] float lettersOutBack(float t, float s) {
        t = SearchLetters::clamp01(t) - 1.F;
        return t * t * ((s + 1.F) * t + s) + 1.F;
    }
    [[gnu::noinline]] float lettersOutCubic(float t) {
        const float INV = 1.F - SearchLetters::clamp01(t);
        return 1.F - INV * INV * INV;
    }
    // dock/DockMenu.cpp:592-593 (P arrives clamped), :647-648
    [[gnu::noinline]] float menuRowE(float P) {
        const float E = 1.F - (1.F - P) * (1.F - P) * (1.F - P); // cubic out
        return E;
    }
    [[gnu::noinline]] float menuRowA(float P) {
        return P * P * (3.F - 2.F * P);
    }
    [[gnu::noinline]] float menuHover(float x) {
        const float T = std::clamp(x, 0.F, 1.F);
        return T * T * (3.F - 2.F * T);
    }
    // dock/DockPointer.cpp:533 easeOutQuint
    [[gnu::noinline]] float pointerOutQuint(float t) {
        const float INV = 1.F - std::clamp(t, 0.F, 1.F);
        return 1.F - INV * INV * INV * INV * INV;
    }
    // dock/SpringLoad.cpp:235 (P is a float, the curve double), :259 smoothstep
    [[gnu::noinline]] double springCommit(float P) {
        const double E = 1.0 - std::pow(1.0 - P, 3.0);
        return E;
    }
    [[gnu::noinline]] double springSmoothstep(double t) {
        t = std::clamp(t, 0.0, 1.0);
        return t * t * (3.0 - 2.0 * t);
    }
    // layout/FullscreenDesktop.cpp:551 (TIMEL is a double)
    [[gnu::noinline]] double fullscreenTravel(double TIMEL) {
        return 1.0 - std::pow(1.0 - TIMEL, 3.0);
    }
    // motion/FlightBlur.cpp:497 smoothstep01, :585 (T arrives clamped)
    [[gnu::noinline]] float blurSmoothstep01(float t) {
        const float T = std::clamp(t, 0.F, 1.F);
        return T * T * (3.F - 2.F * T);
    }
    [[gnu::noinline]] float blurFadeKeep(float T) {
        const float KEEP = 1.F - T * T * (3.F - 2.F * T);
        return KEEP;
    }
    // overview/OverviewSearchView.cpp:536-537 (x = the raw ms ratio)
    [[gnu::noinline]] float searchLit(float x) {
        const float T = std::clamp(x, 0.F, 1.F);
        return T * T * (3.F - 2.F * T);
    }
    // stacks/StackIcon.cpp:188 (TRI is a double), stacks/StackRender.cpp:446
    [[gnu::noinline]] double stackIconSweep(double TRI) {
        const double E = TRI * TRI * (3.0 - 2.0 * TRI);
        return E;
    }
    [[gnu::noinline]] float stackBarAlpha(float barAlpha) {
        const float E = barAlpha * barAlpha * (3.F - 2.F * barAlpha);
        return E;
    }
}

namespace M = Hyprdrive::Motion;

static void testA3() {
    // t: every 397th float bit pattern in [0, 1] (about 2.7 million, dense near
    // 0 where float is), every 4093rd from -1.5 up to 0 and from 1 up to 2.5
    // (the sites that do not clamp see overshoot), and the edges.
    std::vector<float> T;
    auto               bitsOf = [](float f) {
        uint32_t u;
        std::memcpy(&u, &f, 4);
        return u;
    };
    auto floatOf = [](uint32_t u) {
        float f;
        std::memcpy(&f, &u, 4);
        return f;
    };
    for (uint32_t u = 0; u <= bitsOf(1.F); u += 397)
        T.push_back(floatOf(u));
    for (uint32_t u = bitsOf(1.F); u <= bitsOf(2.5F); u += 4093)
        T.push_back(floatOf(u));
    for (uint32_t u = 0x80000000U; u <= bitsOf(-1.5F); u += 4093)
        T.push_back(floatOf(u));
    for (float e : {0.F, -0.F, 1.F, 0.5F, 1e-38F, -1e-38F, 0.99999994F, 1.0000001F, -1.F, 2.F})
        T.push_back(e);
    const float S[] = {1.1F, 1.4F, 1.70158F, 1.6F, 3.2F, 3.2F * 0.8F, 0.F, 1.75F};

    long n = 0, bad[24] = {};
    for (float t : T) {
        ++n;
        const float CT = std::clamp(t, 0.F, 1.F);
        bad[0] += !sameBits(M::outQuad(t), old_a3::dockEaseOutQuad(t));
        bad[1] += !sameBits(M::outQuad(std::clamp(t, 0.F, 1.F)), old_a3::dockLayoutReveal(t));
        bad[2] += !sameBits(M::outQuad(t), old_a3::tileChromeHover(t));
        bad[3] += !sameBits(M::outCubic(std::clamp(t, 0.F, 1.F)), old_a3::dockLayoutArrange(t));
        bad[4] += !sameBits(M::outQuad(t), old_a3::stackOutQuad(t));
        bad[5] += !sameBits(M::outCubicPow(t), old_a3::stackOutCubic(t));
        bad[6] += !sameBits(M::outQuintPow(t), old_a3::stackOutQuint(t));
        for (float s : S) {
            bad[7] += !sameBits(M::outBack(t, s), old_a3::stackOutBack(t, s));
            bad[8] += !sameBits(SearchLetters::easeOutBack(t, s), old_a3::lettersOutBack(t, s));
        }
        bad[9] += !sameBits(SearchLetters::easeOutCubic(t), old_a3::lettersOutCubic(t));
        bad[10] += !sameBits(M::outCubic(CT), old_a3::menuRowE(CT));
        bad[11] += !sameBits(M::smoothstep(CT), old_a3::menuRowA(CT));
        bad[12] += !sameBits(M::smoothstep(std::clamp(t, 0.F, 1.F)), old_a3::menuHover(t));
        bad[13] += !sameBits(M::outQuint(std::clamp(t, 0.F, 1.F)), old_a3::pointerOutQuint(t));
        bad[14] += !sameBits(M::outCubicPow(static_cast<double>(CT)), old_a3::springCommit(CT));
        bad[15] += !sameBits(M::smoothstep(std::clamp(static_cast<double>(t), 0.0, 1.0)), old_a3::springSmoothstep(t));
        bad[16] += !sameBits(M::outCubicPow(static_cast<double>(CT)), old_a3::fullscreenTravel(CT));
        bad[17] += !sameBits(M::smoothstep(std::clamp(t, 0.F, 1.F)), old_a3::blurSmoothstep01(t));
        bad[18] += !sameBits(1.F - M::smoothstep(CT), old_a3::blurFadeKeep(CT));
        bad[19] += !sameBits(M::smoothstep(std::clamp(t, 0.F, 1.F)), old_a3::searchLit(t));
        bad[20] += !sameBits(M::smoothstep(static_cast<double>(CT)), old_a3::stackIconSweep(CT));
        bad[21] += !sameBits(M::smoothstep(CT), old_a3::stackBarAlpha(CT));
    }
    // The double sites also see doubles that are not floats.
    long nd = 0;
    for (int i = -1000; i <= 1001000; ++i) {
        const double D  = i / 1000000.0 + 1e-13 * (i % 7);
        const double CD = std::clamp(D, 0.0, 1.0);
        ++nd;
        bad[22] += !sameBits(M::outCubicPow(CD), old_a3::fullscreenTravel(CD));
        bad[23] += !sameBits(M::smoothstep(CD), old_a3::stackIconSweep(CD)) + !sameBits(M::smoothstep(std::clamp(D, 0.0, 1.0)), old_a3::springSmoothstep(D));
    }
    const long NS = n * static_cast<long>(std::size(S));
    report("A3 outQuad == Dock.cpp easeOutQuad", n, bad[0]);
    report("A3 outQuad(clamp) == DockLayout.cpp revealEase (house)", n, bad[1]);
    report("A3 outQuad == TileChrome.cpp easedHover (house)", n, bad[2]);
    report("A3 outCubic(clamp) == DockLayout.cpp arrangeEase (house)", n, bad[3]);
    report("A3 outQuad == StackFan.cpp outQuad", n, bad[4]);
    report("A3 outCubicPow == StackFan.cpp outCubic", n, bad[5]);
    report("A3 outQuintPow == StackFan.cpp outQuint", n, bad[6]);
    report("A3 outBack == StackFan.cpp outBack", NS, bad[7]);
    report("A3 SearchLetters::easeOutBack on outBack == its old body", NS, bad[8]);
    report("A3 SearchLetters::easeOutCubic on outCubic == its old body", n, bad[9]);
    report("A3 outCubic == DockMenu.cpp row cubic", n, bad[10]);
    report("A3 smoothstep == DockMenu.cpp row alpha", n, bad[11]);
    report("A3 smoothstep(clamp) == DockMenu.cpp row hover", n, bad[12]);
    report("A3 outQuint(clamp) == DockPointer.cpp easeOutQuint", n, bad[13]);
    report("A3 outCubicPow(double) == SpringLoad.cpp commit", n, bad[14]);
    report("A3 smoothstep(double, clamp) == SpringLoad.cpp smoothstep", n, bad[15]);
    report("A3 outCubicPow(double) == FullscreenDesktop.cpp travel", n, bad[16]);
    report("A3 smoothstep(clamp) == FlightBlur.cpp smoothstep01", n, bad[17]);
    report("A3 1 - smoothstep == FlightBlur.cpp fadeMotion keep", n, bad[18]);
    report("A3 smoothstep(clamp) == OverviewSearchView.cpp searchLitT", n, bad[19]);
    report("A3 smoothstep(double) == StackIcon.cpp sweep", n, bad[20]);
    report("A3 smoothstep == StackRender.cpp scrollbar alpha", n, bad[21]);
    report("A3 outCubicPow(double) == FullscreenDesktop.cpp travel, double grid", nd, bad[22]);
    report("A3 smoothstep(double) == StackIcon.cpp / SpringLoad.cpp, double grid", 2 * nd, bad[23]);
}

// ============================================================================
// A4 -- the critically damped spring. Both old structs verbatim; each is
// driven through the same long sequence of steps (random dt including 0,
// negative and long frames, goals that jump mid-flight, both directions,
// several omegas) next to the new one, and the whole state is compared after
// EVERY step -- a trace, so a difference cannot hide by settling out.
// ============================================================================
namespace old_a4 {
    // layout/ShowDesktopMath.hpp:43 SSpring
    struct SShowDesktop {
        double x = 0; // progress
        double v = 0; // progress per second
        [[gnu::noinline]] void step(double dt, double target, double omega) {
            if (dt <= 0)
                return;
            const double Y0 = x - target;
            const double C  = v + omega * Y0;
            const double E  = std::exp(-omega * dt);
            x               = target + (Y0 + C * dt) * E;
            v               = (v - omega * C * dt) * E;
            x               = std::clamp(x, 0.0, 1.0);
        }
        bool settled(double target) const {
            return std::abs(x - target) < 1e-3 && std::abs(v) < 1e-2;
        }
    };
    // desktops/DesktopStripModel.hpp:60 SSpring
    struct SStrip {
        double pos = 0, vel = 0;
        [[gnu::noinline]] void step(double goal, double omega, double dt) {
            if (dt <= 0)
                return;
            const double X = pos - goal;
            const double B = vel + omega * X;
            const double E = std::exp(-omega * dt);
            pos            = goal + (X + B * dt) * E;
            vel            = (vel - omega * B * dt) * E;
        }
        bool settled(double goal, double eps) const {
            return std::abs(pos - goal) < eps && std::abs(vel) < eps * 20.0;
        }
    };
}

static void testA4() {
    std::mt19937_64                        rng(20261110);
    std::uniform_real_distribution<double> DT(0.0, 0.05), LONGDT(0.05, 2.0), GOAL(-3.0, 12.0), U(0.0, 1.0);
    const double                           OMEGAS[] = {Hyprdrive::ShowDesktopMath::OUT_OMEGA, Hyprdrive::ShowDesktopMath::IN_OMEGA, 13.0, 4.7 / 0.06, 4.7 / 0.06 * 2.5, 1.0, 120.0};
    long                                   n = 0, badShow = 0, badStrip = 0;
    for (int run = 0; run < 400; ++run) {
        old_a4::SShowDesktop                 oldShow;
        Hyprdrive::ShowDesktopMath::SSpring newShow;
        old_a4::SStrip                       oldStrip;
        Hyprdrive::StripModel::SSpring       newStrip;
        // Start somewhere moving, as a turned-round slide does.
        oldShow.x = newShow.x = U(rng);
        oldShow.v = newShow.v = (U(rng) - 0.5) * 40.0;
        oldStrip.pos = newStrip.pos = GOAL(rng);
        oldStrip.vel = newStrip.vel = (U(rng) - 0.5) * 80.0;
        double target = U(rng) < 0.5 ? 0.0 : 1.0, goal = GOAL(rng);
        double omega  = OMEGAS[run % std::size(OMEGAS)];
        for (int i = 0; i < 2000; ++i) {
            const double R  = U(rng);
            double       dt = R < 0.02 ? 0.0 : R < 0.03 ? -DT(rng) : R < 0.06 ? LONGDT(rng) : DT(rng);
            if (U(rng) < 0.01)
                target = 1.0 - target; // a second press: turn round mid-flight
            if (U(rng) < 0.01)
                goal = GOAL(rng);
            if (U(rng) < 0.005)
                omega = OMEGAS[static_cast<size_t>(U(rng) * std::size(OMEGAS)) % std::size(OMEGAS)];
            oldShow.step(dt, target, omega);
            newShow.step(dt, target, omega);
            oldStrip.step(goal, omega, dt);
            newStrip.step(goal, omega, dt);
            ++n;
            badShow += !(sameBits(oldShow.x, newShow.x) && sameBits(oldShow.v, newShow.v) && oldShow.settled(target) == newShow.settled(target));
            badStrip += !(sameBits(oldStrip.pos, newStrip.pos) && sameBits(oldStrip.vel, newStrip.vel) && oldStrip.settled(goal, 1e-3) == newStrip.settled(goal, 1e-3));
        }
    }
    report("A4 ShowDesktopMath SSpring trace == its old step", n, badShow);
    report("A4 StripModel SSpring trace == its old step", n, badStrip);
}

// ============================================================================
// A5 -- the preset bakes. The old Motion.cpp table verbatim (motion/Motion.cpp
// :42-60 before the move), and the three ways it used to be baked: the live
// slots re-baked IN PLACE on every preset change (ease), easeNamed's
// single-slot cache (also in place, whenever the name changed), and
// houseFixed's own pair. Each is compared to the one shared bake.
// ============================================================================
namespace old_a5 {
    using Hyprutils::Math::Vector2D;
    struct SCurve {
        float x1, y1, x2, y2;
    };
    struct SPreset {
        std::string_view name;
        SCurve           decel, accel, pop;
    };
        constexpr SCurve SERP_EXIT{0.33F, 0.F, 0.67F, 0.F};      // InCubic
        constexpr SCurve SERP_POP{0.33F, 1.39F, 0.66F, 1.F};     // OutBack, overshoot 1.2
        constexpr std::array<SPreset, 9> PRESETS{{
            {"hyprdrive", {0.16F, 1.F, 0.3F, 1.F}, {0.7F, 0.F, 0.84F, 0.F}, {0.34F, 1.56F, 0.64F, 1.F}},
            // myBezier, compositors/hyprland/config/settings.lua -- used in
            // and out alike, so the exit is the same curve.
            {"serpantinum", {0.05F, 0.9F, 0.1F, 1.05F}, {0.05F, 0.9F, 0.1F, 1.05F}, SERP_POP},
            {"serpantinum_glide", {0.22F, 1.09F, 0.46F, 1.F}, SERP_EXIT, SERP_POP},  // OutQuint
            {"serpantinum_soft", {0.33F, 1.F, 0.66F, 1.F}, SERP_EXIT, SERP_POP},     // OutCubic
            {"serpantinum_intro", {0.15F, 1.04F, 0.35F, 0.99F}, SERP_EXIT, SERP_POP}, // OutExpo
            {"serpantinum_pop", SERP_POP, SERP_EXIT, SERP_POP},                        // OutBack 1.2
            // Quick off the mark: OutCirc / InCirc.
            {"swift", {0.F, 0.55F, 0.45F, 1.F}, {0.55F, 0.F, 1.F, 0.45F}, {0.F, 0.55F, 0.45F, 1.F}},
            // Everything lands with a little overshoot.
            {"springy", {0.34F, 1.30F, 0.64F, 1.F}, {0.36F, 0.F, 0.66F, -0.3F}, {0.34F, 1.70F, 0.64F, 1.F}},
            {"linear", {0.F, 0.F, 1.F, 1.F}, {0.F, 0.F, 1.F, 1.F}, {0.F, 0.F, 1.F, 1.F}},
        }};

    // motion/Motion.cpp:79 bake
    void bake(Hyprutils::Animation::CBezierCurve& curve, const SCurve& c) {
        curve.setup(std::array<Vector2D, 2>{Vector2D{c.x1, c.y1}, Vector2D{c.x2, c.y2}});
    }
}

static void testA5() {
    namespace P = Hyprdrive::Motion::Presets;
    // The table itself moved: names and every control point, bit for bit.
    long nt = 0, badT = 0;
    for (size_t i = 0; i < old_a5::PRESETS.size(); ++i) {
        const auto& O = old_a5::PRESETS[i];
        const auto& N = P::PRESETS[i];
        const old_a5::SCurve* oc[] = {&O.decel, &O.accel, &O.pop};
        const P::SCurve*      nc[] = {&N.decel, &N.accel, &N.pop};
        for (int r = 0; r < 3; ++r) {
            ++nt;
            badT += !(sameBits(oc[r]->x1, nc[r]->x1) && sameBits(oc[r]->y1, nc[r]->y1) && sameBits(oc[r]->x2, nc[r]->x2) && sameBits(oc[r]->y2, nc[r]->y2));
        }
        ++nt;
        badT += O.name != N.name || &P::presetNamed(O.name) != &N || P::indexOf(N) != i;
    }
    ++nt;
    badT += old_a5::PRESETS.size() != P::PRESETS.size() || &P::presetNamed("no-such-preset") != &P::PRESETS[0];
    report("A5 PRESETS moved unchanged (names, points, lookup)", nt, badT);

    // t: every 4093rd float in [0, 1], a sweep past both ends (the callers
    // clamp; ease / easeNamed / houseFixed clamp with std::clamp), and the edges.
    std::vector<float> T;
    for (uint32_t u = 0; u <= 0x3f800000U; u += 4093) {
        float f;
        std::memcpy(&f, &u, 4);
        T.push_back(f);
    }
    for (float e : {-1.F, -0.F, 1.F, 1.5F, 0.5F, 0.99999994F})
        T.push_back(e);
    const auto& TABLE = P::bakedPresets();

    // ease(): the live slots, re-baked in place, through a preset sequence
    // that visits every preset and comes back to some (as the panel does).
    std::array<Hyprutils::Animation::CBezierCurve, 3> live;
    std::vector<size_t>                               seq;
    for (size_t i = 0; i < old_a5::PRESETS.size(); ++i)
        seq.push_back(i);
    for (size_t i : {3, 0, 7, 7, 1, 8, 0, 5, 2, 6, 4, 0})
        seq.push_back(i);
    long ne = 0, badE = 0;
    for (size_t i : seq) {
        old_a5::bake(live[0], old_a5::PRESETS[i].decel);
        old_a5::bake(live[1], old_a5::PRESETS[i].accel);
        old_a5::bake(live[2], old_a5::PRESETS[i].pop);
        for (int r = 0; r < 3; ++r)
            for (float t : T) {
                const float CT = std::clamp(t, 0.F, 1.F);
                ++ne;
                badE += !sameBits(live[r].getYForPoint(CT), TABLE[i][r].getYForPoint(CT));
            }
    }
    report("A5 shared bake == ease()'s live slots, re-baked in place (21 preset changes, 3 roles)", ne, badE);

    // easeNamed(): one fresh bake per preset (its cache's first fill), all roles.
    long nn = 0, badN = 0;
    for (size_t i = 0; i < old_a5::PRESETS.size(); ++i) {
        std::array<Hyprutils::Animation::CBezierCurve, 3> fresh;
        old_a5::bake(fresh[0], old_a5::PRESETS[i].decel);
        old_a5::bake(fresh[1], old_a5::PRESETS[i].accel);
        old_a5::bake(fresh[2], old_a5::PRESETS[i].pop);
        for (int r = 0; r < 3; ++r)
            for (float t : T) {
                ++nn;
                badN += !sameBits(fresh[r].getYForPoint(std::clamp(t, 0.F, 1.F)), TABLE[i][r].getYForPoint(std::clamp(t, 0.F, 1.F)));
            }
    }
    report("A5 shared bake == easeNamed()'s bake, all 9 presets x 3 roles", nn, badN);

    // houseFixed(): its own house pair.
    Hyprutils::Animation::CBezierCurve hd, ha;
    old_a5::bake(hd, old_a5::PRESETS[0].decel);
    old_a5::bake(ha, old_a5::PRESETS[0].accel);
    long nh = 0, badH = 0;
    for (float t : T) {
        const float CT = std::clamp(t, 0.F, 1.F);
        nh += 2;
        badH += !sameBits(hd.getYForPoint(CT), TABLE[0][0].getYForPoint(CT)) + !sameBits(ha.getYForPoint(CT), TABLE[0][1].getYForPoint(CT));
    }
    report("A5 shared bake == houseFixed()'s pair", nh, badH);
}

// ============================================================================
// A6 -- the per-site branch. The six old "house ? own curve : preset curve"
// sites verbatim, with isHouse() / presetEase() standing in for Motion::housePreset() / Motion::ease() as
// the preset under test (ease's body: clamp, then the active row of the bake --
// A5 holds that row to the old bakes). Each is compared to resolveCurve(), the
// body of Motion::curve(), under all nine presets.
// ============================================================================
namespace old_a6 {
    using Hyprdrive::Motion::ERole;
    size_t g_preset = 0;
    bool   isHouse() {
        return g_preset == 0;
    }
    [[gnu::noinline]] float presetEase(ERole role, float t) {
        const float T = std::clamp(t, 0.F, 1.F);
        return Hyprdrive::Motion::Presets::bakedPresets()[g_preset][static_cast<size_t>(role)].getYForPoint(T);
    }
    // motion/FlightController.cpp:320 easeOutExpo
    [[gnu::noinline]] float easeOutExpo(float t) {
        if (!isHouse())
            return presetEase(ERole::DECEL, t);
        return t >= 1.F ? 1.F : 1.F - std::pow(2.F, -10.F * t);
    }
    // dock/DockLayout.cpp:20 revealEase (its A3 form; A3 holds that to the original)
    [[gnu::noinline]] float revealEase(float t) {
        if (!isHouse())
            return presetEase(ERole::DECEL, t);
        return Hyprdrive::Motion::outQuad(std::clamp(t, 0.F, 1.F));
    }
    // dock/DockLayout.cpp:89 arrangeEase (its A3 form)
    [[gnu::noinline]] float arrangeEase(float t) {
        if (!isHouse())
            return presetEase(ERole::DECEL, t);
        return Hyprdrive::Motion::outCubic(std::clamp(t, 0.F, 1.F));
    }
    // dock/DockLayout.cpp:95 spawnEase
    [[gnu::noinline]] float spawnEase(float t) {
        if (!isHouse())
            return presetEase(ERole::POP, t);
        t                  = std::clamp(t, 0.F, 1.F);
        constexpr float C1 = 1.75F;
        constexpr float C3 = C1 + 1.F;
        const float     U  = t - 1.F;
        return 1.F + C3 * U * U * U + C1 * U * U;
    }
    // dock/Dock.cpp:1408 the launch zoom (its A3 form; LT arrives clamped)
    [[gnu::noinline]] float launchZoom(float LT) {
        const float E = isHouse() ? Hyprdrive::Motion::outQuad(LT) : presetEase(ERole::DECEL, LT);
        return E;
    }
    // overview/TileChrome.cpp:243 easedHover (its A3 form; T arrives clamped)
    [[gnu::noinline]] float tileHover(float T) {
        const float E = isHouse() ? Hyprdrive::Motion::outQuad(T) : presetEase(ERole::DECEL, T);
        return E;
    }
}

static void testA6() {
    using Hyprdrive::Motion::ESite;
    std::vector<float> T;
    for (uint32_t u = 0; u <= 0x3f800000U; u += 2039) {
        float f;
        std::memcpy(&f, &u, 4);
        T.push_back(f);
    }
    for (int i = -2000; i <= 0; ++i)
        T.push_back(i / 4000.F);
    for (int i = 0; i <= 2000; ++i)
        T.push_back(1.F + i / 4000.F);
    for (float e : {-0.F, 0.99999994F, 1.0000001F})
        T.push_back(e);

    struct SCase {
        const char* name;
        ESite       site;
        bool        clampedByCaller;
        float (*old)(float);
    };
    const SCase CASES[] = {
        {"A6 curve(FLIGHT) == FlightController.cpp easeOutExpo", ESite::FLIGHT, false, old_a6::easeOutExpo},
        {"A6 curve(DOCK_REVEAL) == DockLayout.cpp revealEase", ESite::DOCK_REVEAL, false, old_a6::revealEase},
        {"A6 curve(DOCK_ARRANGE) == DockLayout.cpp arrangeEase", ESite::DOCK_ARRANGE, false, old_a6::arrangeEase},
        {"A6 curve(DOCK_SPAWN) == DockLayout.cpp spawnEase", ESite::DOCK_SPAWN, false, old_a6::spawnEase},
        {"A6 curve(DOCK_LAUNCH_ZOOM) == Dock.cpp launch zoom", ESite::DOCK_LAUNCH_ZOOM, true, old_a6::launchZoom},
        {"A6 curve(TILE_HOVER) == TileChrome.cpp easedHover", ESite::TILE_HOVER, true, old_a6::tileHover},
    };
    for (const auto& C : CASES) {
        long n = 0, bad = 0;
        for (size_t p = 0; p < Hyprdrive::Motion::Presets::PRESETS.size(); ++p) {
            old_a6::g_preset = p;
            for (float t0 : T) {
                // The caller-clamped sites only ever see 0..1 (and the caller's clamp).
                const float t = C.clampedByCaller ? std::clamp(t0, 0.F, 1.F) : t0;
                ++n;
                const float NEW = Hyprdrive::Motion::resolveCurve(C.site, t, old_a6::isHouse(), old_a6::presetEase);
                bad += !sameBits(NEW, C.old(t));
            }
        }
        report(C.name, n, bad);
    }
}

// ============================================================================
// A7 -- the turning value (STween). For each adopting site: the old fields and
// functions verbatim, and the site's new code on STween, transcribed. Both are
// driven by one recorded sequence -- a fake clock advancing by frame-sized and
// long steps, the target flipping at random moments (mid-way, at the end, twice
// in one frame), the value sampled every step -- and every sample and every
// stored field is compared bit for bit. The clock is a parameter: each site's
// kind (anim-scaled for DockPreview and WindowPill, a held `now` for
// BorderFade) is modelled as the site reads it.
// ============================================================================
namespace a7 {
    using clock = std::chrono::steady_clock;
    using TP    = clock::time_point;
    TP    g_now{std::chrono::nanoseconds{259'200'123'456'789LL}};
    float g_scale = 1.F;
    // Config::animElapsedMs, with the scale as a parameter (its body).
    float animElapsedMs(const TP& since) {
        const float SCALE = g_scale;
        if (SCALE <= 0.F)
            return 1e9F;
        return Hyprdrive::Motion::msBetween(g_now, since) / SCALE;
    }
    // houseEase under the house preset (Motion::ease: clamp, then the bake).
    float houseEase(bool decel, float t) {
        return Hyprdrive::Motion::Presets::bakedPresets()[0][decel ? 0 : 1].getYForPoint(std::clamp(t, 0.F, 1.F));
    }

    // ---- dock/DockPreview.cpp:184-196 + DockParts.hpp:1386-1389 ----
    struct SOldPanel {
        float fadeFrom  = 0.F;
        float fadeTo    = 1.F;
        TP    fadeSince = g_now;
        float fadeMs    = 170.F;
    };
    [[gnu::noinline]] float oldPanelAlpha(const SOldPanel& panel) {
        const float T = std::clamp(animElapsedMs(panel.fadeSince) / std::max(1.F, panel.fadeMs), 0.F, 1.F);
        const float E = houseEase(panel.fadeTo > panel.fadeFrom, T);
        return panel.fadeFrom + ((panel.fadeTo - panel.fadeFrom) * E);
    }
    [[gnu::noinline]] void oldPanelFade(SOldPanel& panel, float to, float ms, const TP& now) {
        panel.fadeFrom  = oldPanelAlpha(panel);
        panel.fadeTo    = to;
        panel.fadeSince = now;
        panel.fadeMs    = ms;
    }
    struct SNewPanel {
        Hyprdrive::Motion::STween<TP> fade{.from = 0.F, .to = 1.F, .start = g_now};
        float                         fadeMs = 170.F;
    };
    float newPanelAlpha(const SNewPanel& panel) {
        const float T = std::clamp(animElapsedMs(panel.fade.start) / std::max(1.F, panel.fadeMs), 0.F, 1.F);
        const float E = houseEase(panel.fade.rising(), T);
        return panel.fade.at(E);
    }
    void newPanelFade(SNewPanel& panel, float to, float ms, const TP& now) {
        panel.fade.retarget(newPanelAlpha(panel), to, now);
        panel.fadeMs = ms;
    }

    // ---- overview/WindowPill.cpp:115-146, 231-234, 266 ----
    struct SOldReveal {
        float from  = 0.F;
        float to    = 0.F;
        float ms    = 1.F;
        TP    start = {};
    };
    SOldReveal g_oldReveal;
    [[gnu::noinline]] float oldPillEase() {
        const float ELAPSED = animElapsedMs(g_oldReveal.start);
        if (ELAPSED >= g_oldReveal.ms)
            return g_oldReveal.to;
        const float T = std::clamp(ELAPSED / std::max(1.F, g_oldReveal.ms), 0.F, 1.F);
        return g_oldReveal.from + (g_oldReveal.to - g_oldReveal.from) * houseEase(g_oldReveal.to > g_oldReveal.from, T);
    }
    [[gnu::noinline]] void oldSetSuperHeld(bool held) {
        g_oldReveal.from  = oldPillEase();
        g_oldReveal.to    = held ? 1.F : 0.F;
        g_oldReveal.ms    = held ? 140.F : 105.F;
        g_oldReveal.start = g_now;
    }
    bool oldPillAnimating() {
        return animElapsedMs(g_oldReveal.start) < g_oldReveal.ms;
    }
    struct SNewReveal : Hyprdrive::Motion::STween<TP> {
        float ms = 1.F;
    };
    SNewReveal g_newReveal;
    float      newPillEase() {
        const float ELAPSED = animElapsedMs(g_newReveal.start);
        if (ELAPSED >= g_newReveal.ms)
            return g_newReveal.to;
        const float T = std::clamp(ELAPSED / std::max(1.F, g_newReveal.ms), 0.F, 1.F);
        return g_newReveal.at(houseEase(g_newReveal.rising(), T));
    }
    void newSetSuperHeld(bool held) {
        const float NOW_VALUE = newPillEase();
        g_newReveal.ms        = held ? 140.F : 105.F;
        g_newReveal.retarget(NOW_VALUE, held ? 1.F : 0.F, g_now);
    }
    bool newPillAnimating() {
        return animElapsedMs(g_newReveal.start) < g_newReveal.ms;
    }

    // ---- motion/BorderFade.cpp:20-26, 68-87, 101-121, 141 ----
    struct SCurve {
        float                                     ms     = 0.F;
        const Hyprutils::Animation::CBezierCurve* bezier = nullptr;
    };
    float bfEase(const SCurve& c, float t) {
        if (t >= 1.F)
            return 1.F;
        return c.bezier ? c.bezier->getYForPoint(t) : t;
    }
    struct SOldFade {
        bool  lit  = false;
        float from = 0.F;
        TP    start;
    };
    [[gnu::noinline]] float oldValueAt(const SOldFade& f, const SCurve& c, TP now) {
        const float TARGET = f.lit ? 1.F : 0.F;
        if (c.ms <= 0.F)
            return TARGET;
        const float T = std::clamp(Hyprdrive::Motion::msBetween(now, f.start) / c.ms, 0.F, 1.F);
        return f.from + ((TARGET - f.from) * bfEase(c, T));
    }
    [[gnu::noinline]] float oldMix(SOldFade& f, bool fresh, bool lit, const SCurve& C, TP NOW) {
        if (fresh) {
            f.lit   = lit;
            f.from  = 0.F;
            f.start = NOW;
        } else if (f.lit != lit) {
            f.from  = oldValueAt(f, C, NOW);
            f.lit   = lit;
            f.start = NOW;
        }
        return oldValueAt(f, C, NOW);
    }
    bool oldAnimating(const SOldFade& f, const SCurve& C, TP NOW) {
        return oldValueAt(f, C, NOW) != (f.lit ? 1.F : 0.F);
    }
    struct SNewFade {
        Hyprdrive::Motion::STween<TP> fade;
    };
    float newValueAt(const SNewFade& f, const SCurve& c, TP now) {
        if (c.ms <= 0.F)
            return f.fade.to;
        const float T = std::clamp(Hyprdrive::Motion::msBetween(now, f.fade.start) / c.ms, 0.F, 1.F);
        return f.fade.at(bfEase(c, T));
    }
    float newMix(SNewFade& f, bool fresh, bool lit, const SCurve& C, TP NOW) {
        if (fresh) {
            f.fade = {.from = 0.F, .to = lit ? 1.F : 0.F, .start = NOW};
        } else if ((f.fade.to == 1.F) != lit) {
            f.fade.retarget(newValueAt(f, C, NOW), lit ? 1.F : 0.F, NOW);
        }
        return newValueAt(f, C, NOW);
    }
    bool newAnimating(const SNewFade& f, const SCurve& C, TP NOW) {
        return newValueAt(f, C, NOW) != f.fade.to;
    }
}

static void testA7() {
    using namespace a7;
    std::mt19937_64                       rng(7);
    std::uniform_real_distribution<float> U(0.F, 1.F);
    auto                                  step = [&] {
        const float R  = U(rng);
        const long  NS = R < 0.03F ? 0 : R < 0.08F ? static_cast<long>(U(rng) * 2e9F) : static_cast<long>(U(rng) * 4.0e7F);
        g_now += std::chrono::nanoseconds{NS};
    };
    const float SCALES[] = {1.F, 0.5F, 2.F, 10.F, 0.05F, 0.F};

    // DockPreview: entrances at PANEL_IN_MS 170, dismissals at PANEL_OUT_MS 130.
    long nP = 0, badP = 0;
    for (float scale : SCALES) {
        g_scale = scale;
        SOldPanel o;
        SNewPanel n;
        for (int i = 0; i < 200000; ++i) {
            step();
            if (U(rng) < 0.04F) {
                const bool IN = U(rng) < 0.5F;
                oldPanelFade(o, IN ? 1.F : 0.F, IN ? 170.F : 130.F, g_now);
                newPanelFade(n, IN ? 1.F : 0.F, IN ? 170.F : 130.F, g_now);
            }
            ++nP;
            badP += !(sameBits(oldPanelAlpha(o), newPanelAlpha(n)) && sameBits(o.fadeFrom, n.fade.from) && sameBits(o.fadeTo, n.fade.to) && o.fadeSince == n.fade.start &&
                      sameBits(o.fadeMs, n.fadeMs));
        }
    }
    report("A7 DockPreview panel fade on STween == old, replayed (6 anim scales incl. 0)", nP, badP);

    // WindowPill: SUPER pressed and released at random, IN 140 / OUT 105.
    long nW = 0, badW = 0;
    for (float scale : SCALES) {
        g_scale     = scale;
        g_oldReveal = {};
        g_newReveal = {};
        bool held   = false;
        for (int i = 0; i < 200000; ++i) {
            step();
            if (U(rng) < 0.05F) {
                held = !held;
                oldSetSuperHeld(held);
                newSetSuperHeld(held);
            }
            ++nW;
            badW += !(sameBits(oldPillEase(), newPillEase()) && oldPillAnimating() == newPillAnimating() && sameBits(g_oldReveal.from, g_newReveal.from) &&
                      sameBits(g_oldReveal.to, g_newReveal.to) && sameBits(g_oldReveal.ms, g_newReveal.ms) && g_oldReveal.start == g_newReveal.start);
        }
    }
    report("A7 WindowPill reveal on STween == old, replayed (6 anim scales incl. 0)", nW, badW);

    // BorderFade: rings lit and unlit at random (several a frame), forgotten
    // and re-seen (fresh), under a bezier, a linear and a disabled curve.
    Hyprutils::Animation::CBezierCurve quint;
    quint.setup(std::array<Hyprutils::Math::Vector2D, 2>{Hyprutils::Math::Vector2D{0.23, 1.0}, Hyprutils::Math::Vector2D{0.32, 1.0}});
    const SCurve CURVES[] = {{539.F, &quint}, {539.F, nullptr}, {0.F, &quint}, {149.F, &quint}};
    long         nB = 0, badB = 0;
    for (const auto& C : CURVES) {
        SOldFade o;
        SNewFade n;
        bool     lit = false, fresh = true;
        for (int i = 0; i < 200000; ++i) {
            step();
            const int CALLS = 1 + static_cast<int>(U(rng) * 3.F); // a key mixed more than once a frame
            for (int k = 0; k < CALLS; ++k) {
                if (U(rng) < 0.05F)
                    lit = !lit;
                if (U(rng) < 0.002F)
                    fresh = true;
                const float A = oldMix(o, fresh, lit, C, g_now);
                const float B = newMix(n, fresh, lit, C, g_now);
                fresh         = false;
                ++nB;
                badB += !(sameBits(A, B) && oldAnimating(o, C, g_now) == newAnimating(n, C, g_now) && sameBits(o.from, n.fade.from) &&
                          sameBits(o.lit ? 1.F : 0.F, n.fade.to) && o.start == n.fade.start);
            }
        }
    }
    report("A7 BorderFade on STween == old, replayed (bezier, linear, disabled, short)", nB, badB);
}

// ============================================================================
// A8 -- the shared timings. The copies they replaced, verbatim: the sites now
// alias these, so equal bits here are equal bits there.
// ============================================================================
static void testA8() {
    namespace M = Hyprdrive::Motion;
    // dock/DockParts.hpp:886-887, overview/Overview.hpp:741, layout/MosaicGestures.cpp:175-176
    const float OLD_IN[]  = {150.F, 150.F, 150.F};
    const float OLD_OUT[] = {110.F, 110.F, 110.F};
    long        bad       = 0, n = 0;
    for (float v : OLD_IN)
        bad += !sameBits(v, M::LABEL_FADE_IN_MS), ++n;
    for (float v : OLD_OUT)
        bad += !sameBits(v, M::LABEL_FADE_OUT_MS), ++n;
    // dock/Dock.hpp:430, dock/DockParts.hpp:490-491
    bad += !sameBits(420.F, M::HOP_MS) + !sameBits(0.34F, M::HOP_HEIGHT) + !sameBits(0.42F, M::HOP_UP_SHARE);
    n += 3;
    report("A8 shared timings == the copies they replaced", n, bad);
}

// ============================================================================
// B3 -- one animation clock (MotionClock.hpp). The bodies it replaced, verbatim,
// as the reference: scale 1 must be them bit for bit; 2 doubles every duration;
// 0 is finished at once; a stamp in the future stays a real-time delay.
// ============================================================================
namespace old_b3 {
    using clock = std::chrono::steady_clock;
    // core/Config.cpp animElapsedMs, the scale a parameter.
    [[gnu::noinline]] float configAnim(clock::time_point now, clock::time_point then, float scale) {
        const float SCALE = std::max(0.F, scale);
        if (SCALE <= 0.F)
            return 1e9F;
        return static_cast<float>(std::chrono::duration<float, std::milli>(now - then).count()) / SCALE;
    }
    // stacks/StackFan.cpp tick: DT /= AS (AS > 0).
    [[gnu::noinline]] float stackFanDt(float dt, float as) {
        return dt / as;
    }
    // layout/ShowDesktop.cpp: DT / SCALE, double over float.
    [[gnu::noinline]] double showDesktopDt(double dt, float scale) {
        return dt / scale;
    }
    // dock/CpuPulse.cpp, stacks/StackIcon.cpp, dock/ScratchpadExit.cpp loops.
    [[gnu::noinline]] double cpuPhase(double t, float period) {
        return std::fmod(t, period) / period;
    }
    [[gnu::noinline]] double sweepPhase(double ms) {
        return std::fmod(ms, 1600.0) / 1600.0;
    }
    [[gnu::noinline]] float driftPhase(float ms, float period) {
        return std::fmod(ms, period) / period;
    }
}

static float g_testScale = 1.F;
static float testScale() {
    return g_testScale;
}

static void testB3() {
    namespace M = Hyprdrive::Motion;
    using clock = std::chrono::steady_clock;
    using ns    = std::chrono::nanoseconds;
    M::Clock::g_scaleSource = &testScale;

    const long long BASES[] = {0LL, 3'600'000'000'000LL, 259'200'123'456'789LL};
    std::vector<long long> d;
    for (long long i = 0; i <= 2048; ++i)
        d.push_back(i * 997);
    for (double x = 4097; x < 2e13; x *= 1.031)
        d.push_back(static_cast<long long>(x));
    const size_t N = d.size();
    for (size_t i = 0; i < N; ++i)
        d.push_back(-d[i]);

    long n = 0, bad1 = 0, bad2 = 0, bad0 = 0, badFuture = 0, badD = 0;
    for (long long base : BASES) {
        const clock::time_point THEN{ns{base}};
        for (long long dd : d) {
            const clock::time_point NOW{ns{base + dd}};
            ++n;
            g_testScale = 1.F;
            bad1 += !sameBits(M::Clock::animMsBetween(NOW, THEN), old_b3::configAnim(NOW, THEN, 1.F));
            bad1 += !sameBits(M::Clock::animMsBetween<double>(NOW, THEN), M::msBetween<double>(NOW, THEN));
            const float REAL = M::msBetween(NOW, THEN);
            if (dd >= 0) {
                // 2: the same progress at twice the real time -- every
                // duration takes twice as long; elapsed / 2 is exact.
                g_testScale = 2.F;
                bad2 += !sameBits(M::Clock::animMsBetween(NOW, THEN), old_b3::configAnim(NOW, THEN, 2.F));
                bad2 += !sameBits(M::scaleMs(REAL * 2.F, 2.F), REAL);
                // 0: past every duration at once.
                g_testScale = 0.F;
                bad0 += !(M::Clock::animMsBetween(NOW, THEN) >= M::SNAP_MS) + !sameBits(M::Clock::animMsBetween(NOW, THEN), old_b3::configAnim(NOW, THEN, 0.F));
            } else {
                // A future stamp (a delay still running): real time at every
                // scale, so the delay keeps its length and nothing snaps past it.
                for (float sc : {0.F, 0.5F, 2.F, 10.F}) {
                    g_testScale = sc;
                    badFuture += !sameBits(M::Clock::animMsBetween(NOW, THEN), REAL);
                }
            }
        }
    }
    g_testScale = 1.F;
    report("B3 scale 1: Clock::animMsBetween == Config::animElapsedMs, bit for bit", n, bad1);
    report("B3 scale 2: elapsed halved (durations doubled)", n, bad2);
    report("B3 scale 0: finished at once (SNAP_MS)", n, bad0);
    report("B3 a future stamp is a real-time delay at every scale", n, badFuture);

    // The frame steps the integrators took, at scale 1 and 2, and the snap.
    long nd = 0;
    for (float dt = 0.F; dt < 300.F; dt += 0.173F) {
        ++nd;
        badD += !sameBits(M::scaleDt(dt, 1.F), old_b3::stackFanDt(dt, 1.F)) + !sameBits(M::scaleDt(dt, 2.F), old_b3::stackFanDt(dt, 2.F));
        const double DD = dt / 1000.0;
        badD += !sameBits(M::scaleDt(DD, 1.F), old_b3::showDesktopDt(DD, 1.F)) + !sameBits(M::scaleDt(DD, 2.F), old_b3::showDesktopDt(DD, 2.F));
        badD += !M::isSnapDt(M::scaleDt(dt, 0.F)) + M::isSnapDt(M::scaleDt(dt, 1.F)) + !M::isSnapDt(M::scaleDt(DD, 0.F));
        badD += !sameBits(M::scaleDuration(dt, 1.F), dt) + !sameBits(M::scaleDuration(dt, 2.F), dt * 2.F) + !sameBits(M::scaleDuration(dt, 0.F), 0.F);
    }
    report("B3 frame steps: scale 1/2 == StackFan/ShowDesktop's, 0 snaps", nd, badD);

    // Loops: scale 1 is the old phase; 0 holds the rest phase.
    long nl = 0, badL = 0;
    for (double t = 0.0; t < 5000.0; t += 0.0371) {
        ++nl;
        badL += !sameBits(M::loopPhaseAt<double>(t, 1.2F, 1.F, 0.5), old_b3::cpuPhase(t, 1.2F));
        badL += !sameBits(M::loopPhaseAt<double>(t * 1000.0, 1600.0, 1.F, 0.25), old_b3::sweepPhase(t * 1000.0));
        badL += !sameBits(M::loopPhaseAt<float>(static_cast<float>(t * 100.0), 2400.F, 1.F, 0.F), old_b3::driftPhase(static_cast<float>(t * 100.0), 2400.F));
        badL += !sameBits(M::loopPhaseAt<double>(t, 1.2F, 0.F, 0.5), 0.5);
    }
    report("B3 loops: scale 1 == the old phases, 0 holds still", nl, badL);

    // An integrator handed the snapping step: the closed-form spring lands
    // exactly on its goal (DesktopStrip's row).
    {
        long bad = 0;
        for (double x0 : {-3.0, 0.0, 0.25, 7.5})
            for (double v0 : {-40.0, 0.0, 12.0}) {
                double x = x0, v = v0;
                M::criticalStep(x, v, 2.0, 18.0, M::scaleDt(0.016, 0.F));
                bad += !(x == 2.0 && v == 0.0);
            }
        report("B3 the critical spring settles on a snapping step", 12, bad);
    }

    // SearchLetters on the clock: at 0 a typed letter is at rest the moment it
    // is typed, a deleted one gone, and nothing asks for frames.
    {
        long                     bad = 0;
        SearchLetters::SAnimator A;
        const auto               T0 = clock::time_point{ns{259'200'123'456'789LL}};
        g_testScale                 = 0.F;
        A.sync("ab", T0);
        bad += A.busy(T0);
        const auto P = A.place({10.F, 10.F}, 1.F, 1.F, T0);
        for (const auto& L : P)
            bad += !(L.scale == 1.F && L.y == 0.F && L.opacity == 1.F);
        A.sync("a", T0);
        bad += A.busy(T0);
        A.prune(T0);
        bad += A.letters.size() != 1;
        // ...and at 2 the entrance is still running at 1.9x its 1x length.
        g_testScale = 2.F;
        SearchLetters::SAnimator B;
        B.sync("a", T0);
        const auto LATER = T0 + std::chrono::microseconds(static_cast<long long>(SearchLetters::ENTRANCE_MS * 1900.F));
        bad += !B.busy(LATER);
        bad += B.busy(T0 + std::chrono::microseconds(static_cast<long long>(SearchLetters::ENTRANCE_MS * 2100.F)));
        g_testScale = 1.F;
        report("B3 SearchLetters: snaps at 0, twice as long at 2", 8, bad);
    }
    M::Clock::g_scaleSource = nullptr;
}

// ============================================================================
// B4 -- animations:enabled = 0 folds into the clock: the effective scale is 0
// and every animation is finished at once, whatever anim_scale says. With
// animations on it is anim_scale exactly as Config::animScale read it.
// ============================================================================
static void testB4() {
    namespace M = Hyprdrive::Motion;
    using clock = std::chrono::steady_clock;
    using ns    = std::chrono::nanoseconds;
    long n = 0, badOn = 0, badOff = 0;
    for (float s : {-5.F, -0.F, 0.F, 1e-6F, 0.01F, 0.5F, 1.F, 1.0001F, 2.F, 10.F, 100.F, std::nanf("")}) {
        ++n;
        badOn += !sameBits(M::effectiveScale(s, true), std::max(0.F, s)); // core/Config.cpp animScale, verbatim
        badOff += !sameBits(M::effectiveScale(s, false), 0.F) + !M::snapsAt(M::effectiveScale(s, false));
    }
    report("B4 animations on: the scale is anim_scale as before, bit for bit", n, badOn);
    report("B4 animations off: scale 0 whatever anim_scale is", n, badOff);

    // Through the clock: off = finished immediately (elapsed and step), and a
    // pending delay still waits in real time.
    static float s_off;
    s_off                   = M::effectiveScale(1.F, false);
    M::Clock::g_scaleSource = [] { return s_off; };
    const clock::time_point T0{ns{259'200'123'456'789LL}};
    long bad = 0;
    bad += !(M::Clock::animMsBetween(T0, T0) >= M::SNAP_MS); // elapsed 0, at the start, is already past every duration
    bad += !(M::Clock::animMsBetween(T0 + std::chrono::milliseconds(1), T0) >= M::SNAP_MS);
    bad += !M::isSnapDt(M::Clock::animDt(0.016F));
    bad += !sameBits(M::Clock::animMsBetween(T0, T0 + std::chrono::milliseconds(2000)), -2000.F);
    bad += M::Clock::enabled();
    M::Clock::g_scaleSource = nullptr;
    report("B4 animations off through the clock: snapped, delays real", 5, bad);
}


// ============================================================================
// B2 -- reversal pops. SLitSet (MotionTween.hpp) replaces the two-slot
// "now/prev on one clock" cross-fades of the search rows (O12), the dock menu
// rows (D8) and the strip's resting numbers (O10). An UNINTERRUPTED sequence
// (every change after the last one finished) must give the old levels bit for
// bit; an interrupted one must not jump at the turn, where the old one did.
// ============================================================================
namespace old_b2 {
    using TP = std::chrono::steady_clock::time_point;
    // OverviewSearchView.cpp searchLitT/searchLit and DockMenu.cpp rowLit, verbatim
    // but for the clock (the fade's own ms in, so the test owns time).
    struct SCross {
        int   now = -1, prev = -1;
        TP    at{};
        float ms;
        [[gnu::noinline]] float t(TP clockNow) const {
            const float MS = std::chrono::duration<float, std::milli>(clockNow - at).count();
            return Hyprdrive::Motion::smoothstep(std::clamp(MS / ms, 0.F, 1.F));
        }
        void move(int k, TP clockNow) {
            if (k != now) {
                prev = now;
                now  = k;
                at   = clockNow;
            }
        }
        [[gnu::noinline]] float lit(int i, TP clockNow) const {
            const float T = t(clockNow);
            return (i == now ? T : 0.F) + (i == prev ? 1.F - T : 0.F);
        }
    };
    // OverviewStrip.cpp restingNumberFade, verbatim but for the clock.
    struct SResting {
        size_t cell = std::numeric_limits<size_t>::max(), prev = std::numeric_limits<size_t>::max();
        TP     at{};
        void   move(size_t k, TP clockNow) {
            if (k != cell) {
                prev = cell;
                cell = k;
                at   = clockNow;
            }
        }
        [[gnu::noinline]] float fade(size_t i, TP clockNow) const {
            if (i != cell && i != prev)
                return 1.F;
            const float MS = std::chrono::duration<float, std::milli>(clockNow - at).count();
            const float E  = SearchLetters::easeOutCubic(SearchLetters::clamp01(MS / 160.F));
            return i == cell ? 1.F - E : E;
        }
    };
}

static void testB2() {
    namespace M = Hyprdrive::Motion;
    using TP    = std::chrono::steady_clock::time_point;
    using us    = std::chrono::microseconds;
    const TP T0{std::chrono::nanoseconds{259'200'123'456'789LL}};
    static TP g_now; // the clock the eased functions read
    const auto SMOOTH = [](const M::STween<TP>& f) {
        return M::smoothstep(std::clamp(std::chrono::duration<float, std::milli>(g_now - f.start).count() / 140.F, 0.F, 1.F));
    };
    const auto CUBIC = [](const M::STween<TP>& f) {
        return SearchLetters::easeOutCubic(SearchLetters::clamp01(std::chrono::duration<float, std::milli>(g_now - f.start).count() / 160.F));
    };

    std::mt19937 rng(20261111);
    long n = 0, badExact = 0, badRest = 0;
    // 1. Uninterrupted: random keys, each change >= the duration after the
    //    last. Every key's level on a dense time grid, bit for bit.
    for (int run = 0; run < 200; ++run) {
        old_b2::SCross  O{.ms = 140.F};
        old_b2::SResting OR;
        M::SLitSet<TP>  N, NR{.rest = 1.F, .on = 0.F};
        TP              t = T0;
        for (int step = 0; step < 12; ++step) {
            const int K = static_cast<int>(rng() % 6) - 1; // -1 = none
            g_now       = t;
            O.move(K, t);
            N.moveTo(K, t, SMOOTH);
            const size_t KR = K < 0 ? std::numeric_limits<size_t>::max() : static_cast<size_t>(K);
            OR.move(KR, t);
            NR.moveTo(K < 0 ? M::SLitSet<TP>::NONE : K, t, CUBIC);
            const long GAP = 160'000 + static_cast<long>(rng() % 400'000);
            for (long d = 0; d <= GAP; d += 1'337) {
                g_now = t + us{d};
                // Row indices only: -1 (no row) is never drawn, and the old
                // code answered T for it.
                for (int i = 0; i < 5; ++i) {
                    ++n;
                    badExact += !sameBits(N.level(i, SMOOTH), O.lit(i, g_now));
                    badExact += !sameBits(NR.level(i, CUBIC), OR.fade(static_cast<size_t>(i), g_now));
                }
            }
            t += us{GAP};
        }
        // Settled: the lit key at `on`, every other at rest, exactly.
        g_now = t;
        for (int i = -1; i < 5; ++i) {
            badRest += !(N.level(i, SMOOTH) == (i == N.lit && i >= 0 ? 1.F : 0.F));
            if (i >= 0)
                badRest += !(NR.level(i, CUBIC) == (i == NR.lit ? 0.F : 1.F));
        }
        badRest += N.entries.size() > 2;
    }
    report("B2 SLitSet uninterrupted == the old two-slot cross-fades (search/menu, resting numbers), bit for bit", n, badExact);
    report("B2 SLitSet settles exactly (lit at on, the rest at rest, finished keys dropped)", 200, badRest);

    // 2. Interrupted: changes every 5..120 ms. At every change, every key's
    //    level just before == just after (no jump); the old code jumps.
    long nt = 0, badJump = 0, oldJumps = 0;
    float worstOld = 0.F;
    for (int run = 0; run < 300; ++run) {
        old_b2::SCross O{.ms = 140.F};
        M::SLitSet<TP> N;
        TP             t = T0;
        for (int step = 0; step < 20; ++step) {
            const int K = static_cast<int>(rng() % 3);
            g_now       = t;
            float beforeN[3], beforeO[3];
            for (int i = 0; i < 3; ++i) {
                beforeN[i] = N.level(i, SMOOTH);
                beforeO[i] = O.lit(i, t);
            }
            O.move(K, t);
            N.moveTo(K, t, SMOOTH);
            for (int i = 0; i < 3; ++i) {
                ++nt;
                badJump += std::abs(N.level(i, SMOOTH) - beforeN[i]) > 1e-6F;
                const float J = std::abs(O.lit(i, t) - beforeO[i]);
                oldJumps += J > 1e-3F;
                worstOld = std::max(worstOld, J);
            }
            // ...and it still heads where it should: in range, monotone toward
            // the target over the next leg.
            float last[3];
            for (int i = 0; i < 3; ++i)
                last[i] = N.level(i, SMOOTH);
            const long GAP = 5'000 + static_cast<long>(rng() % 115'000);
            for (long d = 1'000; d <= GAP; d += 1'000) {
                g_now = t + us{d};
                for (int i = 0; i < 3; ++i) {
                    const float L = N.level(i, SMOOTH);
                    badJump += L < 0.F || L > 1.F || (i == K ? L < last[i] - 1e-6F : L > last[i] + 1e-6F);
                    last[i] = L;
                }
            }
            t += us{GAP};
        }
        // Left alone, it arrives exactly.
        g_now = t + us{200'000};
        for (int i = 0; i < 3; ++i)
            badJump += !(N.level(i, SMOOTH) == (i == N.lit ? 1.F : 0.F));
    }
    report("B2 SLitSet interrupted: no jump at a turn, monotone toward the target, exact arrival", nt, badJump);
    std::printf("B2 (contrast) the old two-slot cross-fade jumped at %ld of %ld turns, worst %.3f\n", oldJumps, nt, worstOld);
    if (oldJumps == 0)
        report("B2 the contrast run exercised no old jump (the test proves nothing)", 1, 1);

    // 3. STween's retarget from the DRAWN value (TileChrome's SEase, the dock
    //    tooltip, the strip label): the value is continuous at the turn, where
    //    taking the linear progress (the old SEase) is not.
    {
        long  nn = 0, bad = 0, oldBad = 0;
        const auto OUTQ = [](float x) { return M::outQuad(x); };
        for (float turn = 0.05F; turn < 1.F; turn += 0.05F) {
            M::STween<float> a{.from = 0.F, .to = 1.F, .start = 0.F};
            const float      DRAWN = a.at(OUTQ(turn));
            M::STween<float> b     = a, old = a;
            b.retarget(DRAWN, 0.F, turn);
            old.retarget(a.at(turn), 0.F, turn); // the old SEase: from = linear value()
            ++nn;
            bad += std::abs(b.at(OUTQ(0.F)) - DRAWN) > 0.F;
            oldBad += std::abs(old.at(OUTQ(0.F)) - DRAWN) > 1e-3F;
            bad += !(b.at(OUTQ(1.F)) == 0.F);
        }
        report("B2 STween retarget from the drawn value: continuous at the turn, lands on 0", nn, bad);
        if (oldBad == 0)
            report("B2 the linear-from contrast did not jump (the test proves nothing)", 1, 1);
    }
}


// ============================================================================
// B6 -- the velocity-matched retarget (MotionRetarget.hpp). For each site's
// curve, over random turns: a leg with no kick is the site's old value to the
// bit; a turned leg is continuous at the turn, lands exactly where the plain
// leg lands, never passes its target; heading there already it never steps
// back and never runs slower than FLOOR of the plain leg; a reversal carries
// on the old way by at most its room, has turned by w/3, then only closes in;
// and it is never slower to arrive than the plain retarget.
// ============================================================================
static void testB6() {
    namespace M = Hyprdrive::Motion;
    const auto& TABLE = M::Presets::bakedPresets();
    struct SC {
        const char*                  name;
        std::function<float(float)> f;
    };
    const std::vector<SC> CURVES{
        {"house DECEL", [&](float t) { return TABLE[0][0].getYForPoint(std::clamp(t, 0.F, 1.F)); }},
        {"house ACCEL", [&](float t) { return TABLE[0][1].getYForPoint(std::clamp(t, 0.F, 1.F)); }},
        {"outQuad (border-like)", [](float t) { return M::outQuad(t); }},
        {"outCubic", [](float t) { return M::outCubic(t); }},
        {"linear", [](float t) { return t; }},
        {"outExpo", [](float t) { return t >= 1.F ? 1.F : 1.F - std::pow(2.F, -10.F * t); }},
        {"FLIGHT (site)", [](float t) { return M::houseCurve(M::ESite::FLIGHT, t); }},
        {"DOCK_ARRANGE (site)", [](float t) { return M::houseCurve(M::ESite::DOCK_ARRANGE, t); }},
    };
    std::mt19937                           rng(6);
    std::uniform_real_distribution<double> U01(0.02, 0.97), POS(-2000.0, 2000.0), MS(80.0, 2000.0);
    const double                           CARRIES[] = {0.0, 0.1, 24.0, 32.0, 1e9};
    long   n = 0, turned = 0, badPlain = 0, badCont = 0, badBound = 0, badLand = 0, badMono = 0, badFloor = 0, badCarry = 0, badTurn = 0, badAfter = 0, badLate = 0;
    long   reversals = 0, matched = 0, plainJumped = 0, badWorse = 0;
    double worstCarry = 0.0, worstTurnU = 0.0;
    constexpr int STEPS = 2000;
    for (const auto& C : CURVES) {
        for (int i = 0; i < 6000; ++i) {
            // An old leg (itself possibly kicked), turned at u0 toward a new target.
            const double FROM = POS(rng), TO = POS(rng), WANT = POS(rng);
            const double OLDMS = MS(rng), NEWMS = (i % 2) ? OLDMS : MS(rng);
            const M::SKickLimits LIM{.legMs = NEWMS, .maxCarry = CARRIES[i % 5], .arrive = (i % 3 == 0) ? 1.0 / 255.0 : 1.0 + (i % 4)};
            const M::SKick K0 = (i % 3 == 0) ? M::matchKick(C.f, TO - FROM, POS(rng) * 0.01 * std::abs(TO - FROM + 1), M::SKickLimits{.legMs = OLDMS, .maxCarry = 32.0}) : M::SKick{};
            const double U0   = U01(rng);
            const double CUR  = M::withKick(FROM + (TO - FROM) * C.f(static_cast<float>(U0)), K0, U0);
            const double VEL  = M::legSlope(TO - FROM, M::curveSlope(C.f, U0), K0, U0) * NEWMS / OLDMS;
            const double D    = WANT - CUR;
            const M::SKick K  = M::matchKick(C.f, D, VEL, LIM);
            ++n;
            const double PLAIN37 = CUR + D * C.f(0.37F);
            badPlain += !sameBits(M::withKick(PLAIN37, M::SKick{}, 0.37), PLAIN37);
            if (!K.on() || std::abs(D) < 1e-9)
                continue;
            ++turned;
            const double AD  = std::abs(D);
            const auto   VAL = [&](double u) { return M::withKick(CUR + D * C.f(static_cast<float>(u)), K, u); };
            const auto   PLN = [&](double u) { return CUR + D * C.f(static_cast<float>(u)); };
            badCont += std::abs(VAL(0.0) - CUR) > 1e-9 * (1 + std::abs(CUR));
            badLand += !sameBits(VAL(1.0), PLN(1.0));
            const bool   REV = VEL / D < 0;
            reversals += REV;
            // the start velocity: matched, or (clamped) never further from the old one than the plain retarget's
            const double NEWV = M::legSlope(D, M::curveSlope(C.f, 0.0), K, 0.0), PLAINV = D * M::curveSlope(C.f, 0.0);
            matched += std::abs(NEWV - VEL) <= 1e-6 * (1 + std::abs(VEL));
            plainJumped += std::abs(PLAINV - VEL) > 1e-3 * (1 + std::abs(VEL));
            badWorse += std::abs(NEWV - VEL) > std::abs(PLAINV - VEL) + 1e-6 * (1 + std::abs(VEL));
            // walk the leg in its own direction: s = progress toward the target, in units
            const double SG = D > 0 ? 1.0 : -1.0;
            double       prev = 0.0, carry = 0.0;
            bool         turnedBack = !REV;
            double       turnU      = 0.0;
            for (int j = 1; j <= STEPS; ++j) {
                const double UU = static_cast<double>(j) / STEPS, UP = static_cast<double>(j - 1) / STEPS;
                const double S  = (VAL(UU) - CUR) * SG;
                const double PS = (PLN(UU) - CUR) * SG, PPS = (PLN(UP) - CUR) * SG;
                badBound += S > std::max(AD, PS) + 1e-6 * (1 + AD);
                const double STEP = S - prev, PSTEP = PS - PPS;
                carry = std::max(carry, -S);
                if (!turnedBack && STEP > 0.05 * PSTEP) { // moving the new way, not the grain of a stop
                    turnedBack = true;
                    turnU      = UP;
                }
                if (turnedBack) {
                    badMono += STEP < -1e-5 * (1 + AD);
                    // a reversal, past w/3: at least the plain leg's own speed
                    if (REV && UP >= K.w / 3.0 + 1.0 / STEPS)
                        badAfter += STEP < PSTEP - 1e-7 * (1 + AD);
                    // heading there already: never slower than (nearly) FLOOR of the plain speed
                    if (!REV && PSTEP > 1e-9 * AD)
                        badFloor += STEP < (M::FLOOR - 0.1) * PSTEP;
                }
                prev = S;
            }
            if (REV) {
                const double ROOM = std::min(M::REVERSAL_ROOM * AD, LIM.maxCarry);
                badCarry += carry > ROOM + 2e-3 * AD + 1e-9; // the room is measured on 64 points: slack between them
                worstCarry = std::max(worstCarry, carry / AD);
                badTurn += !turnedBack || turnU > K.w / 3.0 + 2.0 / STEPS;
                worstTurnU = std::max(worstTurnU, turnU / K.w);
            }
            // never slower to arrive than the plain retarget, at the site's precision and finer
            for (const double TAU : {LIM.arrive, LIM.arrive * 0.5}) {
                double arrK = 0.0, arrP = 0.0;
                for (int j = 0; j <= STEPS; ++j) {
                    const double UU = static_cast<double>(j) / STEPS;
                    if (std::abs(WANT - VAL(UU)) > TAU)
                        arrK = UU;
                    if (std::abs(WANT - PLN(UU)) > TAU)
                        arrP = UU;
                }
                badLate += arrK > arrP + 1e-12;
            }
        }
    }
    report("B6 a leg with no kick is the site's plain value, bit for bit", n, badPlain);
    report("B6 turned: value continuous at the turn", turned, badCont);
    report("B6 turned: lands exactly where the plain leg lands", turned, badLand);
    report("B6 turned: never past the target, nor past the plain overshoot", turned * STEPS, badBound);
    report("B6 turned: after the turn it only closes in (monotonic, no step back)", turned * STEPS, badMono);
    report("B6 turned toward: never slower than FLOOR of the plain leg (no stall)", turned * STEPS, badFloor);
    report("B6 reversal: carries on the old way at most its room", reversals, badCarry);
    report("B6 reversal: has turned by w/3", reversals, badTurn);
    report("B6 reversal: past w/3 at least the plain leg's own speed", reversals * STEPS, badAfter);
    report("B6 turned: never slower to arrive than the plain retarget", turned * 2, badLate);
    report("B6 turned: the start velocity is never further from the old one than the plain retarget's", turned, badWorse);
    std::printf("B6 %ld turns kicked of %ld (%ld reversals): start velocity matched in %ld, the plain retarget jumped in %ld; worst carry %.3f of the leg, latest turn %.2f of w\n",
                turned, n, reversals, matched, plainJumped, worstCarry, worstTurnU);
}

// B6, F1: a minimize reversed mid-air into a restore and back, as
// FlightController drives it: alpha 1 -> 0 (or 0 -> 1) over minimize_ms on
// the FLIGHT curve, turned at u0, the new leg kicked with fadeCarry. The
// alpha never leaves 0..1, and the leg lands on the plain flight's value.
static void testB6Flight() {
    namespace M = Hyprdrive::Motion;
    const auto   CURVE = [](float t) { return M::houseCurve(M::ESite::FLIGHT, t); };
    std::mt19937 rng(61);
    std::uniform_real_distribution<double> U01(0.01, 0.99), MS(100.0, 1200.0);
    long n = 0, kicked = 0, badRange = 0, badLand = 0, badCont = 0;
    for (int i = 0; i < 20000; ++i) {
        const bool   MINIMIZING = i % 2; // the old leg
        const double A0 = MINIMIZING ? 1.0 : 0.0, A1 = 1.0 - A0, LEGMS = MS(rng), U0 = U01(rng);
        const double CUR = A0 + (A1 - A0) * CURVE(static_cast<float>(U0));
        const double VEL = (A1 - A0) * M::curveSlope(CURVE, U0); // per u; same duration both legs
        const double D   = A0 - CUR;                              // back the other way
        const M::SKick K = M::matchKick(CURVE, D, VEL, M::SKickLimits{.legMs = LEGMS, .maxCarry = M::fadeCarry(CUR, D), .arrive = 1.0 / 255.0});
        ++n;
        if (!K.on())
            continue;
        ++kicked;
        badCont += std::abs(M::withKick(CUR, K, 0.0) - CUR) > 1e-12;
        for (int j = 0; j <= 1000; ++j) {
            const double U = j / 1000.0, V = M::withKick(CUR + D * CURVE(static_cast<float>(U)), K, U);
            badRange += V < -1e-9 || V > 1.0 + 1e-9;
        }
        badLand += !(M::withKick(CUR + D * CURVE(1.F), K, 1.0) == CUR + D * CURVE(1.F));
    }
    report("B6 F1 a reversed flight's alpha stays inside 0..1", kicked * 1001, badRange);
    report("B6 F1 a reversed flight's alpha is continuous at the turn and lands on the plain value", kicked * 2, badCont + badLand);
    std::printf("B6 F1 %ld of %ld reversals kicked\n", kicked, n);
    if (kicked == 0)
        report("B6 F1 no reversal was kicked (the test proves nothing)", 1, 1);
}

// B6, boxes (matchBoxKick / withBoxKick): the dock glide, the preview panel.
// No kick is the plain box to the bit; a turned box is continuous at the turn
// and lands on the plain box; a nudge, a finished leg and a turn inside the
// last correction take the plain retarget.
static void testB6Box() {
    namespace M = Hyprdrive::Motion;
    struct SB {
        double x = 0, y = 0, width = 0, height = 0;
    };
    const auto   CURVE = [](float t) { return M::houseCurve(M::ESite::DOCK_ARRANGE, t); };
    const auto   LERP  = [](const SB& a, const SB& b, double e) { return SB{a.x + (b.x - a.x) * e, a.y + (b.y - a.y) * e, a.width + (b.width - a.width) * e, a.height + (b.height - a.height) * e}; };
    std::mt19937 rng(62);
    std::uniform_real_distribution<double> P(-800.0, 800.0), Z(4.0, 300.0), U(0.02, 0.95);
    const M::SKickLimits LIM{.legMs = 400.0, .maxCarry = 16.0, .arrive = 0.5};
    long n = 0, kicked = 0, badPlain = 0, badCont = 0, badLand = 0, badNudge = 0, badBurst = 0, badSize = 0;
    for (int i = 0; i < 20000; ++i) {
        const SB     F{P(rng), P(rng), Z(rng), Z(rng)}, T{P(rng), P(rng), Z(rng), Z(rng)}, W{P(rng), P(rng), Z(rng), Z(rng)};
        const double U0  = U(rng);
        const SB     CUR = LERP(F, T, CURVE(static_cast<float>(U0)));
        const auto   K   = M::matchBoxKick(CURVE, F, T, M::SBoxKick{}, U0, 1.0, CUR, W, LIM);
        ++n;
        const SB P37 = LERP(CUR, W, CURVE(0.37F));
        const SB Q37 = M::withBoxKick(P37, M::SBoxKick{}, 0.37);
        badPlain += !(sameBits(Q37.x, P37.x) && sameBits(Q37.y, P37.y) && sameBits(Q37.width, P37.width) && sameBits(Q37.height, P37.height));
        const SB NUDGE{T.x + 3.0, T.y - 2.0, T.width + 1.0, T.height};
        badNudge += M::boxKicked(M::matchBoxKick(CURVE, F, T, M::SBoxKick{}, U0, 1.0, CUR, NUDGE, LIM));
        badNudge += M::boxKicked(M::matchBoxKick(CURVE, F, T, M::SBoxKick{}, 1.0, 1.0, CUR, W, LIM));
        if (!M::boxKicked(K))
            continue;
        ++kicked;
        badBurst += M::boxKicked(M::matchBoxKick(CURVE, CUR, W, K, 0.01, 1.0, M::withBoxKick(LERP(CUR, W, CURVE(0.01F)), K, 0.01), T, LIM));
        const SB A0 = M::withBoxKick(LERP(CUR, W, CURVE(0.F)), K, 0.0), A1 = M::withBoxKick(LERP(CUR, W, CURVE(1.F)), K, 1.0), P1 = LERP(CUR, W, CURVE(1.F));
        badCont += std::abs(A0.x - CUR.x) > 1e-9 || std::abs(A0.y - CUR.y) > 1e-9 || std::abs(A0.width - CUR.width) > 1e-9 || std::abs(A0.height - CUR.height) > 1e-9;
        badLand += !(A1.x == P1.x && A1.y == P1.y && A1.width == P1.width && A1.height == P1.height);
        for (int j = 0; j <= 200; ++j) {
            const SB B = M::withBoxKick(LERP(CUR, W, CURVE(j / 200.F)), K, j / 200.0);
            badSize += !(B.width >= 2.0 && B.height >= 2.0 && std::isfinite(B.x) && std::isfinite(B.y));
        }
    }
    report("B6 box: no kick is the plain box, bit for bit", n, badPlain);
    report("B6 box: a nudge or a finished leg takes the plain retarget", n * 2, badNudge);
    report("B6 box: a turn inside the last correction takes the plain retarget", kicked, badBurst);
    report("B6 box: turned, continuous at the turn and lands on the plain box", kicked * 2, badCont + badLand);
    report("B6 box: finite, never under 2 px", kicked * 201, badSize);
    if (kicked == 0)
        report("B6 box: nothing was kicked (the test proves nothing)", 1, 1);
}

int main() {
    testA1();
    testA2();
    testA3();
    testA4();
    testA5();
    testA6();
    testA7();
    testA8();
    testB3();
    testB4();
    testB2();
    testB6();
    testB6Flight();
    testB6Box();

    std::printf("motion_math_test: %d checks, %ld bitwise comparisons, %ld differ\n", g_checks, g_compared, g_failed);
    return g_checkFailed ? 1 : 0;
}
