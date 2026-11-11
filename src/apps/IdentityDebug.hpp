#pragma once

#include <string>

// `hyprdrive:debug identity` (docs/DEBUG.md): every mapped window's live
// AppIdentity answer (IdentityLive.hpp) -- its facts (the worker's X
// properties and real pid once resolved), key, name, icon, family,
// confidence, resolver, the dock group key, provisional/pending/resolved,
// placeability and the evidence -- and the worker's own state.
namespace Hyprdrive::AppIdentity {
    std::string debugReport();
}
