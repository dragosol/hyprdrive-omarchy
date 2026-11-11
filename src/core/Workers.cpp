#include "Workers.hpp"

#include <dlfcn.h>
#include <pthread.h>

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <exception>
#include <mutex>
#include <optional>
#include <thread>
#include <typeinfo>
#include <utility>
#include <vector>

namespace Hyprdrive::Workers::detail {
    struct SState {
        std::string             name;
        std::stop_source        stop;
        std::thread             thread;
        // `done` under `mx`: the body returned (the thread is about to end).
        std::mutex              mx;
        std::condition_variable cv;
        bool                    done = false;
        // One joiner at a time (a module's join, a reap at the next start, the
        // final shutdown), and held while the thread is being created.
        std::mutex              joinMx;
        std::atomic<bool>       finished{false}; // `done`, readable without mx
        std::atomic<bool>       joined{false};
        std::atomic<bool>       abandoned{false};
    };
}

namespace {
    using Hyprdrive::Workers::detail::SState;
    using Clock    = std::chrono::steady_clock;
    using Deadline = std::optional<Clock::time_point>;

    void defaultLog(const std::string& line) {
        std::fprintf(stderr, "%s\n", line.c_str());
    }

    void report(const std::string& line);

    // Abandoning a worker: keep this shared object mapped for good. dlopen with
    // RTLD_NOLOAD finds the image already loaded (it loads nothing), takes a
    // reference that is never dropped, and RTLD_NODELETE marks it never to be
    // unloaded -- Hyprland's dlclose then leaves the code where the thread is.
    void pinSelf() {
        static std::once_flag once;
        std::call_once(once, [] {
            Dl_info info{};
            if (dladdr(reinterpret_cast<void*>(&pinSelf), &info) == 0 || !info.dli_fname) {
                report("[hyprdrive] workers: could not find the plugin's own file to keep it loaded");
                return;
            }
            if (!dlopen(info.dli_fname, RTLD_LAZY | RTLD_NOLOAD | RTLD_NODELETE))
                report(std::string("[hyprdrive] workers: could not keep ") + info.dli_fname + " loaded");
        });
    }

    std::mutex                           g_mx;
    std::vector<std::shared_ptr<SState>> g_all;
    bool                                 g_accepting = true;
    Deadline                             g_deadline;
    std::atomic<void (*)(const std::string&)> g_log{defaultLog};
    std::atomic<void (*)()>              g_pin{pinSelf};

    void report(const std::string& line) {
        try {
            g_log.load()(line);
        } catch (...) {}
    }

    Deadline currentDeadline() {
        std::scoped_lock lock(g_mx);
        return g_deadline;
    }

    void abandon(SState& s) {
        s.thread.detach();
        s.abandoned = true;
        report("[hyprdrive] worker \"" + s.name + "\" did not stop in time: left running, the plugin's code is kept loaded until Hyprland exits");
        g_pin.load()();
    }

    // Wait for the body (forever without a deadline), then join. A missed
    // deadline abandons the worker when `mayAbandon`, else leaves it be.
    bool joinUntil(SState& s, Deadline deadline, bool mayAbandon) {
        std::scoped_lock j(s.joinMx);
        if (s.joined)
            return true;
        if (s.abandoned)
            return s.finished; // never waited for again: it has ended or it has not
        {
            std::unique_lock l(s.mx);
            if (!deadline)
                s.cv.wait(l, [&] { return s.done; });
            else if (!s.cv.wait_until(l, *deadline, [&] { return s.done; })) {
                l.unlock();
                if (mayAbandon)
                    abandon(s);
                return false;
            }
        }
        s.thread.join(); // the body returned: only its last few instructions are left
        s.joined = true;
        return true;
    }

    // Join the workers that have ended and forget the joined ones. The joins
    // happen outside g_mx: one may wait for a module's join to let go.
    void reap() {
        std::vector<std::shared_ptr<SState>> ended;
        {
            std::scoped_lock lock(g_mx);
            for (const auto& S : g_all)
                if (S->finished && !S->abandoned && !S->joined)
                    ended.push_back(S);
        }
        for (const auto& S : ended)
            joinUntil(*S, std::nullopt, false);
        std::scoped_lock lock(g_mx);
        std::erase_if(g_all, [](const auto& S) { return S->joined || (S->abandoned && S->finished); });
    }
}

namespace Hyprdrive::Workers {

    Worker::Worker(std::shared_ptr<detail::SState> state) : m_state(std::move(state)) {}
    Worker::Worker(Worker&&) noexcept            = default;
    Worker& Worker::operator=(Worker&&) noexcept = default;
    Worker::~Worker()                            = default;

    bool Worker::joinable() const {
        return m_state && !m_state->joined && !(m_state->abandoned && m_state->finished);
    }

    bool Worker::done() const {
        return m_state && m_state->finished;
    }

    void Worker::requestStop() {
        if (m_state)
            m_state->stop.request_stop();
    }

    std::stop_token Worker::stopToken() const {
        return m_state ? m_state->stop.get_token() : std::stop_token{};
    }

    bool Worker::join() {
        if (!m_state)
            return true;
        const auto DEADLINE = currentDeadline();
        return joinUntil(*m_state, DEADLINE, DEADLINE.has_value());
    }

    bool Worker::joinFor(std::chrono::milliseconds budget) {
        if (!m_state)
            return true;
        return joinUntil(*m_state, Clock::now() + budget, false);
    }

    Worker start(std::string name, std::function<void(std::stop_token)> body) {
        reap();
        std::shared_ptr<detail::SState> s;
        try {
            s       = std::make_shared<detail::SState>();
            s->name = std::move(name);
            std::scoped_lock lock(g_mx);
            if (!g_accepting)
                return {};
            // A worker abandoned at an earlier unload of this same (pinned)
            // image and still running: its module's state is still its own,
            // so no second one of that name starts until it has ended.
            for (const auto& S : g_all)
                if (S->abandoned && !S->finished && S->name == s->name)
                    return {};
            // Registered before the thread exists, so a shutdown that begins
            // meanwhile requests its stop and waits for it (on joinMx, below).
            g_all.push_back(s);
        } catch (...) {
            return {};
        }
        std::scoped_lock creating(s->joinMx);
        try {
            s->thread = std::thread([s, body = std::move(body)]() mutable {
                try {
                    const std::string SHORT = s->name.substr(0, 15);
                    pthread_setname_np(pthread_self(), SHORT.c_str());
                    body(s->stop.get_token());
                } catch (const std::exception& e) {
                    report("[hyprdrive] worker \"" + s->name + "\" ended on " + typeid(e).name() + ": " + e.what());
                } catch (...) {
                    report("[hyprdrive] worker \"" + s->name + "\" ended on an unknown exception");
                }
                {
                    std::scoped_lock l(s->mx);
                    s->done     = true;
                    s->finished = true;
                }
                s->cv.notify_all();
            });
        } catch (...) {
            // No thread: nothing to wait for or join.
            {
                std::scoped_lock l(s->mx);
                s->done     = true;
                s->finished = true;
            }
            s->joined = true;
            s->cv.notify_all();
            report("[hyprdrive] worker \"" + s->name + "\" could not be started");
            return {};
        }
        return Worker(std::move(s));
    }

    bool launch(std::string name, std::function<void(std::stop_token)> body) {
        return start(std::move(name), std::move(body)).joinable();
    }

    void beginShutdown(std::chrono::milliseconds budget) {
        std::scoped_lock lock(g_mx);
        g_accepting = false;
        g_deadline  = Clock::now() + budget;
        for (const auto& S : g_all)
            S->stop.request_stop();
    }

    size_t finishShutdown() {
        std::vector<std::shared_ptr<SState>> all;
        Deadline                             deadline;
        {
            std::scoped_lock lock(g_mx);
            g_accepting = false;
            if (!g_deadline)
                g_deadline = Clock::now() + SHUTDOWN_BUDGET;
            deadline = g_deadline;
            for (const auto& S : g_all)
                S->stop.request_stop();
            all = g_all;
        }
        for (const auto& S : all)
            joinUntil(*S, deadline, true);
        std::scoped_lock lock(g_mx);
        std::erase_if(g_all, [](const auto& S) { return S->joined || (S->abandoned && S->finished); });
        size_t left = 0;
        for (const auto& S : g_all)
            left += S->abandoned && !S->finished;
        return left;
    }

    void reopen() {
        std::scoped_lock lock(g_mx);
        g_accepting = true;
        g_deadline.reset();
    }

    bool accepting() {
        std::scoped_lock lock(g_mx);
        return g_accepting;
    }

    size_t live() {
        std::scoped_lock lock(g_mx);
        size_t n = 0;
        for (const auto& S : g_all)
            n += !S->joined && !(S->abandoned && S->finished);
        return n;
    }

    size_t abandoned() {
        std::scoped_lock lock(g_mx);
        size_t n = 0;
        for (const auto& S : g_all)
            n += S->abandoned && !S->finished;
        return n;
    }

    void setLog(void (*log)(const std::string& line)) {
        g_log = log ? log : defaultLog;
    }

    void setPin(void (*pin)()) {
        g_pin = pin ? pin : pinSelf;
    }
}
