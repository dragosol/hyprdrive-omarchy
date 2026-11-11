#pragma once

// GAMES AND WINE PROGRAMS AS DOCK APPS (user, 2026-11-04: "ensure lutris apps
// are named and work properly via dock, and any apps that I do launch through
// lutris apps also have proper context menu entries (e.g. add to favorites,
// always load in another workspace, etc) and have proper icons and app names").
//
// A Wine window's class is its exe ("battle.net.exe",
// "hogwartslegacy-win64-shipping.exe"); nothing in a desktop file names it, so
// the dock showed an unnamed, iconless app that could not be pinned. Any
// distro, native or Flatpak Lutris:
//
//   * LUTRIS GAMES. Lutris's own library (pga.db + games/*.yml) names every
//     installed game. Each one is an app: its Lutris desktop entry when Lutris
//     made one (net.lutris.<slug>-<id>), else one made here with the same id,
//     Lutris's icon (lutris_<slug>) and `lutris lutris:rungameid/<id>`.
//   * WHICH GAME A WINDOW IS. Lutris stamps every process it starts, children
//     included, with GAME_NAME (/proc/<pid>/environ). A window whose program
//     lives in that game's folder IS the game (a Unreal game's
//     *-Win64-Shipping.exe included) and joins its app.
//   * PROGRAMS A GAME STARTED -- Overwatch from Battle.net, a game from the
//     Epic store -- and any other Wine program nothing describes: an app of
//     their own, made from the window's process: the name and icon from the
//     .exe itself (its version info and icon resources), launched again the way
//     it ran (the same Wine, prefix and Wine settings, in its own folder).
//     Remembered across sessions ($XDG_STATE_HOME/hyprdrive/wine-apps.tsv).
//
// AppSearch asks through two hooks (AppSearch.hpp): extra inventory entries,
// and a window class -> app id map.
namespace Hyprdrive::GameApps {
    void init();
    void shutdown();
}
