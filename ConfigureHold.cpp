// See ConfigureHold.hpp.
#include "ConfigureHold.hpp"

#include <hyprland/src/desktop/view/Window.hpp>

#include <utility>
#include <vector>

namespace ScrollOverview::ConfigureHold {
    namespace {
        struct SHeld {
            PHLWINDOWREF window;
            bool         force = false;
        };

        int                                                  g_depth = 0;
        std::vector<SHeld>                                   g_held;
        std::function<void(Desktop::View::CWindow*, bool)> g_sender;
    }

    void setSender(std::function<void(Desktop::View::CWindow*, bool)> sender) {
        g_sender = std::move(sender);
    }

    bool intercept(Desktop::View::CWindow* window, bool force) {
        if (g_depth <= 0 || !window)
            return false;

        const auto SELF = window->m_self.lock();
        if (!SELF)
            return false;

        for (auto& held : g_held) {
            if (held.window.lock() == SELF) {
                held.force = held.force || force;
                return true;
            }
        }
        g_held.push_back({SELF, force});
        return true;
    }

    void flush() {
        auto held = std::move(g_held);
        g_held.clear();
        if (!g_sender)
            return;
        for (const auto& h : held)
            if (const auto WINDOW = h.window.lock(); WINDOW && WINDOW->m_isMapped)
                g_sender(WINDOW.get(), h.force);
    }

    CHold::CHold() {
        ++g_depth;
    }

    CHold::~CHold() {
        if (--g_depth > 0)
            return;
        g_depth = 0;
        flush();
    }
}
