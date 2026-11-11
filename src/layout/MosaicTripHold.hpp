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
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace Hyprdrive::MosaicTripHold {
    // One window of the order, as far as "did anything change" goes.
    struct SKey {
        const void* who = nullptr; // the layout target
        double      w = 0, h = 0;  // its mosaic size
        long long   id = 0;        // its tile
        bool operator==(const SKey&) const = default;
    };

    // The home desktop when the trip started, the traveller taken out.
    struct SHold {
        const void*       traveller = nullptr;
        std::size_t       index     = 0; // its place in the order
        std::vector<SKey> rest;          // everyone else, in order
        std::uint64_t     edits     = 0; // the desktop's edit count after it left
    };

    // Is `arriving` coming home to exactly what it left? `fromTrip`: it is
    // carried home by its fullscreen trip (not moved there by hand). `now`:
    // the order as it is; `edits`: the desktop's edit count now; `arranged`:
    // a drop or a swap set an arrangement meanwhile.
    inline bool restorable(const SHold& h, const void* arriving, bool fromTrip, const std::vector<SKey>& now, std::uint64_t edits, bool arranged) {
        return fromTrip && arriving && h.traveller == arriving && edits == h.edits && !arranged && now == h.rest;
    }

    // Where it goes back into an order of `size` windows.
    inline std::size_t homeIndex(const SHold& h, std::size_t size) {
        return std::min(h.index, size);
    }
}
