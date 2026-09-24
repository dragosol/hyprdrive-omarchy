#pragma once

// GET INFO for a dock app (user, 2026-09-24: "get info should come from a more
// universal place"). The menu's old Show Details ran gnome-software, which an
// Omarchy install does not have -- and neither do most of the desktops a public
// plugin will land on. What every one of them does have is the app's .desktop
// file and a package manager that owns it, so that is where the answers come
// from:
//
//   Flatpak   the export path, and `flatpak info` for the version
//   Web app   Omarchy's own launcher (omarchy-launch-webapp / --app=), and its URL
//   pacman    owner, repos-vs-AUR, version, size, homepage, install date
//   dpkg/rpm  owner and version, for the distros that are not Arch
//   neither   "Local (not from a package)"
//
// Package queries take tens of milliseconds, which the compositor thread cannot
// spend, so they run on one worker; `generation()` moves when an answer lands.

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace AppInfo {

    struct SInfo {
        bool                                             ready = false;
        std::vector<std::pair<std::string, std::string>> fields; // ("Version", "1.2.3"), in display order
    };

    // The answer for the app whose .desktop file is `desktopPath`; starts the
    // lookup if there is none yet. Never blocks.
    SInfo lookup(const std::string& desktopPath, const std::string& exec);

    uint64_t generation();

    void shutdown();

} // namespace AppInfo
