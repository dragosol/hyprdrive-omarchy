#pragma once

// TEST-ONLY: a C++ model of Omarchy's per-monitor workspace reconcile
// (/usr/share/omarchy/default/hypr/monitor-workspaces.lua, omarchy-settings
// 4.5.0-3, file dated 2026-11-04), from the reading in
// rigs-backup/hyprdrive-omarchy-blocks-design-2026-11-11.md section (a).
// Pinned to that version: when Omarchy changes the script, re-read it and
// update this model before trusting desktop_rows_test.
//
//   M.reconcile = restore(); place(); merge(); own_views()
//   - only workspaces WITH WINDOWS are renumbered (plus what each monitor shows)
//   - ids already in their own monitor's block are never touched
//   - block_of_id = (id-1)//SLOTS; adopt keeps the slot if free, else the
//     first free one; append_slot = highest used id in the block + 1, else
//     any free id in it, else nil (stays a guest)
//   - renumber() moves merged[old] to merged[new]

#include <algorithm>
#include <map>
#include <optional>
#include <vector>

namespace OmarchyModel {
    using Id = long long;

    struct Ws {
        unsigned long key     = 0;
        Id            id      = 0;
        int           monitor = 0; // index into World::block
        bool          windows = false;
    };
    struct Record {
        Id original = 0;
        Id originBlock = 0; // the block of its origin monitor
    };
    struct World {
        Id                      slots = 10;
        std::map<int, Id>       block;  // connected monitor -> its block
        std::vector<Ws>         ws;
        std::map<int, unsigned long> shown; // monitor -> key of the workspace it shows
        std::map<Id, Record>    merged; // current id -> record
        unsigned long           nextKey = 1'000'000;
    };

    inline Id blockOf(const World& w, Id id) {
        return (id - 1) / w.slots;
    }
    inline std::optional<int> ownerOf(const World& w, Id block) {
        for (const auto& [M, B] : w.block)
            if (B == block)
                return M;
        return std::nullopt;
    }
    inline bool taken(const World& w, Id id) {
        return std::ranges::any_of(w.ws, [&](const Ws& x) { return x.id == id; });
    }
    inline void renumber(World& w, Ws& x, Id to, int& changes) {
        if (const auto IT = w.merged.find(x.id); IT != w.merged.end()) {
            const Record R = IT->second;
            w.merged.erase(IT);
            w.merged[to] = R;
        }
        x.id = to;
        ++changes;
    }
    // adopt: the same slot in `block` if free, else the first free one.
    inline std::optional<Id> adoptId(const World& w, Id id, Id block) {
        const Id SAME = block * w.slots + (id - 1) % w.slots + 1;
        if (!taken(w, SAME))
            return SAME;
        for (Id s = 1; s <= w.slots; ++s)
            if (!taken(w, block * w.slots + s))
                return block * w.slots + s;
        return std::nullopt;
    }
    inline std::optional<Id> appendSlot(const World& w, Id block) {
        Id high = 0;
        for (const auto& X : w.ws)
            if (X.id >= 1 && blockOf(w, X.id) == block)
                high = std::max(high, X.id);
        if (high && high + 1 <= block * w.slots + w.slots && !taken(w, high + 1))
            return high + 1;
        if (!high && !taken(w, block * w.slots + 1))
            return block * w.slots + 1;
        for (Id s = 1; s <= w.slots; ++s)
            if (!taken(w, block * w.slots + s))
                return block * w.slots + s;
        return std::nullopt;
    }

    // Returns how many ids it changed (changeworkspaceid events).
    inline int reconcile(World& w) {
        int changes = 0;
        // 1. restore
        for (auto it = w.merged.begin(); it != w.merged.end();) {
            const auto OWNER = ownerOf(w, it->second.originBlock);
            auto       X     = std::ranges::find_if(w.ws, [&](const Ws& x) { return x.id == it->first; });
            if (!OWNER || X == w.ws.end()) {
                ++it;
                continue;
            }
            const Record R = it->second;
            it             = w.merged.erase(it);
            X->monitor     = *OWNER;
            Id to          = R.original;
            if (taken(w, to) && to != X->id) {
                const auto A = adoptId(w, X->id, R.originBlock);
                to           = A ? *A : X->id;
            }
            if (to != X->id) {
                X->id = to;
                ++changes;
            }
        }
        // 3. place
        for (auto& X : w.ws) {
            if (!X.windows || X.id < 1 || !w.block.contains(X.monitor))
                continue;
            const Id   B     = blockOf(w, X.id);
            const auto OWNER = ownerOf(w, B);
            if (!OWNER || *OWNER == X.monitor)
                continue;
            if (const auto TO = adoptId(w, X.id, w.block.at(X.monitor)))
                renumber(w, X, *TO, changes);
            else
                X.monitor = *OWNER; // no room: to its owner
        }
        // 4. merge
        for (auto& X : w.ws) {
            if (!X.windows || X.id < 1 || !w.block.contains(X.monitor))
                continue;
            const Id B = blockOf(w, X.id);
            if (ownerOf(w, B))
                continue;
            const auto TO = appendSlot(w, w.block.at(X.monitor));
            if (!TO)
                continue; // no room: stays a guest at its foreign id
            const Id OLD = X.id;
            // (renumber() moves an existing record; merge writes the new one)
            renumber(w, X, *TO, changes);
            if (!w.merged.contains(*TO) || w.merged[*TO].original == OLD)
                w.merged[*TO] = {OLD, B};
        }
        // 5. own_views
        for (const auto& [M, B] : w.block) {
            const auto IT = w.shown.find(M);
            const Ws*  S  = nullptr;
            if (IT != w.shown.end())
                for (const auto& X : w.ws)
                    if (X.key == IT->second)
                        S = &X;
            const bool OWN   = S && S->id >= 1 && blockOf(w, S->id) == B;
            const bool GUEST = S && S->windows && !OWN;
            if (OWN || GUEST)
                continue;
            const Ws* home = nullptr;
            for (const auto& X : w.ws)
                if (X.monitor == M && X.id >= 1 && blockOf(w, X.id) == B && (!home || X.id < home->id))
                    home = &X;
            if (home)
                w.shown[M] = home->key;
            else if (!taken(w, B * w.slots + 1)) {
                w.ws.push_back({w.nextKey++, B * w.slots + 1, M, false});
                w.shown[M] = w.ws.back().key;
            }
        }
        return changes;
    }

    // Hyprland drops an empty workspace nothing shows.
    inline void collect(World& w) {
        std::erase_if(w.ws, [&](const Ws& x) {
            if (x.windows)
                return false;
            for (const auto& [M, K] : w.shown)
                if (K == x.key)
                    return false;
            return true;
        });
    }
}
