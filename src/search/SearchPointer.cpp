// The search half of the overview's pointer (SearchPointer.hpp). Cut out of
// COverview::onMouseMove and onMouseButton with the branch order kept verbatim.
#include "OverviewParts.hpp"
#include "SearchPointer.hpp"

bool Search::onPointerMove(CSession& S, MONITORID monitor, const Vector2D& point) {
    if (S.pointer.drag && S.pointer.drag->live)
        return false;
    if (S.layoutOn(monitor).pill.containsPoint(point)) {
        if (S.nav.hoverIndex != -1 || S.nav.intent == ESearchIntent::Hovering) {
            S.nav.hoverIndex = -1;
            S.nav.intent     = ESearchIntent::Typing;
            return true;
        }
    } else if (const auto* ROW = searchRowAt(S.layoutOn(monitor), point); ROW) {
        if (S.nav.hoverIndex != ROW->index) {
            S.nav.hoverIndex = ROW->index;
            S.nav.selection  = ROW->index;
            S.nav.zone       = EOverviewNavZone::Result;
            S.nav.intent     = ESearchIntent::Hovering;
            return true;
        }
    } else if (S.nav.hoverIndex != -1) {
        S.nav.hoverIndex = -1;
        if (S.nav.intent == ESearchIntent::Hovering)
            S.nav.intent = S.nav.explicitSelection ? ESearchIntent::Browsing : ESearchIntent::Typing;
        return true;
    }
    return false;
}

bool Search::onPointerDragMove(CSession& S, const Vector2D& point, float dragThresholdSq) {
    if (!(S.pointer.drag && !S.pointer.drag->desktopId.empty()))
        return false;
    if (!S.pointer.drag->live) {
        const float HELDMS = Hyprdrive::Config::realElapsedMs(S.pointer.drag->startedAt);
        if (HELDMS >= G12_HOLD_MS && S.pointer.drag->start.distanceSq(point) > dragThresholdSq)
            S.pointer.drag->live = true;
    }
    return S.pointer.drag->live;
}

std::optional<SResultDrag> Search::takeDrag(CSession& S) {
    if (!S.pointer.drag)
        return std::nullopt;
    const auto DRAG = *S.pointer.drag;
    S.pointer.drag.reset();
    return DRAG;
}

int Search::clickedRow(CSession& S, MONITORID monitor, const Vector2D& point, const SResultDrag& drag) {
    if (const auto* ROW = searchRowAt(S.layoutOn(monitor), point); ROW && ROW->index == drag.index)
        return ROW->index;
    return -1;
}

bool Search::takeUiPress(CSession& S) {
    return std::exchange(S.pointer.uiPress, false);
}

Search::EPress Search::onPointerPress(CSession& S, MONITORID monitor, const Vector2D& point) {
    if (const auto* ROW = searchRowAt(S.layoutOn(monitor), point); ROW) {
        S.nav.selection         = ROW->index;
        S.nav.zone              = EOverviewNavZone::Result;
        S.nav.intent            = ESearchIntent::Hovering;
        S.pointer.drag =
            SResultDrag{.desktopId = ROW->desktopId, .icon = ROW->icon, .start = point, .startedAt = Time::steadyNow(), .index = ROW->index};
        return EPress::Row;
    }
    if (S.layoutOn(monitor).pill.containsPoint(point)) {
        S.query.active = true;
        S.nav.zone = EOverviewNavZone::Search;
        S.nav.selection = -1;
        S.pointer.uiPress = true;
        return EPress::Pill;
    }
    if (S.layoutOn(monitor).containerBox.width > 1.F && S.layoutOn(monitor).containerBox.containsPoint(point)) {
        S.pointer.uiPress = true;
        return EPress::Pane;
    }
    return EPress::None;
}
