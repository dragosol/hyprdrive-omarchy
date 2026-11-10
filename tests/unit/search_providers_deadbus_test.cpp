// Host test: SearchProviders when the session bus DIES under a running worker
// (review 2026-11-09). A private dbus-daemon stands in for the session bus and
// is killed while the worker is connected to it. The worker used to spin at
// 100% CPU on the closed connection (no fd for poll() to wait on, no timeout)
// and never connected again. Here its CPU time over half a second must stay
// near zero, and once a bus is back at the same address the next search must
// reconnect. Skipped (passes) where dbus-daemon is not installed.
#include "SearchProviders.hpp"

#include <systemd/sd-bus.h>

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fcntl.h>
#include <filesystem>
#include <spawn.h>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char** environ;

static int g_checks = 0, g_failed = 0;
#define CHECK(c)                                                                                                                                                                   \
    do {                                                                                                                                                                           \
        ++g_checks;                                                                                                                                                                \
        if (!(c)) {                                                                                                                                                                \
            ++g_failed;                                                                                                                                                            \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c);                                                                                                               \
        }                                                                                                                                                                          \
    } while (0)

// Connections on the bus (unique names), this probe's own included; -1 when
// there is no bus to ask.
static int connections() {
    sd_bus* bus = nullptr;
    if (sd_bus_open_user(&bus) < 0)
        return -1;
    sd_bus_message* reply = nullptr;
    sd_bus_error    error = SD_BUS_ERROR_NULL;
    int             n     = -1;
    if (sd_bus_call_method(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "ListNames", &error, &reply, "") >= 0) {
        char** names = nullptr;
        if (sd_bus_message_read_strv(reply, &names) >= 0 && names) {
            n = 0;
            for (char** p = names; *p; ++p) {
                n += (*p)[0] == ':';
                free(*p);
            }
            free(names);
        }
    }
    sd_bus_message_unref(reply);
    sd_bus_error_free(&error);
    sd_bus_flush_close_unref(bus);
    return n;
}

static bool waitFor(int want) {
    for (int i = 0; i < 400; ++i) {
        if (connections() == want)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

// dbus-daemon on `socket`, answering; 0 if it cannot be started.
static pid_t startBus(const std::string& socket) {
    std::filesystem::remove(socket);
    const std::string          ADDRESS = "--address=unix:path=" + socket;
    const char*                argv[]  = {"dbus-daemon", "--session", "--nofork", "--nopidfile", ADDRESS.c_str(), nullptr};
    posix_spawn_file_actions_t files;
    posix_spawn_file_actions_init(&files);
    posix_spawn_file_actions_addopen(&files, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&files, 2, "/dev/null", O_WRONLY, 0);
    pid_t      pid = 0;
    const bool OK  = posix_spawnp(&pid, "dbus-daemon", &files, nullptr, const_cast<char**>(argv), environ) == 0;
    posix_spawn_file_actions_destroy(&files);
    if (!OK)
        return 0;
    for (int i = 0; i < 400; ++i) {
        if (connections() >= 1)
            return pid;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    kill(pid, SIGKILL);
    waitpid(pid, nullptr, 0);
    return 0;
}

static void killBus(pid_t pid) {
    kill(pid, SIGKILL);
    waitpid(pid, nullptr, 0);
}

static double cpuMs() {
    timespec t{};
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t);
    return static_cast<double>(t.tv_sec) * 1e3 + static_cast<double>(t.tv_nsec) / 1e6;
}

int main() {
    std::string made = (std::filesystem::temp_directory_path() / "sp_deadbus_test.XXXXXX").string();
    if (!mkdtemp(made.data())) {
        std::printf("FAIL mkdtemp\n");
        return 1;
    }
    const std::string DIR    = made;
    const std::string SOCKET = DIR + "/bus";
    setenv("DBUS_SESSION_BUS_ADDRESS", ("unix:path=" + SOCKET).c_str(), 1);
    // No search providers anywhere: only the worker's connection is under test.
    setenv("XDG_DATA_HOME", DIR.c_str(), 1);
    setenv("XDG_DATA_DIRS", DIR.c_str(), 1);

    pid_t bus = startBus(SOCKET);
    if (!bus) {
        std::filesystem::remove_all(DIR);
        std::printf("skipped: no dbus-daemon to stand in for the session bus\n");
        return 0;
    }

    using namespace SearchProviders;
    setBusRetry(std::chrono::milliseconds(0));

    query("first");
    CHECK(waitFor(2)); // the probe and the worker

    // The bus dies under the connected worker.
    killBus(bus);
    std::this_thread::sleep_for(std::chrono::milliseconds(300)); // time to notice
    const double BEFORE = cpuMs();
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    const double SPENT = cpuMs() - BEFORE;
    CHECK(SPENT < 100.0); // a spinning worker burns ~600 ms here

    // A bus at the same address again: the next search connects to it.
    bus = startBus(SOCKET);
    CHECK(bus != 0);
    query("second");
    CHECK(waitFor(2));

    shutdown();
    if (bus)
        killBus(bus);
    std::filesystem::remove_all(DIR);

    std::printf("%d checks, %d failed (%.0f ms CPU in the 600 ms after the bus died)\n", g_checks, g_failed, SPENT);
    return g_failed ? 1 : 0;
}
