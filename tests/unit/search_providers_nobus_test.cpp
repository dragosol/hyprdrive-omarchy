// Host test: SearchProviders with NO session bus (native port, 2026-10-14).
//
// The first headless run on fedora had no bus. sd_bus_open_user failed, the
// worker thread returned on its own and stayed joinable, and the next overview
// open assigned a new std::thread over it -- std::terminate, compositor gone.
// This drives exactly that sequence many times over (retry delay 0, so every
// call is a fresh start/fail/join cycle) and checks the rest of the contract:
// no crash, no fd leak, nothing queued runs later, shutdown is clean, and the
// default 5 s retry means repeated keystrokes cost one attempt, not one each.
#include "SearchProviders.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <thread>

static int g_checks = 0, g_failed = 0;
#define CHECK(c)                                                                                                                                                                   \
    do {                                                                                                                                                                           \
        ++g_checks;                                                                                                                                                                \
        if (!(c)) {                                                                                                                                                                \
            ++g_failed;                                                                                                                                                            \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c);                                                                                                               \
        }                                                                                                                                                                          \
    } while (0)

static size_t openFds() {
    size_t n = 0;
    for ([[maybe_unused]] const auto& E : std::filesystem::directory_iterator("/proc/self/fd"))
        ++n;
    return n;
}

// Wait (bounded) until the worker has reported `n` failures.
static bool waitFailures(uint64_t n) {
    for (int i = 0; i < 400; ++i) {
        if (SearchProviders::busFailures() >= n)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

int main() {
    // No bus anywhere: sd_bus_open_user reads these at call time.
    setenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent/hyprdrive-no-bus", 1);
    setenv("XDG_RUNTIME_DIR", "/nonexistent/hyprdrive-no-runtime", 1);

    using namespace SearchProviders;
    setBusRetry(std::chrono::milliseconds(0));

    query("first");
    CHECK(waitFailures(1));
    const size_t FDS = openFds();

    // The crash: every one of these used to replace a joinable std::thread.
    for (int i = 0; i < 50; ++i) {
        const uint64_t BEFORE = busFailures();
        if (i % 3 == 0)
            warm();
        else if (i % 3 == 1)
            query(i % 2 ? "abc" : "abcd");
        else
            activate(SResult{.providerBus = "org.example.Nope"}, "x"); // unknown provider: a no-op, must not start anything odd
        if (i % 3 != 2)
            CHECK(waitFailures(BEFORE + 1));
    }
    CHECK(openFds() <= FDS); // each retry closes the previous wake pipe
    CHECK(results().empty() || std::ranges::none_of(results(), [](const SResult& R) { return R.stale; }));

    // The default retry: a burst of keystrokes after a failure is ONE attempt.
    setBusRetry(std::chrono::seconds(5));
    query("burst");
    CHECK(waitFailures(busFailures())); // settled
    const uint64_t SETTLED = busFailures();
    for (const char* Q : {"b", "bu", "bur", "burs", "burst!"})
        query(Q);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(busFailures() == SETTLED);

    shutdown();
    shutdown(); // idempotent
    CHECK(openFds() == FDS - 2); // the last wake pipe is closed too

    std::printf("%d checks, %d failed (%llu no-bus attempts, none fatal)\n", g_checks, g_failed, static_cast<unsigned long long>(busFailures()));
    return g_failed ? 1 : 0;
}
