// The hover-preview panel and live previews, the frosted queue they share,
// pointer focus over the dock and the retile tracker.
#include "DockParts.hpp"

namespace Hyprdrive::Dock {
using namespace DockDetail;
namespace DockDetail {

// The app's display name, over a dark rounded plate, centred on the icon and
// sitting above the pill.
// ponytail: no delay and no fade -- it appears with the highlight it belongs to.
// GNOME staggers it by ~500ms; add that if sweeping the dock reads as noisy.
// X31. The dot says RUNNING, and as of X32 that is a real distinction rather
// than a tautology: a pinned favourite with no window is an icon with no dot,
// and the dot is the only thing on the pill that tells the two apart. The
// caller gates on SItem::running.
//
// It hangs off the ICON, DOT_MARGIN_B below it -- NOT off the pill's bottom
// edge, which is where the first cut anchored it. Measured on the host: icon
// bottom 176, dot 178-182, so the dot's own bottom lands flush with the bottom
// of the hover highlight. Anchoring to the pill put it ITEM_PAD_BOTTOM -
// HOVER_PAD = 8px too low, and that gap is what read as "not 1:1".

// X87. Dock frost is LIVE (xray=false), desktop and overview, and only when
// Hyprland window blur is on. One surface, so it does not reintroduce the
// multi-card compound bug that forced shared sampling for strip/search.
// decoration:blur:* still applies when the dock keys are -1 (X71). Sampling
// m_blurFB here was wrong for the desktop path (no behind-chrome pre-blur
// exists) and broke once the shared draw path was rewritten.
void queueDockFrost(PHLMONITOR monitor, const CBox& box, int rounding, float alpha, const CBox& clip = {}, const CRegion* shape = nullptr, bool currentPass = false,
                           float roundingPower = 2.F) {
    if (!Stacks::drawableBox(box) || alpha <= 0.F) // box.empty() lets a NEGATIVE box through
        return;

    // Same switch window frost and overview chrome capture use. Dock keys of
    // -1 already mean "follow decoration:blur:*"; with the theme blur off a
    // frost pass would still run and look like a dock-only effect.
    if (!Hyprdrive::Config::getValue<bool>("decoration:blur:enabled"))
        return;

    COverviewBlurPassElement::SData blur;
    blur.box            = box;
    blur.rounding       = rounding;
    blur.roundingPower  = roundingPower;
    blur.alpha          = alpha;
    blur.xray           = false; // live dual-kawase of whatever is behind the pill
    blur.clip           = clip;
    if (shape)
        blur.shape = shape->copy();
    // -1 on these keys means leave decoration:blur:* alone (X71).
    blur.blurSize       = Config::getValue<int>(KEY_BLUR_SIZE);
    blur.blurPasses     = Config::getValue<int>(KEY_BLUR_PASSES);
    blur.blurBrightness = Config::getValue<float>(KEY_BLUR_BRIGHT);
    blur.blurNoise      = Config::getValue<float>(KEY_BLUR_NOISE);
    blur.blurContrast   = Config::getValue<float>(KEY_BLUR_CONTR);
    blur.blurVibrancy   = Config::getValue<float>(KEY_BLUR_VIBR);
    if (currentPass)
        g_pHyprRenderer->addPassElement(makeUnique<COverviewBlurPassElement>(blur));
    else
        g_pHyprRenderer->m_renderPass.add(makeUnique<COverviewBlurPassElement>(blur));
}

// REAL milliseconds, deliberately NOT the dock's elapsedMs. That one divides by
// plugin:scrolloverview:anim_scale, which is right for every animation in this
// file and wrong here: this is a staleness window on a frame-callback policy,
// not a curve. At anim_scale 10 it would hold entries for 2.5s, and at 0.05 it
// would expire them in 12ms and starve the previews it exists to keep alive.
float realElapsedMs(const Time::steady_tp& since) {
    return sc<float>(std::chrono::duration<float, std::milli>(Time::steadyNow() - since).count());
}

void notePreviewing(const PHLWINDOW& window) {
    const auto NOW = Time::steadyNow();
    std::erase_if(g_previewed, [&](const auto& e) { return !e.first.lock() || realElapsedMs(e.second) >= PREVIEWING_MS; });

    for (auto& [REF, AT] : g_previewed) {
        if (REF.lock() == window) {
            AT = NOW;
            return;
        }
    }

    g_previewed.emplace_back(PHLWINDOWREF{window}, NOW);
}

void renderLivePreview(PHLMONITOR monitor, const PHLWINDOW& window, const CBox& box, const Time::steady_tp& now, float alpha = 1.F, bool asOverviewTile = false,
                       bool ownsRenderPass = true, int roundingPx = -1) {
    // BEFORE notePreviewing, which is what refreshes the entry -- asking after
    // it would always answer "healthy". See Membership::previewShadowEnabled.
    if (Membership::previewShadowEnabled()) {
        static std::unordered_map<const void*, Time::steady_tp> LASTDRAWN;
        const auto IT   = LASTDRAWN.find(window.get());
        const float GAP = IT == LASTDRAWN.end() ? -1.F : realElapsedMs(IT->second);
        Membership::previewGap(window->m_class, GAP, !isPreviewing(window), window->m_suspended);
        LASTDRAWN[window.get()] = Time::steadyNow();
        if (LASTDRAWN.size() > 64)
            LASTDRAWN.clear();
    }

    notePreviewing(window);

    const bool STASHED = Minimize::isMinimized(window);
    const bool OFF_ACTIVE =
        !window->m_workspace || !monitor || window->m_workspace != monitor->m_activeWorkspace;
    // Up on ANOTHER monitor right now: that monitor draws it and paces its frames.
    // Forcing it as well unhid group members, undid fullscreen hiding and its
    // workspace fades there, and a second presentation feedback on this
    // monitor's clock stole the client's frame timing from the one it is really
    // on (user, 2026-09-14: previews "flaky" for apps on the other monitor).
    const auto WINDOWMONITOR = window->m_monitor.lock();
    const bool SHOWN_ELSEWHERE = !STASHED && WINDOWMONITOR && WINDOWMONITOR != monitor && window->m_workspace && !window->isHidden() &&
        (window->m_workspace == WINDOWMONITOR->m_activeWorkspace || window->m_workspace == WINDOWMONITOR->m_activeSpecialWorkspace);
    // Minimized always; otherwise any window not on this monitor's active ws,
    // unless another monitor is already showing it.
    const bool NEED_FORCE = !SHOWN_ELSEWHERE && (STASHED || OFF_ACTIVE);

    bool       wasWsVisible = false;
    bool       wasWsForce   = false;
    PHLWORKSPACE ws;
    if (NEED_FORCE) {
        forcePreviewVisible(window);
        ws = window->m_workspace;
        if (ws) {
            wasWsVisible         = ws->m_visible;
            wasWsForce           = ws->m_forceRendering;
            ws->m_visible        = true;
            ws->m_forceRendering = true;
        }
    }

    const auto  SIZE        = window->m_realSize->value();
    const float RENDERSCALE = SIZE.y >= 1.0 ? sc<float>(box.height / (SIZE.y * monitor->m_scale)) : 1.F;

    OverviewWindow::renderOverviewWindow({
        .monitor                 = monitor,
        .window                  = window,
        .windowBox               = box,
        .renderScale             = RENDERSCALE,
        .now                     = now,
        // X90: dock THUMBNAILS must not touch decoration:blur:size / m_blurFB.
        // The flight is not a thumbnail and takes the overview path instead, so
        // its blur scales and stays live like every other real window.
        .scaleBlurToRenderScale  = asOverviewTile,
        // X91: do not rewrite real window geometry (ghost mini outline).
        .dockLivePreview         = !asOverviewTile,
        .alpha                   = alpha,
        .ownsRenderPass          = ownsRenderPass,
        .previewRoundingPx       = roundingPx,
    });

    if (ws) {
        ws->m_visible        = wasWsVisible;
        ws->m_forceRendering = wasWsForce;
    }

    // Frame callbacks for anything that would otherwise freeze (hidden ws).
    if (NEED_FORCE)
        presentPreview(window, monitor, now);

    // A window living on another monitor also needs THAT monitor to present: a
    // fifo client's next commit is held until an output it is on does, and an
    // idle monitor never would.
    //
    // JUST THE WINDOW'S OWN BOX. This asked for the whole output, and it asks
    // once per frame for as long as the preview is up -- so previewing one
    // window of the other screen kept that entire screen, every window on it and
    // every blur behind them, repainting at its refresh rate (user,
    // 2026-09-14). Releasing a held commit needs a frame on that output, not a
    // full redraw of it, and the window's own rectangle is a frame.
    if (WINDOWMONITOR && WINDOWMONITOR != monitor && !asOverviewTile) {
        const double WSCALE = std::max(WINDOWMONITOR->m_scale, 0.01F);
        const auto   POS    = window->m_realPosition->value();
        const auto   WSIZE  = window->m_realSize->value();

        CBox         local{(POS.x - WINDOWMONITOR->m_position.x) * WSCALE, (POS.y - WINDOWMONITOR->m_position.y) * WSCALE, WSIZE.x * WSCALE, WSIZE.y * WSCALE};
        // A window's decorations and shadow live outside m_realSize; the pump
        // only has to produce a frame, but damaging a little wide costs nothing
        // and keeps a border from going stale if one ever rides this path.
        local.expand(32.0 * WSCALE);

        pumpFrame(WINDOWMONITOR, local);
    }
}

// Entrances decelerate, exits accelerate (the house rule), from wherever the
// fade was when it turned.
static float panelAlpha(const SPanel& panel) {
    const float T = std::clamp(elapsedMs(panel.fadeSince) / std::max(1.F, panel.fadeMs), 0.F, 1.F);
    const float E = houseEase(panel.fadeTo > panel.fadeFrom, T);
    return panel.fadeFrom + ((panel.fadeTo - panel.fadeFrom) * E);
}

static void panelFade(SPanel& panel, float to, float ms, const Time::steady_tp& now) {
    panel.fadeFrom  = panelAlpha(panel);
    panel.fadeTo    = to;
    panel.fadeSince = now;
    panel.fadeMs    = ms;
}

// [FLIGHT] dismissHoverPanelNow moved out of the anonymous namespace -- the
// flight module calls it across a TU boundary now -- and is defined below,
// where this anonymous block closes. g_hover / g_tip stay here.


void forgetPanelGeometry() {
    g_hover.drawn = CBox{};
    g_hover.cells.clear();
}

// Y1. Inside the plate that is actually on screen -- and not while it is
// leaving, or a dismissal already under way would be resurrected by a pointer
// that merely happens to be where the shrinking plate still is.
//
// `point` is in `monitor`'s pixels, and the panel lives on one monitor only: the
// same pixel on the other one is not inside it.
bool panelOn(const PHLMONITOR& monitor) {
    return monitor && g_hover.panel && g_hover.panel->monitor == monitor->m_id;
}

bool panelContains(const PHLMONITOR& monitor, const Vector2D& point) {
    return panelOn(monitor) && !g_hover.panel->out && g_hover.drawn.width >= 1 && g_hover.drawn.height >= 1 && g_hover.drawn.containsPoint(point);
}

// Y2. The preview cell under the pointer and the window in it. The BOX comes
// back with the window because the title plate is centred on the cell, not on
// the icon the panel belongs to.
std::optional<std::pair<CBox, PHLWINDOW>> panelCellAt(const PHLMONITOR& monitor, const Vector2D& point) {
    if (!panelOn(monitor))
        return std::nullopt;

    for (const auto& [BOX, REF] : g_hover.cells) {
        if (!BOX.containsPoint(point))
            continue;

        if (const auto WINDOW = REF.lock(); WINDOW)
            return std::make_pair(BOX, WINDOW);
    }

    return std::nullopt;
}

bool sameBox(const CBox& a, const CBox& b) {
    return sc<int>(a.x) == sc<int>(b.x) && sc<int>(a.y) == sc<int>(b.y) && sc<int>(a.width) == sc<int>(b.width) && sc<int>(a.height) == sc<int>(b.height);
}

// The windows an icon previews. itemWindows already includes minimized ones,
// which is what makes a minimized window reachable from its own icon.
std::vector<PHLWINDOW> panelWindows(const std::string& wmClass, const std::string& desktopId = {}, const std::string& scratchpadName = {}) {
    std::vector<PHLWINDOW> out;

    // The scratchpad's slot previews a WORKSPACE. Its windows have nothing in
    // common but where they are parked, so there is no class to ask for them.
    if (!scratchpadName.empty())
        out = Scratchpad::windowsOn(Scratchpad::byShortName(scratchpadName));
    else if (!wmClass.empty() || !desktopId.empty()) {
        SItem item;
        item.wmClass   = wmClass;
        item.desktopId = desktopId;
        out            = itemWindows(item);
    } else
        return {};

    std::erase_if(out, [](const auto& window) { return !window || !window->m_isMapped || !window->wlSurface() || !window->wlSurface()->resource(); });
    if (out.size() > PANEL_MAX)
        out.resize(PANEL_MAX);

    return out;
}
SPanelRows panelRows(PHLMONITOR monitor, const std::vector<PHLWINDOW>& windows) {
    const float        SCALE = std::max(monitor->m_scale, 0.01F);
    const float        GAP   = PANEL_CELL_GAP * SCALE;
    float              cellH = PANEL_CELL_H * SCALE;
    std::vector<float> widths;
    widths.reserve(windows.size());
    for (const auto& W : windows)
        widths.push_back(cellH * previewAspect(W));
    SPanelRows out;
    out.counts = Hyprdrive::PreviewGrid::rows(widths, GAP, PANEL_ROW_SHARE * sc<float>(monitor->m_size.x) * SCALE);
    // Too many rows for the screen: every cell shrinks alike.
    const float ROWS  = sc<float>(out.counts.size());
    const float MAXH  = PANEL_MAX_H_SHARE * sc<float>(monitor->m_size.y) * SCALE - 2.F * PANEL_PAD * SCALE - GAP * std::max(0.F, ROWS - 1.F);
    const float SHRINK = ROWS > 0.F ? std::clamp(MAXH / (ROWS * cellH), 0.1F, 1.F) : 1.F;
    cellH *= SHRINK;
    for (auto& w : widths)
        w *= SHRINK;
    out.cellH = cellH;
    out.width = Hyprdrive::PreviewGrid::widest(widths, out.counts, GAP);
    return out;
}

// One cell per window, each keeping its window's aspect, in rows past half
// the screen's width (panelRows).
CBox panelTarget(PHLMONITOR monitor, const SLayout& layout, const std::vector<PHLWINDOW>& windows, const CBox& anchor) {
    const float SCALE = std::max(monitor->m_scale, 0.01F);
    const auto  ROWS  = panelRows(monitor, windows);
    const float NROWS = sc<float>(std::max<size_t>(1, ROWS.counts.size()));

    const float WIDTH  = std::round(ROWS.width + 2.F * PANEL_PAD * SCALE);
    const float HEIGHT = std::round(NROWS * ROWS.cellH + (NROWS - 1.F) * PANEL_CELL_GAP * SCALE + 2.F * PANEL_PAD * SCALE);
    const float CX     = sc<float>(anchor.x) + sc<float>(anchor.width) / 2.F;

    // Clamped to the monitor: an icon near either end would otherwise push half
    // the panel off screen, which is where a taskbar preview stops being one.
    const float MARGIN = PANEL_PAD * SCALE;
    const float MAXX   = sc<float>(monitor->m_size.x) * SCALE - WIDTH - MARGIN;
    const float X      = std::clamp(CX - WIDTH / 2.F, MARGIN, std::max(MARGIN, MAXX));

    return CBox{std::round(X), std::round(sc<float>(layout.pill.y) - PANEL_GAP * SCALE - HEIGHT), WIDTH, HEIGHT};
}

// A quarter of Hyprland's corner: |x|^p + |y|^p = r^p about (cx, cy), the
// shape its rounding shader cuts with decoration:rounding_power p (2 = a
// circle). From angle a0 to a1 either way round, as cairo_arc /
// cairo_arc_negative would, so the outline is concentric with the previews'
// own corners whatever the power.
static void cornerArc(cairo_t* cr, double cx, double cy, double r, double a0, double a1, double p) {
    if (r <= 0.0) {
        cairo_line_to(cr, cx, cy);
        return;
    }
    const double E     = 2.0 / std::max(p, 0.1);
    const auto   CURVE = [E](double v) { return std::copysign(std::pow(std::abs(v), E), v); };
    constexpr int SEGMENTS = 32;
    for (int i = 0; i <= SEGMENTS; ++i) {
        const double T = a0 + ((a1 - a0) * i / SEGMENTS);
        cairo_line_to(cr, cx + (r * CURVE(std::cos(T))), cy + (r * CURVE(std::sin(T))));
    }
}

static void organicPath(cairo_t* cr, const std::vector<SPanelBand>& b, double R, double P, double ox, double oy) {
    const size_t N   = b.size();
    const auto   W   = [&](size_t k) { return b[k].x1 - b[k].x0; };
    const auto   H   = [&](size_t k) { return b[k].y1 - b[k].y0; };
    const auto   CVX = [&](size_t k) { return std::max(0.0, std::min({R, W(k) / 2.0, H(k) / 2.0})); };
    const double PI  = 3.14159265358979323846;
    // A step of `d` px between rows k and k+1: convex corner c, concave curve f.
    // Both are the plate's own radius (user, 2026-10-16: "the inverted curve
    // ... should always be the same radius as the regular curve padding"),
    // except where the step has no runway for both: the outer corner keeps R
    // (it is concentric with the preview under it) and the inverted curve takes
    // what is left. renderHoverPreview merges rows whose step cannot hold the
    // outer corner at all.
    const auto STEP = [&](size_t k, double d, double& f, double& c) {
        const double ROOM = std::min(H(k), H(k + 1)) / 2.0;
        c                 = std::max(0.0, std::min({R, d, ROOM}));
        f                 = std::max(0.0, std::min({R, d - c, ROOM}));
    };

    // top edge, left to right
    const double R0 = CVX(0);
    cairo_move_to(cr, b[0].x0 + R0 - ox, b[0].y0 - oy);
    cornerArc(cr, b[0].x1 - R0 - ox, b[0].y0 + R0 - oy, R0, -PI / 2, 0, P);
    // right side, top to bottom
    for (size_t k = 0; k + 1 < N; ++k) {
        const double Y = b[k].y1 - oy, X = b[k].x1 - ox, NX = b[k + 1].x1 - ox;
        double       f = 0, c = 0;
        if (NX > X + 0.5) { // the next row is wider: fillet out, then its corner
            STEP(k, NX - X, f, c);
            cornerArc(cr, X + f, Y - f, f, PI, PI / 2, P);
            cornerArc(cr, NX - c, Y + c, c, -PI / 2, 0, P);
        } else if (NX < X - 0.5) { // narrower: this row's corner, then fillet in
            STEP(k, X - NX, f, c);
            cornerArc(cr, X - c, Y - c, c, 0, PI / 2, P);
            cornerArc(cr, NX + f, Y + f, f, 3 * PI / 2, PI, P);
        } else
            cairo_line_to(cr, X, Y);
    }
    // bottom edge, right to left
    const double RL = CVX(N - 1);
    cornerArc(cr, b[N - 1].x1 - RL - ox, b[N - 1].y1 - RL - oy, RL, 0, PI / 2, P);
    cornerArc(cr, b[N - 1].x0 + RL - ox, b[N - 1].y1 - RL - oy, RL, PI / 2, PI, P);
    // left side, bottom to top
    for (size_t k = N - 1; k > 0; --k) {
        const double Y = b[k].y0 - oy, X = b[k].x0 - ox, PX = b[k - 1].x0 - ox;
        double       f = 0, c = 0;
        if (PX < X - 0.5) { // the row above is wider: fillet out, then its corner
            STEP(k - 1, X - PX, f, c);
            cornerArc(cr, X - f, Y + f, f, 0, -PI / 2, P);
            cornerArc(cr, PX + c, Y - c, c, PI / 2, PI, P);
        } else if (PX > X + 0.5) { // narrower: this row's corner, then fillet in
            STEP(k - 1, PX - X, f, c);
            cornerArc(cr, X + c, Y + c, c, PI, 3 * PI / 2, P);
            cornerArc(cr, PX - f, Y - f, f, PI / 2, 0, P);
        } else
            cairo_line_to(cr, X, Y);
    }
    cornerArc(cr, b[0].x0 + R0 - ox, b[0].y0 + R0 - oy, R0, PI, 3 * PI / 2, P);
    cairo_close_path(cr);
}

// Draws the plate: frost through the outline's region, then the tint texture.
static void organicPlate(PHLMONITOR monitor, const std::vector<SPanelBand>& bands, double R, double power, float alpha) {
    if (bands.empty() || alpha <= 0.F)
        return;
    double x0 = bands[0].x0, x1 = bands[0].x1;
    for (const auto& B : bands) {
        x0 = std::min(x0, B.x0);
        x1 = std::max(x1, B.x1);
    }
    const double y0 = bands.front().y0, y1 = bands.back().y1;
    const int    W = sc<int>(std::ceil(x1 - x0)), H = sc<int>(std::ceil(y1 - y0));
    if (W < 2 || H < 2)
        return;
    const auto TINT = background();

    std::string key = std::format("{:.0f}|{:.2f}{:.3f}{:.3f}{:.3f}", R, power, TINT.r, TINT.g, TINT.b);
    for (const auto& B : bands)
        key += std::format("|{:.0f},{:.0f},{:.0f},{:.0f}", B.x0, B.x1, B.y0, B.y1);

    if (g_organicPlate.key != key || !g_organicPlate.tex) {
        cairo_surface_t* const SURF = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, W, H);
        cairo_t* const         CR   = cairo_create(SURF);
        cairo_set_antialias(CR, CAIRO_ANTIALIAS_BEST);
        organicPath(CR, bands, R, power, x0, y0);
        cairo_set_source_rgba(CR, TINT.r, TINT.g, TINT.b, 1.0);
        cairo_fill(CR);
        cairo_surface_flush(SURF);

        // The frost's region: the outline's pixels, one rect per run of rows
        // that cover the same span.
        CRegion            region;
        const auto*        DATA   = cairo_image_surface_get_data(SURF);
        const int          STRIDE = cairo_image_surface_get_stride(SURF);
        int                runY = 0, runA = -1, runB = -1;
        const auto FLUSH = [&](int yEnd) {
            if (runA >= 0 && runB > runA && yEnd > runY)
                region.add(CBox{x0 + runA, y0 + runY, sc<double>(runB - runA), sc<double>(yEnd - runY)});
        };
        for (int y = 0; y < H; ++y) {
            const auto* ROW = reinterpret_cast<const uint32_t*>(DATA + (sc<ptrdiff_t>(y) * STRIDE));
            int         a = 0, bEnd = W;
            while (a < W && (ROW[a] >> 24) < 128)
                ++a;
            while (bEnd > a && (ROW[bEnd - 1] >> 24) < 128)
                --bEnd;
            if (a >= bEnd)
                a = bEnd = -1;
            if (a != runA || bEnd != runB) {
                FLUSH(y);
                runY = y, runA = a, runB = bEnd;
            }
        }
        FLUSH(H);

        g_organicPlate.tex    = makeShared<Render::GL::CGLTexture>(DRM_FORMAT_ARGB8888, cairo_image_surface_get_data(SURF), sc<uint32_t>(STRIDE),
                                                                   Vector2D{sc<double>(W), sc<double>(H)});
        g_organicPlate.region = std::move(region);
        g_organicPlate.box    = CBox{x0, y0, sc<double>(W), sc<double>(H)};
        g_organicPlate.key    = key;
        cairo_destroy(CR);
        cairo_surface_destroy(SURF);
    }

    // X13/X29/X86: the same frost as every dock plate, painted through the outline.
    // Frost and tint both go to the CURRENT pass, the one the previews go to
    // (user, 2026-10-16, a recording: while the overview opened the plate came
    // apart from its previews -- the scaling desktop windows covered it). Under
    // the overview's motion-blur pass (FlightBlur::drawBlurred) the frost is
    // lifted out ahead of the smeared content as every frosted plate is.
    queueDockFrost(monitor, g_organicPlate.box, 0, alpha, {}, &g_organicPlate.region, true);

    CTexPassElement::SRenderData d;
    d.tex = g_organicPlate.tex;
    d.box = g_organicPlate.box;
    d.a   = TINT.a * alpha;
    g_pHyprRenderer->addPassElement(makeUnique<CTexPassElement>(d));
}

CBox panelRiseOrigin(const SLayout& layout, const CBox& anchor) {
    CBox origin = anchor;
    origin.height = 1;
    origin.y      = sc<float>(layout.pill.y) - 1;
    origin.round();
    return origin;
}

// Pass an empty class when nothing is hovered; like the tooltip, the panel
// outlives the hover so that it can animate AWAY instead of blinking out.
void renderHoverPreview(PHLMONITOR monitor, const SLayout& layout, const CBox& anchor, const std::string& wmClass, const std::string& desktopId,
                        const std::string& scratchpadName, const Time::steady_tp& now) {
    auto       windows = panelWindows(wmClass, desktopId, scratchpadName);
    const bool WANT    = !windows.empty();

    if (WANT) {
        const CBox TARGET = panelTarget(monitor, layout, windows, anchor);

        if (!g_hover.panel) {
            // Rises off the dock's top edge, decelerating into place. Growing
            // from the icon box itself painted the live thumbnails over the pill.
            g_hover.panel = SPanel{.wmClass = wmClass, .desktopId = desktopId, .scratchpadName = scratchpadName, .monitor = monitor->m_id, .anchor = anchor, .target = TARGET, .from = panelRiseOrigin(layout, anchor), .moveSince = now, .moveMs = PANEL_IN_MS, .decel = true, .fadeSince = now};
        } else {
            g_hover.panel->leaving = false;
            g_hover.panel->wmClass = wmClass;
            g_hover.panel->desktopId = desktopId;
            g_hover.panel->scratchpadName = scratchpadName;
            g_hover.panel->anchor  = anchor;

            // Caught mid-dismissal, or moved to another icon: in both cases the
            // move restarts from WHERE IT IS, which is what makes it read as one
            // panel following the pointer rather than two panels swapping.
            if (g_hover.panel->out || !sameBox(g_hover.panel->target, TARGET)) {
                const float T = std::clamp(elapsedMs(g_hover.panel->moveSince) / std::max(1.F, g_hover.panel->moveMs), 0.F, 1.F);
                if (g_hover.panel->out)
                    panelFade(*g_hover.panel, 1.F, PANEL_IN_MS, now);

                g_hover.panel->from      = lerpBox(g_hover.panel->from, g_hover.panel->target, houseEase(g_hover.panel->decel, T));
                g_hover.panel->target    = TARGET;
                g_hover.panel->moveSince = now;
                g_hover.panel->moveMs    = g_hover.panel->out ? PANEL_IN_MS : PANEL_SLIDE_MS;
                g_hover.panel->decel     = true;
                g_hover.panel->out       = false;
            }
        }
    } else if (g_hover.panel) {
        if (!g_hover.panel->out && !g_hover.panel->leaving) {
            g_hover.panel->leaving = true;
            g_hover.panel->leftAt  = now;
        }

        // The grace expires: shrink back to the dock's top edge, accelerating.
        // Wall-clock, not animElapsedMs: at anim_scale 0 that clock is already
        // finished, so leaving an icon for the 8px gap above the pill dismissed
        // the panel in the same frame and the dock hid under the previews.
        if (g_hover.panel->leaving && Hyprdrive::Config::realElapsedMs(g_hover.panel->leftAt) >= PANEL_GRACE_MS) {
            const float T = std::clamp(elapsedMs(g_hover.panel->moveSince) / std::max(1.F, g_hover.panel->moveMs), 0.F, 1.F);

            g_hover.panel->from      = lerpBox(g_hover.panel->from, g_hover.panel->target, houseEase(g_hover.panel->decel, T));
            g_hover.panel->target    = panelRiseOrigin(layout, g_hover.panel->anchor);
            g_hover.panel->moveSince = now;
            g_hover.panel->moveMs    = PANEL_OUT_MS;
            panelFade(*g_hover.panel, 0.F, PANEL_OUT_MS, now);
            g_hover.panel->decel     = false;
            g_hover.panel->out       = true;
            g_hover.panel->leaving   = false;
        }

        // The panel keeps drawing the LAST app's windows while it dismisses --
        // the pointer has already left, so nothing else names them.
        windows = panelWindows(g_hover.panel->wmClass, g_hover.panel->desktopId, g_hover.panel->scratchpadName);
        if (windows.empty()) {
            g_hover.panel.reset();
            forgetPanelGeometry();
            return;
        }
    } else {
        forgetPanelGeometry();
        return;
    }

    const float T = std::clamp(elapsedMs(g_hover.panel->moveSince) / std::max(1.F, g_hover.panel->moveMs), 0.F, 1.F);
    if (g_hover.panel->out && T >= 1.F) {
        g_hover.panel.reset();
        forgetPanelGeometry();
        return;
    }

    CBox        DRAWN = lerpBox(g_hover.panel->from, g_hover.panel->target, houseEase(g_hover.panel->decel, T));
    const float SCALE = std::max(monitor->m_scale, 0.01F);
    // Never let the growing plate (or its live window thumbnails) eat the pill.
    // The rise origin sits on the dock's top edge; this is the belt.
    if (const double LIMIT = layout.pill.y; DRAWN.y + DRAWN.height > LIMIT)
        DRAWN.height = std::max(0.0, LIMIT - DRAWN.y);
    DRAWN.round();

    if (DRAWN.width < 2.0 || DRAWN.height < 2.0) {
        forgetPanelGeometry();
        return;
    }

    // The plate fades with the growth so the frost does not slam on at full
    // strength over the icon it is still the size of.
    const float ALPHA   = panelAlpha(*g_hover.panel);
    // CONCENTRIC CORNERS (user, 2026-09-30: "rounding on the app preview only
    // goes so far... an upper limit of 24px outer border radius with an inner
    // border radius to match on the actual window preview"). The plate follows
    // Hyprland's decoration:rounding out by its padding, 12..24 logical px; the
    // previews inside it are the plate's radius less that padding -- which is
    // just decoration:rounding until the plate hits its cap. The window IMAGE
    // is rounded to that too: it used to get the window's rounding scaled down
    // with the thumbnail (~1 px), a square picture in a rounded ring.
    const float OUTERL  = std::clamp(sc<float>(Hyprdrive::Config::themeRounding()) + PANEL_PAD, PANEL_RADIUS, PANEL_RADIUS_MAX);
    const float INNERL  = std::clamp(std::min(sc<float>(Hyprdrive::Config::themeRounding()), OUTERL - PANEL_PAD), 0.F, OUTERL);
    const int   ROUNDED = sc<int>(std::round(OUTERL * SCALE));


    // Cells are laid out inside the box that is actually on screen this frame,
    // not inside the settled one -- otherwise the contents would overflow the
    // plate for the whole of the entrance.
    // Rows (panelRows), scaled to the box that is on screen this frame: the
    // settled cell height times however much of the settled plate is drawn,
    // the smaller of width and height, so the cells never overflow the plate.
    const float PAD     = PANEL_PAD * SCALE;
    const float CGAP    = PANEL_CELL_GAP * SCALE;
    const auto  ROWS    = panelRows(monitor, windows);
    const float NROWS   = sc<float>(std::max<size_t>(1, ROWS.counts.size()));
    const float SETTLEDH = NROWS * ROWS.cellH + (NROWS - 1.F) * CGAP;
    const float FITH    = (sc<float>(DRAWN.height) - 2.F * PAD) / std::max(SETTLEDH, 1.F);
    const float FITW    = (sc<float>(DRAWN.width) - 2.F * PAD) / std::max(ROWS.width, 1.F);
    const float FIT     = std::min(FITH, FITW);
    const float CELLH   = ROWS.cellH * FIT;
    if (CELLH < 1.F || FIT <= 0.F) {
        forgetPanelGeometry();
        return;
    }

    // THE PLATE WRAPS THE ROWS (user, 2026-10-16: "can we have multi-row dock
    // live preview pane correctly wrap around the windows"). One band per row,
    // each its row's width plus the padding, centred like the row -- a shorter
    // row steps in instead of leaving empty glass at its ends. The bands meet
    // half-way across the gap between rows and are drawn as ONE organic
    // outline (organicPlate): convex where it turns out, a concave fillet where
    // it steps between rows. Rows of one width share a band. The widest band
    // is the drawn plate's width, so the rise still grows one shape.
    {
        std::vector<float> rowW;
        size_t             first = 0;
        for (const size_t C : ROWS.counts) {
            float w = 0.F;
            for (size_t i = first; i < first + C && i < windows.size(); ++i)
                w += CELLH * previewAspect(windows[i]) + (i > first ? CGAP * FIT : 0.F);
            rowW.push_back(w);
            first += C;
        }
        if (rowW.empty())
            rowW.push_back(0.F);
        const float WIDEST = *std::ranges::max_element(rowW);
        const float STEP   = CELLH + CGAP * FIT;
        const float TOP    = sc<float>(DRAWN.y) + (sc<float>(DRAWN.height) - (NROWS * CELLH + (NROWS - 1.F) * CGAP * FIT)) / 2.F;

        // WHERE TWO ROWS' BANDS MEET (user, 2026-10-16: "the dock preview
        // multi-row strip does not have consistent padding"). Half-way across
        // the gap left the wider row's EXPOSED edge -- the part of it that
        // sticks out past the other row -- only half a gap from its previews.
        // The join goes where that exposed edge keeps the full padding: a row
        // wider than the one above starts PAD above its own previews, a row
        // wider than the one below ends PAD below them. Equal rows: mid-gap.
        // Rows whose widths differ by less than ONE corner radius per side share
        // one band (the wider): that step cannot hold the outer corner, which is
        // concentric with the previews' corners (user, 2026-10-16: "it doesn't
        // curve around and respect the curvature of the live preview"). Any
        // wider step gets that corner at full radius and its inverted curve as
        // much of the radius as the rest of the step leaves (organicPath).
        std::vector<double> bandW(rowW.size());
        for (size_t k = 0; k < rowW.size(); ++k)
            bandW[k] = std::round(sc<float>(DRAWN.width) - (WIDEST - rowW[k]));
        std::vector<std::pair<size_t, size_t>> groups; // first..last row
        for (size_t k = 0; k < rowW.size(); ++k) {
            if (!groups.empty()) {
                auto&        g  = groups.back();
                const double GW = *std::max_element(bandW.begin() + sc<std::ptrdiff_t>(g.first), bandW.begin() + sc<std::ptrdiff_t>(g.second) + 1);
                if (std::abs(bandW[k] - GW) < 2.0 * ROUNDED) {
                    g.second = k;
                    continue;
                }
            }
            groups.emplace_back(k, k);
        }
        const auto GROUPW = [&](const std::pair<size_t, size_t>& g) {
            return *std::max_element(bandW.begin() + sc<std::ptrdiff_t>(g.first), bandW.begin() + sc<std::ptrdiff_t>(g.second) + 1);
        };
        const auto JOIN = [&](size_t gi) { // between group gi and gi+1
            const size_t K      = groups[gi].second; // last row above the join
            const double BOTTOM = TOP + sc<float>(K) * STEP + CELLH, NEXTTOP = TOP + sc<float>(K + 1) * STEP;
            const double ABOVE = GROUPW(groups[gi]), BELOW = GROUPW(groups[gi + 1]);
            double       y     = (BOTTOM + NEXTTOP) / 2.0;
            if (BELOW > ABOVE + 0.5)
                y = NEXTTOP - PAD;
            else if (BELOW < ABOVE - 0.5)
                y = BOTTOM + PAD;
            return std::round(std::clamp(y, BOTTOM, NEXTTOP));
        };
        std::vector<SPanelBand> bands;
        for (size_t gi = 0; gi < groups.size(); ++gi) {
            const double W  = GROUPW(groups[gi]);
            const double X0 = std::round(sc<float>(DRAWN.x) + (sc<float>(DRAWN.width) - W) / 2.0);
            const double Y0 = gi == 0 ? DRAWN.y : JOIN(gi - 1);
            const double Y1 = gi + 1 == groups.size() ? DRAWN.y + DRAWN.height : JOIN(gi);
            bands.push_back({X0, X0 + W, Y0, Y1});
        }
        // X13/X29/X86: panel frost shares overview chrome blur when Super is open.
        // The corners are Hyprland's rounding_power, as the previews' are, and
        // the plate goes to the pass they go to.
        const float POWER = Hyprdrive::Config::themeRoundingPower();
        if (bands.size() == 1)
            frostedPlate(monitor, DRAWN, ROUNDED, ALPHA, {}, POWER, true);
        else
            organicPlate(monitor, bands, ROUNDED, POWER, ALPHA);
    }

    // Y1/Y2. Published for the pointer, which is why it is the DRAWN plate and
    // the DRAWN cells rather than the settled ones. Stretch the hit box down
    // through PANEL_GAP to the pill so moving from an icon into the previews
    // is never a frame of "left the dock". Cells and frost stay on DRAWN.
    g_hover.drawn = DRAWN;
    if (!g_hover.panel->out) {
        if (const double LIMIT = layout.pill.y; g_hover.drawn.y + g_hover.drawn.height < LIMIT)
            g_hover.drawn.height = LIMIT - g_hover.drawn.y;
    }
    g_hover.cells.clear();
    g_hover.cells.reserve(windows.size());

    const auto  MOUSE      = (g_pInputManager->getMouseCoordsInternal() - monitor->m_position) * monitor->m_scale;
    const int   BORDERSIZE = std::max(1, Hyprdrive::Config::getValue<int>("general:border_size"));
    const auto  ACCENT     = Hyprdrive::Config::highlightColor();
    const float ROUNDPWR   = Hyprdrive::Config::themeRoundingPower();

    const float BLOCKH = NROWS * CELLH + (NROWS - 1.F) * CGAP * FIT;
    float       y      = sc<float>(DRAWN.y) + (sc<float>(DRAWN.height) - BLOCKH) / 2.F;
    size_t      row = 0, inRow = 0;
    float       x = 0.F;
    for (const auto& WINDOW : windows) {
        if (inRow == 0) {
            // A row starts: centred across the plate.
            float  rowW  = 0.F;
            size_t first = 0;
            for (size_t k = 0; k < row; ++k)
                first += ROWS.counts[k];
            for (size_t i = first; i < first + ROWS.counts[row] && i < windows.size(); ++i)
                rowW += CELLH * previewAspect(windows[i]) + (i > first ? CGAP * FIT : 0.F);
            x = sc<float>(DRAWN.x) + (sc<float>(DRAWN.width) - rowW) / 2.F;
        }
        const float WIDTH = CELLH * previewAspect(WINDOW);

        CBox cell{std::round(x), std::round(y), std::round(WIDTH), std::round(CELLH)};
        // Never past half the cell (a very short or narrow preview).
        const int INNER = std::clamp(sc<int>(std::round(INNERL * SCALE)), 0, std::max(0, sc<int>(std::min(cell.width, cell.height) / 2.0) - BORDERSIZE));
        renderLivePreview(monitor, WINDOW, cell, now, 1.F, false, false, INNER);
        g_hover.cells.emplace_back(cell, PHLWINDOWREF{WINDOW});

        // A window parked on a SCRATCHPAD is not one of the app's ordinary
        // windows, and the preview says so rather than leaving the user to
        // wonder why clicking it slides a workspace in. Suppressed on the
        // scratchpad's own panel, where every cell would carry the same label.
        //
        // A window ASKING FOR ATTENTION wears the same dimming, labelled
        // "Needs attention" in yellow (user, 2026-11-04): the dock icon jumps
        // for the app, and an app with several windows (Ghostty) leaves you
        // guessing which one it is. Urgency wins over the scratchpad label.
        const bool URGENT = WINDOW->m_isUrgent;
        const bool PARKED = g_hover.panel->scratchpadName.empty() && Scratchpad::holdsWindow(WINDOW);
        if (URGENT || PARKED) {
            const int DIMROUND = INNER;

            CRectPassElement::SRectData dim;
            dim.box           = cell.copy().round();
            dim.color         = CHyprColor{0.F, 0.F, 0.F, PANEL_PARKED_DIM * ALPHA};
            dim.round         = DIMROUND;
            dim.roundingPower = ROUNDPWR;
            g_pHyprRenderer->addPassElement(makeUnique<CRectPassElement>(dim));

            // Deliberately NOT a theme colour, and that is the whole point of
            // the label: it is read over a DARKENED thumbnail, and a dark
            // theme's own foreground is the one colour that disappears there.
            // Wrapped, centred line by line (user, 2026-11-04): a narrow cell
            // breaks "Needs attention" onto two lines instead of cutting it to
            // "Needs attenti...". The ellipsis is left for a cell too small
            // even for that.
            const int LABELPX = std::max(1, sc<int>(std::round(PANEL_PARKED_PT * SCALE)));
            const int LABELPAD = sc<int>(std::round(8.F * SCALE));
            if (const auto TEX = OverviewRender::textWrapped(URGENT ? "Needs attention" : "in scratchpad",
                                                             URGENT ? CHyprColor{1.F, 0.86F, 0.2F, 1.F} : CHyprColor{1.F, 1.F, 1.F, 0.96F}, LABELPX,
                                                             std::max(8, sc<int>(cell.width) - 2 * LABELPAD), std::max(8, sc<int>(cell.height) - 2 * LABELPAD), false, "",
                                                             URGENT ? 700 : 400, true);
                TEX && TEX->ok()) {
                CTexPassElement::SRenderData label;
                label.tex = TEX;
                label.box = CBox{std::round(sc<float>(cell.x) + (sc<float>(cell.width) - sc<float>(TEX->m_size.x)) / 2.F),
                                 std::round(sc<float>(cell.y) + (sc<float>(cell.height) - sc<float>(TEX->m_size.y)) / 2.F), sc<float>(TEX->m_size.x),
                                 sc<float>(TEX->m_size.y)};
                label.a   = ALPHA;
                g_pHyprRenderer->addPassElement(makeUnique<CTexPassElement>(label));
            }
        }

        // Theme inactive_border is often fully transparent (Omarchy border_size
        // 0), which is why Gemini's ring was queued and still invisible. Same
        // accent the pill uses, thinner when the cell is not hovered.
        // The step between the two fades on Hyprland's `border` animation, as
        // a window's border does when it takes focus (BorderFade.hpp).
        const bool IS_HOVERED = !g_hover.panel->out && cell.containsPoint(MOUSE);
        const int  ROUND      = INNER;
        CBorderPassElement::SBorderData border;
        border.box = cell.copy().round();
        border.grad1.reset(BorderFade::color(monitor, BorderFade::key(WINDOW.get(), (sc<uint64_t>(monitor->m_id) << 8) | 0xD0), IS_HOVERED, ACCENT.modifyA(ALPHA * 0.55F),
                                             ACCENT.modifyA(ALPHA)));
        border.a             = 1.F;
        border.borderSize    = BORDERSIZE;
        border.round         = ROUND;
        border.roundingPower = ROUNDPWR;
        g_pHyprRenderer->addPassElement(makeUnique<CBorderPassElement>(border));

        x += WIDTH + CGAP * FIT;
        if (++inRow >= ROWS.counts[row] && row + 1 < ROWS.counts.size()) {
            inRow = 0;
            ++row;
            y += CELLH + CGAP * FIT;
        }
    }
}

// Y1. The autohide's question, answered against the plate on screen. Declared
// far above, beside updateAutohide, which runs long before any of this is in
// scope.
bool panelHoldsDock(PHLMONITOR monitor) {
    if (!monitor || !g_hover.panel)
        return false;

    return panelContains(monitor, (g_pInputManager->getMouseCoordsInternal() - monitor->m_position) * monitor->m_scale);
}

// Whether `monitor` has frames to draw for the panel: it is the panel's monitor,
// or -- a preview is only live if its window keeps committing -- it is where a
// previewed window lives. A client on a fifo surface only gets its next commit
// released when a monitor it is on presents, and nothing else would make an
// otherwise idle monitor present.
bool panelNeedsFramesOn(const PHLMONITOR& monitor) {
    // ITS OWN MONITOR ONLY. This used to answer true for the monitor a
    // PREVIEWED WINDOW happens to live on as well, which kept that whole output
    // repainting at its refresh rate for a panel it was not drawing (user,
    // 2026-09-14). The fifo client still gets unblocked: renderLivePreview
    // damages that window's own box on that output every frame it draws it,
    // which is all a held commit needs and a thousandth of the pixels.
    return monitor && g_hover.panel && g_hover.panel->monitor == monitor->m_id;
}

// HOW MUCH OF THE OUTPUT ONE DOCK ANIMATION FRAME HAS TO REPAINT.
//
// pumpFrame damaged the WHOLE monitor for every frame of every dock animation,
// which on the plain desktop means re-rendering every window and every blur
// behind them so an icon can lean two degrees (user, 2026-09-14). The dock draws
// in a band along the bottom edge, so the band is what gets damaged -- FULL
// WIDTH, deliberately: the pill already spans most of the output, and the caret,
// the badge, the tooltip and the hover highlight all move sideways within it, so
// horizontal precision buys nothing and can only be wrong.
//
// nullopt means "damage everything", and it is the answer for anything that
// leaves the band or whose box this function has not been handed. Being wrong
// here is a STALE ROW on screen, so every unknown resolves that way.
std::optional<CBox> dockDamageRegion(PHLMONITOR monitor, const SLayout& layout) {
    if (!monitor)
        return std::nullopt;

    // WITHDRAWN (user, 2026-09-15): "going through previews it slices half of
    // the image off my windows". The band below is computed from boxes the LAST
    // frame published -- g_hover.drawn above all -- so every frame of the hover
    // panel's entrance damages the box the panel had BEFORE it grew, and the
    // strip of panel above that line keeps whatever was under it. The same hole
    // exists for anything that moves or grows between the damage decision and
    // the draw, which is most of what the dock animates.
    //
    // Damaging what the NEXT frame will draw is only knowable after laying that
    // frame out -- the region would have to come out of render(), not out of
    // damageIfChanged. Until then the dock damages its whole output, as it
    // always did: the saving was real, but it was never the reason the overview
    // stutters (that is the GPU batch, measured 2026-09-15), and correctness is
    // not the thing to trade for it.
    return std::nullopt;

    // The overview repaints the whole output every frame regardless.
    if (scrollOverviewForMonitor(monitor))
        return std::nullopt;

    // Everything that leaves the dock's neighbourhood. A flight crosses the
    // screen, an icon flies home from wherever it was dropped, a launch blow-up
    // grows to 3x and drifts upward, the V2 window pill sits on a window
    // anywhere, and a window drag paints a halo the pill's own box does not
    // describe. None of them is bounded by the band.
    if (Flight::anyOn(monitor) || ghostShowing() || ghostFlying() || !g_flight.launchZooms.empty() || !g_flight.bounces.empty() || g_flight.windowDragActive ||
        WindowPill::animating())
        return std::nullopt;

    // A stack fan or grid: transient, and its plate is laid out inside its own
    // module. Not worth a second geometry API to save a handful of frames.
    if (Stacks::popupMonitor() == monitor->m_id)
        return std::nullopt;

    const double SCALE = std::max(monitor->m_scale, 0.01F);
    const double H     = sc<double>(monitor->m_size.y) * SCALE;

    // The RESTING pill, not the slid one: while the dock is on its way out the
    // slid box is lower (or gone entirely) and the pixels being cleared are the
    // ones it used to occupy, up at the resting height.
    const auto& BASE = baseLayout(monitor);
    if (BASE.pill.empty())
        return std::nullopt; // nothing to reason about; the caller gets the lot

    double top = std::min(sc<double>(BASE.pill.y), layout.pill.empty() ? sc<double>(BASE.pill.y) : sc<double>(layout.pill.y));

    // The hover panel and the context menu float ABOVE the pill and are much
    // taller than it. Both publish the box they were actually drawn at, and
    // both start life with no box at all -- on that first frame there is
    // nothing to reach over with, so the whole output is the honest answer.
    if (g_hover.panel && g_hover.panel->monitor == monitor->m_id) {
        if (g_hover.drawn.width < 1 || g_hover.drawn.height < 1)
            return std::nullopt;
        top = std::min(top, sc<double>(g_hover.drawn.y));
    }

    if (g_menu && g_menu->monitor == monitor->m_id) {
        if (g_menu->box.empty())
            return std::nullopt;
        top = std::min(top, sc<double>(g_menu->box.y));
    }

    // GENEROUS, on purpose. Above the topmost box there is still the plate's
    // drop shadow and the frost's sampling margin, the tooltip -- which hangs
    // over the pill and has no remembered box of its own -- the bounce lifting
    // an icon out of the cell, and DockTilt's projection throwing a leaning
    // icon's corner past its rectangle. All of them are small; the band is
    // cheap; so the margin is one whole dock's worth rather than a measured
    // minimum that a future surface would quietly outgrow.
    constexpr double MARGIN = 140.0; // logical px
    top -= MARGIN * SCALE;

    // Down to the bottom edge, and past it: the autohide tucks the pill BELOW
    // the monitor and the last frames of that slide clear rows at the very
    // bottom.
    return CBox{0.0, std::max(0.0, top), sc<double>(monitor->m_size.x) * SCALE, H - std::max(0.0, top)};
}
}


// Z15 (user): a flight click kills the Y1 hover panel INSTANTLY. Leaving it
// alive re-eased it from a stale parked box, which read as "a panel dragged from
// the bottom of the screen" under the restore. The tooltip goes with it: it is a
// separate element and lingered into the flight ("sh" bubble, frame 279 of the
// verification film), and the fade-out path only runs while a name is
// remembered. Named, because FlightController.cpp calls it.
void dismissHoverPanelNow() {
    g_hover.panel.reset();
    g_tip = {};
}

// The strip between the pill and an open hover-preview panel. Crossing it from
// an icon to its previews is one gesture; counted as empty space, the window
// underneath got an enter/leave on the way (user, 2026-09-29, hover pass-through).
// Whichever side of the pill the panel is on; the panel's own span across it.
static bool panelBridgeContains(const PHLMONITOR& monitor, const Vector2D& point) {
    if (!panelOn(monitor) || g_hover.panel->out || g_hover.drawn.width < 1 || g_hover.drawn.height < 1)
        return false;
    const auto PILL = layout(monitor).pill;
    if (PILL.width < 1 || PILL.height < 1)
        return false;
    const auto& P = g_hover.drawn;
    CBox        bridge;
    if (P.y + P.height <= PILL.y) // above (bottom dock)
        bridge = CBox{P.x, P.y + P.height, P.width, PILL.y - (P.y + P.height)};
    else if (P.y >= PILL.y + PILL.height) // below (top dock)
        bridge = CBox{P.x, PILL.y + PILL.height, P.width, P.y - (PILL.y + PILL.height)};
    else if (P.x + P.width <= PILL.x) // left of a right-hand dock
        bridge = CBox{P.x + P.width, P.y, PILL.x - (P.x + P.width), P.height};
    else if (P.x >= PILL.x + PILL.width) // right of a left-hand dock
        bridge = CBox{PILL.x + PILL.width, P.y, P.x - (PILL.x + PILL.width), P.height};
    return bridge.width >= 1 && bridge.height >= 1 && bridge.containsPoint(point);
}

bool occupiesPointer(PHLMONITOR monitor, const Vector2D& point) {
    if (!monitor)
        return false;
    return pillContains(monitor, point) || panelContains(monitor, point) || panelBridgeContains(monitor, point) || menuContains(monitor, point) ||
        Stacks::contains(monitor, point);
}

void syncPointerFocus(PHLMONITOR monitor, const Vector2D& point) {
    if (monitor && occupiesPointer(monitor, point)) {
        g_dockSeatHeld = true;
        stealDockSeat();
        if (g_pEventLoopManager) {
            if (g_dockSeatDoLaterSeq) {
                g_pEventLoopManager->removeDoLater(g_dockSeatDoLaterSeq);
                g_dockSeatDoLaterSeq = 0;
            }
            g_dockSeatDoLaterSeq = g_pEventLoopManager->doLater([] {
                g_dockSeatDoLaterSeq = 0;
                if (g_dockSeatHeld)
                    stealDockSeat();
            });
        }
        return;
    }
    restoreDockSeat();
}

void releasePointerFocus() {
    // ONLY THE SEAT. This used to cancel every monitor's outstanding frame
    // request as well, and it is called from the pointer hook on every motion
    // over a monitor with an overview on it -- so moving the mouse on one
    // output silently dropped the OTHER output's pump and froze its dock
    // mid-animation (user, 2026-09-14). The pump marks are self-clearing (see
    // pumpFrame) and forgetMonitor takes them out when an output goes; nothing
    // about the seat needs them cancelled.
    if (g_pEventLoopManager && g_dockSeatDoLaterSeq) {
        g_pEventLoopManager->removeDoLater(g_dockSeatDoLaterSeq);
        g_dockSeatDoLaterSeq = 0;
    }
    restoreDockSeat();
}

void noteRetiled(const PHLWINDOW& window) {
    if (!window)
        return;

    std::erase_if(g_retiled, [&](const auto& e) { return !e.first.lock() || realElapsedMs(e.second) >= RETILED_MS; });
    for (auto& e : g_retiled) {
        if (e.first.lock() == window) {
            e.second = Time::steadyNow();
            return;
        }
    }
    g_retiled.emplace_back(PHLWINDOWREF{window}, Time::steadyNow());
}

bool isRetiling(const PHLWINDOW& window) {
    if (!window)
        return false;

    return std::ranges::any_of(g_retiled, [&](const auto& e) { return e.first.lock() == window && realElapsedMs(e.second) < RETILED_MS; });
}

void pumpRetiled(const PHLWINDOW& window, PHLMONITOR monitor, const Time::steady_tp& now) {
    if (!window || !monitor || !isRetiling(window))
        return;

    if (!window->wlSurface() || !window->wlSurface()->resource())
        return;

    presentPreview(window, monitor, now);
}

bool isPreviewing(const PHLWINDOW& window) {
    if (!window)
        return false;

    return std::ranges::any_of(g_previewed, [&](const auto& e) { return e.first.lock() == window && realElapsedMs(e.second) < PREVIEWING_MS; });
}

// A live preview let go of. Near where it was picked up it is still the click
// it always was; anywhere off the dock it is the window itself, landing.
bool panelDragRelease(PHLMONITOR monitor, const Vector2D& point) {
    if (!g_dockDrag.panel)
        return false;

    const auto DRAG = *g_dockDrag.panel;
    g_dockDrag.panel.reset();

    const auto WINDOW = DRAG.window.lock();
    if (!WINDOW || !monitor)
        return true;

    // The only release that still reaches here is a press that never became a
    // drag -- past the threshold the gesture was handed to the window drag and
    // this state was dropped with it. So this is the click it always was.

    // MINIMIZED IS NOT AN ORDINARY SELECTION. A stashed window lives on
    // special:minimized, which is not one of the overview's cards -- so
    // selectAndClose falls into its "this window is nowhere in the overview"
    // branch, sets closeOnWindow and closes; the close then reads
    // closeOnWindow->m_workspace and lands the viewport ON special:minimized,
    // which Omarchy slides in VERTICALLY. That is the "comes up from below the
    // screen" this looked like, and it is exactly the hazard activateWindow's
    // own comment warns about ("never toggle special:minimized"). activateWindow
    // guards it; selectAndClose never learned to. Un-stash through the guarded
    // path first, then just shut the overview.
    if (Minimize::isMinimized(WINDOW)) {
        activateWindow(WINDOW); // showOriginalWorkspace + restore, onto a real desktop
        if (const auto OVERVIEW = scrollOverviewForMonitor(monitor))
            OVERVIEW->close();
        return true;
    }

    if (const auto OVERVIEW = scrollOverviewForMonitor(monitor); OVERVIEW)
        OVERVIEW->selectAndClose(WINDOW);
    else
        activateWindow(WINDOW);
    return true;
}

bool panelClick(PHLMONITOR monitor, const Vector2D& point, bool release) {
    if (!monitor)
        return false;

    // A live tear-out answers its own release wherever it lands -- ahead of the
    // panelContains gate below, because the whole point is that it ends up
    // somewhere the panel is not.
    if (release && g_dockDrag.panel)
        return panelDragRelease(monitor, point);

    if (!panelContains(monitor, point))
        return false;

    if (!release) {
        // Record what it would take to tear this preview out. NOTHING is
        // committed here: the drag only goes live once the pointer passes the
        // threshold, so a press that does not move is still a click.
        if (const auto CELL = panelCellAt(monitor, point)) {
            g_dockDrag.panel = SPanelDrag{.window = CELL->second,
                                          .start  = point,
                                          .home   = CELL->first,
                                          .grab   = point - Vector2D{CELL->first.x, CELL->first.y},
                                          .at     = point};
        }
        return true;
    }

    const auto CELL = panelCellAt(monitor, point);
    if (!CELL)
        return true;

    const auto WINDOW = CELL->second;

    // Always go to the window on its own desktop. A restore-flight from here
    // would pull it onto the current desktop from the dock (below the screen);
    // switching first is what "click the preview of that window" means.
    // ...and the cursor goes to it, CENTRED (user, 2026-11-03: "not only
    // switch me to the desktop ... and focus on that window but also center my
    // cursor on it"). On a canvas desktop the focus starts a pan to bring it
    // into view; the cursor, put on the window first, rides that pan.
    if (const auto OVERVIEW = scrollOverviewForMonitor(monitor); OVERVIEW) {
        if (pointerFollowsClick())
            overviewCenterCursorOnHandoff(); // the overview's close does the warp, once it has handed focus over
        OVERVIEW->selectAndClose(WINDOW);
    } else {
        activateWindow(WINDOW);
        if (pointerFollowsClick())
            pointerToWindow(WINDOW);
    }

    return true;
}

// Y5. The flight is a real window at real SIZE, not a thumbnail, so where it is
// legal it draws the way the overview draws a tile -- live blur, frost scaled to
// the render scale, real decorations. It takes that path EVERYWHERE, desktop
// included, which is the point of the user's instruction. What differs off the
// overview is only PASS OWNERSHIP: inside the overview the plugin owns the
// render pass and the path may flush it mid-draw, while at RENDER_POST_WINDOWS a
// flush renders and CLEARS everything Hyprland queued for the rest of the frame.
// Measured: flushing there wedged the frame loop and stranded a restoring window
// on the stash permanently. Blur scaling and forced live blur do not depend on
// the flush, so the frost is correct in both.
//
// This was a lambda inside render(). It is a named function because the flight
// LOOP moved to FlightController.cpp and the drawing did not: the dock owns the
// render context, so the flight module hands it back the window, the box, the
// alpha and this frame's `now`, and the pixels are produced by exactly the code
// that produced them before.
void drawFlightSurface(PHLMONITOR monitor, const PHLWINDOW& window, const CBox& box, float alpha, const Time::steady_tp& now, bool ownsPass) {
    if (!window)
        return;

    // Z9r5: a window hidden for its flight is skipped by the surface machinery
    // (validMapped), so the proxy rendered as border + tint with no content --
    // measured, frame 130 of the zen film. Unhide strictly around this draw: the
    // compositor's own window pass ran before the dock's chrome pass this frame,
    // so it never observes the flip.
    const bool REHIDE = window->isHidden();
    if (REHIDE) {
        window->setHidden(false);
        Membership::bump("flight draw unhide");
    }
    renderLivePreview(monitor, window, box, now, alpha, true, ownsPass);
    if (REHIDE) {
        window->setHidden(true);
        Membership::bump("flight draw rehide");
    }
}
}
