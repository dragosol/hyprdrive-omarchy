// A TRIP NEVER SHOWS AN UNDRAWN PICTURE (TripFrame.hpp, TripFrameRule.hpp).
//
// How it draws: Hyprland's own snapshot of the window (makeSnapshotFB, the one
// its close fade uses: the output's size, the window drawn where it is) is
// taken in hold(), outside any render pass. While the hold lasts, each frame
// that draws the window (RENDER_PRE_WINDOW .. RENDER_POST_WINDOW, the pass it
// queued into) has its MAIN SURFACE element swapped for one that draws the
// snapshot's window rectangle into that element's box -- the box the trip
// moved the window to this frame -- with the same rounding and alpha; its
// subsurfaces are not drawn (they are in the snapshot), its popups, border
// and shadow stay. Each surface is still told it was presented (discarded),
// so the client goes on drawing. While the live picture is shown instead (TripFrameRule.hpp, phases A
// and C), each of its surfaces -- the subsurfaces too: a video can be one --
// is drawn stretched into the box the same way: its whole buffer, placed and
// sized by the box over the main surface's own size. (Hyprland would crop a
// buffer bigger than the box and smear one smaller.) Everything is decided
// outside the render pass: the commit listeners, render.pre and a short timer
// release the hold and damage the window; a render only reads.
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
#include <hyprland/src/render/pass/ClearPassElement.hpp>
#undef protected
#undef private
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Framebuffer.hpp>
#include <hyprland/src/desktop/view/window/Window.hpp>
#include "HyprCompat.hpp"
#include <hyprland/src/workspace/HLWorkspace.hpp>
#include <hyprland/src/protocols/core/Compositor.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/Compositor.hpp>
#include "TripFrame.hpp"
#include "TripFrameRule.hpp"
#include "ConfigureHold.hpp"
#include "FullscreenDesktop.hpp"
#include "MotionClock.hpp"
#include "MotionCurves.hpp"
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
            CHyprSignalListener      commit, precommit;
            // A native Wayland window's subsurfaces: a frame can come from
            // one (TripFrameRule.hpp); `old` its size when held.
            struct SSub {
                WP<CWLSurfaceResource> surf;
                Vector2D               old; // {}: made during the hold
                CHyprSignalListener    commit, precommit;
                double                 rep = 0; // what its commits since its last commit event repainted (pendingRepaint)
            };
            Vector2D                 cBuf; // the main surface's size at its last commit
            double                   rep = 0; // the main surface's, as SSub::rep
            std::vector<SSub>        subs;
            int                      frames = 0, seen = 0, live = 0; // drawn from the snapshot / frames that drew the window / ... live
            // The phases (TripFrameRule.hpp, "THE PICTURE STRETCHES").
            Vector2D          oldSurf;            // its main surface's size when held: its old frames
            bool              fresh      = false; // it has committed at another size since
            bool              good       = true;  // its current picture is a drawn one
            bool              boxResized = false; // its box has changed size during the hold
            double            landedMs   = 0;     // when the box last stopped changing size (ms since the hold); < 0 while it changes
            bool              released   = false; // the rule let it go: the live picture fades in
            bool              fading     = false;
            Clock::time_point fadeAt;
            double            fadeMs = 0;
            double            weight = 0; // the live picture's, this frame: 0 the snapshot alone, 1 the live one alone
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
            const auto& PENDING = Hyprdrive::Priv::pendingReportedSize(*w);
            return PENDING && PENDING->x > 0 && PENDING->y > 0 ? *PENDING : w->backend().reportedSize();
        }
        // X11 only: the position last sent in a configure; a Wayland window has none.
        Vector2D reportedPosition(const PHLWINDOW& w) {
            return w->backend().isX11() ? sc<Desktop::View::CX11Backend&>(w->backend()).m_reportedPosition : Vector2D{};
        }
        SHeld* find(const PHLWINDOW& w) {
            for (auto& H : g_held)
                if (H.win.lock() == w)
                    return &H;
            return nullptr;
        }

        // A texture's rectangle `src` (its px) drawn into `box` (monitor px).
        struct SLayer {
            SP<Render::ITexture> tex;
            Vector2D             texSize;
            CBox                 src, box;
            float                alpha = 1.F;
            int                  round = 0;
            float                roundingPower = 2.F;
        };
        // A surface's whole picture (its viewport's source, if it has one),
        // into `box`.
        SLayer surfaceLayer(const SP<CWLSurfaceResource>& s, const SP<Render::ITexture>& tex, const CBox& box, float alpha, int round, float roundingPower) {
            const auto& C = s->m_current;
            return {.tex           = tex ? tex : C.texture,
                    .texSize       = C.bufferSize,
                    .src           = C.viewport.hasSource ? C.viewport.source : CBox{{}, C.bufferSize},
                    .box           = box,
                    .alpha         = alpha,
                    .round         = round,
                    .roundingPower = roundingPower};
        }

        // Draws its layers in order: the snapshot's window rectangle, or a
        // live surface, stretched into the box the trip moves it through.
        class CTripFramePassElement : public IPassElement {
          public:
            struct SData {
                std::vector<SLayer> layers;
                CBox                clip;
                // The surface this element stands in for: told it was
                // presented (or discarded, when only the snapshot shows), as
                // Hyprland's own element does -- its frame callbacks, without
                // which a client stops drawing (Firefox drew one frame per
                // hold, and a video in it froze).
                SP<CWLSurfaceResource> surface;
                PHLMONITORREF          mon;
                Time::steady_tp        when = Time::steadyNow();
                bool                   discarded = false;
            };
            explicit CTripFramePassElement(SData d) : m_d(std::move(d)) {}
            std::vector<UP<IPassElement>> draw(Render::CRenderContext& ctx) override {
                const auto MON = ctx.m_data.pMonitor.lock();
                if (!MON)
                    return {};
                // Inside a transformed window (the frost of a moving window:
                // FlightBlur), Hyprland 0.57 draws the window's elements onto
                // a canvas the size of the window: the damage is the canvas's
                // own (0,0 at the window's corner) and a render modifier moves
                // the boxes onto it. A box is matched against that damage once
                // moved too, and the clip (monitor px) does not apply, as in
                // Hyprland's own surface drawing (ElementRenderer drawSurface).
                // Matching the monitor-px box against the canvas's damage drew
                // only the part of the picture right of the box's own x: a
                // black band from the left, growing as the box moved right.
                const bool CANVAS    = ctx.m_data.renderingTransformedSource;
                const auto SAVEDCLIP = ctx.m_data.clipBox;
                if (!m_d.clip.empty() && !CANVAS)
                    ctx.m_data.clipBox = m_d.clip;
                for (const auto& L : m_d.layers) {
                    if (!L.tex || !L.tex->ok() || !finiteBox(L.box) || !finiteBox(L.src) || L.texSize.x < 2 || L.texSize.y < 2 || L.alpha <= 0.F)
                        continue;
                    CBox target = L.box;
                    if (CANVAS)
                        ctx.m_data.renderModif.applyToBox(target);
                    CRegion damage{ctx.m_data.damage};
                    damage.intersect(target);
                    if (damage.empty())
                        continue;
                    Render::GL::g_pHyprOpenGL->renderTexture(ctx, L.tex, L.box,
                                                             Render::GL::CHyprOpenGLImpl::STextureRenderData{
                                                                 .damage                      = &damage,
                                                                 .a                           = L.alpha,
                                                                 .round                       = L.round,
                                                                 .roundingPower               = L.roundingPower,
                                                                 .allowCustomUV               = true,
                                                                 .allowDim                    = false,
                                                                 .primarySurfaceUVTopLeft     = L.src.pos() / L.texSize,
                                                                 .primarySurfaceUVBottomRight = (L.src.pos() + L.src.size()) / L.texSize,
                                                             });
                }
                ctx.m_data.clipBox = SAVEDCLIP;
                feedback(ctx, m_d.discarded);
                return {};
            }
            void discard(Render::CRenderContext& ctx) override {
                feedback(ctx, true);
            }
            ePassElementType type() override {
                return EK_CUSTOM;
            }
            bool needsLiveBlur(Render::CRenderContext&) override {
                return false;
            }
            bool needsPrecomputeBlur(Render::CRenderContext&) override {
                return false;
            }
            std::optional<CBox> boundingBox(Render::CRenderContext& ctx) override {
                const auto MON = ctx.m_data.pMonitor.lock();
                if (!MON || MON->m_scale <= 0)
                    return std::nullopt;
                std::optional<CBox> all;
                for (const auto& L : m_d.layers)
                    if (finiteBox(L.box))
                        all = all ? CBox{std::min(all->x, L.box.x), std::min(all->y, L.box.y), 0, 0} : L.box;
                if (!all)
                    return std::nullopt;
                double x2 = 0, y2 = 0;
                for (const auto& L : m_d.layers)
                    if (finiteBox(L.box)) {
                        x2 = std::max(x2, L.box.x + L.box.w);
                        y2 = std::max(y2, L.box.y + L.box.h);
                    }
                all->w = x2 - all->x;
                all->h = y2 - all->y;
                return all->scale(1.F / MON->m_scale).round();
            }
            CRegion opaqueRegion(Render::CRenderContext&) override {
                return {};
            }
            const char* passName() override {
                return PASS_NAME;
            }
            static constexpr const char* PASS_NAME = "CTripFramePassElement";

          private:
            void feedback(Render::CRenderContext& ctx, bool discarded) {
                if (const auto MON = m_d.mon.lock(); m_d.surface && MON && !ctx.m_blockSurfaceFeedback)
                    m_d.surface->presentFeedback(m_d.when, MON, discarded);
            }
            SData m_d;
        };

        // The hold ends: Hyprland draws the live window again from the next frame.
        void release(std::vector<SHeld>::iterator it, const std::string& why) {
            const auto W = it->win.lock();
            if (W)
                trace(std::format("{} live again after {:.0f} ms: {} ({} commit(s) at {}x{}; {} of {} frames drawn from the snapshot, {} live; box {:.0f}x{:.0f} of {:.0f}x{:.0f})",
                                  W->metadata().appID(), msSince(it->at), why, it->state.drawn, it->sent.x, it->sent.y, it->frames, it->seen, it->live,
                                  W->sizeAnimation()->value().x, W->sizeAnimation()->value().y, W->sizeAnimation()->goal().x, W->sizeAnimation()->goal().y));
            g_held.erase(it);
            if (W && W->mapped())
                g_pHyprRenderer->damageWindow(W, true);
        }

        void watchSubs(SHeld& h, const PHLWINDOW& w, bool fresh); // (below)

        // Every hold, looked at again: the size it was sent, its box, the
        // rule, and what this frame draws (SHeld::weight).
        void review() {
            for (auto it = g_held.begin(); it != g_held.end();) {
                const auto W = it->win.lock();
                if (!W || !W->mapped() || !W->wlSurface() || !W->wlSurface()->resource()) {
                    it = g_held.erase(it);
                    continue;
                }
                auto&          H    = *it;
                watchSubs(H, W, false); // (a subsurface made meanwhile)
                const double   MS   = msSince(H.at);
                const Vector2D SENT = sentSize(W);
                TripFrameRule::sent(H.state, !near(SENT, H.snapSize), !near(SENT, H.sent));
                H.sent              = SENT;
                double     left     = 0;
                const bool RESIZING = FullscreenDesktop::resizing(W, &left);
                if (RESIZING) {
                    H.boxResized = true;
                    H.landedMs   = -1;
                } else if (H.landedMs < 0) {
                    H.landedMs = MS;
                    trace(std::format("{} box landed after {:.0f} ms at {:.0f}x{:.0f}", W->metadata().appID(), MS, W->sizeAnimation()->value().x, W->sizeAnimation()->value().y));
                }
                const auto R = TripFrameRule::decide(H.state, MS, ConfigureHold::waiting(W), H.landedMs);
                if (R != TripFrameRule::ERelease::Hold && !H.released) {
                    H.released = true;
                    if (H.boxResized && H.weight < 1.0 && Motion::Clock::enabled() && MS < TripFrameRule::CAP_MS) {
                        if (!H.fading) {
                            H.fading = true;
                            H.fadeAt = Clock::now();
                            H.fadeMs = TripFrameRule::fadeMs(!RESIZING, left);
                        }
                        trace(std::format("{} released after {:.0f} ms: {}; fading in over {:.0f} ms{}", W->metadata().appID(), MS, TripFrameRule::why(R), H.fadeMs,
                                          RESIZING ? "" : " (landed)"));
                    }
                }
                // The live picture's weight: a fade under way runs on (a toggle
                // back mid-fade starts from what is on screen); else 1 when it
                // may be shown, 0 when the snapshot stands in for it.
                const bool SHOWN = H.released || TripFrameRule::liveNow(H.state, H.boxResized, H.fresh, H.good);
                if (MS >= TripFrameRule::CAP_MS) {
                    // Never frozen past the cap: live at once.
                    H.fading = false;
                    H.weight = 1.0;
                } else if (SHOWN && H.fading) {
                    const double P = std::clamp(Motion::Clock::animMsBetween<double>(Clock::now(), H.fadeAt) / std::max(H.fadeMs, 1.0), 0.0, 1.0);
                    H.weight       = Motion::smoothstep(P);
                    if (P >= 1.0) {
                        H.fading = false;
                        trace(std::format("{} faded in after {:.0f} ms{}", W->metadata().appID(), MS, RESIZING ? ", its box still changing size" : ""));
                    }
                } else {
                    H.fading = false;
                    H.weight = SHOWN ? 1.0 : 0.0;
                }
                if (TripFrameRule::over(R, MS, H.boxResized, RESIZING, H.weight)) {
                    const std::string WHY = MS >= TripFrameRule::LAST_MS ? std::string{"held as long as a hold may"} :
                        H.boxResized ? std::format("{}, landed and faded in", TripFrameRule::why(R)) : std::string{TripFrameRule::why(R)};
                    release(it, WHY);
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
        double repainted(const SSurfaceState& C) {
            const auto AREA = [](const CRegion& r) {
                double a = 0;
                for (const auto& R : r.getRects())
                    a += double(R.x2 - R.x1) * double(R.y2 - R.y1);
                return a;
            };
            const double SA = C.size.x * C.size.y, BA = C.bufferSize.x * C.bufferSize.y;
            double       f  = 0;
            if (SA > 0)
                f = std::max(f, AREA(C.damage.copy().intersect(CBox{{}, C.size})) / SA);
            if (BA > 0)
                f = std::max(f, AREA(C.bufferDamage.copy().intersect(CBox{{}, C.bufferSize})) / BA);
            return f;
        }
        // ...read from the commit's PENDING state, at its precommit: by its
        // commit event the damage is gone. Hyprland 0.57's own commit
        // handler (it listens first) damages the surface, and that takes the
        // damage (SSurfaceState::accumulateBufferDamage clears the surface
        // damage, and the buffer damage too for a dmabuf buffer); 0.56 moved
        // it into the buffer damage and left it there. Every Firefox page
        // commit read 0% and its hold always waited out SETTLE_MS. `acc` is
        // the most repainted of the commits since the last commit event (a
        // synced subsurface's commits arrive as one); the commit event takes it.
        void pendingRepaint(const SP<CWLSurfaceResource>& s, double& acc) {
            acc = std::max(acc, repainted(s->m_pending));
        }
        double takeRepaint(const SP<CWLSurfaceResource>& s, double& acc) {
            const double R = std::max(acc, repainted(s->m_current));
            acc            = 0;
            return R;
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
            // A native Wayland client's picture newly at the size it was sent
            // (a new buffer, or a new viewport onto its old one) is a whole
            // frame, whatever damage it reports (Chromium keeps its fullscreen
            // buffer, crops it with a viewport and reports only what changed:
            // 17-24% of a window just resized, and its hold waited ~1 s for a
            // full repaint).
            const bool   NEWBUF = !W->backend().isX11() && AT && !near(RES->m_current.size, H->cBuf);
            H->cBuf             = RES->m_current.size;
            const double REP    = takeRepaint(RES, H->rep);
            // Moved since its last commit, its size the same: the X server's
            // re-commit of what it has (TripFrameRule.hpp).
            const bool MOVED = W->backend().isX11() && near(SENT, H->cSize) && !near(reportedPosition(W), H->cPos);
            H->cSize         = SENT;
            H->cPos          = reportedPosition(W);
            // Its first commit at another size than its old one: the end of its
            // old frames (phase A); the snapshot stands in until it has drawn.
            if (!H->fresh && !near(RES->m_current.size, H->oldSurf)) {
                H->fresh = true;
                trace(std::format("{} first commit at {}x{} after {:.0f} ms: the snapshot until it has drawn", W->metadata().appID(), RES->m_current.size.x, RES->m_current.size.y,
                                  msSince(H->at)));
            }
            if (AT && H->state.resized)
                trace(std::format("{} commit at {}x{} after {:.0f} ms, {:.0f}% repainted{}", W->metadata().appID(), SENT.x, SENT.y, msSince(H->at), REP * 100,
                                  MOVED ? " (only moved since its last: not a frame)" : ""));
            TripFrameRule::committed(H->state, msSince(H->at), AT && TripFrameRule::frame(H->state, !W->backend().isX11(), NEWBUF, REP), MOVED, !W->backend().isX11());
            review();
        }

        // A subsurface of a native Wayland window committed: a frame when the
        // window is at the size it was sent, this surface has changed size
        // since the hold (laid out again for it) and it repainted all of it.
        void onSubCommit(const PHLWINDOWREF& ref, const WP<CWLSurfaceResource>& surf) {
            const auto W = ref.lock();
            SHeld*     H = W ? find(W) : nullptr;
            const auto S = surf.lock();
            if (!H || !S || W->backend().isX11() || !W->wlSurface() || !W->wlSurface()->resource())
                return;
            const auto     MAIN = W->wlSurface()->resource();
            const Vector2D SENT = sentSize(W);
            Vector2D       old  = {};
            double         REP  = repainted(S->m_current);
            for (auto& SUB : H->subs)
                if (SUB.surf == surf) {
                    old = SUB.old;
                    REP = takeRepaint(S, SUB.rep);
                }
            const bool AT = near(MAIN->m_current.size, SENT) && !near(S->m_current.size, old);
            if (!H->fresh && old.x > 0 && !near(S->m_current.size, old)) {
                H->fresh = true; // (its precommit did not see it coming)
                trace(std::format("{} a subsurface at {}x{} after {:.0f} ms: the snapshot until it has drawn", W->metadata().appID(), S->m_current.size.x, S->m_current.size.y, msSince(H->at)));
            }
            if (AT && H->state.resized)
                trace(std::format("{} subsurface commit at {}x{} (window {}x{}) after {:.0f} ms, {:.0f}% repainted", W->metadata().appID(), S->m_current.size.x, S->m_current.size.y,
                                  SENT.x, SENT.y, msSince(H->at), REP * 100));
            TripFrameRule::committed(H->state, msSince(H->at), AT && TripFrameRule::frame(H->state, true, false, REP), false, true);
            review();
        }
        // Every subsurface of a native Wayland window watched (`fresh`: their
        // sizes now are the old ones -- a new hold; else only new ones are
        // added, with no old size: any size is a new one).
        void onSubPrecommit(const PHLWINDOWREF& ref, const WP<CWLSurfaceResource>& surf); // (below)
        void watchSubs(SHeld& h, const PHLWINDOW& w, bool fresh) {
            if (w->backend().isX11() || !w->wlSurface() || !w->wlSurface()->resource())
                return;
            const auto MAIN = w->wlSurface()->resource();
            std::erase_if(h.subs, [](const SHeld::SSub& s) { return !s.surf; });
            if (fresh)
                for (auto& SUB : h.subs)
                    SUB.old = SUB.surf->m_current.size;
            const PHLWINDOWREF REF = w;
            MAIN->breadthfirst(
                [&](SP<CWLSurfaceResource> s, const Vector2D&, void*) {
                    if (s == MAIN || std::ranges::any_of(h.subs, [&](const SHeld::SSub& x) { return x.surf == s; }))
                        return;
                    const WP<CWLSurfaceResource> WS = s;
                    h.subs.push_back({.surf = s, .old = fresh ? s->m_current.size : Vector2D{}});
                    h.subs.back().commit    = s->m_events.commit.listen([REF, WS] { onSubCommit(REF, WS); });
                    h.subs.back().precommit = s->m_events.precommit.listen([REF, WS] { onSubPrecommit(REF, WS); });
                },
                nullptr);
        }

        // The snapshot again, from the window's current (old, drawn) frame: its
        // surfaces at their own size and place, as the client drew them --
        // not as the travelling box would show them -- fitted into the
        // monitor's px. Outside any render pass, as makeSnapshotFB.
        bool resnap(SHeld& h, const PHLWINDOW& w) {
            const auto MON  = h.mon.lock();
            const auto MAIN = w->wlSurface() ? w->wlSurface()->resource() : nullptr;
            if (!MON || !MAIN || MON->m_scale <= 0 || MON->m_pixelSize.x < 2 || MON->m_pixelSize.y < 2 || g_pHyprRenderer->context().m_renderingSnapshot)
                return false;
            const Vector2D NAT = MAIN->m_current.size;
            if (NAT.x < 2 || NAT.y < 2)
                return false;
            const double        K = MON->m_scale * std::min({1.0, MON->m_pixelSize.x / (NAT.x * MON->m_scale), MON->m_pixelSize.y / (NAT.y * MON->m_scale)});
            std::vector<SLayer> layers;
            MAIN->breadthfirst(
                [&](SP<CWLSurfaceResource> s, const Vector2D& off, void*) {
                    if (s->m_current.texture && s->m_current.size.x >= 1 && s->m_current.size.y >= 1)
                        layers.push_back(surfaceLayer(s, nullptr, CBox{off * K, s->m_current.size * K}, 1.F, 0, 2.F));
                },
                nullptr);
            if (layers.empty())
                return false;
            auto fb = g_pHyprRenderer->createFB("hyprdrive trip frame");
            if (!fb)
                return false;
            fb->alloc(MON->m_pixelSize.x, MON->m_pixelSize.y, DRM_FORMAT_ABGR8888);
            fb->setImageDescription(MON->workBufferImageDescription());
            CRegion fake{0, 0, sc<int>(MON->m_transformedSize.x), sc<int>(MON->m_transformedSize.y)};
            if (!g_pHyprRenderer->beginFullFakeRender(MON, fake, fb))
                return false;
            auto& CTX               = g_pHyprRenderer->context();
            CTX.m_renderingSnapshot = true;
            g_pHyprRenderer->draw(CTX, CClearPassElement::SClearData{CHyprColor(0, 0, 0, 0)});
            g_pHyprRenderer->startRenderPass(CTX);
            Render::IHyprRenderer::addPassElement(CTX, makeUnique<CTripFramePassElement>(CTripFramePassElement::SData{.layers = std::move(layers)}));
            g_pHyprRenderer->endRender();
            if (!fb->isAllocated() || !fb->getTexture() || !fb->getTexture()->ok())
                return false;
            h.fb  = std::move(fb);
            h.src = CBox{{}, NAT * K};
            return true;
        }

        // About to commit another size than its old one, its old frame still
        // the current one: that frame is the snapshot (the last good one).
        void onPrecommit(const PHLWINDOWREF& ref) {
            const auto W = ref.lock();
            SHeld*     H = W ? find(W) : nullptr;
            if (!H || !W->wlSurface() || !W->wlSurface()->resource())
                return;
            const auto RES = W->wlSurface()->resource();
            pendingRepaint(RES, H->rep);
            if (H->fresh || !H->good)
                return;
            if (!RES->m_pending.updated.bits.buffer || !RES->m_pending.buffer || near(RES->m_pending.size, H->oldSurf) || !near(RES->m_current.size, H->oldSurf))
                return;
            if (resnap(*H, W))
                trace(std::format("{} snapshot again from its last {}x{} frame, {:.0f} ms in", W->metadata().appID(), RES->m_current.size.x, RES->m_current.size.y, msSince(H->at)));
        }
        // ...or a subsurface of a native Wayland window first: Firefox's page
        // takes its new size ~6 ms before the window does. Every surface is
        // still at its old size here, so the snapshot is taken now, and the
        // old frames end (phase A): the snapshot taken at the window's own
        // commit had the page at its new size in the old window -- cropped
        // in a corner of black, the fade's underlay leaving fullscreen.
        void onSubPrecommit(const PHLWINDOWREF& ref, const WP<CWLSurfaceResource>& surf) {
            const auto W = ref.lock();
            SHeld*     H = W ? find(W) : nullptr;
            const auto S = surf.lock();
            if (!H || !S || !W->wlSurface() || !W->wlSurface()->resource())
                return;
            Vector2D old = {};
            for (auto& SUB : H->subs)
                if (SUB.surf == surf) {
                    old = SUB.old;
                    pendingRepaint(S, SUB.rep);
                }
            if (H->fresh)
                return;
            if (old.x <= 0 || !S->m_pending.updated.bits.buffer || !S->m_pending.buffer || near(S->m_pending.size, old) || !near(S->m_current.size, old))
                return;
            const auto MAIN = W->wlSurface()->resource();
            if (H->good && near(MAIN->m_current.size, H->oldSurf) && resnap(*H, W))
                trace(std::format("{} snapshot again from its last {}x{} frame, {:.0f} ms in (a subsurface's new size first)", W->metadata().appID(), MAIN->m_current.size.x,
                                  MAIN->m_current.size.y, msSince(H->at)));
            H->fresh = true;
            trace(std::format("{} a subsurface goes to {}x{} after {:.0f} ms: the snapshot until it has drawn", W->metadata().appID(), S->m_pending.size.x, S->m_pending.size.y, msSince(H->at)));
        }

        // The window's surface elements from `start` on: the main one drawn
        // from the snapshot, the live picture over it at its weight; the
        // subsurfaces drawn only live (the snapshot has them). A window
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
                const auto&  D     = SURF->m_data;
                const double LIVE  = h.weight;
                const float  ALPHA = D.alpha * D.fadeAlpha;
                const int    ROUND = D.dontRound ? 0 : D.rounding;
                const CBox   BOX   = CBox{(D.pos - mon->m_position) * mon->m_scale, Vector2D{D.w, D.h} * mon->m_scale};
                if (!SURF->m_data.mainSurface) {
                    // A subsurface: in the snapshot; live, stretched with the
                    // main surface -- its place and size by the box over the
                    // main surface's own size.
                    const auto MAIN = w->wlSurface()->resource();
                    CTripFramePassElement::SData data{.clip = D.clipBox, .surface = D.surface, .mon = mon, .when = D.when, .discarded = true};
                    if (LIVE > 0.0 && D.surface && MAIN && MAIN->m_current.size.x >= 1 && MAIN->m_current.size.y >= 1 && finiteBox(BOX)) {
                        const Vector2D S = Vector2D{D.w, D.h} / MAIN->m_current.size;
                        const CBox     SUB{(D.pos + D.localPos * S - mon->m_position) * mon->m_scale, D.surface->m_current.size * S * mon->m_scale};
                        data.layers    = {surfaceLayer(D.surface, D.texture, SUB, ALPHA * sc<float>(LIVE), ROUND, D.roundingPower)};
                        data.discarded = false;
                    }
                    els[i].element = makeUnique<CTripFramePassElement>(std::move(data));
                    swapped = true;
                    ++i;
                    continue;
                }
                if (!finiteBox(BOX)) {
                    ++i;
                    continue;
                }
                // The snapshot under (opaque, the window's own alpha), the live
                // picture over it at its weight: no dip in between.
                std::vector<SLayer> layers;
                if (LIVE < 1.0)
                    layers.push_back({.tex = h.fb->getTexture(), .texSize = h.fb->m_size, .src = h.src, .box = BOX, .alpha = ALPHA, .round = ROUND, .roundingPower = D.roundingPower});
                if (LIVE > 0.0 && D.surface)
                    layers.push_back(surfaceLayer(D.surface, D.texture, BOX, ALPHA * sc<float>(LIVE), ROUND, D.roundingPower));
                els[i].element = makeUnique<CTripFramePassElement>(
                    CTripFramePassElement::SData{.layers = std::move(layers), .clip = D.clipBox, .surface = D.surface, .mon = mon, .when = D.when, .discarded = LIVE <= 0.0});
                swapped        = true;
                ++i;
            }
            return swapped;
        }

        // RENDER_PRE_WINDOW / RENDER_POST_WINDOW: the window's main surface,
        // drawn from the snapshot.
        void onStage(eRenderStage stage, Render::CRenderContext& ctx) {
            if (g_held.empty() || ctx.m_renderingSnapshot)
                return;
            if (stage == RENDER_PRE_WINDOW) {
                const auto W = ctx.m_data.currentWindow.lock();
                g_open       = {};
                if (SHeld* H = W ? find(W) : nullptr) {
                    ++H->seen;
                    g_open = {.win = W, .pass = &Render::IHyprRenderer::currentPass(ctx), .start = Render::IHyprRenderer::currentPass(ctx).m_passElements.size()};
                }
                return;
            }
            if (stage != RENDER_POST_WINDOW)
                return;
            const auto W = g_open.win.lock();
            auto*      PASS = g_open.pass;
            const auto START = g_open.start;
            g_open           = {};
            if (!W || !PASS || PASS != &Render::IHyprRenderer::currentPass(ctx) || W != ctx.m_data.currentWindow.lock())
                return;
            SHeld*     H   = find(W);
            const auto MON = ctx.m_data.pMonitor.lock();
            if (!H || !MON || H->mon.lock() != MON || !H->fb || !H->fb->getTexture() || !H->fb->getTexture()->ok() || openScrollOverviewForMonitor(MON))
                return;
            if (msSince(H->at) >= TripFrameRule::LAST_MS) // its timer is a few ms late: never past the last moment
                return;
            if (swapSurfaces(PASS->m_passElements, START, W, *H, MON))
                ++(H->weight < 1.0 ? H->frames : H->live);
        }
    }

    bool holding(const PHLWINDOW& w) {
        return w && find(w);
    }

    void hold(const PHLWINDOW& w) {
        if (!w || !w->mapped() || !w->m_workspace || !g_pHyprRenderer || !w->wlSurface() || !w->wlSurface()->resource())
            return;
        const auto MON = w->m_monitor.lock();
        if (!MON || MON->m_scale <= 0 || openScrollOverviewForMonitor(MON))
            return;
        // Already held (a quick toggle back before it drew): the snapshot it
        // has IS the last good frame -- what is on screen now is not.
        // Its live picture is a drawn one still, unless it had committed at
        // its new size and not drawn yet (phase B): then the snapshot stays.
        if (SHeld* H = find(w)) {
            H->good     = H->released || !H->fresh;
            H->at       = Clock::now();
            H->state    = {};
            H->sent     = sentSize(w);
            H->cSize    = H->sent;
            H->cPos     = reportedPosition(w);
            H->oldSurf  = w->wlSurface()->resource()->m_current.size;
            H->fresh    = false;
            H->released = false;
            H->landedMs = 0;
            H->cBuf     = w->wlSurface()->resource()->m_current.size;
            watchSubs(*H, w, true);
            trace(std::format("{} held again ({})", w->metadata().appID(), H->good ? "its live picture is a drawn one" : "it has not drawn since"));
            armTimer();
            return;
        }
        // Where renderWindow draws it (Renderer.cpp renderWindow), in the
        // snapshot's monitor px.
        const Vector2D POS = w->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT) + (Hyprdrive::Compat::pinned(w) ? Vector2D{} : w->m_workspace->m_renderOffset->value()) +
            w->presentation().floatingOffset();
        const Vector2D SIZE = w->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
        const CBox     SRC{(POS - MON->m_position) * MON->m_scale, SIZE * MON->m_scale};
        if (!finiteBox(SRC))
            return;
        auto fb = g_pHyprRenderer->makeSnapshotFB(w);
        if (!fb || !fb->isAllocated() || !fb->getTexture() || !fb->getTexture()->ok() || fb->m_size.x < 2 || fb->m_size.y < 2)
            return;
        g_held.push_back({.win      = w,
                          .mon      = MON,
                          .fb       = std::move(fb),
                          .src      = SRC,
                          .snapSize = SIZE,
                          .sent     = sentSize(w),
                          .cSize    = sentSize(w),
                          .cPos     = reportedPosition(w),
                          .at       = Clock::now(),
                          .oldSurf  = w->wlSurface()->resource()->m_current.size});
        const PHLWINDOWREF REF  = w;
        g_held.back().commit    = w->wlSurface()->resource()->m_events.commit.listen([REF] { onCommit(REF); });
        g_held.back().precommit = w->wlSurface()->resource()->m_events.precommit.listen([REF] { onPrecommit(REF); });
        g_held.back().cBuf      = w->wlSurface()->resource()->m_current.size;
        watchSubs(g_held.back(), w, true);
        trace(std::format("{} held at {}x{} (its last good frame)", w->metadata().appID(), SIZE.x, SIZE.y));
        armTimer();
    }

    void preRender(const PHLMONITOR& monitor) {
        if (g_held.empty() || !monitor)
            return;
        review();
        // Drawn live (or fading in), stretched: every frame redraws all of
        // it -- the client's own damage is where its picture would be at its
        // own size, not where the stretch puts it.
        for (const auto& H : g_held)
            if (const auto W = H.win.lock(); W && W->mapped() && H.mon.lock() == monitor && (H.weight > 0.0 || H.fading))
                g_pHyprRenderer->damageWindow(W, true);
    }

    void init() {
        g_stage = Event::bus()->m_events.render.stage.listen([](Event::SRenderStageEvent e) {
            if (e.context)
                onStage(e.stage, e.context->get());
        });
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
            for (auto& E : g_pHyprRenderer->context().m_pass.m_passElements)
                if (E.element && E.element->type() == EK_TRANSFORMED_WINDOW)
                    if (auto* const T = dynamic_cast<CTransformedWindowPassElement*>(E.element.get()); T && T->m_data.pass)
                        T->m_data.pass->removeAllOfType(CTripFramePassElement::PASS_NAME);
            g_pHyprRenderer->context().m_pass.removeAllOfType(CTripFramePassElement::PASS_NAME);
        }
    }
}
