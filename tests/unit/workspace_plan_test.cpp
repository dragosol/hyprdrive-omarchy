// Host test for WorkspacePlan::plan -- which desktop ids each monitor's strip
// may create. Needs no compositor. The multi-monitor cases are the user's bug
// (2026-09-14: DP-1's spare desktop was one that already lived on eDP-1) and
// the regressions a later edit could reintroduce.

#include "WorkspacePlan.hpp"

#include <cstdio>
#include <tuple>
#include <string>

using namespace Hyprdrive::WorkspacePlan;

static int g_fail = 0, g_run = 0;

static void ok(bool cond, const std::string& what) {
    ++g_run;
    if (!cond) {
        ++g_fail;
        std::printf("FAIL  %s\n", what.c_str());
    }
}

static std::string ids(const std::vector<ID>& v) {
    std::string s;
    for (auto i : v)
        s += (s.empty() ? "" : ",") + std::to_string(i);
    return "[" + s + "]";
}

// Every invariant, on any plan: a new spare never collides with an existing id
// or another monitor's spare, a gap is never an existing id, and no id is
// planned twice across the whole output.
static void invariants(const std::string& name, const std::map<ID, SRow>& p, const std::vector<SWorkspace>& ws) {
    std::set<ID> existing, planned;
    for (const auto& W : ws)
        if (W.id > 0)
            existing.insert(W.id);

    for (const auto& [MON, ROW] : p) {
        if (ROW.spare) {
            if (ROW.existing) {
                bool mine = false;
                for (const auto& W : ws)
                    mine |= W.id == *ROW.spare && W.monitor == MON && W.windows == 0;
                ok(mine, name + ": reused spare " + std::to_string(*ROW.spare) + " is an empty desktop on monitor " + std::to_string(MON));
            } else {
                ok(!existing.contains(*ROW.spare), name + ": new spare " + std::to_string(*ROW.spare) + " does not already exist");
                ok(!planned.contains(*ROW.spare), name + ": spare " + std::to_string(*ROW.spare) + " planned once");
                planned.insert(*ROW.spare);
            }
        }
        for (auto g : ROW.gaps) {
            bool ownPhantom = false;
            for (const auto& W : ws)
                ownPhantom |= W.id == g && W.monitor == MON && W.held && W.windows == 0;
            ok(ownPhantom || !existing.contains(g), name + ": gap " + std::to_string(g) + " does not already exist (or is this monitor's phantom)");
            ok(!planned.contains(g), name + ": gap " + std::to_string(g) + " planned once");
            planned.insert(g);
        }
    }
}

static std::map<ID, SRow> check(const std::string& name, std::vector<ID> mons, const std::vector<SWorkspace>& ws, const BoundMonitor& bound = {}) {
    auto p = plan(mons, ws, bound);
    invariants(name, p, ws);

    // Order independence (rule 4): the same facts in any monitor order and any
    // workspace order give the same plan.
    auto revMons = mons;
    std::reverse(revMons.begin(), revMons.end());
    auto revWs = ws;
    std::reverse(revWs.begin(), revWs.end());
    const auto Q = plan(revMons, revWs, bound);
    bool same = Q.size() == p.size();
    for (const auto& [M, R] : p) {
        const auto IT = Q.find(M);
        same = same && IT != Q.end() && IT->second.spare == R.spare && IT->second.existing == R.existing && IT->second.gaps == R.gaps;
    }
    ok(same, name + ": plan independent of input order");
    return p;
}

int main() {
    // Single monitor: behaviour unchanged from the pre-plan code.
    {
        auto p = check("single", {0}, {{1, 0, 2}, {2, 0, 1}, {3, 0, 1}});
        ok(p[0].spare == 4 && !p[0].existing, "single: spare is occupied+1");
        ok(p[0].gaps.empty(), "single: no gaps");
    }
    {
        auto p = check("single holes", {0}, {{1, 0, 1}, {5, 0, 1}});
        ok(ids(p[0].gaps) == "[2,3,4]", "single holes: fills 2..4, got " + ids(p[0].gaps));
        ok(p[0].spare == 6, "single holes: spare 6");
    }
    {
        auto p = check("single jumped", {0}, {{5, 0, 1}});
        ok(ids(p[0].gaps) == "[1,2,3,4]", "single jumped: desktop 5 shows five cells, got " + ids(p[0].gaps));
    }
    {
        auto p = check("single reuse", {0}, {{1, 0, 1}, {2, 0, 0}});
        ok(p[0].spare == 2 && p[0].existing, "single reuse: the existing empty desktop 2 is the spare");
    }
    {
        auto p = check("single empty monitor", {0}, {{1, 0, 0}});
        ok(!p[0].spare, "single empty monitor: no windows, no spare");
    }
    {
        auto p = check("single empty jumped", {0}, {{5, 0, 0}});
        ok(!p[0].spare && ids(p[0].gaps) == "[1,2,3,4]", "single empty jumped: empty desktop 5 still shows five cells, got " + ids(p[0].gaps));
    }
    {
        auto p = check("single ceiling", {0}, {{300, 0, 1}});
        ok(p[0].gaps.size() == 32, "single ceiling: at most 32 phantom desktops, got " + std::to_string(p[0].gaps.size()));
    }

    // THE BUG: eDP-1 has 1,2,3,6,7; DP-1 has 4,5. DP-1's spare was 6.
    {
        const std::vector<SWorkspace> WS = {{1, 0, 1}, {2, 0, 1}, {3, 0, 1}, {6, 0, 1}, {7, 0, 1}, {4, 1, 1}, {5, 1, 1}};
        auto                          p  = check("user layout", {0, 1}, WS);
        ok(p[0].spare == 8, "user layout: eDP-1 spare 8, got " + std::to_string(p[0].spare.value_or(-1)));
        ok(p[1].spare == 9, "user layout: DP-1 spare is not 6/7/8 (eDP-1's), got " + std::to_string(p[1].spare.value_or(-1)));
        ok(p[0].gaps.empty() && p[1].gaps.empty(), "user layout: no gaps claimed across monitors");
    }
    // ...and once eDP-1's spare 8 exists, a rebuild keeps both answers.
    {
        const std::vector<SWorkspace> WS = {{1, 0, 1}, {2, 0, 1}, {3, 0, 1}, {6, 0, 1}, {7, 0, 1}, {8, 0, 0}, {4, 1, 1}, {5, 1, 1}};
        auto                          p  = check("user layout, eDP spare created", {0, 1}, WS);
        ok(p[0].spare == 8 && p[0].existing, "stable: eDP-1 reuses 8");
        ok(p[1].spare == 9 && !p[1].existing, "stable: DP-1 still plans 9");
    }
    // ...and if DP-1's overview happened to create its spare FIRST.
    {
        const std::vector<SWorkspace> WS = {{1, 0, 1}, {2, 0, 1}, {3, 0, 1}, {6, 0, 1}, {7, 0, 1}, {4, 1, 1}, {5, 1, 1}, {9, 1, 0}};
        auto                          p  = check("user layout, DP spare created first", {0, 1}, WS);
        ok(p[0].spare == 8 && !p[0].existing, "stable: eDP-1 plans 8");
        ok(p[1].spare == 9 && p[1].existing, "stable: DP-1 reuses 9");
    }
    // Both having everything created: nothing new planned at all.
    {
        const std::vector<SWorkspace> WS = {{1, 0, 1}, {8, 0, 0}, {2, 1, 1}, {9, 1, 0}};
        auto                          p  = check("settled", {0, 1}, WS);
        ok(p[0].existing && p[1].existing, "settled: both reuse");
    }

    // The external monitor holds the LOW ids.
    {
        auto p = check("external low", {0, 1}, {{3, 0, 1}, {1, 1, 1}, {2, 1, 1}});
        ok((p[1].spare == 4 && p[0].spare == 5) || (p[0].spare == 4 && p[1].spare == 5), "external low: spares 4 and 5, one each");
        ok(p[0].spare == 4, "external low: monitor 0 plans first and gets 4");
    }

    // Gaps: a hole between two of MY desktops is mine; a hole at a border is nobody's.
    {
        // eDP: 1, 3   DP: 5
        auto p = check("gap ownership", {0, 1}, {{1, 0, 1}, {3, 0, 1}, {5, 1, 1}});
        ok(ids(p[0].gaps) == "[2]", "gap ownership: eDP-1 fills 2, got " + ids(p[0].gaps));
        ok(p[1].gaps.empty(), "gap ownership: DP-1 does not claim eDP-1's 2 or the border 4, got " + ids(p[1].gaps));
    }
    {
        // DP: 3   eDP: 5  -> ids 1,2 lie below DP's lowest: DP's; 4 is a border.
        auto p = check("gap below lowest", {0, 1}, {{5, 0, 1}, {3, 1, 1}});
        ok(p[0].gaps.empty(), "gap below lowest: eDP-1 claims nothing, got " + ids(p[0].gaps));
        ok(ids(p[1].gaps) == "[1,2]", "gap below lowest: DP-1 fills 1,2, got " + ids(p[1].gaps));
    }
    {
        // A spare planned for DP sits inside eDP's range: it is a border for eDP.
        // eDP: 1, 7 (hole 2..6)   DP: 1? no -- DP: 2 occupied -> DP spare 3.
        auto p = check("spare is a neighbour", {0, 1}, {{1, 0, 1}, {7, 0, 1}, {2, 1, 1}});
        ok(p[1].spare == 3, "spare is a neighbour: DP-1 spare 3");
        for (auto g : p[0].gaps)
            ok(g > 3, "spare is a neighbour: eDP-1 gap " + std::to_string(g) + " is not below DP-1's desktops");
    }

    // Workspace rules binding ids to a monitor.
    {
        const BoundMonitor BOUND = [](ID id) -> std::optional<ID> {
            if (id == 4)
                return 1; // workspace 4 is bound to DP-1
            return std::nullopt;
        };
        auto p = check("bound", {0, 1}, {{1, 0, 1}, {2, 0, 1}, {3, 0, 1}}, BOUND);
        ok(p[0].spare == 5, "bound: eDP-1 skips 4 (bound to DP-1), got " + std::to_string(p[0].spare.value_or(-1)));

        auto q = check("bound own", {0, 1}, {{3, 1, 1}}, BOUND);
        ok(q[1].spare == 4, "bound own: DP-1 may use its own bound 4");
        ok(ids(q[1].gaps) == "[1,2]", "bound own: DP-1 gaps 1,2, got " + ids(q[1].gaps));
    }

    // Three monitors, all interleaved.
    {
        const std::vector<SWorkspace> WS = {{1, 0, 1}, {4, 0, 1}, {2, 1, 1}, {5, 1, 1}, {3, 2, 1}, {6, 2, 0}};
        auto                          p  = check("three", {0, 1, 2}, WS);
        ok(p[2].spare == 6 && p[2].existing, "three: monitor 2 reuses its empty 6");
        ok(p[0].spare == 7, "three: monitor 0 spare 7, got " + std::to_string(p[0].spare.value_or(-1)));
        ok(p[1].spare == 8, "three: monitor 1 spare 8, got " + std::to_string(p[1].spare.value_or(-1)));
        ok(p[0].gaps.empty() && p[1].gaps.empty() && p[2].gaps.empty(), "three: interleaved ids leave no one's gaps");
    }

    // Rule 6: phantoms are kept while they qualify, and released when they stop.
    {
        auto p = check("phantom kept", {0}, {{1, 0, 1}, {2, 0, 0, true}, {3, 0, 1}});
        ok(ids(p[0].gaps) == "[2]", "phantom kept: the held gap 2 stays planned, got " + ids(p[0].gaps));
        ok(p[0].spare == 4, "phantom kept: spare 4");
    }
    {
        // The window on 3 moved away: 2 is no longer below anything occupied.
        auto p = check("phantom released", {0}, {{1, 0, 1}, {2, 0, 0, true}, {3, 0, 0}});
        ok(p[0].gaps.empty(), "phantom released: nothing to hold, got " + ids(p[0].gaps));
        ok(p[0].spare == 2 && p[0].existing, "phantom released: the phantom is the nearest empty and becomes the spare");
    }
    {
        // eDP holds phantom 2 between its 1 and 3; DP's spare must not take 2.
        auto p = check("phantom taken", {0, 1}, {{1, 0, 1}, {2, 0, 0, true}, {3, 0, 1}, {1000, 1, 0}});
        ok(ids(p[0].gaps) == "[2]", "phantom taken: eDP keeps 2");
        auto q = check("phantom vs spare", {0, 1}, {{1, 1, 1}, {3, 0, 1}, {2, 0, 0, true}});
        ok(q[1].spare != 2, "phantom vs spare: DP-1's spare never lands on eDP-1's phantom 2");
        ok(std::find(q[1].gaps.begin(), q[1].gaps.end(), 2) == q[1].gaps.end(), "phantom vs spare: nor does its gap list");
    }

    // Special/negative ids never enter the plan.
    {
        auto p = check("special", {0}, {{-97, 0, 3}, {1, 0, 1}});
        ok(p[0].spare == 2, "special: scratchpad windows do not raise the spare");
    }

    // Exhaustive-ish: random layouts on up to 3 monitors, invariants + order independence.
    {
        uint64_t s = 0x9E3779B97F4A7C15ULL;
        auto     rnd = [&s](uint64_t n) {
            s ^= s << 13;
            s ^= s >> 7;
            s ^= s << 17;
            return s % n;
        };
        for (int t = 0; t < 4000; ++t) {
            const ID                MONS = 1 + static_cast<ID>(rnd(3));
            std::vector<ID>         mons;
            for (ID m = 0; m < MONS; ++m)
                mons.push_back(m);
            std::vector<SWorkspace> ws;
            std::set<ID>            used;
            const int               N = 1 + rnd(8);
            for (int i = 0; i < N; ++i) {
                ID id = 1 + rnd(14);
                if (used.contains(id))
                    continue;
                used.insert(id);
                const size_t WIN = rnd(3);
                ws.push_back({id, static_cast<ID>(rnd(MONS)), WIN, WIN == 0 && rnd(3) == 0});
            }
            check("random#" + std::to_string(t), mons, ws);
        }
    }

    // CONVERGENCE. Apply plans the way the overviews do -- each monitor's
    // overview rebuilding in a random order, creating what its row asks for and
    // releasing what it holds and no longer needs -- and require that the
    // desktops settle within a few rounds, never with two of the same id.
    {
        uint64_t s = 0xD1B54A32D192ED03ULL;
        auto     rnd = [&s](uint64_t n) {
            s ^= s << 13;
            s ^= s >> 7;
            s ^= s << 17;
            return s % n;
        };
        struct SLive {
            SWorkspace ws;
            bool       spareOf = false; // held by its monitor's overview as the trailing empty
        };
        int unsettled = 0, dupes = 0;
        for (int t = 0; t < 3000; ++t) {
            const ID           MONS = 1 + static_cast<ID>(rnd(3));
            std::vector<ID>    mons;
            for (ID m = 0; m < MONS; ++m)
                mons.push_back(m);
            std::vector<SLive> live;
            std::set<ID>       used;
            for (int i = 0, n = 1 + rnd(7); i < n; ++i) {
                ID id = 1 + rnd(12);
                if (used.insert(id).second)
                    live.push_back({{id, static_cast<ID>(rnd(MONS)), rnd(3)}});
            }

            const auto facts = [&] {
                std::vector<SWorkspace> f;
                for (const auto& L : live)
                    f.push_back(L.ws);
                return f;
            };
            const auto rebuild = [&](ID mon) {
                const auto ROW = plan(mons, facts(), {})[mon];
                // Release what this overview holds and the row no longer wants.
                std::erase_if(live, [&](const SLive& L) {
                    if (L.ws.monitor != mon || L.ws.windows > 0)
                        return false;
                    if (L.ws.held && std::find(ROW.gaps.begin(), ROW.gaps.end(), L.ws.id) == ROW.gaps.end())
                        return true;
                    return L.spareOf && ROW.spare != L.ws.id;
                });
                const auto exists = [&](ID id) { return std::any_of(live.begin(), live.end(), [id](const SLive& L) { return L.ws.id == id; }); };
                if (ROW.spare && !ROW.existing && !exists(*ROW.spare))
                    live.push_back({{*ROW.spare, mon, 0, false}, true});
                for (auto g : ROW.gaps)
                    if (!exists(g))
                        live.push_back({{g, mon, 0, true}});
            };
            const auto snapshot = [&] {
                std::vector<std::tuple<ID, ID, bool>> v;
                for (const auto& L : live)
                    v.emplace_back(L.ws.id, L.ws.monitor, L.ws.held);
                std::sort(v.begin(), v.end());
                return v;
            };

            for (int round = 0; round < 4; ++round) {
                auto order = mons;
                for (size_t i = order.size(); i > 1; --i)
                    std::swap(order[i - 1], order[rnd(i)]);
                for (auto m : order)
                    rebuild(m);
            }
            const auto BEFORE = snapshot();
            for (auto m : mons)
                rebuild(m);
            if (snapshot() != BEFORE)
                ++unsettled;

            std::set<ID> seen;
            for (const auto& L : live)
                if (!seen.insert(L.ws.id).second)
                    ++dupes;
        }
        ok(unsettled == 0, "convergence: every layout settles within 4 rebuild rounds, unsettled=" + std::to_string(unsettled));
        ok(dupes == 0, "convergence: no two desktops ever share an id, dupes=" + std::to_string(dupes));
    }

    // ---- ROWS (Omarchy's per-monitor blocks): the owner's case, one external
    // monitor owning block 2 with desktops 21..24 -- no "20 empty desktops
    // below 21", the spare is 25.
    {
        using Hyprdrive::Row;
        const std::vector<SWorkspace> WS{{21, 1, 2}, {22, 1, 1}, {23, 1, 1}, {24, 1, 3}};
        const auto                    P = plan({1}, WS, {}, {{1, Row::block(2, 10)}});
        invariants("rows: block 2", P, WS);
        ok(P.at(1).gaps.empty(), "rows: block 2 fills no gap below 21, got " + ids(P.at(1).gaps));
        ok(P.at(1).spare == 25 && !P.at(1).existing, "rows: block 2's spare is 25");
        // and with the implicit row the same desktops DO get 1..20 (the bug)
        const auto OLD = plan({1}, WS);
        ok(OLD.at(1).gaps.size() == 20, "rows: the implicit row fills 1..20 below 21 (the old behaviour), got " + std::to_string(OLD.at(1).gaps.size()));
    }
    // two monitors, blocks 0 and 1: holes only inside each block
    {
        using Hyprdrive::Row;
        const std::vector<SWorkspace> WS{{1, 1, 1}, {3, 1, 1}, {11, 2, 1}, {14, 2, 1}};
        const auto                    P = plan({1, 2}, WS, {}, {{1, Row::block(0, 10)}, {2, Row::block(1, 10)}});
        invariants("rows: two blocks", P, WS);
        ok(P.at(1).gaps == std::vector<ID>{2}, "rows: block 0 fills 2, got " + ids(P.at(1).gaps));
        ok(P.at(2).gaps == (std::vector<ID>{12, 13}), "rows: block 1 fills 12,13, got " + ids(P.at(2).gaps));
        ok(P.at(1).spare == 4 && P.at(2).spare == 15, "rows: spares 4 and 15");
    }
    // a full block: the spare is the row's first overflow place, never 21
    {
        using Hyprdrive::Row;
        std::vector<SWorkspace> WS;
        for (ID i = 11; i <= 20; ++i)
            WS.push_back({i, 2, 1});
        WS.push_back({21, 3, 1});
        const auto P = plan({2, 3}, WS, {}, {{2, Row::block(1, 10)}, {3, Row::block(2, 10)}});
        invariants("rows: full block", P, WS);
        ok(P.at(2).spare == Row::block(1, 10).idOf(11), "rows: a full block's spare is its overflow, got " + std::to_string(P.at(2).spare.value_or(-1)));
        ok(P.at(3).spare == 22, "rows: the next block's spare is its own");
        // an overflow desktop with a hole in the block: the hole is filled, the spare follows the overflow
        std::vector<SWorkspace> HOLE;
        for (ID i = 11; i <= 20; ++i)
            if (i != 13)
                HOLE.push_back({i, 2, 1});
        HOLE.push_back({Row::block(1, 10).idOf(11), 2, 1});
        const auto Q = plan({2}, HOLE, {}, {{2, Row::block(1, 10)}});
        invariants("rows: overflow + hole", Q, HOLE);
        ok(Q.at(2).gaps == std::vector<ID>{13}, "rows: the hole under the overflow is filled, got " + ids(Q.at(2).gaps));
        ok(Q.at(2).spare == Row::block(1, 10).idOf(12), "rows: the spare is the next overflow place");
    }
    // a visitor (an Omarchy guest at 3 on block 2's monitor) is not planned around
    {
        using Hyprdrive::Row;
        const std::vector<SWorkspace> WS{{3, 1, 1}, {21, 1, 1}, {23, 1, 1}};
        const auto                    P = plan({1}, WS, {}, {{1, Row::block(2, 10)}});
        ok(P.at(1).gaps == std::vector<ID>{22}, "rows: a visitor is no part of the row, got " + ids(P.at(1).gaps));
        ok(P.at(1).spare == 24, "rows: spare 24");
    }

    std::printf("workspace_plan_test: %d checks, %d failed\n", g_run, g_fail);
    return g_fail ? 1 : 0;
}
