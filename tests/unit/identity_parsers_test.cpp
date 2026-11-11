// AppIdentity's parsers (src/apps/identity/: Vdf, Acf, LutrisDb, Ico, WinExe)
// on the fixtures in tests/fixtures/identity/ -- the layouts of real Steam,
// Lutris, umu, Wine and Bottles installs, binaries synthetic (gen/) -- and then
// fuzzed: every prefix truncation of every fixture file, and 10,000 seeded
// multi-byte flips per parser, each case timed. A parser may answer anything
// for garbage; it may not crash, read out of bounds (make check-asan), throw,
// or take long.
#include "Acf.hpp"
#include "Ico.hpp"
#include "LutrisDb.hpp"
#include "WinExe.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <random>
#include <string>
#include <vector>

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

static const std::string FIX = "tests/fixtures/identity/";

static std::vector<uint8_t> slurp(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

namespace {
    struct SKind {
        std::string                       name;
        std::function<void(Bytes)>        run;
        std::vector<std::vector<uint8_t>> corpus;
        std::vector<std::string>          files;
    };
}

int main() {
    // ---- the fixtures, parsed ---------------------------------------------------
    {
        const auto B = slurp(FIX + "steam-native/home/user/.local/share/Steam/steamapps/appmanifest_2357570.acf");
        const auto M = Steam::parseManifest(B);
        CHECK(M && M->appid == 2357570 && M->name == "Overwatch®" && M->installdir == "Overwatch");
        const auto C = Steam::parseManifest(slurp(FIX + "steam-native/home/user/.local/share/Steam/steamapps/appmanifest_1826330.acf"));
        CHECK(C && C->name == "Proton EasyAntiCheat Runtime");
    }
    {
        const auto P = Steam::parseLibraryFolders(slurp(FIX + "steam-native/home/user/.local/share/Steam/steamapps/libraryfolders.vdf"));
        CHECK(P.size() == 3 && P[0] == "/home/user/.local/share/Steam" && P[1] == "/run/media/user/Games/SteamLibrary");
        // The pre-2021 layout: "1" "/path".
        const auto O = Steam::parseLibraryFolders(slurp(FIX + "steam-flatpak/home/user/.var/app/com.valvesoftware.Steam/.local/share/Steam/steamapps/libraryfolders.vdf"));
        CHECK(O.size() == 1 && O[0] == "/home/user/.var/app/com.valvesoftware.Steam/.local/share/Steam");
    }
    {
        const auto S = Steam::parseShortcuts(slurp(FIX + "steam-shortcuts/home/user/.local/share/Steam/userdata/12345678/config/shortcuts.vdf"));
        CHECK(S.size() == 2);
        if (S.size() == 2) {
            CHECK(S[0].appid == 3965729953u && S[0].name == "World of Warcraft Launcher.exe");
            CHECK(S[0].exe == "/home/user/Games/battlenet/drive_c/Program Files (x86)/World of Warcraft/World of Warcraft Launcher.exe"); // quotes gone
            CHECK(S[1].appid == 4207589967u && S[1].exe == "/usr/bin/lutris" && S[1].launchOptions == "lutris:rungameid/3");
            CHECK(S[1].icon.ends_with("lutris_cyberpunk-2077.png"));
        }
    }
    {
        const auto G = Lutris::parseGameSection(slurp(FIX + "lutris/home/user/.local/share/lutris/games/battlenet-12.yml"));
        CHECK(G.contains("exe") && G.at("exe") == "drive_c/Program Files (x86)/Battle.net/Battle.net.exe");
        CHECK(G.contains("prefix") && G.at("prefix") == "/home/user/Games/battlenet");
        CHECK(!G.contains("STAGING_SHARED_MEMORY") && !G.contains("version"));
        const auto Q = Lutris::parseGameSection(slurp(FIX + "lutris/home/user/.local/share/lutris/games/cyberpunk-2077-1767769963.yml"));
        CHECK(Q.contains("exe") && Q.at("exe") == "/home/user/Games/gog/Cyberpunk 2077/bin/x64/Cyberpunk2077.exe"); // quoted
        const auto H = Lutris::parseGameSection(slurp(FIX + "lutris/home/user/.local/share/lutris/games/hogwarts-legacy-1774237099.yml"));
        CHECK(!H.contains("exe") && H.contains("appid"));
        const auto CR = Lutris::parseGameSection(asBytes("game:\r\n  exe: a.exe\r\n  prefix: '/p'\r\nwine:\r\n  exe: no\r\n"));
        CHECK(CR.size() == 2 && CR.at("exe") == "a.exe" && CR.at("prefix") == "/p");
    }
    {
        const auto R = Lutris::parsePgaTsv(slurp(FIX + "lutris/home/user/.local/share/lutris/pga.db.tsv"));
        CHECK(R.size() == 5);
        if (R.size() == 5) {
            CHECK(R[1].slug == "hogwarts-legacy" && R[1].configpath == "hogwarts-legacy-1774237481" && R[1].installed && R[1].service == "egs");
            CHECK(R[0].service.empty()); // \N
            CHECK(!R[3].installed);
            CHECK(R[4].id == 12 && R[4].name == "Battle.net" && R[4].directory == "/home/user/Games/battlenet");
        }
    }
    {
        const auto BN = slurp(FIX + "lutris/home/user/Games/battlenet/drive_c/Program Files (x86)/Battle.net/Battle.net.exe");
        const auto PE = Hyprdrive::WinExe::fromBuffer(BN);
        CHECK(PE.ok);
        CHECK(Hyprdrive::WinExe::name(PE) == "Battle.net");
        const auto I = Hyprdrive::WinExe::iconImage(PE);
        CHECK(I && I->size == 32 && I->isPng()); // the larger (PNG) image of the group
        // ProductName "Unity Player" is the engine's, so FileDescription speaks.
        const auto UC = Hyprdrive::WinExe::fromBuffer(slurp(FIX + "lutris/home/user/Games/battlenet/drive_c/Program Files (x86)/Overwatch/_retail_/UnityCrashHandler64.exe"));
        CHECK(Hyprdrive::WinExe::name(UC) == "Unity Crash Handler");
        // The file reader takes the same path.
        const auto F = Hyprdrive::WinExe::read(FIX + "lutris/home/user/Games/battlenet/drive_c/Program Files (x86)/Battle.net/Battle.net.exe");
        CHECK(F.ok && F.rsrc == PE.rsrc);
        CHECK(!Hyprdrive::WinExe::read(FIX + "no/such.exe").ok);
    }
    {
        const auto ICO = slurp(FIX + "steam-native/home/user/.local/share/Steam/steam/games/f260267377759a5fea6b651817c9a413c6b562b0.ico");
        const auto P   = Ico::best(ICO);
        CHECK(P && P->size == 48 && P->isPng());
        auto E = Ico::directory(ICO, 16);
        CHECK(E.size() == 2);
        // The 16 px DIB: gold (220,170,30) opaque, premultiplied ARGB.
        if (E.size() == 2) {
            const auto D = Ico::decode(Bytes{ICO}.subspan(E[0].ref, E[0].bytes));
            CHECK(D && !D->isPng() && D->image.w == 16 && D->image.h == 16 && D->image.argb.size() == 256);
            CHECK(D && D->image.argb[0] == 0xFFDCAA1Eu);
        }
    }
    // ---- ids ------------------------------------------------------------------------
    {
        using K = Steam::GameId::Kind;
        CHECK((Steam::parseGameId("2357570") == Steam::GameId{K::App, 2357570}));
        CHECK((Steam::parseGameId("3965729953") == Steam::GameId{K::Shortcut, 3965729953u}));
        CHECK((Steam::parseGameId(std::to_string((uint64_t{3965729953u} << 32) | 0x02000000u)) == Steam::GameId{K::Shortcut, 3965729953u}));
        CHECK(!Steam::parseGameId("0") && !Steam::parseGameId("default") && !Steam::parseGameId("") && !Steam::parseGameId("99999999999999999999"));
        CHECK(!Steam::fromClass("steam_app_default") && !Steam::fromClass("steam_app_0") && !Steam::fromClass("steam_app_") && !Steam::fromClass("steam"));
        CHECK((Steam::fromClass("STEAM_APP_2357570") == Steam::GameId{K::App, 2357570}));
        CHECK(Steam::classDigits("steam_app_2357570") == "2357570" && Steam::classDigits("steam_app_battlenet").empty());
        CHECK(Steam::execLaunches("steam steam://rungameid/2357570", "2357570"));
        CHECK(!Steam::execLaunches("steam steam://rungameid/23575701", "2357570"));
        CHECK(Steam::execLaunches("x rungameid/23575701 y rungameid/2357570", "2357570"));
        CHECK((Steam::fromExec("steam steam://rungameid/2357570") == Steam::GameId{K::App, 2357570}));
        CHECK(Steam::keyOf({K::Shortcut, 7}) == "steam-shortcut-7");
    }
    // ---- KeyValues edge cases -----------------------------------------------------
    {
        const auto D = Vdf::parseText(asBytes("// comment\n\"a\" { \"k\" \"v\\\"q\" [$WIN32]\n unquoted value\n \"N\" \"42\" }\n"));
        CHECK(D && D->child("A") && D->child("a")->value("K") == "v\"q" && D->child("a")->value("unquoted") == "value" && D->child("a")->number("n") == 42u);
        CHECK(!Vdf::parseText(asBytes("\"a\" { \"k\" \"v\"")));      // unclosed map
        CHECK(!Vdf::parseText(asBytes("\"a\" \"unterminated")));      // unterminated string
        CHECK(!Vdf::parseText(asBytes("}")));
        std::string deep, ok;
        for (int i = 0; i < 40; ++i)
            deep += "\"k\" { ";
        for (int i = 0; i < 40; ++i)
            deep += "} ";
        CHECK(!Vdf::parseText(asBytes(deep))); // past Caps::DEPTH
        for (int i = 0; i < 32; ++i)
            ok += "\"k\" { ";
        for (int i = 0; i < 32; ++i)
            ok += "} ";
        CHECK(Vdf::parseText(asBytes(ok)));
        std::string big(Caps::VDF + 1, ' ');
        CHECK(!Vdf::parseText(asBytes(big))); // over the cap
        const uint8_t BIN[] = {0, 'r', 0, 2, 'i', 0, 7, 0, 0, 0, 7, 'q', 0, 1, 0, 0, 0, 0, 0, 0, 0, 8, 8};
        const auto    BD    = Vdf::parseBinary(BIN);
        CHECK(BD && BD->child("r") && BD->child("r")->number("i") == 7u && BD->child("r")->number("q") == 1u);
        CHECK(!Vdf::parseBinary(Bytes{BIN, sizeof BIN - 1})); // the last end marker missing
    }

    // ---- fuzz ------------------------------------------------------------------------
    std::vector<SKind> kinds;
    kinds.push_back({"vdf-text", [](Bytes b) {
                         (void)Vdf::parseText(b);
                         (void)Steam::parseManifest(b);
                         (void)Steam::parseLibraryFolders(b);
                     }});
    kinds.push_back({"vdf-binary", [](Bytes b) {
                         (void)Vdf::parseBinary(b);
                         (void)Steam::parseShortcuts(b);
                     }});
    kinds.push_back({"lutris-yml", [](Bytes b) { (void)Lutris::parseGameSection(b); }});
    kinds.push_back({"pga-tsv", [](Bytes b) { (void)Lutris::parsePgaTsv(b); }});
    kinds.push_back({"pe", [](Bytes b) {
                         const auto PE = Hyprdrive::WinExe::fromBuffer(b);
                         (void)Hyprdrive::WinExe::name(PE);
                         (void)Hyprdrive::WinExe::iconImage(PE);
                     }});
    kinds.push_back({"ico", [](Bytes b) {
                         (void)Ico::best(b);
                         (void)Ico::decode(b);
                         (void)Ico::directory(b, 14);
                     }});
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(FIX, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec) || it->is_symlink(ec))
            continue;
        const std::string P = it->path().string(), N = it->path().filename().string();
        int               k = -1;
        if (N == "shortcuts.vdf")
            k = 1;
        else if (N.ends_with(".vdf") || N.ends_with(".acf") || N.ends_with(".tmp"))
            k = 0;
        else if (N.ends_with(".yml"))
            k = 2;
        else if (N.ends_with(".tsv"))
            k = 3;
        else if (N.ends_with(".exe"))
            k = 4;
        else if (N.ends_with(".ico") || N.ends_with(".png") || N.ends_with(".jpg"))
            k = 5;
        if (k < 0)
            continue;
        kinds[k].corpus.push_back(slurp(P));
        kinds[k].files.push_back(P);
    }
    // A sanitizer build runs ~10x slower; the bound is for a hang, not a profile.
    const double LIMIT_MS = 250.0;
    double       worstMs  = 0;
    size_t       cases    = 0;
    auto         timed    = [&](const SKind& K, Bytes b, const std::string& what) {
        const auto T0 = std::chrono::steady_clock::now();
        K.run(b);
        const double MS = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - T0).count();
        worstMs         = std::max(worstMs, MS);
        ++cases;
        if (MS > LIMIT_MS) {
            ++s_failed;
            std::cerr << "SLOW " << K.name << " " << what << " " << MS << " ms\n";
        }
    };
    size_t truncations = 0, flips = 0;
    for (const auto& K : kinds) {
        CHECK(!K.corpus.empty());
        for (size_t f = 0; f < K.corpus.size(); ++f)
            for (size_t n = 0; n <= K.corpus[f].size(); ++n, ++truncations)
                timed(K, Bytes{K.corpus[f].data(), n}, K.files[f] + " cut at " + std::to_string(n));
    }
    for (size_t k = 0; k < kinds.size(); ++k) {
        const auto&  K = kinds[k];
        std::mt19937 rng(0x1D3A7u + static_cast<unsigned>(k));
        for (int i = 0; i < 10000 && !K.corpus.empty(); ++i, ++flips) {
            const size_t F   = rng() % K.corpus.size();
            auto         buf = K.corpus[F];
            if (buf.empty())
                continue;
            const int N = 1 + static_cast<int>(rng() % 8);
            for (int j = 0; j < N; ++j) {
                const size_t AT = rng() % buf.size();
                // Mostly random bytes; sometimes the values that steer a parser
                // (a KeyValues type or end marker, a quote, a brace, 0xFF).
                static constexpr uint8_t STEER[] = {0, 1, 2, 7, 8, 11, '"', '{', '}', 0xFF, 0x80};
                buf[AT] = rng() % 3 ? static_cast<uint8_t>(buf[AT] ^ (1u + rng() % 255)) : STEER[rng() % sizeof STEER];
            }
            if (rng() % 8 == 0)
                buf.resize(rng() % buf.size() + 1);
            timed(K, buf, K.files[F] + " flip #" + std::to_string(i));
        }
    }
    CHECK(flips == kinds.size() * 10000);

    std::cout << "identity_parsers_test: " << s_checks << " checks, " << s_failed << " failed; fuzz " << cases << " cases (" << truncations << " truncations, " << flips
              << " flips), worst " << worstMs << " ms\n";
    return s_failed ? 1 : 0;
}
