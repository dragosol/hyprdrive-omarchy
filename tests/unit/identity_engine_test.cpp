// The identity worker (src/apps/identity/Engine.hpp) on the fixture trees,
// with a fake Gatherer: it builds the indexes itself, resolves a job in full
// and posts the Result; a helper resolved before its app is resolved again
// when the app lands; cancel() drops a window's job and results; a removable
// library whose stat does not answer in time is skipped, remembered, and
// costs one probe timeout, not one per rebuild; a stop with jobs queued and a
// slow source returns at once.
#include "fixture_links.hpp"
#include "Engine.hpp"
#include "IconPng.hpp"

#include <filesystem>
#include <iostream>
#include <map>
#include <thread>

namespace fs = std::filesystem;
using namespace Hyprdrive::AppIdentity;
using namespace std::chrono_literals;

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

    std::unique_ptr<FileSources> fixture(const std::string& name) {
        std::error_code ec;
        const auto      ROOT = (g_tmp / name).string();
        fs::copy("tests/fixtures/identity/" + name, ROOT, fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec);
        fixtureLinks(name, ROOT);
        return std::make_unique<FileSources>(FileSources::Options{
            .root = ROOT, .env = Env::forHome("/home/user"), .writePng = [](const Ico::Picked& p, const std::string& out) { return IconPng::write(p, out); }});
    }

    // A Sources whose stat of anything under /run/media takes `slow`.
    struct SSlowMedia final : Sources {
        const Sources&            in;
        std::chrono::milliseconds slow;
        mutable std::atomic<int>  slowStats{0};
        SSlowMedia(const Sources& s, std::chrono::milliseconds d) : in(s), slow(d) {}
        const Env& env() const override {
            return in.env();
        }
        std::optional<std::vector<uint8_t>> read(const std::string& p, size_t cap) const override {
            return in.read(p, cap);
        }
        std::vector<uint8_t> readRange(const std::string& p, uint64_t off, size_t n) const override {
            return in.readRange(p, off, n);
        }
        std::optional<FileInfo> stat(const std::string& p) const override {
            if (p.starts_with("/run/media")) {
                ++slowStats;
                std::this_thread::sleep_for(slow);
            }
            return in.stat(p);
        }
        std::vector<std::string> list(const std::string& d, size_t max) const override {
            return in.list(d, max);
        }
        std::optional<std::vector<Lutris::Row>> lutrisGames(const std::string& db) const override {
            return in.lutrisGames(db);
        }
        std::string storeIcon(const std::string& n, const Ico::Picked& i) const override {
            return in.storeIcon(n, i);
        }
        std::string storedIcon(const std::string& n) const override {
            return in.storedIcon(n);
        }
    };

    struct SFakeGather final : Gatherer {
        std::map<pid_t, ProcessFacts> procs;
        ProcessFacts                  process(pid_t pid) override {
            const auto IT = procs.find(pid);
            return IT == procs.end() ? ProcessFacts{} : IT->second;
        }
    };

    WindowFacts win(std::string cls, pid_t pid = 0) {
        return {.cls = cls, .initialClass = cls, .x11 = true, .w = 1280, .h = 720, .pid = pid};
    }

    // Results until `n` have arrived or `budget` passed.
    std::vector<Result> await(Engine& e, size_t n, std::chrono::milliseconds budget = 5000ms) {
        std::vector<Result> got;
        const auto          END = std::chrono::steady_clock::now() + budget;
        while (got.size() < n && std::chrono::steady_clock::now() < END) {
            for (auto& r : e.take())
                got.push_back(std::move(r));
            std::this_thread::sleep_for(5ms);
        }
        return got;
    }
    bool awaitIndexed(Engine& e) {
        const auto END = std::chrono::steady_clock::now() + 5s;
        while (!e.stats().indexed && std::chrono::steady_clock::now() < END)
            std::this_thread::sleep_for(5ms);
        return e.stats().indexed;
    }
}

int main() {
    char tmpl[] = "/tmp/identity_engine_test.XXXXXX";
    g_tmp       = mkdtemp(tmpl);
    Hyprdrive::Workers::setPin([] {});

    // ---- a Steam class, resolved in full, the indexes built by the worker ------
    {
        const auto SRC = fixture("steam-native");
        std::atomic<int> wakes{0};
        Engine           e{{.src = SRC.get(), .wake = [&] { ++wakes; }, .probeTimeout = 300ms, .publish = false}};
        CHECK(e.start());
        CHECK(awaitIndexed(e));
        CHECK(wakes > 0 && e.takeIndexesChanged() && !e.takeIndexesChanged());
        CHECK(!SteamLibrary::current()); // publish = false: nothing global touched
        e.enqueue({.addr = 1, .serial = 7, .w = win("steam_app_2357570")});
        auto R = await(e, 1);
        CHECK(R.size() == 1 && R[0].addr == 1 && R[0].serial == 7);
        CHECK(!R.empty() && R[0].id.key == "steam-app-2357570" && R[0].id.displayName == "Overwatch®");
        // The removable libraries (/run/media/...) are outside $HOME: probed;
        // the one that is there answers, the one that is gone is skipped.
        CHECK(e.indexes().steam && e.indexes().steam->skipped.size() == 1 && e.indexes().steam->skipped[0].starts_with("/run/media/user/Gone/"));
        // cancel(): a queued job and a posted result of that window go.
        e.enqueue({.addr = 2, .serial = 1, .w = win("steam_app_2357570")});
        std::this_thread::sleep_for(200ms);
        e.cancel(2);
        CHECK(e.take().empty());
        CHECK(e.stop());
        CHECK(!e.running());
    }

    // ---- a sleeping removable disk -------------------------------------------------
    // (Outside the block: an abandoned probe still holds the Sources, as the
    // plugin's static liveSources() outlives its own.)
    const auto SLOWSRC = fixture("steam-native");
    SSlowMedia SLOW{*SLOWSRC, 1500ms};
    {
        Engine     e{{.src = &SLOW, .probeTimeout = 100ms, .recheckEvery = 0ms, .publish = false}};
        const auto T0 = std::chrono::steady_clock::now();
        CHECK(e.start());
        CHECK(awaitIndexed(e));
        const auto BUILD = std::chrono::steady_clock::now() - T0;
        CHECK(BUILD < 1000ms); // the probe timed out; the index did not wait for the disk
        const auto IX = e.indexes();
        CHECK(IX.steam && IX.steam->skipped.size() == 2 && IX.steam->skipped[0].starts_with("/run/media/"));
        CHECK(IX.steam && IX.steam->app(2357570)); // the home library is still there
        // Remembered: the recheck a job triggers does not probe the disk again.
        const int BEFORE = SLOW.slowStats;
        e.enqueue({.addr = 1, .serial = 1, .w = win("steam_app_2357570")});
        CHECK(await(e, 1).size() == 1);
        CHECK(SLOW.slowStats == BEFORE);
        CHECK(e.stats().probesFailed == 2);
        CHECK(e.stop());
    }

    // ---- a helper before its app ------------------------------------------------------
    {
        const auto SRC = fixture("umu");
        auto       G   = std::make_shared<SFakeGather>();
        const auto PFX = std::pair<std::string, std::string>{"WINEPREFIX", "/home/user/Games/umu/umu-default"};
        const std::vector<std::pair<std::string, std::string>> ENV{PFX, {"GAMEID", "umu-default"}, {"PROTONPATH", "GE-Proton"}, {"SteamAppId", "default"}};
        const auto P = [&](pid_t pid, std::string argv0) {
            return ProcessFacts{.readable = true, .pid = pid, .startTicks = 1, .comm = "wine64-preloader", .argv0 = std::move(argv0), .exe = "/usr/bin/wine64-preloader",
                                .env = ENV, .chain = {{.pid = 77, .comm = "umu-run"}}};
        };
        G->procs[501] = P(501, "C:\\Games\\Alpha\\crashpad_handler.exe");
        G->procs[502] = P(502, "C:\\Games\\Alpha\\Alpha.exe");
        Engine e{{.src = SRC.get(), .gather = G, .publish = false}};
        CHECK(e.start());
        CHECK(awaitIndexed(e));
        e.enqueue({.addr = 10, .serial = 1, .w = win("crashpad_handler.exe", 501)});
        auto first = await(e, 1);
        CHECK(first.size() == 1 && first[0].id.key != "hyprdrive-wine-alpha.exe");
        CHECK(!first.empty() && first[0].session == "wine:/home/user/Games/umu/umu-default|77");
        e.enqueue({.addr = 11, .serial = 1, .w = win("steam_app_default", 502)});
        auto two = await(e, 2);
        CHECK(two.size() == 2);
        bool app = false, helper = false;
        for (const auto& r : two) {
            app    = app || (r.addr == 11 && r.id.key == "hyprdrive-wine-alpha.exe");
            helper = helper || (r.addr == 10 && r.id.key == "hyprdrive-wine-alpha.exe" && r.id.resolver == "HelperOfSession");
        }
        CHECK(app && helper);
        CHECK(e.stop());
    }

    // ---- a stop with jobs queued behind a slow source -----------------------------------
    {
        const auto SRC = fixture("steam-native");
        Engine     e{{.src = SRC.get(), .jobDelay = 5000ms, .publish = false}};
        CHECK(e.start());
        CHECK(awaitIndexed(e));
        for (uintptr_t a = 1; a <= 50; ++a)
            e.enqueue({.addr = a, .serial = 1, .w = win("steam_app_2357570")});
        std::this_thread::sleep_for(50ms);
        const auto T0 = std::chrono::steady_clock::now();
        CHECK(e.stop());
        CHECK(std::chrono::steady_clock::now() - T0 < 500ms);
        CHECK(e.take().empty());
    }

    Hyprdrive::Workers::beginShutdown();
    CHECK(Hyprdrive::Workers::finishShutdown() == 0);
    std::error_code ec;
    fs::remove_all(g_tmp, ec);
    std::cout << "identity_engine_test: " << s_checks << " checks, " << s_failed << " failed\n";
    return s_failed ? 1 : 0;
}
