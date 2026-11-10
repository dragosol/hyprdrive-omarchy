#pragma once

#include "Config.hpp"
#include "globals.hpp"

#include <string>

class CDropIndicator {
  public:
    struct SDropAnchor {
        PHLWINDOW   window;
        CBox        box;
        CBox        logicalBox;
        std::string direction;
    };

    struct SRenderParams {
        PHLMONITOR   monitor;
        PHLWORKSPACE workspace;
        CBox         workspaceUsableBox;
        SDropAnchor  anchor;
        float        renderScale           = 1.F;
        bool         workspaceFullyVisible = false;
        bool         floating              = false;
        ScrollOverview::Config::ELayout layout = ScrollOverview::Config::ELayout::VERTICAL;
    };

    static void renderDropIndicator(const SRenderParams& params);
};
