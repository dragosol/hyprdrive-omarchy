// See SizeAck.hpp.
#include "SizeAck.hpp"

#include <algorithm>
#include <any>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <format>
#include <ranges>
#include <sstream>
#include <vector>

// report() only: m_suspended is private (Hyprland has no getter); the std
// headers above stay clean.
#define private public
#include <hyprland/src/desktop/view/Window.hpp>
#undef private
#include <hyprland/src/protocols/XDGShell.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/protocols/core/Compositor.hpp>
#include <hyprland/src/render/Texture.hpp>

namespace ScrollOverview::SizeAck {
    void onAck(Desktop::View::CWindow* window, uint32_t serial) {
        auto&      acks = window->m_pendingSizeAcks;
        const auto IT   = std::ranges::find_if(acks | std::views::reverse, [serial](const auto& e) { return e.first <= serial; });
        if (IT == acks.rend())
            return;

        // The fix: a copy, not a reference into the vector erase_if compacts.
        const auto ACKED = *IT;
        window->m_pendingSizeAck = ACKED;
        std::erase_if(acks, [&ACKED](const auto& e) { return e.first <= ACKED.first; });

        if (window->m_isX11)
            return;
        const auto RES = window->resource();
        if (!RES)
            return;
        // As Hyprland does: the size applies with the commit that follows.
        RES->m_pending.ackedSize          = ACKED.second;
        RES->m_pending.updated.bits.acked = true;
        window->m_pendingSizeAck.reset();
    }

    std::string report() {
        std::string out = "hyprdrive windows:";
        for (const auto& W : Desktop::windowState()->windows()) {
            if (!W || !W->m_isMapped)
                continue;
            const auto& SZ  = W->sizeAnimation();
            const auto& POS = W->positionAnimation();
            Vector2D    geom;
            if (const auto XDG = W->m_xdgSurface.lock())
                geom = XDG->m_current.geometry.size();
            // Every surface of the window, breadth first: GTK4 may draw its
            // content into a subsurface.
            std::string subs;
            if (const auto RES = W->resource())
                RES->breadthfirst(
                    [&subs](SP<CWLSurfaceResource> surf, const Vector2D& off, void*) {
                        if (!surf)
                            return;
                        const auto& C = surf->m_current;
                        subs += std::format(" [{:.0f},{:.0f} {:.0f}x{:.0f} buf {:.0f}x{:.0f} tex {:.0f}x{:.0f} acked {:.0f}x{:.0f}]", off.x, off.y, C.size.x, C.size.y,
                                            C.bufferSize.x, C.bufferSize.y, C.texture ? C.texture->m_size.x : 0.0, C.texture ? C.texture->m_size.y : 0.0, C.ackedSize.x,
                                            C.ackedSize.y);
                    },
                    nullptr);
            const bool OFF = SZ->value() != SZ->goal() || POS->value() != POS->goal();
            out += std::format("\n {}{:.24} ws={} size {:.0f}x{:.0f}/goal {:.0f}x{:.0f} pos {:.0f},{:.0f}/goal {:.0f},{:.0f}{} geom {:.0f}x{:.0f} reported {:.0f}x{:.0f} "
                               "pending {:.0f}x{:.0f} unacked {} xf={}{}{}{} surfaces{}",
                               OFF && !SZ->isBeingAnimated() && !POS->isBeingAnimated() ? "!! " : "", W->m_class, W->m_workspace ? W->m_workspace->m_id : -1, SZ->value().x,
                               SZ->value().y, SZ->goal().x, SZ->goal().y, POS->value().x, POS->value().y, POS->goal().x, POS->goal().y,
                               SZ->isBeingAnimated() || POS->isBeingAnimated() ? " ANIM" : "", geom.x, geom.y, W->m_reportedSize.x, W->m_reportedSize.y,
                               W->m_pendingReportedSize.x, W->m_pendingReportedSize.y, W->m_pendingSizeAcks.size(), W->m_transformers.size(), W->m_suspended ? " SUSP" : "",
                               W->isHidden() ? " HIDDEN" : "", W->m_isFloating ? " float" : "", subs);
        }
        return out;
    }
}
