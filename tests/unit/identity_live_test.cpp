// The live side's pure parts: the dock's stored ids compared and toggled as
// AppIdentity keys (StoreIds.hpp + canonicalKey) on copies of today's store
// files (tests/fixtures/identity/stores/: favourites, empty-desktop-apps,
// fullscreen-apps as the dock writes them, with a Steam shortcut's desktop
// id in them), and ProcFacts' bounded /proc reads on a fake /proc tree (the
// parent chain, the allow-listed environment, the Wine relaunch settings, a
// container's exe through /proc/<pid>/root, an unreadable environ).
#include "AppIdentity.hpp"
#include "DockMenuModel.hpp"
#include "ProcFacts.hpp"
#include "StoreIds.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;
using namespace Hyprdrive::AppIdentity;

static int s_checks = 0, s_failed = 0;
#define CHECK(c)                                                                                                                                                              \
    do {                                                                                                                                                                       \
        ++s_checks;                                                                                                                                                            \
        if (!(c)) {                                                                                                                                                            \
            ++s_failed;                                                                                                                                                        \
            std::cerr << "FAILED line " << __LINE__ << ": " #c "\n";                                                                                                          \
        }                                                                                                                                                                      \
    } while (0)

namespace {
    std::vector<std::string> readIds(const std::string& name) {
        std::ifstream in("tests/fixtures/identity/stores/" + name);
        return Hyprdrive::Dock::MenuModel::parseIdLines(in);
    }
    void put(const fs::path& p, const std::string& bytes) {
        fs::create_directories(p.parent_path());
        std::ofstream(p, std::ios::binary) << bytes;
    }
    std::string nul(std::initializer_list<std::string> parts) {
        std::string s;
        for (const auto& P : parts)
            s += P + '\0';
        return s;
    }
}

int main() {
    // ---- stored ids ----------------------------------------------------------------------
    {
        // The inventory: Steam's desktop shortcut "Overwatch" launches 2357570.
        const std::vector<InventoryEntry> INV{{.id = "Overwatch", .name = "Overwatch", .exec = "steam steam://rungameid/2357570"},
                                              {.id = "net.lutris.battlenet-12", .name = "Battle.net", .exec = "env LUTRIS_SKIP_INIT=1 lutris lutris:rungameid/12"},
                                              {.id = "org.gnome.Nautilus", .name = "Files"}};
        const InventoryView     VIEW{.entries = INV};
        const StoreIds::Canon   CANON = [&](std::string_view s) { return canonicalKey(s, VIEW); };

        const auto FAV = readIds("favorites");
        CHECK(FAV.size() == 7 && FAV.back() == "Overwatch");
        // The running game's group key and identity key both find the stored shortcut id.
        CHECK(StoreIds::contains(FAV, "hd:steam-app-2357570", CANON));
        CHECK(StoreIds::contains(FAV, "steam-app-2357570", CANON));
        CHECK(sameApp("Overwatch", "steam-app-2357570", VIEW) && !sameApp("Overwatch", "steam-app-1", VIEW));
        // Ordinary ids: unchanged, equal only to themselves.
        CHECK(StoreIds::contains(FAV, "org.gnome.Nautilus", CANON) && !StoreIds::contains(FAV, "org.gnome.Nautilus2", CANON));
        CHECK(canonicalKey("net.lutris.battlenet-12", VIEW) == "net.lutris.battlenet-12" && canonicalKey("hyprdrive-wine-wow.exe", VIEW) == "hyprdrive-wine-wow.exe");
        // Nothing rewritten by reading: the file's bytes round-trip.
        CHECK(Hyprdrive::Dock::MenuModel::serializeIds(FAV) == [] {
            std::ifstream in("tests/fixtures/identity/stores/favorites");
            return std::string(std::istreambuf_iterator<char>(in), {});
        }());

        // "Always open in an empty desktop", saved as "Overwatch": the game holds it.
        const auto EMPTY = readIds("empty-desktop-apps");
        CHECK(StoreIds::contains(EMPTY, "hd:steam-app-2357570", CANON));
        // Toggled off from the running game: the alias goes.
        auto off = StoreIds::toggled(EMPTY, "hd:steam-app-2357570", CANON);
        CHECK(off.empty());
        // On again: written as the canonical key, once.
        auto on = StoreIds::toggled(off, "Overwatch", CANON);
        CHECK(on.size() == 1 && on[0] == "steam-app-2357570");
        // A list that holds both spellings loses both on one toggle.
        auto both = StoreIds::toggled({"Overwatch", "org.gnome.Nautilus", "steam-app-2357570"}, "steam-app-2357570", CANON);
        CHECK(both.size() == 1 && both[0] == "org.gnome.Nautilus");
        const auto FULL = readIds("fullscreen-apps");
        CHECK(StoreIds::contains(FULL, "hd:net.lutris.battlenet-12", CANON) && !StoreIds::contains(FULL, "Overwatch", CANON));
        // The favourites toggle keeps the order of the others.
        auto fav = StoreIds::toggled(FAV, "hd:steam-app-2357570", CANON);
        CHECK(fav.size() == 6 && fav[0] == "app.zen_browser.zen" && fav[5] == "net.lutris.battlenet-12");
        CHECK(StoreIds::erase(fav, "", CANON) == 0 && !StoreIds::same("", "", CANON));
        // wine-apps.tsv v1: four columns, the key is hyprdrive-wine-<column 1>.
        std::ifstream tsv("tests/fixtures/identity/stores/wine-apps.tsv");
        std::string   line;
        std::getline(tsv, line);
        CHECK(std::ranges::count(line, '\t') == 3 && line.starts_with("wow.exe\t"));
    }

    // ---- /proc ---------------------------------------------------------------------------
    {
        char tmpl[] = "/tmp/identity_live_test.XXXXXX";
        const fs::path P = mkdtemp(tmpl);
        // 300 (the game) <- 200 (umu-run) <- 100 (reaper SteamLaunch AppId=2357570) <- 1
        put(P / "300/stat", "300 (Game.exe (x)) S 200 300 300 0 -1 4194304 0 0 0 0 0 0 0 0 20 0 1 0 5000 0 0");
        put(P / "300/cmdline", nul({"C:\\Games\\Game\\Game.exe", "-dx12"}));
        put(P / "300/environ", nul({"HOME=/home/user", "WINEPREFIX=/home/user/pfx", "SteamAppId=2357570", "DXVK_HUD=0", "WINESERVERSOCKET=3", "SECRET_TOKEN=abc", "WINEDEBUG=-all"}));
        fs::create_symlink("/no/such/wine64-preloader", P / "300/exe");
        put(P / "300/root/no/such/wine64-preloader", "");
        put(P / "200/stat", "200 (umu-run) S 100 200 200 0 -1 0 0 0 0 0 0 0 0 0 20 0 1 0 4000 0 0");
        put(P / "200/cmdline", nul({"/usr/bin/python3", "/usr/bin/umu-run", "Game.exe"}));
        put(P / "100/stat", "100 (reaper) S 1 100 100 0 -1 0 0 0 0 0 0 0 0 0 20 0 1 0 3000 0 0");
        put(P / "100/cmdline", nul({"reaper", "SteamLaunch", "AppId=2357570", "--", "proton"}));
        const auto F = ProcFacts::read(300, P.string());
        CHECK(F.readable && F.pid == 300 && F.comm == "Game.exe (x)" && F.startTicks == 5000);
        CHECK(F.argv0 == "C:\\Games\\Game\\Game.exe");
        CHECK(F.getenv("WINEPREFIX") == "/home/user/pfx" && F.getenv("SteamAppId") == "2357570" && F.getenv("HOME").empty() && F.getenv("SECRET_TOKEN").empty());
        bool dxvk = false, sock = false, secret = false, debug = false;
        for (const auto& [K, V] : F.launchEnv) {
            dxvk   = dxvk || K == "DXVK_HUD";
            sock   = sock || K == "WINESERVERSOCKET";
            secret = secret || K == "SECRET_TOKEN";
            debug  = debug || K == "WINEDEBUG";
        }
        CHECK(dxvk && debug && !sock && !secret);
        CHECK(F.exe == (P / "300/root/no/such/wine64-preloader").string());
        CHECK(F.chain.size() == 2 && F.chain[0].pid == 200 && F.chain[0].comm == "umu-run" && F.chain[1].comm == "reaper");
        CHECK(F.chain.size() == 2 && F.chain[1].cmdline == "reaper SteamLaunch AppId=2357570 -- proton");
        CHECK(ProcFacts::startOf(300, P.string()) == 5000 && ProcFacts::startOf(999, P.string()) == 0);
        CHECK(!ProcFacts::read(999, P.string()).readable && !ProcFacts::read(0, P.string()).readable);
        // The chain is capped, a loop included.
        put(P / "400/stat", "400 (loop) S 401 0 0 0 -1 0 0 0 0 0 0 0 0 0 20 0 1 0 1 0 0");
        put(P / "401/stat", "401 (loop) S 400 0 0 0 -1 0 0 0 0 0 0 0 0 0 20 0 1 0 1 0 0");
        CHECK(ProcFacts::read(400, P.string()).chain.size() == MAX_HOPS);
        // A cmdline over its cap is read as far as the cap.
        CHECK(ProcFacts::readCapped((P / "300/environ").string(), 8).value_or("").size() == 8);
        // Not a Wine process: no relaunch settings at all.
        put(P / "500/stat", "500 (kitty) S 1 0 0 0 -1 0 0 0 0 0 0 0 0 0 20 0 1 0 1 0 0");
        put(P / "500/environ", nul({"DXVK_HUD=1"}));
        CHECK(ProcFacts::read(500, P.string()).launchEnv.empty());
        std::error_code ec;
        fs::remove_all(P, ec);
    }

    std::cout << "identity_live_test: " << s_checks << " checks, " << s_failed << " failed\n";
    return s_failed ? 1 : 0;
}
