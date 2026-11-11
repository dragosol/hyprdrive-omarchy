#include "IdentityDebug.hpp"

#include "IdentityLive.hpp"

namespace Hyprdrive::AppIdentity {
    std::string debugReport() {
        return liveReport();
    }
}
