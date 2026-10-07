#pragma once

// APPS GO OUT THROUGH uwsm-app (user, 2026-10-07). Under uwsm, everything the
// compositor spawns lands in Hyprland's OWN unit (wayland-wm@hyprland.desktop.
// service) -- Ghostty, Nautilus, Vesktop shared the compositor's MemoryLow /
// CPUWeight protection and charged their swap to it, which is what the stutter
// work had just given Hyprland alone. Omarchy wraps every launch the same way
// (helpers.lua o.launch, the shell's AppLibrary); `uwsm-app --` puts the app in
// its own app-*.scope under app.slice.
//
// "Open on desktop N silently" still works through it: uwsm-app EXECs the
// daemon's systemd-run --scope line in its own process, so the app keeps the
// spawner's environment -- Hyprland's HL_INITIAL_WORKSPACE_TOKEN included
// (measured nested: [workspace 5 silent] uwsm-app -- foot mapped on 5, in its
// own scope). Not under uwsm, or without uwsm-app: unchanged.

#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

namespace ScrollOverview::Launch {

    // The uwsm-app binary to wrap launches with, or "" when this compositor is
    // not running as uwsm's WM unit. Decided once: neither changes mid-session.
    inline const std::string& uwsmApp() {
        static const std::string PATH = [] {
            std::ifstream     cg("/proc/self/cgroup");
            const std::string CGROUP{std::istreambuf_iterator<char>(cg), std::istreambuf_iterator<char>()};
            if (CGROUP.find("/wayland-wm@") == std::string::npos && CGROUP.find("/wayland-wm-") == std::string::npos)
                return std::string{};
            const char* ENV = std::getenv("PATH");
            std::stringstream dirs(ENV && *ENV ? ENV : "/usr/local/bin:/usr/bin:/bin");
            for (std::string dir; std::getline(dirs, dir, ':');) {
                const auto CANDIDATE = (dir.empty() ? std::string{"."} : dir) + "/uwsm-app";
                if (access(CANDIDATE.c_str(), X_OK) == 0)
                    return CANDIDATE;
            }
            return std::string{};
        }();
        return PATH;
    }

    // binary + args -> uwsm-app -- binary args (or untouched).
    inline void wrap(std::string& binary, std::vector<std::string>& args) {
        const auto& APP = uwsmApp();
        if (APP.empty())
            return;
        args.insert(args.begin(), {"--", binary});
        binary = APP;
    }

    // The same, for a shell command line ("[rule] cmd args..." keeps its rule).
    inline std::string prefix() {
        const auto& APP = uwsmApp();
        return APP.empty() ? std::string{} : APP + " -- ";
    }
}
