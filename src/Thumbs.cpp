// See Thumbs.hpp.
#include "Thumbs.hpp"

#include <glib.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <vector>

#include <sys/resource.h>

// How long a file's mtime must stand still before it is thumbnailed (SETTLE,
// below). scripts/thumbs_test.cpp builds this file with a shorter one.
#ifndef THUMBS_SETTLE_MS
#define THUMBS_SETTLE_MS 2000
#endif

namespace ScrollOverview::Thumbs {
    namespace {
        struct SThumbnailer {
            std::vector<std::string> exec; // argv with %i %u %o %s
        };

        // A FILE STILL BEING WRITTEN WAITS (2026-11-09). A download or a
        // recording changes its mtime all the time: every thumbnail made of
        // it was out of date as it landed, so the next lookup removed it and
        // queued the file again under its new mtime -- thumbnailer runs back
        // to back for as long as the file grew, no preview the whole time,
        // and one more queue key per run (a failed run looped the same way).
        // Now a file is made once its mtime has stood still for SETTLE (the
        // worker watches it; a file it never made, last written longer ago
        // than that, goes at once). Until then the thumbnail it had stays on
        // show (lookup).
        constexpr auto SETTLE = std::chrono::milliseconds(THUMBS_SETTLE_MS);

        // ONE JOB PER PATH, whatever mtimes the file goes through.
        struct SJob {
            std::string                           kind;
            std::filesystem::file_time_type       asked{}; // the version lookup last saw
            std::filesystem::file_time_type       seen{};  // the version the worker last saw...
            std::chrono::steady_clock::time_point since{}; // ...and since when
            std::filesystem::file_time_type       made{};  // the version last made (or failed)
            bool                                  done   = false; // `made` is set
            bool                                  queued = false; // in g_queue, or being made
        };

        std::mutex                            g_mutex;
        std::condition_variable               g_cv;
        std::deque<std::string>               g_queue; // paths waiting their turn
        std::unordered_map<std::string, SJob> g_jobs;
        std::thread                           g_worker;
        bool                                  g_stop = false;
        std::atomic<uint64_t>                 g_gen{1};
        std::function<void()>                 g_ready;

        std::string md5Uri(const std::string& path) {
            char* uri = g_filename_to_uri(path.c_str(), nullptr, nullptr);
            if (!uri)
                return {};
            char*             md5 = g_compute_checksum_for_string(G_CHECKSUM_MD5, uri, -1);
            const std::string OUT = md5 ? md5 : "";
            g_free(md5);
            g_free(uri);
            return OUT;
        }

        std::string ownDir() {
            return std::string(g_get_user_cache_dir()) + "/hyprdrive/thumbnails";
        }

        // mime -> thumbnailer, from every .thumbnailer file (user dirs first).
        const std::unordered_map<std::string, SThumbnailer>& thumbnailers() {
            static std::unordered_map<std::string, SThumbnailer> map;
            static bool                                           loaded = false;
            if (loaded)
                return map;
            loaded = true;
            std::vector<std::string> dirs{std::string(g_get_user_data_dir()) + "/thumbnailers"};
            for (const char* const* d = g_get_system_data_dirs(); d && *d; ++d)
                dirs.emplace_back(std::string(*d) + "/thumbnailers");
            std::error_code ec;
            for (const auto& DIR : dirs) {
                for (const auto& E : std::filesystem::directory_iterator(DIR, ec)) {
                    if (E.path().extension() != ".thumbnailer")
                        continue;
                    GKeyFile* kf = g_key_file_new();
                    if (g_key_file_load_from_file(kf, E.path().c_str(), G_KEY_FILE_NONE, nullptr)) {
                        gchar*  exec  = g_key_file_get_string(kf, "Thumbnailer Entry", "Exec", nullptr);
                        gchar*  tryex = g_key_file_get_string(kf, "Thumbnailer Entry", "TryExec", nullptr);
                        gsize   n     = 0;
                        gchar** mimes = g_key_file_get_string_list(kf, "Thumbnailer Entry", "MimeType", &n, nullptr);
                        bool    ok    = exec != nullptr;
                        if (ok && tryex) {
                            gchar* found = g_find_program_in_path(tryex);
                            ok           = found != nullptr;
                            g_free(found);
                        }
                        gint    argc = 0;
                        gchar** argv = nullptr;
                        if (ok && g_shell_parse_argv(exec, &argc, &argv, nullptr)) {
                            SThumbnailer t;
                            for (int k = 0; k < argc; ++k)
                                t.exec.emplace_back(argv[k]);
                            for (gsize k = 0; k < n; ++k)
                                map.try_emplace(mimes[k], t); // the first definition wins
                            g_strfreev(argv);
                        }
                        g_strfreev(mimes);
                        g_free(tryex);
                        g_free(exec);
                    }
                    g_key_file_free(kf);
                }
            }
            return map;
        }

        bool generate(const std::string& path, const std::string& kind) {
            const auto& MAP = thumbnailers();
            const auto  IT  = MAP.find(kind);
            if (IT == MAP.end())
                return false;
            const auto MD5 = md5Uri(path);
            if (MD5.empty())
                return false;
            std::error_code ec;
            std::filesystem::create_directories(ownDir(), ec);
            const auto OUT = ownDir() + "/" + MD5 + ".png";
            const auto TMP = OUT + ".tmp.png";
            char*      uri = g_filename_to_uri(path.c_str(), nullptr, nullptr);
            std::vector<std::string> args;
            for (auto a : IT->second.exec) {
                for (const auto& [TOK, VAL] : std::vector<std::pair<std::string, std::string>>{{"%i", path}, {"%u", uri ? uri : ""}, {"%o", TMP}, {"%s", "256"}, {"%%", "%"}}) {
                    for (size_t at = a.find(TOK); at != std::string::npos; at = a.find(TOK, at + VAL.size()))
                        a.replace(at, TOK.size(), VAL);
                }
                args.push_back(a);
            }
            g_free(uri);
            std::vector<char*> argv;
            for (auto& a : args)
                argv.push_back(a.data());
            argv.push_back(nullptr);
            gint status = -1;
            const bool RAN = g_spawn_sync(nullptr, argv.data(), nullptr, static_cast<GSpawnFlags>(G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL), nullptr, nullptr,
                                          nullptr, nullptr, &status, nullptr);
            // An empty or failed result is no thumbnail (2026-11-08: a
            // recording thumbnailed while still being written left 0-byte
            // PNGs that stood in for its preview for good).
            if (!RAN || !g_spawn_check_wait_status(status, nullptr) || !std::filesystem::exists(TMP, ec) || std::filesystem::file_size(TMP, ec) == 0 || ec) {
                std::filesystem::remove(TMP, ec);
                return false;
            }
            std::filesystem::rename(TMP, OUT, ec);
            return !ec;
        }

        void workerMain() {
            ::setpriority(PRIO_PROCESS, 0, 10); // it can wait; the desktop cannot
            std::unique_lock lock(g_mutex);
            while (!g_stop) {
                if (g_queue.empty()) {
                    g_cv.wait(lock, [] { return g_stop || !g_queue.empty(); });
                    continue;
                }
                // One pass over the paths waiting: each file that has settled
                // is made, one still being written goes round again, and with
                // nothing made the worker sleeps until the first could settle.
                std::chrono::steady_clock::duration nap  = SETTLE;
                bool                                made = false;
                for (size_t n = g_queue.size(); n > 0 && !g_stop && !g_queue.empty(); --n) {
                    std::string path = std::move(g_queue.front());
                    g_queue.pop_front();
                    // Never under the lock: lookup takes it on the render
                    // thread, and a stat can hang on a dead mount.
                    lock.unlock();
                    std::error_code ec;
                    const auto      MT   = std::filesystem::last_write_time(path, ec);
                    const auto      NOW  = std::chrono::steady_clock::now();
                    const auto      WALL = std::filesystem::file_time_type::clock::now();
                    lock.lock();
                    const auto IT = g_jobs.find(path);
                    if (IT == g_jobs.end())
                        continue;
                    SJob& J = IT->second;
                    if (ec) {
                        g_jobs.erase(IT); // gone: nothing to make
                        continue;
                    }
                    if (J.done && J.made == MT) {
                        J.queued = false; // this version is made already
                        continue;
                    }
                    if (J.seen != MT) {
                        J.seen  = MT;
                        J.since = NOW;
                    }
                    // Settled: seen unchanged for SETTLE -- or, for a file
                    // never made before, last written longer ago than that.
                    // Only then: a file that keeps changing while it looks
                    // old (a share whose clock runs behind) is still being
                    // written. Compared, never subtracted: a far-future mtime
                    // would overflow the difference.
                    const bool RECENT  = MT > WALL - SETTLE; // written under SETTLE ago, or dated ahead
                    const auto WATCHED = NOW - J.since;
                    if (WATCHED < SETTLE && (RECENT || J.done)) {
                        g_queue.push_back(std::move(path));
                        nap = std::min(nap, std::chrono::duration_cast<std::chrono::steady_clock::duration>(SETTLE - WATCHED));
                        if (!J.done && MT <= WALL)
                            nap = std::min(nap, std::chrono::duration_cast<std::chrono::steady_clock::duration>(SETTLE - (WALL - MT)));
                        continue;
                    }
                    const std::string KIND = J.kind;
                    lock.unlock();
                    generate(path, KIND); // a failure is not tried again until the file changes
                    ++g_gen;
                    std::function<void()> cb;
                    {
                        std::scoped_lock l(g_mutex);
                        cb = g_ready;
                    }
                    if (cb)
                        cb();
                    lock.lock();
                    made = true;
                    if (const auto JT = g_jobs.find(path); JT != g_jobs.end()) {
                        JT->second.made   = MT;
                        JT->second.done   = true;
                        JT->second.queued = JT->second.asked != MT; // asked for another version meanwhile: round again
                        if (JT->second.queued)
                            g_queue.push_back(path);
                    }
                }
                if (!made && !g_stop && !g_queue.empty())
                    g_cv.wait_for(lock, nap);
            }
        }
    }

    std::string lookup(const std::string& path, const std::string& kind) {
        const auto MD5 = md5Uri(path);
        if (MD5.empty())
            return {};
        // A thumbnail is good when it has something in it and is not older
        // than the file (a download or recording still being written when it
        // was made). A stale one of our own is made again, and shown until then.
        std::error_code ec;
        const auto      SRC  = std::filesystem::last_write_time(path, ec);
        const bool      HAVE = !ec;
        const auto      good = [&](const std::string& p) {
            std::error_code e;
            if (std::filesystem::file_size(p, e) == 0 || e)
                return false;
            const auto T = std::filesystem::last_write_time(p, e);
            return !e && (!HAVE || T >= SRC);
        };
        const std::string BASE = std::string(g_get_user_cache_dir()) + "/thumbnails/";
        for (const char* SIZE : {"x-large", "large", "xx-large", "normal"}) {
            const auto P = BASE + SIZE + "/" + MD5 + ".png";
            if (g_file_test(P.c_str(), G_FILE_TEST_EXISTS) && good(P))
                return P;
        }
        // One of ours that is out of date stays on show until the new one
        // replaces it (A FILE STILL BEING WRITTEN WAITS, above).
        const auto  OWN = ownDir() + "/" + MD5 + ".png";
        std::string meanwhile;
        if (g_file_test(OWN.c_str(), G_FILE_TEST_EXISTS)) {
            if (good(OWN))
                return OWN;
            std::error_code e;
            if (std::filesystem::file_size(OWN, e) > 0 && !e)
                meanwhile = OWN;
        }

        if (HAVE && (kind.starts_with("image/") || kind.starts_with("video/") || kind == "application/pdf")) {
            std::scoped_lock lock(g_mutex);
            if (!g_stop) {
                if (g_jobs.size() > 4096)
                    std::erase_if(g_jobs, [](const auto& KV) { return !KV.second.queued; });
                // A file that changed since its last thumbnail (or failed
                // run) is asked again: the same job, the newer version.
                auto& J = g_jobs[path];
                J.asked = SRC;
                J.kind  = kind;
                if (!(J.done && J.made == SRC) && !J.queued) {
                    J.queued = true;
                    g_queue.push_back(path);
                    g_cv.notify_one();
                }
                // No thread now (out of resources): the next lookup tries
                // again. Nothing may throw out of here, on the render thread.
                if (!g_queue.empty() && !g_worker.joinable()) {
                    try {
                        g_worker = std::thread(workerMain);
                    } catch (...) {}
                }
            }
        }
        return meanwhile;
    }

    uint64_t generation() {
        return g_gen.load();
    }

    void setReadyCallback(std::function<void()> cb) {
        std::scoped_lock lock(g_mutex);
        g_ready = std::move(cb);
    }

    void shutdown() {
        {
            std::scoped_lock lock(g_mutex);
            g_stop = true;
            g_queue.clear();
            g_ready = nullptr;
        }
        g_cv.notify_all();
        if (g_worker.joinable())
            g_worker.join();
    }
}
