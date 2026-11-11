#pragma once

// DESKTOP NUMBERS NEVER COLLIDE (user, 2026-11-11: "why do i have a fullscreen
// window in desktop 1 where other windows are? that should never happen").
//
// The pure half of DesktopOrder: a plan of new numbers is checked whole against
// every ordinary desktop before one number is written. A plan that would leave
// two desktops on one number, or a desktop below 1, is refused whole -- never
// half-applied (the old renumber skipped a move to 0 and applied the rest:
// shift(0, -1) turned 1,2,3 into 1,1,2).
//
// Desktops are (key, number): the key is the desktop's identity (its pointer in
// the plugin), never its number -- two desktops can share a number, and the plan
// must tell them apart. No Hyprland types here: tests/unit/desktop_plan_test.cpp.

#include "Row.hpp"

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Hyprdrive::DesktopPlan {
    using Key = std::uintptr_t;
    using Id  = long long;

    struct Desk {
        Key key     = 0;
        Id  id      = 0;
        int windows = 0; // mapped windows on it (heal keeps the fuller one)
        Row  row    = {};    // its monitor's row: every number it gets is in it
        bool pinned = false; // an Omarchy guest (a merged record): never renumbered
    };
    struct Move {
        Key key = 0;
        Id  to  = 0;
    };

    enum class Verdict {
        Ok,
        Unknown,   // a move names a desktop that is not an ordinary one
        Twice,     // a desktop moved twice in one plan
        BelowOne,  // a desktop would get a number below 1
        Collision, // a moved desktop would share its number with another
        OutsideRow, // a desktop would get a number outside its monitor's row
        Pinned,    // a pinned desktop (an Omarchy guest) would be renumbered
    };

    inline const char* why(Verdict v) {
        switch (v) {
            case Verdict::Ok: return "ok";
            case Verdict::Unknown: return "names a desktop that is not an ordinary one";
            case Verdict::Twice: return "moves one desktop twice";
            case Verdict::BelowOne: return "would number a desktop below 1";
            case Verdict::Collision: return "would put two desktops on one number";
            case Verdict::OutsideRow: return "would number a desktop outside its monitor's row";
            case Verdict::Pinned: return "would renumber a pinned desktop (an Omarchy guest)";
        }
        return "?";
    }

    // The whole plan, against every ordinary desktop (`desks`). Desktops the plan
    // does not move keep their numbers; every desktop it moves must end on a
    // number >= 1 that no other desktop ends on. (Two untouched desktops already
    // sharing a number are heal()'s, not a reason to refuse an unrelated plan.)
    //
    // ROWS: every moved desktop must land in ITS row (Desk::row, its monitor's)
    // -- the hard guarantee that Hyprdrive only ever picks numbers in a
    // monitor's own block (Row.hpp) -- and a pinned one is not moved at all
    // (a move onto its own number is no move). The implicit row is every
    // number from 1, so natively this adds nothing.
    inline Verdict check(const std::vector<Desk>& desks, const std::vector<Move>& moves) {
        std::unordered_map<Key, Id>          final;
        std::unordered_map<Key, const Desk*> desk;
        for (const auto& D : desks) {
            final[D.key] = D.id;
            desk[D.key]  = &D;
        }
        std::unordered_set<Key> moved;
        for (const auto& M : moves) {
            if (!final.contains(M.key))
                return Verdict::Unknown;
            if (!moved.insert(M.key).second)
                return Verdict::Twice;
            if (M.to < 1)
                return Verdict::BelowOne;
            const Desk& D = *desk.at(M.key);
            if (D.pinned && M.to != D.id)
                return Verdict::Pinned;
            if (!D.row.contains(M.to))
                return Verdict::OutsideRow;
            final[M.key] = M.to;
        }
        std::unordered_map<Id, int> count;
        for (const auto& [K, ID] : final)
            ++count[ID];
        for (const auto& K : moved)
            if (count[final[K]] > 1)
                return Verdict::Collision;
        return Verdict::Ok;
    }

    // A member of `row`: a desktop on a monitor with that row whose number is
    // in it. (A desktop outside its monitor's row is a VISITOR -- an Omarchy
    // guest, a legacy number -- never renumbered, never counted.)
    inline bool member(const Desk& d, const Row& row) {
        return sameRow(d.row, row) && row.contains(d.id);
    }

    // Every member of `row` at the place of `from` or after moves `delta`
    // places along the row (a block's 10th desktop +1 is the row's first
    // overflow desktop). A `from` outside a bounded row: no moves. The
    // implicit row is the global number line: every desktop numbered `from`
    // or more moves `delta`.
    inline std::vector<Move> shiftMoves(const std::vector<Desk>& desks, const Row& row, Id from, int delta) {
        std::vector<Move> out;
        const Id          FROM = row.rank(from);
        if (row.bounded() && FROM < 1)
            return out;
        for (const auto& D : desks)
            if (member(D, row) && row.slot(D.id) >= FROM)
                out.push_back({D.key, row.idOf(row.slot(D.id) + delta)});
        return out;
    }
    inline std::vector<Move> shiftMoves(const std::vector<Desk>& desks, Id from, int delta) {
        return shiftMoves(desks, Row{}, from, delta);
    }

    // A place `at` is paid back while an empty desktop `e` still sits in it:
    // the ones after it come back a place and `e` goes to the end of the row
    // (Hyprland drops it once nothing holds it). Nothing after it: no moves.
    // In a row: the members after `at` come back a place, `e` goes to the
    // row's top (its last member's place, overflow included).
    inline std::vector<Move> payMoves(const std::vector<Desk>& desks, const Row& row, Id at, Key e) {
        std::vector<Move> out;
        const Id          AT = row.rank(at);
        if (row.bounded() && AT < 1)
            return out;
        Id top = AT;
        for (const auto& D : desks)
            if (D.key != e && member(D, row) && row.slot(D.id) > AT) {
                top = std::max(top, row.slot(D.id));
                out.push_back({D.key, row.idOf(row.slot(D.id) - 1)});
            }
        if (top == AT)
            return {};
        out.push_back({e, row.idOf(top)});
        return out;
    }
    inline std::vector<Move> payMoves(const std::vector<Desk>& desks, Id at, Key e) {
        return payMoves(desks, Row{}, at, e);
    }

    // NO EMPTY DESKTOPS AFTER A DISPLAY CHANGE (user, 2026-11-11: "just make it
    // remove all empty desktops when changing monitor config"). The desktops
    // holding windows are numbered 1..N in the order they are in; then each
    // monitor's shown desktop that is empty (still shown -- the screen does not
    // change -- but after the others: left behind between two, it would be the
    // next hole the moment the monitor shows something else); then any other
    // empty one still alive (Hyprland drops it once nothing holds it). Order is
    // kept within each group (by number, then by the list's order on a tie), so
    // every number is new and unique. Already compact: no moves.
    //
    // PER ROW (DesktopOrder::compact runs it once per row): `desks` are the
    // row's members, numbered from its first place; overflow desktops are
    // ordinary members, so they come into the block as soon as it has room
    // -- in order, where Omarchy's append_slot would put them. A PINNED one
    // keeps its number and the others are numbered around it.
    struct Room {
        Key  key       = 0;
        Id   id        = 0;
        bool populated = false; // mapped windows on it
        bool shown     = false; // a monitor shows it
        bool pinned    = false; // an Omarchy guest: stays where it is
    };
    inline std::vector<Move> compactMoves(const std::vector<Room>& desks, const Row& row) {
        std::vector<size_t> order;
        std::set<Id>        held; // the pinned desks' places
        for (size_t i = 0; i < desks.size(); ++i) {
            if (desks[i].pinned)
                held.insert(row.slot(desks[i].id));
            else
                order.push_back(i);
        }
        const auto GROUP = [&](size_t i) { return desks[i].populated ? 0 : desks[i].shown ? 1 : 2; };
        std::ranges::stable_sort(order, [&](size_t a, size_t b) {
            if (GROUP(a) != GROUP(b))
                return GROUP(a) < GROUP(b);
            return row.slot(desks[a].id) < row.slot(desks[b].id);
        });
        std::vector<Move> out;
        Id                next = 0;
        for (const size_t I : order) {
            do
                ++next;
            while (held.contains(next));
            if (const Id TO = row.idOf(next); TO != desks[I].id)
                out.push_back({desks[I].key, TO});
        }
        return out;
    }
    inline std::vector<Move> compactMoves(const std::vector<Room>& desks) {
        return compactMoves(desks, Row{});
    }

    // Two desktops on one number (a plan the old code half-applied, or anything
    // else): the one with more windows keeps it -- the earlier-created on a tie
    // (`desks` is in creation order) -- and each other one gets the next free
    // number above the highest. Windows are never merged nor dropped.
    //
    // ROWS: a loser on a bounded row gets the first free place in ITS row
    // (overflow included, so there always is one); on the implicit row, the
    // next number above the highest, as before. A pinned desktop never loses.
    inline std::vector<Move> healMoves(const std::vector<Desk>& desks) {
        Id           top = 0;
        std::set<Id> taken;
        for (const auto& D : desks) {
            taken.insert(D.id);
            if (!D.row.bounded())
                top = std::max(top, D.id);
        }
        std::unordered_map<Id, size_t> keeper; // number -> index of its keeper
        for (size_t i = 0; i < desks.size(); ++i) {
            const auto IT = keeper.find(desks[i].id);
            if (IT == keeper.end())
                keeper.emplace(desks[i].id, i);
            else if (!desks[IT->second].pinned && (desks[i].pinned || desks[i].windows > desks[IT->second].windows))
                IT->second = i;
        }
        std::vector<Move> out;
        for (size_t i = 0; i < desks.size(); ++i) {
            if (keeper.at(desks[i].id) == i)
                continue;
            const Row& ROW = desks[i].row;
            if (!ROW.bounded()) {
                // (Above every implicit-row number, and clear of any bounded
                // row's that happens to sit there.)
                do
                    ++top;
                while (taken.contains(top));
                taken.insert(top);
                out.push_back({desks[i].key, top});
                continue;
            }
            for (Id s = 1; s <= ROW.lastSlot(); ++s)
                if (const Id ID = ROW.idOf(s); !taken.contains(ID)) {
                    taken.insert(ID);
                    out.push_back({desks[i].key, ID});
                    break;
                }
        }
        return out;
    }
}
