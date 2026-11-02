#include "Compat.hpp"
#include "DebugPaths.hpp"

#include "Config.hpp"
#include "globals.hpp"

#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/config/shared/Types.hpp>
#include <hyprland/src/config/shared/complex/ComplexDataTypes.hpp>

#include <cstdio>
#include <filesystem>
#include <vector>

namespace {

// DebugPaths.hpp: owner-only, in the private runtime directory.
constexpr const char* COMPAT_LOG = "hyprdrive-compat.log";

struct SPair {
    std::string           modern;
    std::string           legacy;
    const std::type_info* underlying = nullptr;
};

std::vector<SPair>& pairs() {
    static std::vector<SPair> PAIRS;
    return PAIRS;
}

bool g_warned = false;

void appendLog(const std::string& line) {
    if (FILE* const F = ScrollOverview::DebugPaths::openLog(COMPAT_LOG)) {
        std::fputs(line.c_str(), F);
        std::fputc('\n', F);
        std::fclose(F);
    }
}

// The live storage behind a config value.
//
// SConfigOptionReply::dataptr is `<T>* const*` -- the slot holding the pointer
// to the value -- which is exactly what CConfigValue binds to and double-
// dereferences. Used only for STRING below: CConfigValue<std::string>::ptr()
// is deliberately unimplemented upstream (it RASSERTs), so the string case
// cannot go through the same accessor as every other type.
template <typename T>
T* replyStorage(const std::string& name) {
    const auto REPLY = ::Config::mgr()->getConfigValue(name);
    return REPLY.dataptr ? *rc<T* const*>(REPLY.dataptr) : nullptr;
}

// Each arm uses the accessor that is known-correct for ITS type rather than one
// generic cast, because these types are not interchangeable here: a bool key
// stores one byte and an int key stores eight, and reading either through the
// other's pointer is how a config copy becomes memory corruption.
template <typename T>
void assignVia(const SPair& pair) {
    auto* FROM = ScrollOverview::Config::getValuePtr<T>(pair.legacy);
    auto* TO   = ScrollOverview::Config::getValuePtr<T>(pair.modern);
    if (FROM && TO)
        *TO = *FROM;
}

void copyValue(const SPair& pair) {
    const auto* TYPE = pair.underlying;
    if (!TYPE)
        return;

    if (*TYPE == typeid(::Config::BOOL))
        assignVia<::Config::BOOL>(pair);
    else if (*TYPE == typeid(::Config::INTEGER))
        assignVia<::Config::INTEGER>(pair);
    else if (*TYPE == typeid(::Config::FLOAT))
        assignVia<::Config::FLOAT>(pair);
    else if (*TYPE == typeid(::Config::STRING)) {
        auto* FROM = replyStorage<::Config::STRING>(pair.legacy);
        auto* TO   = replyStorage<::Config::STRING>(pair.modern);
        if (FROM && TO)
            *TO = *FROM;
    } else {
        // The complex types -- here, the two gradients. CConfigValue's own
        // IComplexConfigValue specialisation knows both storage paths, the
        // native one and hyprlang's CUSTOMTYPE, so the copy borrows it instead
        // of guessing which one a gradient landed on.
        auto& FROM = ScrollOverview::Config::valueRef<::Config::IComplexConfigValue>(pair.legacy);
        auto& TO   = ScrollOverview::Config::valueRef<::Config::IComplexConfigValue>(pair.modern);
        if (!FROM.good() || !TO.good())
            return;
        auto* F = sc<::Config::CGradientValueData*>(FROM.ptr());
        auto* T = sc<::Config::CGradientValueData*>(TO.ptr());
        if (F && T)
            *T = *F;
    }
}

std::filesystem::path stateDir(const char* name) {
    if (const char* STATE = std::getenv("XDG_STATE_HOME"); STATE && *STATE)
        return std::filesystem::path{STATE} / name;

    if (const char* HOME = std::getenv("HOME"); HOME && *HOME)
        return std::filesystem::path{HOME} / ".local" / "state" / name;

    return {};
}

}

namespace ScrollOverview::Compat {

void openLog() {
    if (FILE* const F = ScrollOverview::DebugPaths::openLog(COMPAT_LOG, false)) {
        std::fprintf(F, "# %s compat log -- every use of a pre-rename name, and the one warning\n", NAME);
        std::fclose(F);
    }
}

void noteLegacyUse(const ELegacy kind, const std::string& name) {
    const char* WHAT = kind == ELegacy::KEY        ? "key" :
                       kind == ELegacy::DISPATCHER ? "dispatcher" :
                                                     "lua";

    appendLog(std::string{"use "} + WHAT + " " + name);

    if (g_warned)
        return;

    g_warned = true;

    const std::string WARN = std::string{"WARN "} + name + " uses the pre-rename name '" + LEGACY_NAME + "'. It still works, and will for one release; rename it to '" +
        NAME + "'. Printed once per session.";

    appendLog(WARN);
    std::fprintf(stderr, "[%s] DEPRECATED: %s\n", NAME, WARN.c_str() + 5);
    std::fflush(stderr);
}

void pairKey(const char* modern, const char* legacy, const std::type_info* underlying) {
    if (!modern || !legacy)
        return;
    pairs().emplace_back(SPair{.modern = modern, .legacy = legacy, .underlying = underlying});
}

void adoptLegacyValues() {
    for (const auto& PAIR : pairs()) {
        if (!::Config::mgr()->getConfigValue(PAIR.legacy).setByUser)
            continue;

        noteLegacyUse(ELegacy::KEY, PAIR.legacy);

        // Both names set: the new one wins, silently. Copying the old over it
        // would make the deprecated spelling the authority, which is backwards.
        if (::Config::mgr()->getConfigValue(PAIR.modern).setByUser)
            continue;

        copyValue(PAIR);
    }
}

void adoptLegacyFavorites() {
    const auto MODERN = stateDir(NAME) / "favorites";
    const auto LEGACY = stateDir(LEGACY_NAME) / "favorites";
    if (MODERN.empty() || LEGACY.empty())
        return;

    std::error_code ec;
    if (std::filesystem::exists(MODERN, ec) || !std::filesystem::exists(LEGACY, ec))
        return;

    std::filesystem::create_directories(MODERN.parent_path(), ec);
    std::filesystem::copy_file(LEGACY, MODERN, ec);
    if (!ec)
        noteLegacyUse(ELegacy::KEY, LEGACY.string());
}

}
