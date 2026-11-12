// A TRIP NEVER SHOWS AN UNDRAWN PICTURE (TripFrame.hpp, TripFrameRule.hpp).
//
// How it draws: Hyprland's own snapshot of the window (makeSnapshotFB, the one
// its close fade uses: the output's size, the window drawn where it is) is
// taken in hold(), outside any render pass. While the hold lasts, each frame
// that draws the window (RENDER_PRE_WINDOW .. RENDER_POST_WINDOW, the pass it
// queued into) has its MAIN SURFACE element swapped for one that draws the
// snapshot's window rectangle into that element's box -- the box the trip
// moved the window to this frame -- with the same rounding and alpha; its
// subsurfaces go (they are in the snapshot), its popups, border and shadow
// stay. Everything is decided outside the render pass: the commit listener and
// a short timer release the hold and damage the window; a render only reads.
// (The standard headers before `#define private public`: <any> and the like
// declare private members of their own.)
#include <algorithm>
#include <any>
#include <array>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <format>
#include <optional>
#include <string>
#include <vector>
#define private public
#define protected public
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/pass/Pass.hpp>
#include <hyprland/src/render/pass/SurfacePassElement.hpp>
#include <hyprland/src/render/pass/TransformedWindowPassElement.hpp>
#undef protected
#undef private
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Framebuffer.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/protocols/core/Compositor.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/Compositor.hpp>
#include "TripFrame.hpp"
#include "TripFrameRule.hpp"
#include "ConfigureHold.hpp"
#include "DebugPaths.hpp"
#include "IOverview.hpp"

namespace Hyprdrive::TripFrame {
    namespace {
        using Clock = std::chrono::steady_clock;

        struct SHeld {
            PHLWINDOWREF             win;
            PHLMONITORREF            mon;
            SP<Render::IFramebuffer> fb;
            CBox                     src;      // the window inside the snapshot, monitor px
            Vector2D                 snapSize; // its size then, logical
            Vector2D                 sent;     // the size it was last sent
            Vector2D                 cSize, cPos; // the size and place it had been sent at its last commit
            Clock::time_point        at;       // the hold's start (real time)
            TripFrameRule::SState    state;
            CHyprSignalListener      commit;
            int                      frames = 0, seen = 0; // drawn from the snapshot / frames that drew the window
        };
        std::vector<SHeld> g_held;
        CHyprSignalListener g_stage;
        wl_event_source*    g_timer = nullptr;
        // The frame drawing a held window: the pass it queues into and where
        // its elements start.
        struct SOpen {
            PHLWINDOWREF        win;
            Render::CRenderPass* pass  = nullptr;
            size_t              start = 0;
        } g_open;

        void trace(const std::string& s) {
            if (!DebugPaths::flagOn("hyprdrive-mosaic.on"))
                return;
            if (FILE* const F = DebugPaths::openLog("hyprdrive-mosaic.log")) {
                std::fprintf(F, "%lld tripframe %s\n",
                             (long long)std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count(), s.c_str());
                std::fclose(F);
            }
        }

        bool finiteBox(const CBox& b) {
            return std::isfinite(b.x) && std::isfinite(b.y) && std::isfinite(b.w) && std::isfinite(b.h) && b.w >= 2 && b.h >= 2;
        }
        bool near(const Vector2D& a, const Vector2D& b) {
            return std::abs(a.x - b.x) <= 1.0 && std::abs(a.y - b.y) <= 1.0;
        }
        double msSince(const Clock::time_point& t) {
            return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
        }
        // The size the client was last TOLD. Not getReportedSize(): for an
        // X11 window that is the size of its last COMMIT (commitWindow copies
        // the pending size over), so a client that redraws late seemed never
        // to have been sent the trip's size (TripFrameRule.hpp).
        Vector2D sentSize(const PHLWINDOW& w) {
            return w->m_pendingReportedSize.x > 0 && w->m_pendingReportedSize.y > 0 ? w->m_pendingReportedSize : w->getReportedSize();
        }
        SHeld* find(const PHLWINDOW& w) {
            for (auto& H : g_held)
                if (H.win.lock() == w)
                    return &H;
            return nullptr;
        }

        // Draws the snapshot's window rectangle into `box` (monitor px).
        class CTripFramePassElement : public IPassElement {
          public:
            struct SData {
                SP<Render::ITexture> tex;
                Vector2D             texSize; // the snapshot's px
                CBox                 src, box, clip;
                float                alpha = 1.F;
                int                  round = 0;
                float                roundingPower = 2.F;
            };
            explicit CTripFramePassElement(const SData& d) : m_d(d) {}
            std::vector<UP<IPassElement>> draw() override {
                const auto MON = g_pHyprRenderer->m_renderData.pMonitor.lock();
                if (!MON || !m_d.tex || !m_d.tex->ok() || !finiteBox(m_d.box) || !finiteBox(m_d.src) || m_d.texSize.x < 2 || m_d.texSize.y < 2)
                    return {};
                CRegion damage{g_pHyprRenderer->m_renderData.damage};
                damage.intersect(m_d.box);
                if (damage.empty())
                    return {};
                const auto SAVEDCLIP = g_pHyprRenderer->m_renderData.clipBox;
                if (!m_d.clip.empty())
                    g_pHyprRenderer->m_renderData.clipBox = m_d.clip;
                g_pHyprRenderer->pushMonitorTransformEnabled(true);
                Render::GL::g_pHyprOpenGL->renderTexture(m_d.tex, m_d.box,
                                                         Render::GL::CHyprOpenGLImpl::STextureRenderData{
                                                             .damage                      = &damage,
                                                             .a                           = m_d.alpha,
                                                             .round                       = m_d.round,
                                                             .roundingPower               = m_d.roundingPower,
                                                             .allowCustomUV               = true,
                                                             .allowDim                    = false,
                                                             .primarySurfaceUVTopLeft     = m_d.src.pos() / m_d.texSize,
                                                             .primarySurfaceUVBottomRight = (m_d.src.pos() + m_d.src.size()) / m_d.texSize,
                                                         });
                g_pHyprRenderer->popMonitorTransformEnabled();
                g_pHyprRenderer->m_renderData.clipBox = SAVEDCLIP;
                return {};
            }
            ePassElementType type() override {
                return EK_CUSTOM;
            }
            bool needsLiveBlur() override {
                return false;
            }
            bool needsPrecomputeBlur() override {
                return false;
            }
            std::optional<CBox> boundingBox() override {
                const auto MON = g_pHyprRenderer->m_renderData.pMonitor.lock();
                if (!MON || MON->m_scale <= 0)
                    return std::nullopt;
                return m_d.box.copy().scale(1.F / MON->m_scale).round();
            }
            CRegion opaqueRegion() override {
                return {};
            }
            const char* passName() override {
                return PASS_NAME;
            }
            static constexpr const char* PASS_NAME = "CTripFramePassElement";

          private:
            SData m_d;
        };

        // The hold ends: the live window is drawn again from the next frame.
        void release(std::vector<SHeld>::iterator it, const char* why) {
            const auto W = it->win.lock();
            if (W)
                trace(std::format("{} live again after {:.0f} ms: {} ({} commit(s) at {}x{}; {} of {} frames drawn from the snapshot)", W->m_class, msSince(it->at), why,
                                  it->state.drawn, it->sent.x, it->sent.y, it->frames, it->seen));
            g_held.erase(it);
            if (W && W->m_isMapped)
                g_pHyprRenderer->damageWindow(W, true);
        }

        // Every hold, looked at again: the size it was sent, and the rule.
        void review() {
            for (auto it = g_held.begin(); it != g_held.end();) {
                const auto W = it->win.lock();
                if (!W || !W->m_isMapped || !W->wlSurface() || !W->wlSurface()->resource()) {
                    it = g_held.erase(it);
                    continue;
                }
                const Vector2D SENT = sentSize(W);
                TripFrameRule::sent(it->state, !near(SENT, it->snapSize), !near(SENT, it->sent));
                it->sent = SENT;
                const auto R = TripFrameRule::decide(it->state, msSince(it->at), ConfigureHold::waiting(W));
                if (R != TripFrameRule::ERelease::Hold) {
                    release(it, TripFrameRule::why(R));
                    it = g_held.begin(); // (released: start over, cheap -- one or two holds)
                    continue;
                }
                ++it;
            }
        }

        int onTimer(void*) {
            review();
            if (g_timer && !g_held.empty())
                wl_event_source_timer_update(g_timer, 25);
            return 0;
        }
        void armTimer() {
            if (!g_timer && g_pCompositor)
                g_timer = wl_event_loop_add_timer(g_pCompositor->m_wlEventLoop, onTimer, nullptr);
            if (g_timer)
                wl_event_source_timer_update(g_timer, 25);
        }

        // How much of the surface this commit repainted (0..1): a frame the
        // client DREW repaints all of it; X / XWayland also commit for
        // other reasons (a frame callback, an exposure elsewhere), with
        // little or no damage -- those are not a frame.
        double repainted(const SP<CWLSurfaceResource>& s) {
            const auto AREA = [](const CRegion& r) {
                double a = 0;
                for (const auto& R : r.getRects())
                    a += double(R.x2 - R.x1) * double(R.y2 - R.y1);
                return a;
            };
            const auto&  C  = s->m_current;
            const double SA = C.size.x * C.size.y, BA = C.bufferSize.x * C.bufferSize.y;
            double       f  = 0;
            if (SA > 0)
                f = std::max(f, AREA(C.damage.copy().intersect(CBox{{}, C.size})) / SA);
            if (BA > 0)
                f = std::max(f, AREA(C.bufferDamage.copy().intersect(CBox{{}, C.bufferSize})) / BA);
            return f;
        }

        void onCommit(const PHLWINDOWREF& ref) {
            const auto W = ref.lock();
            SHeld*     H = W ? find(W) : nullptr;
            if (!H || !W->wlSurface() || !W->wlSurface()->resource())
                return;
            const Vector2D SENT = sentSize(W);
            TripFrameRule::sent(H->state, !near(SENT, H->snapSize), !near(SENT, H->sent));
            H->sent = SENT;
            const auto RES = W->wlSurface()->resource();
            const bool AT  = near(RES->m_current.size, SENT);
            const double REP = repainted(RES);
            // Moved since its last commit, its size the same: the X server's
            // re-commit of what it has (TripFrameRule.hpp).
            const bool MOVED = W->m_isX11 && near(SENT, H->cSize) && !near(W->m_reportedPosition, H->cPos);
            H->cSize         = SENT;
            H->cPos          = W->m_reportedPosition;
            if (AT && H->state.resized)
                trace(std::format("{} commit at {}x{} after {:.0f} ms, {:.0f}% repainted{}", W->m_class, SENT.x, SENT.y, msSince(H->at), REP * 100,
                                  MOVED ? " (only moved since its last: not a frame)" : ""));
            TripFrameRule::committed(H->state, msSince(H->at), AT && REP >= TripFrameRule::FULL_REPAINT, MOVED);
            review();
        }

        // The window's surface elements from `start` on: the main one drawn
        // from the snapshot, the subsurfaces gone (they are in it). A window
        // with a transformer (the frost of a moving window: FlightBlur) has
        // them in its own pass, inside its CTransformedWindowPassElement.
        bool swapSurfaces(std::vector<Render::CRenderPass::SPassElementData>& els, size_t start, const PHLWINDOW& w, const SHeld& h, const PHLMONITOR& mon) {
            bool swapped = false;
            for (size_t i = std::min(start, els.size()); i < els.size();) {
                auto* const E = els[i].element.get();
                if (E && E->type() == EK_TRANSFORMED_WINDOW) {
                    if (auto* const T = dynamic_cast<CTransformedWindowPassElement*>(E); T && T->m_data.pass)
                        swapped |= swapSurfaces(T->m_data.pass->m_passElements, 0, w, h, mon);
                    ++i;
                    continue;
                }
                auto* const SURF = E && E->type() == EK_SURFACE ? dynamic_cast<CSurfacePassElement*>(E) : nullptr;
                if (!SURF || SURF->m_data.pWindow != w || SURF->m_data.popup) {
                    ++i;
                    continue;
                }
                if (!SURF->m_data.mainSurface) {
                    els.erase(els.begin() + sc<std::ptrdiff_t>(i)); // a subsurface: in the snapshot
                    continue;
                }
                const auto& D   = SURF->m_data;
                const CBox  BOX = CBox{(D.pos - mon->m_position) * mon->m_scale, Vector2D{D.w, D.h} * mon->m_scale};
                if (!finiteBox(BOX)) {
                    ++i;
                    continue;
                }
                CTripFramePassElement::SData data{.tex           = h.fb->getTexture(),
                                                  .texSize       = h.fb->m_size,
                                                  .src           = h.src,
                                                  .box           = BOX,
                                                  .clip          = D.clipBox,
                                                  .alpha         = D.alpha * D.fadeAlpha,
                                                  .round         = D.dontRound ? 0 : D.rounding,
                                                  .roundingPower = D.roundingPower};
                els[i].element = makeUnique<CTripFramePassElement>(data);
                swapped        = true;
                ++i;
            }
            return swapped;
        }

        // RENDER_PRE_WINDOW / RENDER_POST_WINDOW: the window's main surface,
        // drawn from the snapshot.
        void onStage(eRenderStage stage) {
            if (g_held.empty() || g_pHyprRenderer->m_bRenderingSnapshot)
                return;
            if (stage == RENDER_PRE_WINDOW) {
                const auto W = g_pHyprRenderer->m_renderData.currentWindow.lock();
                g_open       = {};
                if (SHeld* H = W ? find(W) : nullptr) {
                    ++H->seen;
                    g_open = {.win = W, .pass = &g_pHyprRenderer->currentPass(), .start = g_pHyprRenderer->currentPass().m_passElements.size()};
                }
                return;
            }
            if (stage != RENDER_POST_WINDOW)
                return;
            const auto W = g_open.win.lock();
            auto*      PASS = g_open.pass;
            const auto START = g_open.start;
            g_open           = {};
            if (!W || !PASS || PASS != &g_pHyprRenderer->currentPass() || W != g_pHyprRenderer->m_renderData.currentWindow.lock())
                return;
            SHeld*     H   = find(W);
            const auto MON = g_pHyprRenderer->m_renderData.pMonitor.lock();
            if (!H || !MON || H->mon.lock() != MON || !H->fb || !H->fb->getTexture() || !H->fb->getTexture()->ok() || openScrollOverviewForMonitor(MON))
                return;
            if (msSince(H->at) >= TripFrameRule::TIMEOUT_MS) // its timer is a few ms late: never past the timeout
                return;
            if (swapSurfaces(PASS->m_passElements, START, W, *H, MON))
                ++H->frames;
        }
    }

    bool holding(const PHLWINDOW& w) {
        return w && find(w);
    }

    void hold(const PHLWINDOW& w) {
        if (!w || !w->m_isMapped || !w->m_workspace || !g_pHyprRenderer || !w->wlSurface() || !w->wlSurface()->resource())
            return;
        const auto MON = w->m_monitor.lock();
        if (!MON || MON->m_scale <= 0 || openScrollOverviewForMonitor(MON))
            return;
        // Already held (a quick toggle back before it drew): the snapshot it
        // has IS the last good frame -- what is on screen now is not.
        if (SHeld* H = find(w)) {
            H->at    = Clock::now();
            H->state = {};
            H->sent  = sentSize(w);
            H->cSize = H->sent;
            H->cPos  = w->m_reportedPosition;
            trace(std::format("{} held again (it has not drawn since)", w->m_class));
            armTimer();
            return;
        }
        // Where renderWindow draws it (Renderer.cpp renderWindow), in the
        // snapshot's monitor px.
        const Vector2D POS = w->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT) + (w->m_pinned ? Vector2D{} : w->m_workspace->m_renderOffset->value()) +
            w->m_floatingOffset;
        const Vector2D SIZE = w->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
        const CBox     SRC{(POS - MON->m_position) * MON->m_scale, SIZE * MON->m_scale};
        if (!finiteBox(SRC))
            return;
        auto fb = g_pHyprRenderer->makeSnapshotFB(w);
        if (!fb || !fb->isAllocated() || !fb->getTexture() || !fb->getTexture()->ok() || fb->m_size.x < 2 || fb->m_size.y < 2)
            return;
        g_held.push_back({.win = w, .mon = MON, .fb = std::move(fb), .src = SRC, .snapSize = SIZE, .sent = sentSize(w), .cSize = sentSize(w), .cPos = w->m_reportedPosition, .at = Clock::now()});
        const PHLWINDOWREF REF = w;
        g_held.back().commit   = w->wlSurface()->resource()->m_events.commit.listen([REF] { onCommit(REF); });
        trace(std::format("{} held at {}x{} (its last good frame)", w->m_class, SIZE.x, SIZE.y));
        armTimer();
    }

    void init() {
        g_stage = Event::bus()->m_events.render.stage.listen([](eRenderStage stage) { onStage(stage); });
    }

    void shutdown() {
        g_stage.reset();
        g_held.clear();
        g_open = {};
        if (g_timer) {
            wl_event_source_remove(g_timer);
            g_timer = nullptr;
        }
        // Its pass element type lives in this object: none may outlive it.
        // (A transformed window keeps its elements in a pass of its own.)
        if (g_pHyprRenderer) {
            for (auto& E : g_pHyprRenderer->m_renderPass.m_passElements)
                if (E.element && E.element->type() == EK_TRANSFORMED_WINDOW)
                    if (auto* const T = dynamic_cast<CTransformedWindowPassElement*>(E.element.get()); T && T->m_data.pass)
                        T->m_data.pass->removeAllOfType(CTripFramePassElement::PASS_NAME);
            g_pHyprRenderer->m_renderPass.removeAllOfType(CTripFramePassElement::PASS_NAME);
        }
    }
}
