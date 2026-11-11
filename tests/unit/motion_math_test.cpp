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

#include <algorithm>
#include <chrono>
#include <cmath>
#include <optional>
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

int main() {
    testA1();
    testA2();
    testA3();
    testA4();

    std::printf("motion_math_test: %d checks, %ld bitwise comparisons, %ld differ\n", g_checks, g_compared, g_failed);
    return g_checkFailed ? 1 : 0;
}
