// See DockRoom.hpp.
#include "DockRoom.hpp"

#include "ConfigureHold.hpp"
#include "Dock.hpp"

#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/layout/target/Target.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/state/MonitorState.hpp>

#include <unordered_map>

namespace ScrollOverview::DockRoom {
    namespace {
        std::unordered_map<MONITORID, CBox> g_lastPill;
        uint64_t                            g_relayoutSeq = 0;

        bool overlapsX(const CBox& a, const CBox& b) {
            return a.x < b.x + b.w && a.x + a.w > b.x;
        }
    }

    void place(void* target, const Layout::STargetBox& box, uint8_t flags, PlaceFn original) {
        // Every pass below sends the client a size; it hears only the last one.
        ConfigureHold::CHold oneResize;
        original(target, box, flags);

        auto* const T = sc<Layout::ITarget*>(target);
        const auto  W = T ? T->window() : PHLWINDOW{};
        if (!W || T->floating() || Fullscreen::controller()->isFullscreen(W))
            return;
        const auto MONITOR = W->m_monitor.lock();
        const CBox PILL    = Dock::roomPillGlobal(MONITOR);
        if (PILL.w < 1 || PILL.h < 1)
            return;

        // As far above the pill as the pill is above the screen's edge.
        const double GAP   = std::max(4.0, MONITOR->m_position.y + MONITOR->m_size.y - (PILL.y + PILL.h));
        const double LIMIT = PILL.y - GAP;

        // Twice: trimming the box can change which gaps Hyprland puts at its
        // bottom (screen edge vs. inner), so the first trim can land a few px
        // off; the second corrects it.
        Layout::STargetBox trimmed = box;
        for (int pass = 0; pass < 2; ++pass) {
            const CBox GOAL{W->position(Desktop::View::IGeometric::GEOMETRIC_GOAL), W->size(Desktop::View::IGeometric::GEOMETRIC_GOAL)};
            if (!overlapsX(GOAL, PILL))
                return;
            const double OVER = GOAL.y + GOAL.h - LIMIT;
            if (OVER <= 0.5)
                return;
            if (trimmed.logicalBox.h - OVER < 80.0) // never crush a window to nothing
                return;
            trimmed.logicalBox.h -= OVER;
            if (!trimmed.visualBox.empty())
                trimmed.visualBox.h -= OVER;
            original(target, trimmed, flags);
        }
    }

    void shutdown() {
        if (g_relayoutSeq && g_pEventLoopManager)
            g_pEventLoopManager->removeDoLater(g_relayoutSeq);
        g_relayoutSeq = 0;
        g_lastPill.clear();
    }

    void watch(PHLMONITOR monitor) {
        if (!monitor)
            return;
        const CBox PILL = Dock::roomPillGlobal(monitor);
        auto&      last = g_lastPill[monitor->m_id];
        if (PILL == last)
            return;
        last = PILL;
        if (g_relayoutSeq || !g_pEventLoopManager)
            return;
        // Not from inside the render pass that noticed it.
        g_relayoutSeq = g_pEventLoopManager->doLater([] {
            g_relayoutSeq = 0;
            if (!g_layoutManager)
                return;
            for (const auto& [id, pill] : g_lastPill) {
                (void)pill;
                for (const auto& M : State::monitorState()->monitors())
                    if (M && M->m_id == id)
                        g_layoutManager->recalculateMonitor(M);
            }
        });
    }
}
