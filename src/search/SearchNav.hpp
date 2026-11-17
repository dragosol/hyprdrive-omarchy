#pragma once

// The keyboard selection's zone, and the search intent (SearchIntent.hpp).
// Cut from SearchSession.hpp by pure code motion.

#include <cstdint>

// X61. Which ZONE the keyboard selection is in. The overview has three surfaces
// the user can land on and they keep their selection in three different places
// -- the strip in viewportCurrentWorkspace, the results in nav.selection, the
// tiles in closeOnWindow -- so the arrows need one explicit answer to "where am
// I" rather than inferring it from whichever of the three happens to be set.
// Return reads it to know what activating means, and the tile border reads it so
// a selection that has moved to the results does not leave a second highlight
// behind on the grid.
enum class EOverviewNavZone : uint8_t {
    Tile = 0,
    Result,
    Desktop,
    Search,
};

#include "SearchIntent.hpp"
