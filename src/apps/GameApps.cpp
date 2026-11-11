#include "GameApps.hpp"
#include "AppSearch.hpp"
#include "DebugPaths.hpp"
#include "ProcFacts.hpp"
#include "Sources.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace Hyprdrive::GameApps {
    namespace {
        using AppIdentity::Family;

        std::string lower(std::string s) {
            std::ranges::transform(s, s.begin(), [](unsigned char c) { return std::tolower(c); });
            return s;
        }
        void trace(const std::string& s) {
            if (!DebugPaths::flagOn("hyprdrive-mosaic.on"))
                return;
            if (FILE* const F = DebugPaths::openLog("hyprdrive-mosaic.log")) {
                std::fprintf(F, "gameapps %s\n", s.c_str());
                std::fclose(F);
            }
        }

        // ---- the two stores ------------------------------------------------------
        // wine-apps.tsv, v1, exactly as before (cls, name, icon, exec): an
        // older Hyprdrive reads it as it always did. app-identities.tsv beside
        // it, the sidecar: what v1 cannot hold (key, family, prefix, exe path,
        // desktop id) and the Steam games no desktop entry names.
        struct SWineApp {
            std::string cls, name, icon, exec;
        };
        struct SRecord {
            std::string key, family, name, icon, launch, prefix, exePath, desktopId;
        };
        std::mutex            g_mutex; // the lists: the worker appends, the compositor thread reads
        std::vector<SWineApp> g_wine;
        std::vector<SRecord>  g_records;
        bool                  g_loaded = false, g_changed = false;

        std::string stateDir() {
            return AppIdentity::liveSources().env().stateHome + "/hyprdrive";
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
        std::vector<std::vector<std::string>> readTsv(const std::string& path) {
            std::vector<std::vector<std::string>> rows;
            const auto BYTES = AppIdentity::liveSources().read(path, 1u << 20);
            if (!BYTES)
                return rows;
            std::stringstream in(std::string(BYTES->begin(), BYTES->end()));
            for (std::string line; std::getline(in, line) && rows.size() < 4096;) {
                std::vector<std::string> col;
                std::stringstream        ss(line);
                for (std::string c; std::getline(ss, c, '\t');)
                    col.push_back(unesc(c));
                rows.push_back(std::move(col));
            }
            return rows;
        }
        void writeTsv(const std::string& path, const std::vector<std::vector<std::string>>& rows) {
            std::error_code ec;
            fs::create_directories(fs::path(path).parent_path(), ec);
            const auto TMP = path + ".tmp";
            {
                std::ofstream f(TMP, std::ios::trunc);
                for (const auto& R : rows) {
                    for (size_t i = 0; i < R.size(); ++i)
                        f << (i ? "\t" : "") << esc(R[i]);
                    f << '\n';
                }
                if (!f)
                    return;
            }
            fs::rename(TMP, path, ec);
        }
        // Under g_mutex.
        void loadLocked() {
            if (g_loaded)
                return;
            g_loaded = true;
            for (const auto& C : readTsv(stateDir() + "/wine-apps.tsv"))
                if (C.size() == 4 && !C[0].empty())
                    g_wine.push_back({C[0], C[1], C[2], C[3]});
            for (const auto& C : readTsv(stateDir() + "/app-identities.tsv"))
                if (C.size() >= 8 && !C[0].empty())
                    g_records.push_back({C[0], C[1], C[2], C[3], C[4], C[5], C[6], C[7]});
        }
        struct SRows {
            std::vector<std::vector<std::string>> wine, records;
        };
        SRows rowsLocked() {
            std::vector<std::vector<std::string>> w, r;
            for (const auto& W : g_wine)
                w.push_back({W.cls, W.name, W.icon, W.exec});
            for (const auto& R : g_records)
                r.push_back({R.key, R.family, R.name, R.icon, R.launch, R.prefix, R.exePath, R.desktopId});
            return {std::move(w), std::move(r)};
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
        // Launched again as it ran: its Wine (beside the running loader), its
        // prefix and settings, in its own folder.
        std::string wineExec(const AppIdentity::Result& r) {
            std::string     wine = "wine";
            std::error_code ec;
            if (!r.p.exe.empty()) {
                const auto DIR = fs::path(r.p.exe).parent_path();
                for (const char* B : {"wine", "wine64"})
                    if (fs::exists(DIR / B, ec)) {
                        wine = (DIR / B).string();
                        break;
                    }
            }
            std::string exec = "env -C " + q(fs::path(r.id.exePath).parent_path().string());
            if (const auto P = r.p.getenv("WINEPREFIX"); !P.empty())
                exec += " " + q("WINEPREFIX=" + std::string{P});
            for (const auto& [K, V] : r.p.launchEnv)
                exec += " " + q(K + "=" + V);
            return exec + " " + q(wine) + " " + q(r.id.exePath);
        }

        void addEntries(std::vector<AppSearch::SEntry>& inv) {
            const auto HAS = [&](const std::string& id) { return std::ranges::any_of(inv, [&](const AppSearch::SEntry& e) { return e.id == id; }); };
            if (const auto L = AppIdentity::LutrisLibrary::current())
                for (const auto& G : L->games) {
                    if (HAS(G.appId))
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
            std::lock_guard l(g_mutex);
            loadLocked();
            for (const auto& W : g_wine) {
                AppSearch::SEntry e;
                e.id       = "hyprdrive-wine-" + W.cls;
                e.name     = W.name;
                e.comment  = "Wine";
                e.exec     = W.exec;
                e.icon     = W.icon;
                e.wmClass  = W.cls.contains('@') ? "" : W.cls;
                e.haystack = lower(e.name + " wine " + W.cls);
                inv.push_back(std::move(e));
            }
            // Sidecar apps no entry names (a Steam game without a desktop
            // shortcut): an entry under the identity's own key, so it can be
            // pinned, searched and launched.
            for (const auto& R : g_records) {
                if (R.launch.empty() || HAS(R.key) || (!R.desktopId.empty() && HAS(R.desktopId)))
                    continue;
                AppSearch::SEntry e;
                e.id       = R.key;
                e.name     = R.name;
                e.generic  = "Game";
                e.comment  = R.family;
                e.exec     = R.launch;
                e.icon     = R.icon;
                e.haystack = lower(e.name + " game " + R.family + " " + R.key);
                inv.push_back(std::move(e));
            }
        }
    }

    void record(const AppIdentity::Result& r) {
        const auto& I = r.id;
        const bool  WINE  = I.resolver == "WineExe" && I.desktopId.empty() && I.key.starts_with("hyprdrive-wine-") && !I.exePath.empty();
        const bool  STEAM = I.resolver == "SteamApp" && I.desktopId.empty() && (I.family == Family::Steam || I.family == Family::SteamShortcut) && I.displayName != r.w.cls;
        if (!WINE && !STEAM)
            return;
        std::unique_lock l(g_mutex);
        loadLocked();
        if (std::ranges::any_of(g_records, [&](const SRecord& R) { return R.key == I.key; }))
            return;
        const std::string ICON = I.icon.kind == AppIdentity::Icon::Kind::Pixels ? std::string{} : I.icon.value;
        SRecord rec{.key = I.key, .family = std::string{familyName(I.family)}, .name = I.displayName, .icon = ICON, .launch = I.launch, .prefix = I.prefix, .exePath = I.exePath};
        if (WINE) {
            const std::string CLS = I.key.substr(15);
            auto       it  = std::ranges::find_if(g_wine, [&](const SWineApp& a) { return a.cls == CLS; });
            if (it == g_wine.end()) {
                g_wine.push_back({CLS, I.displayName, ICON.empty() ? "application-x-ms-dos-executable" : ICON, wineExec(r)});
                it = g_wine.end() - 1;
            }
            rec.launch = it->exec;
        }
        g_records.push_back(std::move(rec));
        const SRows ROWS = rowsLocked();
        g_changed        = true;
        l.unlock();
        // Written outside the lock: the compositor thread's inventory build
        // never waits on this disk.
        static std::mutex s_write;
        std::lock_guard   w(s_write);
        writeTsv(stateDir() + "/wine-apps.tsv", ROWS.wine);
        writeTsv(stateDir() + "/app-identities.tsv", ROWS.records);
        trace(std::format("remembered {} ({}): {}", I.key, familyName(I.family), I.displayName));
    }

    bool takeChanged() {
        std::lock_guard l(g_mutex);
        return std::exchange(g_changed, false);
    }

    void init() {
        AppSearch::extraEntries = &addEntries;
        AppSearch::refreshExtras();
    }

    void shutdown() {
        AppSearch::extraEntries = nullptr;
    }
}
