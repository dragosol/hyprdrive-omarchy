#include "DesktopOrder.hpp"
#include "IOverview.hpp"
#include "DesktopPlan.hpp"
#include "DebugPaths.hpp"

#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/state/WorkspaceState.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/managers/EventManager.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/debug/log/Logger.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <format>
#include <string>
#include <unordered_map>

namespace Hyprdrive::DesktopOrder {
    bool ordinary(const PHLWORKSPACE& ws) {
        return ws && !ws->m_isSpecialWorkspace && ws->m_id >= 1;
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
            Log::logger->log(Log::ERR, "[hyprdrive] desktops: {}", line);
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
                if (W && W->m_isMapped && W->m_workspace == ws)
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
                out.push_back({keyOf(WS), WS->m_id, counted ? windowsOn(WS) : 0});
            return out;
        }
        std::string describe(const std::vector<std::pair<PHLWORKSPACE, WORKSPACEID>>& moves) {
            std::string s;
            for (const auto& [WS, TO] : moves)
                s += std::format("{}{}->{}", s.empty() ? "" : " ", WS ? WS->m_id : WORKSPACE_INVALID, TO);
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
            if (WS->m_id != TO)
                done.push_back({WS, WS->m_id, WS->m_name});
        if (done.empty())
            return true;
        for (const auto& [WS, TO] : moves) {
            const WORKSPACEID FROM = WS->m_id;
            if (FROM == TO)
                continue;
            WS->m_id = TO;
            // A numeric name is the number; a name the user gave stays.
            if (WS->m_name == std::to_string(FROM))
                WS->m_name = std::to_string(TO);
        }
        // (Each monitor's "previous workspace" stack holds numbers too, but
        // Hyprland keeps it private: right after a move, "workspace previous"
        // can land on the neighbour of the desktop it meant.)
        // Bars: the old numbers went, the new ones came; then what each monitor shows.
        if (g_pEventManager) {
            for (const auto& D : done) {
                g_pEventManager->postEvent(SHyprIPCEvent{"destroyworkspace", D.oldName});
                g_pEventManager->postEvent(SHyprIPCEvent{"destroyworkspacev2", std::format("{},{}", D.from, D.oldName)});
            }
            for (const auto& D : done) {
                g_pEventManager->postEvent(SHyprIPCEvent{"createworkspace", D.ws->m_name});
                g_pEventManager->postEvent(SHyprIPCEvent{"createworkspacev2", std::format("{},{}", D.ws->m_id, D.ws->m_name)});
            }
            for (const auto& MON : State::monitorState()->monitors())
                if (MON && MON->m_activeWorkspace) {
                    const auto& WS = MON->m_activeWorkspace;
                    g_pEventManager->postEvent(SHyprIPCEvent{"workspace", WS->m_name});
                    g_pEventManager->postEvent(SHyprIPCEvent{"workspacev2", std::format("{},{}", WS->m_id, WS->m_name)});
                }
        }
        overviewDesktopsRenumbered();
        if (g_renumbered)
            g_renumbered();
        return true;
    }

    bool shift(WORKSPACEID from, int delta) {
        std::vector<std::pair<PHLWORKSPACE, WORKSPACEID>> moves;
        for (const auto& WS : desktops())
            if (WS->m_id >= from)
                moves.emplace_back(WS, WS->m_id + delta);
        return renumber(moves);
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
