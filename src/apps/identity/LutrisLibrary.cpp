#include "LutrisLibrary.hpp"

#include <filesystem>
#include <mutex>

namespace Hyprdrive::AppIdentity::LutrisLibrary {
    namespace {
        std::mutex                      g_mutex;
        std::shared_ptr<const Snapshot> g_current;

        std::string normal(const std::string& p) {
            return std::filesystem::path(p).lexically_normal().string();
        }

        std::unordered_map<std::string, std::string> readYml(const Sources& src, const std::string& path) {
            const auto B = src.read(path, Caps::YML);
            return B ? Lutris::parseGameSection(*B) : std::unordered_map<std::string, std::string>{};
        }

        std::string findIcon(const Sources& src, const std::string& dataDir, const std::string& slug) {
            const std::string NAME = "lutris_" + slug + ".png";
            static constexpr const char* SIZES[] = {"128x128", "256x256", "64x64", "48x48"};
            // The user's hicolor: the theme finds it by name.
            for (const char* SIZE : SIZES)
                if (src.exists(src.env().dataHome + "/icons/hicolor/" + SIZE + "/apps/" + NAME))
                    return "lutris_" + slug;
            // The Flatpak keeps its icons beside its data dir, which no theme indexes.
            const std::string SIDE = std::filesystem::path(dataDir).parent_path().string() + "/icons/hicolor/";
            for (const char* SIZE : SIZES)
                if (const std::string P = SIDE + SIZE + "/apps/" + NAME; src.exists(P))
                    return P;
            // GameApps' icon out of the game's .exe, when it made one.
            if (auto have = src.storedIcon("lutris-" + slug); !have.empty())
                return have;
            for (const char* EXT : {".jpg", ".png"})
                if (const std::string C = dataDir + "/coverart/" + slug + EXT; src.exists(C))
                    return C;
            return "lutris_" + slug;
        }

        void load(const Sources& src, const std::string& dataDir, bool flatpak, Snapshot& out) {
            const auto ROWS = src.lutrisGames(dataDir + "/pga.db");
            if (!ROWS)
                return;
            std::vector<std::string> ymlNames; // listed once, only if a configpath is missing
            bool                     listed = false;
            for (const auto& R : *ROWS) {
                if (!R.installed || R.slug.empty() || R.name.empty() || R.id <= 0)
                    continue;
                Game g{.id = R.id, .slug = R.slug, .name = R.name, .runner = R.runner, .directory = R.directory, .flatpak = flatpak};
                std::unordered_map<std::string, std::string> cfg;
                if (!R.configpath.empty())
                    cfg = readYml(src, dataDir + "/games/" + R.configpath + ".yml");
                if (!cfg.empty())
                    g.ymlFrom = "configpath";
                if (!cfg.contains("exe")) {
                    if (!listed) {
                        ymlNames = src.list(dataDir + "/games");
                        listed   = true;
                    }
                    // games/<slug>-<digits>.yml with an exe, when exactly one has.
                    std::unordered_map<std::string, std::string> only;
                    int                                          found = 0;
                    for (const auto& N : ymlNames) {
                        if (!N.starts_with(R.slug + "-") || !N.ends_with(".yml") || !allDigits(std::string_view{N}.substr(R.slug.size() + 1, N.size() - R.slug.size() - 5)))
                            continue;
                        auto c = readYml(src, dataDir + "/games/" + N);
                        if (c.contains("exe")) {
                            only = std::move(c);
                            ++found;
                        }
                    }
                    if (found == 1) {
                        cfg       = std::move(only);
                        g.ymlFrom = "slug";
                    } else if (auto old = readYml(src, src.env().home + "/.config/lutris/games/" + R.configpath + ".yml"); old.contains("exe")) {
                        cfg       = std::move(old);
                        g.ymlFrom = "config";
                    }
                }
                g.prefix = cfg.contains("prefix") ? cfg.at("prefix") : "";
                if (const auto EXE = cfg.contains("exe") ? cfg.at("exe") : std::string{}; !EXE.empty()) {
                    std::filesystem::path p = EXE;
                    if (p.is_relative())
                        p = std::filesystem::path(!g.prefix.empty() ? g.prefix : R.directory) / p;
                    g.exe    = normal(p.string());
                    g.folder = std::filesystem::path(g.exe).parent_path().string();
                }
                g.appId = "net.lutris." + g.slug + "-" + std::to_string(g.id);
                g.icon  = findIcon(src, dataDir, g.slug);
                out.games.push_back(std::move(g));
            }
        }
    }

    const Game* Snapshot::byName(std::string_view name) const {
        for (const auto& G : games)
            if (G.name == name)
                return &G;
        return nullptr;
    }
    const Game* Snapshot::byId(int64_t id) const {
        for (const auto& G : games)
            if (G.id == id)
                return &G;
        return nullptr;
    }
    const Game* Snapshot::byExe(std::string_view exePath) const {
        if (exePath.empty())
            return nullptr;
        const std::string L = lower(exePath), B = lower(baseName(exePath));
        for (const auto& G : games)
            if (!G.exe.empty() && lower(G.exe) == L)
                return &G;
        for (const auto& G : games)
            if (!G.exe.empty() && lower(baseName(G.exe)) == B)
                return &G;
        return nullptr;
    }

    std::shared_ptr<const Snapshot> rebuild(const Sources& src) {
        auto s = std::make_shared<Snapshot>();
        try {
            if (!src.env().dataHome.empty())
                load(src, src.env().dataHome + "/lutris", false, *s);
            if (!src.env().home.empty())
                load(src, src.env().home + "/.var/app/net.lutris.Lutris/data/lutris", true, *s);
        } catch (...) { s = std::make_shared<Snapshot>(); }
        return s;
    }

    std::shared_ptr<const Snapshot> current() {
        std::lock_guard lock(g_mutex);
        return g_current;
    }
    void publish(std::shared_ptr<const Snapshot> s) {
        std::lock_guard lock(g_mutex);
        g_current = std::move(s);
    }
}
