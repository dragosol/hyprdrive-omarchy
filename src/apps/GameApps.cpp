#include "GameApps.hpp"
#include "AppSearch.hpp"
#include "DebugPaths.hpp"
#include "WinExe.hpp"
#include "globals.hpp"

#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>

#include <dlfcn.h>
#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

namespace Hyprdrive::GameApps {
    namespace {
        std::string lower(std::string s) {
            std::ranges::transform(s, s.begin(), [](unsigned char c) { return std::tolower(c); });
            return s;
        }
        std::string home() {
            const char* H = std::getenv("HOME");
            return H && *H ? H : "";
        }
        std::string dataHome() {
            const char* X = std::getenv("XDG_DATA_HOME");
            return X && *X ? X : home() + "/.local/share";
        }
        std::string stateHome() {
            const char* X = std::getenv("XDG_STATE_HOME");
            return X && *X ? X : home() + "/.local/state";
        }
        void trace(const std::string& s) {
            if (!DebugPaths::flagOn("hyprdrive-mosaic.on"))
                return;
            if (FILE* const F = DebugPaths::openLog("hyprdrive-mosaic.log")) {
                std::fprintf(F, "gameapps %s\n", s.c_str());
                std::fclose(F);
            }
        }

        // ---- Lutris's library -------------------------------------------------
        // pga.db through the system's libsqlite3, opened at run time: no build
        // dependency, and a system without it simply has no Lutris games here.
        struct SSqlite {
            void* lib = nullptr;
            int (*open)(const char*, void**, int, const char*)                 = nullptr;
            int (*prepare)(void*, const char*, int, void**, const char**)       = nullptr;
            int (*step)(void*)                                                  = nullptr;
            const unsigned char* (*text)(void*, int)                            = nullptr;
            int (*integer)(void*, int)                                          = nullptr;
            int (*finalize)(void*)                                              = nullptr;
            int (*close)(void*)                                                 = nullptr;
            bool ok() const {
                return open && prepare && step && text && integer && finalize && close;
            }
        };
        SSqlite& sqlite() {
            static SSqlite S = [] {
                SSqlite s;
                for (const char* NAME : {"libsqlite3.so.0", "libsqlite3.so"})
                    if ((s.lib = dlopen(NAME, RTLD_NOW | RTLD_LOCAL)))
                        break;
                if (!s.lib)
                    return s;
                s.open     = rc<decltype(s.open)>(dlsym(s.lib, "sqlite3_open_v2"));
                s.prepare  = rc<decltype(s.prepare)>(dlsym(s.lib, "sqlite3_prepare_v2"));
                s.step     = rc<decltype(s.step)>(dlsym(s.lib, "sqlite3_step"));
                s.text     = rc<decltype(s.text)>(dlsym(s.lib, "sqlite3_column_text"));
                s.integer  = rc<decltype(s.integer)>(dlsym(s.lib, "sqlite3_column_int"));
                s.finalize = rc<decltype(s.finalize)>(dlsym(s.lib, "sqlite3_finalize"));
                s.close    = rc<decltype(s.close)>(dlsym(s.lib, "sqlite3_close"));
                return s;
            }();
            return S;
        }

        struct SGame {
            int         id = 0;
            std::string slug, name, runner;
            std::string exe;    // the program Lutris starts, absolute ("" if unknown)
            std::string folder; // where it lives: a window from under here IS this game
            std::string appId;  // net.lutris.<slug>-<id>
            std::string icon;
            bool        flatpak = false;
        };
        struct SLibrary {
            std::vector<SGame> games;
            std::string        db;
            int64_t            mtime = -1;
        };
        std::vector<SLibrary> g_libs;

        int64_t mtimeOf(const std::string& p) {
            struct stat st{};
            return ::stat(p.c_str(), &st) == 0 ? sc<int64_t>(st.st_mtim.tv_sec) * 1000000000LL + st.st_mtim.tv_nsec : -1;
        }

        // `game:` section keys of a Lutris game yml: exe, prefix, working_dir.
        std::unordered_map<std::string, std::string> gameSection(const std::string& yml) {
            std::unordered_map<std::string, std::string> out;
            std::ifstream                                f(yml);
            bool                                         in = false;
            for (std::string line; std::getline(f, line);) {
                if (line.empty())
                    continue;
                if (line[0] != ' ') {
                    in = line.rfind("game:", 0) == 0;
                    continue;
                }
                // Two-space keys only (deeper ones are other things' options).
                if (!in || line.size() < 3 || line[1] != ' ' || line[2] == ' ')
                    continue;
                const auto COLON = line.find(':');
                if (COLON == std::string::npos)
                    continue;
                const std::string KEY = line.substr(2, COLON - 2);
                std::string       v   = line.substr(COLON + 1);
                v.erase(0, v.find_first_not_of(' '));
                if (v.size() >= 2 && (v.front() == '\'' || v.front() == '"') && v.back() == v.front())
                    v = v.substr(1, v.size() - 2);
                if (!out.contains(KEY))
                    out[KEY] = v;
            }
            return out;
        }

        std::string findIcon(const std::string& dataDir, const std::string& slug) {
            // Lutris writes its icons into hicolor beside its data dir (the
            // Flatpak keeps them in its sandbox, which no icon theme indexes).
            const std::string NAME = "lutris_" + slug + ".png";
            for (const std::string& ROOT : {dataDir + "/../icons/hicolor", dataHome() + "/icons/hicolor"})
                for (const char* SIZE : {"128x128", "256x256", "64x64", "48x48"}) {
                    std::error_code ec;
                    const auto      P = fs::path(ROOT) / SIZE / "apps" / NAME;
                    if (fs::exists(P, ec))
                        return fs::weakly_canonical(P, ec).string();
                }
            return "lutris_" + slug;
        }

        void loadLibrary(SLibrary& L, const std::string& dataDir, bool flatpak) {
            auto& Q = sqlite();
            L.games.clear();
            if (!Q.ok())
                return;
            void* db = nullptr;
            if (Q.open(L.db.c_str(), &db, 0x00000001 /* SQLITE_OPEN_READONLY */, nullptr) != 0 || !db) {
                if (db)
                    Q.close(db);
                return;
            }
            void* st = nullptr;
            if (Q.prepare(db, "SELECT id, slug, name, runner, configpath, directory FROM games WHERE installed = 1", -1, &st, nullptr) == 0 && st) {
                while (Q.step(st) == 100 /* SQLITE_ROW */) {
                    const auto TXT = [&](int c) {
                        const auto* T = Q.text(st, c);
                        return T ? std::string{rc<const char*>(T)} : std::string{};
                    };
                    SGame g;
                    g.id      = Q.integer(st, 0);
                    g.slug    = TXT(1);
                    g.name    = TXT(2);
                    g.runner  = TXT(3);
                    g.flatpak = flatpak;
                    if (g.slug.empty() || g.name.empty())
                        continue;
                    const auto CFG    = gameSection(dataDir + "/games/" + TXT(4) + ".yml");
                    const auto PREFIX = CFG.contains("prefix") ? CFG.at("prefix") : std::string{};
                    const auto EXE    = CFG.contains("exe") ? CFG.at("exe") : std::string{};
                    if (!EXE.empty()) {
                        fs::path p = EXE;
                        if (p.is_relative())
                            p = fs::path(!PREFIX.empty() ? PREFIX : TXT(5)) / p;
                        g.exe = p.lexically_normal().string();
                    }
                    g.folder = !g.exe.empty() ? fs::path(g.exe).parent_path().string() : std::string{};
                    g.appId  = "net.lutris." + g.slug + "-" + std::to_string(g.id);
                    g.icon   = findIcon(dataDir, g.slug);
                    // No icon from Lutris: the game's own, out of its .exe (once,
                    // kept), else Lutris's cover art.
                    if (!g.icon.starts_with('/')) {
                        std::error_code ec;
                        const auto      DIR = dataHome() + "/hyprdrive/icons";
                        const auto      PNG = DIR + "/lutris-" + g.slug + ".png";
                        if (fs::exists(PNG, ec))
                            g.icon = PNG;
                        else if (lower(g.exe).ends_with(".exe") && fs::exists(g.exe, ec)) {
                            fs::create_directories(DIR, ec);
                            if (const auto PE = WinExe::read(g.exe); PE.ok && WinExe::icon(PE, PNG))
                                g.icon = PNG;
                        }
                        if (!g.icon.starts_with('/'))
                            for (const char* EXT : {".jpg", ".png"})
                                if (const auto C = dataDir + "/coverart/" + g.slug + EXT; fs::exists(C, ec)) {
                                    g.icon = C;
                                    break;
                                }
                    }
                    L.games.push_back(std::move(g));
                }
                Q.finalize(st);
            }
            Q.close(db);
        }

        // The libraries, re-read when Lutris changed them.
        const std::vector<SLibrary>& libraries() {
            if (g_libs.empty()) {
                g_libs.push_back({.db = dataHome() + "/lutris/pga.db"});
                g_libs.push_back({.db = home() + "/.var/app/net.lutris.Lutris/data/lutris/pga.db"});
            }
            for (size_t i = 0; i < g_libs.size(); ++i) {
                auto&         L = g_libs[i];
                const int64_t M = mtimeOf(L.db);
                if (M == L.mtime)
                    continue;
                L.mtime = M;
                if (M < 0)
                    L.games.clear();
                else {
                    loadLibrary(L, fs::path(L.db).parent_path().string(), i == 1);
                    for (const auto& G : L.games)
                        trace(std::format("lutris game {} {} exe={} icon={}", G.appId, G.name, G.exe, G.icon));
                }
            }
            return g_libs;
        }

        const SGame* gameNamed(const std::string& name) {
            for (const auto& L : libraries())
                for (const auto& G : L.games)
                    if (G.name == name)
                        return &G;
            return nullptr;
        }

        // ---- the windows ------------------------------------------------------
        std::unordered_map<std::string, std::string> readEnv(pid_t pid) {
            std::unordered_map<std::string, std::string> env;
            std::ifstream                                f(std::format("/proc/{}/environ", pid), std::ios::binary);
            for (std::string kv; std::getline(f, kv, '\0');)
                if (const auto EQ = kv.find('='); EQ != std::string::npos)
                    env.emplace(kv.substr(0, EQ), kv.substr(EQ + 1));
            return env;
        }
        std::string argv0(pid_t pid) {
            std::ifstream f(std::format("/proc/{}/cmdline", pid), std::ios::binary);
            std::string   a;
            std::getline(f, a, '\0');
            return a;
        }
        // A Wine program's own path, from its argv[0] ("C:\\...\\Game.exe").
        std::string unixExe(const std::string& winPath, const std::string& prefix) {
            if (winPath.size() < 3 || winPath[1] != ':' || prefix.empty())
                return winPath.starts_with('/') ? winPath : "";
            std::string rest = winPath.substr(2);
            std::ranges::replace(rest, '\\', '/');
            std::error_code ec;
            const auto      P = fs::path(prefix) / "dosdevices" / (std::string(1, sc<char>(std::tolower(sc<unsigned char>(winPath[0])))) + ":");
            const auto      R = fs::weakly_canonical(P, ec).string() + rest;
            return fs::exists(R, ec) ? R : "";
        }
        bool under(const std::string& path, const std::string& folder) {
            if (folder.empty() || path.size() <= folder.size())
                return false;
            return lower(path).rfind(lower(folder) + "/", 0) == 0;
        }

        // Window classes that are a Lutris game's: class -> app id.
        std::unordered_map<std::string, std::string> g_alias;
        // Wine programs of their own: class -> entry.
        struct SWineApp {
            std::string cls, name, icon, exec;
        };
        std::vector<SWineApp> g_wine;
        bool                  g_wineLoaded = false;
        std::string           wineStore() {
            return stateHome() + "/hyprdrive/wine-apps.tsv";
        }
        std::string esc(const std::string& s) {
            std::string o;
            for (char c : s)
                o += c == '\t' ? std::string{"\\t"} : c == '\n' ? std::string{"\\n"} : c == '\\' ? std::string{"\\\\"} : std::string(1, c);
            return o;
        }
        std::string unesc(const std::string& s) {
            std::string o;
            for (size_t i = 0; i < s.size(); ++i) {
                if (s[i] == '\\' && i + 1 < s.size()) {
                    const char N = s[++i];
                    o += N == 't' ? '\t' : N == 'n' ? '\n' : N;
                } else
                    o += s[i];
            }
            return o;
        }
        void loadWine() {
            if (g_wineLoaded)
                return;
            g_wineLoaded = true;
            std::ifstream f(wineStore());
            for (std::string line; std::getline(f, line);) {
                std::vector<std::string> col;
                std::stringstream        ss(line);
                for (std::string c; std::getline(ss, c, '\t');)
                    col.push_back(unesc(c));
                if (col.size() == 4 && !col[0].empty())
                    g_wine.push_back({col[0], col[1], col[2], col[3]});
            }
        }
        void saveWine() {
            std::error_code ec;
            fs::create_directories(fs::path(wineStore()).parent_path(), ec);
            const auto    TMP = wineStore() + ".tmp";
            std::ofstream f(TMP, std::ios::trunc);
            for (const auto& W : g_wine)
                f << esc(W.cls) << '\t' << esc(W.name) << '\t' << esc(W.icon) << '\t' << esc(W.exec) << '\n';
            f.close();
            fs::rename(TMP, wineStore(), ec);
        }
        // One Exec= argument, quoted as the desktop entry spec has it.
        std::string q(const std::string& s) {
            std::string o = "\"";
            for (char c : s) {
                if (c == '"' || c == '\\' || c == '$' || c == '`')
                    o += '\\';
                o += c;
            }
            return o + "\"";
        }
        // The Wine settings the program ran with: what launching it again needs.
        bool keepVar(const std::string& k) {
            // Wine's own bookkeeping between its processes, not a setting.
            if (k == "WINELOADERNOEXEC" || k == "WINESERVERSOCKET" || k == "WINEPRELOADRESERVE" || k == "WINELOADER" || k == "WINEDLLDIR0")
                return false;
            for (const char* P : {"WINE", "DXVK_", "VKD3D_", "PROTON_", "STAGING_", "__GL_", "MESA_", "GAMEID", "STORE", "LD_LIBRARY_PATH", "SteamAppId", "SteamGameId"})
                if (k.rfind(P, 0) == 0)
                    return true;
            return false;
        }

        // A window mapped: whose app is it?
        void learn(const PHLWINDOW& w) {
            if (!w || w->m_class.empty())
                return;
            const std::string CLS = lower(w->m_class);
            if (g_alias.contains(CLS) || std::ranges::any_of(g_wine, [&](const SWineApp& a) { return a.cls == CLS; }))
                return;
            const pid_t PID = w->getPID();
            if (PID <= 0)
                return;
            const auto ENV    = readEnv(PID);
            const auto PREFIX = ENV.contains("WINEPREFIX") ? ENV.at("WINEPREFIX") : std::string{};
            const auto A0     = argv0(PID);
            const bool WINE   = !PREFIX.empty() || (A0.size() > 2 && A0[1] == ':');
            std::string exe   = WINE ? unixExe(A0, PREFIX.empty() ? home() + "/.wine" : PREFIX) : std::string{};
            if (exe.empty()) {
                std::error_code ec;
                exe = fs::read_symlink(std::format("/proc/{}/exe", PID), ec).string();
            }
            // A Lutris game, or what one started.
            const SGame* GAME = ENV.contains("GAME_NAME") ? gameNamed(ENV.at("GAME_NAME")) : nullptr;
            if (GAME && (GAME->exe.empty() || under(exe, GAME->folder) || lower(fs::path(exe).filename().string()) == lower(fs::path(GAME->exe).filename().string()))) {
                g_alias[CLS] = GAME->appId;
                trace(std::format("{} is the Lutris game {} ({})", CLS, GAME->name, GAME->appId));
                AppSearch::invalidate();
                return;
            }
            // Not a Wine program, or already an app: nothing to add.
            if (!WINE || exe.empty() || !CLS.ends_with(".exe") || !AppSearch::idForClass(w->m_class).empty())
                return;
            // Wine itself (its loader, its own C:\windows programs: wineboot,
            // winecfg) and installers running out of a temp folder are not apps.
            const std::string LEXE = lower(exe);
            if (!LEXE.ends_with(".exe") || LEXE.find("/drive_c/windows/") != std::string::npos || LEXE.find("/appdata/local/temp/") != std::string::npos ||
                LEXE.find("/temp/") != std::string::npos || LEXE.rfind("/tmp/", 0) == 0)
                return;
            const auto  PE   = WinExe::read(exe);
            std::string name = PE.ok ? WinExe::name(PE) : "";
            if (name.empty())
                // The title when it is just a name (a game's usually is), not
                // "Document - App"; else the exe's own name.
                name = !w->m_title.empty() && w->m_title.size() <= 40 && w->m_title.find(" - ") == std::string::npos ? w->m_title : fs::path(exe).stem().string();
            std::string     icon = "application-x-ms-dos-executable";
            std::error_code ec;
            const auto      ICONDIR = dataHome() + "/hyprdrive/icons";
            fs::create_directories(ICONDIR, ec);
            const auto PNG = ICONDIR + "/wine-" + CLS + ".png";
            if (PE.ok && WinExe::icon(PE, PNG))
                icon = PNG;
            // Launched again as it ran: its Wine (beside the running loader),
            // its prefix and settings, in its own folder.
            std::string wine = "wine";
            if (const auto L = fs::read_symlink(std::format("/proc/{}/exe", PID), ec); !ec) {
                const auto DIR = L.parent_path();
                for (const char* B : {"wine", "wine64"})
                    if (fs::exists(DIR / B, ec)) {
                        wine = (DIR / B).string();
                        break;
                    }
            }
            std::string exec = "env -C " + q(fs::path(exe).parent_path().string());
            if (!PREFIX.empty())
                exec += " " + q("WINEPREFIX=" + PREFIX);
            for (const auto& [K, V] : ENV)
                if (K != "WINEPREFIX" && keepVar(K))
                    exec += " " + q(K + "=" + V);
            exec += " " + q(wine) + " " + q(exe);
            g_wine.push_back({CLS, name, icon, exec});
            saveWine();
            trace(std::format("{} is a Wine program of its own: {} ({})", CLS, name, exe));
            AppSearch::refresh();
        }

        void addEntries(std::vector<AppSearch::SEntry>& inv) {
            for (const auto& L : libraries())
                for (const auto& G : L.games) {
                    if (std::ranges::any_of(inv, [&](const AppSearch::SEntry& e) { return e.id == G.appId; }))
                        continue;
                    AppSearch::SEntry e;
                    e.id       = G.appId;
                    e.name     = G.name;
                    e.generic  = "Game";
                    e.comment  = "Lutris";
                    e.exec     = std::string(G.flatpak ? "flatpak run net.lutris.Lutris" : "env LUTRIS_SKIP_INIT=1 lutris") + " lutris:rungameid/" + std::to_string(G.id);
                    e.icon     = G.icon;
                    e.haystack = lower(e.name + " game lutris " + e.id);
                    inv.push_back(std::move(e));
                }
            loadWine();
            for (const auto& W : g_wine) {
                AppSearch::SEntry e;
                e.id       = "hyprdrive-wine-" + W.cls;
                e.name     = W.name;
                e.comment  = "Wine";
                e.exec     = W.exec;
                e.icon     = W.icon;
                e.wmClass  = W.cls;
                e.haystack = lower(e.name + " wine " + W.cls);
                inv.push_back(std::move(e));
            }
        }

        std::string aliasFor(const std::string& lowerClass) {
            const auto IT = g_alias.find(lowerClass);
            return IT == g_alias.end() ? "" : IT->second;
        }

        CHyprSignalListener g_openLate;
    }

    void init() {
        AppSearch::extraEntries  = &addEntries;
        AppSearch::aliasForClass = &aliasFor;
        AppSearch::refresh();
        g_openLate = Event::bus()->m_events.window.openLate.listen([](PHLWINDOW w) { learn(w); });
        for (const auto& W : Desktop::windowState()->windows())
            if (W && W->m_isMapped)
                learn(W);
    }

    void shutdown() {
        g_openLate.reset();
        AppSearch::extraEntries  = nullptr;
        AppSearch::aliasForClass = nullptr;
    }
}
