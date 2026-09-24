#pragma once

// Folder reads and trash jobs OFF the compositor thread, and inotify for change
// (dock stacks A). Hyprland never iterates GLib's main context, so a
// GFileMonitor would never fire; the inotify fd rides the compositor's own
// event loop instead. Host-tested through readNow().

#include "StackModel.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ScrollOverview::StackReader {
    constexpr size_t MAX_ENTRIES = 5000;

    struct SListing {
        bool                             ready     = false;
        bool                             truncated = false;
        bool                             missing   = false;
        std::vector<StackModel::SEntry> entries;
    };

    SListing listing(const std::string& dir);            // never blocks; starts a read when stale
    void     invalidate(const std::string& dir);         // the next listing() re-reads
    void     watch(const std::vector<std::string>& dirs); // inotify on exactly these dirs
    int      inotifyFd();                                // for wl_event_loop_add_fd; -1 if none
    void     drainInotify();                             // call when inotifyFd() is readable

    struct STrashResult {
        uint64_t    seq   = 0;
        size_t      moved = 0;
        std::string error;
    };
    void         trash(std::vector<std::string> paths); // on the worker; result in lastTrashResult()
    STrashResult lastTrashResult();

    uint64_t generation();                                // moves when a listing or a trash job lands
    void     setReadyCallback(std::function<void()> cb);  // called on the WORKER thread
    void     shutdown();                                  // join the worker, close inotify

    SListing readNow(const std::string& dir); // host-test hook: the worker's read, synchronously
}
