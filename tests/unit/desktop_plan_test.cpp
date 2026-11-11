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
