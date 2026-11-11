#pragma once

// THE FREEDESKTOP TRASH ON EVERY DISK, BY HAND (Locations.cpp: the dock's
// Trash, its count, and Empty Trash when there is no Nautilus to ask). The
// parts that read /proc/mounts and touch the trash directories live here,
// apart from the GLib and Hyprland code, so tests/unit/trash_dirs_test.cpp can run
// them against real directories on the host. Pure POSIX.
//
// A DISK IS NOT TRUSTED (review 2026-11-09). Its trash is whatever the disk
// says it is, and a disk can say `.Trash-1000/files -> /home/u`: the old walk
// followed that link, and Empty Trash deleted every top-level entry of the
// home folder. A volume's trash now counts only when it and its files/ are
// real directories -- never a symlink -- owned by this user (the shared
// $top/.Trash must also have its sticky bit, as the spec says), and emptying
// goes through descriptors opened O_NOFOLLOW, so a link is removed and what it
// points to never is.

#include <cerrno>
#include <cstddef>
#include <dirent.h>
#include <fcntl.h>
#include <initializer_list>
#include <stop_token>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

namespace Hyprdrive::TrashDirs {

    // /proc/mounts writes a space, tab, newline or backslash inside a field as
    // \ooo. Decoded in ONE left-to-right pass, and only where three octal
    // digits follow (review 2026-11-09). The old loop searched from the start
    // again after every replacement, so the backslash it had just produced was
    // read as an escape: a drive labelled `foo\bar` (`foo\134bar`) became
    // std::stoi("bar", 8), which threw on the render thread and took the
    // compositor down within a second of the mount, and again at every login
    // while the drive stayed mounted. `a\134123` was decoded twice, to "aS".
    inline std::string unescapeMountField(std::string_view field) {
        const auto  OCTAL = [](char c) { return c >= '0' && c <= '7'; };
        std::string out;
        out.reserve(field.size());
        for (size_t i = 0; i < field.size(); ++i) {
            // \0oo..\3oo: one byte. Anything else is copied as it stands.
            if (field[i] == '\\' && i + 3 < field.size() && field[i + 1] <= '3' && OCTAL(field[i + 1]) && OCTAL(field[i + 2]) && OCTAL(field[i + 3])) {
                out += static_cast<char>((field[i + 1] - '0') * 64 + (field[i + 2] - '0') * 8 + (field[i + 3] - '0'));
                i += 3;
            } else
                out += field[i];
        }
        return out;
    }

    struct SMount {
        std::string device, dir, type;
    };

    // The first three fields of every line of a /proc/mounts table, decoded.
    // Split on the line, not scanned into fixed buffers: a long path cannot
    // spill into the next field.
    inline std::vector<SMount> parseMounts(std::string_view table) {
        std::vector<SMount> out;
        while (!table.empty()) {
            const size_t     EOL  = table.find('\n');
            std::string_view line = table.substr(0, EOL);
            table.remove_prefix(EOL == std::string_view::npos ? table.size() : EOL + 1);

            std::string_view fields[3];
            size_t           n = 0;
            while (n < 3 && !line.empty()) {
                const size_t SP = line.find(' ');
                fields[n++]     = line.substr(0, SP);
                line.remove_prefix(SP == std::string_view::npos ? line.size() : SP + 1);
            }
            if (n == 3 && !fields[0].empty() && !fields[1].empty())
                out.push_back({unescapeMountField(fields[0]), unescapeMountField(fields[1]), unescapeMountField(fields[2])});
        }
        return out;
    }

    // `path` ITSELF (lstat: a symlink is a symlink, never its target) is a
    // directory owned by `uid` -- or, `shared`, any owner's with the sticky bit,
    // which is what the spec asks of $top/.Trash.
    inline bool realDir(const std::string& path, uid_t uid, bool shared = false) {
        struct stat st {};
        if (::lstat(path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
            return false;
        return shared ? (st.st_mode & S_ISVTX) != 0 : st.st_uid == uid;
    }

    // This user's trashes on the volume mounted at `top`: $top/.Trash/$uid, when
    // $top/.Trash is a real sticky directory, and $top/.Trash-$uid. Each only
    // when it, and its files/, pass realDir.
    inline std::vector<std::string> volumeTrashes(std::string top, uid_t uid) {
        if (top == "/")
            top.clear();
        const std::string        UID = std::to_string(uid);
        std::vector<std::string> out;
        if (realDir(top + "/.Trash", uid, true))
            if (const auto T = top + "/.Trash/" + UID; realDir(T, uid) && realDir(T + "/files", uid))
                out.push_back(T);
        if (const auto T = top + "/.Trash-" + UID; realDir(T, uid) && realDir(T + "/files", uid))
            out.push_back(T);
        return out;
    }

    // How deep a trashed folder is followed down. Below this it is left where
    // it is: every level holds a descriptor and a directory buffer, and a
    // stranger's disk can nest folders as deep as it likes.
    inline constexpr int MAX_DEPTH = 256;

    // Everything inside the directory open on `fd`, which this closes. An entry
    // that is not a directory is unlinked -- a symlink itself, never its target.
    // A directory is entered only as itself (O_NOFOLLOW) and only on the same
    // filesystem (something mounted inside the trash is not the trash), then
    // emptied and removed. A stop requested on `stop` (the plugin unloading)
    // is seen between two entries: what is left stays in the trash.
    inline void removeContents(int fd, dev_t dev, int depth, const std::stop_token& stop = {}) {
        DIR* d = ::fdopendir(fd);
        if (!d) {
            ::close(fd);
            return;
        }
        const int DFD = ::dirfd(d);
        while (const dirent* e = ::readdir(d)) {
            if (stop.stop_requested())
                break;
            const std::string_view NAME = e->d_name;
            if (NAME == "." || NAME == "..")
                continue;
            // Linux answers EISDIR for a directory; anything else that fails
            // (not ours, read-only) is left as it is.
            if (::unlinkat(DFD, e->d_name, 0) == 0 || errno != EISDIR)
                continue;
            if (depth < MAX_DEPTH) {
                const int   SUB = ::openat(DFD, e->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
                struct stat st {};
                if (SUB >= 0 && ::fstat(SUB, &st) == 0 && st.st_dev == dev)
                    removeContents(SUB, dev, depth + 1, stop);
                else if (SUB >= 0)
                    ::close(SUB);
            }
            ::unlinkat(DFD, e->d_name, AT_REMOVEDIR);
        }
        ::closedir(d);
    }

    // Empty one trash: everything inside its files/, info/ and expunged/. A
    // volume's trash (`home` false) is opened O_NOFOLLOW at each step from the
    // trash directory down, and it and each of the three must be this user's.
    // The home trash is the user's own and is opened as a path, as GIO does, so
    // a Trash the user moved to another disk behind a symlink still empties.
    // Inside either, no link is ever followed. `stop` as in removeContents.
    inline void empty(const std::string& trash, bool home, uid_t uid, const std::stop_token& stop = {}) {
        const int NOFOLLOW = home ? 0 : O_NOFOLLOW;
        const int T        = ::open(trash.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | NOFOLLOW);
        if (T < 0)
            return;
        struct stat st {};
        if (::fstat(T, &st) != 0 || (!home && st.st_uid != uid)) {
            ::close(T);
            return;
        }
        for (const char* SUB : {"files", "info", "expunged"}) {
            const int   FD = ::openat(T, SUB, O_RDONLY | O_DIRECTORY | O_CLOEXEC | NOFOLLOW);
            struct stat sub {};
            if (FD >= 0 && ::fstat(FD, &sub) == 0 && (home || sub.st_uid == uid))
                removeContents(FD, sub.st_dev, 0, stop);
            else if (FD >= 0)
                ::close(FD);
        }
        ::close(T);
    }
}
