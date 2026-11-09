// Thumbs.cpp's queue and freshness rules, against a stand-in thumbnailer that
// logs every run, in a private XDG tree. Built with a short settle time
// (-DTHUMBS_SETTLE_MS, host_tests.sh) so it waits a fraction of a second, not
// two: a file still being written, one that fails to thumbnail while it
// grows, a rewrite of a file whose thumbnail is on show, and a file dated in
// the future.
#include "../src/Thumbs.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace std::chrono_literals;

static int  g_fail = 0, g_checks = 0;
static void check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) {
        ++g_fail;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

static fs::path g_log; // the stand-in's: one line per run, the file it was run on
static const std::string KIND = "image/x-hyprdrive-thumbs-test";

static int runsOf(const fs::path& p) {
    std::ifstream in(g_log);
    int           n = 0;
    for (std::string line; std::getline(in, line);)
        n += line == p.string();
    return n;
}

// Until the thumbnailer has run on `p` `n` times (each run moves generation()).
static bool waitRuns(const fs::path& p, int n, std::chrono::milliseconds most = 3000ms) {
    for (auto t = 0ms; t < most; t += 5ms) {
        if (runsOf(p) >= n)
            break;
        std::this_thread::sleep_for(5ms);
    }
    // ...and until its result has landed: the log line comes before the rename.
    std::this_thread::sleep_for(30ms);
    return runsOf(p) >= n;
}

static void append(const fs::path& p) {
    std::ofstream(p, std::ios::app) << "more ";
}

static std::string lookup(const fs::path& p) {
    return ScrollOverview::Thumbs::lookup(p.string(), KIND);
}

int main() {
    char           tmpl[] = "/tmp/thumbs_test.XXXXXX";
    const fs::path ROOT   = ::mkdtemp(tmpl);
    // Before glib first asks: it keeps the directories it read.
    ::setenv("XDG_CACHE_HOME", (ROOT / "cache").c_str(), 1);
    ::setenv("XDG_DATA_HOME", (ROOT / "data").c_str(), 1);
    ::setenv("XDG_DATA_DIRS", (ROOT / "none").c_str(), 1);
    fs::create_directories(ROOT / "data" / "thumbnailers");
    fs::create_directories(ROOT / "files");
    g_log = ROOT / "runs.log";
    // The stand-in thumbnailer: logs the run, writes a non-empty "PNG"
    // (lookup asks no more of one), and fails on a file named broken*.
    const auto SCRIPT = ROOT / "thumbnail.sh";
    std::ofstream(SCRIPT) << "#!/bin/sh\necho \"$1\" >> '" << g_log.string() << "'\ncase \"$1\" in */broken*) exit 1;; esac\nprintf png > \"$2\"\n";
    fs::permissions(SCRIPT, fs::perms::owner_all);
    std::ofstream(ROOT / "data" / "thumbnailers" / "test.thumbnailer") << "[Thumbnailer Entry]\nExec=" << SCRIPT.string() << " %i %o\nMimeType=" << KIND << ";\n";

    const auto HOUR_AGO = fs::file_time_type::clock::now() - 1h;

    // A FILE STILL BEING WRITTEN: nothing is made while it grows (it was a
    // thumbnailer run per write), then one run once it has stood still.
    const auto GROWING = ROOT / "files" / "recording.png";
    const auto BROKEN  = ROOT / "files" / "broken-download.png";
    append(GROWING);
    append(BROKEN);
    check(lookup(GROWING).empty() && lookup(BROKEN).empty(), "a new file has no thumbnail yet");
    std::this_thread::sleep_for(60ms);
    check(runsOf(GROWING) == 0, "a file just written is not thumbnailed at once");
    for (int i = 0; i < 8; ++i) {
        std::this_thread::sleep_for(40ms);
        append(GROWING);
        append(BROKEN);
        (void)lookup(GROWING);
        (void)lookup(BROKEN);
    }
    check(runsOf(GROWING) == 0, "no thumbnailer runs while the file grows (" + std::to_string(runsOf(GROWING)) + " ran)");
    check(runsOf(BROKEN) == 0, "nor for one whose thumbnailer fails (" + std::to_string(runsOf(BROKEN)) + " ran)");
    check(waitRuns(GROWING, 1) && runsOf(GROWING) == 1, "made once when it stops growing");
    const auto THUMB = lookup(GROWING);
    check(!THUMB.empty() && fs::exists(THUMB), "...and that thumbnail is the one shown");
    check(waitRuns(BROKEN, 1) && runsOf(BROKEN) == 1, "the failing one is tried once when it stops growing");
    for (int i = 0; i < 3; ++i)
        (void)lookup(BROKEN);
    std::this_thread::sleep_for(300ms); // past the settle time: a retry would have come
    check(runsOf(BROKEN) == 1, "...and not again for the same version (" + std::to_string(runsOf(BROKEN)) + " runs)");

    // KEPT ON SHOW: rewritten, the file keeps its old thumbnail until the new
    // one lands (it was removed, and the file had no preview until then).
    append(GROWING);
    const auto MEANWHILE = lookup(GROWING);
    check(MEANWHILE == THUMB && fs::exists(THUMB), "a rewritten file keeps the thumbnail it had meanwhile");
    std::this_thread::sleep_for(60ms);
    check(runsOf(GROWING) == 1, "...and is not made again before it settles");
    check(waitRuns(GROWING, 2) && runsOf(GROWING) == 2, "...then made again, once");
    check(lookup(GROWING) == THUMB, "...in the same place");

    // DATED IN THE FUTURE (a camera's clock, an archive): it gets a preview.
    // Its thumbnail used to be removed at once as older than the file, and
    // never made again: the queue key, path and mtime, was already spent.
    const auto FUTURE = ROOT / "files" / "from-the-camera.png";
    append(FUTURE);
    fs::last_write_time(FUTURE, fs::file_time_type::clock::now() + 24h);
    (void)lookup(FUTURE);
    check(waitRuns(FUTURE, 1), "a file dated tomorrow is thumbnailed");
    check(!lookup(FUTURE).empty(), "...and the thumbnail is shown");
    check(!lookup(FUTURE).empty(), "...still, asked again");

    // A file last written an hour ago goes at once: no settle wait.
    const auto OLD = ROOT / "files" / "old-photo.png";
    append(OLD);
    fs::last_write_time(OLD, HOUR_AGO);
    (void)lookup(OLD);
    check(waitRuns(OLD, 1, 150ms), "a file written long ago is made without waiting");
    check(!lookup(OLD).empty(), "...and shown");

    ScrollOverview::Thumbs::shutdown();
    std::error_code ec;
    fs::remove_all(ROOT, ec);
    std::printf("thumbs_test: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
