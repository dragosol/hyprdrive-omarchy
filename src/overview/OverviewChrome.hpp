#pragma once

// Overview card chrome: the drop-shadow and corner-rounding helpers, split out
// of OverviewInternal.hpp so the dock can draw a card shadow without pulling the
// ~1900-line everything-header (which includes Dock.hpp -- the include knot the
// dock sat inside). Kept inline in namespace OvInternal so the moved code and
// every call site are byte-identical; OverviewInternal.hpp re-includes this
// BEFORE it opens its own OvInternal block (this header opens the namespace
// itself, so including it inside another OvInternal block would double-nest it).

#include "scrollOverview.hpp"       // Config, PHLMONITOR/PHLLS, CBox, sc<>
#include "OverviewPassElement.hpp"  // COverviewShadowPassElement

#include <hyprland/src/output/Monitor.hpp>                     // CMonitor, m_layerSurfaceLayers
#include <hyprland/src/render/Renderer.hpp>                    // g_pHyprRenderer
#include <hyprland/protocols/wlr-layer-shell-unstable-v1.hpp>  // ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY

#include <algorithm>

namespace OvInternal {

struct SOverviewShadowConfig {
    bool       enabled     = false;
    int        range       = 0;
    int        renderPower = 1;
    Config::CGradientValueData color;
};

// Read once per card AND once per strip cell, every frame. It used to cost
// seven map lookups plus TWO unconditional CGradientValueData copies -- the
// plugin's colour and the compositor's, each a heap allocation, one of which
// was always thrown away. The colours are now looked at through pointers and
// exactly one copy is made, into the struct the shadow pass element takes by
// value; the two compositor ints are resolved to a handle once per call site.
inline SOverviewShadowConfig getOverviewShadowConfig() {
    const auto  enabled     = ScrollOverview::Config::getShadowEnabled();
    const auto  range       = ScrollOverview::Config::getShadowRange();
    const auto  renderPower = ScrollOverview::Config::getShadowRenderPower();
    const auto* color       = ScrollOverview::Config::getShadowColor();

    static auto& globalRangeVal       = ScrollOverview::Config::valueRef<Hyprlang::INT>("decoration:shadow:range");
    static auto& globalRenderPowerVal = ScrollOverview::Config::valueRef<Hyprlang::INT>("decoration:shadow:render_power");
    static auto& globalColorVal       = ScrollOverview::Config::valueRef<::Config::IComplexConfigValue>("decoration:shadow:color");

    const auto globalRange       = sc<int>(*globalRangeVal);
    const auto globalRenderPower = sc<int>(*globalRenderPowerVal);

    Config::CGradientValueData shadowColor;
    if (color && !color->m_colors.empty())
        shadowColor = *color;
    else if (const auto* globalColor = ScrollOverview::Config::gradientPtr(globalColorVal); globalColor && !globalColor->m_colors.empty())
        shadowColor = *globalColor;

    return {
        .enabled      = !!enabled,
        .range        = std::max(0, range >= 0 ? range : globalRange),
        .renderPower  = std::clamp(renderPower >= 0 ? renderPower : globalRenderPower, 1, 4),
        .color        = shadowColor,
    };
}

// E8-c: cards take the theme's corner radius and rounding power rather than a
// hardcoded one, so switching themes moves the overview with it.
struct SOverviewCardRounding {
    int   rounding = 0;
    float power    = 2.F;
};

inline SOverviewCardRounding getOverviewCardRounding(PHLMONITOR monitor, float overviewScale) {
    const auto THEMEROUNDING = ScrollOverview::Config::themeRounding();
    const auto THEMEPOWER    = ScrollOverview::Config::themeRoundingPower();

    const float MONITORSCALE = monitor ? monitor->m_scale : 1.F;

    // If a screen-corners overlay is present (illogical-impulse names it
    // quickshell:screenCorners), cards match that radius. Omarchy has none, so
    // this loop is a no-op and decoration:rounding wins -- including 0, which
    // is Omarchy's squared default.
    if (monitor) {
        for (auto const& lsRef : monitor->m_layerSurfaceLayers[ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY]) {
            const auto LS = lsRef.lock();
            if (!LS || LS->m_namespace != "quickshell:screenCorners")
                continue;

            const int DIM = sc<int>(std::round(std::min(LS->m_geometry.width, LS->m_geometry.height)));
            if (DIM <= 0)
                continue;

            return {
                .rounding = std::max(0, sc<int>(std::round(DIM * MONITORSCALE))),
                .power    = THEMEPOWER,
            };
        }
    }

    const int LOGICAL = std::max(THEMEROUNDING, 0);

    return {
        .rounding = std::max(0, sc<int>(std::round(LOGICAL * MONITORSCALE * overviewScale))),
        .power    = THEMEPOWER,
    };
}

inline void renderOverviewWorkspaceShadow(PHLMONITOR monitor, const CBox& workspaceBox, float overviewScale, bool cutoutCenter, float alpha = 1.F) {
    if (!monitor)
        return;

    const auto SHADOW = getOverviewShadowConfig();
    const bool HASVISIBLECOLOR = std::ranges::any_of(SHADOW.color.m_colors, [](const CHyprColor& color) { return color.a > 0.F; });
    if (!SHADOW.enabled || SHADOW.range <= 0 || !HASVISIBLECOLOR || alpha <= 0.F)
        return;

    const int RANGE = sc<int>(std::round(SHADOW.range * monitor->m_scale * overviewScale));
    if (RANGE <= 0)
        return;

    auto baseBox = workspaceBox.copy().round();
    if (baseBox.width < 1 || baseBox.height < 1)
        return;

    const auto CARDROUND = getOverviewCardRounding(monitor, overviewScale);

    g_pHyprRenderer->addPassElement(makeUnique<COverviewShadowPassElement>(COverviewShadowPassElement::SData{
        .monitor       = monitor,
        .fullBox       = baseBox.copy().expand(RANGE).round(),
        .cutoutBox     = baseBox,
        .rounding      = CARDROUND.rounding,
        .roundingPower = CARDROUND.power,
        .range         = RANGE,
        .renderPower   = SHADOW.renderPower,
        .color         = SHADOW.color,
        .alpha         = alpha,
        .ignoreWindow  = cutoutCenter,
    }));
}

} // namespace OvInternal
