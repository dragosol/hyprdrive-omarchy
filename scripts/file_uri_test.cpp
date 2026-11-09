// FileUri.hpp (review 2026-11-09): file names that broke "file://" + path.
// A '#' made GLib refuse the URI, "%41" came back as "A", and Show in Files
// went out as gdbus GVariant text that a ' or \ broke. Every name below must
// survive the trip to a URI and back, and must reach a stand-in
// FileManager1.ShowItems on a private dbus-daemon exactly (that half is
// skipped where dbus-daemon is not installed).
#include "../src/FileUri.hpp"

#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <spawn.h>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

extern char** environ;
using ScrollOverview::FileUri::of;
using ScrollOverview::FileUri::showItems;

static int checks = 0, failed = 0;

static void expect(bool ok, const std::string& what) {
    ++checks;
    if (!ok) {
        ++failed;
        std::printf("FAIL %s\n", what.c_str());
    }
}

// What a URI path may not carry raw: a space, '#' (a fragment), '?' (a
// query), '"', '\', a '%' that does not start an escape, a non-ASCII byte.
// ('\'' is a legal sub-delimiter and GLib leaves it; sd-bus carries it fine.)
static bool cleanUri(const std::string& uri) {
    for (size_t i = 0; i < uri.size(); ++i) {
        const unsigned char C = uri[i];
        if (C <= ' ' || C >= 0x7f || C == '#' || C == '?' || C == '"' || C == '\\')
            return false;
        if (C == '%' && (i + 2 >= uri.size() || !std::isxdigit(static_cast<unsigned char>(uri[i + 1])) || !std::isxdigit(static_cast<unsigned char>(uri[i + 2]))))
            return false;
    }
    return true;
}

static std::string decoded(const std::string& uri) {
    gchar*            p   = g_filename_from_uri(uri.c_str(), nullptr, nullptr);
    const std::string OUT = p ? p : "<refused>";
    g_free(p);
    return OUT;
}

// ---- a private session bus, and a file manager on it ----------------------
static pid_t startBus(const std::string& socket) {
    const std::string          ADDRESS = "--address=unix:path=" + socket;
    const char*                argv[]  = {"dbus-daemon", "--session", "--nofork", "--nopidfile", ADDRESS.c_str(), nullptr};
    posix_spawn_file_actions_t files;
    posix_spawn_file_actions_init(&files);
    posix_spawn_file_actions_addopen(&files, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&files, 2, "/dev/null", O_WRONLY, 0);
    pid_t      pid = 0;
    const bool OK  = posix_spawnp(&pid, "dbus-daemon", &files, nullptr, const_cast<char**>(argv), environ) == 0;
    posix_spawn_file_actions_destroy(&files);
    return OK ? pid : 0;
}

static std::vector<std::string> g_shown;

static int onShowItems(sd_bus_message* m, void*, sd_bus_error*) {
    char** uris = nullptr;
    if (sd_bus_message_read_strv(m, &uris) >= 0 && uris) {
        for (char** u = uris; *u; ++u) {
            g_shown.emplace_back(*u);
            free(*u);
        }
        free(uris);
    }
    return sd_bus_reply_method_return(m, nullptr);
}

static const sd_bus_vtable FILE_MANAGER[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("ShowItems", "ass", "", onShowItems, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END,
};

int main() {
    const std::vector<std::string> NAMES = {
        "/home/u/it's here.txt", "/home/u/back\\slash", "/home/u/#1 hit.mp3", "/home/u/100%41.pdf", "/home/u/with space/and more",
        "/home/u/caf\xc3\xa9 \xe2\x84\x96 1",  // UTF-8
        "/home/u/latin1 \xe9",                 // not UTF-8 at all: still a file name
        "/home/u/[brackets] {braces} ?q=1&x",
    };

    // ---- the URI and back --------------------------------------------------
    for (const auto& P : NAMES) {
        const auto URI = of(P);
        expect(URI.starts_with("file:///"), "[" + P + "] is a file:// URI: " + URI);
        expect(cleanUri(URI), "[" + P + "] has nothing left unescaped: " + URI);
        expect(decoded(URI) == P, "[" + P + "] comes back as itself, not [" + decoded(URI) + "]");
    }
    expect(of("relative/path") == "file://relative/path", "a relative path keeps the old spelling");

    // ---- ShowItems, as the file manager receives it ------------------------
    std::string made = (std::filesystem::temp_directory_path() / "file_uri_test.XXXXXX").string();
    if (!mkdtemp(made.data())) {
        std::printf("FAIL mkdtemp\n");
        return 1;
    }
    const std::string SOCKET = made + "/bus";
    setenv("DBUS_SESSION_BUS_ADDRESS", ("unix:path=" + SOCKET).c_str(), 1);
    const pid_t BUS = startBus(SOCKET);
    sd_bus*     fm  = nullptr;
    for (int i = 0; BUS && i < 400; ++i) {
        if (sd_bus_open_user(&fm) >= 0)
            break;
        fm = nullptr;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const char* SKIPPED = "";
    if (!fm)
        SKIPPED = " (ShowItems skipped: no dbus-daemon)";
    else {
        sd_bus* dock = nullptr;
        const bool READY = sd_bus_add_object_vtable(fm, nullptr, "/org/freedesktop/FileManager1", "org.freedesktop.FileManager1", FILE_MANAGER, nullptr) >= 0 &&
            sd_bus_request_name(fm, "org.freedesktop.FileManager1", 0) >= 0 && sd_bus_open_user(&dock) >= 0;
        expect(READY, "a stand-in file manager on the private bus");
        if (READY) {
            for (const auto& P : NAMES)
                expect(showItems(dock, of(P)), "[" + P + "] ShowItems sent");
            for (int i = 0; i < 400 && g_shown.size() < NAMES.size(); ++i) {
                while (sd_bus_process(fm, nullptr) > 0) { }
                sd_bus_wait(fm, 5'000);
            }
            expect(g_shown.size() == NAMES.size(), "every ShowItems arrived (" + std::to_string(g_shown.size()) + " of " + std::to_string(NAMES.size()) + ")");
            for (size_t i = 0; i < std::min(g_shown.size(), NAMES.size()); ++i)
                expect(decoded(g_shown[i]) == NAMES[i], "[" + NAMES[i] + "] reached the file manager as itself");
        }
        if (dock)
            sd_bus_flush_close_unref(dock);
        sd_bus_flush_close_unref(fm);
    }
    if (BUS) {
        kill(BUS, SIGKILL);
        waitpid(BUS, nullptr, 0);
    }
    std::filesystem::remove_all(made);

    std::printf("file_uri_test: %d checks, %d failed%s\n", checks, failed, SKIPPED);
    return failed ? 1 : 0;
}
