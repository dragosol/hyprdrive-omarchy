// The spring-loaded dock: a client file drag held over an icon.
#include "DockParts.hpp"

namespace ScrollOverview::Dock {
using namespace DockDetail;
namespace DockDetail {

    float springSinceMs(const std::optional<Time::steady_tp>& t) {
        return t ? std::chrono::duration<float, std::milli>(Time::steadyNow() - *t).count() : 0.F;
    }

    void springDamage() {
        for (const auto& MON : State::monitorState()->monitors())
            if (MON && MON->m_id == g_spring.monitor)
                g_pHyprRenderer->damageMonitor(MON);
    }
}

void springReset() {
    if (!g_spring.key.empty())
        springDamage();
    g_spring = {};
}

bool springUpdate(PHLMONITOR monitor, const Vector2D& local, bool folderDrag, int delayMs) {
    std::string  key, subject;
    PHLWINDOWREF target;

    if (monitor && !folderDrag) {
        const auto LAYOUT = layout(monitor);
        if (pillContains(monitor, local)) {
            const auto ITEM = std::ranges::find_if(LAYOUT.items, [&](const SItem& item) { return hoverBox(item.iconBox, monitor->m_scale).containsPoint(local); });
            if (ITEM != LAYOUT.items.end() && !ITEM->stack && ITEM->uri.empty() && !ITEM->placeholder && !ITEM->scratchpad && ITEM->running) {
                if (const auto WINDOW = recentWindow(itemWindows(*ITEM)); WINDOW) {
                    key     = "app:" + ITEM->subject;
                    subject = ITEM->subject;
                    target  = WINDOW;
                }
            }
        } else if (const auto CELL = panelCellAt(monitor, local); CELL && CELL->second) {
            key    = "win:" + std::to_string(rc<uintptr_t>(CELL->second.get()));
            target = CELL->second;
            if (g_hover.panel) {
                // The preview's own app icon flashes: the panel belongs to it.
                for (const auto& ITEM : LAYOUT.items)
                    if (ITEM.wmClass == g_hover.panel->wmClass && ITEM.desktopId == g_hover.panel->desktopId)
                        subject = ITEM.subject;
            }
        }
    }

    if (key.empty()) {
        springReset();
        return false;
    }
    if (key != g_spring.key) {
        springReset();
        g_spring.key     = key;
        g_spring.subject = subject;
        g_spring.window  = target;
        g_spring.monitor = monitor->m_id;
        g_spring.since   = Time::steadyNow();
        springDamage(); // the armed ring
        return true;
    }
    if (g_spring.done)
        return false;

    if (!g_spring.flashAt) {
        if (springSinceMs(g_spring.since) >= delayMs) {
            g_spring.flashAt = Time::steadyNow();
            springDamage();
        }
        return true;
    }
    springDamage();
    if (springSinceMs(g_spring.flashAt) < SPRING_FLASH_MS)
        return true;

    g_spring.done = true;
    if (const auto WINDOW = g_spring.window.lock(); WINDOW && WINDOW->m_isMapped)
        activateWindow(WINDOW);
    springDamage();
    return false;
}

// 0..1: how far into a flash pulse this icon is (two pulses), 0 = not flashing.
float springFlash(const SItem& item) {
    if (!g_spring.flashAt || g_spring.done || item.subject.empty() || item.subject != g_spring.subject)
        return 0.F;
    const float T = springSinceMs(g_spring.flashAt) / SPRING_FLASH_MS; // 0..1 over both pulses
    if (T >= 1.F)
        return 0.F;
    return std::sin(T * 2.F * sc<float>(M_PI)) * std::sin(T * 2.F * sc<float>(M_PI)); // two humps
}

// The icon a held drag is waiting on (the ring, before the flash).
bool springArmed(const SItem& item) {
    return !g_spring.done && !g_spring.flashAt && !item.subject.empty() && item.subject == g_spring.subject;
}

bool springAnimating() {
    return !g_spring.key.empty() && !g_spring.done;
}
}
