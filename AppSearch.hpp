#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// OVERVIEW_CPP_SPEC §13.2 F3. The app half of the GNOME-like search.
//
// This is a result model that lives ALONGSIDE shouldShowOverviewWindow's
// membership gate, never inside it: the gate decides which windows exist for
// every overview path at once, and app results are a second, ordered list that
// the grid does not represent. See RICE_PROGRESS.md "F3-scope" §1.
namespace AppSearch {
    struct SEntry {
        std::string id;      // desktop id, subdirs flattened with '-'
        std::string name;    // Name=
        std::string generic; // GenericName=
        std::string comment; // Comment=
        std::string keywords;
        std::string exec;    // raw Exec=, field codes not yet stripped
        std::string icon;
        std::string wmClass; // StartupWMClass=, measured absent on 185/186 entries
        bool        terminal = false;

        std::string path;     // the .desktop file itself, so actions can be read on demand
        std::string haystack; // lowercased name/generic/comment/keywords/id, built once
    };

    // OVERVIEW_CPP_SPEC §13.9 X28. One `[Desktop Action <id>]` group.
    struct SAction {
        std::string name; // Name=
        std::string exec; // raw Exec=, field codes not yet stripped
    };

    // Scans lazily on first call. ~19 visible apps in the test VM, so this is a
    // plain in-memory vector: no index, no cache, no invalidation story.
    const std::vector<SEntry>& inventory();
    void                       refresh();

    // NEW APPS AND NEW ICONS, WITHOUT A RELOAD (user, 2026-09-19: "some icons
    // including ghostty do not have a dock icon"). The inventory and the icon
    // index were each built exactly once per plugin load, so anything installed
    // afterwards -- Ghostty that very afternoon -- had no entry and no icon until
    // the next login. Call once per frame: it looks at the application and
    // icon-theme directories at most every couple of seconds (a handful of
    // stat()s), rescans the desktop entries when one of them changed, and
    // rebuilds the icon index OFF the render thread when a theme did.
    void                       pollForChanges();

    // Moves whenever the icon index is rebuilt. Anything caching what an icon
    // NAME resolved to -- textures above all -- keys on this.
    uint64_t                   iconIndexGeneration();

    // Moves every time the inventory is (re)scanned. Anything that caches a
    // result built out of `inventory()` -- the search rows, which hold raw
    // SEntry pointers into it -- keys on this, so a refresh cannot leave a
    // dangling pointer behind in a memo.
    uint64_t                   inventoryGeneration();

    // Ranked best-first. Empty query yields nothing -- an unfiltered overview
    // shows windows, not a list of every installed app.
    std::vector<const SEntry*> search(const std::string& query, size_t limit);

    // OVERVIEW_CPP_SPEC §13.12 X51/X52. Build the `stem -> path` icon index up
    // front, so the first search does not pay for it. Idempotent, and a no-op
    // once built; call it wherever a stall is invisible (plugin load), never
    // from a frame that is already late.
    void warmIconIndex();

    // Y13. Build the icon index on a worker instead of inline. warmIconIndex()
    // stays the blocking entry point every reader already uses; this is only
    // about WHEN the work happens, not what it produces.
    void warmIconIndexAsync();
    void shutdownIconIndex();

    // QUICKSHELL PANELS SHARE ONE APP ID (user, 2026-09-19: omamail "isn't
    // picked up by my dock as an actual app"). Quickshell runs every panel
    // plugin inside a SINGLE process, so each panel toplevel carries the same
    // Wayland app_id -- org.quickshell -- and a per-window app_id is not
    // something a plugin can set. The only thing telling Omamail apart from
    // the pager, the speed test or the Wi-Fi QR panel is its TITLE.
    //
    // So identity for those windows is class+title, "org.quickshell:Omamail",
    // and entryIndexForClass resolves that against Name= the way it already
    // resolves a bare class. Every other window is returned unchanged, which is
    // what makes this safe to put on the identity path wholesale.
    std::string identityClass(const std::string& wmClass, const std::string& title);

    // Absolute path to the best icon file for an Icon= value, or "" if none.
    // Pure filesystem: decode and texture upload belong to the render layer.
    // Falls back to a generic application icon when the named one is missing
    // from the theme chain -- an entry with no icon at all still gets a tile.
    bool        iconExists(const std::string& iconName);
    std::optional<bool> iconExistsNow(const std::string& iconName); // nullopt: the index is busy/unbuilt, ask again
    std::string resolveIconPath(const std::string& iconName);
    // The same, in another icon theme (its own index, built on a worker the
    // first time; answers from the main theme until then).
    std::string resolveIconPathInTheme(const std::string& iconName, const std::string& theme);
    // The icon theme the current Omarchy theme names
    // (~/.local/state/omarchy/current/theme/icons.theme), re-read at most every
    // 2 s so a theme switch is picked up; "" when there is none.
    std::string omarchyIconTheme();

    // Detached, so the app outlives the overview closing.
    // terminalCmd is used only for Terminal=true entries; empty falls back to
    // $TERMINAL.
    bool launch(const SEntry& entry, const std::string& terminalCmd);

    // V3. The same launch, ON a named desktop. `execRule` is a Hyprland exec
    // rule WITHOUT its brackets ("workspace 5 silent"), and an empty one means
    // "here", which is plain launch().
    //
    // A rule cannot be handed to CProcess, because a rule is not a property of
    // the process: Hyprland matches it against the WINDOW when it maps, through
    // a token it plants in the child's environment. So a targeted launch is the
    // one path that goes out through the compositor's own executor -- the same
    // machinery `exec = [workspace 5 silent] foo` in a config uses, and the
    // only mechanism here that does not depend on `misc:initial_workspace_
    // tracking`, which this rice turns OFF (so the HL_INITIAL_WORKSPACE_TOKEN
    // route would silently place nothing).
    //
    // The executor takes a SHELL STRING and we hold argv, so the argv is
    // re-quoted rather than re-parsed: field codes are stripped and Terminal=
    // wrapped exactly once, here, by the same code plain launch() uses.
    bool launchWithRule(const SEntry& entry, const std::string& terminalCmd, const std::string& execRule);

    // OVERVIEW_CPP_SPEC §13.7 X18, open-window half. Parity with
    // windows-search-provider@G-dH, read off the HOST's own settings rather
    // than its defaults: search-method=0 is Strict, not fuzzy. Strict means
    // every space-separated term must appear somewhere in the text, in any
    // order, case- and accent-insensitively. Lives here because the accent
    // fold and the app-name lookup both belong to the desktop-entry layer.
    bool        strictMatch(const std::string& query, const std::string& text);

    // The app NAME for a window class ("org.gnome.Nautilus" -> "Files"), or ""
    // when no desktop entry claims it. The host matches window results against
    // the app name, so without this "files" would never find a Nautilus window.
    std::string appNameForClass(const std::string& wmClass);

    // The Icon= value for a window class, or "" when no entry claims it.
    // Shares appNameForClass's lookup and cache -- the dock (X27) asks the same
    // question about the same classes, once per frame.
    std::string iconForClass(const std::string& wmClass);

    // The desktop id for a window class, or "". X28's Show Details needs it.
    std::string idForClass(const std::string& wmClass);

    // X32. The entry for a desktop id, or nullptr. Favourites are stored as ids
    // and a pinned app is usually NOT running, so the dock cannot reach its icon
    // or its Exec through a window class the way every other lookup here does.
    // The pointer is into inventory(), so it dies on refresh() -- use it and
    // drop it, never store it.
    const SEntry* entryForId(const std::string& id);

    // X28. The entry's `Actions=` groups in declared order, or empty. Re-reads
    // the .desktop file: this is asked once per right-click, not per frame, so
    // the inventory stays the small thing the search path wants.
    std::vector<SAction> actionsForClass(const std::string& wmClass);
    // The same for a desktop id: a pinned app that is not running has no window
    // class to go through, and its menu used to come up empty (user, 2026-09-24).
    std::vector<SAction> actionsForId(const std::string& id);

    // X28. Detached spawn of a raw Exec= string, field codes stripped. The
    // action half of launch(); a desktop action is never Terminal=.
    bool launchExec(const std::string& exec);
}
