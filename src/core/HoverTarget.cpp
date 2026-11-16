#include "HoverTarget.hpp"
#include "Dock.hpp"
#include "IOverview.hpp"

#include <hyprland/src/desktop/view/window/Window.hpp>
#include "HyprCompat.hpp"

namespace Hyprdrive::HoverTarget {
    namespace {
        PHLWINDOW overviewTile() {
            if (scrollOverviews().empty())
                return {};
            // The overview under the pointer (or the one the keyboard is in), if open.
            const auto OVERVIEW = activeScrollOverview();
            if (!OVERVIEW || !OVERVIEW->pMonitor || openScrollOverviewForMonitor(OVERVIEW->pMonitor.lock()) != OVERVIEW)
                return {};
            const auto TILE = OVERVIEW->actionTile();
            return TILE && TILE->mapped() ? TILE : PHLWINDOW{};
        }
    }

    PHLWINDOW window() {
        const auto TILE = overviewTile();
        const auto CELL = TILE ? PHLWINDOW{} : Dock::previewUnderPointer();
        switch (source(!!TILE, !!CELL)) {
            case ESource::OVERVIEW_TILE: return TILE;
            case ESource::DOCK_PREVIEW: return CELL;
            case ESource::NONE: break;
        }
        return {};
    }
}
