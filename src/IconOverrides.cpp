#include "IconOverrides.hpp"
#include "Compat.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace ScrollOverview::IconOverrides {
    namespace {
        // Twice a second, like the favourites store: on the layout path, and the
        // file changes when someone clicks in the settings panel.
        constexpr auto POLL = std::chrono::milliseconds(500);

        struct SStore {
            std::unordered_map<std::string, std::string> map;
            std::filesystem::file_time_type              stamp{};
            bool                                         present = false;
            std::chrono::steady_clock::time_point        checkedAt{};
            uint64_t                                     generation = 1;
        };

        SStore& store() {
            static SStore s;
            return s;
        }

        std::filesystem::path path() {
            if (const char* STATE = std::getenv("XDG_STATE_HOME"); STATE && *STATE)
                return std::filesystem::path{STATE} / ScrollOverview::Compat::NAME / "icons";
            if (const char* HOME = std::getenv("HOME"); HOME && *HOME)
                return std::filesystem::path{HOME} / ".local" / "state" / ScrollOverview::Compat::NAME / "icons";
            return {};
        }

        void refresh() {
            auto&      s   = store();
            const auto NOW = std::chrono::steady_clock::now();
            if (s.checkedAt != std::chrono::steady_clock::time_point{} && NOW - s.checkedAt < POLL)
                return;
            s.checkedAt = NOW;

            std::error_code ec;
            const auto      P = path();
            if (P.empty() || !std::filesystem::exists(P, ec)) {
                // Deleted (or never written): no overrides. Only a change bumps.
                if (s.present || !s.map.empty()) {
                    s.map.clear();
                    s.present = false;
                    ++s.generation;
                }
                return;
            }
            const auto WRITTEN = std::filesystem::last_write_time(P, ec);
            if (ec || (s.present && WRITTEN == s.stamp))
                return;

            std::ifstream     file(P);
            std::stringstream buf;
            buf << file.rdbuf();
            s.map     = parse(buf.str());
            s.stamp   = WRITTEN;
            s.present = true;
            ++s.generation;
        }
    }

    const std::string* forId(const std::string& desktopId) {
        if (desktopId.empty())
            return nullptr;
        refresh();
        const auto& M  = store().map;
        const auto  IT = M.find(desktopId);
        return IT == M.end() ? nullptr : &IT->second;
    }

    uint64_t generation() {
        refresh();
        return store().generation;
    }
}
