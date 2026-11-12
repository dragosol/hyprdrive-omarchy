// See ConfigureHold.hpp.
#include "ConfigureHold.hpp"

#include <hyprland/src/desktop/view/Window.hpp>

#include <algorithm>
#include <utility>
#include <vector>

namespace Hyprdrive::ConfigureHold {
    namespace {
        struct SHeld {
            PHLWINDOWREF window;
            bool         force = false;
        };

        int                                                  g_depth = 0;
        std::vector<SHeld>                                   g_held;
        std::function<void(Desktop::View::CWindow*, bool)> g_sender;
        std::function<bool(const PHLWINDOW&)>              g_gate;
        std::vector<SHeld>                                   g_gated; // held by the gate, not by a CHold
        bool note(std::vector<SHeld>& list, const PHLWINDOW& w, bool force) {
            for (auto& held : list)
                if (held.window.lock() == w) {
                    held.force = held.force || force;
                    return true;
                }
            list.push_back({w, force});
            return true;
        }
    }

    void setSender(std::function<void(Desktop::View::CWindow*, bool)> sender) {
        g_sender = std::move(sender);
    }

    bool intercept(Desktop::View::CWindow* window, bool force) {
        if (!window)
            return false;
        const auto SELF = window->m_self.lock();
        if (!SELF)
            return false;
        if (g_gate && g_gate(SELF))
            return note(g_gated, SELF, force);
        if (g_depth <= 0)
            return false;
        return note(g_held, SELF, force);
    }
    void setGate(std::function<bool(const PHLWINDOW&)> gate) {
        g_gate = std::move(gate);
        if (!g_gate)
            release();
    }
    void release() {
        std::vector<SHeld> due;
        std::erase_if(g_gated, [&](const SHeld& h) {
            const auto W = h.window.lock();
            if (!W)
                return true;
            if (g_gate && g_gate(W))
                return false;
            due.push_back(h);
            return true;
        });
        if (!g_sender)
            return;
        for (const auto& h : due)
            if (const auto WINDOW = h.window.lock(); WINDOW && WINDOW->m_isMapped) {
                if (g_depth > 0)
                    note(g_held, WINDOW, h.force); // a CHold is up: its end sends it
                else
                    g_sender(WINDOW.get(), h.force);
            }
    }
    namespace {
        void sendHeld(bool all) {
            auto held = std::move(g_held);
            g_held.clear();
            if (all) {
                // The gate's code may be unloaded next: nothing stays gated either.
                g_gate = nullptr;
                for (auto& h : g_gated)
                    held.push_back(h);
                g_gated.clear();
            }
            if (!g_sender)
                return;
            for (const auto& h : held)
                if (const auto WINDOW = h.window.lock(); WINDOW && WINDOW->m_isMapped)
                    g_sender(WINDOW.get(), h.force);
        }
    }
    void flush() {
        sendHeld(true);
    }
    bool held() {
        return g_depth > 0;
    }
    bool waiting(const PHLWINDOW& w) {
        const auto IS = [&](const SHeld& h) { return h.window.lock() == w; };
        return w && (std::ranges::any_of(g_gated, IS) || std::ranges::any_of(g_held, IS));
    }

    CHold::CHold() {
        ++g_depth;
    }

    CHold::~CHold() {
        if (--g_depth > 0)
            return;
        g_depth = 0;
        sendHeld(false);
    }
}
