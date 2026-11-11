#pragma once

// ProcessFacts from /proc, on the identity worker only: a read of another
// process's cmdline or environ can wait on that process's mmap_lock, which
// is why the compositor thread never makes one for identity.
//
// Every read is bounded (open, then read up to a cap: /proc files report
// size 0, so no fstat is trusted), every failure is "not readable", nothing
// throws. `procRoot` is "/proc" live and a fixture tree in tests.
//
//   * cmdline -> argv0 (Wine rewrites it to C:\...\Game.exe);
//   * environ -> the allow-listed pairs (Facts.hpp ENV_ALLOW), plus, kept
//     apart, the Wine settings a relaunch needs (launchEnv);
//   * exe -> the link; a path that does not exist on the host but does
//     through /proc/<pid>/root (a pressure-vessel or Flatpak container) is
//     given as that;
//   * stat -> comm and starttime, and the parent chain (at most MAX_HOPS,
//     parent first) with each hop's comm and cmdline.

#include "Facts.hpp"

#include <string>

namespace Hyprdrive::AppIdentity::ProcFacts {
    ProcessFacts read(pid_t pid, const std::string& procRoot = "/proc");
    // The starttime of a pid (0: gone or unreadable).
    uint64_t startOf(pid_t pid, const std::string& procRoot = "/proc");
    // What of a Wine process's environment a relaunch needs: WINE*, DXVK_*,
    // VKD3D_*, PROTON_*, ... without Wine's own bookkeeping between its
    // processes (GameApps.cpp's rule, unchanged).
    bool keepForLaunch(std::string_view key);
    // The bounded read itself: at most `cap` bytes, nullopt on failure.
    std::optional<std::string> readCapped(const std::string& path, size_t cap);
}
