#include "OverviewPassElement.hpp"
#include <algorithm>
#include <any>
#include <array>
#include <chrono>
#include <cmath>
#include <functional>
#include <helpers/Color.hpp>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#define private public
#define protected public
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/helpers/math/Math.hpp>
#undef protected
#undef private
#include <hyprutils/utils/ScopeGuard.hpp>
#include "Config.hpp"
#include "OverviewRender.hpp"
#include "IOverview.hpp"

static CRegion roundedRectRegion(const CBox& box, int rounding, float roundingPower) {
    const auto ROUNDEDBOX = box.copy().round();
    const int  x          = sc<int>(ROUNDEDBOX.x);
    const int  y          = sc<int>(ROUNDEDBOX.y);
    const int  w          = sc<int>(ROUNDEDBOX.width);
    const int  h          = sc<int>(ROUNDEDBOX.height);

    if (w <= 0 || h <= 0)
        return {};

    const int radius = std::clamp(rounding, 0, std::min(w, h) / 2);
    if (radius <= 0)
        return CRegion{ROUNDEDBOX};

    CRegion    region;
    const auto power = std::max(roundingPower, 0.1F);

    auto insetForRow = [radius, h, power](int row) {
        const double rowCenter = row + 0.5;
        double       dy        = 0.0;

        if (rowCenter < radius)
            dy = radius - rowCenter;
        else if (rowCenter > h - radius)
            dy = rowCenter - (h - radius);

        if (dy <= 0.0)
            return 0;

        const double radiusPow = std::pow(radius, power);
        const double inner     = std::pow(std::max(0.0, radiusPow - std::pow(dy, power)), 1.0 / power);
        return std::clamp(sc<int>(std::ceil(radius - inner)), 0, radius);
    };

    int runStart = 0;
    int runInset = insetForRow(0);

    auto addRun = [&](int from, int to, int inset) {
        if (to <= from)
            return;

        const int width = w - inset * 2;
        if (width <= 0)
            return;

        region.add(CBox{x + inset, y + from, width, to - from});
    };

    for (int row = 1; row < h; ++row) {
        const int inset = insetForRow(row);
        if (inset == runInset)
            continue;

        addRun(runStart, row, runInset);
        runStart = row;
        runInset = inset;
    }

    addRun(runStart, h, runInset);

    return region;
}

COverviewBlurPassElement::COverviewBlurPassElement(const SData& data_) : data(data_) {
    ;
}

std::vector<UP<IPassElement>> COverviewBlurPassElement::draw() {
    if (data.box.width < 1 || data.box.height < 1 || data.alpha <= 0.F)
        return {};

    const auto MONITOR = g_pHyprRenderer->m_renderData.pMonitor;
    if (!MONITOR)
        return {};

    // X29. Blur shader reads global decoration:blur:*; per-surface overrides ride
    // on the element and restore after (dock keys, -1 = leave global alone).
    const auto PREVSIZE       = ScrollOverview::Config::getValue<int>("decoration:blur:size");
    const auto PREVPASSES     = ScrollOverview::Config::getValue<int>("decoration:blur:passes");
    const auto PREVBRIGHTNESS = ScrollOverview::Config::getValue<float>("decoration:blur:brightness");
    const auto PREVNOISE      = ScrollOverview::Config::getValue<float>("decoration:blur:noise");
    const auto PREVCONTRAST   = ScrollOverview::Config::getValue<float>("decoration:blur:contrast");
    const auto PREVVIBRANCY   = ScrollOverview::Config::getValue<float>("decoration:blur:vibrancy");

    auto       restoreBlur = Hyprutils::Utils::CScopeGuard([PREVSIZE, PREVPASSES, PREVBRIGHTNESS, PREVNOISE, PREVCONTRAST, PREVVIBRANCY] {
        ScrollOverview::Config::setValue("decoration:blur:size", PREVSIZE);
        ScrollOverview::Config::setValue("decoration:blur:passes", PREVPASSES);
        ScrollOverview::Config::setValue("decoration:blur:brightness", PREVBRIGHTNESS);
        ScrollOverview::Config::setValue("decoration:blur:noise", PREVNOISE);
        ScrollOverview::Config::setValue("decoration:blur:contrast", PREVCONTRAST);
        ScrollOverview::Config::setValue("decoration:blur:vibrancy", PREVVIBRANCY);
    });

    if (data.blurSize > 0)
        ScrollOverview::Config::setValue("decoration:blur:size", data.blurSize);
    if (data.blurPasses > 0)
        ScrollOverview::Config::setValue("decoration:blur:passes", data.blurPasses);
    if (data.blurBrightness >= 0.F)
        ScrollOverview::Config::setValue("decoration:blur:brightness", data.blurBrightness);
    if (data.blurNoise >= 0.F)
        ScrollOverview::Config::setValue("decoration:blur:noise", data.blurNoise);
    if (data.blurContrast >= 0.F)
        ScrollOverview::Config::setValue("decoration:blur:contrast", data.blurContrast);
    if (data.blurVibrancy >= 0.F)
        ScrollOverview::Config::setValue("decoration:blur:vibrancy", data.blurVibrancy);

    // X96. Always full-monitor dual-kawase at decoration:blur:* with a=1, then
    // UV-sample into the rounded panel. Thin damage + native renderRect blur
    // (X95) under-delivers the pass pyramid on a strip/dock/pill vs large window
    // surfaces even when size/passes match on paper. Fade is data.alpha on the
    // sample only — never the kawase radius.
    //
    // xray=true  → private chrome FB (X92 capture: full mon, a=1).
    // xray=false → live blurMainFramebuffer(1, full mon) right now.
    const Vector2D MONSIZE = MONITOR->m_transformedSize;
    if (MONSIZE.x < 1.0 || MONSIZE.y < 1.0)
        return {};

    // Prefer the private full-mon chrome FB (one dual-kawase for many samplers).
    // Always fall back to live blurMainFramebuffer(a=1, full mon) so a missing
    // capture (or wallpaper-blur-only config) never paints tint-without-frost.
    //
    // Y17 (GPU, 2026-09-14): the xray=false path below is a FULL-MONITOR live
    // kawase per element -- search pill, dock pill, hover panel, menu, stacks
    // popup and the SUPER-hold pill each pay one. Two fixes were considered and
    // both rejected:
    //   (a) restrict the blur to data.box plus a margin -- same X96 hazard as
    //       the chrome capture; see the long note over
    //       OverviewRender::captureFrameChromeBlur for why damage-restricted
    //       kawase cannot be shown pixel-identical by reading Hyprland's code.
    //   (b) blur once per frame and share it between the elements -- they are
    //       drawn at different points of the pass and each is meant to frost
    //       whatever is UNDER IT AT THAT MOMENT, which is precisely what a
    //       shared capture would destroy (that is what xray=true already is,
    //       and why these are xray=false in the first place).
    // Left alone deliberately.
    SP<Render::ITexture> blurred;
    if (data.xray)
        blurred = OverviewRender::frameChromeBlur();
    if (!blurred || !blurred->ok()) {
        CRegion fullDamage{CBox{0, 0, MONSIZE.x, MONSIZE.y}};
        blurred = g_pHyprRenderer->blurMainFramebuffer(1.F, &fullDamage);
    }

    if (!blurred || !blurred->ok())
        return {};

    const CBox BOX = data.box;
    CRegion    paintDamage = roundedRectRegion(BOX, data.rounding, data.roundingPower);
    if (paintDamage.empty())
        paintDamage = CRegion{BOX.copy().round()};

    g_pHyprRenderer->pushMonitorTransformEnabled(true);
    auto restoreXform = Hyprutils::Utils::CScopeGuard([] { g_pHyprRenderer->popMonitorTransformEnabled(); });

    Render::GL::g_pHyprOpenGL->renderTexture(
        blurred, BOX,
        Render::GL::CHyprOpenGLImpl::STextureRenderData{
            .damage                      = &paintDamage,
            .a                           = data.alpha,
            .round                       = data.rounding,
            .roundingPower               = data.roundingPower,
            .allowCustomUV               = true,
            .allowDim                    = false,
            .primarySurfaceUVTopLeft     = BOX.pos() / MONSIZE,
            .primarySurfaceUVBottomRight = (BOX.pos() + BOX.size()) / MONSIZE,
        });
    return {};
}


bool COverviewBlurPassElement::needsLiveBlur() {
    return !data.xray && data.alpha > 0.F;
}

bool COverviewBlurPassElement::needsPrecomputeBlur() {
    // X88: we sample OverviewRender::frameChromeBlur(), not m_blurFB. Advertising
    // needsPrecomputeBlur would only make Hyprland maintain the desktop xray FB.
    return false;
}

std::optional<CBox> COverviewBlurPassElement::boundingBox() {
    const auto MONITOR = g_pHyprRenderer->m_renderData.pMonitor;
    if (!MONITOR)
        return std::nullopt;

    return data.box.copy().scale(1.F / MONITOR->m_scale).round();
}

CRegion COverviewBlurPassElement::opaqueRegion() {
    return {};
}

COverviewSplitBorderPassElement::COverviewSplitBorderPassElement(const SData& data_) : data(data_) {
    ;
}

std::vector<UP<IPassElement>> COverviewSplitBorderPassElement::draw() {
    if (data.box.width < 1 || data.box.height < 1 || data.borderSize < 1 || data.a <= 0.F)
        return {};

    auto stroke = [this](int round) {
        Render::GL::g_pHyprOpenGL->renderBorder(data.box, data.color,
                                                {
                                                    .round         = round,
                                                    .roundingPower = data.roundingPower,
                                                    .borderSize    = data.borderSize,
                                                    .a             = data.a,
                                                });
    };

    if (data.roundLeft == data.roundRight) {
        stroke(data.roundLeft);
        return {};
    }

    // The two halves meet on a straight edge far from every corner, so the top
    // and bottom strokes abut exactly and there is nothing to seam.
    //
    // Splitting by intersecting m_renderData.damage was tried first -- the idiom
    // the shadow element below uses -- and drew NOTHING at all: the inner cell's
    // single-pass stroke measured 182 luminance while the split cell's whole span
    // sat at the fill's 94. clipBox is what the cell FILL splits with two screens
    // up, and it is the knob the scissor actually reads.
    const auto SAVEDCLIP = g_pHyprRenderer->m_renderData.clipBox;
    auto       restore   = Hyprutils::Utils::CScopeGuard([SAVEDCLIP] { g_pHyprRenderer->m_renderData.clipBox = SAVEDCLIP; });

    const double MID = std::round(data.box.x + data.box.width / 2.0);

    for (int half = 0; half < 2; ++half) {
        const bool   LEFT = half == 0;
        const double B    = data.borderSize;

        // The border grows OUTWARD from the box, so the clip has to grow with it
        // or the stroke is shaved off its own edge -- but NOT at the seam, where
        // an overlap would blend the translucent stroke over itself.
        CBox clip = LEFT ? CBox{data.box.x - B, data.box.y - B, MID - data.box.x + B, data.box.height + 2 * B} :
                           CBox{MID, data.box.y - B, data.box.x + data.box.width - MID + B, data.box.height + 2 * B};

        if (SAVEDCLIP.width > 0 && SAVEDCLIP.height > 0)
            clip = clip.intersection(SAVEDCLIP);

        if (clip.width < 1 || clip.height < 1)
            continue;

        g_pHyprRenderer->m_renderData.clipBox = clip;
        stroke(LEFT ? data.roundLeft : data.roundRight);
    }

    return {};
}

bool COverviewSplitBorderPassElement::needsLiveBlur() {
    return false;
}

bool COverviewSplitBorderPassElement::needsPrecomputeBlur() {
    return false;
}

std::optional<CBox> COverviewSplitBorderPassElement::boundingBox() {
    const auto MONITOR = g_pHyprRenderer->m_renderData.pMonitor;
    if (!MONITOR)
        return std::nullopt;

    return data.box.copy().expand(data.borderSize).scale(1.F / MONITOR->m_scale).round();
}

CRegion COverviewSplitBorderPassElement::opaqueRegion() {
    return {};
}

COverviewShadowPassElement::COverviewShadowPassElement(const SData& data_) : data(data_) {
    ;
}

// Y17 (GPU, 2026-09-14): skipping the shadow at a range below ~2px was
// considered and NOT taken. renderRoundedShadow at range 1 still paints a
// one-pixel gradient ring, and on the 2x eDP that is a real pixel, so "range <
// 2 is invisible" is an assumption rather than a fact -- and the rule for this
// pass is that the output does not move. The `range <= 0` guard below is the
// only one that is provably free.
std::vector<UP<IPassElement>> COverviewShadowPassElement::draw() {
    const auto MONITOR = data.monitor.lock();
    const bool HASVISIBLECOLOR = std::ranges::any_of(data.color.m_colors, [](const CHyprColor& color) { return color.a > 0.F; });
    if (!MONITOR || data.fullBox.width < 1 || data.fullBox.height < 1 || data.range <= 0 || !HASVISIBLECOLOR || data.alpha <= 0.F)
        return {};

    CRegion shadowDamage = g_pHyprRenderer->m_renderData.damage.copy().intersect(data.fullBox);
    if (data.ignoreWindow)
        shadowDamage.subtract(roundedRectRegion(data.cutoutBox, data.rounding + 1, data.roundingPower));

    if (shadowDamage.empty())
        return {};

    const auto SAVEDDAMAGE       = g_pHyprRenderer->m_renderData.damage;
    const auto SAVEDCURRENTWINDOW = g_pHyprRenderer->m_renderData.currentWindow;
    g_pHyprRenderer->m_renderData.damage = shadowDamage;
    g_pHyprRenderer->m_renderData.currentWindow.reset();
    auto restoreRenderData = Hyprutils::Utils::CScopeGuard([SAVEDDAMAGE, SAVEDCURRENTWINDOW] {
        g_pHyprRenderer->m_renderData.damage        = SAVEDDAMAGE;
        g_pHyprRenderer->m_renderData.currentWindow = SAVEDCURRENTWINDOW;
    });

    std::optional<int> previousRenderPower;
    if (data.renderPower > 0) {
        previousRenderPower = ScrollOverview::Config::getValue<int>("decoration:shadow:render_power");
        ScrollOverview::Config::setValue("decoration:shadow:render_power", data.renderPower);
    }

    auto restoreRenderPower = Hyprutils::Utils::CScopeGuard([previousRenderPower] {
        if (!previousRenderPower)
            return;

        ScrollOverview::Config::setValue("decoration:shadow:render_power", *previousRenderPower);
    });

    Render::GL::g_pHyprOpenGL->renderRoundedShadow(data.fullBox, data.rounding, data.roundingPower, data.range, data.color, std::clamp(data.alpha, 0.F, 1.F));

    return {};
}

bool COverviewShadowPassElement::needsLiveBlur() {
    return false;
}

bool COverviewShadowPassElement::needsPrecomputeBlur() {
    return false;
}

std::optional<CBox> COverviewShadowPassElement::boundingBox() {
    const auto MONITOR = data.monitor.lock();
    if (!MONITOR)
        return std::nullopt;

    return data.fullBox.copy().scale(1.F / MONITOR->m_scale).round();
}

CRegion COverviewShadowPassElement::opaqueRegion() {
    return {};
}

CDockLivePreviewPassElement::CDockLivePreviewPassElement(SData&& data_) : data(std::move(data_)) {
    ;
}

std::vector<UP<IPassElement>> CDockLivePreviewPassElement::draw() {
    if (!data.pass)
        return {};

    const auto MONITOR = g_pHyprRenderer->m_renderData.pMonitor.lock();
    if (!MONITOR)
        return {};

    // EVERYTHING CRenderPass::render() WRITES ON ITS WAY OUT, not just what we
    // set going in. This draws a WHOLE PASS from inside one element of the pass
    // that is currently rendering, and the nested render ends by publishing its
    // own state as if it were the frame's: finalDamage becomes the nested
    // pass's region, damage becomes its damage, and -- the one that shows --
    // m_blurFBShouldRender is set from the NESTED pass's element list, which
    // holds one window and never precomputes blur. The outer frame had already
    // decided it needed that framebuffer, so clearing the flag mid-pass left
    // every window behind the dock drawing its blur from a framebuffer nobody
    // rendered: chunks of the real window going missing for exactly the frames
    // a preview was live (user, 2026-09-15: "the actual window is cutting out
    // when the preview is rendering").
    const auto SAVEDCLIP   = g_pHyprRenderer->m_renderData.clipBox;
    const auto SAVEDMODIF  = g_pHyprRenderer->m_renderData.renderModif;
    const auto SAVEDDAMAGE = g_pHyprRenderer->m_renderData.damage;
    const auto SAVEDFINAL  = g_pHyprRenderer->m_renderData.finalDamage;
    const bool SAVEDBLURFB = MONITOR->m_blurFBShouldRender;
    auto       restore     = Hyprutils::Utils::CScopeGuard([SAVEDCLIP, SAVEDMODIF, SAVEDDAMAGE, SAVEDFINAL, SAVEDBLURFB, MONITOR] {
        g_pHyprRenderer->m_renderData.clipBox     = SAVEDCLIP;
        g_pHyprRenderer->m_renderData.renderModif = SAVEDMODIF;
        g_pHyprRenderer->m_renderData.damage      = SAVEDDAMAGE;
        g_pHyprRenderer->m_renderData.finalDamage = SAVEDFINAL;
        MONITOR->m_blurFBShouldRender             = SAVEDBLURFB;
    });

    if (data.box.width >= 1 && data.box.height >= 1)
        g_pHyprRenderer->m_renderData.clipBox = data.box;

    data.pass->render(CRegion{CBox{{}, MONITOR->m_transformedSize}});
    return {};
}

bool CDockLivePreviewPassElement::needsLiveBlur() {
    return false;
}

bool CDockLivePreviewPassElement::needsPrecomputeBlur() {
    return false;
}

std::optional<CBox> CDockLivePreviewPassElement::boundingBox() {
    const auto MONITOR = g_pHyprRenderer->m_renderData.pMonitor.lock();
    if (!MONITOR)
        return std::nullopt;

    return data.box.copy().scale(1.F / MONITOR->m_scale).round();
}

CRegion CDockLivePreviewPassElement::opaqueRegion() {
    return {};
}

bool CDockLivePreviewPassElement::disableSimplification() {
    return true;
}

bool CDockLivePreviewPassElement::undiscardable() {
    return true;
}
