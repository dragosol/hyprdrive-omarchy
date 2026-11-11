#pragma once

// What is known about one window (WindowFacts) and the process behind it
// (ProcessFacts) when AppIdentity resolves it. Plain data: the compositor
// side fills WindowFacts from Hyprland's in-memory fields only, a worker
// (next part) fills the X properties and ProcessFacts from /proc. The /proc
// text parsers are here, pure, so they are fuzzed with the rest.
//
// Units: mapTicks and startTicks are both in /proc/<pid>/stat's starttime
// clock (clock ticks since boot). A process that STARTED AFTER its window
// mapped is not that window's process -- a PID read inside a PID namespace
// (Flatpak Steam, Lutris, Bottles) names some unrelated host process -- and
// its facts are ignored.

#include "Caps.hpp"

#include <sys/types.h>

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Hyprdrive::AppIdentity {
    struct Icon {
        enum class Kind : uint8_t { None, Theme, File, Pixels };
        Kind                                         kind = Kind::None;
        std::string                                  value; // a theme name or an absolute path
        std::shared_ptr<const std::vector<uint32_t>> argb;  // Pixels: premultiplied, size x size
        int                                          size = 0;

        static Icon of(const std::string& nameOrPath) {
            if (nameOrPath.empty())
                return {};
            return {.kind = nameOrPath.starts_with('/') ? Kind::File : Kind::Theme, .value = nameOrPath};
        }
        bool empty() const {
            return kind == Kind::None;
        }
    };

    struct WindowFacts {
        std::string                cls, initialClass, instance, title;
        bool                       x11 = false, overrideRedirect = false, fixedSize = false, contentGame = false;
        bool                       hasParent    = false; // transient for / a child of another window
        bool                       hyprdriveOwn = false; // one of Hyprdrive's own surfaces (the tray container, next parts)
        uint32_t                   xid = 0;
        int                        w = 0, h = 0;
        pid_t                      pid      = 0;
        uint64_t                   mapTicks = 0; // when it mapped (0: unknown)
        std::optional<std::string> steamGameProp; // the STEAM_GAME X property
        Icon                       netWmIcon;     // _NET_WM_ICON, largest
    };

    struct ProcessFacts {
        bool        readable   = false;
        pid_t       pid        = 0;
        uint64_t    startTicks = 0;
        std::string comm, argv0, exe; // exe: /proc/<pid>/exe resolved through /proc/<pid>/root
        // The allow-listed environment (ENV_ALLOW), as at exec.
        std::vector<std::pair<std::string, std::string>> env;
        struct Hop {
            pid_t       pid = 0;
            std::string comm, cmdline; // cmdline: argv joined with spaces, capped
        };
        std::vector<Hop> chain; // parent first, at most MAX_HOPS

        std::string_view getenv(std::string_view k) const {
            for (const auto& [K, V] : env)
                if (K == k)
                    return V;
            return {};
        }
    };

    inline constexpr size_t MAX_HOPS = 12;

    // The environment worth keeping: what identifies a game, its launcher and
    // its Wine prefix. Nothing else is read (tokens, keys and paths of other
    // programs live in environments too).
    inline constexpr std::string_view ENV_ALLOW[] = {"SteamAppId",       "SteamGameId", "STEAM_COMPAT_DATA_PATH", "WINEPREFIX", "GAME_NAME", "GAME_DIRECTORY",
                                                     "LUTRIS_GAME_UUID", "STORE",       "GAMEID",                 "PROTONPATH", "FLATPAK_ID", "UMU_ID"};

    // /proc/<pid>/environ (NUL-separated) -> the allow-listed pairs. Over
    // Caps::ENVIRON is read as far as the cap.
    std::vector<std::pair<std::string, std::string>> parseEnviron(Bytes environ);
    // /proc/<pid>/cmdline -> argv[0], and argv joined with spaces (capped at
    // `cap` bytes).
    std::string argv0Of(Bytes cmdline);
    std::string joinCmdline(Bytes cmdline, size_t cap = 4096);
    // /proc/<pid>/stat -> (comm, ppid, starttime); nullopt when malformed. The
    // comm is between the FIRST '(' and the LAST ')': it may hold either.
    struct StatLine {
        std::string comm;
        pid_t       ppid      = 0;
        uint64_t    startTime = 0;
    };
    std::optional<StatLine> parseStat(Bytes stat);
}
