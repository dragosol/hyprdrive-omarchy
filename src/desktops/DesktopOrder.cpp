#include "DesktopOrder.hpp"
#include "IOverview.hpp"

#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/state/WorkspaceState.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/managers/EventManager.hpp>

#include <algorithm>
#include <format>
#include <string>
#include <unordered_map>

namespace Hyprdrive::DesktopOrder {
    bool ordinary(const PHLWORKSPACE& ws) {
        return ws && !ws->m_isSpecialWorkspace && ws->m_id >= 1;
    }

    void renumber(const std::vector<std::pair<PHLWORKSPACE, WORKSPACEID>>& moves) {
        if (moves.empty())
            return;
        std::unordered_map<WORKSPACEID, WORKSPACEID> map;
        struct SDone {
            PHLWORKSPACE ws;
            WORKSPACEID  from;
            std::string  oldName;
        };
        std::vector<SDone> done;
        for (const auto& [WS, TO] : moves) {
            if (!ordinary(WS) || TO < 1 || WS->m_id == TO)
                continue;
            map[WS->m_id] = TO;
            done.push_back({WS, WS->m_id, WS->m_name});
        }
        for (auto& D : done) {
            const WORKSPACEID TO = map.at(D.from);
            D.ws->m_id           = TO;
            // A numeric name is the number; a name the user gave stays.
            if (D.ws->m_name == std::to_string(D.from))
                D.ws->m_name = std::to_string(TO);
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
    }

    void shift(WORKSPACEID from, int delta) {
        std::vector<std::pair<PHLWORKSPACE, WORKSPACEID>> moves;
        for (const auto& REF : State::workspaceState()->workspaces()) {
            const auto WS = REF.lock();
            if (ordinary(WS) && WS->m_id >= from)
                moves.emplace_back(WS, WS->m_id + delta);
        }
        renumber(moves);
    }
}
