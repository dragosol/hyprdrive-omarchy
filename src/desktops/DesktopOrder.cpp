#include "DesktopOrder.hpp"
#include "IOverview.hpp"
#include "DesktopPlan.hpp"
#include "DebugPaths.hpp"
#include "Rows.hpp"

#include <hyprland/src/workspace/HLWorkspace.hpp>
#include "HyprCompat.hpp"
#include <hyprland/src/state/WorkspaceState.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/state/workspace/LifecyclePolicyAdapter.hpp>
#include <hyprland/src/desktop/history/WorkspaceHistoryTracker.hpp>
#include <hyprland/src/config/supplementary/propRefresher/PropRefresher.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/ipc/s2/S2.hpp>
#include <hyprland/src/desktop/view/window/Window.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/debug/log/Logger.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <format>
#include <optional>
#include <string>
#include <unordered_map>

namespace Hyprdrive::DesktopOrder {
    bool ordinary(const PHLWORKSPACE& ws) {
        return ws && !Hyprdrive::Compat::isSpecial(ws) && Hyprdrive::Compat::id(ws) >= 1;
    }

    namespace {
        void (*g_renumbered)() = nullptr;

        // Refusals and heals: rare, and the evidence when numbers go wrong. Each
        // distinct line once (a plan refused every frame must not flood RAM-backed
        // runtime dirs).
        void note(const std::string& line) {
            static std::string last;
            if (line == last)
                return;
            last = line;
            Log::logger->log(Log::ERR, Log::logFnName(), "[hyprdrive] desktops: {}", line);
            if (FILE* const F = DebugPaths::openLog("hyprdrive-desktops.log")) {
                std::fprintf(F, "%lld desktops %s\n",
                             (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(),
                             line.c_str());
                std::fclose(F);
            }
        }

        DesktopPlan::Key keyOf(const PHLWORKSPACE& ws) {
            return reinterpret_cast<DesktopPlan::Key>(ws.get());
        }

        int windowsOn(const PHLWORKSPACE& ws) {
            int n = 0;
            for (const auto& W : Desktop::windowState()->windows())
                if (W && W->mapped() && W->m_workspace == ws)
                    ++n;
            return n;
        }

        // Every ordinary desktop, in creation order (Hyprland's list is).
        std::vector<PHLWORKSPACE> desktops() {
            std::vector<PHLWORKSPACE> out;
            for (const auto& REF : State::workspaceState()->workspaces())
                if (const auto WS = REF.lock(); ordinary(WS))
                    out.push_back(WS);
            return out;
        }
        std::vector<DesktopPlan::Desk> desks(const std::vector<PHLWORKSPACE>& all, bool counted) {
            std::vector<DesktopPlan::Desk> out;
            out.reserve(all.size());
            for (const auto& WS : all)
                out.push_back({keyOf(WS), Hyprdrive::Compat::id(WS), counted ? windowsOn(WS) : 0, Rows::ofWorkspace(WS)});
            return out;
        }
        std::string describe(const std::vector<std::pair<PHLWORKSPACE, WORKSPACEID>>& moves) {
            std::string s;
            for (const auto& [WS, TO] : moves)
                s += std::format("{}{}->{}", s.empty() ? "" : " ", WS ? Hyprdrive::Compat::id(WS) : WORKSPACE_INVALID, TO);
            return s;
        }
    }

    void onRenumbered(void (*hook)()) {
        g_renumbered = hook;
    }

    bool renumber(const std::vector<std::pair<PHLWORKSPACE, WORKSPACEID>>& moves) {
        if (moves.empty())
            return true;
        // THE WHOLE PLAN FIRST, against every ordinary desktop: nothing is
        // written unless all of it can be.
        std::vector<DesktopPlan::Move> plan;
        plan.reserve(moves.size());
        for (const auto& [WS, TO] : moves)
            plan.push_back({ordinary(WS) ? keyOf(WS) : 0, TO});
        if (const auto V = DesktopPlan::check(desks(desktops(), false), plan); V != DesktopPlan::Verdict::Ok) {
            note(std::format("refused a renumber that {}: {}", DesktopPlan::why(V), describe(moves)));
            return false;
        }
        struct SDone {
            PHLWORKSPACE ws;
            WORKSPACEID  from;
            std::string  oldName;
        };
        std::vector<SDone> done;
        for (const auto& [WS, TO] : moves)
            if (Hyprdrive::Compat::id(WS) != TO)
                done.push_back({WS, Hyprdrive::Compat::id(WS), WS->displayName()});
        if (done.empty())
            return true;
        // (Omarchy's merged records under the numbers that change are
        // dropped by its own rule around it: Rows::renumbering.)
        Rows::renumbering(moves, [&] {
            for (const auto& [WS, TO] : moves) {
                const WORKSPACEID FROM = Hyprdrive::Compat::id(WS);
                if (FROM == TO)
                    continue;
                const bool NUMERIC = WS->displayName() == std::to_string(FROM);
                if (TO <= 0 || !WS->numberedID())
                    continue;
                // Not CHLWorkspace::changeID: on 0.57 it posts
                // changeworkspaceid>>OLD,NEW per desktop, which Omarchy-style
                // scripts (and the blocks gates) read as a workspace
                // renumbering fight. 0.56 wrote the id in place and the bars
                // got only the destroy/create/workspace burst below; keep that.
                // Hyprland's own bookkeeping from changeID stays (lifecycle
                // policy + history follow the number, ext-workspace renames).
                const auto OLD_IDENTITY = State::Workspace::SWorkspaceIdentity{WS->m_id, WS->addressableName(), WS->type()};
                WS->m_id              = Workspace::SWorkspaceNumberedID{sc<Workspace::WorkspaceIDContainer>(TO)};
                WS->m_addressableName = std::to_string(TO);
                // A numeric name is the number; a name the user gave stays.
                if (!WS->m_wasRenamed || NUMERIC)
                    WS->m_name = std::to_string(TO);
                Config::Supplementary::refresher()->scheduleRefresh(Config::Supplementary::REFRESH_ALL);
                State::Workspace::workspaceIdentityChanged(OLD_IDENTITY, {WS->m_id, WS->addressableName(), WS->type()});
                Desktop::History::workspaceTracker()->workspaceIdentityChanged(WS);
                WS->m_events.idChanged.emit();
            }
        });
        // (Each monitor's "previous workspace" stack holds numbers too, but
        // Hyprland keeps it private: right after a move, "workspace previous"
        // can land on the neighbour of the desktop it meant.)
        // Bars: the old numbers went, the new ones came; then what each monitor shows.
        {
            for (const auto& D : done) {
                Hyprdrive::Compat::postEvent("destroyworkspace", D.oldName);
                Hyprdrive::Compat::postEvent("destroyworkspacev2", std::format("{},{}", D.from, D.oldName));
            }
            for (const auto& D : done) {
                Hyprdrive::Compat::postEvent("createworkspace", D.ws->displayName());
                Hyprdrive::Compat::postEvent("createworkspacev2", std::format("{},{}", Hyprdrive::Compat::id(D.ws), D.ws->displayName()));
            }
            for (const auto& MON : State::monitorState()->monitors())
                if (MON && MON->m_activeWorkspace) {
                    const auto& WS = MON->m_activeWorkspace;
                    Hyprdrive::Compat::postEvent("workspace", WS->displayName());
                    Hyprdrive::Compat::postEvent("workspacev2", std::format("{},{}", Hyprdrive::Compat::id(WS), WS->displayName()));
                }
        }
        overviewDesktopsRenumbered();
        if (g_renumbered)
            g_renumbered();
        return true;
    }

    bool shift(const Row& row, WORKSPACEID from, int delta) {
        const auto ALL = desktops();
        if (row.bounded() && !row.contains(from)) {
            note(std::format("refused a shift from {} by {}: not in the monitor's row (base {})", from, delta, row.base));
            return false;
        }
        std::vector<std::pair<PHLWORKSPACE, WORKSPACEID>> moves;
        for (const auto& M : DesktopPlan::shiftMoves(desks(ALL, false), row, from, delta))
            for (const auto& WS : ALL)
                if (keyOf(WS) == M.key)
                    moves.emplace_back(WS, M.to);
        return renumber(moves);
    }

    bool payBack(const Row& row, WORKSPACEID at, const PHLWORKSPACE& e) {
        const auto                                        ALL = desktops();
        std::vector<std::pair<PHLWORKSPACE, WORKSPACEID>> moves;
        for (const auto& M : DesktopPlan::payMoves(desks(ALL, false), row, at, keyOf(e)))
            for (const auto& WS : ALL)
                if (keyOf(WS) == M.key)
                    moves.emplace_back(WS, M.to);
        return renumber(moves);
    }

    std::optional<std::vector<std::pair<WORKSPACEID, WORKSPACEID>>> compact(const std::string& why, const Row* only) {
        const auto                    ALL = desktops();
        std::vector<DesktopPlan::Room> rooms;
        std::vector<Row>               rowOf;
        rooms.reserve(ALL.size());
        for (const auto& WS : ALL) {
            bool shown = false;
            for (const auto& MON : State::monitorState()->monitors())
                shown |= MON && MON->m_activeWorkspace == WS;
            rooms.push_back({keyOf(WS), Hyprdrive::Compat::id(WS), windowsOn(WS) > 0, shown});
            rowOf.push_back(Rows::ofWorkspace(WS));
        }
        // ONCE PER ROW: each row's members from its first place. Visitors
        // (outside their monitor's row) and provisional rows (a monitor
        // Omarchy has not given a block yet) are left as they are. Blocks
        // off: one row, every desktop -- the plan it always was.
        std::vector<DesktopPlan::Move> PLAN;
        std::vector<bool>              done(ALL.size(), false);
        for (size_t i = 0; i < ALL.size(); ++i) {
            if (done[i] || rowOf[i].provisional || !rowOf[i].contains(rooms[i].id) || (only && !sameRow(rowOf[i], *only)))
                continue;
            std::vector<DesktopPlan::Room> group;
            for (size_t j = i; j < ALL.size(); ++j)
                if (!done[j] && sameRow(rowOf[j], rowOf[i]) && rowOf[i].contains(rooms[j].id)) {
                    done[j] = true;
                    group.push_back(rooms[j]);
                }
            for (const auto& M : DesktopPlan::compactMoves(group, rowOf[i]))
                PLAN.push_back(M);
        }
        if (PLAN.empty())
            return std::nullopt;
        std::vector<std::pair<PHLWORKSPACE, WORKSPACEID>> moves;
        std::vector<std::pair<WORKSPACEID, WORKSPACEID>>  row;
        std::string                                       before, after;
        for (size_t i = 0; i < ALL.size(); ++i) {
            WORKSPACEID to = Hyprdrive::Compat::id(ALL[i]);
            for (const auto& M : PLAN)
                if (M.key == rooms[i].key)
                    to = M.to;
            if (to != Hyprdrive::Compat::id(ALL[i]))
                moves.emplace_back(ALL[i], to);
            if (rooms[i].populated)
                row.emplace_back(Hyprdrive::Compat::id(ALL[i]), to);
        }
        // (Listed in the old order, so before and after read side by side.)
        std::vector<size_t> byOld(ALL.size());
        for (size_t i = 0; i < byOld.size(); ++i)
            byOld[i] = i;
        std::ranges::sort(byOld, [&](size_t a, size_t b) { return Hyprdrive::Compat::id(ALL[a]) < Hyprdrive::Compat::id(ALL[b]); });
        for (const size_t I : byOld) {
            WORKSPACEID to = Hyprdrive::Compat::id(ALL[I]);
            for (const auto& [WS, TO] : moves)
                if (WS == ALL[I])
                    to = TO;
            const char* tag = rooms[I].populated ? "" : rooms[I].shown ? "(shown, empty)" : "(empty)";
            before += std::format("{}{}{}", before.empty() ? "" : " ", Hyprdrive::Compat::id(ALL[I]), tag);
            after += std::format("{}{}{}", after.empty() ? "" : " ", to, tag);
        }
        note(std::format("{}: no empty desktops between: {} -> {}", why, before, after));
        if (!renumber(moves))
            return std::nullopt;
        return row;
    }

    bool heal(const char* when) {
        const auto ALL   = desktops();
        const auto PLAN  = DesktopPlan::healMoves(desks(ALL, true));
        if (PLAN.empty())
            return false;
        std::vector<std::pair<PHLWORKSPACE, WORKSPACEID>> moves;
        for (const auto& M : PLAN)
            for (const auto& WS : ALL)
                if (keyOf(WS) == M.key)
                    moves.emplace_back(WS, M.to);
        note(std::format("{}: two desktops shared a number, the emptier one moves: {}", when, describe(moves)));
        return renumber(moves);
    }
}
