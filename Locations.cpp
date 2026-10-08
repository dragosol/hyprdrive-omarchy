#include "Locations.hpp"
#include "Launch.hpp"

#include "Config.hpp"
#include "MountSort.hpp"
#include "GvfsMount.hpp"
#include "StackModel.hpp"

#include <gio/gio.h>
#include <gio/gunixmounts.h>
#include <systemd/sd-bus.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
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
    // ponytail: the HOME trash only. Files deleted on another filesystem live
    // in `<mount>/.Trash-1000` and are not counted here, so the icon can read
    // empty while another disk's trash is not. Upgrade path is to walk the same
    // mount list readMounts() already reads and stat each one's `.Trash-$UID`.
    void readTrash(SLocation& out) {
        out.uri   = TRASH_URI;
        out.name  = "Trash";
        out.empty = true;

        const char* HOME = std::getenv("HOME");
        if (HOME) {
            const std::string FILES = std::string{HOME} + "/.local/share/Trash/files";
            std::error_code   error;
            out.empty = !std::filesystem::exists(FILES, error) || std::filesystem::is_empty(FILES, error) || error;
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
        if (sd_bus_message_new_method_call(bus, &call, "org.freedesktop.FileManager1", "/org/freedesktop/FileManager1", "org.freedesktop.Properties", "Get") < 0 || !call)
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
}

// THE FILE MANAGER'S OWN MOUNTS (GvfsMount.hpp): an smb://, sftp:// or MTP
// mount made in Nautilus is gvfsd's, not the kernel's. Asked of gvfsd's
// MountTracker exactly the way readOpenLocations asks Nautilus: never
// auto-started (no gvfsd, no network mounts -- nothing to wait for) and
// bounded at TIMEOUT_US, because this is the render thread once a second.
// The list itself is gvfsd's in-memory table: no server is contacted, so a
// share whose server has gone away still answers at once.
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

void activate(const std::string& uri) {
    if (uri.empty())
        return;

    // Open with Nautilus directly. Fedora ships BOTH
    // org.freedesktop.FileManager1.service (nautilus) and
    // org.kde.dolphin.FileManager1.service (dolphin --daemon); xdg-open /
    // FileManager1 activation can land on Dolphin when Nautilus is slow or
    // stuck. This rice uses Nautilus as the file manager (dock + overview
    // search + trash). One argv element so a volume label stays one URI.
    // Nautilus lives on: its own scope, not Hyprland's (Launch.hpp). The gio
    // mount helpers below are gone in a second and stay as they are.
    std::string              binary = "nautilus";
    std::vector<std::string> args   = {uri};
    Launch::wrap(binary, args);
    spawn(binary, args);
}

void emptyTrash() {
    sd_bus* bus = userBus();
    if (!bus)
        return;

    sd_bus_error    error = SD_BUS_ERROR_NULL;
    sd_bus_message* msg   = nullptr;

    // ask_confirmation TRUE, exactly as `launchAction` passes it. The dialog is
    // Nautilus's, and it is the only thing between a stray click and the user's
    // files -- which is also why this asks Nautilus rather than emptying the
    // trash directory itself.
    if (sd_bus_message_new_method_call(bus, &msg, "org.gnome.Nautilus", "/org/gnome/Nautilus/FileOperations2", "org.gnome.Nautilus.FileOperations2", "EmptyTrash") >= 0 && msg) {
        sd_bus_message_append(msg, "b", 1);
        sd_bus_message_open_container(msg, 'a', "{sv}"); // platform_data: no parent handle to give
        sd_bus_message_close_container(msg);

        // Not bounded the way the poll is: this one is a USER ACTION, it may
        // legitimately have to start Nautilus, and it happens on a click rather
        // than once a second. Emptying the trash is worth a frame.
        sd_bus_call(bus, msg, 0, &error, nullptr);
        sd_bus_message_unref(msg);
    }

    sd_bus_error_free(&error);
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
