#pragma once

#include <cstdint>

enum class ESearchIntent : uint8_t {
    Typing = 0,
    Browsing,
    Hovering,
};

// SPACE is Quick Look only when the user has pointed at a previewable file
// (hover or keyboard), not merely because a result is auto-selected.
inline bool overviewSpacePreviews(ESearchIntent intent, bool hoverCanPreview, bool selectionCanPreview, bool explicitSelection = true) {
    if (intent == ESearchIntent::Hovering)
        return hoverCanPreview;
    if (intent == ESearchIntent::Browsing && explicitSelection)
        return selectionCanPreview;
    return false;
}
