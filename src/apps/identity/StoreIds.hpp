#pragma once

// The dock's stored app ids -- favourites, empty-desktop-apps,
// fullscreen-apps, dock-apps, IconOverrides -- compared through
// AppIdentity::canonicalKey on BOTH sides (AppIdentity design, (d)
// "Migration of stored ids"): a favourite saved as a Steam shortcut's
// desktop id ("Overwatch") is the same app as the running game
// (steam-app-2357570) and as a later canonical entry. Nothing is rewritten
// until the user edits: a toggle then writes the canonical key and removes
// every alias of it, so a list never holds one app twice. Pure: `canon` is
// canonicalKey over the live inventory in the plugin, a table in the tests.

#include <algorithm>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace Hyprdrive::AppIdentity::StoreIds {
    using Canon = std::function<std::string(std::string_view)>;

    inline bool same(std::string_view a, std::string_view b, const Canon& canon) {
        if (a.empty() || b.empty())
            return false;
        return a == b || (canon && canon(a) == canon(b));
    }

    inline bool contains(const std::vector<std::string>& ids, std::string_view id, const Canon& canon) {
        return std::ranges::any_of(ids, [&](const std::string& s) { return same(s, id, canon); });
    }

    // The first stored id that is this app, or nullptr.
    inline const std::string* find(const std::vector<std::string>& ids, std::string_view id, const Canon& canon) {
        const auto IT = std::ranges::find_if(ids, [&](const std::string& s) { return same(s, id, canon); });
        return IT == ids.end() ? nullptr : &*IT;
    }

    // Every alias of the app removed; returns how many.
    inline size_t erase(std::vector<std::string>& ids, std::string_view id, const Canon& canon) {
        return std::erase_if(ids, [&](const std::string& s) { return same(s, id, canon); });
    }

    // On: off, every alias gone. Off: on, as its canonical key, at the end.
    inline std::vector<std::string> toggled(std::vector<std::string> ids, std::string_view id, const Canon& canon) {
        if (id.empty())
            return ids;
        if (erase(ids, id, canon) == 0)
            ids.emplace_back(canon ? canon(id) : std::string{id});
        return ids;
    }
}
