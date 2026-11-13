// AppIdentity::resolve() (src/apps/identity/), table-driven: window and
// process facts against the fixture trees in tests/fixtures/identity/ ->
// the Identity the design (d) specifies -- Steam games by env, class or the
// reaper hop, non-Steam shortcuts in both id forms, umu games that share
// steam_app_default, Battle.net and the Overwatch it starts, launcher.exe in
// two prefixes, a Flatpak PID that names another process, helpers joining
// their session's app, Lutris's configpath/yml mismatch, a native app,
// unreadable /proc, Wine's systray fallback window. Then canonicalKey on the
// store formats in use today, and the fast tier.
#include "fixture_links.hpp"
#include "AppIdentity.hpp"
#include "IconPng.hpp"
#include "Resolvers.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <random>

namespace fs = std::filesystem;
using namespace Hyprdrive::AppIdentity;
using namespace std::string_literals;

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
    fs::path g_tmp;

    struct SFixture {
        std::string                 root;
        std::unique_ptr<FileSources> src;
        std::vector<InventoryEntry> inv;
        Indexes                     ix;
    };

    // A private copy (icons get written), its Sources, its desktop entries
    // (as AppSearch names them: the path under applications/, '/' -> '-').
    SFixture load(const std::string& name) {
        SFixture        f;
        std::error_code ec;
        f.root = (g_tmp / name).string();
        fs::copy("tests/fixtures/identity/" + name, f.root, fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec);
        fixtureLinks(name, f.root);
        f.src = std::make_unique<FileSources>(FileSources::Options{
            .root = f.root, .env = Env::forHome("/home/user"), .writePng = [](const Ico::Picked& p, const std::string& out) { return IconPng::write(p, out); }});
        const fs::path APPS = f.root + "/home/user/.local/share/applications";
        for (auto it = fs::recursive_directory_iterator(APPS, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (it->path().extension() != ".desktop")
                continue;
            std::string id = it->path().lexically_relative(APPS).string();
            std::ranges::replace(id, '/', '-');
            id.resize(id.size() - 8);
            InventoryEntry e{.id = id};
            std::ifstream  in(it->path());
            for (std::string line; std::getline(in, line);) {
                const auto EQ = line.find('=');
                if (EQ == std::string::npos)
                    continue;
                const auto K = line.substr(0, EQ), V = line.substr(EQ + 1);
                if (K == "Name")
                    e.name = V;
                else if (K == "Exec")
                    e.exec = V;
                else if (K == "Icon")
                    e.icon = V;
                else if (K == "StartupWMClass")
                    e.wmClass = V;
            }
            f.inv.push_back(std::move(e));
        }
        std::ranges::sort(f.inv, {}, &InventoryEntry::id);
        f.ix = {.steam = SteamLibrary::rebuild(*f.src), .lutris = LutrisLibrary::rebuild(*f.src)};
        return f;
    }

    WindowFacts win(std::string cls, std::string title = "", bool x11 = true) {
        return {.cls = cls, .initialClass = cls, .title = std::move(title), .x11 = x11, .w = 1280, .h = 720, .pid = 4242};
    }
    ProcessFacts proc(std::string argv0, std::vector<std::pair<std::string, std::string>> env = {}, std::vector<ProcessFacts::Hop> chain = {}) {
        return {.readable = true, .pid = 4242, .startTicks = 100, .comm = "wine64-preloader", .argv0 = std::move(argv0), .exe = "/usr/bin/wine64-preloader", .env = std::move(env),
                .chain = std::move(chain)};
    }
    Identity run(const SFixture& f, const WindowFacts& w, const ProcessFacts& p, std::function<const Identity*(std::string_view)> owner = {}) {
        return resolveWith(w, p, f.ix, InventoryView{.entries = f.inv, .sessionOwner = std::move(owner)}, f.src.get());
    }
    InventoryEntry* entry(SFixture& f, std::string_view id) {
        for (auto& E : f.inv)
            if (E.id == id)
                return &E;
        return nullptr;
    }
    bool has(const std::string& s, std::string_view part) {
        return s.find(part) != std::string::npos;
    }
    void show(const char* what, const Identity& i) {
        if (std::getenv("APP_IDENTITY_VERBOSE"))
            std::cout << what << ": key=" << i.key << " name=" << i.displayName << " icon=" << i.icon.value << " family=" << familyName(i.family) << " conf=" << int(i.confidence)
                      << " by=" << i.resolver << " placeable=" << i.placeable << " | " << i.evidence << "\n";
    }
}

int main() {
    char tmpl[] = "/tmp/app_identity_test.XXXXXX";
    g_tmp       = mkdtemp(tmpl);
    const uint64_t SHORT64 = (uint64_t{3965729953u} << 32) | 0x02000000u;
    const uint64_t CP64    = (uint64_t{4207589967u} << 32) | 0x02000000u;

    // ---- Steam -----------------------------------------------------------------------
    {
        auto S = load("steam-native");
        // Two shortcuts launch 2357570 (Steam kept the old one after a rename):
        // the newer file names the game.
        entry(S, "Overwatch")->mtimeNs   = 200;
        entry(S, "Overwatch 2")->mtimeNs = 100;
        // a) by env (no telling class)
        auto i = run(S, win("overwatch.exe", "Overwatch"), proc("C:\\Program Files\\Overwatch\\Overwatch.exe", {{"SteamAppId", "2357570"}, {"WINEPREFIX", "/home/user/.local/share/Steam/steamapps/compatdata/2357570/pfx"}}));
        show("overwatch env", i);
        CHECK(i.key == "steam-app-2357570" && i.displayName == "Overwatch®" && i.desktopId == "Overwatch" && i.family == Family::Steam && i.isGame);
        CHECK(i.icon.kind == Icon::Kind::Theme && i.icon.value == "steam_icon_2357570" && i.resolver == "SteamApp" && has(i.evidence, "env SteamAppId=2357570"));
        CHECK(groupKeyOf(i, win("overwatch.exe")) == "hd:steam-app-2357570");
        // b) by class, with no process facts at all -- and the fast tier agrees
        i = run(S, win("steam_app_2357570"), ProcessFacts{});
        CHECK(i.key == "steam-app-2357570" && i.displayName == "Overwatch®" && has(i.evidence, "class steam_app_2357570"));
        const auto FAST = resolveFast(win("steam_app_2357570"), S.ix, InventoryView{.entries = S.inv});
        CHECK(FAST.key == i.key && FAST.displayName == i.displayName && FAST.icon.value == i.icon.value);
        // c) by the reaper hop, the environment scrubbed
        i = run(S, win("overwatch.exe"),
                proc("C:\\Program Files\\Overwatch\\Overwatch.exe", {}, {{.pid = 4000, .comm = "pv-adverb", .cmdline = "pv-adverb --"}, {.pid = 3990, .comm = "reaper", .cmdline = "/home/user/.local/share/Steam/ubuntu12_32/reaper SteamLaunch AppId=2357570 -- proton waitforexitandrun"}}));
        CHECK(i.key == "steam-app-2357570" && has(i.evidence, "reaper (pid 3990) AppId=2357570"));
        // d) STEAM_COMPAT_DATA_PATH alone
        i = run(S, win("overwatch.exe"), proc("C:\\Overwatch.exe", {{"STEAM_COMPAT_DATA_PATH", "/home/user/.local/share/Steam/steamapps/compatdata/2357570/"}}));
        CHECK(i.key == "steam-app-2357570");
        // e) no desktop shortcut: the manifest's name, the .ico's PNG
        i = run(S, win("steam_app_3321460"), ProcessFacts{});
        show("crimson", i);
        CHECK(i.key == "steam-app-3321460" && i.displayName == "Crimson Desert Enhanced" && i.desktopId.empty() && has(i.evidence, "appmanifest"));
        CHECK(i.icon.kind == Icon::Kind::File && i.icon.value == "/home/user/.local/share/hyprdrive/icons/steam-3321460-f2602673.png" && fs::exists(S.root + i.icon.value));
        // f) the removable library, jpg only
        i = run(S, win("steam_app_252950"), ProcessFacts{});
        CHECK(i.displayName == "Rocket League" && i.icon.value.ends_with(".jpg"));
        // g) a Steam game no index knows: still its own key
        i = run(S, win("steam_app_999"), ProcessFacts{});
        CHECK(i.key == "steam-app-999" && i.displayName == "steam_app_999" && i.icon.value == "steam_icon_999");
        // h) Steam itself: steam.desktop, today's key
        i = run(S, win("steam", "Steam"), proc("/home/user/.local/share/Steam/ubuntu12_32/steam"));
        CHECK(i.key == "steam" && i.desktopId == "steam" && i.family == Family::Native && groupKeyOf(i, win("steam")) == "steam" && !i.isGame);
        // i) a native app with a desktop file: today's identity, the class as group key
        const auto NW = win("org.gnome.Nautilus", "Home", false);
        i             = run(S, NW, proc("/usr/bin/nautilus"));
        show("nautilus", i);
        CHECK(i.key == "org.gnome.Nautilus" && i.displayName == "Files" && i.icon.value == "org.gnome.Nautilus" && i.family == Family::Native && i.resolver == "DesktopStrict");
        CHECK(groupKeyOf(i, NW) == "org.gnome.Nautilus" && resolveFast(NW, S.ix, InventoryView{.entries = S.inv}).key == i.key);
        // j) unreadable /proc, no entry: today's Fallback exactly
        const auto UW = win("SomeTool", "Some Tool", false);
        i             = run(S, UW, ProcessFacts{});
        CHECK(i.key == "SomeTool" && i.displayName == "SomeTool" && i.icon.value == "sometool" && i.family == Family::Unknown && i.resolver == "Fallback");
        CHECK(groupKeyOf(i, UW) == "SomeTool" && i.placeable);
        // k) a Flatpak PID: the "pid" names a host process started after the
        //    window mapped, whose env would say another game -- ignored.
        auto FW     = win("steam_app_2357570");
        FW.mapTicks = 1000;
        auto FP     = proc("/usr/bin/bash", {{"SteamAppId", "3321460"}});
        FP.startTicks = 5000;
        i             = run(S, FW, FP);
        CHECK(i.key == "steam-app-2357570" && has(i.evidence, "started after the window mapped"));
        auto GW     = win("game.exe");
        GW.mapTicks = 1000;
        auto GP     = proc("C:\\Games\\Other.exe", {{"WINEPREFIX", "/home/user/Games/other"}});
        GP.startTicks = 5000;
        i             = run(S, GW, GP);
        CHECK(i.key == "game.exe" && i.resolver == "Fallback");
        // l) the Steam overlay inside a game: the game (helpers group under their app)
        i = run(S, win("gameoverlayui.exe"), proc("C:\\Program Files\\Overwatch\\gameoverlayui.exe", {{"SteamAppId", "2357570"}}));
        CHECK(i.key == "steam-app-2357570");
        // m) Name == class no longer claims an .exe ("Launcher" is a native tool here)
        i = run(S, win("launcher"), ProcessFacts{});
        CHECK(i.key == "launcher-tool"); // today's answer for a native class "launcher"
        // canonicalKey on today's stores: a Steam shortcut's file id follows the app id.
        const InventoryView V{.entries = S.inv};
        CHECK(canonicalKey("Overwatch", V) == "steam-app-2357570");
        CHECK(canonicalKey("Overwatch 2", V) == "steam-app-2357570");
        CHECK(canonicalKey("org.gnome.Nautilus", V) == "org.gnome.Nautilus");
        CHECK(canonicalKey("net.lutris.battlenet-12", V) == "net.lutris.battlenet-12");
        CHECK(canonicalKey("hyprdrive-wine-wow.exe", V) == "hyprdrive-wine-wow.exe" && canonicalKey("hyprdrive-wine-forzahorizon6.exe", V) == "hyprdrive-wine-forzahorizon6.exe");
        CHECK(canonicalKey("steam-app-5", V) == "steam-app-5" && canonicalKey("not-installed", V) == "not-installed" && canonicalKey("", V).empty());
    }
    // ---- non-Steam shortcuts ------------------------------------------------------------
    {
        auto S    = load("steam-shortcuts");
        auto L    = load("lutris");
        S.ix.lutris = L.ix.lutris; // Steam's shortcut hands over to Lutris's game
        auto i    = run(S, win("steam_app_" + std::to_string(SHORT64)), ProcessFacts{});
        show("wow shortcut 64", i);
        CHECK(i.key == "steam-shortcut-3965729953" && i.displayName == "World of Warcraft Launcher.exe" && i.family == Family::SteamShortcut && has(i.evidence, "shortcuts.vdf"));
        i = run(S, win("world of warcraft launcher.exe"), proc("C:\\Program Files (x86)\\World of Warcraft\\World of Warcraft Launcher.exe", {{"SteamGameId", "3965729953"}}));
        CHECK(i.key == "steam-shortcut-3965729953" && has(i.evidence, "env SteamGameId=3965729953"));
        i = run(S, win("steam_app_" + std::to_string(CP64)), ProcessFacts{});
        show("cyberpunk via steam", i);
        CHECK(i.key == "net.lutris.cyberpunk-2077-3" && i.family == Family::Lutris && i.displayName == "Cyberpunk 2077" && has(i.evidence, "lutris:rungameid/3"));
    }
    // ---- umu: two games, one class -------------------------------------------------------
    {
        auto       U   = load("umu");
        const auto PFX = std::pair<std::string, std::string>{"WINEPREFIX", "/home/user/Games/umu/umu-default"};
        const auto ENV = std::vector<std::pair<std::string, std::string>>{PFX, {"GAMEID", "umu-default"}, {"PROTONPATH", "GE-Proton"}, {"SteamAppId", "default"}};
        const auto A   = run(U, win("steam_app_default"), proc("C:\\Games\\Alpha\\Alpha.exe", ENV, {{.pid = 77, .comm = "umu-run"}}));
        const auto B   = run(U, win("steam_app_default"), proc("C:\\Games\\Beta\\Beta.exe", ENV, {{.pid = 78, .comm = "umu-run"}}));
        show("umu alpha", A);
        show("umu beta", B);
        CHECK(A.key == "hyprdrive-wine-alpha.exe" && B.key == "hyprdrive-wine-beta.exe" && A.key != B.key);
        CHECK(A.displayName == "Alpha Game" && B.displayName == "Beta Game" && A.family == Family::Proton && A.isGame);
        CHECK(A.icon.kind == Icon::Kind::File && A.icon.value == "/home/user/.local/share/hyprdrive/icons/wine-alpha.exe.png" && fs::exists(U.root + A.icon.value));
        CHECK(groupKeyOf(A, win("steam_app_default")) == "hd:hyprdrive-wine-alpha.exe");
        // The fast tier cannot tell them apart (no /proc): today's class, nothing wrong.
        CHECK(resolveFast(win("steam_app_default"), U.ix, InventoryView{}).key == "steam_app_default");
        // explorer.exe of that prefix: a helper, so Alpha's (its session's app).
        const auto OWNER = [&](std::string_view k) -> const Identity* { return k == "wine:/home/user/Games/umu/umu-default|77" ? &A : nullptr; };
        auto       EW    = win("explorer.exe", "Wine Desktop");
        auto       i     = run(U, EW, proc("C:\\windows\\explorer.exe", ENV, {{.pid = 77, .comm = "umu-run"}}), OWNER);
        CHECK(i.key == A.key && i.resolver == "HelperOfSession" && i.placeable);
        // Wine's systray fallback: explorer.exe, no title, 32x32 -- never placed.
        auto TW      = win("explorer.exe", "");
        TW.w         = 32;
        TW.h         = 32;
        TW.fixedSize = true;
        i            = run(U, TW, proc("C:\\windows\\explorer.exe", ENV, {{.pid = 77, .comm = "umu-run"}}), OWNER);
        show("systray", i);
        CHECK(!i.placeable && i.key == A.key && has(i.evidence, "systray"));
        i = run(U, TW, proc("C:\\windows\\explorer.exe", ENV));
        CHECK(!i.placeable && i.key == "explorer.exe");
        CHECK(!resolveFast(TW, U.ix, InventoryView{}).placeable);
        // ...but a native process that happens to be called that is not Wine's.
        auto NP = proc("/usr/bin/explorer.exe");
        NP.comm = "explorer.exe";
        NP.exe  = "/usr/bin/explorer.exe";
        i       = run(U, TW, NP);
        CHECK(i.placeable && i.key == "explorer.exe");
        // A Wine popup menu (override-redirect): not placeable either.
        auto PW             = win("alpha.exe");
        PW.overrideRedirect = true;
        CHECK(!resolveFast(PW, U.ix, InventoryView{}).placeable);
        // A crash handler of that session joins its app.
        i = run(U, win("crashpad_handler.exe"), proc("C:\\Games\\Alpha\\crashpad_handler.exe", ENV, {{.pid = 77, .comm = "umu-run"}}), OWNER);
        CHECK(i.key == A.key && i.resolver == "HelperOfSession");
        // Without a known session owner a helper is no app of its own: Fallback.
        i = run(U, win("crashpad_handler.exe"), proc("C:\\Games\\Alpha\\crashpad_handler.exe", ENV));
        CHECK(i.key == "crashpad_handler.exe");
    }
    // ---- Lutris -------------------------------------------------------------------------
    {
        auto        L   = load("lutris");
        const auto* LIB = L.ix.lutris.get();
        CHECK(LIB && LIB->games.size() == 4); // death-stranding is not installed
        const auto* HG = LIB ? LIB->byName("Hogwarts Legacy") : nullptr;
        CHECK(HG && HG->exe.empty() && HG->ymlFrom.empty() && HG->icon == "/home/user/.local/share/lutris/coverart/hogwarts-legacy.jpg"); // the configpath mismatch
        const auto* EP = LIB ? LIB->byId(1) : nullptr;
        CHECK(EP && EP->exe == "/home/user/Games/epic-games-store/drive_c/Program Files/Epic Games/Launcher/Portal/Binaries/Win64/EpicGamesLauncher.exe");
        const auto* BN = LIB ? LIB->byId(12) : nullptr;
        CHECK(BN && BN->icon == "lutris_battlenet" && BN->folder == "/home/user/Games/battlenet/drive_c/Program Files (x86)/Battle.net");

        const auto BENV = std::vector<std::pair<std::string, std::string>>{
            {"WINEPREFIX", "/home/user/Games/battlenet"}, {"GAME_NAME", "Battle.net"}, {"GAME_DIRECTORY", "/home/user/Games/battlenet"}, {"LUTRIS_GAME_UUID", "6f1d2c3b-0000-4000-8000-000000000001"}};
        const auto BNET = run(L, win("battle.net.exe", "Battle.net"), proc("C:\\Program Files (x86)\\Battle.net\\Battle.net.exe", BENV));
        show("battle.net", BNET);
        CHECK(BNET.key == "net.lutris.battlenet-12" && BNET.displayName == "Battle.net" && BNET.icon.value == "lutris_battlenet" && BNET.family == Family::Lutris && has(BNET.evidence, "exe under"));
        // Overwatch, started by Battle.net, inherits GAME_NAME=Battle.net: its own app.
        const auto OW = run(L, win("overwatch.exe", "Overwatch"), proc("C:\\Program Files (x86)\\Overwatch\\_retail_\\Overwatch.exe", BENV));
        show("overwatch child", OW);
        CHECK(OW.key == "hyprdrive-wine-overwatch.exe" && OW.displayName == "Overwatch" && OW.key != BNET.key);
        // Its crash handler joins Overwatch, the session's app here.
        const auto OWNER = [&](std::string_view k) -> const Identity* { return k == "lutris:6f1d2c3b-0000-4000-8000-000000000001" ? &OW : nullptr; };
        auto       i     = run(L, win("unitycrashhandler64.exe"), proc("C:\\Program Files (x86)\\Overwatch\\_retail_\\UnityCrashHandler64.exe", BENV), OWNER);
        CHECK(i.key == OW.key && i.resolver == "HelperOfSession");
        // Hogwarts: pga.db's configpath names no file, so the exe is unknown --
        // GAME_NAME and GAME_DIRECTORY still give the game.
        const auto HENV = std::vector<std::pair<std::string, std::string>>{
            {"WINEPREFIX", "/home/user/Games/epic-games-store"}, {"GAME_NAME", "Hogwarts Legacy"}, {"GAME_DIRECTORY", "/home/user/Games/epic-games-store"}};
        i = run(L, win("hogwartslegacy.exe", "Hogwarts Legacy"), proc("C:\\Program Files\\Epic Games\\HogwartsLegacy\\Phoenix\\Binaries\\Win64\\HogwartsLegacy.exe", HENV));
        show("hogwarts", i);
        CHECK(i.key == "net.lutris.hogwarts-legacy-2" && i.confidence == 70 && has(i.evidence, "exe unknown"));
        // ...and the Epic launcher it runs through is Epic's own app, not Hogwarts.
        i = run(L, win("epicgameslauncher.exe", "Epic Games Launcher"), proc("C:\\Program Files\\Epic Games\\Launcher\\Portal\\Binaries\\Win64\\EpicGamesLauncher.exe", HENV));
        CHECK(i.key == "net.lutris.epic-games-store-1" && i.displayName == "Epic Games Store");
        // A program outside GAME_DIRECTORY does not take the unknown-exe game.
        i = run(L, win("tool.exe"), proc("C:\\tool.exe", {{"WINEPREFIX", "/home/user/Games/other"}, {"GAME_NAME", "Hogwarts Legacy"}, {"GAME_DIRECTORY", "/home/user/Games/epic-games-store"}}));
        CHECK(i.key != "net.lutris.hogwarts-legacy-2");
    }
    {
        auto       L = load("lutris-flatpak");
        const auto i = run(L, win("galaxyclient.exe", "GOG GALAXY"), proc("C:\\Program Files (x86)\\GOG Galaxy\\GalaxyClient.exe", {{"WINEPREFIX", "/home/user/Games/gog-galaxy"}, {"GAME_NAME", "GOG Galaxy"}}));
        CHECK(i.key == "net.lutris.gog-galaxy-4" && i.launch == "flatpak run net.lutris.Lutris lutris:rungameid/4");
        CHECK(i.icon.value == "/home/user/.var/app/net.lutris.Lutris/data/icons/hicolor/128x128/apps/lutris_gog-galaxy.png");
    }
    // ---- winemenubuilder entries, and launcher.exe in two prefixes ----------------------
    {
        auto       W  = load("wine-menus");
        const auto AP = proc("C:\\Alpha\\launcher.exe", {{"WINEPREFIX", "/home/user/Games/alpha/"}});
        const auto BP = proc("C:\\Beta\\launcher.exe", {{"WINEPREFIX", "/home/user/Games/beta"}});
        const auto A  = run(W, win("launcher.exe", "Alpha"), AP);
        const auto B  = run(W, win("launcher.exe", "Beta"), BP);
        show("launcher A", A);
        show("launcher B", B);
        CHECK(A.key == "wine-Programs-Alpha-Alpha Launcher" && A.family == Family::Wine && has(A.evidence, "WINEPREFIX matches"));
        CHECK(B.key == "hyprdrive-wine-launcher.exe" && B.displayName == "Beta" && B.key != A.key);
        CHECK(groupKeyOf(A, win("launcher.exe")) == "hd:wine-Programs-Alpha-Alpha Launcher");
        // Today's store form (wine-apps.tsv, keyed by class) already holds
        // Alpha's launcher.exe: Beta's gets @hash8 of its prefix and path.
        auto T = load("wine-menus");
        std::erase_if(T.inv, [](const InventoryEntry& e) { return e.id.starts_with("wine-Programs-Alpha"); });
        T.inv.push_back({.id      = "hyprdrive-wine-launcher.exe",
                         .name    = "Alpha",
                         .icon    = "/home/user/.local/share/hyprdrive/icons/wine-launcher.exe.png",
                         .wmClass = "launcher.exe",
                         .exec    = "env -C \"/home/user/Games/alpha/drive_c/Alpha\" \"WINEPREFIX=/home/user/Games/alpha\" \"wine\" \"/home/user/Games/alpha/drive_c/Alpha/launcher.exe\""});
        const auto TA = run(T, win("launcher.exe"), AP);
        const auto TB = run(T, win("launcher.exe"), BP);
        CHECK(TA.key == "hyprdrive-wine-launcher.exe" && TA.displayName == "Alpha");
        CHECK(TB.key == "hyprdrive-wine-launcher.exe@" + Resolvers::hash8("/home/user/Games/beta|/home/user/Games/beta/drive_c/Beta/launcher.exe") && TB.displayName == "Beta");
        // WoW Classic: the winemenubuilder entry, with its prefix checked; the
        // fast tier (prefix unknown) lands on the same entry.
        const auto WW = win("world of warcraft launcher.exe", "World of Warcraft");
        const auto i  = run(W, WW, proc("C:\\Program Files (x86)\\World of Warcraft\\World of Warcraft Launcher.exe", {{"WINEPREFIX", "/home/user/Games/battlenet"}}));
        CHECK(i.key == "wine-Programs-World of Warcraft Classic-World of Warcraft Classic" && i.displayName == "World of Warcraft Classic" && i.icon.value == "5298_World of Warcraft Launcher.0");
        const auto F = resolveFast(WW, W.ix, InventoryView{.entries = W.inv});
        CHECK(F.key == i.key && F.resolver == "Fallback" && has(F.evidence, "prefix unverified"));
    }
    // ---- Bottles: a plain Wine prefix ----------------------------------------------------
    {
        auto       B = load("bottles");
        const auto i = run(B, win("notepad++.exe", "new 1 - Notepad++"),
                           proc("C:\\Program Files\\Notepad++\\notepad++.exe", {{"WINEPREFIX", "/home/user/.var/app/com.usebottles.bottles/data/bottles/bottles/Office"}}));
        CHECK(i.key == "hyprdrive-wine-notepad++.exe" && i.displayName == "Notepad++" && i.family == Family::Wine);
    }
    // ---- Hyprdrive's own surface, and the data tables ----------------------------------
    {
        auto       S  = load("steam-native");
        auto       HW = win("hyprdrive-tray");
        HW.hyprdriveOwn = true;
        const auto i    = resolveFast(HW, S.ix, InventoryView{.entries = S.inv});
        CHECK(i.key == "hyprdrive-own" && !i.placeable);
        CHECK(Resolvers::isAmbiguousClass("steam_app_default") && Resolvers::isAmbiguousClass("Explorer.exe") && Resolvers::isAmbiguousClass("steam_app_battlenet"));
        CHECK(Resolvers::isAmbiguousClass("unitycrashhandler64.exe") && !Resolvers::isAmbiguousClass("steam_app_2357570") && !Resolvers::isAmbiguousClass("org.gnome.nautilus"));
        CHECK(Resolvers::isHelper("CrashReporter.exe") && Resolvers::isHelper("igoproxy64.exe") && Resolvers::isHelper("steamwebhelper") && !Resolvers::isHelper("overwatch.exe"));
        CHECK(Resolvers::glob("*overlay*.exe", "gameoverlayui.exe") && !Resolvers::glob("*overlay*.exe", "overlay.dll") && Resolvers::glob("a*b*c", "aXbYc"));
        CHECK(Resolvers::wineUnixPath("C:\\Games\\A.exe", "/p/") == "/p/drive_c/Games/A.exe" && Resolvers::wineUnixPath("D:\\x.exe", "/p") == "/p/dosdevices/d:/x.exe");
        CHECK(Resolvers::wineUnixPath("Z:\\home\\a.exe", "/p") == "/home/a.exe" && Resolvers::wineUnixPath("/usr/bin/x", "/p").empty());
        CHECK(Resolvers::execWinePrefix("env \"WINEPREFIX=/home/user/Games/battlenet\" wine x") == "/home/user/Games/battlenet");
        CHECK(Resolvers::execWinePrefix("env WINEPREFIX=/p/ wine x") == "/p" && Resolvers::execWinePrefix("steam").empty());
        ProcessFacts P;
        CHECK(sessionKey(P).empty());
        P = proc("x", {{"SteamAppId", "0"}, {"SteamGameId", "2357570"}});
        CHECK(sessionKey(P) == "steam:2357570");
        P = proc("x", {{"LUTRIS_GAME_UUID", "u"}, {"WINEPREFIX", "/p"}});
        CHECK(sessionKey(P) == "lutris:u");
    }
    // ---- /proc text ---------------------------------------------------------------------
    {
        const std::string ENV = "PATH=/usr/bin\0SteamAppId=2357570\0STEAM_API_TOKEN=secret\0WINEPREFIX=/p\0=x\0noequals\0SteamAppId=dup\0"s;
        const auto        E   = parseEnviron(asBytes(ENV));
        CHECK(E.size() == 2 && E[0].first == "SteamAppId" && E[0].second == "2357570" && E[1].first == "WINEPREFIX");
        const std::string CMD = "C:\\Games\\a.exe\0--flag\0"s;
        CHECK(argv0Of(asBytes(CMD)) == "C:\\Games\\a.exe" && joinCmdline(asBytes(CMD)) == "C:\\Games\\a.exe --flag");
        const auto S = parseStat(asBytes("4242 (wine (x) y) S 4000 4242 4000 0 -1 4194560 1 0 0 0 5 3 0 0 20 0 9 0 777777 1000 100"));
        CHECK(S && S->comm == "wine (x) y" && S->ppid == 4000 && S->startTime == 777777);
        CHECK(!parseStat(asBytes("4242 (a) S 4000")) && !parseStat(asBytes("garbage")));
    }
    // ...fuzzed like the file parsers: 10,000 seeded flips each, and every cut.
    {
        const std::string SEEDS[] = {"PATH=/usr/bin\0SteamAppId=2357570\0WINEPREFIX=/p\0GAME_NAME=Battle.net\0"s,
                                     "4242 (wine (x) y) S 4000 4242 4000 0 -1 4194560 1 0 0 0 5 3 0 0 20 0 9 0 777777 1000 100"s};
        std::mt19937 rng(0xFAC7u);
        size_t       n = 0;
        for (const auto& SEED : SEEDS) {
            for (size_t cut = 0; cut <= SEED.size(); ++cut, ++n) {
                (void)parseEnviron(asBytes(std::string_view{SEED}.substr(0, cut)));
                (void)parseStat(asBytes(std::string_view{SEED}.substr(0, cut)));
            }
            for (int k = 0; k < 10000; ++k, ++n) {
                std::string b = SEED;
                for (int j = 1 + static_cast<int>(rng() % 6); j > 0; --j)
                    b[rng() % b.size()] = static_cast<char>(rng() % 3 ? rng() : "\0=() "[rng() % 5]);
                (void)parseEnviron(asBytes(b));
                (void)parseStat(asBytes(b));
                (void)argv0Of(asBytes(b));
                (void)joinCmdline(asBytes(b), 16);
            }
        }
        CHECK(n == SEEDS[0].size() + SEEDS[1].size() + 2 + 20000);
    }

    std::error_code ec;
    fs::remove_all(g_tmp, ec);
    std::cout << "app_identity_test: " << s_checks << " checks, " << s_failed << " failed\n";
    return s_failed ? 1 : 0;
}
