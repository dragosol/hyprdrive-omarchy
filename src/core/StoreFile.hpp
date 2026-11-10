#pragma once

// A STORE FILE IS REPLACED WHOLE OR NOT AT ALL (review 2026-11-09). The
// stacks store wrote its temp file through an ofstream and renamed it over
// the real one without asking whether the text had reached the disk. The text
// sat in the stream's buffer, so the write that failed on a full disk was the
// one in the destructor, unchecked -- and an empty temp file was renamed over
// `stacks`. Every pinned stack was gone at the next login.
//
// Here every step is checked (open, each write, close) and a temp file that
// did not take all of the text is deleted, leaving the old file as it was.
//
// NO FSYNC, on purpose: this runs on the compositor thread, and an fsync
// there is a stall -- 2.4 ms median, 9 ms worst, for this file on an idle
// btrfs (measured 2026-11-09), and far more under writeback load. The
// rename is what keeps a crash from leaving half a file, and ext4
// (auto_da_alloc) and btrfs both start writing a file's data when it is
// renamed over an existing one.
//
// Pure POSIX: tests/unit/store_file_test.cpp runs it on the host.

#include <cerrno>
#include <fcntl.h>
#include <string>
#include <string_view>
#include <unistd.h>

namespace ScrollOverview::StoreFile {

    // `path` holds exactly `text` afterwards, or is untouched. 0 on success,
    // else the errno of the step that failed.
    inline int replace(const std::string& path, std::string_view text) {
        const std::string TMP = path + ".tmp";
        const int         FD  = ::open(TMP.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
        if (FD < 0)
            return errno;
        int error = 0;
        for (size_t at = 0; !error && at < text.size();) {
            const ssize_t N = ::write(FD, text.data() + at, text.size() - at);
            if (N > 0)
                at += static_cast<size_t>(N);
            else if (N < 0 && errno != EINTR)
                error = errno;
            else if (N == 0)
                error = EIO;
        }
        if (::close(FD) != 0 && !error)
            error = errno;
        if (!error && ::rename(TMP.c_str(), path.c_str()) != 0)
            error = errno;
        if (error)
            ::unlink(TMP.c_str());
        return error;
    }
}
