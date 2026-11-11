// Host-side unit test for Hyprdrive::Geometry -- the overview's pure
// transforms. Needs no compositor, no Hyprland, no display: it links
// OverviewGeometry.cpp against -lhyprutils and does arithmetic.
//
//   g++ -std=c++2b -O2 -o /tmp/geometry_test tests/unit/geometry_test.cpp \
//       OverviewGeometry.cpp $(pkg-config --cflags --libs hyprutils) && /tmp/geometry_test
//
// `make check` runs exactly that.
//
// WHAT IT IS FOR. B1 -- render, hit-test and drag place a tile with the SAME
// transform -- has until now been checkable only by photographing a tile, which
// costs a compositor, a fixture and three minutes, and which cannot tell a
// transform that is wrong everywhere from a fixture that is in the wrong place.
// The GOLDENs below are the transform's answers computed by hand, off the
// tree, for cases with exact arithmetic; the PROPERTYs are the algebraic
// identities the rest of the plugin leans on (offset is a translation, scale
// fixes the viewport centre, the resize corner opposite the grabbed one does
// not move). Either kind failing is a real bug, and neither can be satisfied by
// a lucky picture.

#include "OverviewGeometry.hpp"
#include "MotionMath.hpp"

#include <cmath>
#include <cstdio>
#include <optional>
#include <string>

using namespace Hyprdrive::Geometry;

static int g_fail = 0, g_run = 0;

static void ok(bool cond, const std::string& what) {
    ++g_run;
    if (!cond) {
        ++g_fail;
        std::printf("FAIL  %s\n", what.c_str());
    }
}

static bool near(double a, double b, double eps = 1e-4) {
    return std::abs(a - b) <= eps;
}

static bool boxIs(const CBox& b, double x, double y, double w, double h, double eps = 1e-4) {
    return near(b.x, x, eps) && near(b.y, y, eps) && near(b.w, w, eps) && near(b.h, h, eps);
}

static std::string show(const CBox& b) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "[%.3f %.3f %.3f %.3f]", b.x, b.y, b.w, b.h);
    return buf;
}

// The rig's output, and a HiDPI one, because monitorScale multiplies in two
// places (the pre-scale of the box and the post-scale of the view offset) and a
// scale of 1 hides a mistake in either.
static const SViewport VP1{{0, 0}, {2560, 1600}, 1.0F};
static const SViewport VP2{{1920, 0}, {1280, 800}, 2.0F};

int main() {
    // ---- axis helpers --------------------------------------------------------
    ok(axisOffsetVector(7.F, EAxis::HORIZONTAL) == Vector2D(7, 0), "axisOffsetVector horizontal");
    ok(axisOffsetVector(7.F, EAxis::VERTICAL) == Vector2D(0, 7), "axisOffsetVector vertical");
    ok(near(axisValue({3, 5}, EAxis::HORIZONTAL), 3), "axisValue horizontal");
    ok(near(axisValue({3, 5}, EAxis::VERTICAL), 5), "axisValue vertical");
    ok(near(axisSize({3, 5}, EAxis::HORIZONTAL), 3), "axisSize horizontal");
    ok(near(axisSize({3, 5}, EAxis::VERTICAL), 5), "axisSize vertical");

    // ---- GOLDEN: the transform, computed by hand ----------------------------
    // VP1, overview scale 0.6, no pan, no offset. Centre {1280,800}:
    //   {100,200,800,600} -> -centre {-1180,-600,800,600} -> *0.6
    //   {-708,-360,480,360} -> +centre {572,440,480,360}.
    {
        const auto B = overviewBox({100, 200, 800, 600}, VP1, 0.6F, {}, 0.F, EAxis::HORIZONTAL);
        ok(boxIs(B, 572, 440, 480, 360), "GOLDEN overviewBox VP1 s=0.6 -> " + show(B));
    }
    // VP2: monitorScale 2 pre-multiplies the box, and the centre is renderSize/2
    //   = {1280,800} again. {100,200,800,600}*2 = {200,400,1600,1200}
    //   -> -centre {-1080,-400,1600,1200} -> *0.6 {-648,-240,960,720}
    //   -> +centre {632,560,960,720}.
    {
        const auto B = overviewBox({100, 200, 800, 600}, VP2, 0.6F, {}, 0.F, EAxis::HORIZONTAL);
        ok(boxIs(B, 632, 560, 960, 720), "GOLDEN overviewBox VP2 s=0.6 -> " + show(B));
    }
    // The pan is applied in RENDER px after the scale: -viewOffset*scale*mScale
    //   = -{50,25}*0.6*2 = {-60,-30}.
    {
        const auto B = overviewBox({100, 200, 800, 600}, VP2, 0.6F, {50, 25}, 0.F, EAxis::HORIZONTAL);
        ok(boxIs(B, 572, 530, 960, 720), "GOLDEN overviewBox VP2 pan -> " + show(B));
    }
    // overviewGlobalBox subtracts the output's global position FIRST, so a box
    // at {2020,200} on an output at x=1920 is the local {100,200} above.
    {
        const auto B = overviewGlobalBox({2020, 200, 800, 600}, VP2, 0.6F, {}, 0.F, EAxis::HORIZONTAL);
        ok(boxIs(B, 632, 560, 960, 720), "GOLDEN overviewGlobalBox VP2 -> " + show(B));
    }
    // The workspace rectangle at scale 1 is the whole render surface.
    {
        const auto B = overviewWorkspaceBox(VP2, 1.F, {}, 0.F, EAxis::HORIZONTAL);
        ok(boxIs(B, 0, 0, 2560, 1600), "GOLDEN overviewWorkspaceBox VP2 s=1 -> " + show(B));
    }

    // ---- PROPERTY: at scale 1 with no pan the transform is monitorScale ------
    for (const auto& VP : {VP1, VP2}) {
        const CBox IN{10, 20, 30, 40};
        const auto B = overviewBox(IN, VP, 1.F, {}, 0.F, EAxis::VERTICAL);
        ok(boxIs(B, IN.x * VP.scale, IN.y * VP.scale, IN.w * VP.scale, IN.h * VP.scale), "PROPERTY identity at scale 1 -> " + show(B));
    }

    // ---- PROPERTY: offset is a pure translation along the axis --------------
    // The whole horizontal/vertical layout switch rests on this: the desktop
    // strip lays workspaces out by handing the same box a different offset.
    for (const auto AXIS : {EAxis::HORIZONTAL, EAxis::VERTICAL}) {
        const auto A = overviewBox({100, 200, 800, 600}, VP2, 0.6F, {3, 4}, 0.F, AXIS, false);
        const auto B = overviewBox({100, 200, 800, 600}, VP2, 0.6F, {3, 4}, 137.F, AXIS, false);
        const auto D = axisOffsetVector(137.F, AXIS);
        ok(boxIs(B, A.x + D.x, A.y + D.y, A.w, A.h), "PROPERTY offset translates only");
    }

    // ---- PROPERTY: the viewport centre is the fixed point of the scale ------
    // Why the overview grows out of the middle of the screen rather than the
    // corner, and why chrome pinned with OVERVIEW_CHROME_NO_PAN stays put.
    for (float s : {0.2F, 0.5F, 1.F, 1.7F}) {
        // VP1 is scale 1, so logical == render: a 100x100 box centred on
        // {1280,800} is centred on the viewport centre.
        const CBox CENTRED{1230, 750, 100, 100};
        const auto B = overviewBox(CENTRED, VP1, s, {}, 0.F, EAxis::HORIZONTAL, false);
        ok(near(B.middle().x, VP1.renderSize().x / 2) && near(B.middle().y, VP1.renderSize().y / 2), "PROPERTY centre is the scale's fixed point");
    }

    // ---- PROPERTY: sizes scale by scale*monitorScale ------------------------
    for (const auto& VP : {VP1, VP2}) {
        for (float s : {0.25F, 0.6F, 1.F}) {
            const auto B = overviewBox({0, 0, 400, 300}, VP, s, {}, 0.F, EAxis::HORIZONTAL, false);
            ok(near(B.w, 400 * s * VP.scale) && near(B.h, 300 * s * VP.scale), "PROPERTY size scales by scale*monitorScale");
        }
    }

    // ---- hit-testing --------------------------------------------------------
    {
        const SGaps G{5, 7, 5, 7};
        const auto  B = expandHitbox({100, 100, 200, 200}, 0.5F, 2.F, G);
        // TOTALSCALE 1, multiplier 2: left/top pulled out by 10 and 14, width
        // grows by both left and right gaps.
        ok(boxIs(B, 90, 86, 220, 228), "expandHitbox grows by gaps*2*scale -> " + show(B));
    }
    {
        // Negative gaps are a legal Hyprland config and must not SHRINK the
        // hitbox: the original clamps each side at 0 independently.
        const SGaps G{-40, -40, 6, 6};
        const auto  B = expandHitbox({100, 100, 200, 200}, 1.F, 1.F, G);
        ok(boxIs(B, 100, 100, 212, 212), "expandHitbox clamps negative gaps -> " + show(B));
    }
    {
        const CBox BOX{100, 100, 200, 100};
        ok(near(pointDistanceSqToBox({150, 150}, BOX), 0), "pointDistanceSq inside is 0");
        ok(near(pointDistanceSqToBox({150, 100}, BOX), 0), "pointDistanceSq on the edge is 0");
        ok(near(pointDistanceSqToBox({97, 150}, BOX), 9), "pointDistanceSq left of the box");
        ok(near(pointDistanceSqToBox({97, 96}, BOX), 9 + 16), "pointDistanceSq off a corner is the diagonal");
        ok(near(pointDistanceSqToBox({304, 203}, BOX), 16 + 9), "pointDistanceSq off the far corner");
    }

    // ---- viewport predicates ------------------------------------------------
    ok(boxIntersectsViewport({-10, -10, 20, 20}, VP1), "a box straddling the origin intersects");
    ok(!boxIntersectsViewport({-30, 0, 20, 20}, VP1), "a box entirely left of the output does not");
    ok(!boxIntersectsViewport({0, 0, 0, 20}, VP1), "a zero-width box intersects nothing");
    ok(boxIntersectsViewport({2550, 1590, 20, 20}, VP1), "a box straddling the far corner intersects");
    ok(!boxIntersectsViewport({2560, 0, 20, 20}, VP1), "a box starting exactly at the right edge does not");
    ok(boxFullyVisibleInViewport({0, 0, 2560, 1600}, VP1), "the exact viewport is fully visible");
    ok(!boxFullyVisibleInViewport({0, 0, 2561, 1600}, VP1), "one pixel over is not");
    ok(!boxFullyVisibleInViewport({-1, 0, 10, 10}, VP1), "one pixel left is not");
    // VP2 is 1280x800 at scale 2, so its RENDER surface is 2560x1600 -- the
    // predicates work in render px, which is the bug this line would catch.
    ok(boxFullyVisibleInViewport({0, 0, 2560, 1600}, VP2), "predicates are in render px, not logical");

    // ---- areas and distances ------------------------------------------------
    ok(near(boxIntersectionArea({0, 0, 10, 10}, {5, 5, 10, 10}), 25), "intersection area");
    ok(near(boxIntersectionArea({0, 0, 10, 10}, {20, 20, 10, 10}), 0), "disjoint boxes have no intersection");
    ok(near(boxArea({0, 0, 10, 10}), 100), "box area");
    ok(near(boxArea({0, 0, -10, 10}), 0), "a negative box has no area");
    ok(near(boxCenterDistanceSquared({0, 0, 10, 10}, {30, 40, 10, 10}), 900 + 1600), "centre distance squared");

    // ---- the half-pixel tolerance both spellings share ----------------------
    ok(boxesEqual({0, 0, 10, 10}, {0.4, 0, 10, 10}), "boxesEqual tolerates under half a pixel");
    ok(!boxesEqual({0, 0, 10, 10}, {0.6, 0, 10, 10}), "boxesEqual rejects over half a pixel");
    ok(!boxesDiffer({0, 0, 10, 10}, {0.4, 0, 10, 10}), "boxesDiffer agrees under half a pixel");
    ok(boxesDiffer({0, 0, 10, 10}, {0, 0, 10, 10.6}), "boxesDiffer sees a size change too");

    // ---- placement ----------------------------------------------------------
    {
        const auto B = centerBoxIn({0, 0, 100, 50}, {200, 100, 400, 300});
        ok(boxIs(B, 200 + 150, 100 + 125, 100, 50), "centerBoxIn -> " + show(B));
        // A box bigger than its container is pinned to the container's corner,
        // not pushed off it by a negative half-difference.
        const auto C = centerBoxIn({0, 0, 900, 900}, {200, 100, 400, 300});
        ok(boxIs(C, 200, 100, 900, 900), "centerBoxIn never pushes an oversized box out");
    }
    {
        const CBox CONT{0, 0, 1000, 1000};
        ok(boxIs(clampBoxTo({-50, -50, 100, 100}, CONT), 0, 0, 100, 100), "clampBoxTo pulls back from the near edge");
        ok(boxIs(clampBoxTo({2000, 2000, 100, 100}, CONT), 900, 900, 100, 100), "clampBoxTo pulls back from the far edge");
        ok(boxIs(clampBoxTo({-50, -50, 100, 100}, CONT, 20.F), 20, 20, 100, 100), "clampBoxTo honours the margin");
        // An oversized box cannot satisfy both margins; it takes the near one.
        ok(boxIs(clampBoxTo({-50, -50, 5000, 5000}, CONT, 20.F), 20, 20, 5000, 5000), "clampBoxTo degrades to the near margin");
        ok(boxIs(clampBoxTo({5, 5, 10, 10}, {0, 0, 0, 0}), 5, 5, 10, 10), "an empty container clamps nothing");
    }

    // ---- resize: the corner opposite the grabbed one does not move ----------
    {
        const CBox ORIG{100, 100, 400, 300};
        struct {
            uint8_t  corner;
            Vector2D anchor;
            const char* name;
        } CASES[] = {
            {CORNER_TOPLEFT, {500, 400}, "topleft"},
            {CORNER_TOPRIGHT, {100, 400}, "topright"},
            {CORNER_BOTTOMLEFT, {500, 100}, "bottomleft"},
            {CORNER_BOTTOMRIGHT, {100, 100}, "bottomright"},
        };
        for (const auto& C : CASES) {
            for (const Vector2D D : {Vector2D{37, -21}, Vector2D{-90, 60}, Vector2D{0, 0}}) {
                const auto B = resizedBoxFromCorner(ORIG, D, C.corner, {1, 1}, std::nullopt);
                const double L = B.x, T = B.y, R = B.x + B.w, BT = B.y + B.h;
                const bool   ANCHORHELD = (near(L, C.anchor.x) || near(R, C.anchor.x)) && (near(T, C.anchor.y) || near(BT, C.anchor.y));
                ok(ANCHORHELD, std::string("PROPERTY resize holds the opposite corner (") + C.name + ") -> " + show(B));
            }
        }
        // A drag far past the minimum stops at the minimum, and still holds the
        // anchor -- the two rules together are what stops a resize inverting.
        const auto TINY = resizedBoxFromCorner(ORIG, {-9999, -9999}, CORNER_BOTTOMRIGHT, {60, 40}, std::nullopt);
        ok(boxIs(TINY, 100, 100, 60, 40), "resize clamps to the minimum size -> " + show(TINY));
        const auto TINYTL = resizedBoxFromCorner(ORIG, {9999, 9999}, CORNER_TOPLEFT, {60, 40}, std::nullopt);
        ok(boxIs(TINYTL, 440, 360, 60, 40), "resize from the top-left clamps against the fixed bottom-right -> " + show(TINYTL));
        const auto HUGE = resizedBoxFromCorner(ORIG, {9999, 9999}, CORNER_BOTTOMRIGHT, {1, 1}, Vector2D{800, 500});
        ok(boxIs(HUGE, 100, 100, 800, 500), "resize clamps to the maximum size -> " + show(HUGE));
        // A max below the min is nonsense the caller can produce; min wins.
        const auto MINWINS = resizedBoxFromCorner(ORIG, {9999, 9999}, CORNER_BOTTOMRIGHT, {600, 400}, Vector2D{10, 10});
        ok(boxIs(MINWINS, 100, 100, 600, 400), "a maximum under the minimum loses");
    }

    // ---- resize clamp: only the grabbed edges are pushed back ---------------
    {
        const CBox CONT{0, 0, 1000, 1000};
        const auto B = clampResizedBoxTo({-50, -50, 400, 300}, CONT, CORNER_TOPLEFT, 10.F);
        ok(boxIs(B, 10, 10, 340, 240), "clampResizedBoxTo moves the grabbed top-left only -> " + show(B));
        const auto C = clampResizedBoxTo({800, 800, 400, 300}, CONT, CORNER_BOTTOMRIGHT, 10.F);
        ok(boxIs(C, 800, 800, 190, 190), "clampResizedBoxTo shortens against the far margin -> " + show(C));
        // The corner NOT grabbed is left alone even when it is out of bounds --
        // the drag only owns the edges the pointer is holding.
        const auto D = clampResizedBoxTo({-50, -50, 400, 300}, CONT, CORNER_BOTTOMRIGHT, 0.F);
        ok(boxIs(D, -50, -50, 400, 300), "clampResizedBoxTo leaves the ungrabbed corner where it is");
        ok(boxIs(clampResizedBoxTo({-50, -50, 400, 300}, {0, 0, 0, 0}, CORNER_TOPLEFT, 10.F), -50, -50, 400, 300), "an empty container clamps no resize");
    }

    // ---- strip pitch --------------------------------------------------------
    ok(near(workspaceRenderedPitch(VP2, 0.5F, EAxis::HORIZONTAL, 40.F), (1280 * 0.5F + 40) * 2), "workspaceRenderedPitch");
    ok(near(workspaceLogicalPitch(VP2, 0.5F, EAxis::VERTICAL, 40.F), 800 + 40 / 0.5F), "workspaceLogicalPitch");
    // The pitch divides by the scale, so a zero scale would be a division by
    // zero on the frame the overview opens.
    ok(std::isfinite(workspaceLogicalPitch(VP2, 0.F, EAxis::VERTICAL, 40.F)), "workspaceLogicalPitch survives scale 0");

    // ---- overlap ------------------------------------------------------------
    ok(near(spanOverlap(0, 100, 50, 100), 50), "spanOverlap partial");
    ok(near(spanOverlap(0, 100, 200, 100), 0), "spanOverlap disjoint is 0, never negative");
    ok(near(spanOverlap(0, 100, 10, 20), 20), "spanOverlap contained is the inner span");

    // ---- grid progress ------------------------------------------------------
    ok(near(gridProgress(1.F, 0.6F), 0), "gridProgress is 0 while the overview is closed");
    ok(near(gridProgress(0.6F, 0.6F), 1), "gridProgress is 1 once it is open");
    ok(near(gridProgress(0.8F, 0.6F), 0.5), "gridProgress is linear between them");
    ok(near(gridProgress(0.1F, 0.6F), 1), "gridProgress clamps past the target");
    ok(near(gridProgress(1.4F, 0.6F), 0), "gridProgress clamps before the start");
    // A configured scale of 1 means the tiles never leave their desktop rects;
    // the ratio would be 0/0 and the guard makes it 1 instead of NaN.
    ok(near(gridProgress(1.F, 1.F), 1), "gridProgress at target scale 1 is 1, not NaN");

    // ---- interpolation ------------------------------------------------------
    // (Motion::lerp, MotionMath.hpp -- motion_math_test holds it to the copies it replaced.)
    using Hyprdrive::Motion::lerp;
    ok(boxIs(lerp(CBox{0, 0, 10, 10}, CBox{100, 200, 30, 50}, 0.F), 0, 0, 10, 10), "lerp box at t=0");
    ok(boxIs(lerp(CBox{0, 0, 10, 10}, CBox{100, 200, 30, 50}, 1.F), 100, 200, 30, 50), "lerp box at t=1");
    ok(boxIs(lerp(CBox{0, 0, 10, 10}, CBox{100, 200, 30, 50}, 0.5F), 50, 100, 20, 30), "lerp box halfway");
    ok(near(lerp(10.F, 20.F, 0.25F), 12.5), "lerp float");
    ok(lerp(Vector2D{0, 0}, Vector2D{10, 20}, 0.5F) == Vector2D(5, 10), "lerp Vector2D");

    std::printf("geometry_test: %d checks, %d failed\n", g_run, g_fail);
    return g_fail ? 1 : 0;
}
