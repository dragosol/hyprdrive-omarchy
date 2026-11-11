// Host test for DesktopPlan.hpp ON ROWS (Row.hpp): every plan Hyprdrive makes
// keeps each desktop inside its monitor's row and never touches another row;
// Omarchy's guests (merged records) are ordinary members, never a reason to
// refuse; and -- against a model of Omarchy's own reconcile
// (omarchy_model.hpp) -- every layout it leaves is a FIXED POINT of it, and
// no merged record ever describes a desktop other than the one under its id
// (Hyprdrive has Omarchy drop the record of every desktop it renumbers:
// Rows::renumbering). desktop_plan_test.cpp is the implicit row (blocks off),
// unchanged.
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
static Desk desk(Key k, Id id, const Row& row, int windows = 1) {
    return {.key = k, .id = id, .windows = windows, .row = row};
}

// ---- Hyprdrive renumbering a model world (DesktopOrder::renumber with
// Rows::renumbering): Omarchy's workspace.created rule for each moved
// desktop under its old number, the moves, then under each new number that
// still has a record. Returns how many desktops moved.
static int hyprdriveApply(OmarchyModel::World& w, const std::vector<Move>& plan) {
    std::set<Id> dropped;
    for (const auto& M : plan)
        for (const auto& X : w.ws)
            if (X.key == M.key && X.id != M.to && w.merged.contains(X.id)) {
                OmarchyModel::created(w, X.id);
                dropped.insert(X.id);
            }
    int          n = 0;
    std::set<Id> arrived;
    for (const auto& M : plan)
        for (auto& X : w.ws)
            if (X.key == M.key && X.id != M.to) {
                X.id = M.to;
                arrived.insert(M.to);
                ++n;
            }
    for (const Id ID : arrived)
        if (!dropped.contains(ID))
            OmarchyModel::created(w, ID);
    return n;
}
static Row rowOfMon(const OmarchyModel::World& w, int mon) {
    return w.block.contains(mon) ? Row::block(w.block.at(mon), w.slots) : Row{};
}
static std::vector<Desk> desksOf(const OmarchyModel::World& w) {
    std::vector<Desk> out;
    for (const auto& X : w.ws)
        out.push_back(desk(X.key, X.id, rowOfMon(w, X.monitor), X.windows));
    return out;
}

// ---- Hyprdrive's compaction over an Omarchy model world (DesktopOrder::compact)
static std::vector<Move> hyprdriveCompact(const OmarchyModel::World& w) {
    std::vector<Desk> desks;
    std::vector<Room> rooms;
    std::vector<Row>  rows;
    for (const auto& X : w.ws) {
        const Row R = rowOfMon(w, X.monitor);
        bool      shown = false;
        for (const auto& [M, K] : w.shown)
            shown |= K == X.key;
        desks.push_back(desk(X.key, X.id, R, X.windows));
        rooms.push_back({X.key, X.id, X.windows, shown});
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
    return hyprdriveApply(w, hyprdriveCompact(w));
}
// Each connected monitor's row: its populated members are its first places,
// in a row (overflow included), whatever records there are.
static bool contiguous(const OmarchyModel::World& w) {
    for (const auto& [M, B] : w.block) {
        const Row R = Row::block(B, w.slots);
        Id        top = 0, n = 0;
        for (const auto& X : w.ws)
            if (X.monitor == M && R.contains(X.id) && X.windows) {
                top = std::max(top, R.slot(X.id));
                ++n;
            }
        // every place below the top holds one of them, or a desktop of
        // another monitor numbered in this row (Omarchy's to move)
        for (Id s = 1; s < top; ++s) {
            bool ok = false;
            for (const auto& X : w.ws)
                ok |= X.id == R.idOf(s) && (X.monitor != M || X.windows);
            if (!ok)
                return false;
        }
        (void)n;
    }
    return true;
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
        if (!R.contains(X.id) && !full)
            return false;
    }
    return true;
}

int main() {
    const Row B0 = Row::block(0, 10), B1 = Row::block(1, 10), B2 = Row::block(2, 10);

    // ---- check: a move out of the row is refused; nothing else is
    {
        const std::vector<Desk> D{desk(1, 11, B1), desk(2, 12, B1), desk(3, 21, B2)};
        CHECK(check(D, {{1, 13}}) == Verdict::Ok);
        CHECK(check(D, {{1, 21}}) == Verdict::OutsideRow); // another monitor's block
        CHECK(check(D, {{1, 3}}) == Verdict::OutsideRow);
        CHECK(check(D, {{1, 10}}) == Verdict::OutsideRow); // below the row
        CHECK(check(D, {{2, 21}}) == Verdict::OutsideRow);
        CHECK(check(D, {{1, B1.idOf(11)}}) == Verdict::Ok); // its own overflow
        CHECK(check(D, {{1, B2.idOf(11)}}) == Verdict::OutsideRow);
        // an Omarchy guest (desk 1, a merged record) is a member like any other
        const std::vector<Desk> P{desk(1, 21, B2), desk(2, 22, B2)};
        CHECK(check(P, {{1, 23}}) == Verdict::Ok);
        CHECK(check(P, {{1, 22}}) == Verdict::Collision); // a real collision still is refused
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
        const std::vector<Desk> D{desk(1, 21, B2), desk(2, 22, B2, 0), desk(3, 23, B2), desk(4, 1'002'001, B2), desk(5, 11, B1)};
        const auto              M = payMoves(D, B2, 22, 2);
        CHECK(check(D, M) == Verdict::Ok);
        const auto A = applied(D, M);
        CHECK(A[2].id == 22 && A[3].id == 30 && A[1].id == 1'002'001 && A[4].id == 11); // the overflow desk comes back into the block
    }
    // ---- compact in a block with a guest (2) and an overflow desktop: all
    // of them from the row's first place, the shown empty one after
    {
        const std::vector<Room> R{{1, 22, true}, {2, 24, true}, {3, 27, true}, {4, 1'002'001, true}, {5, 29, false, true}};
        const auto              M = compactMoves(R, B2);
        std::map<Key, Id>       to{{1, 22}, {2, 24}, {3, 27}, {4, 1'002'001}, {5, 29}};
        for (const auto& X : M)
            to[X.key] = X.to;
        CHECK(to[1] == 21 && to[2] == 22 && to[3] == 23 && to[4] == 24 && to[5] == 25);
        // idempotent
        std::vector<Room> R2 = R;
        for (auto& X : R2)
            X.id = to[X.key];
        CHECK(compactMoves(R2, B2).empty());
    }
    // ---- heal in a row: the loser gets the first free place in ITS row
    {
        const std::vector<Desk> D{desk(1, 21, B2, 2), desk(2, 21, B2, 1), desk(3, 22, B2), desk(4, 11, B1)};
        const auto              M = healMoves(D);
        CHECK(M.size() == 1 && M[0].key == 2 && M[0].to == 23);
        CHECK(check(D, M) == Verdict::Ok);
        // a full block: the loser goes to overflow, not to 31
        std::vector<Desk> F;
        for (Id i = 21; i <= 30; ++i)
            F.push_back(desk(Key(i), i, B2));
        F.push_back(desk(99, 25, B2, 0));
        const auto MF = healMoves(F);
        CHECK(MF.size() == 1 && MF[0].to == 1'002'001);
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
                D.push_back(desk(k++, ID, R, int(rng() % 3)));
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
                if (member(X, ROW)) {
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
                if (A != B && sameRow(H[A].row, H[B].row))
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
                        group.push_back({X.key, X.id, X.windows > 0, false});
                const auto M = compactMoves(group, R);
                plan.insert(plan.end(), M.begin(), M.end());
                // populated first..: places 1..n, in their old order
                const auto      A = applied(D, M);
                std::vector<Id> oldOrder;
                for (const auto& X : D)
                    if (member(X, R) && X.windows > 0)
                        oldOrder.push_back(R.slot(X.id));
                std::ranges::sort(oldOrder);
                Id   s       = 0;
                bool ordered = true;
                for (const Id OLD : oldOrder) {
                    ++s;
                    bool found = false;
                    for (size_t i = 0; i < D.size(); ++i)
                        if (member(D[i], R) && D[i].windows > 0 && R.slot(D[i].id) == OLD)
                            found = R.slot(A[i].id) == s;
                    ordered &= found;
                }
                CHECK(ordered);
                // idempotent
                std::vector<Room> again;
                for (size_t i = 0; i < D.size(); ++i)
                    if (member(D[i], R))
                        again.push_back({A[i].key, A[i].id, A[i].windows > 0, false});
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
        // guests: records for blocks no connected monitor owns, and stale
        // ones (no desktop under the id)
        for (const auto& X : w.ws)
            if (rng() % 7 == 0) {
                const Id ORIGIN = 4 + Id(rng() % 3);
                w.merged[X.id] = {ORIGIN * 10 + 1 + Id(rng() % 10), ORIGIN, X.key};
            }
        for (int i = int(rng() % 3); i > 0; --i)
            if (const Id ID = 1 + Id(rng() % 40); !OmarchyModel::taken(w, ID))
                w.merged[ID] = {41, 4, 0};
        CHECK(OmarchyModel::recordsTrue(w));

        // the fixed point: once Hyprdrive compacted, Omarchy changes nothing
        // (and its renumbers, whatever the start, are never undone)
        int  rounds = 0, lastOmarchy = -1, lastHyprdrive = -1;
        for (; rounds < 6; ++rounds) {
            lastOmarchy = OmarchyModel::reconcile(w);
            OmarchyModel::collect(w);
            lastHyprdrive = applyCompact(w);
            OmarchyModel::collect(w);
            CHECK(OmarchyModel::recordsTrue(w));
            if (rounds >= 1 && lastOmarchy == 0 && lastHyprdrive == 0)
                break;
        }
        CHECK(rounds <= 2);
        const auto BEFORE = ids(w);
        CHECK(OmarchyModel::reconcile(w) == 0);
        CHECK(ids(w) == BEFORE);
        CHECK(applyCompact(w) == 0);
        CHECK(settledWell(w));
        CHECK(contiguous(w));
        CHECK(OmarchyModel::recordsTrue(w));
        if ((!settledWell(w) || !contiguous(w)) && std::getenv("DUMP")) {
            for (const auto& [M, B] : w.block)
                std::printf(" mon%d=block%lld", M, B);
            for (const auto& X : w.ws)
                std::printf(" [k%lu id%lld m%d w%d%s]", X.key, X.id, X.monitor, X.windows, w.merged.contains(X.id) ? " guest" : "");
            std::printf("\n");
        }
    }

    // ---- ANY SEQUENCE of Hyprdrive's operations (fullscreen trips with
    // their room made and paid back, strip reorders, heals, compactions)
    // interleaved with Omarchy's reconciles, Hyprland dropping empty
    // desktops, and monitors (a guest's origin among them) coming and going:
    // no plan is ever refused for a record, no record ever describes a
    // desktop other than the one under its id, and after a compaction the
    // rows are contiguous and a fixed point of the next reconcile.
    for (int round = 0; round < 2000; ++round) {
        OmarchyModel::World w;
        w.known = {0, 1, 2, 3};
        w.block[0] = 2; // the external monitor
        if (rng() % 2)
            w.block[1] = 1;
        unsigned long k = 1;
        // the owner's state: guests from the panel (block 0) merged into
        // block 2, records "origin = panel", plus own desktops
        const int GUESTS = 1 + int(rng() % 5);
        for (int i = 0; i < GUESTS; ++i)
            w.ws.push_back({k++, Id(1 + i), 0, true});
        w.shown[0] = 1;
        OmarchyModel::reconcile(w);
        CHECK(int(w.merged.size()) == GUESTS);
        for (int i = int(rng() % 4); i > 0; --i) {
            const Id ID = 21 + Id(rng() % 10);
            if (!OmarchyModel::taken(w, ID))
                w.ws.push_back({k++, ID, 0, true});
        }
        CHECK(OmarchyModel::recordsTrue(w));
        for (int step = 0; step < 25; ++step) {
            std::vector<int> mons;
            for (const auto& [M, B] : w.block)
                mons.push_back(M);
            const int MON = mons[rng() % mons.size()];
            const Row R   = rowOfMon(w, MON);
            std::vector<OmarchyModel::Ws*> mem;
            for (auto& X : w.ws)
                if (X.monitor == MON && R.contains(X.id))
                    mem.push_back(&X);
            std::ranges::sort(mem, [&](auto* a, auto* b) { return R.slot(a->id) < R.slot(b->id); });
            const auto D = desksOf(w);
            switch (rng() % 8) {
                case 0: { // a fullscreen trip from a member: room at the next place, the trip's desktop made there
                    if (mem.empty())
                        break;
                    const auto* H  = mem[rng() % mem.size()];
                    const Id    AT = R.idOf(R.slot(H->id) + 1);
                    const auto  M  = shiftMoves(D, R, AT, +1);
                    const auto  V  = check(D, M);
                    CHECK(V == Verdict::Ok || V == Verdict::Collision); // (another monitor's desktop numbered in this row)
                    if (V != Verdict::Ok || !AT)
                        break;
                    hyprdriveApply(w, M);
                    if (OmarchyModel::taken(w, AT))
                        break;
                    w.ws.push_back({k++, AT, MON, true});
                    OmarchyModel::created(w, AT); // (Hyprland creates it: Omarchy's rule)
                    w.shown[MON] = w.ws.back().key;
                    break;
                }
                case 1: { // a place paid back: a member emptied, the ones after it come back
                    if (mem.empty())
                        break;
                    auto* E = mem[rng() % mem.size()];
                    E->windows = false;
                    const auto M = payMoves(desksOf(w), R, E->id, E->key);
                    const auto V = check(desksOf(w), M);
                    CHECK(V == Verdict::Ok || V == Verdict::Collision);
                    if (V == Verdict::Ok)
                        hyprdriveApply(w, M);
                    break;
                }
                case 2: { // a strip reorder: the row's own numbers, permuted
                    std::vector<Id> pool;
                    for (auto* X : mem)
                        pool.push_back(X->id);
                    std::ranges::shuffle(pool, rng);
                    std::vector<Move> M;
                    for (size_t i = 0; i < mem.size(); ++i)
                        M.push_back({mem[i]->key, pool[i]});
                    const auto V = check(D, M);
                    CHECK(V == Verdict::Ok);
                    hyprdriveApply(w, M);
                    break;
                }
                case 3: // a display change / config reload: Omarchy, then Hyprdrive compacts
                    OmarchyModel::reconcile(w);
                    OmarchyModel::collect(w);
                    applyCompact(w);
                    CHECK(contiguous(w));
                    break;
                case 4: { // a window closes: Hyprland drops the empty desktop (its record goes stale)
                    if (mem.empty())
                        break;
                    mem[rng() % mem.size()]->windows = false;
                    OmarchyModel::collect(w);
                    break;
                }
                case 5: { // a window opens on a new desktop (Hyprland creates it)
                    const Id ID = R.idOf(1 + Id(rng() % 12));
                    if (ID && !OmarchyModel::taken(w, ID)) {
                        w.ws.push_back({k++, ID, MON, true});
                        OmarchyModel::created(w, ID);
                    }
                    break;
                }
                case 6: // the origin monitor (the panel, block 0) comes back, or goes
                    if (w.block.contains(7))
                        w.block.erase(7);
                    else
                        w.block[7] = 0;
                    for (auto& X : w.ws)
                        if (!w.block.contains(X.monitor))
                            X.monitor = 0; // its desktops wait on the external
                    std::erase_if(w.shown, [&](const auto& P) { return !w.block.contains(P.first); });
                    OmarchyModel::reconcile(w); // restore() / merge(): Omarchy's own moves
                    OmarchyModel::collect(w);
                    break;
                default: { // heal (nothing to heal: no-op) and a plain reload
                    const auto M = healMoves(D);
                    CHECK(M.empty());
                    OmarchyModel::reconcile(w);
                    break;
                }
            }
            CHECK(OmarchyModel::recordsTrue(w));
            if (!OmarchyModel::recordsTrue(w) && std::getenv("DUMP")) {
                std::printf("step %d:", step);
                for (const auto& [ID, Rc] : w.merged)
                    std::printf(" rec %lld->desk %lu", ID, Rc.desk);
                for (const auto& X : w.ws)
                    std::printf(" [k%lu id%lld m%d w%d]", X.key, X.id, X.monitor, X.windows);
                std::printf("\n");
                return 1;
            }
        }
        // then a display change settles it: contiguous, and a fixed point
        OmarchyModel::reconcile(w);
        OmarchyModel::collect(w);
        applyCompact(w);
        OmarchyModel::collect(w);
        CHECK(contiguous(w));
        CHECK(OmarchyModel::recordsTrue(w));
        const auto SETTLED = ids(w);
        CHECK(OmarchyModel::reconcile(w) == 0);
        CHECK(ids(w) == SETTLED);
        CHECK(applyCompact(w) == 0);
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
    // ("origin = panel"). Guests are members: a trip from the 2nd makes room
    // through them, the payback and a reorder go through, a hole closes at
    // the next display change, and every record left still describes the
    // desktop under its id; the next reconcile changes nothing.
    {
        OmarchyModel::World w;
        w.block[0] = 2;
        for (unsigned long i = 1; i <= 4; ++i)
            w.ws.push_back({i, Id(20 + i), 0, true});
        w.shown[0] = 1;
        CHECK(OmarchyModel::reconcile(w) == 0 && applyCompact(w) == 0);

        OmarchyModel::World v;
        v.block[0] = 2;
        v.known    = {0, 2};
        for (unsigned long i = 1; i <= 4; ++i)
            v.ws.push_back({i, Id(i), 0, true});
        v.shown[0] = 1;
        CHECK(OmarchyModel::reconcile(v) == 4); // block 0 has no connected owner: merged to 21..24
        CHECK(v.merged.size() == 4 && v.merged.contains(21) && v.merged.at(21).originBlock == 0 && v.merged.at(21).desk == 1);
        CHECK(applyCompact(v) == 0); // already contiguous
        CHECK(OmarchyModel::reconcile(v) == 0);
        // a fullscreen trip from the 2nd (22): room at 23, 23 24 -> 24 25
        const auto TRIP = shiftMoves(desksOf(v), B2, 23, +1);
        CHECK(TRIP.size() == 2 && check(desksOf(v), TRIP) == Verdict::Ok);
        hyprdriveApply(v, TRIP);
        v.ws.push_back({5, 23, 0, true}); // the trip's desktop
        OmarchyModel::created(v, 23);
        CHECK(OmarchyModel::recordsTrue(v));
        CHECK(v.merged.size() == 2 && v.merged.contains(21) && v.merged.contains(22)); // 3 and 4 adopted
        CHECK(OmarchyModel::reconcile(v) == 0);
        // the trip ends: 23 empty, paid back (24 25 -> 23 24, 23 to the top)
        v.ws[4].windows = false;
        const auto PAY  = payMoves(desksOf(v), B2, 23, 5);
        CHECK(check(desksOf(v), PAY) == Verdict::Ok);
        hyprdriveApply(v, PAY);
        OmarchyModel::collect(v);
        CHECK(ids(v).at(3) == 23 && ids(v).at(4) == 24 && OmarchyModel::recordsTrue(v));
        // a strip reorder: the guest on 21 swaps with 22
        const std::vector<Move> SWAP{{1, 22}, {2, 21}};
        CHECK(check(desksOf(v), SWAP) == Verdict::Ok);
        hyprdriveApply(v, SWAP);
        CHECK(v.merged.empty() && OmarchyModel::recordsTrue(v));
        CHECK(OmarchyModel::reconcile(v) == 0);
        // a STALE record: 22's windows close (Hyprland drops it, the record
        // would stay); a display change closes the hole, nothing blocks
        v.merged[22] = {2, 0, 1};
        std::erase_if(v.ws, [](const OmarchyModel::Ws& x) { return x.id == 22; });
        CHECK(OmarchyModel::reconcile(v) == 0);
        CHECK(applyCompact(v) == 2); // 23 24 -> 22 23
        CHECK(!v.merged.contains(22) && OmarchyModel::recordsTrue(v) && contiguous(v));
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
        CHECK(fights == 18);
    }

    // ---- THE ORIGIN REALLY COMES BACK: guests Hyprdrive never moved keep
    // their records and Omarchy's restore() takes them home; the one
    // Hyprdrive moved is the external's for good; both rows then compact.
    {
        OmarchyModel::World v;
        v.block[0] = 2;
        v.known    = {0, 1, 2};
        for (unsigned long i = 1; i <= 3; ++i)
            v.ws.push_back({i, Id(10 + i), 0, true}); // from block 1's monitor, unplugged
        v.shown[0] = 1;
        CHECK(OmarchyModel::reconcile(v) == 3); // 21 22 23, records origin block 1
        const std::vector<Move> SWAP{{2, 23}, {3, 22}};
        hyprdriveApply(v, SWAP);
        CHECK(v.merged.size() == 1 && v.merged.contains(21) && OmarchyModel::recordsTrue(v));
        v.block[1] = 1; // block 1's monitor is plugged in again
        CHECK(OmarchyModel::reconcile(v) == 1); // restore(): 21 -> 11 on monitor 1
        CHECK(ids(v).at(1) == 11 && v.ws[0].monitor == 1 && v.merged.empty());
        OmarchyModel::collect(v);
        CHECK(applyCompact(v) == 2); // the external's 22 23 -> 21 22
        CHECK(contiguous(v) && OmarchyModel::reconcile(v) == 0 && applyCompact(v) == 0);
    }

    std::printf("desktop_rows_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
