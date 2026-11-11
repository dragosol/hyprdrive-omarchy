// Host test for Workers.hpp: the one owner of every thread the plugin starts.
// Start / stop / join, a stop while the body is busy, joining twice, a start
// after the shutdown refused, a worker that ignores its stop abandoned at the
// deadline (and the code pinned, here counted), a throwing body that does not
// terminate the process, and launch()'s fire-and-forget jobs joined at the end.
#include "Workers.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <thread>

using namespace Hyprdrive;
using namespace std::chrono_literals;

static int g_checks = 0, g_failed = 0;
#define CHECK(c)                                                                                                                                                                   \
    do {                                                                                                                                                                           \
        ++g_checks;                                                                                                                                                                \
        if (!(c)) {                                                                                                                                                                \
            ++g_failed;                                                                                                                                                            \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c);                                                                                                               \
        }                                                                                                                                                                          \
    } while (0)

static std::atomic<int> g_pins{0};
static std::atomic<int> g_logs{0};
static void countPin() {
    ++g_pins;
}
static void countLog(const std::string&) {
    ++g_logs;
}

static void waitFor(const auto& pred) {
    for (int i = 0; i < 400 && !pred(); ++i)
        std::this_thread::sleep_for(5ms);
}

int main() {
    Workers::setPin(countPin);
    Workers::setLog(countLog);

    // Start, run, join.
    {
        std::atomic<int> ran{0};
        auto             w = Workers::start("test-plain", [&](std::stop_token) { ++ran; });
        CHECK(w.joinable());
        CHECK(w.join());
        CHECK(ran == 1);
        CHECK(!w.joinable());
        CHECK(w.join()); // a second join is a no-op, not std::terminate
        CHECK(w.done());
    }

    // A stop while the body is busy: it sees the token and ends.
    {
        std::atomic<bool> inside{false};
        auto              w = Workers::start("test-busy", [&](std::stop_token st) {
            inside = true;
            while (!st.stop_requested())
                std::this_thread::sleep_for(1ms);
        });
        waitFor([&] { return inside.load(); });
        CHECK(inside);
        CHECK(!w.joinFor(20ms)); // still busy: not joined, not abandoned
        CHECK(w.joinable());
        w.requestStop();
        CHECK(w.joinFor(2s));
        CHECK(!w.joinable());
    }

    // Dropping a running handle neither joins nor terminates: the owner keeps
    // the thread, and the next start reaps it once it ended.
    {
        std::atomic<bool> go{false};
        {
            auto w = Workers::start("test-dropped", [&](std::stop_token) {
                while (!go)
                    std::this_thread::sleep_for(1ms);
            });
            CHECK(w.joinable());
            w = Workers::start("test-replaced", [](std::stop_token) {}); // assigning over a running one
            CHECK(w.join());
        }
        CHECK(Workers::live() >= 1);
        go = true;
        std::this_thread::sleep_for(50ms);
        CHECK(Workers::start("test-reaper", [](std::stop_token) {}).join()); // reaps the dropped one
        CHECK(Workers::live() == 0);
    }

    // A body that throws ends its worker, logged; the process lives.
    {
        const int BEFORE = g_logs;
        auto      w      = Workers::start("test-throws", [](std::stop_token) { throw std::runtime_error("boom"); });
        CHECK(w.join());
        CHECK(g_logs == BEFORE + 1);
    }

    // launch(): nobody keeps the handle; finishShutdown joins it.
    std::atomic<int> launched{0};
    CHECK(Workers::launch("test-launch", [&](std::stop_token st) {
        while (!st.stop_requested())
            std::this_thread::sleep_for(1ms);
        ++launched;
    }));
    CHECK(Workers::live() == 1);

    // The shutdown: every stop requested, a cooperative worker joins, starts
    // are refused, and one that ignores its stop is abandoned at the deadline.
    std::atomic<bool> release{false};
    auto              stubborn = Workers::start("test-stubborn", [&](std::stop_token) {
        while (!release)
            std::this_thread::sleep_for(1ms);
    });
    auto              polite   = Workers::start("test-polite", [](std::stop_token st) {
        while (!st.stop_requested())
            std::this_thread::sleep_for(1ms);
    });
    const auto T0 = std::chrono::steady_clock::now();
    Workers::beginShutdown(150ms);
    CHECK(!Workers::accepting());
    CHECK(polite.join());
    auto refused = Workers::start("test-refused", [](std::stop_token) {});
    CHECK(!refused.joinable());
    CHECK(!Workers::launch("test-refused", [](std::stop_token) {}));
    CHECK(!stubborn.join()); // abandoned at the deadline
    CHECK(g_pins == 1);
    CHECK(stubborn.joinable()); // still running: a module must not start another
    CHECK(!stubborn.join());    // and a second join does not wait again
    CHECK(Workers::finishShutdown() == 1);
    CHECK(Workers::finishShutdown() == 1); // a second finish is harmless
    CHECK(g_pins == 1);                    // pinned once, not per call
    const auto TOOK = std::chrono::steady_clock::now() - T0;
    CHECK(TOOK >= 140ms && TOOK < 1s); // bounded by the budget, not by the stubborn worker
    CHECK(launched == 1);
    CHECK(Workers::abandoned() == 1);

    // A re-initialised (pinned) image: no second worker of an abandoned name
    // while that one still runs; another name starts.
    Workers::reopen();
    CHECK(!Workers::start("test-stubborn", [](std::stop_token) {}).joinable());
    CHECK(Workers::start("test-other", [](std::stop_token) {}).join());

    // The abandoned thread ends on its own: no longer joinable, no longer live.
    release = true;
    waitFor([&] { return !stubborn.joinable(); });
    CHECK(!stubborn.joinable());
    CHECK(stubborn.join()); // ended: true, without waiting
    CHECK(Workers::abandoned() == 0);

    // PLUGIN_INIT again (a pinned image re-initialised): starts work again.
    Workers::reopen();
    CHECK(Workers::accepting());
    auto again = Workers::start("test-again", [](std::stop_token) {});
    CHECK(again.join());
    CHECK(Workers::finishShutdown() == 0);

    // An empty handle is harmless everywhere.
    Workers::Worker none;
    CHECK(!none.joinable());
    CHECK(none.join());
    none.requestStop();
    CHECK(!none.stopToken().stop_possible());

    std::printf("workers_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
