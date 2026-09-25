#include "StackReader.hpp"
#include "DockDropModel.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <fcntl.h>
#include <filesystem>
#include <mutex>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>

#include <gio/gio.h>

namespace {
    using ScrollOverview::StackModel::SEntry;

    // UNLOAD MUST NOT WAIT ON A LONG JOB (review 2026-09-24): every loop below
    // checks this per entry / per file, and copies take the cancellable, so
    // shutdown() interrupts a 5 000-entry read, an Empty Folder or a cross-drive
    // copy instead of joining behind it.
    std::atomic<bool> g_abort{false};
    GCancellable*     g_cancel = nullptr;
    GCancellable*     cancellable() {
        if (!g_cancel)
            g_cancel = g_cancellable_new();
        return g_cancel;
    }
    using ScrollOverview::StackReader::SListing;
    using ScrollOverview::StackReader::STrashResult;

    std::string contentKind(const std::string& path, bool dir) {
        if (dir)
            return "inode/directory";
        gboolean uncertain = FALSE;
        char*    ct        = g_content_type_guess(path.c_str(), nullptr, 0, &uncertain); // name only: no open()
        std::string out    = ct ? ct : "application/octet-stream";
        g_free(ct);
        return out;
    }

    // EVERY candidate name, most specific first ("application-pdf" ...
    // "x-office-document" ... "text-x-generic"), '|'-joined: which of them the
    // user's icon theme actually has is a question for the compositor thread
    // (AppSearch's index), not for this worker.
    std::string iconFor(const std::string& kind) {
        if (kind == "inode/directory")
            return "folder";
        std::string out;
        GIcon*      icon = g_content_type_get_icon(kind.c_str());
        if (icon && G_IS_THEMED_ICON(icon)) {
            for (const char* const* N = g_themed_icon_get_names(G_THEMED_ICON(icon)); N && *N; ++N)
                out += (out.empty() ? "" : "|") + std::string(*N);
        }
        if (icon)
            g_object_unref(icon);
        if (char* GEN = g_content_type_get_generic_icon_name(kind.c_str()); GEN) {
            out += (out.empty() ? "" : "|") + std::string(GEN);
            g_free(GEN);
        }
        out += (out.empty() ? "" : "|") + std::string("text-x-generic");
        return out;
    }

    SListing readDirectory(const std::string& dir) {
        SListing out;
        out.ready = true;
        std::error_code ec;
        if (!std::filesystem::is_directory(dir, ec)) {
            out.missing = true;
            return out;
        }
        auto it = std::filesystem::directory_iterator{dir, std::filesystem::directory_options::skip_permission_denied, ec};
        if (ec) {
            out.missing = true;
            return out;
        }
        for (const auto& E : it) {
            if (g_abort.load())
                break;
            const auto NAME = E.path().filename().string();
            if (NAME.empty() || NAME[0] == '.')
                continue;
            if (out.entries.size() >= ScrollOverview::StackReader::MAX_ENTRIES) {
                out.truncated = true;
                break;
            }
            if (ScrollOverview::StackModel::isPartialDownload(NAME))
                ++out.downloads;
            const auto  PATH = E.path().string();
            struct statx sx{};
            SEntry       e{.path = PATH, .name = NAME};
            if (statx(AT_FDCWD, PATH.c_str(), AT_STATX_SYNC_AS_STAT, STATX_BTIME | STATX_MTIME | STATX_CTIME | STATX_TYPE, &sx) == 0) {
                e.dir   = S_ISDIR(sx.stx_mode);
                e.mtime = sx.stx_mtime.tv_sec;
                e.btime = (sx.stx_mask & STATX_BTIME) ? sx.stx_btime.tv_sec : sx.stx_ctime.tv_sec;
            }
            e.kind = contentKind(PATH, e.dir);
            e.icon = iconFor(e.kind);
            out.entries.push_back(std::move(e));
        }
        return out;
    }

    struct SJob {
        enum class EKind { List, Trash, Move } kind = EKind::List;
        std::string              dir;
        std::vector<std::string> paths;
    };

    STrashResult moveOrCopy(const std::string& dir, const std::vector<std::string>& paths) {
        STrashResult r;
        struct stat  dst{};
        if (::stat(dir.c_str(), &dst) != 0 || !S_ISDIR(dst.st_mode)) {
            r.error = "the stack's folder is gone";
            return r;
        }
        for (const auto& P : paths) {
            if (g_abort.load()) {
                r.error = "stopped";
                break;
            }
            if (ScrollOverview::DockDropModel::isInside(dir, P)) {
                r.error = "a folder cannot go inside itself";
                break;
            }
            struct stat src{};
            if (::lstat(P.c_str(), &src) != 0) {
                r.error = "not found: " + P;
                break;
            }
            const auto SLASH = P.find_last_of('/');
            const auto BASE  = SLASH == std::string::npos ? P : P.substr(SLASH + 1);
            // Already here (dropped onto its own stack): nothing to do.
            if (P.substr(0, SLASH == std::string::npos ? 0 : SLASH) == dir)
                continue;
            const auto NAME = ScrollOverview::DockDropModel::uniqueName(dir, BASE, [](const std::string& p) {
                struct stat st{};
                return ::lstat(p.c_str(), &st) == 0;
            });
            const bool SAMEFS = src.st_dev == dst.st_dev;
            if (!SAMEFS && S_ISDIR(src.st_mode)) {
                r.error = "folders are not copied between drives";
                break;
            }
            GFile*     from = g_file_new_for_path(P.c_str());
            GFile*     to   = g_file_new_for_path((dir + "/" + NAME).c_str());
            GError*    err  = nullptr;
            const bool OK   = SAMEFS ? g_file_move(from, to, G_FILE_COPY_NONE, cancellable(), nullptr, nullptr, &err) :
                                       g_file_copy(from, to, G_FILE_COPY_NONE, cancellable(), nullptr, nullptr, &err);
            g_object_unref(from);
            g_object_unref(to);
            if (!OK) {
                r.error = err && err->message ? err->message : "could not move";
                if (err)
                    g_error_free(err);
                break;
            }
            ++r.moved;
        }
        return r;
    }

    std::mutex                                g_mutex;
    std::condition_variable                   g_cv;
    std::deque<SJob>                          g_jobs;
    std::unordered_map<std::string, SListing> g_cache;     // dir -> last listing
    std::unordered_map<std::string, bool>     g_inflight;  // dir -> a read is queued
    std::unordered_map<std::string, bool>     g_dirty;     // dir -> changed while its read was in flight
    std::thread                               g_thread;
    bool                                      g_stop = false;
    std::atomic<uint64_t>                     g_generation{1};
    std::function<void()>                     g_ready;
    STrashResult                              g_trash;
    uint64_t                                  g_trashSeq = 0;

    int                                   g_inotify = -1;
    std::unordered_map<int, std::string>  g_watchDir; // wd -> dir

    void notify() {
        g_generation.fetch_add(1);
        std::function<void()> cb;
        {
            std::lock_guard l(g_mutex);
            cb = g_ready;
        }
        if (cb)
            cb();
    }

    void workerMain() {
        std::unique_lock lock(g_mutex);
        while (!g_stop) {
            g_cv.wait(lock, [] { return g_stop || !g_jobs.empty(); });
            if (g_stop)
                break;
            auto job = std::move(g_jobs.front());
            g_jobs.pop_front();
            lock.unlock();

            if (job.kind == SJob::EKind::List) {
                auto result = readDirectory(job.dir);
                lock.lock();
                g_cache[job.dir]    = std::move(result);
                g_inflight[job.dir] = false;
                // The folder changed while it was being read: this answer may
                // predate the change, so read it again (review 2026-09-24).
                if (g_dirty[job.dir]) {
                    g_dirty[job.dir]         = false;
                    g_cache[job.dir].ready   = false;
                    g_inflight[job.dir]      = true;
                    g_jobs.push_back(SJob{.kind = SJob::EKind::List, .dir = job.dir});
                }
                lock.unlock();
            } else if (job.kind == SJob::EKind::Move) {
                auto r = moveOrCopy(job.dir, job.paths);
                lock.lock();
                r.seq   = ++g_trashSeq;
                g_trash = r;
                lock.unlock();
            } else {
                STrashResult r;
                for (const auto& P : job.paths) {
                    if (g_abort.load())
                        break;
                    GFile*  f   = g_file_new_for_path(P.c_str());
                    GError* err = nullptr;
                    const bool OK = g_file_trash(f, cancellable(), &err);
                    g_object_unref(f);
                    if (!OK) {
                        r.error = err && err->message ? err->message : "could not move to Trash";
                        if (err)
                            g_error_free(err);
                        break;
                    }
                    ++r.moved;
                }
                lock.lock();
                r.seq   = ++g_trashSeq;
                g_trash = r;
                lock.unlock();
            }
            notify();
            lock.lock();
        }
    }

    void enqueue(SJob job) {
        cancellable(); // created HERE, on the compositor thread, never lazily on the worker
        if (!g_thread.joinable()) {
            g_stop   = false;
            g_thread = std::thread(workerMain);
        }
        g_jobs.push_back(std::move(job));
        g_cv.notify_all();
    }
}

namespace ScrollOverview::StackReader {

    SListing readNow(const std::string& dir) {
        return readDirectory(dir);
    }

    SListing listing(const std::string& dir) {
        std::lock_guard l(g_mutex);
        const auto      IT = g_cache.find(dir);
        if (IT != g_cache.end() && IT->second.ready)
            return IT->second;
        if (!g_inflight[dir]) {
            g_inflight[dir] = true;
            enqueue(SJob{.kind = SJob::EKind::List, .dir = dir});
        }
        return IT != g_cache.end() ? IT->second : SListing{};
    }

    SStatus status(const std::string& dir) {
        std::lock_guard l(g_mutex);
        const auto      IT = g_cache.find(dir);
        if (IT == g_cache.end() || !IT->second.ready) {
            if (!g_inflight[dir]) {
                g_inflight[dir] = true;
                enqueue(SJob{.kind = SJob::EKind::List, .dir = dir});
            }
            if (IT == g_cache.end())
                return {};
        }
        return SStatus{.ready = IT->second.ready, .missing = IT->second.missing, .count = IT->second.entries.size(), .downloads = IT->second.downloads};
    }

    void invalidate(const std::string& dir) {
        std::lock_guard l(g_mutex);
        if (g_inflight[dir])
            g_dirty[dir] = true;
        if (auto IT = g_cache.find(dir); IT != g_cache.end())
            IT->second.ready = false; // keep the old entries to draw until the new read lands
    }

    void watch(const std::vector<std::string>& dirs) {
        if (g_inotify < 0)
            g_inotify = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (g_inotify < 0)
            return;
        for (auto it = g_watchDir.begin(); it != g_watchDir.end();) {
            if (std::ranges::find(dirs, it->second) == dirs.end()) {
                inotify_rm_watch(g_inotify, it->first);
                it = g_watchDir.erase(it);
            } else
                ++it;
        }
        for (const auto& D : dirs) {
            if (std::ranges::any_of(g_watchDir, [&](const auto& kv) { return kv.second == D; }))
                continue;
            const int WD = inotify_add_watch(g_inotify, D.c_str(), IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_CLOSE_WRITE | IN_DELETE_SELF | IN_MOVE_SELF | IN_ATTRIB);
            if (WD >= 0)
                g_watchDir[WD] = D;
        }
    }

    int inotifyFd() {
        return g_inotify;
    }

    void drainInotify() {
        alignas(struct inotify_event) char buf[4096];
        bool changed = false;
        for (;;) {
            const ssize_t N = ::read(g_inotify, buf, sizeof(buf));
            if (N <= 0)
                break;
            for (ssize_t off = 0; off < N;) {
                const auto* EV = reinterpret_cast<const struct inotify_event*>(buf + off);
                if (const auto IT = g_watchDir.find(EV->wd); IT != g_watchDir.end()) {
                    invalidate(IT->second);
                    changed = true;
                    if (EV->mask & (IN_DELETE_SELF | IN_MOVE_SELF | IN_IGNORED))
                        g_watchDir.erase(IT); // the folder is gone: the cell dims, no dead watch
                }
                off += sizeof(struct inotify_event) + EV->len;
            }
        }
        if (changed)
            g_generation.fetch_add(1);
    }

    void trash(std::vector<std::string> paths) {
        std::lock_guard l(g_mutex);
        enqueue(SJob{.kind = SJob::EKind::Trash, .paths = std::move(paths)});
    }

    void moveOrCopyInto(const std::string& dir, std::vector<std::string> paths) {
        std::lock_guard l(g_mutex);
        enqueue(SJob{.kind = SJob::EKind::Move, .dir = dir, .paths = std::move(paths)});
    }

    STrashResult moveOrCopyNow(const std::string& dir, const std::vector<std::string>& paths) {
        return moveOrCopy(dir, paths);
    }

    STrashResult lastTrashResult() {
        std::lock_guard l(g_mutex);
        return g_trash;
    }

    uint64_t generation() {
        return g_generation.load();
    }

    void setReadyCallback(std::function<void()> cb) {
        std::lock_guard l(g_mutex);
        g_ready = std::move(cb);
    }

    void shutdown() {
        {
            std::lock_guard l(g_mutex);
            g_stop = true;
            g_jobs.clear();
            g_ready = nullptr;
        }
        g_abort.store(true);
        if (g_cancel)
            g_cancellable_cancel(g_cancel);
        g_cv.notify_all();
        if (g_thread.joinable())
            g_thread.join();
        g_abort.store(false); // a reload starts a fresh worker
        if (g_cancel) {
            g_object_unref(g_cancel);
            g_cancel = nullptr;
        }
        if (g_inotify >= 0) {
            ::close(g_inotify);
            g_inotify = -1;
        }
        g_watchDir.clear();
        std::lock_guard l(g_mutex);
        g_cache.clear();
        g_inflight.clear();
        g_dirty.clear();
    }
}
