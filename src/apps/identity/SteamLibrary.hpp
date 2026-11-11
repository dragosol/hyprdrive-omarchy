#pragma once

// Steam's games, as one in-memory index (a Snapshot): every library's
// appmanifest_<id>.acf (name, install dir) and every user's non-Steam
// shortcuts, from the native, ~/.steam and Flatpak installs. The index is
// built by rebuild(), the one function here that touches the disk -- a
// library can sit on a removable disk where a read blocks for seconds, so it
// belongs on a worker -- and read through current() by anything that must not
// wait.
//
// THE ICON of a Steam game, best first (AppIdentity design, (b)):
//   1. the theme name steam_icon_<id>, when Steam installed one into hicolor
//      (it does when a desktop shortcut was made);
//   2. Steam's own steam/games/<clienticon>.ico, up to 512 px, written out as
//      a PNG (no icon loader here reads .ico). Its <clienticon> hash is the
//      name of the top-level appcache/librarycache/<id>/<sha1>.jpg, so
//      appinfo.vdf is never parsed;
//   3. that .jpg itself (32x32), as the last resort;
//   4. else steam_icon_<id> anyway: what the dock showed before this module.

#include "Acf.hpp"
#include "Sources.hpp"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace Hyprdrive::AppIdentity::SteamLibrary {
    struct App {
        uint32_t    appid = 0;
        std::string name, installdir, library; // library: the steamapps folder
        std::string icon;                      // a theme name or an absolute path
        std::string iconFrom;                  // "theme", "ico", "jpg", "default"
    };
    struct Shortcut {
        uint32_t    id32 = 0;
        std::string name, exe, startDir, icon, launchOptions;
    };
    struct Snapshot {
        std::unordered_map<uint32_t, App>      apps;
        std::unordered_map<uint32_t, Shortcut> shortcuts;
        std::vector<std::string>               roots, libraries; // what was looked at
        int64_t                                builtAtNs = 0;    // steady clock

        const App*      app(uint32_t id) const;
        const Shortcut* shortcut(uint32_t id32) const;
        // The id's display name ("" if unknown) and icon (rule above).
        std::string nameFor(Steam::GameId id) const;
        std::string iconFor(Steam::GameId id) const;
    };

    // Reads everything (bounded: Caps::VDF per manifest, Caps::SHORTCUTS per
    // shortcuts.vdf, Caps::DIR_ENTRIES per folder). Writes at most one PNG per
    // game whose only icon is Steam's .ico (Sources::storeIcon, kept). Never
    // throws; a library that cannot be read is simply absent.
    std::shared_ptr<const Snapshot> rebuild(const Sources& src);

    // The published index (null until the first publish). Thread-safe.
    std::shared_ptr<const Snapshot> current();
    void                            publish(std::shared_ptr<const Snapshot> s);

    // STOPGAP until the identity worker exists (next part): the old
    // AppSearch timing. The index is rebuilt here, synchronously, when there is
    // none yet, or when `want` is not in it and the last build is at least
    // 30 s old (a game mid-install shows up within 30 s, a miss costs one
    // rebuild per 30 s at most). Returns current(). `nowNs` is the steady
    // clock, injectable for tests (0 = now).
    std::shared_ptr<const Snapshot> ensure(const Sources& src, Steam::GameId want, int64_t nowNs = 0);
    inline constexpr int64_t        NEGATIVE_TTL_NS = 30'000'000'000LL;

    // The roots Steam keeps its files under (native, the ~/.steam link,
    // Flatpak), for a home.
    std::vector<std::string> roots(const Env& env);

    // Which of n desktop entries is the Steam shortcut for `digits` (the
    // class's steam_app_<digits>): the one whose Exec= launches rungameid/
    // <digits>, the NEWEST file when there are several -- Steam leaves the old
    // shortcut behind when a game is renamed ("Overwatch 2" next to
    // "Overwatch", same id). An entry with no mtime (0) never beats one found
    // before it. npos when none.
    template <typename ExecAt, typename MtimeAt>
    size_t newestShortcut(size_t n, std::string_view digits, ExecAt execAt, MtimeAt mtimeAt) {
        size_t  found  = std::string::npos;
        int64_t newest = 0;
        if (digits.empty())
            return found;
        for (size_t i = 0; i < n; ++i) {
            if (!Steam::execLaunches(execAt(i), digits))
                continue;
            const int64_t WHEN = mtimeAt(i);
            if (found == std::string::npos || (WHEN && WHEN > newest)) {
                found  = i;
                newest = WHEN ? WHEN : newest;
            }
        }
        return found;
    }
}
