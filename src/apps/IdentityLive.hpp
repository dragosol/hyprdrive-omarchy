#pragma once

// AppIdentity on the compositor thread (AppIdentity design, (d) "compositor-
// thread API"): which app each window is, answered at once and never by
// waiting. At openEarly a window's in-memory facts go through the FAST tier
// (resolveFast over the published indexes: today's answer or better, Steam
// numeric classes already named) and a job goes to the `hd-identity` worker
// (Engine.hpp), which reads X properties, the real pid and /proc and
// resolves in full. Its result lands through an eventfd on the event loop;
// when it changes a window's app the generation is bumped, the dock and the
// overview rebuild (Membership), and an "Always Open in" rule the new app
// has is applied late, within 1.5 s of the map (owner ruling, Q8).
//
// CACHES. L1, per window (by address and a serial, so a reused address is a
// new window): the facts, the identity, provisional or resolved. L2, per
// (class, pid): what a sibling window of the same process resolved to, as the
// provisional answer of the next one (dropped when the process's last window
// goes). L3 is the worker's: the indexes and the PE icons on disk.
// Invalidated by: a class change (resolved again), a title change (only for
// the answers that came from the title), the window going, an inventory
// rescan (every window resolved again).
//
// Never blocks, never throws, never does I/O: everything that reads lives on
// the worker.

#include "AppIdentity.hpp"

#include <hyprland/src/desktop/DesktopTypes.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace Hyprdrive::AppIdentity {
    void init();
    void shutdown();

    // The window's identity: resolved, else provisional. Valid until the
    // next call into this module.
    const Identity& of(const PHLWINDOW& w);
    // The dock's grouping key (was AppSearch::identityClass(class, title)):
    // the class itself for an app a desktop file names (nothing changes for
    // ordinary apps), else "hd:<key>".
    std::string groupKey(const PHLWINDOW& w);
    // The last identity seen with this key ("hd:" optional), or nullptr.
    const Identity* forKey(std::string_view key);
    // canonicalKey / sameApp over the live app inventory.
    std::string canonical(std::string_view storedId);
    bool        sameApp(std::string_view a, std::string_view b);
    // Should Hyprdrive place it (tile, dock, overview)? Wine's systray
    // fallback and its popups are not placeable.
    bool     placeable(const PHLWINDOW& w);
    uint64_t generation();

    // `hyprdrive:debug identity`: every window's live answer, its facts and
    // the worker's state.
    std::string liveReport();
}
