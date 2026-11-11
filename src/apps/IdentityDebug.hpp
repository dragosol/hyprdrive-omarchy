#pragma once

#include <string>

// `hyprdrive:debug identity` (docs/DEBUG.md): for every mapped window, the
// WindowFacts Hyprland's in-memory fields give and what AppIdentity's FAST
// tier resolves from them -- key, name, icon, family, confidence, evidence,
// the dock group key it would use and whether it is placeable. No /proc, no
// X round trip, no disk: the published Steam/Lutris indexes as they are
// (none until something built them). For inspecting; nothing consumes it.
namespace Hyprdrive::AppIdentity {
    std::string debugReport();
}
