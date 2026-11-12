#pragma once

// A FULLSCREEN ROUND TRIP PUTS EVERYTHING BACK (owner's design, Fullscreen
// [11-05..11-09]: "exit puts everything back"; gate fs_trip).
//
// On a Mosaic desktop a window going fullscreen leaves for a desktop of its own
// (FullscreenDesktop.hpp) and the others close up behind it. Coming home it
// used to arrive like any window put down -- at the END of the arrival order,
// and the packer reads the order (MosaicPack: any-order, not order-blind):
// a1 207,242 + a2 1117,342 came back as a2 207,342 + a1 817,242.
//
// So the home desktop remembers, when the trip starts, where in the order the
// window stood (its entry, its tile in the hybrid tiling's tree, and the
// arrangement in force) and, when the SAME window comes home from that trip,
// puts it back there -- provided nothing changed on that desktop meanwhile: the
// windows still there are the same, in the same order, at the same sizes, and
// nobody opened, closed, dropped, swapped, moved or resized a window there.
// If anything did, the normal Mosaic rules apply (the window arrives as any
// window does). The pure part is this file (host test: mosaic_trip_hold_test).
//
// THE APP'S OWN WINDOWS ARE NOT AN EDIT (user, 2026-11-11: "battle.net
// struggling exiting in and out of fullscreen"). A launcher (Battle.net:
// Wine/CEF) maps and unmaps helper windows of its own -- same class, same pid
// -- around its fullscreen changes; one of them closing at home while the
// launcher was away voided the hold, and after a few quick toggles the
// launcher came home last: on the other side of the browser, for good. So the
// traveller's app's other windows (sameApp) neither edit the desktop nor
// consume the hold, and are left out of "the same windows": only another
// app's window opened, closed, moved or resized there, a drag, a swap, or the
// traveller moved home by hand voids it. Its place is counted among the
// windows that are not its app's (homeIndex).
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Hyprdrive::MosaicTripHold {
    // Which app a window is: its pid, its class. One app = one process (the
    // same pid: a launcher and its helpers); only when a pid is not known (a
    // Wayland window being unmapped has none any more) the same class. Not
    // "the same class" alone: two terminals of one class are two apps, and
    // one of them closing at home is an edit.
    struct SApp {
        long long   pid = 0;
        std::string cls;
        bool        operator==(const SApp&) const = default;
    };
    inline bool sameApp(const SApp& a, const SApp& b) {
        if (a.pid > 0 && b.pid > 0)
            return a.pid == b.pid;
        return !a.cls.empty() && a.cls == b.cls;
    }

    // One window of the order, as far as "did anything change" goes.
    struct SKey {
        const void* who = nullptr; // the layout target
        double      w = 0, h = 0;  // its mosaic size
        long long   id = 0;        // its tile
        SApp        app;
        bool        operator==(const SKey&) const = default;
    };

    // The home desktop when the trip started, the traveller taken out.
    struct SHold {
        const void*       traveller = nullptr;
        SApp              app;           // the traveller's
        std::size_t       index = 0;     // its place among the windows not of its app
        std::vector<SKey> rest;          // everyone else, in order
        std::uint64_t     edits = 0;     // the desktop's edit count after it left
    };

    // Does something this window does on the desktop (opens, closes, arrives,
    // leaves, is resized) change the desktop the hold keeps? Not when it is
    // one of the traveller's app's own windows.
    inline bool isEdit(const SHold* h, const SApp& who) {
        return !h || !sameApp(h->app, who);
    }

    // An order without the traveller's app's windows.
    inline std::vector<SKey> withoutApp(const SHold& h, const std::vector<SKey>& keys) {
        std::vector<SKey> out;
        for (const auto& K : keys)
            if (!sameApp(h.app, K.app))
                out.push_back(K);
        return out;
    }

    // A window arrives at the desktop the hold is kept for. RESTORE: it is the
    // traveller, back to what it left -- put it back. SIBLING: another window
    // of its app -- it arrives as any window, the hold is KEPT. Every other
    // answer consumes the hold: the window (the traveller, or another app's)
    // arrives as any window; why() says which check differed.
    enum class EHome { Restore, Sibling, Other, ByHand, Edited, Arranged, Changed, Tiles };
    inline const char* why(EHome e) {
        switch (e) {
            case EHome::Restore: return "restored";
            case EHome::Sibling: return "a window of its app: the hold stays";
            case EHome::Other: return "another window";
            case EHome::ByHand: return "moved home by hand, not by its trip";
            case EHome::Edited: return "the desktop was edited (a window of another app opened, closed or was moved or resized there)";
            case EHome::Arranged: return "a drop or a swap set an arrangement";
            case EHome::Changed: return "the windows there are not the same, in the same order, at the same sizes";
            case EHome::Tiles: return "in the tiles, and its app's windows there changed (their tiles are not the kept tree's)";
        }
        return "?";
    }
    // `fromTrip`: it is carried home by its fullscreen trip (not moved there
    // by hand). `now`: the order as it is; `edits`: the desktop's edit count
    // now; `arranged`: a drop or a swap set an arrangement meanwhile; `tiles`:
    // the desktop is in the tiles (the kept tree only has the windows that
    // were there).
    inline EHome decide(const SHold& h, const void* arriving, const SApp& app, bool fromTrip, const std::vector<SKey>& now, std::uint64_t edits, bool arranged,
                        bool tiles = false) {
        if (!arriving)
            return EHome::Other;
        if (h.traveller != arriving)
            return sameApp(h.app, app) ? EHome::Sibling : EHome::Other;
        if (!fromTrip)
            return EHome::ByHand;
        if (edits != h.edits)
            return EHome::Edited;
        if (arranged)
            return EHome::Arranged;
        if (withoutApp(h, now) != withoutApp(h, h.rest))
            return EHome::Changed;
        if (tiles && now != h.rest)
            return EHome::Tiles;
        return EHome::Restore;
    }
    inline bool restorable(const SHold& h, const void* arriving, bool fromTrip, const std::vector<SKey>& now, std::uint64_t edits, bool arranged) {
        return decide(h, arriving, h.app, fromTrip, now, edits, arranged) == EHome::Restore;
    }

    // Its place among the windows not of its app, counted from the order
    // when it leaves.
    inline std::size_t placeOf(const SHold& h, const std::vector<SKey>& before) {
        return withoutApp(h, before).size();
    }
    // Where it goes back into the order `now`: right after the index-th
    // window not of its app (the app's own windows in between stay before it).
    inline std::size_t homeIndex(const SHold& h, const std::vector<SKey>& now) {
        if (h.index == 0)
            return 0;
        std::size_t seen = 0;
        for (std::size_t i = 0; i < now.size(); ++i)
            if (!sameApp(h.app, now[i].app) && ++seen == h.index)
                return i + 1;
        return now.size();
    }
}
