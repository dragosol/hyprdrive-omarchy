#pragma once

#include <cstdint>
#include <chrono>
#include <string>
#include <vector>

// OVERVIEW_CPP_SPEC §13.7 X19. The GNOME half of the search: an
// org.gnome.Shell.SearchProvider2 client, so an app that ships a provider is
// searchable without a line of code here.
//
// ASYNC BY CONSTRUCTION. org.gnome.Weather is installed in the test VM and
// activates but never replies -- 28 s to the D-Bus default timeout. A blocking
// client freezes the overview on the first keystroke, so every call lives on a
// worker thread with its own reply deadline and the render thread only ever
// takes a snapshot. See share/X19_PROBE_searchprovider2.md.
namespace SearchProviders {
    struct SProvider {
        std::string desktopId;  // DesktopId= minus .desktop
        std::string busName;    // BusName=
        std::string objectPath; // ObjectPath=
        int         version = 2;
    };

    struct SResult {
        std::string providerBus;
        std::string providerDesktopId;
        std::string id;
        std::string name;
        std::string description;
        std::string icon;          // icon NAME or absolute path; feed to AppSearch::resolveIconPath
        std::string clipboardText; // Calculator returns the bare answer here
        size_t      providerRank = 0;
        // X20: carried over from the previous query while this provider's answer
        // for the current one is still in flight.
        bool        stale = false;
    };

    // Scans both search-providers dirs on first call.
    const std::vector<SProvider>& providers();
    void                          refresh();

    // Called from the render/input thread on every keystroke. Non-blocking:
    // starts a new generation, abandons whatever the previous one had in
    // flight, returns immediately. Empty query clears.
    void query(const std::string& text);

    // Launches every provider's D-Bus service without searching. NOT called
    // at overview open any more (X54, reversed, in main.cpp): that cold-started
    // GTK apps on every open, during the animation. The first keystroke's
    // query starts a cold provider itself; the SP_BENCH test still uses this.
    void warm();

    // Snapshot of whatever has answered so far for the newest query, ordered by
    // provider then by the provider's own ranking. Safe to call every frame.
    std::vector<SResult> results();

    // ActivateResult on the owning provider. Detached, like AppSearch::launch.
    void activate(const SResult& result, const std::string& queryText);

    // Stops the worker. Idempotent; safe if never started, and safe after a
    // worker that ended on its own because there was no session bus.
    void shutdown();

    // How many times the worker found no session bus (sd_bus_open_user
    // failed). With no bus there are simply no provider rows; nothing throws.
    uint64_t busFailures();

    // After a failed connect, the next attempt waits this long (default 5 s).
    // For scripts/search_providers_nobus_test.cpp, which sets 0 to drive the
    // worker through many start/fail/join cycles in a second.
    void setBusRetry(std::chrono::milliseconds retry);
}
