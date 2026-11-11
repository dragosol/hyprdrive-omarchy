#pragma once

// The identity worker's Gatherer on the live system (LiveGatherer.cpp): X
// properties and the XRes client pid through its own xcb connection, and the
// process through ProcFacts. Worker thread only.

#include "Engine.hpp"

#include <memory>

namespace Hyprdrive::AppIdentity {
    std::shared_ptr<Gatherer> liveGatherer(const Sources& src);
}
