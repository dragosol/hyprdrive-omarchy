#pragma once

#include <cstdint>
#include <string>
#include <vector>

// OVERVIEW_CPP_SPEC §13.9 X35. The dock's non-app items: the trash, and mounted
// volumes.
//
// Dash to Dock models both as fake `ShellApp`s so its dash can hold them, and
// then spends `locations.js` undoing the consequences. Here they stay their own
// kind: everything in Dock.cpp that identifies an app keys on a WINDOW CLASS,
// and a location has none -- so a location that pretended to be an SItem would
// answer every one of those lookups with the empty string.
namespace ScrollOverview::Locations {

    struct SLocation {
        std::string uri;             // `trash:///`, or the mount root as a file:// URI
        std::string mountPath;       // mounts only: the bare path, so fstab and the mount table can be matched
        std::string name;            // "Trash", or the volume's label
        std::string icon;            // themed icon NAME, asked of the location itself
        bool        mount    = false;
        bool        canEject = false; // eject rather than unmount, per the reference
        bool        mounted  = true;  // mounts only: false is an fstab point not currently mounted
        bool        empty    = true;  // trash only: no `Empty Trash` row while it is

        bool operator==(const SLocation&) const = default;
    };

    // THE POLL, AS A NUMBER. The dock memoizes the item list it builds out of
    // items()/openWindows() and needs one cheap thing to key it on (user,
    // 2026-09-14). It moves only when a poll actually changed something --
    // a mount appearing, the trash filling, a Files window opening on a
    // location -- so a poll that finds the desktop exactly as it left it does
    // not throw the dock's cache away once a second for nothing.
    uint64_t generation();

    // In dock order: removables first, then the trash -- `Locations.getApps()`.
    //
    // Cached. The filesystem and the mount table are re-read at most every
    // POLL_MS, which is the reference's own `UPDATE_TRASH_DELAY`; the dock asks
    // this once per frame and neither answer changes at 120Hz.
    const std::vector<SLocation>& items();

    // Is a file-manager window showing this location right now? Nautilus
    // publishes the map on `org.freedesktop.FileManager1`, and the UNION of its
    // values is all the running dot needs -- which is what lets this skip
    // matching window ids to CWindows, a match with no reliable answer under
    // Wayland.
    bool openWindows(const std::string& uri);

    // Hand the URI to the desktop's default handler. Nautilus raises the window
    // it already has for a location rather than opening a second one, so this is
    // also the reference's "focus the window showing it".
    void activate(const std::string& uri);

    // The reference's one trash action, and it asks NAUTILUS to do it, with the
    // confirmation dialog: this plugin never deletes the user's files itself.
    void emptyTrash();

    // The reference's removable actions. A mount can offer both, and the menu
    // draws a row for each, so which one runs is the ROW's answer and not the
    // location's.
    void unmount(const std::string& uri, bool eject);

    // The other half of it, for a location that `show_mounts_only_mounted false`
    // put on the dock precisely because it is NOT mounted.
    void mount(const std::string& uri);
}
