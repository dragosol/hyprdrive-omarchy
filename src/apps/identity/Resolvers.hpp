#pragma once

// The resolver table behind AppIdentity::resolve() (AppIdentity design, (d)).
// Each resolver looks at the facts and answers an Identity or nothing; they
// run in order and the first answer at or above WIN wins. A lower answer only
// fills in a name, an icon or a desktop entry the winner lacks (the
// placeability ones, 0, only ever set placeable = false and let the rest
// name the window).
//
//   0  HyprdriveOwn, WineSystrayFallback, WinePopup   -> not placeable
//   1  DesktopStrict   StartupWMClass / id / id tail / Name, never for an
//                      ambiguous class; a Wine entry also needs the window's
//                      WINEPREFIX
//   2  SteamApp        env SteamGameId / SteamAppId, STEAM_COMPAT_DATA_PATH,
//                      the `reaper ... AppId=<id>` hop, class steam_app_<digits>
//   3  LutrisGame      GAME_NAME (+ the exe under the game's folder, or its
//                      basename, or GAME_DIRECTORY when the exe is unknown)
//   4  HelperOfSession a helper (table below) joins its session's app
//   5  WineExe         the program itself: PE name and icon, hyprdrive-wine-<exe>
//   6  X11Props        _NET_WM_ICON; a short title for an .exe class
//   7  DesktopLoose    AppSearch's `_app` / `-app` pass
//   8  Fallback        today's class answer exactly
//
// Pure and table-driven: the helper list and the ambiguous classes are data.

#include "AppIdentity.hpp"

#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace Hyprdrive::AppIdentity::Resolvers {
    inline constexpr uint8_t WIN = 60;

    struct Ctx {
        const WindowFacts&   w;
        const ProcessFacts&  p;      // empty (readable = false) when unusable
        const Indexes&       ix;
        const InventoryView& inv;
        const Sources*       io = nullptr; // null: no disk
        std::string          cls;          // the class, lowercase
        std::string          want;         // what desktop entries are matched against: cls, or a quickshell panel's title
        std::string          exe;          // the program's basename, lowercase ("battle.net.exe")
        std::string          exePath;      // its unix path when known
        std::string          prefix;       // WINEPREFIX, no trailing slash
        bool                 wine = false; // a Wine process (prefix, or a C:\ argv0, or an .exe class)
        std::string          pidNote;      // why the process facts were dropped ("" if they were not)
    };

    struct Resolver {
        std::string_view name;
        bool             fast; // runs in the fast tier (no /proc, no disk)
        std::optional<Identity> (*run)(const Ctx&);
    };
    std::span<const Resolver> table();

    // ---- the data and the helpers, public for the tests ---------------------
    // Classes no desktop entry may claim by class alone.
    bool isAmbiguousClass(std::string_view lowerCls);
    // A helper process of some app (explorer.exe, crash handlers, overlays,
    // steamwebhelper): its window belongs to its session's app.
    bool isHelper(std::string_view lowerExeOrClass);
    // Wine's own programs and installers running from a temp folder.
    bool isWineSystemPath(std::string_view lowerUnixPath);
    // "C:\\Games\\a.exe" in `prefix` -> "<prefix>/drive_c/Games/a.exe"; other
    // drive letters through dosdevices; "" when it is not a Windows path.
    std::string wineUnixPath(std::string_view winPath, std::string_view prefix);
    // The WINEPREFIX an Exec= sets ("env \"WINEPREFIX=/p\" wine ..."), or "".
    std::string execWinePrefix(std::string_view exec);
    // FNV-1a, 8 hex digits: the @hash8 of a colliding Wine key.
    std::string hash8(std::string_view s);
    // Does a glob with '*' match (ASCII, already lowercase)?
    bool glob(std::string_view pattern, std::string_view s);

    Ctx makeCtx(const WindowFacts& w, const ProcessFacts& p, const Indexes& ix, const InventoryView& inv, const Sources* io, const ProcessFacts& empty);
}
