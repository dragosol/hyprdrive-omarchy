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

#include <algorithm>
#include <cstdint>
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
    };

    inline const char* why(Verdict v) {
        switch (v) {
            case Verdict::Ok: return "ok";
            case Verdict::Unknown: return "names a desktop that is not an ordinary one";
            case Verdict::Twice: return "moves one desktop twice";
            case Verdict::BelowOne: return "would number a desktop below 1";
            case Verdict::Collision: return "would put two desktops on one number";
        }
        return "?";
    }

    // The whole plan, against every ordinary desktop (`desks`). Desktops the plan
    // does not move keep their numbers; every desktop it moves must end on a
    // number >= 1 that no other desktop ends on. (Two untouched desktops already
    // sharing a number are heal()'s, not a reason to refuse an unrelated plan.)
    inline Verdict check(const std::vector<Desk>& desks, const std::vector<Move>& moves) {
        std::unordered_map<Key, Id> final;
        for (const auto& D : desks)
            final[D.key] = D.id;
        std::unordered_set<Key> moved;
        for (const auto& M : moves) {
            if (!final.contains(M.key))
                return Verdict::Unknown;
            if (!moved.insert(M.key).second)
                return Verdict::Twice;
            if (M.to < 1)
                return Verdict::BelowOne;
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

    // Every desktop numbered `from` or more moves `delta` places.
    inline std::vector<Move> shiftMoves(const std::vector<Desk>& desks, Id from, int delta) {
        std::vector<Move> out;
        for (const auto& D : desks)
            if (D.id >= from)
                out.push_back({D.key, D.id + delta});
        return out;
    }

    // A place `at` is paid back while an empty desktop `e` still sits in it:
    // the ones after it come back a place and `e` goes to the end of the row
    // (Hyprland drops it once nothing holds it). Nothing after it: no moves.
    inline std::vector<Move> payMoves(const std::vector<Desk>& desks, Id at, Key e) {
        std::vector<Move> out;
        Id                top = at;
        for (const auto& D : desks)
            if (D.key != e && D.id > at) {
                top = std::max(top, D.id);
                out.push_back({D.key, D.id - 1});
            }
        if (top == at)
            return {};
        out.push_back({e, top});
        return out;
    }

    // Two desktops on one number (a plan the old code half-applied, or anything
    // else): the one with more windows keeps it -- the earlier-created on a tie
    // (`desks` is in creation order) -- and each other one gets the next free
    // number above the highest. Windows are never merged nor dropped.
    inline std::vector<Move> healMoves(const std::vector<Desk>& desks) {
        Id top = 0;
        for (const auto& D : desks)
            top = std::max(top, D.id);
        std::unordered_map<Id, size_t> keeper; // number -> index of its keeper
        for (size_t i = 0; i < desks.size(); ++i) {
            const auto IT = keeper.find(desks[i].id);
            if (IT == keeper.end())
                keeper.emplace(desks[i].id, i);
            else if (desks[i].windows > desks[IT->second].windows)
                IT->second = i;
        }
        std::vector<Move> out;
        for (size_t i = 0; i < desks.size(); ++i)
            if (keeper.at(desks[i].id) != i)
                out.push_back({desks[i].key, ++top});
        return out;
    }
}
