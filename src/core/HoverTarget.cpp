#include "HoverTarget.hpp"
#include "IOverview.hpp"

#include <hyprland/src/desktop/view/Window.hpp>

namespace Hyprdrive::HoverTarget {
    PHLWINDOW window() {
        if (scrollOverviews().empty())
            return {};
        // The overview under the pointer (or the one the keyboard is in), if open.
        const auto OVERVIEW = activeScrollOverview();
        if (!OVERVIEW || !OVERVIEW->pMonitor || openScrollOverviewForMonitor(OVERVIEW->pMonitor.lock()) != OVERVIEW)
            return {};
        const auto TILE = OVERVIEW->actionTile();
        return TILE && TILE->m_isMapped ? TILE : PHLWINDOW{};
    }
}
