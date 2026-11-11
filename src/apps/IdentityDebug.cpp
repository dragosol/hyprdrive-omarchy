#include "IdentityDebug.hpp"

#include "AppIdentity.hpp"
#include "AppSearch.hpp"

#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/xwayland/XSurface.hpp>

#include <chrono>
#include <format>

namespace Hyprdrive::AppIdentity {
    namespace {
        WindowFacts factsOf(const PHLWINDOW& W) {
            WindowFacts f;
            f.cls          = W->m_class;
            f.initialClass = W->m_initialClass;
            f.title        = W->m_title;
            f.x11          = W->m_isX11;
            f.contentGame  = W->getContentType() == NContentType::CONTENT_TYPE_GAME;
            f.pid          = W->getPID();
            f.hasParent    = W->parent() != nullptr;
            if (W->m_isX11) {
                f.overrideRedirect = W->isX11OverrideRedirect();
                f.hasParent        = f.hasParent || W->x11Parent() != nullptr;
                if (const auto XS = W->m_xwaylandSurface.lock()) {
                    f.xid = XS->m_xID;
                    f.w   = static_cast<int>(XS->m_geometry.width);
                    f.h   = static_cast<int>(XS->m_geometry.height);
                    if (XS->m_sizeHints) {
                        const auto& H = *XS->m_sizeHints;
                        f.fixedSize   = H.min_width > 0 && H.min_height > 0 && H.min_width == H.max_width && H.min_height == H.max_height;
                    }
                }
            }
            return f;
        }

        std::string iconText(const Icon& i) {
            switch (i.kind) {
                case Icon::Kind::None: return "-";
                case Icon::Kind::Theme: return "theme:" + i.value;
                case Icon::Kind::File: return "file:" + i.value;
                case Icon::Kind::Pixels: return std::format("pixels:{}px", i.size);
            }
            return "-";
        }

        std::string age(int64_t builtAtNs) {
            const int64_t NOW = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
            return std::format("{} s ago", (NOW - builtAtNs) / 1'000'000'000LL);
        }
    }

    std::string debugReport() {
        // AppSearch's entries as the resolvers see them (in memory: the dock
        // has scanned them long before anyone asks).
        std::vector<InventoryEntry> inv;
        for (const auto& E : AppSearch::inventory())
            inv.push_back({.id = E.id, .name = E.name, .icon = E.icon, .wmClass = E.wmClass, .exec = E.exec, .mtimeNs = E.mtimeNs});
        const InventoryView VIEW{.entries = inv};
        const Indexes       IX = Indexes::current();

        std::string report = std::format("hyprdrive identity (fast tier: in-memory window fields, no /proc or X properties yet): {} inventory entries; steam index {}; lutris index {}",
                                         inv.size(), IX.steam ? std::format("{} apps, {} shortcuts, built {}", IX.steam->apps.size(), IX.steam->shortcuts.size(), age(IX.steam->builtAtNs)) : "not built yet",
                                         IX.lutris ? std::format("{} games", IX.lutris->games.size()) : "not built yet (the worker is the next part)");
        size_t n = 0;
        for (const auto& W : Desktop::windowState()->windows()) {
            if (!W || !W->m_isMapped)
                continue;
            const WindowFacts F = factsOf(W);
            const Identity    I = resolveFast(F, IX, VIEW);
            report += std::format("\n  0x{:x} {} class={} title=\"{}\" pid={}{}{} -> key={} name=\"{}\" icon={} family={} conf={} by={} group={}{} | {}",
                                  reinterpret_cast<uintptr_t>(W.get()), F.x11 ? "x11" : "wl", F.cls, F.title.substr(0, 60), F.pid, F.xid ? std::format(" xid=0x{:x}", F.xid) : "",
                                  F.overrideRedirect ? " override-redirect" : "", I.key, I.displayName, iconText(I.icon), familyName(I.family), I.confidence, I.resolver,
                                  groupKeyOf(I, F), I.placeable ? "" : " NOT-PLACEABLE", I.evidence);
            ++n;
        }
        report += std::format("\n  {} windows", n);
        return report;
    }
}
