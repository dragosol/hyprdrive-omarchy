#pragma once

// Lutris's installed games as one in-memory index, from pga.db (native
// ~/.local/share/lutris and the Flatpak's ~/.var/app/net.lutris.Lutris/data/
// lutris) and each game's yml. Built by rebuild() (disk; a worker's job),
// read by the resolvers.
//
// A game's yml is games/<configpath>.yml, except when it is not: on fedora
// pga.db named hogwarts-legacy-1774237481 while the file on disk was
// hogwarts-legacy-1774237099.yml (AppIdentity design, (a)). So a missing
// configpath falls back to the one games/<slug>-<digits>.yml that has an
// exe, then ~/.config/lutris/games/ (older Lutris). A game whose exe stays
// unknown is still indexed, flagged, and the resolver asks more of a window
// before it gives it that game.

#include "Sources.hpp"

#include <memory>
#include <string>
#include <vector>

namespace Hyprdrive::AppIdentity::LutrisLibrary {
    struct Game {
        int64_t     id = 0;
        std::string slug, name, runner, directory;
        std::string exe;     // absolute, "" when unknown
        std::string folder;  // the exe's folder: a window from under it IS this game
        std::string prefix;  // the Wine prefix, if any
        std::string appId;   // net.lutris.<slug>-<id>
        std::string icon;    // theme name or absolute path
        std::string ymlFrom; // "configpath", "slug", "config", "" (none)
        bool        flatpak = false;
    };
    struct Snapshot {
        std::vector<Game> games;
        const Game*       byName(std::string_view name) const;
        const Game*       byId(int64_t id) const;
        // The game whose exe is this one (path, else basename), or nullptr.
        const Game* byExe(std::string_view exePath) const;
    };

    std::shared_ptr<const Snapshot> rebuild(const Sources& src);
    std::shared_ptr<const Snapshot> current();
    void                            publish(std::shared_ptr<const Snapshot> s);
}
