#include "AppInfo.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <deque>
#include <fcntl.h>
#include <mutex>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>

extern char** environ;

namespace AppInfo {

    namespace {

        // One line per field, "key<TAB>value". $1 = the .desktop path, $2 = its
        // Exec= line. Both arrive as ARGUMENTS; nothing is spliced into the script.
        constexpr const char* SCRIPT = R"SH(
p="$1"; ex="$2"
case "$p" in
  */flatpak/exports/*)
    id=$(basename "$p" .desktop)
    printf 'Source\tFlatpak\nPackage\t%s\n' "$id"
    v=$(flatpak info "$id" 2>/dev/null | sed -n 's/^ *Version: *//p' | head -1)
    [ -n "$v" ] && printf 'Version\t%s\n' "$v"
    exit 0 ;;
esac
case "$ex" in
  *omarchy-launch-webapp*|*--app=*)
    printf 'Source\tWeb app\n'
    u=$(printf '%s' "$ex" | grep -oE 'https?://[^ "]+' | head -1)
    [ -n "$u" ] && printf 'Address\t%s\n' "$u"
    exit 0 ;;
esac
if command -v pacman >/dev/null 2>&1; then
  pkg=$(pacman -Qqo "$p" 2>/dev/null | head -1)
  if [ -n "$pkg" ]; then
    if pacman -Qqm "$pkg" >/dev/null 2>&1; then src=AUR; else src='Arch repos'; fi
    pacman -Qi "$pkg" 2>/dev/null | awk -F' *: ' -v pkg="$pkg" -v src="$src" '
      /^Version/        { v=$2 }
      /^Installed Size/ { s=$2 }
      /^URL/            { u=$2 }
      /^Install Date/   { d=$2 }
      END { if (v!="") print "Version\t" v; print "Source\t" src; print "Package\t" pkg;
            if (s!="") print "Size\t" s; if (d!="") print "Installed\t" d; if (u!="" && u!="None") print "Homepage\t" u }'
    exit 0
  fi
elif command -v dpkg >/dev/null 2>&1; then
  pkg=$(dpkg -S "$p" 2>/dev/null | head -1 | cut -d: -f1)
  if [ -n "$pkg" ]; then
    printf 'Version\t%s\nSource\tdpkg\nPackage\t%s\n' "$(dpkg-query -W -f='${Version}' "$pkg" 2>/dev/null)" "$pkg"
    exit 0
  fi
elif command -v rpm >/dev/null 2>&1; then
  pkg=$(rpm -qf --qf '%{NAME}\t%{VERSION}-%{RELEASE}\n' "$p" 2>/dev/null)
  if [ $? -eq 0 ] && [ -n "$pkg" ]; then
    printf 'Version\t%s\nSource\trpm\nPackage\t%s\n' "${pkg#*	}" "${pkg%%	*}"
    exit 0
  fi
fi
printf 'Source\tLocal (not from a package)\n'
)SH";

        constexpr int RUN_MAX_MS = 5000;

        struct SReq {
            std::string path, exec;
        };

        std::mutex                             g_mutex;
        std::condition_variable                g_cv;
        std::deque<SReq>                       g_queue;
        std::unordered_map<std::string, SInfo> g_cache; // by .desktop path
        std::atomic<uint64_t>                  g_generation{0};
        std::thread                            g_thread;
        bool                                   g_stop = false;
        std::atomic<pid_t>                     g_child{-1};

        std::string run(const SReq& req) {
            int fds[2];
            if (pipe2(fds, O_CLOEXEC) != 0)
                return "";

            posix_spawn_file_actions_t actions;
            posix_spawn_file_actions_init(&actions);
            posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
            posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
            posix_spawn_file_actions_addclose(&actions, fds[0]);

            std::string        a0 = "sh", a1 = "-c", a2 = SCRIPT, a3 = "hyprdrive-info", a4 = req.path, a5 = req.exec;
            std::vector<char*> argv = {a0.data(), a1.data(), a2.data(), a3.data(), a4.data(), a5.data(), nullptr};

            pid_t     pid = -1;
            const int RC  = posix_spawnp(&pid, "sh", &actions, nullptr, argv.data(), environ);
            posix_spawn_file_actions_destroy(&actions);
            close(fds[1]);
            if (RC != 0) {
                close(fds[0]);
                return "";
            }
            g_child.store(pid);

            std::string out;
            const auto  DEADLINE = std::chrono::steady_clock::now() + std::chrono::milliseconds(RUN_MAX_MS);
            char        buf[2048];
            bool        timedOut = false;
            while (true) {
                const auto LEFT = std::chrono::duration_cast<std::chrono::milliseconds>(DEADLINE - std::chrono::steady_clock::now()).count();
                pollfd     pfd{fds[0], POLLIN, 0};
                if (LEFT <= 0 || poll(&pfd, 1, static_cast<int>(LEFT)) <= 0) {
                    timedOut = true;
                    break;
                }
                const ssize_t N = read(fds[0], buf, sizeof(buf));
                if (N <= 0)
                    break;
                out.append(buf, static_cast<size_t>(N));
            }
            close(fds[0]);
            if (timedOut)
                kill(pid, SIGKILL);
            int status = 0;
            waitpid(pid, &status, 0);
            g_child.store(-1);
            return out;
        }

        SInfo parse(const std::string& out) {
            SInfo info;
            info.ready = true;
            size_t at  = 0;
            while (at < out.size()) {
                const auto NL   = out.find('\n', at);
                const auto LINE = out.substr(at, NL == std::string::npos ? std::string::npos : NL - at);
                if (const auto TAB = LINE.find('\t'); TAB != std::string::npos && TAB > 0 && TAB + 1 < LINE.size())
                    info.fields.emplace_back(LINE.substr(0, TAB), LINE.substr(TAB + 1));
                if (NL == std::string::npos)
                    break;
                at = NL + 1;
            }
            return info;
        }

        void workerMain() {
            std::unique_lock lock(g_mutex);
            while (!g_stop) {
                g_cv.wait(lock, [] { return g_stop || !g_queue.empty(); });
                if (g_stop)
                    break;
                const auto REQ = g_queue.front();
                g_queue.pop_front();

                lock.unlock();
                auto info = parse(run(REQ));
                lock.lock();
                if (g_stop)
                    break;

                g_cache[REQ.path] = std::move(info);
                g_generation.fetch_add(1, std::memory_order_release);
            }
        }

    } // namespace

    SInfo lookup(const std::string& desktopPath, const std::string& exec) {
        if (desktopPath.empty())
            return SInfo{.ready = true, .fields = {}};

        std::lock_guard lock(g_mutex);
        if (const auto IT = g_cache.find(desktopPath); IT != g_cache.end())
            return IT->second;

        // Mark it in flight so a menu that asks every frame queues it once.
        g_cache[desktopPath] = SInfo{};
        g_queue.push_back(SReq{desktopPath, exec});
        if (!g_thread.joinable()) {
            g_stop   = false;
            g_thread = std::thread(workerMain);
        }
        g_cv.notify_all();
        return SInfo{};
    }

    uint64_t generation() {
        return g_generation.load(std::memory_order_acquire);
    }

    void shutdown() {
        {
            std::lock_guard lock(g_mutex);
            g_stop = true;
            g_queue.clear();
        }
        if (const pid_t CHILD = g_child.load(); CHILD > 0)
            kill(CHILD, SIGKILL);
        g_cv.notify_all();
        if (g_thread.joinable())
            g_thread.join();
        std::lock_guard lock(g_mutex);
        g_cache.clear();
    }

} // namespace AppInfo
