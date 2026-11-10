#pragma once

// A PATH GOES TO THE FILE MANAGER AS A REAL URI (review 2026-11-09).
// Locations.cpp glued "file://" onto paths: a '#' in a name turned the rest
// into a fragment (GLib refuses such a URI outright), a "%41" in a name came
// back as "A" -- another file -- and Show in Files handed its URI to gdbus as
// GVariant TEXT, where a ' or a \ in the name broke the parse and the call
// silently did nothing. A path now becomes a URI the way GIO and every file
// manager spell it, and ShowItems is marshalled by sd-bus, so no file name
// can break the call.
//
// tests/unit/file_uri_test.cpp runs both on the host; a private dbus-daemon and
// a stand-in FileManager1 take the call.

#include <glib.h>
#include <systemd/sd-bus.h>

#include <string>

namespace ScrollOverview::FileUri {

    // An absolute path as a percent-encoded file:// URI -- or the old spelling
    // where GLib refuses (a relative path), which is no worse than before.
    inline std::string of(const std::string& path) {
        gchar*            uri = g_filename_to_uri(path.c_str(), nullptr, nullptr);
        const std::string OUT = uri ? std::string{uri} : "file://" + path;
        g_free(uri);
        return OUT;
    }

    // org.freedesktop.FileManager1.ShowItems([uri], ""): the folder holding it,
    // with it selected. Sent and not waited for, as Locations' EmptyTrash: the
    // bus may have to start the file manager first, and this runs on the
    // compositor thread. Flushed, since nothing else pumps this connection.
    inline bool showItems(sd_bus* bus, const std::string& uri) {
        sd_bus_message* msg = nullptr;
        if (!bus ||
            sd_bus_message_new_method_call(bus, &msg, "org.freedesktop.FileManager1", "/org/freedesktop/FileManager1", "org.freedesktop.FileManager1", "ShowItems") < 0 ||
            !msg)
            return false;
        const char* URIS[] = {uri.c_str(), nullptr};
        bool        ok     = sd_bus_message_append_strv(msg, const_cast<char**>(URIS)) >= 0 && sd_bus_message_append(msg, "s", "") >= 0;
        if (ok) {
            sd_bus_message_set_expect_reply(msg, 0);
            ok = sd_bus_send(bus, msg, nullptr) >= 0;
            if (ok)
                sd_bus_flush(bus);
        }
        sd_bus_message_unref(msg);
        return ok;
    }
}
