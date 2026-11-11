#include "SteamLibrary.hpp"

#include <algorithm>
#include <chrono>
#include <mutex>

namespace Hyprdrive::AppIdentity::SteamLibrary {
    namespace {
        std::mutex                      g_mutex;
        std::shared_ptr<const Snapshot> g_current;

        int64_t steadyNs() {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        bool isSha1Jpg(std::string_view n) {
            return n.size() == 44 && n.ends_with(".jpg") && std::ranges::all_of(n.substr(0, 40), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
        }

        // appmanifest_<digits>.acf exactly: Steam leaves appmanifest_<id>.acf.<n>.tmp behind.
        bool isManifestName(std::string_view n) {
            static constexpr std::string_view PRE = "appmanifest_", SUF = ".acf";
            return n.size() > PRE.size() + SUF.size() && n.starts_with(PRE) && n.ends_with(SUF) && allDigits(n.substr(PRE.size(), n.size() - PRE.size() - SUF.size()));
        }

        void findIcon(const Sources& src, const std::vector<std::string>& steamRoots, App& a) {
            const std::string ID = std::to_string(a.appid);
            const std::string THEMED = "steam_icon_" + ID;
            static constexpr const char* SIZES[] = {"256x256", "128x128", "64x64", "48x48", "32x32", "24x24", "16x16"};
            // 1. Steam's theme icon. In the user's hicolor the theme index finds
            //    it by name; the Flatpak's is inside its sandbox, so a path.
            for (const char* SIZE : SIZES)
                if (src.exists(src.env().dataHome + "/icons/hicolor/" + SIZE + "/apps/" + THEMED + ".png")) {
                    a.icon     = THEMED;
                    a.iconFrom = "theme";
                    return;
                }
            for (const char* SIZE : SIZES)
                if (const std::string P = src.env().home + "/.var/app/com.valvesoftware.Steam/data/icons/hicolor/" + SIZE + "/apps/" + THEMED + ".png"; src.exists(P)) {
                    a.icon     = P;
                    a.iconFrom = "theme";
                    return;
                }
            // 2./3. The library cache's top-level <sha1>.jpg names the .ico.
            for (const auto& ROOT : steamRoots) {
                const std::string CACHE = ROOT + "/appcache/librarycache/" + ID;
                std::string       jpg;
                for (const auto& N : src.list(CACHE, 256))
                    if (isSha1Jpg(N)) {
                        jpg = N;
                        break;
                    }
                if (jpg.empty())
                    continue;
                const std::string HASH = jpg.substr(0, 40);
                const std::string NAME = "steam-" + ID + "-" + HASH.substr(0, 8);
                if (auto have = src.storedIcon(NAME); !have.empty()) {
                    a.icon     = have;
                    a.iconFrom = "ico";
                    return;
                }
                if (const auto ICO = src.read(ROOT + "/steam/games/" + HASH + ".ico", Caps::ICO))
                    if (const auto P = Ico::best(*ICO))
                        if (auto stored = src.storeIcon(NAME, *P); !stored.empty()) {
                            a.icon     = stored;
                            a.iconFrom = "ico";
                            return;
                        }
                a.icon     = CACHE + "/" + jpg;
                a.iconFrom = "jpg";
                return;
            }
            a.icon     = THEMED;
            a.iconFrom = "default";
        }
    }

    const App* Snapshot::app(uint32_t id) const {
        const auto IT = apps.find(id);
        return IT == apps.end() ? nullptr : &IT->second;
    }
    const Shortcut* Snapshot::shortcut(uint32_t id32) const {
        const auto IT = shortcuts.find(id32);
        return IT == shortcuts.end() ? nullptr : &IT->second;
    }
    std::string Snapshot::nameFor(Steam::GameId id) const {
        if (id.kind == Steam::GameId::Kind::App) {
            const App* A = app(id.id);
            return A ? A->name : "";
        }
        const Shortcut* S = shortcut(id.id);
        return S ? S->name : "";
    }
    std::string Snapshot::iconFor(Steam::GameId id) const {
        if (id.kind == Steam::GameId::Kind::App) {
            const App* A = app(id.id);
            return A ? A->icon : "steam_icon_" + std::to_string(id.id);
        }
        const Shortcut* S = shortcut(id.id);
        return S && S->icon.starts_with('/') ? S->icon : "";
    }

    std::vector<std::string> roots(const Env& env) {
        if (env.home.empty())
            return {};
        return {env.home + "/.local/share/Steam", env.home + "/.steam/steam", env.home + "/.var/app/com.valvesoftware.Steam/.local/share/Steam"};
    }

    std::shared_ptr<const Snapshot> rebuild(const Sources& src, const Reachable& reachable) {
        auto s = std::make_shared<Snapshot>();
        try {
            s->builtAtNs = steadyNs();
            for (const auto& ROOT : roots(src.env())) {
                if (!src.exists(ROOT + "/steamapps") && !src.exists(ROOT + "/userdata"))
                    continue;
                s->roots.push_back(ROOT);
                // Libraries: what libraryfolders.vdf lists (it lists the root
                // too, by its real path); the root's own steamapps only when it
                // has none, so the ~/.steam/steam link is not read twice.
                std::vector<std::string> libs;
                if (const auto VDF = src.read(ROOT + "/steamapps/libraryfolders.vdf", Caps::VDF))
                    for (const auto& P : Steam::parseLibraryFolders(*VDF))
                        libs.push_back(P + "/steamapps");
                if (libs.empty())
                    libs.push_back(ROOT + "/steamapps");
                for (const auto& L : libs)
                    if (std::ranges::find(s->libraries, L) == s->libraries.end())
                        s->libraries.push_back(L);
                // Non-Steam shortcuts, every user.
                for (const auto& UID : src.list(ROOT + "/userdata", 64)) {
                    if (!allDigits(UID))
                        continue;
                    const auto VDF = src.read(ROOT + "/userdata/" + UID + "/config/shortcuts.vdf", Caps::SHORTCUTS);
                    if (!VDF)
                        continue;
                    for (auto& e : Steam::parseShortcuts(*VDF))
                        s->shortcuts.try_emplace(e.appid, Shortcut{.id32 = e.appid, .name = e.name, .exe = e.exe, .startDir = e.startDir, .icon = e.icon, .launchOptions = e.launchOptions});
                }
            }
            for (const auto& LIB : s->libraries) {
                if (reachable && !reachable(LIB)) {
                    s->skipped.push_back(LIB);
                    continue;
                }
                for (const auto& N : src.list(LIB)) {
                    if (!isManifestName(N))
                        continue;
                    const auto ACF = src.read(LIB + "/" + N, Caps::VDF);
                    if (!ACF)
                        continue;
                    if (auto m = Steam::parseManifest(*ACF); m && !s->apps.contains(m->appid))
                        s->apps.emplace(m->appid, App{.appid = m->appid, .name = m->name, .installdir = m->installdir, .library = LIB});
                }
            }
            for (auto& [ID, a] : s->apps)
                findIcon(src, s->roots, a);
        } catch (...) {
            // Out of memory, most likely: an empty index, never a throw.
            s = std::make_shared<Snapshot>();
            s->builtAtNs = steadyNs();
        }
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
