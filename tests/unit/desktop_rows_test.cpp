// Host test for DesktopPlan.hpp ON ROWS (Row.hpp): every plan Hyprdrive makes
// keeps each desktop inside its monitor's row, never touches another row or a
// pinned guest, and -- against a model of Omarchy's own reconcile
// (omarchy_model.hpp) -- is a FIXED POINT of it: Omarchy renumbers nothing
// Hyprdrive compacted, so the two never fight. desktop_plan_test.cpp is the
// implicit row (blocks off), unchanged.
#include "DesktopPlan.hpp"
#include "omarchy_model.hpp"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <random>
#include <set>

using namespace Hyprdrive;
using namespace Hyprdrive::DesktopPlan;

static int g_checks = 0, g_failed = 0;
#define CHECK(c)                                                                                                                                                                   \
    do {                                                                                                                                                                           \
        ++g_checks;                                                                                                                                                                \
        if (!(c)) {                                                                                                                                                                \
            ++g_failed;                                                                                                                                                            \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c);                                                                                                               \
        }                                                                                                                                                                          \
    } while (0)

static std::vector<Desk> applied(std::vector<Desk> desks, const std::vector<Move>& moves) {
    for (const auto& M : moves)
        for (auto& D : desks)
            if (D.key == M.key)
                D.id = M.to;
    return desks;
}
static bool unique(const std::vector<Desk>& desks) {
    std::set<Id> seen;
    for (const auto& D : desks)
        if (D.id < 1 || !seen.insert(D.id).second)
            return false;
    return true;
}
static Desk desk(Key k, Id id, const Row& row, bool pinned = false, int windows = 1) {
    return {.key = k, .id = id, .windows = windows, .row = row, .pinned = pinned};
}

// ---- Hyprdrive's compaction over an Omarchy model world (DesktopOrder::compact)
static std::vector<Move> hyprdriveCompact(const OmarchyModel::World& w) {
    std::vector<Desk> desks;
    std::vector<Room> rooms;
    std::vector<Row>  rows;
    for (const auto& X : w.ws) {
        const Row R = w.block.contains(X.monitor) ? Row::block(w.block.at(X.monitor), w.slots) : Row{};
        bool      shown = false;
        for (const auto& [M, K] : w.shown)
            shown |= K == X.key;
        const bool PIN = w.merged.contains(X.id);
        desks.push_back(desk(X.key, X.id, R, PIN, X.windows));
        rooms.push_back({X.key, X.id, X.windows, shown, PIN});
        rows.push_back(R);
    }
    std::vector<Move> plan;
    std::vector<bool> done(rooms.size(), false);
    for (size_t i = 0; i < rooms.size(); ++i) {
        if (done[i] || !rows[i].contains(rooms[i].id))
            continue;
        std::vector<Room> group;
        for (size_t j = i; j < rooms.size(); ++j)
            if (!done[j] && sameRow(rows[j], rows[i]) && rows[i].contains(rooms[j].id)) {
                done[j] = true;
                group.push_back(rooms[j]);
            }
        for (const auto& M : compactMoves(group, rows[i]))
            plan.push_back(M);
    }
    CHECK(check(desks, plan) == Verdict::Ok);
    return plan;
}
static int applyCompact(OmarchyModel::World& w) {
    const auto PLAN = hyprdriveCompact(w);
    for (const auto& M : PLAN)
        for (auto& X : w.ws)
            if (X.key == M.key)
                X.id = M.to; // (Hyprdrive never renumbers a merged record's id: pinned)
    return int(PLAN.size());
}
static std::map<unsigned long, Id> ids(const OmarchyModel::World& w) {
    std::map<unsigned long, Id> out;
    for (const auto& X : w.ws)
        out[X.key] = X.id;
    return out;
}
static bool settledWell(const OmarchyModel::World& w) {
    std::set<Id> seen;
    for (const auto& X : w.ws) {
        if (!seen.insert(X.id).second)
            return false;
        if (!w.block.contains(X.monitor) || !X.windows)
            continue;
        const Row R = Row::block(w.block.at(X.monitor), w.slots);
        // every desktop with windows is in its monitor's row (block or
        // overflow) -- or a guest Omarchy left at its foreign id because the
        // block had no room (a visitor: Omarchy's, never Hyprdrive's)
        bool full = true;
        for (Id s = 1; s <= R.capacity; ++s)
            full &= OmarchyModel::taken(w, R.idOf(s));
        if (!R.contains(X.id) && !w.merged.contains(X.id) && !full)
            return false;
    }
    return true;
}

int main() {
    const Row B0 = Row::block(0, 10), B1 = Row::block(1, 10), B2 = Row::block(2, 10);

    // ---- check: a move out of the row is refused; a pinned desk is never moved
    {
        const std::vector<Desk> D{desk(1, 11, B1), desk(2, 12, B1), desk(3, 21, B2)};
        CHECK(check(D, {{1, 13}}) == Verdict::Ok);
        CHECK(check(D, {{1, 21}}) == Verdict::OutsideRow); // another monitor's block
        CHECK(check(D, {{1, 3}}) == Verdict::OutsideRow);
        CHECK(check(D, {{1, 10}}) == Verdict::OutsideRow); // below the row
        CHECK(check(D, {{2, 21}}) == Verdict::OutsideRow);
        CHECK(check(D, {{1, B1.idOf(11)}}) == Verdict::Ok); // its own overflow
        CHECK(check(D, {{1, B2.idOf(11)}}) == Verdict::OutsideRow);
        const std::vector<Desk> P{desk(1, 21, B2, true), desk(2, 22, B2)};
        CHECK(check(P, {{1, 23}}) == Verdict::Pinned);
        CHECK(check(P, {{1, 21}}) == Verdict::Ok); // onto its own number: no move
    }
    // ---- shift in a block: only that row; the 10th goes to overflow, never to 21
    {
        std::vector<Desk> D;
        for (Id i = 11; i <= 20; ++i)
            D.push_back(desk(Key(i), i, B1));
        D.push_back(desk(100, 21, B2));
        D.push_back(desk(101, 1, B0));
        const auto M = shiftMoves(D, B1, 15, +1);
        CHECK(M.size() == 6);
        CHECK(check(D, M) == Verdict::Ok);
        const auto A = applied(D, M);
        CHECK(unique(A));
        CHECK(A[9].id == 1'001'001); // was 20
        CHECK(A[10].id == 21 && A[11].id == 1);
        CHECK(shiftMoves(D, B1, 25, +1).empty()); // not in the row
        CHECK(shiftMoves(D, B1, 5, +1).empty());
        // and back: the overflow desktop returns to 20
        const auto BACK = shiftMoves(A, B1, 16, -1);
        CHECK(check(A, BACK) == Verdict::Ok);
        const auto A2 = applied(A, BACK);
        for (size_t i = 0; i < 10; ++i)
            CHECK(i == 4 || A2[i].id == Id(11 + i)); // (15 itself went to 16 and back to 15: the -1 from 16 skips it)
        CHECK(A2[9].id == 20);
    }
    // ---- payMoves in a row: the empty desk goes to the row's top
    {
        const std::vector<Desk> D{desk(1, 21, B2), desk(2, 22, B2, false, 0), desk(3, 23, B2), desk(4, 1'002'001, B2), desk(5, 11, B1)};
        const auto              M = payMoves(D, B2, 22, 2);
        CHECK(check(D, M) == Verdict::Ok);
        const auto A = applied(D, M);
        CHECK(A[2].id == 22 && A[3].id == 30 && A[1].id == 1'002'001 && A[4].id == 11); // the overflow desk comes back into the block
    }
    // ---- compact in a block with a pinned guest and an overflow desktop
    {
        const std::vector<Room> R{{1, 22, true}, {2, 24, true, false, true}, {3, 27, true}, {4, 1'002'001, true}, {5, 29, false, true}};
        const auto              M = compactMoves(R, B2);
        std::map<Key, Id>       to{{1, 22}, {2, 24}, {3, 27}, {4, 1'002'001}, {5, 29}};
        for (const auto& X : M)
            to[X.key] = X.to;
        CHECK(to[1] == 21 && to[2] == 24 && to[3] == 22 && to[4] == 23 && to[5] == 25);
        // idempotent
        std::vector<Room> R2 = R;
        for (auto& X : R2)
            X.id = to[X.key];
        CHECK(compactMoves(R2, B2).empty());
    }
    // ---- heal in a row: the loser gets the first free place in ITS row
    {
        const std::vector<Desk> D{desk(1, 21, B2, false, 2), desk(2, 21, B2, false, 1), desk(3, 22, B2), desk(4, 11, B1)};
        const auto              M = healMoves(D);
        CHECK(M.size() == 1 && M[0].key == 2 && M[0].to == 23);
        CHECK(check(D, M) == Verdict::Ok);
        // a full block: the loser goes to overflow, not to 31
        std::vector<Desk> F;
        for (Id i = 21; i <= 30; ++i)
            F.push_back(desk(Key(i), i, B2));
        F.push_back(desk(99, 25, B2, false, 0));
        const auto MF = healMoves(F);
        CHECK(MF.size() == 1 && MF[0].to == 1'002'001);
        // a pinned guest keeps its number even when emptier
        const std::vector<Desk> P{desk(1, 21, B2, false, 5), desk(2, 21, B2, true, 0)};
        const auto              MP = healMoves(P);
        CHECK(MP.size() == 1 && MP[0].key == 1);
    }

    // ---- PROPERTIES on random rows: every operation stays in its row
    std::mt19937 rng(20261111);
    for (int round = 0; round < 3000; ++round) {
        const bool        IMPLICIT = rng() % 4 == 0;
        std::vector<Row>  rows;
        if (IMPLICIT)
            rows.push_back(Row{});
        else {
            std::set<Id> used;
            const int    N = 1 + int(rng() % 3);
            while (int(rows.size()) < N) {
                const Id B = rng() % 4;
                if (used.insert(B).second)
                    rows.push_back(Row::block(B, 10));
            }
        }
        std::vector<Desk> D;
        std::set<Id>      taken;
        Key               k = 1;
        for (const auto& R : rows) {
            const int N = int(rng() % 13);
            for (int i = 0; i < N; ++i) {
                const Id S  = 1 + Id(rng() % (R.bounded() ? 14 : 30));
                const Id ID = R.idOf(S);
                if (!ID || !taken.insert(ID).second)
                    continue;
                D.push_back(desk(k++, ID, R, R.bounded() && rng() % 9 == 0, int(rng() % 3)));
            }
        }
        const auto IN_ROW = [&](const std::vector<Desk>& before, const std::vector<Move>& moves) {
            for (const auto& M : moves)
                for (const auto& X : before)
                    if (X.key == M.key && !X.row.contains(M.to))
                        return false;
            return true;
        };
        const auto OTHERS_UNTOUCHED = [&](const std::vector<Move>& moves, const Row& row) {
            for (const auto& M : moves)
                for (const auto& X : D)
                    if (X.key == M.key && !sameRow(X.row, row))
                        return false;
            return true;
        };
        const Row& ROW  = rows[rng() % rows.size()];
        const Id   FROM = ROW.idOf(1 + Id(rng() % 12));
        for (const int DELTA : {+1, -1}) {
            const auto M = shiftMoves(D, ROW, FROM, DELTA);
            CHECK(OTHERS_UNTOUCHED(M, ROW));
            const auto V = check(D, M);
            CHECK(V != Verdict::Unknown && V != Verdict::Twice);
            if (V == Verdict::Ok) {
                CHECK(IN_ROW(D, M));
                CHECK(unique(applied(D, M)));
            }
        }
        // pay
        {
            std::vector<Key> members;
            for (const auto& X : D)
                if (member(X, ROW))
                    members.push_back(X.key);
            if (!members.empty()) {
                const Key  E = members[rng() % members.size()];
                Id         at = 0;
                for (const auto& X : D)
                    if (X.key == E)
                        at = X.id;
                const auto M = payMoves(D, ROW, at, E);
                CHECK(OTHERS_UNTOUCHED(M, ROW));
                if (check(D, M) == Verdict::Ok)
                    CHECK(IN_ROW(D, M) && unique(applied(D, M)));
            }
        }
        // permute (a strip drag): the row's own ids, reordered
        {
            std::vector<Desk*> mem;
            std::vector<Id>    pool;
            for (auto& X : D)
                if (member(X, ROW) && !X.pinned) {
                    mem.push_back(&X);
                    pool.push_back(X.id);
                }
            std::ranges::shuffle(pool, rng);
            std::vector<Move> M;
            for (size_t i = 0; i < mem.size(); ++i)
                M.push_back({mem[i]->key, pool[i]});
            CHECK(check(D, M) == Verdict::Ok);
            CHECK(OTHERS_UNTOUCHED(M, ROW));
        }
        // heal (on a copy with one duplicate)
        {
            auto H = D;
            if (H.size() >= 2) {
                const size_t A = rng() % H.size(), B = rng() % H.size();
                if (A != B && sameRow(H[A].row, H[B].row) && !H[B].pinned)
                    H[B].id = H[A].id;
            }
            const auto M = healMoves(H);
            CHECK(check(H, M) == Verdict::Ok);
            CHECK(IN_ROW(H, M) || !unique(H));
            CHECK(unique(applied(H, M)));
        }
        // compact, per row
        {
            std::vector<Move> plan;
            for (const auto& R : rows) {
                std::vector<Room> group;
                for (const auto& X : D)
                    if (member(X, R))
                        group.push_back({X.key, X.id, X.windows > 0, false, X.pinned});
                const auto M = compactMoves(group, R);
                plan.insert(plan.end(), M.begin(), M.end());
                // populated first..: in their old order, around the pinned ones
                const auto     A = applied(D, M);
                std::set<Id>   pins;
                for (const auto& X : D)
                    if (member(X, R) && X.pinned)
                        pins.insert(R.slot(X.id));
                std::vector<Id> oldOrder, newSlots;
                for (const auto& X : D)
                    if (member(X, R) && !X.pinned && X.windows > 0)
                        oldOrder.push_back(R.slot(X.id));
                std::ranges::sort(oldOrder);
                Id s = 0;
                bool ordered = true;
                for (const Id OLD : oldOrder) {
                    do
                        ++s;
                    while (pins.contains(s));
                    bool found = false;
                    for (size_t i = 0; i < D.size(); ++i)
                        if (member(D[i], R) && !D[i].pinned && D[i].windows > 0 && R.slot(D[i].id) == OLD)
                            found = R.slot(A[i].id) == s;
                    ordered &= found;
                }
                CHECK(ordered);
                // idempotent
                std::vector<Room> again;
                for (size_t i = 0; i < D.size(); ++i)
                    if (member(D[i], R))
                        again.push_back({A[i].key, A[i].id, A[i].windows > 0, false, A[i].pinned});
                CHECK(compactMoves(again, R).empty());
            }
            CHECK(check(D, plan) == Verdict::Ok);
            CHECK(IN_ROW(D, plan));
            CHECK(unique(applied(D, plan)));
        }
    }

    // ---- NO FIGHT: compact(x) is a fixed point of Omarchy's reconcile, and
    // reconcile + compact converge in one round from anything.
    for (int round = 0; round < 3000; ++round) {
        OmarchyModel::World w;
        std::set<Id>        blocksUsed;
        const int           MONS = 1 + int(rng() % 3);
        for (int m = 0; m < MONS; ++m) {
            Id b;
            do
                b = rng() % 4;
            while (!blocksUsed.insert(b).second);
            w.block[m] = b;
        }
        std::set<Id>  taken;
        unsigned long k = 1;
        const int     N = int(rng() % 16);
        for (int i = 0; i < N; ++i) {
            const int M = int(rng() % MONS);
            Id        id;
            switch (rng() % 5) {
                case 0: id = 1 + Id(rng() % 10); break;                         // block 0
                case 1: id = w.block[M] * 10 + 1 + Id(rng() % 10); break;      // own block
                case 2: id = 1 + Id(rng() % 50); break;                         // anywhere
                case 3: id = Row::block(w.block[M], 10).idOf(11 + Id(rng() % 3)); break; // own overflow
                default: id = 1 + Id(rng() % 30); break;
            }
            if (!taken.insert(id).second)
                continue;
            w.ws.push_back({k++, id, M, rng() % 4 != 0});
        }
        for (int m = 0; m < MONS; ++m)
            for (const auto& X : w.ws)
                if (X.monitor == m && rng() % 2) {
                    w.shown[m] = X.key;
                    break;
                }
        // guests: records for blocks no connected monitor owns
        for (const auto& X : w.ws)
            if (rng() % 7 == 0) {
                const Id ORIGIN = 4 + Id(rng() % 3);
                w.merged[X.id] = {ORIGIN * 10 + 1 + Id(rng() % 10), ORIGIN};
            }

        // the fixed point: once Hyprdrive compacted, Omarchy changes nothing
        // (and its renumbers, whatever the start, are never undone)
        int  rounds = 0, lastOmarchy = -1, lastHyprdrive = -1;
        for (; rounds < 6; ++rounds) {
            lastOmarchy = OmarchyModel::reconcile(w);
            OmarchyModel::collect(w);
            lastHyprdrive = applyCompact(w);
            OmarchyModel::collect(w);
            if (rounds >= 1 && lastOmarchy == 0 && lastHyprdrive == 0)
                break;
        }
        CHECK(rounds <= 2);
        const auto BEFORE = ids(w);
        CHECK(OmarchyModel::reconcile(w) == 0);
        CHECK(ids(w) == BEFORE);
        CHECK(applyCompact(w) == 0);
        CHECK(settledWell(w));
        if (!settledWell(w) && std::getenv("DUMP")) {
            for (const auto& [M, B] : w.block)
                std::printf(" mon%d=block%lld", M, B);
            for (const auto& X : w.ws)
                std::printf(" [k%lu id%lld m%d w%d%s]", X.key, X.id, X.monitor, X.windows, w.merged.contains(X.id) ? " pin" : "");
            std::printf("\n");
        }
        // no empty desktop below the last populated one in any row
        for (const auto& [M, B] : w.block) {
            const Row R = Row::block(B, 10);
            Id        top = 0;
            for (const auto& X : w.ws)
                if (X.monitor == M && R.contains(X.id) && X.windows && !w.merged.contains(X.id)) // (a pinned guest may sit past a hole)
                    top = std::max(top, R.slot(X.id));
            for (Id s = 1; s < top; ++s) {
                bool filled = false;
                for (const auto& X : w.ws)
                    filled |= X.id == R.idOf(s) && (X.windows || w.merged.contains(X.id));
                bool pinnedElsewhere = false;
                for (const auto& [ID, _] : w.merged)
                    pinnedElsewhere |= ID == R.idOf(s);
                const bool OK = filled || pinnedElsewhere || std::ranges::any_of(w.ws, [&](const OmarchyModel::Ws& x) { return x.id == R.idOf(s) && x.monitor != M; });
                CHECK(OK);
                if (!OK && std::getenv("DUMP")) {
                    std::printf("hole at %lld (block %lld, mon %d):", R.idOf(s), B, M);
                    for (const auto& X : w.ws)
                        std::printf(" [k%lu id%lld m%d w%d%s]", X.key, X.id, X.monitor, X.windows, w.merged.contains(X.id) ? " pin" : "");
                    std::printf("\n");
                    return 1;
                }
            }
        }
    }

    // ---- OVERFLOW: a full block plus an 11th is a fixed point; a freed slot
    // takes the overflow back without a fight, whoever moves first.
    for (const bool OMARCHY_FIRST : {false, true}) {
        OmarchyModel::World w;
        w.block[0] = 2;
        unsigned long k = 1;
        for (Id i = 21; i <= 30; ++i)
            w.ws.push_back({k++, i, 0, true});
        w.ws.push_back({k++, Row::block(2, 10).idOf(11), 0, true});
        w.shown[0] = 1;
        CHECK(OmarchyModel::reconcile(w) == 0); // block full: it stays a guest at its overflow id
        CHECK(applyCompact(w) == 0);
        // desktop 23's windows close; Hyprland drops it
        std::erase_if(w.ws, [](const OmarchyModel::Ws& x) { return x.id == 23; });
        if (OMARCHY_FIRST) {
            CHECK(OmarchyModel::reconcile(w) == 1); // merge(): into the free slot, with a record
            applyCompact(w);
        } else {
            CHECK(applyCompact(w) == 8); // 24..30 down a place, the overflow into 30
            CHECK(ids(w).at(11) == 30);
        }
        const auto SETTLED = ids(w);
        CHECK(OmarchyModel::reconcile(w) == 0);
        CHECK(applyCompact(w) == 0);
        CHECK(ids(w) == SETTLED);
        CHECK(settledWell(w));
        std::set<Id> got;
        for (const auto& X : w.ws)
            got.insert(X.id);
        CHECK(got.size() == 10 && *got.begin() == 21 && *got.rbegin() == 30);
    }

    // ---- THE OWNER'S CASE: internal panel off, the external monitor owns
    // block 2. (a) desktops 21..24: nothing moves. (b) the old build had
    // compacted them to 1..4 and Omarchy merged them back with records
    // ("origin = panel"): pinned, nothing moves either.
    {
        OmarchyModel::World w;
        w.block[0] = 2;
        for (unsigned long i = 1; i <= 4; ++i)
            w.ws.push_back({i, Id(20 + i), 0, true});
        w.shown[0] = 1;
        CHECK(OmarchyModel::reconcile(w) == 0 && applyCompact(w) == 0);

        OmarchyModel::World v;
        v.block[0] = 2;
        for (unsigned long i = 1; i <= 4; ++i)
            v.ws.push_back({i, Id(i), 0, true});
        v.shown[0] = 1;
        CHECK(OmarchyModel::reconcile(v) == 4); // block 0 has no connected owner: merged to 21..24
        CHECK(v.merged.size() == 4 && v.merged.contains(21) && v.merged.at(21).originBlock == 0);
        CHECK(applyCompact(v) == 0); // pinned guests: Hyprdrive leaves them
        CHECK(OmarchyModel::reconcile(v) == 0);
        // the OLD behaviour (one global row) is the fight
        int fights = 0;
        for (int r = 0; r < 3; ++r) {
            for (auto& X : v.ws) // old compact: 1..N globally
                if (X.id >= 21) {
                    X.id -= 20;
                    ++fights;
                }
            v.merged.clear(); // (and a panel blink restores them...)
            fights += OmarchyModel::reconcile(v);
        }
        CHECK(fights == 24);
    }

    std::printf("desktop_rows_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
