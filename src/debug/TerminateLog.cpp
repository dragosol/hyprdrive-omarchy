#include "TerminateLog.hpp"
#include "Demangle.hpp"

#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cxxabi.h>
#include <dlfcn.h>
#include <exception>
#include <execinfo.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <typeinfo>
#include <unistd.h>

namespace Hyprdrive::TerminateLog {
    namespace {
        std::terminate_handler g_previous = nullptr;
        bool                   g_installed = false;
        std::string            g_path;
        // The path's directories, made only when a record is written, never at
        // load: a plugin load leaves nothing on disk.
        char                   g_stateDir[PATH_MAX] = {};
        char                   g_ourDir[PATH_MAX]   = {};
        char                   g_file[PATH_MAX]     = {};

        void put(int fd, const char* text) {
            size_t left = std::strlen(text);
            while (left > 0) {
                const ssize_t N = ::write(fd, text, left);
                if (N <= 0) {
                    if (N < 0 && errno == EINTR)
                        continue;
                    return;
                }
                text += N;
                left -= static_cast<size_t>(N);
            }
        }

        void writeRecord() {
            if (!g_file[0])
                return;
            ::mkdir(g_stateDir, 0700); // EEXIST is the usual answer
            ::mkdir(g_ourDir, 0700);
            const int FD = ::open(g_file, O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_CLOEXEC, 0600);
            if (FD < 0)
                return;
            struct stat st {};
            if (::fstat(FD, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != ::getuid()) {
                ::close(FD);
                return;
            }
            if (static_cast<size_t>(st.st_size) > CAP)
                (void)::ftruncate(FD, 0); // O_APPEND: the record goes at the new end
            ::fchmod(FD, 0600);

            char         line[512];
            const time_t NOW = ::time(nullptr);
            struct tm    tm {};
            char         when[64] = "?";
            if (::gmtime_r(&NOW, &tm))
                std::strftime(when, sizeof(when), "%Y-%m-%dT%H:%M:%SZ", &tm);
            std::snprintf(line, sizeof(line), "---- std::terminate in pid %d at %s (logged by hyprdrive; the process aborts next) ----\n", static_cast<int>(::getpid()), when);
            put(FD, line);

            // The exception in flight, if any. Rethrown to read what(), inside
            // a try that catches everything: a terminate handler may do this.
            if (const std::type_info* TYPE = abi::__cxa_current_exception_type()) {
                put(FD, "exception: ");
                put(FD, demangleType(TYPE->name()).c_str());
                put(FD, "\n");
                try {
                    if (const auto EP = std::current_exception())
                        std::rethrow_exception(EP);
                } catch (const std::exception& e) {
                    put(FD, "what(): ");
                    put(FD, e.what());
                    put(FD, "\n");
                } catch (...) {
                    put(FD, "what(): (not a std::exception)\n");
                }
            } else
                put(FD, "exception: none in flight (std::terminate called directly, a joinable std::thread destroyed, or a noexcept violation)\n");

            put(FD, "backtrace:\n");
            void*     frames[64];
            const int N = ::backtrace(frames, 64);
            ::backtrace_symbols_fd(frames, N, FD);
            put(FD, "\n");
            ::fsync(FD);
            ::close(FD);
        }

        [[noreturn]] void onTerminate() {
            // Once: a second terminate from inside the first (a throw while
            // writing) goes straight on to the previous handler.
            static std::atomic<bool> s_inside{false};
            if (!s_inside.exchange(true))
                writeRecord();
            if (g_previous && g_previous != onTerminate)
                g_previous();
            std::abort(); // a handler must not return; the default would abort too
        }

        // A handler installed over ours may chain to it: this shared object
        // then stays mapped for good (RTLD_NOLOAD loads nothing; RTLD_NODELETE
        // makes Hyprland's dlclose leave it), or that chain would jump into
        // unmapped code.
        void keepLoaded() {
            Dl_info info{};
            if (::dladdr(reinterpret_cast<void*>(&onTerminate), &info) != 0 && info.dli_fname)
                (void)::dlopen(info.dli_fname, RTLD_LAZY | RTLD_NOLOAD | RTLD_NODELETE);
        }

        bool copy(char (&to)[PATH_MAX], const std::string& from) {
            if (from.size() >= PATH_MAX)
                return false;
            std::memcpy(to, from.c_str(), from.size() + 1);
            return true;
        }
    }

    std::string pathFor(const char* xdgStateHome, const char* home) {
        if (xdgStateHome && xdgStateHome[0] == '/')
            return std::string{xdgStateHome} + "/hyprdrive/terminate.log";
        if (home && home[0] == '/')
            return std::string{home} + "/.local/state/hyprdrive/terminate.log";
        return {};
    }

    void install() {
        if (g_installed)
            return;
        g_path = pathFor(std::getenv("XDG_STATE_HOME"), std::getenv("HOME"));
        g_file[0] = g_stateDir[0] = g_ourDir[0] = '\0';
        if (!g_path.empty()) {
            const auto OURS  = g_path.substr(0, g_path.rfind('/'));
            const auto STATE = OURS.substr(0, OURS.rfind('/'));
            if (!(copy(g_file, g_path) && copy(g_ourDir, OURS) && copy(g_stateDir, STATE)))
                g_file[0] = '\0';
        }
        // backtrace() loads libgcc's unwinder on its first call, which
        // allocates: done here, so the handler itself does not.
        void* warm[2];
        (void)::backtrace(warm, 2);
        g_previous  = std::set_terminate(onTerminate);
        g_installed = true;
    }

    bool uninstall() {
        if (!g_installed)
            return true;
        if (std::get_terminate() != onTerminate) {
            keepLoaded();
            return false;
        }
        std::set_terminate(g_previous);
        g_previous  = nullptr;
        g_installed = false;
        return true;
    }

    bool installed() {
        return g_installed;
    }

    const std::string& path() {
        return g_path;
    }
}
