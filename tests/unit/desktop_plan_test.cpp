// Host test for DesktopPlan.hpp: a renumber plan is checked whole against every
// ordinary desktop -- refused whole when it would put two desktops on one
// number or one below 1 (the shift(0, -1) that turned 1,2,3 into 1,1,2) --
// and healMoves() pulls apart desktops that already share a number.
#include "DesktopPlan.hpp"

#include <cstdio>
#include <random>
#include <set>

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

static std::vector<Desk> numbered(std::initializer_list<Id> ids) {
    std::vector<Desk> out;
    Key               k = 100;
    for (const Id I : ids)
        out.push_back({k++, I, 1});
    return out;
}

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

int main() {
    // ---- the exact corruption: "paying back place -1" -> shift(0, -1)
    {
        const auto D = numbered({1, 2, 3});
        const auto M = shiftMoves(D, 0, -1);
        CHECK(M.size() == 3);
        CHECK(check(D, M) == Verdict::BelowOne); // refused whole: 2 -> 1 is NOT applied
    }
    // ...and the same with the bad place one higher: 2 -> 1 lands on 1
    {
        const auto D = numbered({1, 2, 3});
        CHECK(check(D, shiftMoves(D, 2, -1)) == Verdict::Collision);
    }
    // ---- ordinary shifts are fine
    {
        const auto D = numbered({1, 2, 3});
        const auto M = shiftMoves(D, 2, +1);
        CHECK(check(D, M) == Verdict::Ok);
        CHECK(unique(applied(D, M)));
        const auto D2 = applied(D, M); // 1,3,4
        CHECK(check(D2, shiftMoves(D2, 3, -1)) == Verdict::Ok);
    }
    {
        // 1,3,4: shift(4,-1) moves 4 -> 3 onto 3
        const auto D = numbered({1, 3, 4});
        CHECK(check(D, shiftMoves(D, 4, -1)) == Verdict::Collision);
    }
    // ---- permutations (the overview's drag) are fine; a moved desk on an untouched number is not
    {
        const auto D = numbered({1, 2, 5});
        CHECK(check(D, {{D[0].key, 5}, {D[2].key, 1}}) == Verdict::Ok);
        CHECK(check(D, {{D[0].key, 2}}) == Verdict::Collision);
        CHECK(check(D, {{D[0].key, 3}}) == Verdict::Ok);
        CHECK(check(D, {{999, 3}}) == Verdict::Unknown);
        CHECK(check(D, {{D[0].key, 3}, {D[0].key, 4}}) == Verdict::Twice);
        CHECK(check(D, {{D[0].key, 0}}) == Verdict::BelowOne);
        CHECK(check(D, {}) == Verdict::Ok);
    }
    // ---- an untouched pair already sharing a number does not block an unrelated plan,
    //      but a plan that moves one of them onto the other's number is refused
    {
        const auto D = numbered({1, 1, 3});
        CHECK(check(D, {{D[2].key, 4}}) == Verdict::Ok);
        CHECK(check(D, shiftMoves(D, 1, +1)) == Verdict::Collision); // both 1s -> 2
    }
    // ---- payMoves: the empty desk in the place goes to the end, the rest come back
    {
        const auto D = numbered({1, 2, 3, 4}); // 2 is the paid place, empty
        const auto M = payMoves(D, 2, D[1].key);
        CHECK(check(D, M) == Verdict::Ok);
        const auto A = applied(D, M);
        CHECK(A[0].id == 1 && A[2].id == 2 && A[3].id == 3 && A[1].id == 4);
        CHECK(payMoves(numbered({1, 2}), 2, 101).empty()); // nothing after it
    }
    // ---- heal: the fuller keeps the number, the other gets the next above the highest
    {
        std::vector<Desk> D = {{1, 1, 1}, {2, 1, 4}, {3, 2, 1}, {4, 7, 2}};
        const auto        M = healMoves(D);
        CHECK(M.size() == 1 && M[0].key == 1 && M[0].to == 8);
        CHECK(check(D, M) == Verdict::Ok);
        CHECK(unique(applied(D, M)));
        // a tie: the older (first in the list) keeps it
        std::vector<Desk> T = {{1, 3, 2}, {2, 3, 2}, {3, 3, 0}};
        const auto        MT = healMoves(T);
        CHECK(MT.size() == 2 && MT[0].key == 2 && MT[0].to == 4 && MT[1].key == 3 && MT[1].to == 5);
        CHECK(unique(applied(T, MT)));
        CHECK(healMoves(numbered({1, 2, 3})).empty());
    }
    // ---- compactMoves: no empty desktops after a display change
    {
        const auto ROOMS = [](std::initializer_list<Room> r) { return std::vector<Room>(r); };
        const auto AFTER = [](std::vector<Room> r, const std::vector<Move>& m) {
            for (const auto& M : m)
                for (auto& R : r)
                    if (R.key == M.key)
                        R.id = M.to;
            return r;
        };
        const auto DESKS = [](const std::vector<Room>& r) {
            std::vector<Desk> d;
            for (const auto& R : r)
                d.push_back({R.key, R.id, 0});
            return d;
        };
        // the report: 21, 22, 28, 29, 30 (21 shown) -> 1..5 in order
        {
            const auto R = ROOMS({{1, 28, true}, {2, 21, true, true}, {3, 30, true}, {4, 22, true}, {5, 29, true}});
            const auto M = compactMoves(R);
            CHECK(check(DESKS(R), M) == Verdict::Ok);
            const auto A = AFTER(R, M);
            CHECK(A[1].id == 1 && A[3].id == 2 && A[0].id == 3 && A[4].id == 4 && A[2].id == 5);
        }
        // holes below, between and above
        {
            const auto R = ROOMS({{1, 3, true, true}, {2, 5, true}, {3, 9, true}});
            const auto A = AFTER(R, compactMoves(R));
            CHECK(A[0].id == 1 && A[1].id == 2 && A[2].id == 3);
        }
        // a shown empty one stays (shown), after the ones with windows; another empty one after it
        {
            const auto R = ROOMS({{1, 1, true}, {2, 2, false, true}, {3, 4, true}, {4, 3, false, false}, {5, 7, false, true}});
            const auto M = compactMoves(R);
            CHECK(check(DESKS(R), M) == Verdict::Ok);
            const auto A = AFTER(R, M);
            CHECK(A[0].id == 1 && A[2].id == 2 && A[1].id == 3 && A[4].id == 4 && A[3].id == 5);
        }
        // already compact: nothing moves (a shown empty one on top is fine)
        CHECK(compactMoves(ROOMS({{1, 1, true}, {2, 2, true}, {3, 3, false, true}})).empty());
        CHECK(compactMoves({}).empty());
        // property: populated ones become 1..N in their order, shown empties next,
        // every number unique, check() accepts the plan, and a second pass moves nothing
        std::mt19937 rng(1111);
        for (int round = 0; round < 20000; ++round) {
            std::vector<Room> R;
            std::set<Id>      used;
            const int         N = rng() % 9;
            for (int i = 0; i < N; ++i) {
                Id id;
                do
                    id = 1 + rng() % 40;
                while (!used.insert(id).second);
                R.push_back({Key(50 + i), id, rng() % 3 != 0, rng() % 4 == 0});
            }
            const auto M = compactMoves(R);
            CHECK(check(DESKS(R), M) == Verdict::Ok);
            const auto A = AFTER(R, M);
            CHECK(unique(DESKS(A)));
            CHECK(compactMoves(A).empty());
            size_t pop = 0;
            for (const auto& X : R)
                pop += X.populated;
            bool ok = true;
            for (size_t i = 0; i < R.size(); ++i) {
                if (R[i].populated && (A[i].id < 1 || A[i].id > Id(pop)))
                    ok = false;
                if (!R[i].populated && A[i].id <= Id(pop))
                    ok = false;
                for (size_t j = 0; j < R.size(); ++j)
                    if (R[i].populated == R[j].populated && R[i].shown == R[j].shown && R[i].id < R[j].id && A[i].id > A[j].id)
                        ok = false; // order kept
            }
            CHECK(ok);
        }
    }
    // ---- property: any plan check() accepts leaves every moved desk on a unique number >= 1
    {
        std::mt19937 rng(7);
        for (int round = 0; round < 20000; ++round) {
            std::vector<Desk> D;
            const int         N = 1 + rng() % 6;
            std::set<Id>      used;
            for (int i = 0; i < N; ++i) {
                Id id;
                do
                    id = 1 + rng() % 8;
                while (!used.insert(id).second);
                D.push_back({Key(10 + i), id, int(rng() % 3)});
            }
            std::vector<Move> M;
            if (rng() % 2)
                M = shiftMoves(D, Id(rng() % 10) - 1, (rng() % 2) ? 1 : -1);
            else
                for (const auto& d : D)
                    if (rng() % 2)
                        M.push_back({d.key, Id(rng() % 10) - 1});
            if (check(D, M) == Verdict::Ok)
                CHECK(unique(applied(D, M)));
        }
    }
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
