#pragma once

// THE FILE MANAGER'S OWN MOUNTS (user, 2026-10-08: "I have an SMB mount in
// nautilus but it isn't showing up in the dock"). A share Nautilus mounts --
// smb://, sftp://, dav://, a phone over MTP -- is a GVfs mount: it lives in
// gvfsd, and the kernel's mount table only has gvfsd-fuse's one umbrella mount
// at /run/user/$UID/gvfs, which glib marks system-internal. The dock read only
// the kernel table, so it never saw any of them.
//
// Locations.cpp asks gvfsd's MountTracker for its list over D-Bus (bounded, no
// auto-start: it runs on the render thread). What it gets back is turned into
// dock items here. Pure -- no GLib, no sd-bus -- scripts/gvfs_mount_test.cpp
// builds it on the host.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ScrollOverview::Gvfs {

    // One entry of org.gtk.vfs.MountTracker.ListMounts2, the parts the dock uses.
    struct SMount {
        std::string                                      display;  // "office cloud on 100.122.4.49"
        std::string                                      icon;     // a serialized GIcon: ". GThemedIcon folder-remote folder ..."
        std::string                                      fuse;     // /run/user/1000/gvfs/smb-share:server=...,share=...
        std::string                                      prefix;   // the mount spec's path prefix, "/" when none
        std::vector<std::pair<std::string, std::string>> spec;     // type=smb-share, server=..., share=...
        bool                                             userVisible = false;
    };

    inline std::string specValue(const SMount& m, std::string_view key) {
        for (const auto& [K, V] : m.spec)
            if (K == key)
                return V;
        return {};
    }

    // The first name of a serialized GThemedIcon (g_icon_to_string), or "".
    // ". GThemedIcon folder-remote folder ..." -> "folder-remote".
    inline std::string firstThemedIcon(const std::string& serialized) {
        constexpr std::string_view TAG = ". GThemedIcon ";
        if (!serialized.starts_with(TAG))
            return {};
        const auto REST = serialized.substr(TAG.size());
        const auto END  = REST.find(' ');
        return REST.substr(0, END);
    }

    // RFC 3986: everything but the unreserved set is %XX. `keepSlash` for paths.
    inline std::string escape(std::string_view s, bool keepSlash) {
        static constexpr char HEX[] = "0123456789ABCDEF";
        std::string           out;
        for (const unsigned char C : s) {
            const bool UNRESERVED = (C >= 'A' && C <= 'Z') || (C >= 'a' && C <= 'z') || (C >= '0' && C <= '9') || C == '-' || C == '.' || C == '_' || C == '~';
            if (UNRESERVED || (keepSlash && C == '/'))
                out += static_cast<char>(C);
            else {
                out += '%';
                out += HEX[C >> 4];
                out += HEX[C & 15];
            }
        }
        return out;
    }

    // Kinds that are a server somewhere else: dock:show_mounts_network hides them.
    inline bool isNetwork(std::string_view type) {
        constexpr std::string_view NETWORK[] = {"smb-share", "smb-server", "sftp", "ftp", "ftps", "dav", "afp-volume", "afp-server", "nfs"};
        for (const auto N : NETWORK)
            if (type == N)
                return true;
        return false;
    }

    // gvfsd's own places, which Nautilus shows in its sidebar, not as mounts.
    inline bool listed(const SMount& m) {
        if (!m.userVisible)
            return false;
        const auto                 TYPE   = specValue(m, "type");
        constexpr std::string_view HIDDEN[] = {"trash", "recent", "computer", "network", "burn", "admin", "localtest", ""};
        for (const auto H : HIDDEN)
            if (TYPE == H)
                return false;
        return true;
    }

    // The location as Nautilus names it -- what it opens, what `gio mount -u`
    // takes, and what FileManager1 reports for a window showing it (so the
    // running dot can light). The kinds the dock understands get their own
    // scheme; anything else is opened through its FUSE path, which every app
    // can read. Nothing at all when there is neither.
    inline std::optional<std::string> uriFor(const SMount& m) {
        const auto TYPE = specValue(m, "type");
        auto       host = specValue(m, "host");
        if (host.empty())
            host = specValue(m, "server");
        if (host.find(':') != std::string::npos && !host.starts_with('['))
            host = "[" + host + "]"; // an IPv6 literal
        const auto USER = specValue(m, "user");
        const auto PORT = specValue(m, "port");
        const auto AUTH = (USER.empty() ? std::string{} : escape(USER, false) + "@") + host + (PORT.empty() ? std::string{} : ":" + PORT);
        const auto PATH = escape(m.prefix.empty() ? std::string{"/"} : m.prefix, true);

        if (TYPE == "smb-share" && !host.empty() && !specValue(m, "share").empty())
            return "smb://" + AUTH + "/" + escape(specValue(m, "share"), false) + "/";
        if (TYPE == "smb-server" && !host.empty())
            return "smb://" + AUTH + "/";
        if ((TYPE == "sftp" || TYPE == "ftp" || TYPE == "ftps") && !host.empty())
            return TYPE + "://" + AUTH + PATH;
        if (TYPE == "dav" && !host.empty())
            return std::string{specValue(m, "ssl") == "true" ? "davs" : "dav"} + "://" + AUTH + PATH;
        if (TYPE == "afp-volume" && !host.empty() && !specValue(m, "volume").empty())
            return "afp://" + AUTH + "/" + escape(specValue(m, "volume"), false) + "/";
        if (TYPE == "nfs" && !host.empty())
            return "nfs://" + AUTH + PATH;
        if (!m.fuse.empty())
            return "file://" + escape(m.fuse, true);
        return std::nullopt;
    }
}
