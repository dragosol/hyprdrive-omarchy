#pragma once

// WHERE THE DEBUG LOGS AND THEIR SWITCH FILES LIVE, AND WHO MAY FLIP THEM.
//
// The session's own runtime directory ($XDG_RUNTIME_DIR, normally
// /run/user/<uid>, mode 0700): never /tmp. A switch file in a shared /tmp could
// be created by ANOTHER local account, turning on logs of search text, folder
// paths and window titles, and the logs inherited the umask -- readable by
// everyone at 022 (marketplace review, #11392). So:
//
//   dir()        the runtime directory only if it is a directory of ours that
//                nobody else can enter; otherwise none, and debugging is off.
//   flagOn()     a switch counts only as a regular file of ours, not writable
//                by anyone else (lstat: a symlink is not a switch).
//   openLog()    opens (creates) a log as a regular file of ours, never through
//                a symlink, and forces it to 0600 -- a log that already exists
//                with a wider mode is narrowed before anything is written.
//
// Header-only, no Hyprland headers: scripts/debug_paths_test.cpp checks it.
// docs/DEBUG.md lists every name.

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

namespace ScrollOverview::DebugPaths {

    // A runtime directory may hold switches and logs: a directory, ours, no
    // access for group or others. Pure: the test feeds it any stat.
    inline bool privateDir(const struct stat& st, uid_t uid) {
        return S_ISDIR(st.st_mode) && st.st_uid == uid && (st.st_mode & 077) == 0;
    }

    // A switch file is on: a regular file, ours, writable by nobody else.
    inline bool ownFlag(const struct stat& st, uid_t uid) {
        return S_ISREG(st.st_mode) && st.st_uid == uid && (st.st_mode & 022) == 0;
    }

    // The directory for a given XDG_RUNTIME_DIR value (null = unset), or ""
    // when it is not an absolute path to a private directory of ours.
    inline std::string dirFor(const char* runtimeDir, uid_t uid = ::getuid()) {
        struct stat st{};
        if (!runtimeDir || runtimeDir[0] != '/' || ::lstat(runtimeDir, &st) != 0 || !privateDir(st, uid))
            return {};
        std::string dir{runtimeDir};
        while (dir.size() > 1 && dir.back() == '/')
            dir.pop_back();
        return dir;
    }

    // Read once: the compositor's environment does not change under it.
    inline const std::string& dir() {
        static const std::string DIR = dirFor(std::getenv("XDG_RUNTIME_DIR"));
        return DIR;
    }

    // e.g. path("hyprdrive-compat.log") -> /run/user/1000/hyprdrive-compat.log,
    // or "" when there is no private directory (every open then fails).
    inline std::string pathIn(const std::string& dir, std::string_view name) {
        return dir.empty() ? std::string{} : dir + "/" + std::string{name};
    }
    inline std::string path(std::string_view name) {
        return pathIn(dir(), name);
    }

    inline bool flagOnIn(const std::string& dir, std::string_view name, uid_t uid = ::getuid()) {
        const auto P = pathIn(dir, name);
        struct stat st{};
        return !P.empty() && ::lstat(P.c_str(), &st) == 0 && ownFlag(st, uid);
    }
    inline bool flagOn(std::string_view name) {
        return flagOnIn(dir(), name);
    }

    // `append` false truncates (a fresh log per load). The caller fclose()s.
    inline FILE* openLogIn(const std::string& dir, std::string_view name, bool append = true) {
        const auto P = pathIn(dir, name);
        if (P.empty())
            return nullptr;
        const int FD = ::open(P.c_str(), O_WRONLY | O_CREAT | O_NOFOLLOW | O_CLOEXEC | (append ? O_APPEND : O_TRUNC), 0600);
        if (FD < 0)
            return nullptr;
        struct stat st{};
        if (::fstat(FD, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != ::getuid() || ((st.st_mode & 0777) != 0600 && ::fchmod(FD, 0600) != 0)) {
            ::close(FD);
            return nullptr;
        }
        FILE* const F = ::fdopen(FD, append ? "a" : "w");
        if (!F)
            ::close(FD);
        return F;
    }
    inline FILE* openLog(std::string_view name, bool append = true) {
        return openLogIn(dir(), name, append);
    }
}
