#pragma once

// A MONITOR'S ROW OF DESKTOPS (user, 2026-11-11: "per monitor workspace blocks
// - adopt but don't keep empty workspaces generated on config switch"; and on a
// full block: "no limit").
//
// Omarchy 4.5 gives every monitor a BLOCK of desktop numbers
// (default/hypr/monitor-workspaces.lua: block 0 = 1-10, block 1 = 11-20, ...,
// keyed by the monitor in ~/.local/state/omarchy/monitor-workspaces) and
// renumbers into it on every config reload and monitor change. Hyprdrive used
// one global row 1..N and renumbered back: the two fought. Now each monitor has
// a Row, and Hyprdrive only ever numbers a desktop inside the row of the
// monitor it is on (DesktopPlan::check, Verdict::OutsideRow). Omarchy's
// reconcile never moves a desktop that already sits in its own monitor's block,
// so every layout Hyprdrive makes is a fixed point of it: no fight.
//
// A row is SLOTS: slot 1..capacity are the block's own ids (base+1 ..
// base+capacity); slot capacity+1, +2, ... are the row's OVERFLOW, ids
// OVERFLOW_ORIGIN + block*OVERFLOW_SPAN + 1, +2, ... A full block still takes
// an 11th desktop ("no limit"), and it can never take another monitor's first
// id (block*10+11 is the next block's). Omarchy sees an overflow id as a guest
// of a block no monitor owns (block_of_id = (id-1)//10 is ~100000+): merge()'s
// append_slot finds no room while the block is full, so it stays put; once the
// block has room Hyprdrive's own compaction pulls it in first, in order --
// where append_slot would put it.
//
// With blocks off (no Omarchy, Omarchy before 4.5, omarchy.lua not loaded)
// every monitor has the IMPLICIT row: base 0, unbounded, slot == id -- one
// shared global number line, exactly the pre-blocks behaviour.
//
// No Hyprland types: tests/unit/row_test.cpp.

#include <cstdint>

namespace Hyprdrive {
    struct Row {
        using Id = long long;

        // Overflow ids: far above any block Omarchy hands out (lowest free
        // block first, one per monitor ever seen), one span per block.
        static constexpr Id OVERFLOW_ORIGIN = 1'000'000;
        static constexpr Id OVERFLOW_SPAN   = 1'000;

        Id   base        = 0;
        Id   capacity    = 0; // the block's own slots; 0 = the implicit, unbounded row
        Id   overflow    = 0; // the id before the first overflow id (bounded rows)
        bool provisional = false; // block guessed (the monitor is not in Omarchy's file yet)

        static constexpr Row block(Id index, Id slots, bool provisional = false) {
            return {.base = index * slots, .capacity = slots, .overflow = OVERFLOW_ORIGIN + index * OVERFLOW_SPAN, .provisional = provisional};
        }

        constexpr bool bounded() const {
            return capacity > 0;
        }
        constexpr Id first() const {
            return base + 1;
        }
        // The highest slot a row can have (the implicit row: none).
        constexpr Id lastSlot() const {
            return bounded() ? capacity + OVERFLOW_SPAN : INT64_MAX;
        }
        // The desktop's place in the row, 1-based; 0 when the id is not in it.
        constexpr Id slot(Id id) const {
            if (!bounded())
                return id >= 1 ? id - base : 0;
            if (id > base && id <= base + capacity)
                return id - base;
            if (id > overflow && id <= overflow + OVERFLOW_SPAN)
                return capacity + (id - overflow);
            return 0;
        }
        constexpr bool contains(Id id) const {
            return slot(id) > 0;
        }
        // In the block itself (not its overflow).
        constexpr bool inBlock(Id id) const {
            const Id S = slot(id);
            return S > 0 && (!bounded() || S <= capacity);
        }
        constexpr bool inOverflow(Id id) const {
            return bounded() && slot(id) > capacity;
        }
        // The id of a slot. A slot below 1 gives base + slot (below the row:
        // DesktopPlan::check refuses it), one past the overflow gives 0.
        constexpr Id idOf(Id s) const {
            if (!bounded() || s <= capacity)
                return base + s;
            if (s <= capacity + OVERFLOW_SPAN)
                return overflow + (s - capacity);
            return 0;
        }
        // Where an id falls on this row's slot line for a range ("every desktop
        // from `from` on"): its slot. The implicit row also takes ids below 1
        // as they are (shift(0, -1) must stay refusable as BelowOne); a bounded
        // row has no place for an id outside it: nullopt-like 0.
        constexpr Id rank(Id id) const {
            if (!bounded())
                return id;
            return slot(id);
        }
    };

    constexpr bool sameRow(const Row& a, const Row& b) {
        return a.base == b.base && a.capacity == b.capacity && a.overflow == b.overflow;
    }
}
