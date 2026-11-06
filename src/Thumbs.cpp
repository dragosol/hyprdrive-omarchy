// See Thumbs.hpp.
#include "Thumbs.hpp"

#include <glib.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <sys/resource.h>

namespace ScrollOverview::Thumbs {
    namespace {
        struct SThumbnailer {
            std::vector<std::string> exec; // argv with %i %u %o %s
        };

        std::mutex                      g_mutex;
        std::condition_variable         g_cv;
        std::deque<std::pair<std::string, std::string>> g_queue; // path, kind
        std::unordered_set<std::string> g_queued;                // paths queued, running, or failed
        std::thread                     g_worker;
        bool                            g_stop = false;
        std::atomic<uint64_t>           g_gen{1};
        std::function<void()>           g_ready;

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
            if (!RAN || !std::filesystem::exists(TMP, ec)) {
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
                g_cv.wait(lock, [] { return g_stop || !g_queue.empty(); });
                if (g_stop)
                    break;
                auto job = std::move(g_queue.front());
                g_queue.pop_front();
                lock.unlock();
                generate(job.first, job.second); // a failure stays in g_queued: never retried this session
                ++g_gen;
                std::function<void()> cb;
                {
                    std::scoped_lock l(g_mutex);
                    cb = g_ready;
                }
                if (cb)
                    cb();
                lock.lock();
            }
        }
    }

    std::string lookup(const std::string& path, const std::string& kind) {
        const auto MD5 = md5Uri(path);
        if (MD5.empty())
            return {};
        const std::string BASE = std::string(g_get_user_cache_dir()) + "/thumbnails/";
        for (const char* SIZE : {"x-large", "large", "xx-large", "normal"}) {
            const auto P = BASE + SIZE + "/" + MD5 + ".png";
            if (g_file_test(P.c_str(), G_FILE_TEST_EXISTS))
                return P;
        }
        const auto OWN = ownDir() + "/" + MD5 + ".png";
        if (g_file_test(OWN.c_str(), G_FILE_TEST_EXISTS))
            return OWN;

        if (kind.starts_with("image/") || kind.starts_with("video/") || kind == "application/pdf") {
            std::scoped_lock lock(g_mutex);
            if (!g_stop && g_queued.insert(path).second) {
                g_queue.emplace_back(path, kind);
                if (!g_worker.joinable())
                    g_worker = std::thread(workerMain);
                g_cv.notify_one();
            }
        }
        return {};
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
