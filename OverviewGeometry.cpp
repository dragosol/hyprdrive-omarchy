#include "OverviewGeometry.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

// Every body here is the one that was in OverviewInternal.hpp, transliterated
// with the compositor lookups turned into parameters. Where the original read
// `monitor->m_scale` it now reads `viewport.scale`; where it read
// Config::getScale() or Config::getWorkspaceGap() the caller passes the value.
// Nothing else changed -- deliberately, so a disagreement between the old
// pixels and the new ones is a transcription slip and not a redesign.

namespace ScrollOverview::Geometry {

    Vector2D axisOffsetVector(float offset, EAxis axis) {
        return axis == EAxis::HORIZONTAL ? Vector2D{offset, 0.F} : Vector2D{0.F, offset};
    }

    float axisValue(const Vector2D& vector, EAxis axis) {
        return axis == EAxis::HORIZONTAL ? vector.x : vector.y;
    }

    float axisSize(const Vector2D& size, EAxis axis) {
        return axis == EAxis::HORIZONTAL ? size.x : size.y;
    }

    CBox overviewBox(CBox box, const SViewport& viewport, float scale, const Vector2D& viewOffset, float offset, EAxis axis, bool round) {
        const auto MONITORSCALE    = viewport.scale;
        const auto VIEWPORT_CENTER = CBox{{}, viewport.size * MONITORSCALE}.middle();

        box = {box.pos() * MONITORSCALE, box.size() * MONITORSCALE};
        box.translate(-VIEWPORT_CENTER)
            .scale(scale)
            .translate(VIEWPORT_CENTER)
            .translate(viewport.originOffset)
            .translate(-viewOffset * scale * MONITORSCALE)
            .translate(axisOffsetVector(offset, axis));
        if (round)
            box.round();

        return box;
    }

    CBox overviewGlobalBox(CBox globalBox, const SViewport& viewport, float scale, const Vector2D& viewOffset, float offset, EAxis axis, bool round) {
        return overviewBox({globalBox.pos() - viewport.position, globalBox.size()}, viewport, scale, viewOffset, offset, axis, round);
    }

    CBox overviewWorkspaceBox(const SViewport& viewport, float scale, const Vector2D& viewOffset, float offset, EAxis axis) {
        return overviewBox({{}, viewport.size}, viewport, scale, viewOffset, offset, axis);
    }

    CBox expandHitbox(CBox box, float scale, float monitorScale, const SGaps& gaps) {
        constexpr float GAP_MULTIPLIER = 2.F;
        const float     TOTALSCALE     = scale * monitorScale;

        box.x -= static_cast<float>(std::max<int64_t>(0, gaps.left)) * TOTALSCALE * GAP_MULTIPLIER;
        box.y -= static_cast<float>(std::max<int64_t>(0, gaps.top)) * TOTALSCALE * GAP_MULTIPLIER;
        box.width += static_cast<float>(std::max<int64_t>(0, gaps.left) + std::max<int64_t>(0, gaps.right)) * TOTALSCALE * GAP_MULTIPLIER;
        box.height += static_cast<float>(std::max<int64_t>(0, gaps.top) + std::max<int64_t>(0, gaps.bottom)) * TOTALSCALE * GAP_MULTIPLIER;

        return box;
    }

    float pointDistanceSqToBox(const Vector2D& point, const CBox& box) {
        const float dx = point.x < box.x ? box.x - point.x : point.x > box.x + box.width ? point.x - (box.x + box.width) : 0.F;
        const float dy = point.y < box.y ? box.y - point.y : point.y > box.y + box.height ? point.y - (box.y + box.height) : 0.F;

        return dx * dx + dy * dy;
    }

    bool boxIntersectsViewport(const CBox& box, const SViewport& viewport) {
        if (box.width <= 0 || box.height <= 0)
            return false;

        const auto RENDERSIZE = viewport.renderSize();

        return box.x < RENDERSIZE.x && box.x + box.width > 0 && box.y < RENDERSIZE.y && box.y + box.height > 0;
    }

    bool boxFullyVisibleInViewport(const CBox& box, const SViewport& viewport) {
        if (box.empty())
            return false;

        const CBox VIEWPORT = {{}, viewport.renderSize()};
        return box.x >= VIEWPORT.x && box.y >= VIEWPORT.y && box.x + box.width <= VIEWPORT.x + VIEWPORT.width && box.y + box.height <= VIEWPORT.y + VIEWPORT.height;
    }

    double boxIntersectionArea(const CBox& a, const CBox& b) {
        const auto INTERSECTION = a.intersection(b);
        return std::max(0.0, INTERSECTION.width) * std::max(0.0, INTERSECTION.height);
    }

    double boxArea(const CBox& box) {
        return std::max(0.0, box.width) * std::max(0.0, box.height);
    }

    double boxCenterDistanceSquared(const CBox& a, const CBox& b) {
        const auto ACENTER = a.middle();
        const auto BCENTER = b.middle();
        const auto DX      = ACENTER.x - BCENTER.x;
        const auto DY      = ACENTER.y - BCENTER.y;

        return DX * DX + DY * DY;
    }

    bool boxesEqual(const CBox& a, const CBox& b) {
        return std::abs(a.x - b.x) < 0.5 && std::abs(a.y - b.y) < 0.5 && std::abs(a.width - b.width) < 0.5 && std::abs(a.height - b.height) < 0.5;
    }

    bool boxesDiffer(const CBox& a, const CBox& b) {
        return std::abs(a.x - b.x) > 0.5 || std::abs(a.y - b.y) > 0.5 || std::abs(a.w - b.w) > 0.5 || std::abs(a.h - b.h) > 0.5;
    }

    CBox centerBoxIn(CBox box, const CBox& container) {
        if (container.width <= 0 || container.height <= 0)
            return box;

        box.x = container.x + std::max(0.F, static_cast<float>(container.width - box.width)) / 2.F;
        box.y = container.y + std::max(0.F, static_cast<float>(container.height - box.height)) / 2.F;

        return box;
    }

    CBox clampBoxTo(CBox box, const CBox& container, float margin) {
        if (container.width <= 0 || container.height <= 0)
            return box;

        const float CLAMPMARGIN = std::max(0.F, margin);
        const float MINX        = container.x + CLAMPMARGIN;
        const float MINY        = container.y + CLAMPMARGIN;
        const float MAXX        = container.x + std::max(0.F, static_cast<float>(container.width - box.width - 2.F * CLAMPMARGIN)) + CLAMPMARGIN;
        const float MAXY        = container.y + std::max(0.F, static_cast<float>(container.height - box.height - 2.F * CLAMPMARGIN)) + CLAMPMARGIN;

        box.x = std::clamp(static_cast<float>(box.x), MINX, std::max(MINX, MAXX));
        box.y = std::clamp(static_cast<float>(box.y), MINY, std::max(MINY, MAXY));

        return box;
    }

    CBox resizedBoxFromCorner(const CBox& originalBox, const Vector2D& delta, uint8_t corner, const Vector2D& minSizePx, const std::optional<Vector2D>& maxSizePx) {
        float left   = originalBox.x;
        float top    = originalBox.y;
        float right  = originalBox.x + originalBox.width;
        float bottom = originalBox.y + originalBox.height;

        switch (corner) {
            case CORNER_TOPLEFT:
                left += delta.x;
                top += delta.y;
                break;
            case CORNER_TOPRIGHT:
                right += delta.x;
                top += delta.y;
                break;
            case CORNER_BOTTOMLEFT:
                left += delta.x;
                bottom += delta.y;
                break;
            case CORNER_BOTTOMRIGHT:
            default:
                right += delta.x;
                bottom += delta.y;
                break;
        }

        float width  = right - left;
        float height = bottom - top;

        const float minWidth  = static_cast<float>(std::max(1.0, minSizePx.x));
        const float minHeight = static_cast<float>(std::max(1.0, minSizePx.y));
        const float maxWidth  = maxSizePx ? static_cast<float>(std::max(static_cast<double>(minWidth), maxSizePx->x)) : std::numeric_limits<float>::max();
        const float maxHeight = maxSizePx ? static_cast<float>(std::max(static_cast<double>(minHeight), maxSizePx->y)) : std::numeric_limits<float>::max();

        width  = std::clamp(width, minWidth, maxWidth);
        height = std::clamp(height, minHeight, maxHeight);

        switch (corner) {
            case CORNER_TOPLEFT:
                left = right - width;
                top  = bottom - height;
                break;
            case CORNER_TOPRIGHT:
                right = left + width;
                top   = bottom - height;
                break;
            case CORNER_BOTTOMLEFT:
                left   = right - width;
                bottom = top + height;
                break;
            case CORNER_BOTTOMRIGHT:
            default:
                right  = left + width;
                bottom = top + height;
                break;
        }

        return CBox{{left, top}, {right - left, bottom - top}};
    }

    CBox clampResizedBoxTo(const CBox& box, const CBox& container, uint8_t corner, float marginPx) {
        if (container.width <= 0 || container.height <= 0)
            return box;

        const float minX = container.x + std::max(0.F, marginPx);
        const float minY = container.y + std::max(0.F, marginPx);
        const float maxX = container.x + container.width - std::max(0.F, marginPx);
        const float maxY = container.y + container.height - std::max(0.F, marginPx);

        float left   = box.x;
        float top    = box.y;
        float right  = box.x + box.width;
        float bottom = box.y + box.height;

        switch (corner) {
            case CORNER_TOPLEFT:
                left = std::max(left, minX);
                top  = std::max(top, minY);
                break;
            case CORNER_TOPRIGHT:
                right = std::min(right, maxX);
                top   = std::max(top, minY);
                break;
            case CORNER_BOTTOMLEFT:
                left   = std::max(left, minX);
                bottom = std::min(bottom, maxY);
                break;
            case CORNER_BOTTOMRIGHT:
            default:
                right  = std::min(right, maxX);
                bottom = std::min(bottom, maxY);
                break;
        }

        return CBox{{left, top}, {right - left, bottom - top}};
    }

    float workspaceRenderedPitch(const SViewport& viewport, float scale, EAxis axis, float workspaceGap) {
        return (axisSize(viewport.size, axis) * scale + workspaceGap) * viewport.scale;
    }

    float workspaceLogicalPitch(const SViewport& viewport, float scale, EAxis axis, float workspaceGap) {
        const auto safeScale = std::max(scale, 0.01F);
        return axisSize(viewport.size, axis) + workspaceGap / safeScale;
    }

    float spanOverlap(double aStart, double aSize, double bStart, double bSize) {
        const double overlap = std::min(aStart + aSize, bStart + bSize) - std::max(aStart, bStart);
        return std::max(0.F, static_cast<float>(overlap));
    }

    float gridProgress(float scale, float targetScale) {
        if (std::abs(1.F - targetScale) < 0.001F)
            return 1.F;

        return std::clamp((1.F - scale) / (1.F - targetScale), 0.F, 1.F);
    }

    CBox lerpBox(const CBox& from, const CBox& to, float t) {
        return CBox{
            from.x + (to.x - from.x) * t,
            from.y + (to.y - from.y) * t,
            from.w + (to.w - from.w) * t,
            from.h + (to.h - from.h) * t,
        };
    }

    float hyprlerp(float from, float to, float perc) {
        return (to - from) * perc + from;
    }

    Vector2D hyprlerp(const Vector2D& from, const Vector2D& to, float perc) {
        return Vector2D{hyprlerp(static_cast<float>(from.x), static_cast<float>(to.x), perc), hyprlerp(static_cast<float>(from.y), static_cast<float>(to.y), perc)};
    }

} // namespace ScrollOverview::Geometry
