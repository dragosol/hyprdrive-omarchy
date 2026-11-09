#include "Locations.hpp"
#include "Launch.hpp"

#include "Config.hpp"
#include "MountSort.hpp"
#include "GvfsMount.hpp"
#include "StackModel.hpp"
#include "TrashDirs.hpp"

#include <gio/gio.h>
#include <gio/gunixmounts.h>
#include <gio/gdesktopappinfo.h>
#include <systemd/sd-bus.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <cstdio>
#include <unistd.h>
#include <thread>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <hyprutils/os/Process.hpp>

using namespace Hyprutils::OS;

namespace ScrollOverview::Locations {

namespace {

    constexpr const char* KEY_SHOW_TRASH  = "plugin:hyprdrive:dock:show_trash";
    constexpr const char* KEY_SHOW_MOUNTS = "plugin:hyprdrive:dock:show_mounts";
    constexpr const char* KEY_MOUNTS_NET  = "plugin:hyprdrive:dock:show_mounts_network";
    constexpr const char* KEY_MOUNTS_ONLY_MOUNTED = "plugin:hyprdrive:dock:show_mounts_only_mounted";
    constexpr const char* KEY_MOUNT_SORT          = "plugin:hyprdrive:dock:mount_sort";

    constexpr const char* TRASH_URI = "trash:///";

    // `UPDATE_TRASH_DELAY`. The reference debounces the trash monitor by this
    // much; here it is the poll period for the whole set, because the dock asks
    // items() once per frame and neither the trash count nor the mount table
    // moves at 120Hz.
    constexpr auto POLL = std::chrono::milliseconds{1000};

    // The hard ceiling on the one D-Bus round trip the poll makes. A poll that
    // costs longer than this is a dropped frame, and a dock item's running dot
    // is not worth one.
    constexpr uint64_t TIMEOUT_US = 20'000;

    // `FALLBACK_TRASH_ICON` / `FALLBACK_REMOVABLE_MEDIA_ICON`.
    constexpr const char* FALLBACK_TRASH = "user-trash";
    constexpr const char* FALLBACK_MEDIA = "drive-removable-media";

    std::vector<SLocation>                g_items;
    std::chrono::steady_clock::time_point g_polled{};
    bool                                  g_have = false;

    std::unordered_set<std::string>       g_openLocations;

    // See Locations::generation(). Bumped by the poll below, and only when the
    // poll actually changed one of the two things a caller can observe.
    uint64_t                              g_generation = 1;

    // The first name a GIcon offers, which is the one the icon theme is asked
    // for. A GFileIcon -- a custom per-directory icon -- has no name at all and
    // falls back: resolving it would mean teaching the render layer about file
    // paths for a case neither the trash nor a mount hits.
    std::string iconName(GIcon* icon) {
        if (!icon || !G_IS_THEMED_ICON(icon))
            return {};

        const char* const* NAMES = g_themed_icon_get_names(G_THEMED_ICON(icon));
        return NAMES && NAMES[0] ? std::string{NAMES[0]} : std::string{};
    }

    // `TrashAppInfo`. Empty-or-not, read as a DIRECTORY rather than as
    // `trash:///`.
    //
    // The reference asks GIO, and the first cut here did too -- and wedged the
    // compositor. `g_file_query_info("trash:///")` is a synchronous D-Bus round
    // trip to gvfsd-trash, on the render thread, once a second; when the
    // backend is slow or absent that is a stall the frame loop cannot absorb.
    // Measured 2026-08-03: the guest stopped answering hyprctl entirely.
    //
    // EVERY TRASH (user, 2026-11-06: "i still have items in the trash but
    // hyprdrive showing trash as empty"): files deleted on another disk live
    // in `<mount>/.Trash-1000`, and the home trash alone read empty while an
    // NTFS drive's trash held three. trashRoots() lists them all.
    // $XDG_DATA_HOME/Trash/files -- the spec's home trash, which every file
    // manager shares; ~/.local/share when XDG_DATA_HOME is unset.
    std::string trashFilesDir() {
        if (const char* X = std::getenv("XDG_DATA_HOME"); X && *X)
            return std::string{X} + "/Trash/files";
        if (const char* HOME = std::getenv("HOME"); HOME && *HOME)
            return std::string{HOME} + "/.local/share/Trash/files";
        return {};
    }

    std::vector<std::filesystem::path> trashRoots();

    void readTrash(SLocation& out) {
        out.uri   = TRASH_URI;
        out.name  = "Trash";
        out.empty = true;

        for (const auto& T : trashRoots()) {
            std::error_code error;
            if (!std::filesystem::is_empty(T / "files", error) && !error) {
                out.empty = false;
                break;
            }
        }

        // The two names the icon theme spells the two states with, and the same
        // two GIO would have answered with.
        out.icon = out.empty ? FALLBACK_TRASH : std::string{FALLBACK_TRASH} + "-full";
    }

    // `volume.get_identifier('class') === 'network'`. That identifier is gvfs
    // metadata; the filesystem type is the same fact straight from the kernel,
    // and it is what the local backend classifies on too.
    constexpr bool networkFs(const char* fsType) {
        if (!fsType)
            return false;

        constexpr std::string_view NETWORK[] = {"nfs", "nfs4", "cifs", "smbfs", "smb3", "sshfs", "ftp", "davfs", "afp", "ceph", "glusterfs", "9p"};

        std::string_view type{fsType};
        if (type.starts_with("fuse."))
            type.remove_prefix(5);

        return std::ranges::contains(NETWORK, type);
    }

    // The one piece of X35 the pixel rig cannot reach -- mounting a real NFS
    // export in the test VM to prove a string comparison is not a trade worth
    // making -- so it is checked at COMPILE time instead, and a broken prefix
    // strip or a mistyped entry fails the build rather than the dock.
    static_assert(networkFs("nfs4"));
    static_assert(networkFs("fuse.sshfs"));  // the prefix the kernel actually reports
    static_assert(!networkFs("ext4"));
    static_assert(!networkFs("fuse."));      // the strip must not leave a match on nothing
    static_assert(!networkFs("nfs4x"));      // and it is whole-word, not a prefix test
    static_assert(!networkFs(nullptr));

    // This user's trashes, by the freedesktop Trash spec: the home one, and on
    // each mounted device `$top/.Trash/$uid` or `$top/.Trash-$uid`. Only
    // mounts of a real device (/dev/...) that are not network filesystems:
    // the dock reads this once a POLL_MS on the compositor thread, and a stat
    // of a stale share would hang it. A disk's trash only when it is a real
    // one of this user's (TrashDirs::volumeTrashes: never through a symlink).
    std::vector<std::filesystem::path> trashRoots() {
        std::vector<std::filesystem::path> out;
        if (const auto FILES = trashFilesDir(); !FILES.empty())
            out.push_back(std::filesystem::path(FILES).parent_path());
        std::string table;
        if (FILE* m = std::fopen("/proc/self/mounts", "re")) {
            char   buf[4096];
            size_t n = 0;
            while ((n = std::fread(buf, 1, sizeof(buf), m)) > 0)
                table.append(buf, n);
            std::fclose(m);
        }
        // Decoded by TrashDirs (the \ooo escapes, in one pass): a drive whose
        // label holds a backslash used to make the decode here throw.
        for (const auto& M : TrashDirs::parseMounts(table)) {
            if (!M.device.starts_with("/dev/") || networkFs(M.type.c_str()))
                continue;
            for (const auto& T : TrashDirs::volumeTrashes(M.dir, getuid()))
                if (std::ranges::find(out, std::filesystem::path(T)) == out.end())
                    out.emplace_back(T);
        }
        return out;
    }

    // Mounted volumes come from the kernel's mount table, which is exactly the
    // set `show-mounts-only-mounted true` asks for -- so the common path needs
    // neither the gvfs proxy volume monitor nor a GMainLoop to drive it, and it
    // cannot go stale between polls the way a monitor with no loop to pump it
    // would.
    //
    // With the key FALSE the unmounted half comes from the mount POINTS --
    // fstab -- which is another plain file read on the same poll. Deliberately
    // NOT `g_volume_monitor_get()`: that reaches for the gvfs proxy monitor over
    // D-Bus, and this runs on the render thread once a second, where an earlier
    // version of this file already wedged the compositor by blocking.
    //
    // ponytail: the ceiling is a device that is neither in fstab nor mounted --
    // an inserted disc, a stick nobody has mounted yet. It gets no row. Upgrade
    // path is still the volume monitor, but on a PRIVATE GMainContext pumped
    // with `g_main_context_iteration(ctx, FALSE)`, and pumped from somewhere
    // that is allowed to block, which is not here.
    void readMounts(std::vector<SLocation>& out) {
        const bool NET          = Config::getValue<bool>(KEY_MOUNTS_NET);
        const bool ONLY_MOUNTED = Config::getValue<bool>(KEY_MOUNTS_ONLY_MOUNTED);

        GList* mounts = g_unix_mount_entries_get(nullptr);
        for (GList* l = mounts; l; l = l->next) {
            auto* entry = static_cast<GUnixMountEntry*>(l->data);
            if (!entry)
                continue;

            // `mount.is_shadowed()` and `!can_eject() && !can_unmount()` in one:
            // a mount the platform calls internal is /proc, /sys, the root and
            // every bind under them, and nothing there is unmountable by a user.
            if (g_unix_mount_entry_is_system_internal(entry) || !g_unix_mount_entry_guess_should_display(entry))
                continue;

            if (networkFs(g_unix_mount_entry_get_fs_type(entry)) && !NET)
                continue;

            const char* PATH = g_unix_mount_entry_get_mount_path(entry);
            if (!PATH)
                continue;

            char* uri   = g_filename_to_uri(PATH, nullptr, nullptr);
            char* label = g_unix_mount_entry_guess_name(entry);
            GIcon* icon = g_unix_mount_entry_guess_icon(entry);

            SLocation loc;
            loc.uri       = uri ? std::string{uri} : std::string{"file://"} + PATH;
            loc.mountPath = PATH;
            loc.name      = label ? std::string{label} : std::string{PATH};
            loc.icon     = iconName(icon);
            loc.mount    = true;
            loc.canEject = g_unix_mount_entry_guess_can_eject(entry);

            if (loc.icon.empty())
                loc.icon = FALLBACK_MEDIA;

            out.emplace_back(std::move(loc));

            if (icon)
                g_object_unref(icon);
            g_free(uri);
            g_free(label);
        }

        g_list_free_full(mounts, reinterpret_cast<GDestroyNotify>(g_unix_mount_entry_free));

        if (ONLY_MOUNTED)
            return;

        const size_t MOUNTEDCOUNT = out.size();
        GList*       points       = g_unix_mount_points_get(nullptr);
        for (GList* l = points; l; l = l->next) {
            auto* point = static_cast<GUnixMountPoint*>(l->data);
            if (!point)
                continue;

            // A mount point has no `is_system_internal` / `guess_should_display`
            // -- glib only offers those for live mounts. `is_user_mountable` is
            // the honest equivalent and a sharper filter than either: it is true
            // exactly for the fstab entries carrying `user`/`users`, which are
            // the ones this dock could actually mount if the row were clicked.
            if (!g_unix_mount_point_is_user_mountable(point))
                continue;

            if (networkFs(g_unix_mount_point_get_fs_type(point)) && !NET)
                continue;

            const char* PATH = g_unix_mount_point_get_mount_path(point);
            if (!PATH)
                continue;

            // fstab lists the mounted ones too, and those already have a row
            // from the mount table above -- with the eject flag and the label
            // the KERNEL reports, which is the better of the two answers.
            const bool ALREADYMOUNTED = std::any_of(out.begin(), out.begin() + MOUNTEDCOUNT, [PATH](const SLocation& l) { return l.mountPath == PATH; });
            if (ALREADYMOUNTED)
                continue;

            char*  label = g_unix_mount_point_guess_name(point);
            GIcon* icon  = g_unix_mount_point_guess_icon(point);

            SLocation loc;
            loc.uri       = std::string{"file://"} + PATH;
            loc.mountPath = PATH;
            loc.name      = label ? std::string{label} : std::string{PATH};
            loc.icon      = iconName(icon);
            loc.mount     = true;
            loc.mounted   = false;
            loc.canEject  = g_unix_mount_point_guess_can_eject(point);

            if (loc.icon.empty())
                loc.icon = FALLBACK_MEDIA;

            out.emplace_back(std::move(loc));

            if (icon)
                g_object_unref(icon);
            g_free(label);
        }

        g_list_free_full(points, reinterpret_cast<GDestroyNotify>(g_unix_mount_point_free));
    }

    // One connection for the lifetime of the process. Opening a user bus means
    // a connect and an auth handshake, and doing that once a second on the
    // render thread is a cost with no reason to exist.
    sd_bus* userBus() {
        static sd_bus* bus = [] {
            sd_bus* b = nullptr;
            return sd_bus_default_user(&b) < 0 ? nullptr : b;
        }();

        return bus;
    }

    // Every location any file-manager window currently shows. The property is
    // `a{sas}` -- window id -> locations -- and only the VALUES are read: which
    // window shows it is a question nothing here asks, and one with no reliable
    // answer under Wayland.
    //
    // Built by hand rather than with sd_bus_get_property because two things
    // about the call matter more than its brevity, and that helper exposes
    // neither: it must NOT activate Nautilus (a dock poll is not a reason to
    // start a file manager, and the activation wait is seconds long), and it
    // must give up fast. This runs on the render thread once a second.
    void readOpenLocations() {
        g_openLocations.clear();

        sd_bus* bus = userBus();
        if (!bus)
            return;

        sd_bus_message* call = nullptr;
        if (sd_bus_message_new_method_call(bus, &call, "org.freedesktop.FileManager1", "/org/freedesktop/FileManager1", "org.freedesktop.DBus.Properties", "Get") < 0 || !call)
            return;

        sd_bus_message_set_auto_start(call, 0);
        sd_bus_message_append(call, "ss", "org.freedesktop.FileManager1", "OpenWindowsWithLocations");

        sd_bus_error    error = SD_BUS_ERROR_NULL;
        sd_bus_message* reply = nullptr;

        const int R = sd_bus_call(bus, call, TIMEOUT_US, &error, &reply);
        sd_bus_message_unref(call);

        if (R >= 0 && reply && sd_bus_message_enter_container(reply, 'v', "a{sas}") > 0 && sd_bus_message_enter_container(reply, 'a', "{sas}") > 0) {
            while (sd_bus_message_enter_container(reply, 'e', "sas") > 0) {
                sd_bus_message_skip(reply, "s"); // the window id

                char** uris = nullptr;
                if (sd_bus_message_read_strv(reply, &uris) > 0 && uris) {
                    for (char** u = uris; *u; ++u) {
                        g_openLocations.insert(*u);
                        free(*u);
                    }
                    free(uris);
                }

                sd_bus_message_exit_container(reply);
            }
            sd_bus_message_exit_container(reply);
            sd_bus_message_exit_container(reply);
        }

        if (reply)
            sd_bus_message_unref(reply);
        sd_bus_error_free(&error);
    }

    void spawn(const std::string& binary, const std::vector<std::string>& args) {
        CProcess process(binary, args);
        process.runAsync();
    }

    constexpr const char* NAUTILUS_NAME = "org.gnome.Nautilus";

    // Nautilus can answer on the session bus: running (the name has an owner)
    // or startable by the bus (an activatable name). Asked of the bus daemon
    // itself, which answers at once; neither call starts anything.
    bool nautilusOnBus() {
        sd_bus* bus = userBus();
        if (!bus)
            return false;

        bool            found = false;
        sd_bus_error    error = SD_BUS_ERROR_NULL;
        sd_bus_message* reply = nullptr;
        int             owned = 0;
        if (sd_bus_call_method(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "NameHasOwner", &error, &reply, "s", NAUTILUS_NAME) >= 0 && reply &&
            sd_bus_message_read(reply, "b", &owned) > 0)
            found = owned != 0;
        if (reply)
            reply = sd_bus_message_unref(reply);
        sd_bus_error_free(&error);

        if (!found && sd_bus_call_method(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "ListActivatableNames", &error, &reply, "") >= 0 && reply) {
            char** names = nullptr;
            if (sd_bus_message_read_strv(reply, &names) > 0 && names) {
                for (char** n = names; *n; ++n) {
                    found = found || std::string_view{*n} == NAUTILUS_NAME;
                    free(*n);
                }
                free(names);
            }
        }
        if (reply)
            sd_bus_message_unref(reply);
        sd_bus_error_free(&error);
        return found;
    }
}

// THE FILE MANAGER'S OWN MOUNTS (GvfsMount.hpp): an smb://, sftp:// or MTP
// mount made in Nautilus is gvfsd's, not the kernel's. Asked of gvfsd's
// MountTracker exactly the way readOpenLocations asks Nautilus: never
// auto-started (no gvfsd, no network mounts -- nothing to wait for) and
// bounded at TIMEOUT_US, because this is the render thread once a second.
// The list itself is gvfsd's in-memory table: no server is contacted, so a
// share whose server has gone away still answers at once.
// NAUTILUS'S ICON THEME (user, 2026-10-08: "network drives icon also needs to
// adopt whatever icon theme nautilus is using"). Nautilus is GTK 4 and takes
// the theme from org.gnome.desktop.interface icon-theme; the dock's bare names
// resolve against GTK 3's settings.ini, which here names none (so Adwaita),
// while Nautilus drew arcticons-dark -> breeze-dark. A dconf read is a mapped
// file, not a round trip. The schema is looked up first: g_settings_new()
// ABORTS the process on a schema that is not installed.
std::string nautilusIconTheme() {
    static GSettings* settings = [] {
        GSettingsSchemaSource* SRC = g_settings_schema_source_get_default();
        GSettingsSchema*       SCH = SRC ? g_settings_schema_source_lookup(SRC, "org.gnome.desktop.interface", TRUE) : nullptr;
        if (!SCH)
            return static_cast<GSettings*>(nullptr);
        const bool HASKEY = g_settings_schema_has_key(SCH, "icon-theme");
        g_settings_schema_unref(SCH);
        return HASKEY ? g_settings_new("org.gnome.desktop.interface") : static_cast<GSettings*>(nullptr);
    }();
    if (!settings)
        return {};
    // Asked per dock icon per frame now (themePrefix): read at most every 2 s.
    static std::string                           cached;
    static std::chrono::steady_clock::time_point at{};
    const auto                                   NOW = std::chrono::steady_clock::now();
    if (at != std::chrono::steady_clock::time_point{} && NOW - at < std::chrono::seconds(2))
        return cached;
    at              = NOW;
    gchar* v        = g_settings_get_string(settings, "icon-theme");
    cached          = v ? v : "";
    g_free(v);
    return cached;
}

static void readGvfsMounts(std::vector<SLocation>& out, bool network) {
    sd_bus* bus = userBus();
    if (!bus)
        return;

    sd_bus_message* call = nullptr;
    if (sd_bus_message_new_method_call(bus, &call, "org.gtk.vfs.Daemon", "/org/gtk/vfs/mounttracker", "org.gtk.vfs.MountTracker", "ListMounts2") < 0 || !call)
        return;
    sd_bus_message_set_auto_start(call, 0);
    sd_bus_message_append(call, "b", 1); // user-visible only

    sd_bus_error    error = SD_BUS_ERROR_NULL;
    sd_bus_message* reply = nullptr;
    const int       R     = sd_bus_call(bus, call, TIMEOUT_US, &error, &reply);
    sd_bus_message_unref(call);
    sd_bus_error_free(&error);
    if (R < 0 || !reply)
        return;

    // "@<theme>:" makes OverviewRender::appIcon resolve in that theme and its
    // Inherits= chain -- the theme Nautilus draws these very icons with.
    const auto THEME  = nautilusIconTheme();
    const auto PREFIX = THEME.empty() ? std::string{} : "@" + THEME + ":";

    // A byte string from gvfsd carries its NUL; one that fails to read is "".
    const auto readBytes = [&](std::string& into) {
        const void* data = nullptr;
        size_t      size = 0;
        into.clear();
        if (sd_bus_message_read_array(reply, 'y', &data, &size) < 0 || !data)
            return false;
        into.assign(static_cast<const char*>(data), size);
        while (!into.empty() && into.back() == '\0')
            into.pop_back();
        return true;
    };

    if (sd_bus_message_enter_container(reply, 'a', "(sossssssbay(aya{sv})ay)") > 0) {
        while (sd_bus_message_enter_container(reply, 'r', "sossssssbay(aya{sv})ay") > 0) {
            Gvfs::SMount m;
            const char * id = nullptr, *obj = nullptr, *display = nullptr, *stable = nullptr, *xcontent = nullptr, *icon = nullptr, *symicon = nullptr, *enc = nullptr;
            int          visible = 0;
            bool         ok      = sd_bus_message_read(reply, "sossssssb", &id, &obj, &display, &stable, &xcontent, &icon, &symicon, &enc, &visible) >= 0;
            ok                   = ok && readBytes(m.fuse);
            if (ok && sd_bus_message_enter_container(reply, 'r', "aya{sv}") > 0) {
                readBytes(m.prefix);
                if (sd_bus_message_enter_container(reply, 'a', "{sv}") > 0) {
                    while (sd_bus_message_enter_container(reply, 'e', "sv") > 0) {
                        const char* key = nullptr;
                        sd_bus_message_read(reply, "s", &key);
                        std::string value;
                        if (sd_bus_message_enter_container(reply, 'v', "ay") > 0) {
                            readBytes(value);
                            sd_bus_message_exit_container(reply);
                        } else
                            sd_bus_message_skip(reply, "v");
                        if (key)
                            m.spec.emplace_back(key, std::move(value));
                        sd_bus_message_exit_container(reply);
                    }
                    sd_bus_message_exit_container(reply);
                }
                sd_bus_message_exit_container(reply);
            }
            std::string defaultLocation;
            readBytes(defaultLocation);
            sd_bus_message_exit_container(reply);
            if (!ok)
                continue;

            m.display     = display ? display : "";
            m.icon        = icon ? icon : "";
            m.userVisible = visible != 0;
            if (m.prefix.empty())
                m.prefix = "/";
            if (!Gvfs::listed(m) || (!network && Gvfs::isNetwork(Gvfs::specValue(m, "type"))))
                continue;
            const auto URI = Gvfs::uriFor(m);
            if (!URI)
                continue;

            SLocation loc;
            loc.uri       = *URI;
            loc.mountPath = m.fuse; // what fstab order and the dedupe below compare
            loc.name      = m.display.empty() ? *URI : m.display;
            loc.icon      = Gvfs::firstThemedIcon(m.icon);
            loc.mount     = true;
            loc.canEject  = false;
            loc.mounted   = true;
            if (loc.icon.empty())
                loc.icon = Gvfs::isNetwork(Gvfs::specValue(m, "type")) ? "folder-remote" : FALLBACK_MEDIA;
            loc.icon = PREFIX + loc.icon;
            if (std::ranges::none_of(out, [&](const SLocation& l) { return l.uri == loc.uri; }))
                out.emplace_back(std::move(loc));
        }
        sd_bus_message_exit_container(reply);
    }
    sd_bus_message_unref(reply);
}

const std::vector<SLocation>& items() {
    const auto NOW = std::chrono::steady_clock::now();
    if (g_have && NOW - g_polled < POLL)
        return g_items;

    g_polled = NOW;
    g_have   = true;

    // Compared, not just replaced: the dock's item cache hangs off
    // generation(), and a poll that re-reads the same mount table and the same
    // trash must not invalidate it. Copies of a handful of small structs once a
    // second is nothing; rebuilding the whole dock item model at 1Hz for no
    // visible change is exactly what this was meant to stop.
    const auto WEREITEMS = g_items;
    const auto WEREOPEN  = g_openLocations;

    g_items.clear();

    // `Locations.getApps()`: removables first, then the trash.
    if (Config::getValue<bool>(KEY_SHOW_MOUNTS)) {
        readMounts(g_items);
        readGvfsMounts(g_items, Config::getValue<bool>(KEY_MOUNTS_NET));
        // dock:mount_sort (MountSort.hpp). `mounted` keeps the mount table's
        // order, which is the order the drives happened to be mounted in. Stable,
        // so drives with the same name keep that order between themselves.
        switch (MountSort::orderFromName(Config::getValue<std::string>(KEY_MOUNT_SORT))) {
            case MountSort::EOrder::Name:
                std::ranges::stable_sort(g_items, [](const SLocation& a, const SLocation& b) { return StackModel::naturalLess(a.name, b.name); });
                break;
            case MountSort::EOrder::Smart: std::ranges::stable_sort(g_items, [](const SLocation& a, const SLocation& b) { return MountSort::smartLess(a.name, b.name); }); break;
            case MountSort::EOrder::Fstab: {
                // g_unix_mount_points_get is fstab, in file order (glib caches
                // it on the file's mtime, so this is no re-parse per poll).
                std::vector<std::string> fstab;
                GList*                   points = g_unix_mount_points_get(nullptr);
                for (GList* l = points; l; l = l->next)
                    if (auto* POINT = static_cast<GUnixMountPoint*>(l->data))
                        if (const char* P = g_unix_mount_point_get_mount_path(POINT))
                            fstab.emplace_back(P);
                g_list_free_full(points, reinterpret_cast<GDestroyNotify>(g_unix_mount_point_free));
                std::ranges::stable_sort(g_items, [&](const SLocation& a, const SLocation& b) {
                    return MountSort::fstabRank(fstab, a.mountPath) < MountSort::fstabRank(fstab, b.mountPath);
                });
                break;
            }
            case MountSort::EOrder::Mounted: break;
        }
    }

    if (Config::getValue<bool>(KEY_SHOW_TRASH))
        readTrash(g_items.emplace_back());

    readOpenLocations();

    if (g_items != WEREITEMS || g_openLocations != WEREOPEN)
        ++g_generation;

    return g_items;
}

uint64_t generation() {
    // items() is what polls, so asking for the generation without asking for
    // the items would hand back a number from before the poll that the caller's
    // very next line is about to run.
    items();
    return g_generation;
}

bool openWindows(const std::string& uri) {
    if (g_openLocations.contains(uri))
        return true;

    // Nautilus reports the trash as `trash:///`, `trash://` or `trash:`
    // depending on how the window was opened, so the one location with three
    // spellings is compared on all of them rather than missed two times in
    // three.
    if (uri == TRASH_URI)
        return g_openLocations.contains("trash://") || g_openLocations.contains("trash:");

    return false;
}

// THE FILE MANAGER (user, 2026-11-04: "Clement changed his default file manager
// but hyprdrive still opens nautilus for him from the dock stacks and
// folders"). plugin:hyprdrive:general:file_manager names a desktop entry
// ("org.kde.dolphin.desktop", Settings > General); empty, it is the system's
// default for folders -- xdg-mime's inode/directory, what `xdg-mime default`
// and every desktop's "default apps" setting change. Nautilus only when
// neither names anything.
namespace {
    GAppInfo* fileManagerApp() {
        std::string id = Config::getValue<std::string>("plugin:hyprdrive:general:file_manager");
        if (id == "[[EMPTY]]")
            id.clear();
        if (!id.empty()) {
            if (!id.ends_with(".desktop"))
                id += ".desktop";
            if (auto* app = g_desktop_app_info_new(id.c_str()))
                return G_APP_INFO(app);
        }
        return g_app_info_get_default_for_type("inode/directory", FALSE);
    }

    std::string appId(GAppInfo* app) {
        const char* ID = app ? g_app_info_get_id(app) : nullptr;
        return ID ? ID : "";
    }

    bool isNautilus(GAppInfo* app) {
        std::string id = appId(app);
        std::ranges::transform(id, id.begin(), [](unsigned char c) { return std::tolower(c); });
        return id.find("nautilus") != std::string::npos;
    }

    // The app's own Exec line with `uri` in its field codes (the Desktop Entry
    // spec: %u/%U a URI, %f/%F a local path, %i %c %k dropped), as argv -- so it
    // is launched through Launch::wrap into its own scope like every other app,
    // not as a child of Hyprland's.
    std::vector<std::string> argvFor(GAppInfo* app, const std::string& uri, const std::vector<std::string>& extra = {}) {
        const char* CL = app ? g_app_info_get_commandline(app) : nullptr;
        if (!CL)
            return {};
        gint    argc = 0;
        gchar** argv = nullptr;
        if (!g_shell_parse_argv(CL, &argc, &argv, nullptr) || !argv)
            return {};
        std::string path = uri;
        if (uri.starts_with("file://"))
            if (gchar* P = g_filename_from_uri(uri.c_str(), nullptr, nullptr)) {
                path = P;
                g_free(P);
            }
        std::vector<std::string> out;
        bool                     placed = false;
        for (gint i = 0; i < argc; ++i) {
            std::string a = argv[i], r;
            bool        drop = false;
            for (size_t k = 0; k < a.size(); ++k) {
                if (a[k] != '%' || k + 1 >= a.size()) {
                    r += a[k];
                    continue;
                }
                const char C = a[++k];
                if (C == 'u' || C == 'U') {
                    r += uri;
                    placed = true;
                } else if (C == 'f' || C == 'F') {
                    r += path;
                    placed = true;
                } else if (C == '%')
                    r += '%';
                else if (r.empty() && k + 1 == a.size())
                    drop = true; // a lone %i / %c / %k
            }
            if (!drop && !r.empty())
                out.push_back(r);
        }
        g_strfreev(argv);
        if (out.empty())
            return {};
        out.insert(out.begin() + 1, extra.begin(), extra.end());
        if (!placed)
            out.push_back(uri);
        return out;
    }

    bool launchWith(GAppInfo* app, const std::string& uri, const std::vector<std::string>& extra = {}) {
        auto argv = argvFor(app, uri, extra);
        if (argv.empty())
            return false;
        std::string              binary = argv.front();
        std::vector<std::string> args(argv.begin() + 1, argv.end());
        Launch::wrap(binary, args);
        spawn(binary, args);
        return true;
    }
}

// The file managers that open `trash:///` themselves: GIO's (Nautilus, Nemo,
// Caja, Thunar, PCManFM) and KIO's (Dolphin, Konqueror). Any other -- a
// terminal one, a sandboxed one -- is handed the trash's own folder, which it
// lists like any other; given trash:/// it showed an empty view.
static bool opensTrashUri(GAppInfo* app) {
    std::string id = appId(app);
    std::ranges::transform(id, id.begin(), [](unsigned char c) { return std::tolower(c); });
    for (const char* K : {"nautilus", "nemo", "caja", "thunar", "pcmanfm", "dolphin", "konqueror"})
        if (id.find(K) != std::string::npos)
            return true;
    return false;
}

void activate(const std::string& uri) {
    if (uri.empty())
        return;
    // One argv element so a volume label stays one URI. The file manager lives
    // on: its own scope, not Hyprland's (Launch.hpp).
    GAppInfo*   app    = fileManagerApp();
    std::string target = uri;
    if (app && uri == TRASH_URI && !opensTrashUri(app))
        if (const auto DIR = trashFilesDir(); !DIR.empty())
            target = "file://" + DIR;
    const bool OK = app && launchWith(app, target);
    if (app)
        g_object_unref(app);
    if (OK)
        return;
    std::string              binary = "nautilus";
    std::vector<std::string> args   = {uri};
    Launch::wrap(binary, args);
    spawn(binary, args);
}

void reveal(const std::string& path) {
    if (path.empty())
        return;
    GAppInfo* app = fileManagerApp();
    // Nautilus (or nothing chosen at all): FileManager1.ShowItems, the parent
    // folder with this one selected.
    if (!app || isNautilus(app)) {
        if (app)
            g_object_unref(app);
        Hyprutils::OS::CProcess("gdbus", {"call", "--session", "--dest", "org.freedesktop.FileManager1", "--object-path", "/org/freedesktop/FileManager1", "--method",
                                          "org.freedesktop.FileManager1.ShowItems", "['file://" + path + "']", ""})
            .runAsync();
        return;
    }
    // Another file manager: Dolphin selects it (--select); the rest open the
    // folder it is in. ShowItems would go to whoever owns FileManager1, which
    // is often still Nautilus.
    std::string id = appId(app);
    std::ranges::transform(id, id.begin(), [](unsigned char c) { return std::tolower(c); });
    const bool        DOLPHIN = id.find("dolphin") != std::string::npos;
    const std::string PARENT  = std::filesystem::path(path).parent_path().string();
    const bool        OK      = DOLPHIN ? launchWith(app, "file://" + path, {"--select"}) : launchWith(app, "file://" + (PARENT.empty() ? path : PARENT));
    g_object_unref(app);
    if (!OK)
        activate("file://" + (PARENT.empty() ? path : PARENT));
}

// Every trash this user has (the spec's home trash, and each mount's
// $topdir/.Trash/$uid and $topdir/.Trash-$uid), emptied on a thread of its own:
// a big trash takes seconds and this is called from a click. Everything inside
// files/, info/ and expunged/ is removed, and no symlink is ever followed
// (TrashDirs::empty). The old walk listed `T/files` through a link: a disk
// carrying `.Trash-1000/files -> /home/u` had Empty Trash delete the home
// folder's contents (review 2026-11-09). The dock's poll sees it empty.
static void emptyTrashDirs() {
    const auto  TRASHES = trashRoots();
    const auto  FILES   = trashFilesDir();
    const auto  HOME    = FILES.empty() ? std::filesystem::path{} : std::filesystem::path(FILES).parent_path();
    const uid_t UID     = getuid();
    // A thread that throws is std::terminate, and so is one that cannot be
    // started: either would take the compositor with it. No thread, no
    // emptying -- the trash stays full and the row can be clicked again.
    try {
        std::thread([TRASHES, HOME, UID] {
            try {
                for (const auto& T : TRASHES)
                    TrashDirs::empty(T.string(), T == HOME, UID);
            } catch (...) {}
        }).detach();
    } catch (...) {}
}

bool emptyTrashAsksFirst() {
    // Nautilus's dialog only when Nautilus is the file manager in use: with
    // another one chosen it is not the trash the user is working with.
    GAppInfo*  app  = fileManagerApp();
    const bool OURS = !app || isNautilus(app);
    if (app)
        g_object_unref(app);
    return OURS && nautilusOnBus();
}

size_t trashCount() {
    size_t n = 0;
    for (const auto& T : trashRoots()) {
        std::error_code ec;
        for (auto it = std::filesystem::directory_iterator(T / "files", ec); !ec && it != std::filesystem::directory_iterator(); it.increment(ec))
            if (++n >= 100000)
                return n;
    }
    return n;
}

void emptyTrash() {
    // No Nautilus to ask (plain Hyprland, another file manager): emptied
    // here, by the freedesktop Trash spec. NOT `gio trash --empty`: that is
    // gvfsd-trash's, and without gvfs running (KDE, a bare session) it fails
    // with "Operation not supported" -- the Trash "can't be emptied". It asks
    // nothing, so the dock asked first (emptyTrashAsksFirst, the menu's
    // confirm rows) -- the user's click there is the confirmation.
    if (!emptyTrashAsksFirst()) {
        emptyTrashDirs();
        return;
    }

    sd_bus* bus = userBus();
    if (!bus)
        return;

    sd_bus_message* msg = nullptr;

    // ask_confirmation TRUE, exactly as `launchAction` passes it. The dialog is
    // Nautilus's, and it is the only thing between a stray click and the user's
    // files -- which is also why this asks Nautilus rather than emptying the
    // trash directory itself.
    if (sd_bus_message_new_method_call(bus, &msg, "org.gnome.Nautilus", "/org/gnome/Nautilus/FileOperations2", "org.gnome.Nautilus.FileOperations2", "EmptyTrash") >= 0 && msg) {
        sd_bus_message_append(msg, "b", 1);
        sd_bus_message_open_container(msg, 'a', "{sv}"); // platform_data: no parent handle to give
        sd_bus_message_close_container(msg);

        // NO REPLY EXPECTED, as DockStacks' Sushi call (GitHub #3, thanks
        // @whogotpwned). Nautilus is D-Bus ACTIVATED: on a cold click the bus
        // daemon has to start it first, and a blocking sd_bus_call froze the
        // whole compositor for all of that startup (~15 s). The reply was never
        // read anyway. Queued instead -- the bus daemon holds it until Nautilus
        // has started -- and flushed, since this connection only watches
        // READABLE. Nautilus still asks before it empties anything.
        sd_bus_message_set_expect_reply(msg, 0);
        if (sd_bus_send(bus, msg, nullptr) >= 0)
            sd_bus_flush(bus);
        sd_bus_message_unref(msg);
    }
}

void unmount(const std::string& uri, bool eject) {
    if (uri.empty())
        return;

    // `gio` is glib's own CLI and glib is already a hard dependency of this
    // build. It carries the polkit prompt and the "device is busy" reporting
    // that a raw umount(2) from in here would not.
    spawn("gio", {"mount", eject ? "-e" : "-u", uri});
}

// Declared in the header and CALLED from Dock.cpp's Mount row since X35, and
// never defined: the .so linked fine and Hyprland would abort with `undefined
// symbol` the first time that row was clicked, because a plugin's unresolved
// symbol is a lazy-binding abort at the call and not a load-time failure.
// Found by scripts/symcheck.sh, which now runs before every load -- not by
// anyone reaching the row.
//
// The mirror of unmount, and gio for the same reason: it carries the polkit
// prompt, which is exactly what mounting a device wants.
void mount(const std::string& uri) {
    if (uri.empty())
        return;

    spawn("gio", {"mount", uri});
}

}
