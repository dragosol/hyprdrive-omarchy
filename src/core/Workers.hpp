#pragma once

// Every thread this plugin starts, owned in one place (2026-11-11).
//
// The plugin is a shared object loaded into the compositor, and hyprpm unloads
// and reloads it in-process. A thread still running when dlclose unmaps the
// code runs into unmapped memory, and the compositor goes with it -- Empty
// Trash used to start a DETACHED thread that could do exactly that. So no
// thread here is detached: each is started through Workers::start (a handle
// the module keeps, like a std::thread) or Workers::launch (owned here, for a
// job nobody waits on), and PLUGIN_EXIT joins them all.
//
// THE SHUTDOWN POLICY. PLUGIN_EXIT calls beginShutdown() first: new starts are
// refused, every worker's stop_token is set, and one deadline is fixed for the
// whole exit (SHUTDOWN_BUDGET from that moment, shared, not per worker). Each
// module's own shutdown still signals its worker its own way (a flag, a killed
// child, a cancelled GCancellable) and then join()s, which waits only until
// that deadline; finishShutdown() at the very end joins whatever is left
// (Empty Trash) the same way.
//
// A worker still running at the deadline is ABANDONED, never killed (no thread
// can be killed safely): it is detached, and the plugin's own shared object is
// pinned (RTLD_NODELETE), so Hyprland's dlclose leaves the code mapped and the
// thread finishes into code that is still there. Its module skips the teardown
// it would have done after the join (join() returned false) and leaks that
// state to it instead. What that costs: the .so stays in memory until Hyprland
// exits, and a reload in the same session dlopens the same, already-mapped
// image (its globals as they were left) rather than a fresh one. An abandoned
// handle stays joinable() until its thread really ends, so a module never
// starts a second worker over one that is still running.
//
// Outside a shutdown join() waits as long as it takes, as std::thread::join
// did. Nothing here names a compositor type: the host tests link it as is.

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>

namespace Hyprdrive::Workers {

    namespace detail {
        struct SState;
    }

    // Two seconds for ALL of PLUGIN_EXIT's joins together. Every worker but
    // one reacts to its stop in milliseconds (a condition variable, a killed
    // child, a cancelled read); the slow cases are a file operation between
    // two stop checks (Empty Trash, a directory read on a sleeping disk) and
    // a URL drop's curl (--max-time 20). Long enough for the first, and short
    // enough that an exit or a reload is never felt as a hang.
    inline constexpr std::chrono::milliseconds SHUTDOWN_BUDGET{2000};

    class Worker {
      public:
        Worker() = default;
        Worker(Worker&&) noexcept;
        Worker& operator=(Worker&&) noexcept; // over a running one: the handle goes, the thread stays owned here
        Worker(const Worker&)            = delete;
        Worker& operator=(const Worker&) = delete;
        ~Worker();                            // never joins and never terminates

        // Started and not yet joined; an abandoned worker stays joinable until
        // its thread has really ended.
        bool            joinable() const;
        // The body returned (it may not be joined yet).
        bool            done() const;
        void            requestStop();
        std::stop_token stopToken() const;
        // Joins: forever outside a shutdown, until the shutdown deadline inside
        // one -- false when the worker was still running then and has been
        // abandoned (see above). True for an empty or already joined handle.
        // On an abandoned one it never waits: true once its thread has ended.
        bool            join();
        // Waits at most `budget` and joins when it ended; false (and still
        // joinable) when it did not. Never abandons.
        bool            joinFor(std::chrono::milliseconds budget);

      private:
        friend Worker start(std::string name, std::function<void(std::stop_token)> body);
        explicit Worker(std::shared_ptr<detail::SState> state);
        std::shared_ptr<detail::SState> m_state;
    };

    // Starts `body` on a thread named `name` (the first 15 bytes, as top and gdb
    // show it). An empty Worker, and nothing thrown, when it is refused (during
    // or after a shutdown, or while an abandoned worker of the same name still
    // runs) or the thread cannot be created. A body that throws
    // ends its worker, logged, instead of std::terminate-ing the compositor.
    Worker start(std::string name, std::function<void(std::stop_token)> body);
    // The same, for a job nobody keeps a handle on: owned here, joined once it
    // ended (at the next start) or at finishShutdown. False when refused.
    bool   launch(std::string name, std::function<void(std::stop_token)> body);

    // PLUGIN_EXIT, first: refuse new starts, request every stop, fix the deadline.
    void   beginShutdown(std::chrono::milliseconds budget = SHUTDOWN_BUDGET);
    // PLUGIN_EXIT, last: join everything left within the deadline. Returns how
    // many workers are abandoned (still running) -- 0 is a clean exit.
    size_t finishShutdown();
    // PLUGIN_INIT: accept starts again (a fresh image already does; a pinned
    // one being re-initialised needs it).
    void   reopen();

    bool   accepting();
    size_t live();      // started and not yet joined (abandoned and still running included)
    size_t abandoned(); // abandoned and still running

    // Where a worker's failure or an abandonment is reported (default stderr).
    // Called from any thread.
    void   setLog(void (*log)(const std::string& line));
    // What abandoning does to keep the code mapped (default: pin this shared
    // object). Host tests replace it to count.
    void   setPin(void (*pin)());
}
