#pragma once

// AppIdentity: which app a window is -- its stable key, name, icon and
// family -- for Wine, Proton, Steam and Lutris windows as much as for native
// ones (AppIdentity design, (d)). This header is the PURE core: resolve() is
// a function of the facts, the file sources and the app inventory, with no
// Hyprland header and no I/O except through Sources, so host tests run it on
// fixtures. The compositor-side API (of(window), groupKey(window),
// placeable(window), the worker and its caches) is the next part.
//
// Owner rulings it encodes (2026-11-11): a game started from a launcher is
// its own app (Overwatch beside Battle.net); one identity per game for
// steam_app_default (umu); helpers (explorer.exe, crash handlers, the Steam
// overlay) group under their session's app; favourites follow the Steam app
// id, not the shortcut's name (canonicalKey).

#include "Facts.hpp"
#include "LutrisLibrary.hpp"
#include "SteamLibrary.hpp"

#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace Hyprdrive::AppIdentity {
    enum class Family : uint8_t { Native, Steam, SteamShortcut, Proton /* umu, non-Steam Proton */, Lutris, Wine, Unknown };
    std::string_view familyName(Family f);

    struct Identity {
        std::string key;         // stable: grouping, favourites, "Always open in", IconOverrides, SNI Id
        std::string displayName;
        Icon        icon;
        Family      family = Family::Unknown;
        std::string desktopId;   // the inventory entry backing it ("" if none)
        std::string launch;      // Exec, if known
        uint8_t     confidence = 0; // 0 fallback .. 100 explicit
        bool        isGame = false, placeable = true;
        std::string evidence;    // "env SteamAppId=2357570 -> appmanifest" (the debug dump)
        std::string resolver;    // which resolver answered
        std::string exePath;     // the program's unix path, when known (Get Info)
        std::string prefix;      // its Wine prefix, when it has one (Get Info)
    };

    // One desktop entry as the resolvers see it (AppSearch::SEntry's fields
    // that matter, copied so the core needs no AppSearch).
    struct InventoryEntry {
        std::string id, name, icon, wmClass, exec;
        std::string chromeStem; // a Chrome web app's class stem, when the adapter knows it
        int64_t     mtimeNs = 0;
    };
    struct InventoryView {
        std::span<const InventoryEntry> entries;
        // The strongest identity already known for a session key
        // (sessionKey()), or nullptr. Empty: no running sessions known.
        std::function<const Identity*(std::string_view sessionKey)> sessionOwner;
    };

    // The warmed indexes the resolvers read (null: that family unknown).
    struct Indexes {
        std::shared_ptr<const SteamLibrary::Snapshot>  steam;
        std::shared_ptr<const LutrisLibrary::Snapshot> lutris;
        static Indexes                                 current(); // the published ones
    };

    // The full resolution. Builds the Steam and Lutris indexes from `src`
    // (a worker's job; resolveWith() takes warmed ones), reads PE resources
    // through it. Never throws: any failure is the Fallback answer.
    Identity resolve(const WindowFacts& w, const ProcessFacts& p, const Sources& src, const InventoryView& inv);
    // The same over given indexes; `io` null means no disk at all.
    Identity resolveWith(const WindowFacts& w, const ProcessFacts& p, const Indexes& ix, const InventoryView& inv, const Sources* io);
    // The FAST tier (compositor thread): window facts only, warmed indexes
    // only, the resolvers that need neither /proc nor the disk (0-2, 7, 8).
    // Today's answer or better.
    Identity resolveFast(const WindowFacts& w, const Indexes& ix, const InventoryView& inv);

    // A stored app id (favourites, empty-desktop-apps, fullscreen-apps,
    // dock-apps, IconOverrides) as the key resolve() gives the same app: a
    // Steam shortcut's desktop id ("Overwatch") -> steam-app-<id>, read from
    // its Exec's rungameid/; everything else (desktop ids, net.lutris.*,
    // hyprdrive-wine-*, steam-*) unchanged.
    std::string canonicalKey(std::string_view storedId, const InventoryView& inv);

    // The dock's grouping key for a resolved window: the class, unchanged
    // (AppSearch::identityClass's quickshell rule included), for a native
    // window a desktop file claims -- so nothing changes for normal apps --
    // else "hd:<key>".
    std::string groupKeyOf(const Identity& id, const WindowFacts& w);

    // Does a group key / stored id name the same app as another (both
    // through canonicalKey)?
    bool sameApp(std::string_view a, std::string_view b, const InventoryView& inv);

    // What one launch's processes share: "steam:<SteamAppId>", else
    // "lutris:<LUTRIS_GAME_UUID>", else "wine:<WINEPREFIX>|<top ancestor
    // pid>". "" when the process says nothing.
    std::string sessionKey(const ProcessFacts& p);
}
