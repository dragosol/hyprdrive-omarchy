#include "ScratchpadExit.hpp"

#include "OverviewInternal.hpp"
#include "OverviewRender.hpp"
#include "Scratchpad.hpp"

#include <hyprland/src/protocols/core/DataDevice.hpp>
#include <hyprland/src/render/pass/TexPassElement.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/desktop/view/LayerSurface.hpp>
#include <hyprland/src/protocols/core/Compositor.hpp>

#include <cairo/cairo.h>
#include <drm_fourcc.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <optional>
#include <unordered_map>

namespace ScrollOverview::ScratchpadExit {

    namespace {
        constexpr const char* LABEL = "\uf062   Drag here to exit scratchpad";

        // THE ZONE (user, 2026-09-24: "a cooler looking more intuitive drag
        // area ... something with like an animated yellow and black 'hazard'
        // lines slowly moving along ... not too in-your-face").
        //
        // A band in the same theme plate the search pill uses, with 45-degree
        // hazard stripes drifting across it at low alpha -- warning tape, not a
        // siren -- and the label on its own dark pill in the middle so the text
        // never has to fight the stripes.
        constexpr float BAND_HL      = 60.F;  // logical px
        constexpr float BAND_MIN_WL  = 520.F;
        constexpr float BAND_SIDE_WL = 90.F;  // stripes either side of the label
        constexpr float TOP_GAPL     = 6.F;   // from the very top of the screen, over the bar
        constexpr float HIT_MARGINL  = 28.F;  // the target is forgiving; the band is not
        constexpr float FADE_IN_MS   = 180.F;

        constexpr float STRIPE_WL    = 11.F;  // one stripe's width, logical
        constexpr float PERIODL      = 26.F;  // stripe + gap
        constexpr float DRIFT_MS     = 2400.F; // one period of drift: slow
        constexpr int   PHASES       = 36;    // pre-rendered drift steps per period
        constexpr float STRIPE_ALPHA = 0.30F;
        constexpr int   TICK_MS      = 33;

        // THE BAR STRIP (user, 2026-09-24: "a transparent more subtle zone
        // similarly animated ... just overlaying the top bar (if there is one
        // visible) with a smaller text called 'hover to exit scratchpad'"). The
        // everyday way out, while the scratchpad is simply up: the same stripes
        // over the bar at a third of the drag zone's strength, and a DWELL
        // rather than an instant trigger -- the bar is where the pointer goes to
        // reach wifi and the clock, and passing over it must not throw the
        // scratchpad away. The stripes brighten as the dwell fills, so the exit
        // is seen coming.
        constexpr const char* BAR_LABEL     = "hover to exit scratchpad";
        constexpr float       BAR_MIN_HL    = 12.F;   // thinner than this is not a bar
        constexpr float       BAR_ALPHA     = 0.38F;  // x STRIPE_ALPHA: quiet
        constexpr float       BAR_DWELL_MS  = 250.F;
        constexpr int         BAR_TICK_MS   = 66;     // it can be up for hours; drift at ~15 fps
        // The words are plain text in the theme's own foreground -- no plate:
        // on a bar a plate is one more widget, and text in the bar's colour
        // reads as part of the bar in light themes and dark ones alike. They sit
        // just after the bar's LEFT cluster (Omarchy: the logo and the
        // workspaces), found by looking at the bar's own pixels, because that
        // cluster grows and shrinks with the workspaces.
        constexpr float BAR_FONT_MAXL   = 11.F;  // logical px; the bar's own text is ~12
        constexpr float BAR_TEXT_GAPL   = 18.F;  // after the left cluster
        constexpr float CLUSTER_GAPL    = 40.F;  // a run of bare bar this wide ends a cluster
        constexpr float BAR_SCAN_MS     = 500.F; // the bar redraws for its clock; rescan no faster
        constexpr float BAR_FALLBACK_X  = 0.18F; // of the width, when the bar cannot be read

        struct SBarScan {
            Time::steady_tp at        = {};
            bool            valid     = false;
            float           leftEndL  = -1.F; // end of the left cluster, logical, from the bar's left edge
            float           nextL     = -1.F; // start of the next cluster (the clock); -1 = none
        };
        std::unordered_map<MONITORID, SBarScan> g_barScan;

        struct SState {
            bool            shown = false;
            Time::steady_tp since = {};
            CBox            zoneL;        // the hit area, GLOBAL LOGICAL; empty when not drawn

            bool                           barShown = false;
            Time::steady_tp                barSince = {};
            CBox                           barL;          // the bar strip, GLOBAL LOGICAL
            bool                           armed = false; // the pointer has been OFF the bar since it appeared
            std::optional<Time::steady_tp> hoverSince;
        };

        std::unordered_map<MONITORID, SState> g_state;

        // The stripe frames, one per drift step, for one band size. Rebuilt only
        // when the band's pixel size changes (a scale change, a new monitor).
        struct SStripes {
            Vector2D                          size;
            std::vector<SP<Render::ITexture>> frames;
        };
        std::unordered_map<uint64_t, SStripes> g_stripes; // (monitor, surface)
        constexpr uint64_t                      SURF_ZONE = 0, SURF_BAR = 1;

        wl_event_source* g_tick = nullptr;

        bool dragging() {
            return PROTO::data && PROTO::data->dndActive();
        }

        PHLWORKSPACE shownScratchpad(const PHLMONITOR& monitor) {
            if (!monitor)
                return {};
            const auto SPECIAL = monitor->m_activeSpecialWorkspace;
            return SPECIAL && Scratchpad::isScratchpad(SPECIAL) ? SPECIAL : PHLWORKSPACE{};
        }

        SP<Render::ITexture> stripeFrame(const PHLMONITOR& monitor, uint64_t surface, const Vector2D& size, int phase) {
            auto& st = g_stripes[(sc<uint64_t>(monitor->m_id) << 1) | surface];
            if (st.size != size) {
                st.size = size;
                st.frames.assign(PHASES, nullptr);
            }
            auto& slot = st.frames[sc<size_t>(phase % PHASES)];
            if (slot)
                return slot;

            const int W = std::max(1, sc<int>(size.x)), H = std::max(1, sc<int>(size.y));
            auto*     surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, W, H);
            auto*     cr   = cairo_create(surf);
            cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
            cairo_paint(cr);
            cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

            const double S      = monitor->m_scale;
            const double PERIOD = PERIODL * S;
            const double STRIPE = STRIPE_WL * S;
            const double OFFSET = PERIOD * sc<double>(phase % PHASES) / PHASES;
            // Omarchy-ish hazard yellow; the "black" is the plate showing through.
            cairo_set_source_rgba(cr, 0.98, 0.78, 0.12, STRIPE_ALPHA);
            for (double x = -H - PERIOD + OFFSET; x < W + PERIOD; x += PERIOD) {
                cairo_move_to(cr, x, H);
                cairo_line_to(cr, x + H, 0);
                cairo_line_to(cr, x + H + STRIPE, 0);
                cairo_line_to(cr, x + STRIPE, H);
                cairo_close_path(cr);
            }
            cairo_fill(cr);
            cairo_destroy(cr);
            cairo_surface_flush(surf);

            slot = g_pHyprRenderer->createTexture(surf);
            cairo_surface_destroy(surf);
            return slot;
        }

        // The bar's dwell. Driven from the timer as well as from motion: a
        // pointer RESTING on the bar sends no motion, and the dwell has to end.
        void updateBarHover() {
            const auto CURSOR = g_pInputManager->getMouseCoordsInternal();
            for (const auto& MON : State::monitorState()->monitors()) {
                if (!MON)
                    continue;
                auto& st = g_state[MON->m_id];
                if (!st.barShown || st.barL.width < 1)
                    continue;

                if (!st.barL.containsPoint(CURSOR)) {
                    st.armed = true;
                    st.hoverSince.reset();
                    continue;
                }
                if (!st.armed)
                    continue; // still where it was when the scratchpad came up
                if (!st.hoverSince) {
                    st.hoverSince = Time::steadyNow();
                    continue;
                }
                if (std::chrono::duration<float, std::milli>(Time::steadyNow() - *st.hoverSince).count() >= BAR_DWELL_MS) {
                    if (const auto SPECIAL = shownScratchpad(MON)) {
                        Scratchpad::hide(SPECIAL);
                        g_pHyprRenderer->damageMonitor(MON);
                    }
                    st.hoverSince.reset();
                }
            }
        }

        // The drift has to keep moving while the pointer is still, and the
        // render pass must not schedule its own frames -- so a timer asks.
        int tickCb(void*) {
            bool zone = false, bar = false;
            for (auto& [id, st] : g_state) {
                if (st.shown && st.zoneL.width >= 1) {
                    g_pHyprRenderer->damageBox(st.zoneL);
                    zone = true;
                }
                if (st.barShown && st.barL.width >= 1) {
                    g_pHyprRenderer->damageBox(st.barL);
                    bar = true;
                }
            }
            updateBarHover();
            if ((zone || bar) && g_tick)
                wl_event_source_timer_update(g_tick, zone ? TICK_MS : BAR_TICK_MS);
            return 0;
        }

        void armTick() {
            if (!g_tick)
                g_tick = wl_event_loop_add_timer(g_pCompositor->m_wlEventLoop, tickCb, nullptr);
            if (g_tick)
                wl_event_source_timer_update(g_tick, TICK_MS);
        }
    }

    // The bar's layer surface: the same test overviewTopChromeBottomL uses.
    static PHLLS topBar(const PHLMONITOR& monitor) {
        for (const auto LEVEL : {ZWLR_LAYER_SHELL_V1_LAYER_TOP, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY}) {
            for (auto const& lsRef : monitor->m_layerSurfaceLayers[LEVEL]) {
                const auto LS = lsRef.lock();
                if (!Desktop::View::validMapped(LS))
                    continue;
                const auto& G = LS->m_geometry;
                if (sc<float>(G.w) < sc<float>(monitor->m_size.x) * 0.5F || sc<float>(G.h) > sc<float>(monitor->m_size.y) / 3.F)
                    continue;
                if (sc<float>(G.y) > sc<float>(monitor->m_position.y) + 1.F)
                    continue;
                return LS;
            }
        }
        return nullptr;
    }

    // Three RGBA rows across the bar's middle (35%, 50%, 65% of its height).
    // Waybar (GTK) hands over shm buffers the CPU can read in place; a bar drawn
    // on the GPU -- Omarchy's Quickshell bar is -- is a dmabuf, read back through
    // a scratch framebuffer instead. Three rows, a few times a second: the
    // readback is a few kilobytes.
    struct SBarRows {
        int                                 w = 0;
        std::array<std::vector<uint8_t>, 3> rgba;
    };

    static std::optional<SBarRows> readShm(const SP<IHLBuffer>& buf) {
        const auto SHM = buf->shm();
        if (!SHM.success)
            return std::nullopt;
        bool bgrx;
        if (SHM.format == DRM_FORMAT_ARGB8888 || SHM.format == DRM_FORMAT_XRGB8888)
            bgrx = true;
        else if (SHM.format == DRM_FORMAT_ABGR8888 || SHM.format == DRM_FORMAT_XBGR8888)
            bgrx = false;
        else
            return std::nullopt;

        const auto [DATA, FMT, LEN] = buf->beginDataPtr(0);
        const int  BW = sc<int>(SHM.size.x), BH = sc<int>(SHM.size.y);
        if (!DATA || BW < 8 || BH < 4 || SHM.stride < BW * 4 || LEN < sc<size_t>(SHM.stride) * BH) {
            buf->endDataPtr();
            return std::nullopt;
        }
        SBarRows   out;
        out.w           = BW;
        const int YS[3] = {BH * 35 / 100, BH / 2, BH * 65 / 100};
        for (int r = 0; r < 3; ++r) {
            out.rgba[r].resize(sc<size_t>(BW) * 4);
            const uint8_t* ROW = DATA + sc<size_t>(YS[r]) * SHM.stride;
            for (int x = 0; x < BW; ++x) {
                const uint8_t* P = ROW + sc<size_t>(x) * 4;
                uint8_t*       O = out.rgba[r].data() + sc<size_t>(x) * 4;
                O[0] = bgrx ? P[2] : P[0];
                O[1] = P[1];
                O[2] = bgrx ? P[0] : P[2];
                O[3] = P[3];
            }
        }
        buf->endDataPtr();
        return out;
    }

    static std::optional<SBarRows> readTexture(const SP<Render::ITexture>& tex) {
        if (!tex || !tex->m_texID || tex->m_type == Render::TEXTURE_EXTERNAL || tex->m_size.x < 8 || tex->m_size.y < 4)
            return std::nullopt;
        const int BW = sc<int>(tex->m_size.x), BH = sc<int>(tex->m_size.y);

        GLint prevRead = 0, prevDraw = 0;
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevRead);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevDraw);

        GLuint fbo = 0;
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex->m_texID, 0);

        std::optional<SBarRows> out;
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
            out.emplace();
            out->w          = BW;
            const int YS[3] = {BH * 35 / 100, BH / 2, BH * 65 / 100};
            for (int r = 0; r < 3; ++r) {
                out->rgba[r].resize(sc<size_t>(BW) * 4);
                glReadPixels(0, YS[r], BW, 1, GL_RGBA, GL_UNSIGNED_BYTE, out->rgba[r].data());
            }
        }

        glBindFramebuffer(GL_READ_FRAMEBUFFER, prevRead);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prevDraw);
        glDeleteFramebuffers(1, &fbo);
        return out;
    }

    // Where the bar's left cluster ends, against the bar's most common colour.
    // A bar that can be read neither way gets the fixed fallback.
    static SBarScan scanBar(const PHLLS& ls) {
        SBarScan out;
        out.valid = true;
        if (!ls || !ls->wlSurface() || !ls->wlSurface()->resource())
            return out;
        const auto& REF = ls->wlSurface()->resource()->m_current.buffer;
        if (!REF || !REF.m_buffer)
            return out;
        auto rows = readShm(REF.m_buffer);
        if (!rows)
            rows = readTexture(REF.m_buffer->m_texture);
        if (!rows)
            return out;

        const int   BW     = rows->w;
        const float PXPERL = sc<float>(BW) / std::max(1.F, sc<float>(ls->m_geometry.w));
        auto        px     = [&](int r, int x) { return rows->rgba[r].data() + sc<size_t>(x) * 4; };

        std::unordered_map<uint32_t, int> hist;
        for (int x = 0; x < BW; ++x) {
            const uint8_t* P = px(1, x);
            ++hist[(sc<uint32_t>(P[0]) << 16) | (sc<uint32_t>(P[1]) << 8) | sc<uint32_t>(P[2])];
        }
        uint32_t bg = 0;
        int      best = -1;
        for (const auto& [c, n] : hist)
            if (n > best) {
                best = n;
                bg   = c;
            }
        const int BR = sc<int>(bg >> 16), BGc = sc<int>((bg >> 8) & 0xFF), BB = sc<int>(bg & 0xFF);

        std::vector<bool> ink(BW, false);
        for (int x = 0; x < BW; ++x)
            for (int r = 0; r < 3; ++r) {
                const uint8_t* P = px(r, x);
                if (std::abs(P[0] - BR) + std::abs(P[1] - BGc) + std::abs(P[2] - BB) > 60) {
                    ink[x] = true;
                    break;
                }
            }

        const int GAP = std::max(4, sc<int>(CLUSTER_GAPL * PXPERL));
        int       first = -1, last = -1, x = 0;
        for (; x < BW; ++x) {
            if (ink[x]) {
                if (first < 0)
                    first = x;
                last = x;
            } else if (first >= 0 && x - last > GAP)
                break;
        }
        out.leftEndL = first < 0 ? 0.F : sc<float>(last + 1) / PXPERL;
        for (; x < BW; ++x)
            if (ink[x]) {
                out.nextL = sc<float>(x) / PXPERL;
                break;
            }
        return out;
    }

    static void renderBar(PHLMONITOR monitor) {
        auto&       st   = g_state[monitor->m_id];
        const float BARL = OvInternal::overviewTopChromeBottomL(monitor) - sc<float>(monitor->m_position.y);
        const bool  WANT = !dragging() && shownScratchpad(monitor) && BARL >= BAR_MIN_HL;
        if (WANT != st.barShown) {
            st.barShown = WANT;
            st.barSince = Time::steadyNow();
            st.armed    = false;
            st.hoverSince.reset();
            if (WANT)
                armTick();
        }
        if (!WANT)
            return;

        const float S       = sc<float>(monitor->m_scale);
        const CBox  BAR{0.0, 0.0, std::round(sc<float>(monitor->m_size.x) * S), std::round(BARL * S)};
        const auto  NOW     = Time::steadyNow();
        const float ELAPSED = std::chrono::duration<float, std::milli>(NOW - st.barSince).count();
        const float IN      = std::clamp(ELAPSED / FADE_IN_MS, 0.F, 1.F);
        const float DWELL   = st.hoverSince ? std::clamp(std::chrono::duration<float, std::milli>(NOW - *st.hoverSince).count() / BAR_DWELL_MS, 0.F, 1.F) : 0.F;

        const int PHASE = sc<int>(std::fmod(ELAPSED, DRIFT_MS) / DRIFT_MS * PHASES);
        if (const auto STRIPES = stripeFrame(monitor, SURF_BAR, Vector2D{BAR.width, BAR.height}, PHASE); STRIPES) {
            CTexPassElement::SRenderData d;
            d.tex = STRIPES;
            d.box = BAR;
            d.a   = IN * (BAR_ALPHA + (1.F - BAR_ALPHA) * DWELL);
            g_pHyprRenderer->m_renderPass.add(makeUnique<CTexPassElement>(d));
        }

        // The words: after the left cluster, in the theme's text colour.
        auto& scan = g_barScan[monitor->m_id];
        if (!scan.valid || std::chrono::duration<float, std::milli>(NOW - scan.at).count() >= BAR_SCAN_MS) {
            scan    = scanBar(topBar(monitor));
            scan.at = NOW;
        }
        const int  FONT = std::max(1, sc<int>(std::round(std::min(BAR_FONT_MAXL, BARL * 0.5F) * S)));
        const auto TEX  = OverviewRender::text(BAR_LABEL, Config::themeForeground(), FONT, false, "", 0, 500);
        if (TEX && TEX->ok()) {
            const float TW = sc<float>(TEX->m_size.x), TH = sc<float>(TEX->m_size.y);
            float       x  = scan.leftEndL >= 0.F ? (scan.leftEndL + BAR_TEXT_GAPL) * S : BAR.width * BAR_FALLBACK_X;
            // Never into the next cluster: nudge back, and give up if even the
            // bare gap is too narrow for the words.
            const bool FITS = scan.nextL < 0.F || x + TW <= (scan.nextL - BAR_TEXT_GAPL) * S;
            if (FITS) {
                CTexPassElement::SRenderData d;
                d.tex = TEX;
                d.box = CBox{std::round(x), std::round((BAR.height - TH) / 2.0), TW, TH};
                d.a   = IN * (0.55F + 0.45F * DWELL);
                g_pHyprRenderer->m_renderPass.add(makeUnique<CTexPassElement>(d));
            }
        }

        st.barL = CBox{sc<double>(monitor->m_position.x), sc<double>(monitor->m_position.y), sc<double>(monitor->m_size.x), BARL};
    }

    void render(PHLMONITOR monitor) {
        if (!monitor)
            return;

        renderBar(monitor);

        auto&      st   = g_state[monitor->m_id];
        const bool WANT = dragging() && shownScratchpad(monitor);
        if (WANT != st.shown) {
            st.shown = WANT;
            st.since = Time::steadyNow();
            if (WANT)
                armTick();
        }

        // The box is NOT cleared here when the zone goes: onPointerMove damages
        // it once more to erase the band, then clears it. Cleared here, nothing
        // would repaint that area and the band would stay as a ghost.
        if (!st.shown)
            return;

        const auto  NOW     = Time::steadyNow();
        const float ELAPSED = std::chrono::duration<float, std::milli>(NOW - st.since).count();
        const float ALPHA   = std::clamp(ELAPSED / FADE_IN_MS, 0.F, 1.F);

        const float S    = sc<float>(monitor->m_scale);
        const float CX   = sc<float>(monitor->m_size.x) * S / 2.F;
        // At the very top, over the bar (drawn at RENDER_LAST_MOMENT for that),
        // so the band hangs as little as it can into the window underneath --
        // the drop the user might really have meant for that window is still
        // there below it (user, 2026-09-24).
        const float TOPL = TOP_GAPL;

        const CBox  LABEL0 = OverviewRender::labelPillBox(monitor, LABEL, CX, 0.0, S);
        if (LABEL0.width < 1)
            return;

        const float W = std::round(std::max(BAND_MIN_WL * S, sc<float>(LABEL0.width) + 2.F * BAND_SIDE_WL * S));
        const float H = std::round(BAND_HL * S);
        CBox        band{std::round(CX - W / 2.F), std::round(TOPL * S), W, H};
        const int   ROUND = sc<int>(std::round(H / 2.F));

        OverviewRender::themePlate(band, ROUND, 2.F, 0.92F * ALPHA, false);

        const int PHASE = sc<int>(std::fmod(ELAPSED, DRIFT_MS) / DRIFT_MS * PHASES);
        if (const auto STRIPES = stripeFrame(monitor, SURF_ZONE, Vector2D{W, H}, PHASE); STRIPES) {
            CTexPassElement::SRenderData d;
            d.tex           = STRIPES;
            d.box           = band;
            d.a             = ALPHA;
            d.round         = ROUND;
            d.roundingPower = 2.F;
            g_pHyprRenderer->m_renderPass.add(makeUnique<CTexPassElement>(d));
        }

        // The label on its own plate, centred on the band.
        OverviewRender::labelPill(monitor, LABEL, CX, sc<float>(band.y) + (H + sc<float>(LABEL0.height)) / 2.F, ALPHA, S);

        st.zoneL = CBox{sc<double>(monitor->m_position.x) + (band.x / S), sc<double>(monitor->m_position.y) + (band.y / S), band.width / S, band.height / S}
                       .expand(HIT_MARGINL);
    }

    void onPointerMove(PHLMONITOR monitor) {
        updateBarHover();

        // Repaint every zone that is up -- the fade and the drag both move --
        // and the one that just went away, so it is erased. Motion is what
        // drives a drag, so it is also what drives these frames.
        for (auto& [id, st] : g_state) {
            if (st.zoneL.width >= 1) {
                g_pHyprRenderer->damageBox(st.zoneL);
                if (!st.shown)
                    st.zoneL = {};
            }
        }

        if (!monitor || !dragging())
            return;

        const auto SPECIAL = shownScratchpad(monitor);
        if (!SPECIAL)
            return;

        auto& st = g_state[monitor->m_id];

        // First motion of the drag: nothing has been drawn yet, so there is no
        // box to damage. Buy the frame that draws it.
        if (st.zoneL.width < 1) {
            g_pHyprRenderer->damageMonitor(monitor);
            return;
        }

        if (st.zoneL.containsPoint(g_pInputManager->getMouseCoordsInternal())) {
            // Out of the way. The drag continues over whatever was underneath;
            // the zone goes with the scratchpad on the next frame.
            Scratchpad::hide(SPECIAL);
            g_pHyprRenderer->damageMonitor(monitor);
        }
    }

    void shutdown() {
        if (g_tick) {
            wl_event_source_remove(g_tick);
            g_tick = nullptr;
        }
        g_stripes.clear();
        g_state.clear();
        g_barScan.clear();
    }

}
