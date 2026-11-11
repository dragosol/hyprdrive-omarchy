#include "Rows.hpp"
#include "RowsFile.hpp"
#include "DesktopOrder.hpp"

#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/state/WorkspaceState.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/debug/log/Logger.hpp>

#include <algorithm>
#include <cstdlib>
#include <format>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <sys/inotify.h>
#include <unistd.h>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

namespace Hyprdrive::Rows {
    namespace {
        constexpr const char* BLOCKS_FILE = "monitor-workspaces";
        constexpr const char* MERGED_FILE = "monitor-workspaces-merged";

        Row::Id                          g_slots = 0; // from Lua; 0 = blocks off
        std::map<std::string, Row::Id>   g_blocks;    // connector -> block (Omarchy's file)
        std::set<WORKSPACEID>            g_merged;    // the guests' ids
        bool                             g_blocksRead = false, g_mergedRead = false;
        int                              g_inotify = -1, g_wdDir = -1, g_wdParent = -1;
        wl_event_source*                 g_source = nullptr;
        wl_event_source*                 g_roomTimer = nullptr;
        bool                             g_live = false;
        CHyprSignalListener              g_addHook, g_goneHook, g_reloadHook, g_preReloadHook, g_removedHook;

        std::string stateHome() {
            if (const char* S = std::getenv("XDG_STATE_HOME"); S && *S)
                return S;
            const char* H = std::getenv("HOME");
            return std::string{H ? H : ""} + "/.local/state";
        }
        std::string dir() {
            return stateHome() + "/omarchy";
        }

        std::optional<std::string> slurp(const std::string& path) {
            std::ifstream f(path, std::ios::binary);
            if (!f)
                return std::nullopt;
            std::ostringstream s;
            s << f.rdbuf();
            if (s.str().size() > 64 * 1024)
                return std::nullopt; // not Omarchy's few lines
            return s.str();
        }

        // Each file on its own: a bad parse keeps what was there.
        void reread() {
            if (const auto T = slurp(dir() + "/" + BLOCKS_FILE))
                if (auto m = RowsFile::blocks(*T)) {
                    g_blocks     = std::move(*m);
                    g_blocksRead = true;
                }
            if (const auto T = slurp(dir() + "/" + MERGED_FILE); T) {
                if (auto m = RowsFile::merged(*T)) {
                    g_merged.clear();
                    for (const auto ID : *m)
                        g_merged.insert(ID);
                    g_mergedRead = true;
                }
            } else {
                g_merged.clear(); // no file: no guests (Omarchy removes it when empty)
            }
        }

        // The omarchy dir when it exists, else its parent until it does.
        void watch() {
            if (g_inotify < 0 || g_wdDir >= 0)
                return;
            g_wdDir = inotify_add_watch(g_inotify, dir().c_str(), IN_CLOSE_WRITE | IN_MOVED_TO | IN_DELETE | IN_DELETE_SELF | IN_MOVE_SELF);
            if (g_wdDir >= 0) {
                if (g_wdParent >= 0) {
                    inotify_rm_watch(g_inotify, g_wdParent);
                    g_wdParent = -1;
                }
                return;
            }
            if (g_wdParent < 0)
                g_wdParent = inotify_add_watch(g_inotify, stateHome().c_str(), IN_CREATE | IN_MOVED_TO);
        }

        int onInotify(int, uint32_t, void*) {
            alignas(struct inotify_event) char buf[4096];
            bool changed = false, rewatch = false;
            for (;;) {
                const ssize_t N = ::read(g_inotify, buf, sizeof(buf));
                if (N <= 0)
                    break;
                for (ssize_t off = 0; off < N;) {
                    const auto*            EV   = reinterpret_cast<const struct inotify_event*>(buf + off);
                    const std::string_view NAME = EV->len ? std::string_view{EV->name} : std::string_view{};
                    if (EV->wd == g_wdDir) {
                        if (EV->mask & (IN_DELETE_SELF | IN_MOVE_SELF | IN_IGNORED)) {
                            g_wdDir = -1;
                            rewatch = true;
                        } else if (NAME == BLOCKS_FILE || NAME == MERGED_FILE)
                            changed = true;
                    } else if (EV->wd == g_wdParent && NAME == "omarchy")
                        rewatch = changed = true;
                    off += sizeof(struct inotify_event) + EV->len;
                }
            }
            if (rewatch)
                watch();
            if (changed)
                reread();
            return 0;
        }

        Row blockRow(Row::Id block, bool provisional) {
            return Row::block(block, g_slots, provisional);
        }

        // The block an id is in (an overflow id: its row's block).
        Row::Id blockOfId(WORKSPACEID id) {
            if (id > Row::OVERFLOW_ORIGIN)
                return (id - Row::OVERFLOW_ORIGIN - 1) / Row::OVERFLOW_SPAN;
            return (id - 1) / g_slots;
        }

        bool ordinary(const PHLWORKSPACE& ws) {
            return DesktopOrder::ordinary(ws);
        }

        bool exists(WORKSPACEID id) {
            for (const auto& REF : State::workspaceState()->workspaces())
                if (const auto WS = REF.lock(); ordinary(WS) && WS->m_id == id)
                    return true;
            return false;
        }

        // OVERFLOW FLOWS BACK FIRST: a row with an overflow desktop and a free
        // place in its block is compacted (after the usual settle) -- by us,
        // in order, before Omarchy's next reconcile would merge it there
        // itself (append_slot) and leave a merged record behind.
        int checkRoom(void*) {
            if (!g_pCompositor || g_pCompositor->m_isShuttingDown || !blocks())
                return 0;
            for (const auto& MON : State::monitorState()->monitors()) {
                if (!MON)
                    continue;
                const Row ROW = of(MON);
                if (!ROW.bounded() || ROW.provisional)
                    continue;
                bool overflow = false;
                for (const auto& WS : members(ROW))
                    overflow |= ROW.inOverflow(WS->m_id) && !pinned(WS->m_id);
                if (!overflow)
                    continue;
                for (Row::Id s = 1; s <= ROW.capacity; ++s)
                    if (!exists(ROW.idOf(s))) {
                        return 0; // (compaction: wired with the per-row compaction)
                    }
            }
            return 0;
        }
    }

    bool blocks() {
        return g_slots > 0;
    }

    Row of(const PHLMONITOR& monitor) {
        if (!blocks() || !monitor)
            return {};
        if (const auto IT = g_blocks.find(monitor->m_name); IT != g_blocks.end())
            return blockRow(IT->second, false);
        // Not in Omarchy's file yet: the bar's fallback, the block of what it shows.
        const auto SHOWN = monitor->m_activeWorkspace;
        if (SHOWN && ordinary(SHOWN))
            return blockRow(std::clamp<Row::Id>(blockOfId(SHOWN->m_id), 0, 99), true);
        return blockRow(0, true);
    }

    Row ofWorkspace(const PHLWORKSPACE& ws) {
        return ws ? of(ws->m_monitor.lock()) : Row{};
    }

    Row containing(WORKSPACEID id) {
        if (!blocks())
            return {};
        for (const auto& MON : State::monitorState()->monitors())
            if (MON)
                if (const Row R = of(MON); R.contains(id))
                    return R;
        return {};
    }

    std::string label(const PHLWORKSPACE& ws) {
        if (!ws)
            return {};
        const Row R = ofWorkspace(ws);
        if (R.contains(ws->m_id))
            return std::to_string(R.slot(ws->m_id));
        return std::to_string(RowsFile::slotOf(ws->m_id, g_slots));
    }

    std::vector<PHLWORKSPACE> members(const Row& row) {
        std::vector<PHLWORKSPACE> out;
        for (const auto& REF : State::workspaceState()->workspaces())
            if (const auto WS = REF.lock(); ordinary(WS) && row.contains(WS->m_id) && sameRow(ofWorkspace(WS), row))
                out.push_back(WS);
        std::ranges::sort(out, [&](const PHLWORKSPACE& a, const PHLWORKSPACE& b) { return row.slot(a->m_id) < row.slot(b->m_id); });
        return out;
    }

    WORKSPACEID firstEmpty(const PHLMONITOR& monitor) {
        const Row R = of(monitor);
        for (Row::Id s = 1; s <= R.lastSlot(); ++s) {
            const WORKSPACEID ID = R.idOf(s);
            PHLWORKSPACE      found;
            for (const auto& REF : State::workspaceState()->workspaces())
                if (const auto WS = REF.lock(); WS && !WS->m_isSpecialWorkspace && WS->m_id == ID)
                    found = WS;
            if (!found || (found->m_monitor.lock() == monitor && found->getWindowCount() == 0))
                return ID;
        }
        return WORKSPACE_INVALID;
    }

    bool pinned(WORKSPACEID id) {
        return blocks() && g_merged.contains(id);
    }

    int luaWorkspaceBlocks(lua_State* L) {
        long long n = 0;
        if (lua_gettop(L) >= 1 && lua_isnumber(L, 1))
            n = sc<long long>(lua_tointeger(L, 1));
        n = std::clamp<long long>(n, 0, 100);
        if (n != g_slots) {
            Log::logger->log(Log::INFO, "[hyprdrive] rows: workspace blocks {} -> {}", g_slots, n);
            g_slots = n;
            if (g_live) {
                watch();
                reread();
            }
        }
        return 0;
    }

    void init() {
        g_live = true;
        if (g_inotify < 0)
            g_inotify = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (g_inotify >= 0 && g_pCompositor && g_pCompositor->m_wlEventLoop && !g_source)
            g_source = wl_event_loop_add_fd(g_pCompositor->m_wlEventLoop, g_inotify, WL_EVENT_READABLE, onInotify, nullptr);
        if (g_pCompositor && g_pCompositor->m_wlEventLoop && !g_roomTimer)
            g_roomTimer = wl_event_loop_add_timer(g_pCompositor->m_wlEventLoop, checkRoom, nullptr);
        watch();
        reread();
        const auto AGAIN = [] {
            if (!g_pCompositor || g_pCompositor->m_isShuttingDown)
                return;
            watch();
            reread();
        };
        g_addHook  = Event::bus()->m_events.monitor.added.listen([AGAIN](PHLMONITOR) { AGAIN(); });
        g_goneHook = Event::bus()->m_events.monitor.removed.listen([AGAIN](PHLMONITOR) { AGAIN(); });
        // A publisher line gone from the config turns blocks off at the next
        // parse: each parse says it again (00-plugin.lua).
        g_preReloadHook = Event::bus()->m_events.config.preReload.listen([] { g_slots = 0; });
        // A config switch is where Omarchy reconciles: the empty desktops it
        // left go once the burst settles, as after a display change
        // (FullscreenDesktop's compaction, per row).
        g_reloadHook = Event::bus()->m_events.config.reloaded.listen([AGAIN] {
            AGAIN();
        });
        g_removedHook = Event::bus()->m_events.workspace.removed.listen([](PHLWORKSPACEREF) {
            if (g_roomTimer && blocks())
                wl_event_source_timer_update(g_roomTimer, 1); // not inside the removal
        });
    }

    void shutdown() {
        g_live = false;
        g_addHook.reset();
        g_goneHook.reset();
        g_reloadHook.reset();
        g_preReloadHook.reset();
        g_removedHook.reset();
        if (g_roomTimer) {
            wl_event_source_remove(g_roomTimer);
            g_roomTimer = nullptr;
        }
        if (g_source) {
            wl_event_source_remove(g_source);
            g_source = nullptr;
        }
        if (g_inotify >= 0) {
            ::close(g_inotify);
            g_inotify = -1;
        }
        g_wdDir = g_wdParent = -1;
        g_blocks.clear();
        g_merged.clear();
        g_blocksRead = g_mergedRead = false;
        g_slots = 0;
    }

    std::string debugReport() {
        std::string s = std::format("hyprdrive rows: blocks {} (slots {}), file {}/{} {}, watch {}", blocks() ? "on" : "off", g_slots, dir(), BLOCKS_FILE,
                                    g_blocksRead ? "read" : "not read", g_wdDir >= 0 ? "dir" : g_wdParent >= 0 ? "parent" : "none");
        s += "\n  file:";
        for (const auto& [CONN, B] : g_blocks)
            s += std::format(" {}={}", CONN, B);
        s += "\n  pinned (merged):";
        for (const auto ID : g_merged)
            s += std::format(" {}", ID);
        for (const auto& MON : State::monitorState()->monitors()) {
            if (!MON)
                continue;
            const Row R = of(MON);
            s += std::format("\n  {}: row base {} capacity {} overflow {}{}", MON->m_name, R.base, R.capacity, R.overflow, R.provisional ? " PROVISIONAL" : "");
            s += "\n    members:";
            for (const auto& WS : members(R))
                s += std::format(" {}={}{}{}", WS->m_id, label(WS), WS->getWindowCount() ? "" : "(empty)", pinned(WS->m_id) ? "(pinned)" : "");
            s += "\n    visitors:";
            for (const auto& REF : State::workspaceState()->workspaces())
                if (const auto WS = REF.lock(); ordinary(WS) && WS->m_monitor.lock() == MON && !R.contains(WS->m_id))
                    s += std::format(" {}={}", WS->m_id, label(WS));
            s += std::format("\n    first empty: {}", firstEmpty(MON));
        }
        return s;
    }
}
