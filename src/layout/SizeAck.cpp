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
#include <hyprland/src/desktop/view/window/Window.hpp>
#include "HyprCompat.hpp"
#undef private
#include <hyprland/src/protocols/XDGShell.hpp>
#include <hyprland/src/workspace/HLWorkspace.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/protocols/core/Compositor.hpp>
#include <hyprland/src/render/Texture.hpp>

namespace Hyprdrive::SizeAck {
    std::string report() {
        std::string out = "hyprdrive windows:";
        for (const auto& W : Desktop::windowState()->windows()) {
            if (!W || !W->mapped())
                continue;
            const auto& SZ  = W->sizeAnimation();
            const auto& POS = W->positionAnimation();
            Vector2D    geom;
            if (!W->backend().isX11())
                geom = W->backend().geometry().box.size();
            const size_t ACKS = W->backend().isX11() ? 0 : sc<Desktop::View::CWaylandBackend&>(W->backend()).m_configureAcks.m_pending.size();
            const auto   PENDING = Hyprdrive::Priv::pendingReportedSize(*W).value_or(Vector2D{});
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
                               OFF && !SZ->isBeingAnimated() && !POS->isBeingAnimated() ? "!! " : "", W->metadata().appID(), W->m_workspace ? Hyprdrive::Compat::id(W->m_workspace) : -1, SZ->value().x,
                               SZ->value().y, SZ->goal().x, SZ->goal().y, POS->value().x, POS->value().y, POS->goal().x, POS->goal().y,
                               SZ->isBeingAnimated() || POS->isBeingAnimated() ? " ANIM" : "", geom.x, geom.y, W->backend().reportedSize().x, W->backend().reportedSize().y,
                               PENDING.x, PENDING.y, ACKS, Hyprdrive::Priv::transformers(*W).size(), Hyprdrive::Priv::suspended(*W) ? " SUSP" : "",
                               W->isHidden() ? " HIDDEN" : "", W->isFloating() ? " float" : "", subs);
        }
        return out;
    }
}
