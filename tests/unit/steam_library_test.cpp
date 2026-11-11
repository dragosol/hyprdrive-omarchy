// SteamLibrary (src/apps/identity/): the Steam index built from the fixture
// trees -- native, a removable library, Flatpak, non-Steam shortcuts -- the
// icon chain (theme name -> Steam's .ico as a PNG -> the library cache's jpg
// -> the theme name anyway), the stopgap's 30 s retry for a miss, and the
// newest-shortcut rule AppSearch uses. Plus FileSources' bounds: the cap, a
// directory, a FIFO, a relative path.
#include "IconPng.hpp"
#include "SteamLibrary.hpp"

#include <sys/stat.h>

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

// A private copy of a fixture tree, so icons written into it never touch the
// checked-in one.
static std::string copyFixture(const std::string& name, const fs::path& tmp) {
    const fs::path DST = tmp / name;
    std::error_code ec;
    fs::create_directories(tmp, ec);
    fs::copy("tests/fixtures/identity/" + name, DST, fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec);
    if (ec)
        std::cerr << "copy " << name << ": " << ec.message() << "\n";
    return DST.string();
}

static FileSources sources(const std::string& root, bool writer) {
    FileSources::Options o{.root = root, .env = Env::forHome("/home/user")};
    if (writer)
        o.writePng = [](const Ico::Picked& p, const std::string& out) { return IconPng::write(p, out); };
    return FileSources{std::move(o)};
}

int main() {
    char       tmpl[] = "/tmp/steam_library_test.XXXXXX";
    const auto TMP    = fs::path(mkdtemp(tmpl));
    using K           = Steam::GameId::Kind;

    {
        const auto ROOT = copyFixture("steam-native", TMP);
        const auto SRC  = sources(ROOT, true);
        const auto S    = SteamLibrary::rebuild(SRC);
        CHECK(S->roots.size() == 1 && S->roots[0] == "/home/user/.local/share/Steam");
        CHECK(S->libraries.size() == 3); // the unplugged one is listed and simply empty
        CHECK(S->apps.size() == 4);
        const auto* OW = S->app(2357570);
        CHECK(OW && OW->name == "Overwatch®" && OW->icon == "steam_icon_2357570" && OW->iconFrom == "theme"); // not the stray .tmp
        const auto* CD = S->app(3321460);
        CHECK(CD && CD->name == "Crimson Desert Enhanced" && CD->iconFrom == "ico");
        CHECK(CD && CD->icon == "/home/user/.local/share/hyprdrive/icons/steam-3321460-f2602673.png");
        if (CD) {
            std::ifstream f(ROOT + CD->icon, std::ios::binary);
            char          sig[4] = {};
            f.read(sig, 4);
            CHECK(f && std::string_view(sig, 4) == "\x89PNG");
        }
        const auto* RL = S->app(252950); // the removable library
        CHECK(RL && RL->name == "Rocket League" && RL->library == "/run/media/user/Games/SteamLibrary/steamapps");
        CHECK(RL && RL->iconFrom == "jpg" && RL->icon == "/home/user/.local/share/Steam/appcache/librarycache/252950/0b6c2d5e4f3a2918171615141312111009080706.jpg");
        CHECK(S->app(1826330) && S->app(1826330)->iconFrom == "default" && S->app(1826330)->icon == "steam_icon_1826330");
        CHECK(S->nameFor({K::App, 2357570}) == "Overwatch®" && S->nameFor({K::App, 1}).empty());
        CHECK(S->iconFor({K::App, 1}) == "steam_icon_1");
        // A second build finds the PNG it wrote instead of decoding again.
        const auto S2 = SteamLibrary::rebuild(SRC);
        CHECK(S2->app(3321460) && S2->app(3321460)->icon == CD->icon);
        // No icon writer: the jpg.
        const auto S3 = SteamLibrary::rebuild(sources(copyFixture("steam-native", TMP / "nowriter"), false));
        CHECK(S3->app(3321460) && S3->app(3321460)->iconFrom == "jpg" && S3->app(3321460)->icon.ends_with("/f260267377759a5fea6b651817c9a413c6b562b0.jpg"));

        // The stopgap: built on the first ask, kept while it answers, rebuilt
        // for a miss only once the build is 30 s old.
        SteamLibrary::publish(nullptr);
        const int64_t T0 = 1'000'000'000;
        const auto    A  = SteamLibrary::ensure(SRC, {K::App, 2357570}, T0);
        CHECK(A && A == SteamLibrary::current());
        CHECK(SteamLibrary::ensure(SRC, {K::App, 2357570}, T0 + 60'000'000'000LL) == A); // a hit never rebuilds
        CHECK(SteamLibrary::ensure(SRC, {K::App, 999}, T0 + 29'000'000'000LL) == A);     // a miss inside 30 s
        const auto B = SteamLibrary::ensure(SRC, {K::App, 999}, T0 + SteamLibrary::NEGATIVE_TTL_NS);
        CHECK(B && B != A && B->builtAtNs == T0 + SteamLibrary::NEGATIVE_TTL_NS);
        SteamLibrary::publish(nullptr);
    }
    {
        const auto S = SteamLibrary::rebuild(sources(copyFixture("steam-flatpak", TMP), true));
        CHECK(S->roots.size() == 1 && S->roots[0].ends_with("/.var/app/com.valvesoftware.Steam/.local/share/Steam"));
        const auto* H = S->app(1145360);
        CHECK(H && H->name == "Hades" && H->iconFrom == "jpg");
    }
    {
        const auto S = SteamLibrary::rebuild(sources(copyFixture("steam-shortcuts", TMP), true));
        CHECK(S->shortcuts.size() == 2 && S->apps.empty());
        CHECK(S->nameFor({K::Shortcut, 3965729953u}) == "World of Warcraft Launcher.exe");
        CHECK(S->iconFor({K::Shortcut, 4207589967u}) == "/home/user/.local/share/icons/hicolor/128x128/apps/lutris_cyberpunk-2077.png");
        CHECK(S->iconFor({K::Shortcut, 3965729953u}).empty());
        CHECK(S->shortcut(4207589967u) && S->shortcut(4207589967u)->launchOptions == "lutris:rungameid/3");
    }
    {
        // Nothing there at all.
        const auto S = SteamLibrary::rebuild(sources((TMP / "empty").string(), true));
        CHECK(S->apps.empty() && S->shortcuts.empty() && S->roots.empty());
    }
    {
        // newestShortcut: rungameid/ exactly, newest mtime, an unknown mtime never wins.
        struct E {
            std::string exec;
            int64_t     mtime;
        };
        const std::vector<E> V = {{"steam steam://rungameid/23575701", 900}, {"steam steam://rungameid/2357570", 100}, {"x", 999}, {"steam steam://rungameid/2357570", 300},
                                  {"steam steam://rungameid/2357570", 0}};
        const auto           N = [&](std::string_view d) {
            return SteamLibrary::newestShortcut(V.size(), d, [&](size_t i) { return std::string_view{V[i].exec}; }, [&](size_t i) { return V[i].mtime; });
        };
        CHECK(N("2357570") == 3);
        CHECK(N("") == std::string::npos && N("42") == std::string::npos);
    }
    {
        // FileSources' bounds.
        const auto ROOT = (TMP / "fs").string();
        fs::create_directories(ROOT + "/d");
        std::ofstream(ROOT + "/big") << std::string(5000, 'x');
        ::mkfifo((ROOT + "/fifo").c_str(), 0600);
        const auto SRC = sources(ROOT, false);
        CHECK(SRC.read("/big", 5000) && SRC.read("/big", 5000)->size() == 5000);
        CHECK(!SRC.read("/big", 4999));
        CHECK(!SRC.read("/d", 100) && !SRC.read("/fifo", 100) && !SRC.read("/missing", 100) && !SRC.read("big", 100));
        CHECK(SRC.readRange("/big", 4990, 100).size() == 10 && SRC.readRange("/big", 6000, 10).empty());
        CHECK(SRC.list("/").size() == 3 && SRC.list("/")[0] == "big" && SRC.list("/", 1).size() == 1);
        CHECK(SRC.stat("/d") && SRC.stat("/d")->dir && SRC.stat("/big")->size == 5000);
        CHECK(SRC.storeIcon("x", Ico::Picked{}).empty()); // no writer
        CHECK(!SRC.lutrisGames("/no/pga.db"));
    }

    std::error_code ec;
    fs::remove_all(TMP, ec);
    std::cout << "steam_library_test: " << s_checks << " checks, " << s_failed << " failed\n";
    return s_failed ? 1 : 0;
}
