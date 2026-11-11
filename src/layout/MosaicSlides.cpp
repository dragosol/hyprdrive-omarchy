// Mosaic: desktop slides -- what a sliding canvas desktop draws, the slide tail, the swipe
// stretch. Split out of Mosaic.cpp; the state is still there (MosaicParts.hpp).
#include "MosaicParts.hpp"

namespace Hyprdrive::Mosaic {

    using namespace MosaicDetail;


    // ---- DESKTOP SLIDES (user, 2026-10-29 / 2026-11-01)
    //
    // OFF THE VIEW, OFF THE SLIDE: Hyprland draws a sliding desktop's windows
    // wherever the slide puts them, and a canvas window parked a screen to the
    // right rode in over the desktop arriving from there. A window out of view
    // on its own desktop is out of the slide. One in view -- even half -- is
    // drawn WHOLE and travels with its desktop, over the next one if it hangs
    // that far (not cut at the desktop's edge: "a cheap bandaid"), and the
    // tail (startSlideTail) carries it off after the slide. The zoomed-out
    // canvas is all on screen and slides as it is (zoomFollowsDesktop).
    static int              slideWatch(void*);

    namespace MosaicDetail {
    void armSlideWatch() {
        if (!g_pCompositor || !g_pCompositor->m_wlEventLoop)
            return;
        if (!g_slideTimer)
            g_slideTimer = wl_event_loop_add_timer(g_pCompositor->m_wlEventLoop, slideWatch, nullptr);
        if (g_slideTimer)
            wl_event_source_timer_update(g_slideTimer, 16);
    }
    }

    static bool gestureSwiping() {
        return g_pUnifiedWorkspaceSwipe && g_pUnifiedWorkspaceSwipe->isGestureInProgress();
    }

    static bool shownNowOn(const PHLWORKSPACE& ws) {
        const auto MON = ws ? ws->m_monitor.lock() : PHLMONITOR{};
        return MON && MON->m_activeWorkspace == ws && !MON->m_activeSpecialWorkspace;
    }

    // A SWIPE WHILE ZOOMED OUT (user, 2026-11-02: with the canvas zoomed out, a
    // trackpad swipe brought the next desktop in at full size, its windows from
    // past the screen's edge popped in at the end, and only then did it zoom
    // out -- every window moving its own distance while the slide still eased;
    // Super+scroll looked right). Super+scroll makes the next desktop active
    // as the slide starts, so its zoom-out runs with the slide; a swipe makes
    // it active only on release. So a canvas desktop a swipe starts bringing
    // in while zoomed out starts zooming out right then, exactly as the
    // Super+scroll one does (user, 2026-11-02: "no longer matches how fedora
    // does super+scroll" -- an instant zoom did not). While
    // Super stays down, a desktop zoomed out this way -- or one just left --
    // stays zoomed out off screen, so swiping back and forth finds it ready
    // ("windows pop in mid-gesture"); letting go of Super ends every zoom but
    // the one shown (superEnd).
    static bool wantsPreZoom(CMosaicAlgorithm* A, const PHLWORKSPACE& ws) {
        if (g_super != ESuper::ZOOMED || g_zoomPressing || !gestureSwiping() || !ws || !ws->m_renderOffset || shownNowOn(ws) || A->zoomed() || A->zoomPending())
            return false;
        const auto OFF = ws->m_renderOffset->value();
        return (ws->m_forceRendering || std::abs(OFF.x) >= 0.5 || std::abs(OFF.y) >= 0.5) && A->canZoomOut();
    }

    // Every 16 ms while a canvas desktop is sliding, its tail running or a
    // zoomed-out one leaving: a slide that stopped with the desktop off screen
    // starts its tail, and a zoom left behind ends there, unseen.
    static int slideWatch(void*) {
        bool again = false;
        for (auto* A : g_instances) {
            const auto WS = A->workspace();
            if (!WS || !WS->m_renderOffset)
                continue;
            const bool SHOWN = shownNowOn(WS), ANIM = WS->m_renderOffset->isBeingAnimated(), SWIPE = gestureSwiping();
            const bool STOPPED_AWAY = !SHOWN && !ANIM && !SWIPE;
            if (wantsPreZoom(A, WS) && A->zoomOut()) {
                A->m_slide.preZoomed = true;
                trace("zooming out for a swipe while zoomed out");
            }
            if (A->m_slide.preZoomed && !SWIPE && !ANIM) {
                // The swipe is over. Landed on and the zoom carried here: it is
                // the live zoom now. Super let go: back in if shown, else the
                // zoom ends unseen. Super still down: it stays zoomed out.
                if (A == liveZoomed())
                    A->m_slide.preZoomed = false;
                else if (g_super != ESuper::ZOOMED) {
                    A->m_slide.preZoomed = false;
                    if (SHOWN)
                        A->zoomIn();
                    else
                        A->endZoomNow();
                }
            }
            if (A->m_slide.zoomLeaving && STOPPED_AWAY && g_super != ESuper::ZOOMED) {
                A->m_slide.zoomLeaving = false;
                A->endZoomNow();
            }
            if (A->m_slide.tail && !ANIM)
                A->endSlideTail();
            // Hyprland damages a sliding desktop's windows only while it is
            // shown: the tail moved with nothing redrawn. From here, not from
            // inside the render pass.
            if (A->m_slide.tail)
                if (const auto MON = WS->m_monitor.lock())
                    g_pHyprRenderer->damageMonitor(MON);
            if (A->m_slide.seen && DesktopStrip::drives(WS))
                A->m_slide.seen = false; // the strip's: no tail (slideFrame)
            if (A->m_slide.seen && STOPPED_AWAY) {
                A->m_slide.seen = false;
                A->startSlideTail();
            }
            if (A->m_slide.seen && SHOWN && !ANIM && !SWIPE)
                A->m_slide.seen = false; // came back, or never left
            again = again || A->m_slide.seen || A->m_slide.tail || A->m_slide.zoomLeaving || A->m_slide.preZoomed;
        }
        if (again && g_slideTimer)
            wl_event_source_timer_update(g_slideTimer, 16);
        return 0;
    }

    // Once per window per frame, from the renderer (main.cpp): the slide is
    // noticed here, and a desktop called back during its tail starts back from
    // where the tail had it -- not from a screen away, a jump the size of what
    // the tail had travelled ("quickly reversable").
    static void slideFrame(CMosaicAlgorithm* A, const PHLWORKSPACE& ws) {
        auto&      OFFV = ws->m_renderOffset;
        const bool ANIM = OFFV->isBeingAnimated();
        if (A->m_slide.tail) {
            const auto GOAL = OFFV->goal();
            if (ANIM && std::abs(GOAL.x - A->m_slide.tailGoal.x) < 0.5 && std::abs(GOAL.y - A->m_slide.tailGoal.y) < 0.5)
                A->m_slide.tailLast = OFFV->value();
            else {
                // A new slide of this desktop: from the tail's side only.
                const auto V = OFFV->value(), L = A->m_slide.tailLast;
                A->endSlideTail();
                if (ANIM && V.x * L.x >= 0 && V.y * L.y >= 0 && (std::abs(L.x) > std::abs(V.x) || std::abs(L.y) > std::abs(V.y)))
                    OFFV->setValue(L);
            }
        }
        // The desktop strip places it (DesktopStrip.hpp), with room for what
        // hangs past its edges: no extension, no tail.
        if (DesktopStrip::drives(ws)) {
            A->m_slide.seen = false;
            return;
        }
        const auto V     = OFFV->value();
        const bool SHOWN = shownNowOn(ws);
        // A slide taking it away just began: it carries its windows off itself.
        if (ANIM && !SHOWN && !A->m_slide.tail && A->extendSlide()) {
            A->m_slide.seen = false;
            armSlideWatch();
            return;
        }
        // The slide just stopped with the desktop away: the tail starts in THIS
        // frame -- Hyprland is deciding right now whether to draw its windows,
        // and from the watcher's next tick they were gone for a few frames.
        if (A->m_slide.seen && !SHOWN && !ANIM && !gestureSwiping()) {
            A->m_slide.seen = false;
            A->startSlideTail();
            if (A->m_slide.tail)
                armSlideWatch();
            return;
        }
        if (ANIM || (SHOWN && (std::abs(V.x) >= 0.5 || std::abs(V.y) >= 0.5))) {
            A->m_slide.seen = true;
            armSlideWatch();
        }
    }

    bool hideDuringSlide(const PHLWINDOW& w, const PHLMONITOR& monitor) {
        if (!w || !monitor || w->m_isFloating || w->m_pinned)
            return false;
        const auto WS = w->m_workspace;
        if (!WS || WS->m_isSpecialWorkspace || !WS->m_renderOffset)
            return false;
        const auto OFF  = WS->m_renderOffset->value();
        const bool ANIM = WS->m_renderOffset->isBeingAnimated();
        if (!ANIM && std::abs(OFF.x) < 0.5 && std::abs(OFF.y) < 0.5)
            return false;
        auto* A = canvasOf(WS);
        if (!A || !A->holds(w))
            return false;
        slideFrame(A, WS);
        // About to be zoomed out for the swipe (wantsPreZoom): the watcher
        // starts its zoom -- from then on drawn exactly as a Super+scroll arrival.
        if (wantsPreZoom(A, WS))
            armSlideWatch();
        if (A->zoomed() || openScrollOverviewForMonitor(monitor) || (std::abs(OFF.x) < 0.5 && std::abs(OFF.y) < 0.5))
            return false;
        const CBox MON{monitor->m_position, monitor->m_size};
        return w->getFullWindowBoundingBox().intersection(MON).empty();
    }

    // THEY SLIDE IN (user, 2026-11-02: a swipe "instantly pops-in the windows
    // outside the desktop area from the next desktop"). A window of the
    // desktop coming in that hangs off its near edge was drawn, whole, over
    // the desktop still shown the moment the slide began. The desktop coming
    // in by a finger swipe now starts that much further out -- the swipe
    // stretched by its largest overhang, the mirror of extendSlide on the way
    // out -- so every window of it enters from the screen's edge, all at the
    // same speed. A swipe sets the offset on every finger update: each new
    // value is scaled here, before the frame (render.pre). Switches by mouse
    // and keyboard are left exactly as they were ("works as intended").
    // Past the overhang by a few pixels more: a border at the very edge of the
    // box left a 1-2 px sliver of the desktop going away for three frames.
    constexpr double SLIDE_MARGIN = 4.0;
    static void arrivalFrame(CMosaicAlgorithm* A, const PHLWORKSPACE& ws, const PHLMONITOR& mon) {
        auto&          OFFV = ws->m_renderOffset;
        const Vector2D V    = OFFV->value();
        const bool     ANIM = OFFV->isBeingAnimated(), SHOWN = shownNowOn(ws);
        if (std::abs(V.x) < 0.5 && std::abs(V.y) < 0.5 && !ANIM) {
            A->m_slide.arriveSet   = {};
            A->m_slide.leaveSet    = {};
            A->m_slide.leaveScaled = false;
            return;
        }
        // Zoomed out (or zooming), the overhang is measured where the windows
        // are drawn: none once it all fits, so nothing changes then.
        if (!A->arrangedAsCanvas())
            return;
        const bool   XAX  = std::abs(V.x) >= std::abs(V.y);
        const double D    = XAX ? V.x : V.y;
        const double SPAN = XAX ? mon->m_size.x : mon->m_size.y;
        if (std::abs(D) < 0.5 || SPAN < 1)
            return;
        // nearOverhang(D): the edge facing the way it moves from -- coming in,
        // its near edge; going away, its trailing one. The same measure.
        const bool SWIPE = gestureSwiping();
        if (SWIPE && ws->m_forceRendering) {
            auto& SET = SHOWN ? A->m_slide.leaveSet : A->m_slide.arriveSet;
            if (std::abs(V.x - SET.x) < 0.01 && std::abs(V.y - SET.y) < 0.01)
                return; // our own value: no finger moved since
            const double O = A->nearOverhang(ws, mon, D, XAX);
            if (O < 1)
                return;
            const double   K   = (SPAN + O + SLIDE_MARGIN) / SPAN;
            const Vector2D NEW = XAX ? Vector2D{V.x * K, V.y} : Vector2D{V.x, V.y * K};
            OFFV->setValueAndWarp(NEW);
            SET                    = NEW;
            A->m_slide.swipeScaled = true;
            A->m_slide.swipeSet    = NEW;
            if (SHOWN)
                A->m_slide.leaveScaled = true;
            return;
        }
        // THEY LEAVE QUICKLY (user, 2026-11-02: "windows still slide away too
        // slowly"). Swiped away and let go: Hyprland sends it exactly a screen
        // off, which would bring its windows hanging off the trailing edge back
        // into view, and the tail then carried them off slowly. The release
        // goes the overhang further instead, on the same animation.
        if (!SWIPE && A->m_slide.leaveScaled) {
            A->m_slide.leaveScaled = false;
            const Vector2D GOAL = OFFV->goal();
            const double   G    = XAX ? GOAL.x : GOAL.y;
            if (!ANIM || SHOWN || std::abs(G) < SPAN * 0.99)
                return; // called back, or not going a screen away
            const double O = A->nearOverhang(ws, mon, G, XAX);
            if (O < 1)
                return;
            const double K = (SPAN + O + SLIDE_MARGIN) / SPAN;
            // FROM WHERE IT WAS LAST DRAWN (user, 2026-11-03: the window hanging
            // off the desktop just left "flashes" once the swipe ends). Let go,
            // Hyprland restarts the slide away from its own, unstretched place
            // -- the overhang nearer than the last frame drew it -- and that
            // frame is drawn before this one stretches the goal: the hanging
            // window leapt back into view for a frame. It goes on from the
            // place the swipe's last frame left it.
            const Vector2D LEFT = A->m_slide.leaveSet;
            const double   LD   = XAX ? LEFT.x : LEFT.y;
            if (LD * D > 0 && std::abs(LD) > std::abs(D))
                OFFV->setValue(LEFT);
            trace(std::format("swipe release goes past the overhang: {:.0f} -> {:.0f}, from {:.0f}", G, G * K, std::abs(LD) > std::abs(D) ? LD : D));
            *OFFV = XAX ? Vector2D{GOAL.x * K, GOAL.y} : Vector2D{GOAL.x, GOAL.y * K};
        }
    }

    // A swiped canvas desktop's stretch at offset `d` (arrivalFrame's): 1 off
    // a canvas or with nothing hanging past the edge it comes in by.
    static double swipeStretchOf(CMosaicAlgorithm* A, const PHLWORKSPACE& ws, const PHLMONITOR& mon, const Vector2D& v) {
        const bool   XAX  = std::abs(v.x) >= std::abs(v.y);
        const double D    = XAX ? v.x : v.y;
        const double SPAN = XAX ? mon->m_size.x : mon->m_size.y;
        if (!A->arrangedAsCanvas() || std::abs(D) < 0.5 || SPAN < 1)
            return 1.0;
        const double O = A->nearOverhang(ws, mon, D, XAX);
        return O < 1 ? 1.0 : (SPAN + O + SLIDE_MARGIN) / SPAN;
    }

    double swipeStretch(const PHLWORKSPACE& ws, const PHLMONITOR& monitor, const Vector2D& offset) {
        if (!ws || !monitor)
            return 1.0;
        for (auto* A : g_instances)
            if (A->workspace() == ws)
                return swipeStretchOf(A, ws, monitor, offset);
        return 1.0;
    }

    // THE RELEASE STARTS WHERE IT IS DRAWN (user, 2026-11-02, spamming peeks:
    // the desktop peeked at "overextends ... beyond peek positions" when let
    // go). A swipe sets each desktop's RAW offset on every finger event, and
    // arrivalFrame stretches it past the overhang only before the next frame.
    // Let go between the two -- a quick peek, released straight after the
    // fingers moved -- and Hyprland's release began from the raw place, an
    // overhang nearer than the frame on screen: the desktop leapt in by that
    // much, then slid away. Every desktop this swipe stretched now starts its
    // release from its stretched place.
    void swipeReleased(const PHLMONITOR& monitor) {
        if (!monitor)
            return;
        for (auto* A : g_instances) {
            const auto WS = A->workspace();
            if (!WS || WS->m_isSpecialWorkspace || !WS->m_renderOffset || WS->m_monitor.lock() != monitor || !A->m_slide.swipeScaled)
                continue;
            A->m_slide.swipeScaled = false;
            auto& OFFV             = WS->m_renderOffset;
            if (!OFFV->isBeingAnimated())
                continue;
            const Vector2D V = OFFV->value(), G = OFFV->goal();
            // From where it was drawn: the raw place stretched, unless a frame
            // already stretched it.
            Vector2D start = V;
            if (V.distance(A->m_slide.swipeSet) >= 0.01) {
                const double K   = swipeStretchOf(A, WS, monitor, V);
                const bool   XAX = std::abs(V.x) >= std::abs(V.y);
                if (K > 1.0)
                    start = XAX ? Vector2D{V.x * K, V.y} : Vector2D{V.x, V.y * K};
            }
            // GOING AWAY, AS FAR AS IT WAS STRETCHED (user, 2026-11-02: with
            // the peeked desktop's windows hanging toward this one, the peek
            // "overextends once I release"). Hyprland sends a desktop going
            // away -- a peek let go, or the one a switch left -- a plain screen
            // off: nearer than its stretched place, so it came back IN across
            // its overhang first. Its goal is stretched the same way, so it only
            // ever moves out (arrivalFrame's release branch had done this for
            // the one a switch left, a frame late: done here, once).
            Vector2D     goal = G;
            const bool   GX   = std::abs(G.x) >= std::abs(G.y);
            const double GD = GX ? G.x : G.y, SPAN = GX ? monitor->m_size.x : monitor->m_size.y;
            if (!shownNowOn(WS) && std::abs(GD) >= SPAN * 0.99) {
                const double K = swipeStretchOf(A, WS, monitor, G);
                if (K > 1.0)
                    goal = GX ? Vector2D{G.x * K, G.y} : Vector2D{G.x, G.y * K};
                A->m_slide.leaveScaled = false;
            }
            if (start.distance(V) < 0.01 && goal.distance(G) < 0.01)
                continue;
            trace(std::format("swipe release from where it is drawn: {:.0f} -> {:.0f}, to {:.0f} -> {:.0f}", GX ? V.x : V.y, GX ? start.x : start.y, GD,
                              GX ? goal.x : goal.y));
            OFFV->setValue(start); // the same release, from there...
            *OFFV = goal;          // ... to there
        }
    }

    double slideOverhang(const PHLWORKSPACE& ws, const PHLMONITOR& monitor, int side, bool xax) {
        if (!ws || !monitor)
            return 0;
        for (auto* A : g_instances)
            if (A->workspace() == ws)
                return A->arrangedAsCanvas() ? A->nearOverhang(ws, monitor, side < 0 ? 1.0 : -1.0, xax) : 0.0;
        return 0;
    }

    void preRender(PHLMONITOR monitor) {
        if (!monitor)
            return;
        for (auto* A : g_instances) {
            const auto WS = A->workspace();
            if (WS && !WS->m_isSpecialWorkspace && WS->m_renderOffset && WS->m_monitor.lock() == monitor)
                arrivalFrame(A, WS, monitor);
        }
    }
}
