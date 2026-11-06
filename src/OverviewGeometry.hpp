#pragma once

// The overview's PURE geometry: the transform every overview path shares, the
// axis helpers it is parameterised on, the box predicates hit-testing and drag
// resolution ask, and the clamps the resize handles apply.
//
// WHY THIS FILE EXISTS AT ALL. B1 says render, hit-test and drag must place a
// tile with the SAME transform, and for most of this tree's life the only way to
// check that was to photograph a tile. This math needs no compositor, no
// monitor, no config and no window: it is arithmetic on a box. Pulled out of
// OverviewInternal.hpp -- which cannot be included outside the plugin, and
// cannot be compiled without Hyprland -- it becomes the first module a host-side
// unit test can hold. That test is scripts/geometry_test.cpp and `make check`
// runs it.
//
// THE RULE FOR WHAT MAY LIVE HERE: no Hyprland header, no config read, no
// global. If a function needs the monitor it takes SViewport; if it needs gaps
// it takes SGaps; if it needs the configured overview scale it takes a float.
// The wrappers in OverviewInternal.hpp keep the old names and the old
// PHLMONITOR signatures and do nothing but fill those structs in, so no call
// site changed and that header stays the plugin's vocabulary.
//
// hyprutils IS allowed: CBox and Vector2D are a maths library that happens to
// ship with Hyprland, they link with -lhyprutils alone, and rewriting them here
// to avoid a dependency the test can satisfy in one flag would be the kind of
// purity nobody asked for.

#include <hyprutils/math/Box.hpp>
#include <hyprutils/math/Vector2D.hpp>

#include <cstdint>
#include <optional>

namespace ScrollOverview::Geometry {

    using Hyprutils::Math::CBox;
    using Hyprutils::Math::Vector2D;

    // Mirrors ScrollOverview::Config::ELayout value for value. OverviewInternal.hpp
    // static_asserts the correspondence at the boundary and casts; this header must
    // not include Config.hpp, which drags in the whole compositor.
    enum class EAxis : uint8_t {
        VERTICAL   = 0,
        HORIZONTAL = 1,
    };

    // Mirrors Layout::eRectCorner, which is a BITMASK (CORNER_TOPLEFT == TOP|LEFT).
    // Same static_assert treatment at the boundary.
    enum ECorner : uint8_t {
        CORNER_NONE        = 0,
        CORNER_TOP         = (1 << 0),
        CORNER_BOTTOM      = (1 << 1),
        CORNER_LEFT        = (1 << 2),
        CORNER_RIGHT       = (1 << 3),
        CORNER_TOPLEFT     = CORNER_TOP | CORNER_LEFT,
        CORNER_TOPRIGHT    = CORNER_TOP | CORNER_RIGHT,
        CORNER_BOTTOMLEFT  = CORNER_BOTTOM | CORNER_LEFT,
        CORNER_BOTTOMRIGHT = CORNER_BOTTOM | CORNER_RIGHT,
    };

    // Everything the transforms below ever read off a monitor. Three numbers.
    // `scale` is a FLOAT and not a double on purpose: CMonitor::m_scale is a
    // float, and widening it here would change the last bits of every product
    // in overviewBox -- which is a rounding boundary away from moving a tile by
    // a pixel for no reason anyone could find later.
    struct SViewport {
        Vector2D position;     // global logical position of the output
        Vector2D size;         // logical size of the output
        float    scale = 1.0F; // the output's own scale factor
        // Extra translation after scale-about-centre, in monitor-local render
        // px. Used to sit the floating card in the band between the top bar
        // and the dock so those two paddings match. Zero keeps the old
        // monitor-centre transform (geometry_test).
        Vector2D originOffset{};

        Vector2D renderSize() const {
            return size * scale;
        }
    };

    // general:gaps_in, already resolved. Negative entries are clamped inside
    // expandHitbox exactly as the original did.
    struct SGaps {
        int64_t left = 0, top = 0, right = 0, bottom = 0;
    };

    // --- axis helpers: the overview is one layout parameterised on an axis -----
    Vector2D axisOffsetVector(float offset, EAxis axis);
    float    axisValue(const Vector2D& vector, EAxis axis);
    float    axisSize(const Vector2D& size, EAxis axis);

    // --- THE transform. B1: render, hit-test and drag all come through here. ---
    // `box` is monitor-local logical; the result is monitor-local render px.
    CBox overviewBox(CBox box, const SViewport& viewport, float scale, const Vector2D& viewOffset, float offset, EAxis axis, bool round = true);
    // Same, for a box given in GLOBAL logical coordinates.
    CBox overviewGlobalBox(CBox globalBox, const SViewport& viewport, float scale, const Vector2D& viewOffset, float offset, EAxis axis, bool round = true);
    // The whole workspace rectangle under that transform.
    CBox overviewWorkspaceBox(const SViewport& viewport, float scale, const Vector2D& viewOffset, float offset, EAxis axis);

    // --- hit-testing ----------------------------------------------------------
    CBox  expandHitbox(CBox box, float scale, float monitorScale, const SGaps& gaps);
    float pointDistanceSqToBox(const Vector2D& point, const CBox& box);

    // --- box predicates the drag and the strip ask ---------------------------
    bool   boxIntersectsViewport(const CBox& box, const SViewport& viewport);
    bool   boxFullyVisibleInViewport(const CBox& box, const SViewport& viewport);
    double boxIntersectionArea(const CBox& a, const CBox& b);
    double boxArea(const CBox& box);
    double boxCenterDistanceSquared(const CBox& a, const CBox& b);
    // Sub-half-pixel equality: two boxes that round to the same pixels.
    bool   boxesEqual(const CBox& a, const CBox& b);
    // The same tolerance, said the other way, for "has the target moved".
    bool   boxesDiffer(const CBox& a, const CBox& b);

    // --- placement ------------------------------------------------------------
    CBox centerBoxIn(CBox box, const CBox& container);
    CBox clampBoxTo(CBox box, const CBox& container, float margin = 0.F);
    CBox resizedBoxFromCorner(const CBox& originalBox, const Vector2D& delta, uint8_t corner, const Vector2D& minSizePx, const std::optional<Vector2D>& maxSizePx);
    CBox clampResizedBoxTo(const CBox& box, const CBox& container, uint8_t corner, float marginPx);

    // --- the desktop strip's spacing -----------------------------------------
    float workspaceRenderedPitch(const SViewport& viewport, float scale, EAxis axis, float workspaceGap);
    float workspaceLogicalPitch(const SViewport& viewport, float scale, EAxis axis, float workspaceGap);

    // --- overlap, for "which window is next to which" ------------------------
    // The 1-D overlap of two spans, never negative. The window-pair helpers in
    // OverviewInternal.hpp are this function with the geometry read off first.
    float spanOverlap(double aStart, double aSize, double bStart, double bSize);

    // --- interpolation --------------------------------------------------------
    // 0 while the overview is closed, 1 once it is fully open. `targetScale` is
    // the configured open scale; at 1 there is nothing to travel and it is 1.
    float    gridProgress(float scale, float targetScale);
    CBox     lerpBox(const CBox& from, const CBox& to, float t);
    float    hyprlerp(float from, float to, float perc);
    Vector2D hyprlerp(const Vector2D& from, const Vector2D& to, float perc);

} // namespace ScrollOverview::Geometry
