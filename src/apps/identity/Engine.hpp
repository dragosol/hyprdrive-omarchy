#pragma once

// The identity worker, `hd-identity` (AppIdentity design, (d) "Async flow").
// One thread, started through Workers, that does everything AppIdentity
// needs the disk, /proc or the X server for, so the compositor thread never
// does:
//
//   * THE INDEXES. It builds the Steam and Lutris indexes at start and
//     publishes them (SteamLibrary/LutrisLibrary::publish; the fast tier and
//     AppSearch read them, never build them). It rebuilds them when their
//     sources changed -- checked when a job arrives, at most once per
//     `recheckEvery`, by the mtimes of libraryfolders.vdf, each library
//     folder, shortcuts.vdf and pga.db (an event: a window mapping, not a
//     timer) -- and for a Steam id the index lacks, at most once per
//     SteamLibrary::NEGATIVE_TTL_NS. A library on a removable disk is stat'ed
//     first on a probe thread, and one that does not answer within
//     `probeTimeout` is skipped and remembered as failed for a minute: a
//     sleeping disk costs a missing game name, never a stalled worker.
//   * THE JOBS. A window's in-memory facts, posted by the compositor thread;
//     the worker adds what the Gatherer reads (X properties, the real pid,
//     /proc), resolves it in full (resolveWith) and posts the Result. The
//     compositor thread is woken through `wake` (an eventfd on its loop) and
//     collects them with take(); a Result names its window by address and a
//     serial, never by pointer, so one for a window that has gone in the
//     meantime is simply dropped there.
//   * THE SESSIONS. It remembers what each window it resolved belongs to
//     (sessionKey), so a helper (explorer.exe, a crash handler) joins the
//     strongest app of its session, and a helper resolved BEFORE its app was
//     is resolved again when the app arrives.
//
// Nothing here names a Hyprland type: the host tests run it on fixtures with
// a fake Gatherer. Never throws past its functions.

#include "AppIdentity.hpp"
#include "Workers.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Hyprdrive::AppIdentity {
    struct Job {
        uintptr_t   addr   = 0; // the window, as an address: never dereferenced off the compositor thread
        uint64_t    serial = 0; // which incarnation of it (an address can be reused)
        WindowFacts w;
        std::string display; // $DISPLAY as the compositor thread saw it ("" = no XWayland)
    };
    struct Result {
        uintptr_t    addr   = 0;
        uint64_t     serial = 0;
        WindowFacts  w; // with what the Gatherer added
        ProcessFacts p;
        Identity     id;
        std::string  session; // sessionKey(p)
    };

    // What the worker asks of the system for one window. Every call is on
    // the worker thread; each must be bounded.
    class Gatherer {
      public:
        virtual ~Gatherer() = default;
        // X properties (WM_CLASS instance, _NET_WM_ICON, STEAM_GAME) and the
        // real client pid (XRes) into `w`; nothing when there is no X server.
        virtual void window(WindowFacts& w, const std::string& display) {
            (void)w, (void)display;
        }
        // The process and its parents.
        virtual ProcessFacts process(pid_t pid) {
            (void)pid;
            return {};
        }
        // The worker is ending: close connections (still on the worker).
        virtual void close() {}
    };

    class Engine {
      public:
        struct Options {
            const Sources*            src = nullptr; // must outlive the worker (liveSources() does)
            std::shared_ptr<Gatherer> gather;        // null: window facts only
            std::function<void()>     wake;          // results or indexes are ready (worker thread)
            // Each Result, on the worker, before it is posted: where a newly
            // seen Wine program or Steam game is remembered (GameApps::record).
            std::function<void(const Result&)> record;
            std::chrono::milliseconds probeTimeout{2000};
            std::chrono::milliseconds recheckEvery{5000};
            std::chrono::milliseconds jobDelay{0}; // a slow source, for the stall gate and tests
            std::string               threadName = "hd-identity";
            bool                      publish    = true; // publish the indexes (false: tests keep them private)
        };
        explicit Engine(Options o);
        ~Engine(); // stop()
        Engine(const Engine&)            = delete;
        Engine& operator=(const Engine&) = delete;

        bool start();
        // Stops and joins (until the Workers shutdown deadline inside one).
        // False when the worker was abandoned still running.
        bool stop();
        bool running() const;

        void enqueue(Job j);
        // The window went: its queued job and its session memory go too.
        void cancel(uintptr_t addr);
        // The app inventory the resolvers match against (copied once per
        // change by the compositor thread).
        void setInventory(std::shared_ptr<const std::vector<InventoryEntry>> inv);
        // The sources may have changed: rebuild the indexes now.
        void rescan();

        std::vector<Result> take();
        // New indexes were published since the last call.
        bool takeIndexesChanged();

        struct Stats {
            size_t   queued = 0, sessions = 0;
            uint64_t resolved = 0, rebuilds = 0, probesFailed = 0;
            bool     indexed  = false;
        };
        Stats   stats() const;
        Indexes indexes() const; // the worker's own (published or not)

        struct SShared; // the worker's state, kept alive by the thread itself

      private:
        Options                  m_o;
        std::shared_ptr<SShared> m_s;
        Workers::Worker          m_worker;
    };
}
