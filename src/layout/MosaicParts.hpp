// Declarations the Mosaic.cpp parts share: the mosaic layout class itself (CMosaicAlgorithm, one
// per desktop on mosaic) and the file-scope state of Mosaic.cpp, declared extern. Internal to
// src/layout/Mosaic*.cpp: never include it anywhere else.
#pragma once

#include "Mosaic.hpp"
#include "MotionClock.hpp"
#include "MotionRetarget.hpp"
#include "Intent.hpp"
#include "DesktopStrip.hpp"
#include "DebugPaths.hpp"
#include "MosaicPack.hpp"
#include "MosaicCanvas.hpp"
#include "MosaicDrive.hpp"
#include "MosaicTripHold.hpp"
#include "FullscreenDesktop.hpp"
#include "Motion.hpp"
#include "globals.hpp"
#include "IOverview.hpp"
#include "Dock.hpp"
#include "DockStacks.hpp"
#include "OverviewRender.hpp"
#include "FlightController.hpp"
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/layout/algorithm/Algorithm.hpp>
#include <hyprland/src/layout/algorithm/TiledAlgorithm.hpp>
#include <hyprland/src/layout/space/Space.hpp>
#include <hyprland/src/layout/target/Target.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/managers/fullscreen/handler/FullscreenHandler.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/animation/AnimationManager.hpp>
#include <hyprland/src/config/shared/animation/AnimationTree.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/protocols/XDGShell.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/state/ViewState.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/layout/supplementary/DragController.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <limits>
#include <hyprland/src/managers/input/UnifiedWorkspaceSwipeGesture.hpp>
#include <hyprland/src/managers/KeybindManager.hpp>
#include <hyprland/src/pointer/cursor/CursorShapeOverrideController.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/config/shared/actions/ConfigActions.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/pointer/PointerController.hpp>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/view/LayerSurface.hpp>
#include <hyprland/src/protocols/LayerShell.hpp>
#include "Config.hpp"
#include <linux/input-event-codes.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <typeinfo>
#include <unordered_map>
#include <vector>

namespace Hyprdrive::Mosaic {

    class CMosaicAlgorithm;
    namespace MosaicDetail {
        extern std::vector<CMosaicAlgorithm*> g_instances;
        extern std::unordered_map<std::string, Vector2D> g_sizes;
        extern bool g_sizesLoaded;

        // A WINDOW'S MOSAIC SIZE, for as long as the window lives (user,
        // 2026-10-15: "when I toggle out of mosaic into dwindle ... windows
        // remember their sizes during pure mosaic"). The layout's own list
        // dies with it: Super+L to dwindle destroys this desktop's mosaic, and
        // the one Super+L builds again was handed every window at its DWINDLE
        // tile (desiredGeometry() is the window's current size) -- 941x1056,
        // 941x616, 941x521 for three windows that had been 900x600, 380x636 and
        // 595x442 -- which no longer fit, so it came back tiled and stayed so.
        // The same held for a window moved to another desktop and back. Kept
        // per WINDOW (a weak reference: a closed window's entry lapses, and a
        // new window at a reused address is not mistaken for it), outside every
        // layout instance, and written whenever a mosaic lays its desktop out.
        //
        // One exception, the old rule: a window that was FLOATED and resized
        // there (Super+V, a new size, Super+V) comes back at its floating size.
        // Hyprland rewrites the floating size on every workspace move, so a
        // changed one alone proves nothing: it counts only when the window
        // really floated since it left (the window.floating event) and its
        // floating size moved from what it was when it left. A Super+drag move
        // floats the window too; that float is not counted (onFloating), so a
        // dragged window keeps its own size even if its client resized itself.
        struct SKept {
            PHLWINDOWREF window;
            Vector2D     size;
            Vector2D     floatAtLeave;      // its floating size when it left a mosaic
            bool         floatedSince = false;
            bool         trusted      = true; // MosaicPack::SNatural::trusted
        };
        extern std::vector<SKept> g_windowSizes;
        // MosaicSizes.cpp
        extern void loadSizes();
        extern void saveSize(const std::string& app, const Vector2D& size);
        extern std::string appOf(const PHLWINDOW& w);
        extern void rememberWindow(const PHLWINDOW& w, const Vector2D& size, bool trusted);
        extern bool keptTrusted(const PHLWINDOW& w);
        extern void leftMosaic(const SP<Layout::ITarget>& t);
        extern void onFloating(const PHLWINDOW& w);
        extern void saveKept();
        extern void loadKept();
        extern std::optional<Vector2D> rememberedWindow(const SP<Layout::ITarget>& t);
        // Mosaic.cpp
        // While the layout is being added: a window that was fullscreen or
        // maximized before the swap is again, in the handler Hyprland reads
        // now and in `self`'s own (FullscreenCarry.hpp). Before it is placed.
        extern void carryFullscreenInto(const SP<Layout::ITarget>& target, Layout::IModeAlgorithm* self);
        extern bool traceOn();
        extern void trace(const std::string& line);
        extern bool canvasOn();
        extern bool canvasAllDirections();
        extern double canvasPanMs();
        extern float canvasEase(double t);
        extern bool g_canvasPanning;
        extern bool g_cursorRiding;
    }

    // One window of the layout, in arrival order (the order the rows read).
    struct SMosaicEntry {
        WP<Layout::ITarget> target;
        Vector2D            natural; // the size the window keeps in the mosaic
        // Its leaf in the TILES' dwindle tree. The tree keeps its own splits,
        // so the tiles never change the size the mosaic gives back (user,
        // 2026-10-14: "coming out of the tiling hybrid mode, windows need to
        // assume the shape and size they were at pre-tiling").
        long long           id = 0;
        bool                trusted = true; // may become its app's size on close

        // The size it is packed at: its natural one -- or, PSEUDOTILED
        // (Hyprland's pseudo, SUPER+P), the size Hyprland will draw it at,
        // centred in whatever slot it is given (WindowTarget's pseudo branch).
        // Packed at its natural size, the difference was empty space around
        // it (user, 2026-11-02: Zen pseudotiled at 967 tall in a 1083 slot on
        // the canvas, 58 px of space above and below it).
        Vector2D packSize() const {
            if (const auto T = target.lock(); T && T->isPseudo()) {
                const Vector2D P = T->pseudoSize();
                if (P.x >= 1 && P.y >= 1)
                    return P;
            }
            return natural;
        }
    };

    // PUSH THROUGH THE EDGE (user, 2026-11-06): the hand pushing at an outer
    // screen edge pans the canvas by the motion the edge swallows, when the
    // focused window hides everything on that side. Latched per push -- the
    // desktop and the direction, kept while the pushes keep coming -- so it
    // goes on once the neighbours come into view, to the canvas's end.
    struct SEdgePanLatch {
        const void*                           who = nullptr;
        Vector2D                              dir;
        std::chrono::steady_clock::time_point at{};
    };
    constexpr auto       EDGE_PAN_HOLD = std::chrono::milliseconds(250);
    namespace MosaicDetail {
        extern SEdgePanLatch g_edgePan;
        extern MosaicPack::SEdgePressure g_edgePressure;
        extern double pushPressure(int axis, double sign);
        extern bool pointerOnChrome(const Vector2D& p);
        extern bool g_hoverStale;
    bool hoverRefresh(bool blocked);
    bool pointerAtOuterEdge(const Vector2D& p);
        extern bool pointerInDockBand(const Vector2D& p);
        extern bool edgePanRecent();
    // The canvas's frame clock (pans, zooms, a drag pushing at the screen's
    // edge): an event-loop timer, never a request from inside the render pass.
    void armCanvasTick();
        extern void drawWindowLabel(const PHLMONITOR& monitor, const PHLWINDOW& w, float alpha);
    }

    // The slide tail's curve (CMosaicAlgorithm::startSlideTail).
    inline constexpr const char* SLIDE_TAIL_BEZIER_NAME = "hyprdriveSlideTail";

    using namespace MosaicDetail;

    class CMosaicAlgorithm : public Layout::ITiledAlgorithm {
      public:
        CMosaicAlgorithm() {
            g_instances.push_back(this);
            // The canvas follows focus (a click, Super+arrows, the dock, a new
            // window): the focused window is brought into view, as the
            // scrolling layout brings its column.
            // Not focus that follows the mouse: hovering a window half off the
            // edge would slide the canvas, put another window under the
            // pointer, focus that, and slide again. A click does reveal.
            m_focusHook = Event::bus()->m_events.window.active.listen([this](PHLWINDOW w, Desktop::eFocusReason reason) {
                // The window focus came FROM (SMART FOCUS PAN, step 2).
                const auto FROM = m_lastFocus.lock();
                if (w && holds(w) && w != FROM) {
                    m_prevFocus = m_lastFocus;
                    m_lastFocus = w;
                }
                // HOVER PANS TOO (user, 2026-10-26: "when I
                // mouseover a window in this mode it should refocus me"): the
                // pointer rides the canvas (rideCursor), so the pan keeps it on
                // the window it hovered and nothing cascades. Never the hover a
                // ride itself causes. A hover while a pan is still landing
                // re-aims it (panTo retargets): quick hops used to leave the
                // last window focused but off screen.
                const bool FFM   = reason == Desktop::FOCUS_REASON_FFM;
                // Never while the dock has a fan, a pane or a menu up (user,
                // 2026-11-03): a hover that got past it must not slide the
                // canvas -- and the pointer with it -- out from under the dock.
                const bool DOCKUP = Hyprdrive::Stacks::isOpen() || Hyprdrive::Dock::menuOpen();
                // A window sliding under the pointer while it still pushes is
                // no hover -- but once the pointer is off the edge, the push
                // is over (user, 2026-11-09: quickly back on the previous
                // window, it was focused and left half off screen).
                const Vector2D PTR    = g_pInputManager->getMouseCoordsInternal();
                const bool     PUSHING = edgePanRecent() && pointerAtOuterEdge(PTR);
                const bool HOVER  = FFM && !g_cursorRiding && !DOCKUP && !PUSHING && !pointerOnChrome(PTR) && !pointerInDockBand(PTR);
                if (m_asCanvas && !g_canvasPanning && !m_zoomed && w && holds(w) && (!FFM || HOVER))
                    revealWindow(w, true, FROM != w ? FROM : PHLWINDOW{});
            });
            // The canvas settings changed (the settings panel writes them and
            // Hyprland reloads): a desktop on the canvas re-arranges at once, and
            // one in the tiles goes onto the canvas when it was just turned on.
            m_reloadHook = Event::bus()->m_events.config.reloaded.listen([this] { canvasSettingsChanged(); });
        }
        ~CMosaicAlgorithm() override {
            std::erase(g_instances, this);
        }

        // The canvas settings changed: a desktop on the canvas re-arranges at
        // once (off: back to the plain mosaic, or the tiles if they do not
        // fit), and one in the tiles goes onto the canvas when it was turned on.
        void canvasSettingsChanged() {
            if (m_asCanvas || (!m_asMosaic && !m_entries.empty() && canvasOn())) {
                m_revealPending = true;
                recalculate();
            }
        }

        // For the corner grab and the remembered sizes.
        bool holds(const PHLWINDOW& w) const {
            for (const auto& E : m_entries)
                if (const auto T = E.target.lock(); T && T->window() == w)
                    return true;
            return false;
        }
        bool arrangedAsMosaic() const {
            return m_asMosaic;
        }
        bool arrangedAsCanvas() const {
            return m_asCanvas;
        }

        // ---- the canvas's view (MosaicCanvas.hpp)
        std::optional<MosaicPack::SCanvasView> canvasViewNow() const {
            const auto AREA = area();
            if (!m_asCanvas || !AREA)
                return std::nullopt;
            return MosaicPack::canvasView(m_canvas.w, m_canvas.h, MosaicPack::SRect{AREA->x, AREA->y, AREA->w, AREA->h});
        }
        Vector2D canvasOffset() const {
            return m_canvasOffset;
        }
        Vector2D canvasSize() const {
            return Vector2D{m_canvas.w, m_canvas.h};
        }
        std::optional<CBox> canvasArea() const {
            return area();
        }
        // Straight to `offset`: a swipe or the Super-pan tracking the hand.
        // Whatever pan was easing stops where it is. `ride`: the pointer
        // moves with the canvas (rideCursor).
        void setCanvasOffset(const Vector2D& offset, bool warp, bool ride = false) {
            m_pan.on            = false;
            const Vector2D WAS  = m_canvasOffset;
            m_canvasOffset      = offset;
            layoutCanvas(warp);
            if (ride)
                rideCursor(WAS);
        }
        // THE POINTER RIDES THE CANVAS (user, 2026-10-26: "so user doesn't
        // accidentally refocus through a bunch of windows at once, the cursor
        // should stay relative to where it was on the windows ... cursor holds
        // a position within the canvas instead of relative to the screen"). The
        // view moved from `was`: the pointer moves with what is under it, so
        // focus that follows the mouse stays on the same window -- kept on its
        // screen. Not mid-drag (the window follows the hand), not zoomed, not
        // under a pointer lock (a game owns the cursor), not when the pointer
        // is on another screen.
        void rideCursor(const Vector2D& was) {
            const Vector2D D = m_canvasOffset - was;
            if ((std::abs(D.x) < 0.5 && std::abs(D.y) < 0.5) || m_zoomed || g_pInputManager->isConstrained())
                return;
            // On the dock (or its menu, a fan, a bar): the pointer is not on a
            // window, and stays where it is (user, 2026-11-09: a dock click
            // that focused an app carried the pointer off the dock with the
            // pan, and the next click could not minimize).
            if (pointerOnChrome(g_pInputManager->getMouseCoordsInternal()) || pointerInDockBand(g_pInputManager->getMouseCoordsInternal()))
                return;
            if (g_layoutManager && g_layoutManager->dragController()->target())
                return;
            const auto WS  = workspace();
            const auto MON = WS ? WS->m_monitor.lock() : PHLMONITOR{};
            if (!MON || MON->m_activeWorkspace != WS || MON->m_activeSpecialWorkspace)
                return;
            const Vector2D P = g_pInputManager->getMouseCoordsInternal();
            const CBox     B = MON->logicalBox();
            if (!B.containsPoint(P))
                return;
            g_cursorRiding = true;
            Pointer::pointerController()->warpTo(Vector2D{std::clamp(P.x - D.x, B.x + 1, B.x + B.w - 2), std::clamp(P.y - D.y, B.y + 1, B.y + B.h - 2)}, true);
            g_cursorRiding = false;
        }
        // Where the view is headed: the end of the pan in flight, else here.
        Vector2D panTarget() const {
            return m_pan.on ? m_pan.to : m_canvasOffset;
        }
        // EASED to `want` (canvasEase over canvasPanMs). `settle`: at the end,
        // keys go to a window in view.
        void panTo(Vector2D want, bool settle = false) {
            const auto VIEW = canvasViewNow();
            if (!VIEW)
                return;
            want = Vector2D{std::clamp(want.x, 0.0, VIEW->maxX), std::clamp(want.y, 0.0, VIEW->maxY)};
            const double MS = canvasPanMs();
            if (MS < 1.0) {
                setCanvasOffset(want, true);
                if (settle)
                    settleFocus();
                return;
            }
            if (m_pan.on) {
                // Under way: it heads somewhere new FROM WHERE IT IS NOW, over
                // a fresh curve (user, 2026-10-26: "panning shouldn't become
                // instant just because i move my cursor to another app halfway
                // through the pan"). Only swapping `to` kept the old start and
                // clock, so half way through the curve the view leapt most of
                // the way to the new target in one frame.
                if (std::abs(want.x - m_pan.to.x) < 0.5 && std::abs(want.y - m_pan.to.y) < 0.5) {
                    m_pan.settle |= settle;
                    return;
                }
                // ...and at the speed it had (B6): the curve's own start slope
                // used to kick a re-aim and snap a reversal.
                const auto NOW  = std::chrono::steady_clock::now();
                const SPan OLD  = m_pan;
                m_pan.from      = m_canvasOffset;
                m_pan.to        = want;
                m_pan.start     = NOW;
                m_pan.ms        = MS;
                SPan::matchKick(m_pan, OLD, m_canvasOffset, NOW);
                m_pan.settle |= settle;
                trace(std::format("pan re-aimed {}x{} -> {}x{}", m_canvasOffset.x, m_canvasOffset.y, want.x, want.y));
                return;
            }
            if (std::abs(want.x - m_canvasOffset.x) < 0.5 && std::abs(want.y - m_canvasOffset.y) < 0.5)
                return;
            m_pan = SPan{.on = true, .settle = settle, .from = m_canvasOffset, .to = want, .start = std::chrono::steady_clock::now(), .ms = MS};
            trace(std::format("pan {}x{} -> {}x{} over {}ms", m_canvasOffset.x, m_canvasOffset.y, want.x, want.y, MS));
            armCanvasTick();
        }
        // The offset that shows `w` (MosaicCanvas.hpp smartRevealAxis), seen
        // from `cur`. Already in view whole -- and the window focus came from
        // with it -- the view stays: a click inside the screen moves nothing.
        std::optional<Vector2D> revealOffset(const PHLWINDOW& w, Vector2D cur, const PHLWINDOW& from = {}) const {
            const auto AREA = area();
            const auto VIEW = canvasViewNow();
            const auto R    = canvasRectOf(w);
            if (!AREA || !VIEW || !R)
                return std::nullopt;
            const auto FR     = from ? canvasRectOf(from) : std::nullopt;
            const auto INVIEW = [&](const MosaicPack::SRect& q) {
                return q.x >= cur.x - 0.5 && q.x + q.w <= cur.x + AREA->w + 0.5 && q.y >= cur.y - 0.5 && q.y + q.h <= cur.y + AREA->h + 0.5;
            };
            if (INVIEW(*R) && (!FR || INVIEW(*FR)))
                return cur;
            // The windows beside it on each axis: overlapping it across that axis.
            std::vector<MosaicPack::SSpan> nearX, nearY;
            for (size_t i = 0; i < m_canvas.rects.size(); ++i) {
                const auto& Q = m_canvas.rects[i];
                if (std::abs(Q.x - R->x) < 0.5 && std::abs(Q.y - R->y) < 0.5)
                    continue;
                if (Q.y < R->y + R->h && Q.y + Q.h > R->y)
                    nearX.push_back({Q.x, Q.w});
                if (Q.x < R->x + R->w && Q.x + Q.w > R->x)
                    nearY.push_back({Q.y, Q.h});
            }
            constexpr double PEEK = 48.0;
            const double     M    = innerGap();
            return Vector2D{
                MosaicPack::smartRevealAxis(AREA->w, VIEW->maxX, {R->x, R->w}, FR ? std::optional<MosaicPack::SSpan>{{FR->x, FR->w}} : std::nullopt, nearX, M, PEEK),
                MosaicPack::smartRevealAxis(AREA->h, VIEW->maxY, {R->y, R->h}, FR ? std::optional<MosaicPack::SSpan>{{FR->y, FR->h}} : std::nullopt, nearY, M, PEEK)};
        }
        // `animate`: pan there (focus moved); otherwise only aim the view, for
        // the layout pass that follows.
        void revealWindow(const PHLWINDOW& w, bool animate, const PHLWINDOW& from = {}) {
            m_pushedAway = false;
            const auto WANT = revealOffset(w, panTarget(), from);
            if (!WANT)
                return;
            // JUST RE-ARRANGED (user, 2026-10-29: the window spawn animation
            // "is buggy looking"): a window arrived or left, so everyone is
            // already sliding to a new place -- and the new window's focus,
            // a frame later, asked for a pan. A plain pan lays the windows out
            // at each frame's offset outright, so they leapt into the new
            // arrangement and only then panned.
            // It used to jump the view to its end instead. It PANS (owner,
            // 2026-11-11), and carries every window from where it stands: the
            // gap between where a window is drawn and its place at this
            // frame's offset shrinks on the pan's own curve (m_panCarry), so it
            // is still one move per window, from where it stands, and the view
            // eases there rather than arriving at once. Position only: a
            // window's size is never carried (a carried size resized every
            // client mid-pan, 896x596 -> 893x593 and back).
            // Never for a window flying back from the dock: its flight aims at
            // where the layout has it once the restore returns, so the view
            // panning under it slid it in from the screen edge at full size
            // (user, 2026-10-29: "you replaced my maximize from dock
            // animation"). The restore focuses it BEFORE its flight exists,
            // so that is asked of the flight controller -- and it still jumps
            // the view and lets the slides play, exactly as it did.
            const bool JUST = animate && std::chrono::steady_clock::now() - m_rearrangedAt < std::chrono::milliseconds(600);
            if (JUST && Hyprdrive::Flight::restoringForFlight(w)) {
                trace(std::format("reveal after a re-arrangement: {}x{} -> {}x{} as window moves (restore flight)", m_canvasOffset.x, m_canvasOffset.y, WANT->x, WANT->y));
                m_pan.on = false;
                m_panCarry.clear();
                m_canvasOffset = *WANT;
                layoutCanvas(false);
                return;
            }
            if (JUST && !Hyprdrive::Flight::hasFlight(w)) {
                trace(std::format("reveal after a re-arrangement: {}x{} -> {}x{}, panned, windows carried from where they are", m_canvasOffset.x, m_canvasOffset.y, WANT->x,
                                  WANT->y));
                captureCarry();
                panTo(*WANT);
                if (!m_pan.on)
                    m_panCarry.clear(); // it snapped (speed 0): nothing to carry
                return;
            }
            if (animate)
                panTo(*WANT);
            else {
                m_pan.on       = false;
                m_canvasOffset = *WANT;
            }
        }
        // A CLICK PANS (user, 2026-10-26: "a click should refocus and pan the
        // same way"). With focus following the mouse the window under the
        // pointer is usually focused already, so the click itself is no focus
        // change and the focus hook never heard it: it pans here, from the
        // window focus had before.
        void clickReveal(const PHLWINDOW& w) {
            if (!m_asCanvas || m_zoomed || !holds(w))
                return;
            const auto LAST = m_lastFocus.lock();
            const auto FROM = LAST == w ? m_prevFocus.lock() : LAST;
            revealWindow(w, true, FROM != w ? FROM : PHLWINDOW{});
        }
        // Keys go to a window you can see: the focused one if it is still
        // mostly in view, else the one showing most of itself.
        // Is this desktop the one on screen? A focus the canvas makes on its own
        // (a pan landing, a swipe settling) only then: focusing a window on a
        // desktop you just left switches straight back to it (user,
        // 2026-10-26: "it wouldn't let me switch desktops ... it would just take
        // me back to that window" -- a Super+scroll pan landed after the
        // scroll that left the desktop).
        bool shownNow() const {
            const auto WS  = workspace();
            const auto MON = WS ? WS->m_monitor.lock() : PHLMONITOR{};
            return MON && MON->m_activeWorkspace == WS && !MON->m_activeSpecialWorkspace;
        }
        void settleFocus() {
            if (!shownNow())
                return;
            const auto F = Desktop::focusState()->window();
            if (F && holds(F) && visibleShare(F) >= 0.5)
                return;
            if (const auto BEST = mostVisible()) {
                const bool WAS  = g_canvasPanning;
                g_canvasPanning = true; // it is in view: no reveal
                Desktop::focusState()->fullWindowFocus(BEST, Desktop::FOCUS_REASON_KEYBIND);
                g_canvasPanning = WAS;
            }
        }
        // PUSH THROUGH THE EDGE: `lost` is the motion an outer screen edge
        // swallowed (onPointerDelta), per axis toward the edge it was lost at.
        // The view moves by exactly that -- only as far as the hand moves --
        // and stops where the canvas ends, which is the last window and its
        // padding. A push STARTS only where there is something to see: a
        // window wholly past that edge, and only after a firm push
        // (PUSH_PRESSURE): a light touch is the dock's.
        bool pushThrough(const Vector2D& lost) {
            if (!Hyprdrive::Config::getValue<bool>("plugin:hyprdrive:mosaic:edge_push"))
                return false;
            // A dock menu or fan is up: the push is not the canvas's.
            if (Hyprdrive::Stacks::isOpen() || Hyprdrive::Dock::menuOpen())
                return false;
            if (!m_asCanvas || m_zoomed || !shownNow())
                return false;
            const auto VIEW = canvasViewNow();
            const auto AREA = area();
            if (!VIEW || !AREA)
                return false;
            const auto     NOW = std::chrono::steady_clock::now();
            const Vector2D CUR = m_canvasOffset;
            Vector2D       want = CUR;
            Vector2D       dir;
            for (int axis = 0; axis < 2; ++axis) {
                const double D = axis ? lost.y : lost.x;
                if (std::abs(D) < 0.01)
                    continue;
                const double SIGN    = D > 0 ? 1.0 : -1.0;
                const double LATCHED = axis ? g_edgePan.dir.y : g_edgePan.dir.x;
                const bool   GOING   = g_edgePan.who == this && LATCHED == SIGN && NOW - g_edgePan.at < EDGE_PAN_HOLD;
                if (!GOING) {
                    if (!pushMayStart(axis, SIGN, CUR, *AREA))
                        continue;
                    if (!g_edgePressure.press(axis, this, SIGN, D, pushPressure(axis, SIGN), NOW, EDGE_PAN_HOLD))
                        continue;
                }
                const double MAX = axis ? VIEW->maxY : VIEW->maxX;
                double&      w   = axis ? want.y : want.x;
                w                = std::clamp(w + D, 0.0, MAX);
                (axis ? dir.y : dir.x) = SIGN;
            }
            if (dir.x == 0 && dir.y == 0)
                return false;
            g_edgePan = {this, dir, NOW};
            if (std::abs(want.x - CUR.x) < 0.01 && std::abs(want.y - CUR.y) < 0.01) {
                focusUnderPush(); // at the canvas's end: held there, on the last window
                return true;
            }
            setCanvasOffset(want, true);
            m_pushedAway = true;
            focusUnderPush();
            return true;
        }
        // At the push's end: is the pointer on another window than the one the
        // push focused? Then that one gets the focus (hoverRefresh).
        bool pointerLeftPushFor(const PHLWINDOW& pf) {
            const Vector2D P = g_pInputManager->getMouseCoordsInternal();
            const auto     W = Desktop::viewState()->hitTest().windowAt(P, Desktop::View::RESERVED_EXTENTS | Desktop::View::INPUT_EXTENTS | Desktop::View::ALLOW_FLOATING);
            if (!W || W == pf || pointerOnChrome(P) || pointerInDockBand(P))
                return false;
            g_hoverStale = true;
            if (hoverRefresh(false))
                return true;
            // Focused already, while the push still held the view: into view now.
            if (W == Desktop::focusState()->window() && holds(W)) {
                clickReveal(W);
                return true;
            }
            return Desktop::focusState()->window() != pf;
        }
        // THE WINDOW THE PUSH BRINGS UNDER THE POINTER (user, 2026-11-09:
        // "make the pushing through focus the first window I mouseover
        // automatically, without having to move my cursor AFTER it's over the
        // window"): it takes the focus as it arrives -- found on the canvas,
        // so a dock over that spot does not hide it -- and the push goes on
        // (no reveal pan: g_canvasPanning).
        void focusUnderPush() {
            const auto VIEW = canvasViewNow();
            if (!VIEW || m_canvas.rects.size() != m_canvasWho.size())
                return;
            // The pointer is pressed against the screen's edge, and the canvas
            // ends a margin short of it (the last window and its padding): the
            // window within MARGIN of the pointer counts, the nearest first.
            constexpr double MARGIN = 40.0;
            const Vector2D   P      = g_pInputManager->getMouseCoordsInternal();
            if (pointerOnChrome(P) || pointerInDockBand(P))
                return; // the push goes on; the focus waits until the pointer leaves the dock
            PHLWINDOW        best;
            double           bestD = MARGIN + 1;
            for (size_t i = 0; i < m_canvasWho.size(); ++i) {
                const auto T = m_canvasWho[i].lock();
                const auto W = T ? T->window() : PHLWINDOW{};
                if (!W)
                    continue;
                const auto&  R  = m_canvas.rects[i];
                const double X0 = VIEW->originX + R.x - m_canvasOffset.x, Y0 = VIEW->originY + R.y - m_canvasOffset.y;
                const double DX = std::max({X0 - P.x, 0.0, P.x - (X0 + R.w)}), DY = std::max({Y0 - P.y, 0.0, P.y - (Y0 + R.h)});
                const double D  = std::max(DX, DY);
                if (D < bestD) {
                    bestD = D;
                    best  = W;
                }
            }
            if (const auto W = best) {
                if (W == Desktop::focusState()->window())
                    return;
                const bool WAS  = g_canvasPanning;
                g_canvasPanning = true;
                Desktop::focusState()->fullWindowFocus(W, Desktop::FOCUS_REASON_KEYBIND);
                g_canvasPanning = WAS;
                g_hoverStale    = true;  // Hyprland still has the pointer in the window it was in
                m_pushedAway    = false; // the focused window is the one under the pointer now
                m_pushFocused   = W;     // revealed once the push is over (tick)
                armCanvasTick();
                trace(std::format("push brought {} under the pointer: focused", W->m_class));
            }
        }
        // BACK ON THE WINDOW (user, 2026-11-08: "if I push but then put my
        // mouse back on the window, it should re-center/refocus on that window
        // such that the whole window is visible"): once the push is over, the
        // hand moving on the focused window -- off the edge it pushed -- brings
        // that window back into view, as a click would. Hovering another
        // window already does (the focus hook); the focused one is no focus
        // change, so it is done here.
        void backFromPush(const Vector2D& p, const CBox& screen) {
            if (!m_pushedAway || edgePanRecent() || !m_asCanvas || m_zoomed || !shownNow())
                return;
            if (p.x <= screen.x + 3 || p.y <= screen.y + 3 || p.x >= screen.x + screen.w - 4 || p.y >= screen.y + screen.h - 4)
                return; // still at an edge: maybe pushing on
            if (pointerOnChrome(p))
                return;
            const auto F = Desktop::focusState()->window();
            if (!F || !holds(F))
                return;
            const CBox FB{F->positionAnimation()->value(), F->sizeAnimation()->value()};
            if (!FB.containsPoint(p))
                return;
            m_pushedAway = false;
            trace("back on the window after a push: reveal");
            clickReveal(F);
        }
        bool pushMayStart(int axis, double sign, const Vector2D& cur, const CBox& area) const {
            const auto F  = Desktop::focusState()->window();
            const auto FR = F && holds(F) ? canvasRectOf(F) : std::nullopt;
            if (!FR)
                return false;
            const double VL = axis ? cur.y : cur.x, VR = VL + (axis ? area.h : area.w);
            const double FC = axis ? FR->y + FR->h / 2.0 : FR->x + FR->w / 2.0;
            for (const auto& Q : m_canvas.rects) {
                if (std::abs(Q.x - FR->x) < 0.5 && std::abs(Q.y - FR->y) < 0.5)
                    continue;
                const double QL = axis ? Q.y : Q.x, QR = QL + (axis ? Q.h : Q.w);
                if ((QL + QR) / 2.0 * sign <= FC * sign)
                    continue; // not on that side of the focused window
                if (sign > 0 ? QL >= VR - 0.5 : QR <= VL + 0.5)
                    return true;
            }
            return false;
        }
        // Where `w` sits on the canvas.
        std::optional<MosaicPack::SRect> canvasRectOf(const PHLWINDOW& w) const {
            for (size_t i = 0; i < m_canvasWho.size() && i < m_canvas.rects.size(); ++i)
                if (const auto T = m_canvasWho[i].lock(); T && T->window() == w)
                    return m_canvas.rects[i];
            return std::nullopt;
        }
        // The window showing the most of itself in the view (after a pan).
        PHLWINDOW mostVisible() const {
            const auto AREA = area();
            if (!m_asCanvas || !AREA)
                return nullptr;
            PHLWINDOW best;
            double    bestA = 0;
            for (const auto& E : m_entries)
                if (const auto T = E.target.lock(); T && T->window()) {
                    const CBox B = T->position();
                    const double W = std::min(B.x + B.w, AREA->x + AREA->w) - std::max(B.x, AREA->x), H = std::min(B.y + B.h, AREA->y + AREA->h) - std::max(B.y, AREA->y);
                    if (W > 0 && H > 0 && W * H / std::max(1.0, B.w * B.h) > bestA) {
                        bestA = W * H / std::max(1.0, B.w * B.h);
                        best  = T->window();
                    }
                }
            return best;
        }
        // A SLIVER: w shows less than SLIVER px (logical) of itself on the screen one way
        // or the other -- the few pixels of the next window peeking in at the
        // edge the hand is pushing at (vetoHoverFocus).
        bool isSliver(const PHLWINDOW& w) const {
            constexpr double SLIVER = 120.0; // under a dock, about
            if (!m_asCanvas || m_zoomed || !shownNow() || !w)
                return false;
            const auto WS  = workspace();
            const auto MON = WS ? WS->m_monitor.lock() : PHLMONITOR{};
            if (!MON)
                return false;
            const CBox SCREEN = MON->logicalBox();
            for (const auto& E : m_entries)
                if (const auto T = E.target.lock(); T && T->window() == w) {
                    const CBox   B = T->position();
                    const double W = std::min(B.x + B.w, SCREEN.x + SCREEN.w) - std::max(B.x, SCREEN.x), H = std::min(B.y + B.h, SCREEN.y + SCREEN.h) - std::max(B.y, SCREEN.y);
                    return W > 0 && H > 0 && (W < SLIVER || H < SLIVER);
                }
            return false;
        }
        double visibleShare(const PHLWINDOW& w) const {
            const auto AREA = area();
            if (!AREA || !w)
                return 0;
            for (const auto& E : m_entries)
                if (const auto T = E.target.lock(); T && T->window() == w) {
                    const CBox   B = T->position();
                    const double W = std::min(B.x + B.w, AREA->x + AREA->w) - std::max(B.x, AREA->x), H = std::min(B.y + B.h, AREA->y + AREA->h) - std::max(B.y, AREA->y);
                    return W > 0 && H > 0 ? W * H / std::max(1.0, B.w * B.h) : 0.0;
                }
            return 0;
        }
        PHLWORKSPACE workspace() const {
            return m_parent && m_parent->space() ? m_parent->space()->workspace() : PHLWORKSPACE{};
        }
        std::optional<Vector2D> naturalOf(const PHLWINDOW& w) const {
            for (const auto& E : m_entries)
                if (const auto T = E.target.lock(); T && T->window() == w)
                    return E.natural;
            return std::nullopt;
        }
        bool trustedOf(const PHLWINDOW& w) const {
            for (const auto& E : m_entries)
                if (const auto T = E.target.lock(); T && T->window() == w)
                    return E.trusted;
            return false;
        }

        // THE FIRST CONFIGURE (user, 2026-10-29: "mosaic still spawns zen in a
        // tiny square ... the window border spawns in the right size but the
        // APP itself is in a tiny square ... when i switch to dwindle that
        // doesn't happen"). Hyprland tells a new tiled window its size before
        // it has drawn anything, from this prediction (CWindow::commitWindow on
        // the initial commit). Without one the client got 0x0 and drew at a
        // size of its own, then newTarget gave it its remembered mosaic size
        // (mosaic-sizes) -- the border at that size around a small first frame.
        // Predicted is what newTarget will give it: its app's mosaic size, or
        // the tile it will split off if that does not fit. An app with no
        // remembered size is told nothing: the mosaic keeps the size it asks
        // for (chooseNatural's `desired`), so there is nothing to resize.
        std::optional<Vector2D> predictSizeForNewTarget() override {
            const auto AREA = area();
            if (!AREA)
                return std::nullopt;
            // The window being configured: the one in its initial commit.
            PHLWINDOW w;
            for (const auto& W : Desktop::windowState()->windows())
                if (W && !W->m_isMapped && !W->m_isX11 && W->m_xdgSurface && W->m_xdgSurface->m_initialCommit)
                    w = W;
            if (!w)
                return std::nullopt;
            loadSizes();
            const auto IT = g_sizes.find(!w->m_initialClass.empty() ? w->m_initialClass : w->fetchClass());
            if (IT == g_sizes.end() || IT->second.x < MIN_SIDE || IT->second.y < MIN_SIDE)
                return std::nullopt;
            const Vector2D N = IT->second;
            // The box is the window with its border; the client gets the inside.
            const Vector2D BORDER = Vector2D{2.0, 2.0} * w->getRealBorderSize();
            std::vector<MosaicPack::SRect> sizes;
            for (const auto& E : m_entries)
                if (!E.target.expired())
                    sizes.push_back({0, 0, E.packSize().x, E.packSize().y});
            sizes.push_back({0, 0, N.x, N.y});
            const MosaicPack::SRect AREAR{AREA->x, AREA->y, AREA->w, AREA->h};
            if (canvasOn() || MosaicPack::packEased(sizes, AREAR, looseArea(), innerGap(), nullptr))
                return N - BORDER;
            // The tiles: the leaf it will be (syncTree), worked out on a copy.
            const long long PROBE = m_nextId + 1;
            const auto      WEIGHT = [&](long long id) {
                if (id == PROBE)
                    return N.x * N.y;
                for (const auto& E : m_entries)
                    if (E.id == id)
                        return E.natural.x * E.natural.y;
                return 0.0;
            };
            auto tree = m_tree;
            if (tree.empty()) {
                std::vector<long long> ids;
                std::vector<double>    weights;
                for (const auto& E : m_entries)
                    if (!E.target.expired()) {
                        ids.push_back(E.id);
                        weights.push_back(WEIGHT(E.id));
                    }
                ids.push_back(PROBE);
                weights.push_back(WEIGHT(PROBE));
                tree.buildWeighted(ids, weights);
            } else {
                long long focusedId = -1;
                if (const auto F = Desktop::focusState()->window())
                    for (const auto& E : m_entries)
                        if (const auto T = E.target.lock(); T && T->window() == F)
                            focusedId = E.id;
                tree.insert(PROBE, focusedId, WEIGHT);
            }
            // Hyprland takes gaps_in off each side a tile shares with another.
            if (const auto C = tree.cellOf(PROBE, AREAR, treeOpts())) {
                const double G     = innerGap() / 2.0;
                const auto   INNER = [](double a, double b) { return std::abs(a - b) > 0.5 ? 1.0 : 0.0; };
                const double W     = C->w - G * (INNER(C->x, AREAR.x) + INNER(C->x + C->w, AREAR.x + AREAR.w));
                const double H     = C->h - G * (INNER(C->y, AREAR.y) + INNER(C->y + C->h, AREAR.y + AREAR.h));
                return Vector2D{W, H} - BORDER;
            }
            return std::nullopt;
        }

        // A FULLSCREEN ROUND TRIP PUTS EVERYTHING BACK (MosaicTripHold.hpp).
        // The trip starts: what this desktop was, without the window leaving.
        // Any other window leaving (closed, moved, dragged off) is an edit.
        // A window opening or arriving consumes the hold (backFromTrip).
        void holdForTrip(const SP<Layout::ITarget>& target) {
            const auto W = target ? target->window() : PHLWINDOW{};
            // The traveller's app's own windows leaving are not an edit
            // (MosaicTripHold: a launcher's helper closing while it is away).
            if (MosaicTripHold::isEdit(m_tripHold ? &m_tripHold->keys : nullptr, tripApp(W)))
                ++m_edits;
            const int I = indexOf(target);
            // Not a trip: a hold kept for another window's trip stays, outdated
            // by the edit (backFromTrip will see it and say so).
            if (I < 0 || !W || !Hyprdrive::FullscreenDesktop::carrying(W, false))
                return;
            ++m_edits;
            STripHold hold{.entry = m_entries[sc<size_t>(I)], .tree = m_tree, .tiledHold = m_tiledHold, .dropAt = m_dropAt, .canvasDrop = m_canvasDrop};
            hold.keys.traveller = target.get();
            hold.keys.app       = tripApp(W);
            std::vector<MosaicTripHold::SKey> before;
            for (size_t i = 0; i < sc<size_t>(I); ++i)
                before.push_back(tripKey(m_entries[i]));
            hold.keys.index = MosaicTripHold::placeOf(hold.keys, before);
            for (const auto& E : m_entries)
                if (E.target.lock() != target)
                    hold.keys.rest.push_back(tripKey(E));
            hold.keys.edits = m_edits;
            m_tripHold      = std::move(hold);
            trace(std::format("trip: {} leaves place {} of {}", appOf(W), I, m_entries.size()));
        }
        // It comes home from that trip: back in its place, its tile and the
        // arrangement as they were -- if nothing changed here meanwhile (else
        // false, and it arrives as any window does: the normal rules). A
        // window of its app arriving meanwhile keeps the hold.
        bool backFromTrip(const SP<Layout::ITarget>& target) {
            if (!m_tripHold)
                return false;
            const auto W = target ? target->window() : PHLWINDOW{};
            std::vector<MosaicTripHold::SKey> now;
            for (const auto& E : m_entries)
                now.push_back(tripKey(E));
            // A drop or a swap set an arrangement -- not one the app's own
            // window arriving set (a helper opened at the pointer).
            const auto OWNDROP = [&](const PHLWINDOW& d) { return !MosaicTripHold::isEdit(&m_tripHold->keys, tripApp(d)); };
            const bool ARRANGED = (m_dropAt && !OWNDROP(m_dropAt->target.lock() ? m_dropAt->target.lock()->window() : PHLWINDOW{})) ||
                (m_canvasDrop && !OWNDROP(m_canvasDrop->target.lock() ? m_canvasDrop->target.lock()->window() : PHLWINDOW{}));
            const auto WHY = MosaicTripHold::decide(m_tripHold->keys, target.get(), tripApp(W), W && Hyprdrive::FullscreenDesktop::carrying(W, true), now, m_edits,
                                                    ARRANGED, !m_tripHold->tree.empty());
            if (WHY == MosaicTripHold::EHome::Sibling) {
                trace(std::format("trip: {} (its app's own window) arrives meanwhile: the hold stays", appOf(W)));
                return false;
            }
            auto hold = std::move(*m_tripHold);
            m_tripHold.reset();
            ++m_edits;
            if (WHY != MosaicTripHold::EHome::Restore) {
                if (W && hold.keys.traveller == target.get())
                    trace(std::format("trip: {} home, but its desktop changed meanwhile: it arrives as any window ({})", appOf(W), MosaicTripHold::why(WHY)));
                return false;
            }
            m_dragHold.reset();
            hold.entry.target = target;
            m_entries.insert(m_entries.begin() + sc<std::ptrdiff_t>(MosaicTripHold::homeIndex(hold.keys, now)), hold.entry);
            m_tree        = std::move(hold.tree);
            m_tiledHold   = hold.tiledHold;
            m_dropAt      = hold.dropAt;
            m_canvasDrop  = hold.canvasDrop;
            trace(std::format("trip: {} home to place {} of {}", appOf(W), indexOf(target), m_entries.size()));
            return true;
        }
        static MosaicTripHold::SApp tripApp(const PHLWINDOW& w) {
            return w ? MosaicTripHold::SApp{.pid = w->getPID(), .cls = appOf(w)} : MosaicTripHold::SApp{};
        }
        static MosaicTripHold::SKey tripKey(const SMosaicEntry& e) {
            const auto T = e.target.lock();
            return {.who = T.get(), .w = e.natural.x, .h = e.natural.y, .id = e.id, .app = tripApp(T ? T->window() : PHLWINDOW{})};
        }

        void newTarget(SP<Layout::ITarget> target) override {
            // First: placed as what it is, a fullscreen window keeps its box.
            carryFullscreenInto(target, this);
            m_revealPending = true;
            if (backFromTrip(target)) {
                recalculate();
                return;
            }
            // A Super+drag let go is the same window put down, not one arriving:
            // the tiles keep their hold (m_tiledHold) through it.
            const bool DROP = droppingNow();
            m_dragHold.reset();
            if (!DROP)
                m_tiledHold = false;
            forgetArrangement();
            arriveWithNatural(target, true);
            placeInTiles(target, std::nullopt, DROP);
            placeDropped(target, std::nullopt);
            // A window the user did not ask for is not put where they are
            // working (Intent.hpp): the packer finds it a place.
            if (!DROP && !Intent::quiet(target->window()))
                placeAtCursor(target);
            recalculate();
        }

        // A NEW WINDOW OPENS AT THE POINTER (user, 2026-11-04: "spawn window
        // nearest to my cursor in mosaic modes so it lands where i intend it
        // to"). The mosaic and the canvas take it as if it were dropped there
        // (PackHint / SCanvasDrop): it lands at the pointer, or on that side of
        // the cluster, everyone else packed around it. Only with the pointer on
        // this desktop's screen; the tiles keep dwindle's own rule
        // (placeInTiles), and the first window still centres.
        void placeAtCursor(const SP<Layout::ITarget>& target) {
            if (!Hyprdrive::Config::getValue<bool>("plugin:hyprdrive:mosaic:spawn_at_cursor") || !m_tree.empty() || m_entries.size() < 2)
                return;
            const auto AREA = area();
            if (!AREA || indexOf(target) < 0)
                return;
            const Vector2D MOUSE = g_pInputManager->getMouseCoordsInternal();
            if (MOUSE.x < AREA->x || MOUSE.y < AREA->y || MOUSE.x > AREA->x + AREA->w || MOUSE.y > AREA->y + AREA->h)
                return;
            trace(std::format("spawn {} at the pointer {}x{}", appOf(target->window()), MOUSE.x, MOUSE.y));
            m_dropAt = SDropAt{target, MOUSE};
        }

        void movedTarget(SP<Layout::ITarget> target, std::optional<Vector2D> focalPoint = std::nullopt) override {
            m_revealPending = true;
            if (indexOf(target) < 0 && backFromTrip(target)) {
                recalculate();
                return;
            }
            const bool DROP = droppingNow();
            m_dragHold.reset();
            if (indexOf(target) < 0) {
                forgetArrangement();
                arriveWithNatural(target, false);
                placeInTiles(target, focalPoint, DROP);
                placeDropped(target, focalPoint);
            }
            recalculate();
        }

        // Hyprland is putting down a window a Super+drag carried.
        static bool droppingNow() {
            return g_layoutManager && g_layoutManager->dragController()->wasDraggingWindow();
        }

        // THE TILES TAKE A WINDOW AS DWINDLE DOES (user, 2026-10-23: "when
        // mosaic goes to dwindle (hybrid) it should behave exactly like
        // dwindle ... I can't swap positions of all windows with super+drag").
        // DwindleAlgorithm::addTarget, with the user's dwindle options: the tile
        // it splits (OPENINGON) and which half it takes. A window let go after a
        // Super+drag splits the tile under the pointer, on the half the pointer
        // is in -- it used to split the newest tile wherever it was dropped. A
        // window moved by direction splits the tile at its focal point; a new
        // window splits the focused one (use_active_for_splits) on the side
        // force_split says. preselect and smart_split as dwindle has them.
        // Only while the tiles stand: in the mosaic the packer places everyone.
        void placeInTiles(const SP<Layout::ITarget>& target, std::optional<Vector2D> focal, bool drop) {
            const long long PICKED = std::exchange(m_dragLeaf, -1);
            if (m_tree.empty())
                return;
            const int  I    = indexOf(target);
            const auto AREA = tilesArea();
            if (I < 0 || !AREA)
                return;
            const auto      O     = treeOpts();
            const Vector2D  MOUSE = g_pInputManager->getMouseCoordsInternal();
            const Vector2D  AT    = focal.value_or(MOUSE);
            long long       near  = -1;
            if (PICKED >= 0 && m_tree.has(PICKED)) {
                // THE DRAG HOLD kept every other tile where it was, and this
                // window's own tile empty. Let go over that hole (or nowhere
                // else): it goes back into its own tile, sizes and all.
                if (drop) {
                    near = m_tree.leafAt(MOUSE.x, MOUSE.y, *AREA, O);
                    if (near == PICKED || near < 0) {
                        m_entries[sc<size_t>(I)].id = PICKED;
                        return;
                    }
                }
                m_tree.remove(PICKED);
            }

            static auto PUSEACTIVE  = CConfigValue<::Config::INTEGER>("dwindle:use_active_for_splits");
            static auto PFORCESPLIT = CConfigValue<::Config::INTEGER>("dwindle:force_split");
            static auto PSMARTSPLIT = CConfigValue<::Config::INTEGER>("dwindle:smart_split");
            static auto PPRECISE    = CConfigValue<::Config::INTEGER>("dwindle:precise_mouse_move");
            static auto PPERMANENT  = CConfigValue<::Config::INTEGER>("dwindle:permanent_direction_override");

            if (near < 0) {
                if (focal || drop)
                    near = m_tree.leafAt(AT.x, AT.y, *AREA, O);
                else if (*PUSEACTIVE) {
                    // The focused window, if it is a tile here and not this one.
                    if (const auto F = Desktop::focusState()->window(); F && F != target->window())
                        for (const auto& E : m_entries)
                            if (const auto T = E.target.lock(); T && T->window() == F && m_tree.has(E.id))
                                near = E.id;
                    if (near < 0)
                        near = m_tree.leafAt(MOUSE.x, MOUSE.y, *AREA, O);
                } else
                    near = m_tree.leafAt(MOUSE.x, MOUSE.y, *AREA, O);
            }

            const MosaicPack::SRect BOX    = m_tree.cellOf(near, *AREA, O).value_or(*AREA);
            bool                    first  = false;
            int                     orient = -1;
            if (m_overrideDir != Math::DIRECTION_DEFAULT) {
                orient = m_overrideDir == Math::DIRECTION_UP || m_overrideDir == Math::DIRECTION_DOWN ? 1 : 0;
                first  = m_overrideDir == Math::DIRECTION_UP || m_overrideDir == Math::DIRECTION_LEFT;
                if (!*PPERMANENT)
                    m_overrideDir = Math::DIRECTION_DEFAULT;
            } else if (*PSMARTSPLIT || (*PPRECISE && drop)) {
                const double DX = AT.x - (BOX.x + BOX.w / 2.0), DY = AT.y - (BOX.y + BOX.h / 2.0);
                if (BOX.w > 0 && std::abs(DX) > 1e-9 && std::abs(DY / DX) < BOX.h / BOX.w) {
                    orient = 0;
                    first  = DX <= 0;
                } else {
                    orient = 1;
                    first  = DY <= 0;
                }
            } else if (*PFORCESPLIT == 0 || focal || drop) {
                static auto PMULT = CConfigValue<::Config::FLOAT>("dwindle:split_width_multiplier");
                const bool  SIDE  = BOX.w > BOX.h * *PMULT;
                first             = SIDE ? AT.x < BOX.x + BOX.w / 2.0 : AT.y < BOX.y + BOX.h / 2.0;
            } else
                first = *PFORCESPLIT == 1;

            // The split takes the two windows' mosaic areas (the tiles look
            // like the mosaic, CDwindleTree::buildWeighted), where dwindle halves.
            const auto AREAOF = [&](long long id) {
                for (const auto& E : m_entries)
                    if (E.id == id)
                        return std::max(0.0, E.natural.x * E.natural.y);
                return 0.0;
            };
            const double WN = AREAOF(m_entries[sc<size_t>(I)].id), WO = AREAOF(near);
            const double SHARE =
                WN + WO > 0 ? std::clamp(WN / (WN + WO), MosaicPack::CDwindleTree::WEIGHT_MIN, 1.0 - MosaicPack::CDwindleTree::WEIGHT_MIN) : 0.5;
            m_tree.insertAt(m_entries[sc<size_t>(I)].id, near, first, SHARE, orient, *AREA, O);
        }

        // The tiles' space, and the dwindle options they follow.
        std::optional<MosaicPack::SRect> tilesArea() const {
            const auto A = area();
            if (!A)
                return std::nullopt;
            return MosaicPack::SRect{A->x, A->y, A->w, A->h};
        }
        static MosaicPack::STreeOpts treeOpts() {
            static auto PPRESERVE   = CConfigValue<::Config::INTEGER>("dwindle:preserve_split");
            static auto PSMARTSPLIT = CConfigValue<::Config::INTEGER>("dwindle:smart_split");
            static auto PPRECISE    = CConfigValue<::Config::INTEGER>("dwindle:precise_mouse_move");
            static auto PMULT       = CConfigValue<::Config::FLOAT>("dwindle:split_width_multiplier");
            return MosaicPack::STreeOpts{.preserve = *PPRESERVE || *PSMARTSPLIT || *PPRECISE, .widthMult = std::max(0.01, sc<double>(*PMULT))};
        }
        long long idOf(const SP<Layout::ITarget>& t) const {
            const int I = indexOf(t);
            return I < 0 ? -1 : m_entries[sc<size_t>(I)].id;
        }
        SP<Layout::ITarget> targetOfId(long long id) const {
            for (const auto& E : m_entries)
                if (E.id == id)
                    return E.target.lock();
            return nullptr;
        }

        // DROP TO REARRANGE (MosaicPack::PackHint). A Super+drag floats the
        // window for the drag; letting go hands it back here, at the end of
        // the order. When it came back from a drag (or with a focal point), the
        // point it was let go at goes to pack() as the hint: the window lands
        // on that side of the cluster (or at the point, dropped inside it),
        // still one tight cluster. The order itself is not touched -- the
        // packer is any-order.
        void placeDropped(const SP<Layout::ITarget>& target, std::optional<Vector2D> focalPoint) {
            if (!focalPoint && !(g_layoutManager && g_layoutManager->dragController()->wasDraggingWindow()))
                return;
            const int I = indexOf(target);
            if (I < 0 || m_entries.size() < 2)
                return;
            const auto AREA = area();
            if (!AREA)
                return;
            const Vector2D AT = focalPoint.value_or(g_pInputManager->getMouseCoordsInternal());
            trace(std::format("drop {} at {}x{}", appOf(target->window()), AT.x, AT.y));
            m_dropAt = SDropAt{target, AT};
        }

        // A window this session already gave a mosaic size comes back at that
        // size, as it was: it is not new, and resizing it
        // here would overwrite the size it is meant to keep.
        void arriveWithNatural(const SP<Layout::ITarget>& target, bool fresh) {
            const auto N = naturalFor(target, fresh);
            arrive(target, Vector2D{N.size.w, N.size.h}, N.trusted);
        }

        // A WINDOW ARRIVES (user, 2026-10-14: "opening new windows ... seems to
        // continue shrinking mosaic windows ... I said this should never
        // happen. Existing windows should not be resized"). Nobody already
        // there changes size -- and NEITHER DOES THE NEWCOMER (user,
        // 2026-10-15: "it should never spawn a window smaller than my default
        // - at that point it should go into dwindle"; it used to come in down
        // to half its size to fit). It keeps its size; if it does not fit, the
        // desktop tiles. The eased margin and any-order rows/columns
        // (MosaicPack) are what keep the tiles from coming too easily.
        void arrive(const SP<Layout::ITarget>& target, Vector2D want, bool trusted = true) {
            m_entries.push_back({target, want, ++m_nextId, trusted});
            trace(std::format("arrive {} want={}x{}{}", appOf(target ? target->window() : PHLWINDOW{}), want.x, want.y, fits() ? "" : " -> tiles"));
        }

        // Does everyone fit as a mosaic at their own sizes?
        bool fits() const {
            const auto AREA = area();
            if (!AREA)
                return false;
            std::vector<MosaicPack::SRect> sizes;
            for (const auto& E : m_entries)
                sizes.push_back({0, 0, E.packSize().x, E.packSize().y});
            return MosaicPack::fitsEased(sizes, MosaicPack::SRect{AREA->x, AREA->y, AREA->w, AREA->h}, looseArea(), innerGap());
        }

        void removeTarget(SP<Layout::ITarget> target) override {
            if (m_resize) {
                setDim(false, *m_resize);
                m_resize.reset();
            }
            // Pulled out by a Super+drag: not a window leaving -- the tiles keep
            // their hold, and its tile waits for it (placeInTiles).
            const bool DRAGGED = target && g_layoutManager && g_layoutManager->dragController()->target() == target;
            if (!DRAGGED)
                holdForTrip(target);
            if (!DRAGGED)
                m_tiledHold = false;
            forgetArrangement();
            leftMosaic(target);
            if (DRAGGED)
                m_dragLeaf = m_tree.empty() ? -1 : idOf(target);
            std::erase_if(m_entries, [&](const SMosaicEntry& e) { return !e.target || e.target.lock() == target; });
            // Pulled out by a drag: the others hold still until it is let go.
            if (DRAGGED) {
                m_dragHold = target;
                return;
            }
            // CLOSED WHILE ZOOMED OUT (user, 2026-10-29: Super+W "cancels the
            // zoom ... without exiting the zoom state so I can easily close what
            // I don't want"): the zoom stays, and the others glide from where
            // they are drawn to their new places, as after a drop.
            if (m_asCanvas && m_zoomed && !m_zDrag.lock()) {
                m_morph.from.clear();
                for (const auto& WT : m_canvasWho)
                    if (const auto T = WT.lock(); T && T != target && T->window())
                        if (const auto B = zoomBoxOf(T->window(), m_zoomZ))
                            m_morph.from.emplace_back(T->window(), *B);
                m_keepZoom = true;
                recalculate();
                m_keepZoom = false;
                if (m_asCanvas && m_zoomed) {
                    m_zoomN = liveEntries();
                    if (const auto VIEW = canvasViewNow(); VIEW && (VIEW->maxX > 1.0 || VIEW->maxY > 1.0)) {
                        layoutCanvas(true);
                        m_morph.start = std::chrono::steady_clock::now();
                        m_morph.ms    = std::max(1.0, canvasPanMs());
                        m_morph.on    = true;
                        drawZoom();
                        armCanvasTick();
                        return;
                    }
                }
                // What is left fits the screen (or left the canvas): nothing
                // to zoom out to.
                if (m_asCanvas && m_zoomed)
                    zoomIn();
                else
                    endZoomNow();
                return;
            }
            recalculate();
        }

        // The drag that pulled a window out is over (or took it to another
        // desktop): the cluster may close up now.
        bool holdingFor() const {
            return !m_dragHold.expired();
        }
        void releaseDragHold() {
            if (m_dragHold.expired())
                return;
            m_dragHold.reset();
            m_dragLeaf = -1; // let go elsewhere: its tile goes (syncTree)
            recalculate();
        }
        // Still in the air over this desktop?
        bool dragHoldLive() const {
            const auto T = m_dragHold.lock();
            if (!T || !g_layoutManager || g_layoutManager->dragController()->target() != T)
                return false;
            const auto W = T->window();
            return W && W->m_workspace && W->m_workspace == workspace();
        }

        // A RESIZE in the mosaic (user, 2026-10-14: "if I resize a window beyond
        // what the current arrangement supports, it should never force other
        // windows to get smaller"). For the length of the drag the other windows
        // are FROZEN where they are, and the resized one grows from its fixed
        // opposite corner up to its CALCULATED LIMIT: the largest size at which
        // some mosaic arrangement of everyone, at their current sizes, still
        // fits (MosaicPack::limitFor -- worked out, not shown). Pushed past the
        // limit, the window stops growing and the rest of the desktop darkens:
        // letting go there tiles, keeping sizes as well as it can. Letting go
        // anywhere else applies the limit's arrangement.
        //
        // In the TILES it is dwindle's resize, live (2026-10-23); letting go
        // lets the window's mosaic size shrink toward its tile -- so shrinking
        // windows in the tiles is the way back to the mosaic once everyone
        // fits again.
        void resizeTarget(const Vector2D& delta, SP<Layout::ITarget> target, Layout::eRectCorner corner = Layout::CORNER_NONE) override {
            const int I = indexOf(target);
            if (I < 0)
                return;
            if (MosaicTripHold::isEdit(m_tripHold ? &m_tripHold->keys : nullptr, tripApp(target ? target->window() : PHLWINDOW{})))
                ++m_edits;
            // IN THE TILES, DWINDLE'S RESIZE (user, 2026-10-23: "behave exactly
            // like dwindle"): live, the splits DwindleAlgorithm::resizeTarget
            // moves (dwindle:smart_resizing or not), every tile following.
            // Letting go still lets the window's mosaic size shrink toward its
            // tile (finishResize).
            const Vector2D PRESS = g_pInputManager->getMouseCoordsInternal() - delta;
            if (!m_tree.empty() && !m_asMosaic) {
                if (!m_resize || m_resize->target.lock() != target)
                    beginResize(target, corner, PRESS);
                const auto AREA = tilesArea();
                if (!m_resize || !AREA)
                    return;
                // A DRAG FREEZES THE TILES TOO (user, 2026-10-27: "when I
                // expand a window I want everything else in the cluster or
                // tiled mode in Mosaic to stay still"): the tile follows the
                // pointer over its still neighbours, drawn on top; letting go
                // moves dwindle's split by the whole way at once (finishResize)
                // and the tiles settle. A keyboard resize stays dwindle's step.
                if (corner != Layout::CORNER_NONE) {
                    const Vector2D P = g_pInputManager->getMouseCoordsInternal() + m_resize->grabOffset;
                    const CBox&    S = m_resize->start;
                    double         x0 = S.x, y0 = S.y, x1 = S.x + S.w, y1 = S.y + S.h;
                    if (Layout::edgeLeft(corner))
                        x0 = std::clamp(P.x, AREA->x, x1 - MIN_SIDE);
                    if (Layout::edgeRight(corner))
                        x1 = std::clamp(P.x, x0 + MIN_SIDE, AREA->x + AREA->w);
                    if (Layout::edgeTop(corner))
                        y0 = std::clamp(P.y, AREA->y, y1 - MIN_SIDE);
                    if (Layout::edgeBottom(corner))
                        y1 = std::clamp(P.y, y0 + MIN_SIDE, AREA->y + AREA->h);
                    if (!m_resize->tileFrozen)
                        if (const auto W = target->window())
                            Desktop::windowState()->raise(W);
                    m_resize->tileFrozen = true;
                    m_resize->tileBox    = CBox{std::round(x0), std::round(y0), std::round(x1 - x0), std::round(y1 - y0)};
                    m_resize->shown      = m_resize->tileBox.size();
                    trace(std::format("tile drag P={}x{} box={}x{} {}x{}", P.x, P.y, m_resize->tileBox.x, m_resize->tileBox.y, m_resize->tileBox.w, m_resize->tileBox.h));
                    applyTileResize();
                    return;
                }
                static auto PSMART = CConfigValue<::Config::INTEGER>("dwindle:smart_resizing");
                const MosaicPack::CDwindleTree::SEdges EDGES{
                    .left = Layout::edgeLeft(corner), .right = Layout::edgeRight(corner), .top = Layout::edgeTop(corner), .bottom = Layout::edgeBottom(corner)};
                m_tree.resizeDwindle(m_entries[sc<size_t>(I)].id, delta.x, delta.y, EDGES, *PSMART != 0, *AREA, treeOpts());
                layoutTiles();
                // A drag snaps every tile, as dwindle does (no animation
                // chasing the pointer).
                if (g_layoutManager && g_layoutManager->dragController()->target() == target)
                    for (const auto& E : m_entries)
                        if (const auto T = E.target.lock())
                            T->warpPositionSize();
                if (corner == Layout::CORNER_NONE)
                    finishResize();
                return;
            }
            // The window grows toward the corner being dragged: a left or top
            // corner pulled outward is a BIGGER window, not a smaller one. No
            // corner (a keyboard resizeactive): the delta is the size change.
            Vector2D grow = delta;
            if (corner != Layout::CORNER_NONE) {
                grow.x = Layout::edgeLeft(corner) ? -delta.x : delta.x;
                grow.y = Layout::edgeTop(corner) ? -delta.y : delta.y;
            }

            if (!m_resize || m_resize->target.lock() != target)
                beginResize(target, corner, PRESS);
            if (!m_resize)
                return;
            if (corner == Layout::CORNER_NONE)
                m_resize->requested = m_resize->requested + grow;
            else
                requestFromPointer();
            stepResize();
            // A keyboard resize is one step: settle at once.
            if (corner == Layout::CORNER_NONE)
                finishResize();
        }

        // A DRAG follows the POINTER, measured from the corner that stays put --
        // not a sum of deltas, which can only drift. The grab's own offset from
        // the corner is kept, and so is how far the view was pushed past the
        // screen's edge (reach).
        void requestFromPointer() {
            if (!m_resize)
                return;
            const auto     corner = m_resize->corner;
            const Vector2D P      = g_pInputManager->getMouseCoordsInternal() + m_resize->grabOffset + m_resize->reach;
            const CBox&    S      = m_resize->start;
            const double   AX = Layout::edgeLeft(corner) ? S.x + S.w : S.x, AY = Layout::edgeTop(corner) ? S.y + S.h : S.y;
            // An EDGE (user, 2026-10-15) moves one side only: the other axis
            // stays at its size.
            const bool HORIZ    = corner & (Layout::CORNER_LEFT | Layout::CORNER_RIGHT), VERT = corner & (Layout::CORNER_TOP | Layout::CORNER_BOTTOM);
            m_resize->requested = Vector2D{HORIZ ? std::max(MIN_SIDE, Layout::edgeLeft(corner) ? AX - P.x : P.x - AX) : S.w,
                                           VERT ? std::max(MIN_SIDE, Layout::edgeTop(corner) ? AY - P.y : P.y - AY) : S.h};
        }
        // PAST THE SCREEN'S EDGE (user, 2026-10-27: "if I expand toward the
        // edge of the canvas/screen, it should let me move beyond the
        // boundaries of the canvas while I expand the window"). With the canvas
        // on, the mouse moving on past an outer edge of the screen slides the
        // whole view -- the frozen windows and the one growing -- by as much as
        // it moved (onPointerDelta), so the side being dragged keeps going past
        // it, up to a screen-sized window. Let
        // go, the desktop packs (onto the canvas if it no longer fits) with the
        // window in view.
        bool resizeCanReach() const {
            return m_resize && !m_resize->tiled && m_resize->corner != Layout::CORNER_NONE && canvasOn();
        }
        void resizeReach(const Vector2D& d) {
            if (!resizeCanReach())
                return;
            m_resize->reach = m_resize->reach + d;
            requestFromPointer();
            stepResize();
            // Grown as far as it may (screen sized): the view stops with it --
            // the slide gives back what the window could not take.
            if (!m_resize)
                return;
            const Vector2D OVER{std::max(0.0, m_resize->requested.x - m_resize->shown.x), std::max(0.0, m_resize->requested.y - m_resize->shown.y)};
            if (OVER.x > 0.5 || OVER.y > 0.5) {
                m_resize->reach = m_resize->reach - Vector2D{d.x > 0 ? OVER.x : d.x < 0 ? -OVER.x : 0.0, d.y > 0 ? OVER.y : d.y < 0 ? -OVER.y : 0.0};
                requestFromPointer();
                stepResize();
            }
        }

        // The drag ended (Mosaic::onButton / onMove see the drag controller let go).
        bool resizing() const {
            return m_resize.has_value();
        }
        bool resizingTarget(const SP<Layout::ITarget>& t) const {
            return m_resize && m_resize->target.lock() == t;
        }
        void finishResize() {
            if (!m_resize)
                return;
            const auto SESSION = *m_resize;
            m_resize.reset();
            setDim(false, SESSION);

            const int I = indexOf(SESSION.target.lock());
            if (SESSION.tiled && SESSION.tileFrozen && I >= 0) {
                // The frozen drag lands: dwindle's split moves by the whole way
                // each dragged side went, as its live resize would have.
                const auto AREA = tilesArea();
                const CBox &S = SESSION.start, &B = SESSION.tileBox;
                const auto C = SESSION.corner;
                const double DX = Layout::edgeLeft(C) ? B.x - S.x : Layout::edgeRight(C) ? (B.x + B.w) - (S.x + S.w) : 0.0;
                const double DY = Layout::edgeTop(C) ? B.y - S.y : Layout::edgeBottom(C) ? (B.y + B.h) - (S.y + S.h) : 0.0;
                static auto  PSMART = CConfigValue<::Config::INTEGER>("dwindle:smart_resizing");
                if (AREA && (std::abs(DX) >= 1 || std::abs(DY) >= 1)) {
                    const MosaicPack::CDwindleTree::SEdges EDGES{.left = Layout::edgeLeft(C), .right = Layout::edgeRight(C), .top = Layout::edgeTop(C), .bottom = Layout::edgeBottom(C)};
                    m_tree.resizeDwindle(m_entries[sc<size_t>(I)].id, DX, DY, EDGES, *PSMART != 0, *AREA, treeOpts());
                }
                layoutTiles();
            }
            if (SESSION.tiled) {
                // A tile let go: its dwindle split moves to the size it was
                // dragged to. Its mosaic size may only SHRINK toward it, per
                // side (user, 2026-10-15: "mosaic ... wont leave tiled mode" --
                // tiles are big, so taking the tile's size as the mosaic size
                // made Zen 1911x1209 and the desktop could never fit again).
                // Shrinking a tile is the way out of the tiles; growing one no
                // longer locks the desktop in them.
                if (I >= 0) {
                    // The splits already moved, live (resizeTarget); the size
                    // the tile ended at is the one the mosaic may shrink to.
                    const auto     T     = SESSION.target.lock();
                    const Vector2D SHOWN = T ? T->position().size() : SESSION.shown;
                    auto&          n     = m_entries[sc<size_t>(I)].natural;
                    n                    = Vector2D{std::max(MIN_SIDE, std::min(n.x, SHOWN.x)), std::max(MIN_SIDE, std::min(n.y, SHOWN.y))};
                    // A tile's size is the tiles', not the app's.
                    m_entries[sc<size_t>(I)].trusted = false;
                }
                if (m_tiledHold && fits())
                    m_tiledHold = false;
            } else {
                // Everyone keeps the size they were frozen at, so the next pass
                // finds the arrangement the limit already proved exists.
                for (size_t k = 0; k < m_entries.size(); ++k) {
                    const auto T = m_entries[k].target.lock();
                    for (const auto& [WT, BOX] : SESSION.frozen)
                        if (WT.lock() == T && T)
                            m_entries[k].natural = Vector2D{std::max(MIN_SIDE, BOX.w), std::max(MIN_SIDE, BOX.h)};
                }
                if (I >= 0) {
                    // The mosaic keeps the size the window had when the tiles
                    // took over -- its limit -- so it comes back exactly so.
                    m_entries[sc<size_t>(I)].natural = SESSION.shown;
                    // The user sized it in the mosaic: that is its size now.
                    m_entries[sc<size_t>(I)].trusted = true;
                    // Past the limit: tiles, until shrinking a tile (or a window
                    // leaving or arriving) lets the mosaic fit again.
                    if (SESSION.past)
                        m_tiledHold = true;
                }
            }
            // With the canvas on, the window just sized is brought into view.
            if (!SESSION.tiled && canvasOn())
                m_revealPending = true;
            trace(std::format("finish past={} shown={}x{} requested={}x{} reach={}x{}", SESSION.past, SESSION.shown.x, SESSION.shown.y, SESSION.requested.x,
                              SESSION.requested.y, SESSION.reach.x, SESSION.reach.y));
            recalculate();
        }

        void recalculate(Layout::eRecalculateReason reason = Layout::RECALCULATE_REASON_UNKNOWN) override {
            (void)reason;
            // Whatever comes next is laid out for real: the zoom lets go first --
            // unless a drag in the zoomed-out view is what re-arranged it, or a
            // screenshot's region picker opening (a new layer re-lays out the
            // monitor) while the zoomed-out canvas is what is being shot.
            if (!m_keepZoom && !zoomSurvives())
                endZoomNow();
            std::erase_if(m_entries, [](const SMosaicEntry& e) { return !e.target; });
            // Every window's mosaic size outlives this layout (g_windowSizes).
            for (const auto& E : m_entries)
                if (const auto T = E.target.lock())
                    rememberWindow(T->window(), E.natural, E.trusted);
            if (m_entries.empty() || !m_parent || !m_parent->space())
                return;

            // A fullscreen window owns the desktop: everything else is the
            // fullscreen handler's to place, as in every built-in layout.
            const auto SPACE = m_parent->space();
            if (Fullscreen::controller()->hasFullscreen(SPACE->workspace(), true)) {
                m_defaultFullscreenHandler->syncTargetSizeAndPosition();
                return;
            }

            // A window in the air (the drag hold): everyone stays put.
            if (dragHoldLive())
                return;

            // Mid-resize in the mosaic, the frozen arrangement stands, whatever
            // asked. (A tile resizes live: the tiles below lay it out.)
            if (m_resize && !m_resize->tiled) {
                applyResize();
                return;
            }
            if (m_resize && m_resize->tileFrozen) {
                applyTileResize();
                return;
            }

            const auto AREAOPT = area();
            if (!AREAOPT)
                return;
            const CBox AREA = *AREAOPT;

            std::vector<MosaicPack::SRect> sizes;
            sizes.reserve(m_entries.size());
            for (const auto& E : m_entries)
                sizes.push_back({0, 0, E.packSize().x, E.packSize().y});

            const MosaicPack::SRect AREAR{AREA.x, AREA.y, AREA.w, AREA.h};
            m_asMosaic = false;
            const bool WASCANVAS = m_asCanvas;
            m_asCanvas = false;
            // After a resize pushed past its limit the desktop stays tiled until
            // a window arrives or leaves (m_tiledHold).
            std::optional<MosaicPack::PackHint> hint;
            if (m_dropAt)
                if (const int I = indexOf(m_dropAt->target.lock()); I >= 0)
                    hint = MosaicPack::PackHint{.idx = sc<size_t>(I), .x = m_dropAt->at.x, .y = m_dropAt->at.y};
            // m_entries stays in ARRIVAL order. pack() is any-order but not
            // order-blind (ties), so the same list in the same order is the same
            // cluster on every pass; re-sorting it by what came back (the old
            // regrouped order) made a different cluster in 139 of 348 bench sets.
            auto PLACED = m_tiledHold ? std::nullopt : MosaicPack::packEased(sizes, AREAR, looseArea(), innerGap(), hint ? &*hint : nullptr);
            traceEntries(PLACED ? "mosaic" : "tiles");
            if (PLACED) {
                m_asMosaic = true;
                // Back in the mosaic: the next time the tiles take over they
                // start from a fresh, balanced tree.
                m_tree.clear();
                // THE MOSAIC: each window exactly where the pack put it. A
                // non-empty visual box is placed as given -- Hyprland adds no
                // tile gaps to it -- and the logical box is the same rect.
                for (size_t i = 0; i < m_entries.size(); ++i) {
                    const auto& R = (*PLACED)[i];
                    const CBox  BOX{R.x, R.y, R.w, R.h};
                    if (const auto T = m_entries[i].target.lock())
                        T->setPositionGlobal(Layout::STargetBox{.logicalBox = BOX, .visualBox = BOX});
                }
                return;
            }

            // THE INFINITE CANVAS instead of the tiles (mosaic:infinite_canvas):
            // everyone at their mosaic size, past the screen, the view panned.
            if (canvasOn()) {
                m_tree.clear();
                m_tiledHold = false;
                arrangeCanvas(WASCANVAS);
                return;
            }

            // THE TILES (user, 2026-10-14): a dwindle tree (MosaicPack::
            // CDwindleTree), each split where the user left it. Plain cells, so Hyprland insets them by gaps_in exactly
            // as it does dwindle's. They never touch `natural`: the mosaic that
            // comes back has every window as it was.
            trace(std::format("tiles n={} hold={}", m_entries.size(), m_tiledHold));
            layoutTiles();
        }

        void arrangeCanvas(bool wasCanvas) {
            const auto AREA = area();
            if (!AREA)
                return;
            const MosaicPack::SRect AREAR{AREA->x, AREA->y, AREA->w, AREA->h};
            // A new arrangement: the zoom (drawn from the old one) ends. A pan
            // in flight carries on if nothing moved (below).
            if (!m_keepZoom && !zoomSurvives())
                endZoomNow();
            std::vector<MosaicPack::SRect> sizes;
            for (const auto& E : m_entries)
                sizes.push_back({0, 0, E.packSize().x, E.packSize().y});
            // A WINDOW LET GO ON THE CANVAS lands where it was let go and the
            // cluster packs around it, as a drop in the plain mosaic does
            // (MosaicCanvas.hpp SCanvasDrop -> PackHint).
            // Worked out once, on the canvas it was dropped on, and kept --
            // like the plain mosaic's m_dropAt -- until a window arrives or
            // leaves (forgetArrangement): every re-layout until then packs the
            // same way, instead of the next one forgetting the drop.
            if (m_dropAt && !m_canvasDrop) {
                const int I = indexOf(m_dropAt->target.lock());
                if (I >= 0 && wasCanvas && m_canvas.w > 0) {
                    const auto OLD = MosaicPack::canvasView(m_canvas.w, m_canvas.h, AREAR);
                    m_canvasDrop   = SCanvasDropAt{m_dropAt->target, MosaicPack::SCanvasDrop{.x    = m_dropAt->at.x - (OLD.originX - m_canvasOffset.x),
                                                                                              .y    = m_dropAt->at.y - (OLD.originY - m_canvasOffset.y),
                                                                                              .oldW = m_canvas.w,
                                                                                              .oldH = m_canvas.h}};
                    trace(std::format("canvas drop at canvas {}x{}", m_canvasDrop->drop.x, m_canvasDrop->drop.y));
                } else if (I >= 0 && !wasCanvas) {
                    // The mosaic is spilling onto the canvas right now (the window
                    // that arrived no longer fits, placeAtCursor): there is no old
                    // canvas to read the point off, but the old view was the screen
                    // itself, so the point is taken on a canvas the screen's size.
                    m_canvasDrop = SCanvasDropAt{m_dropAt->target, MosaicPack::SCanvasDrop{.x    = m_dropAt->at.x - AREAR.x,
                                                                                            .y    = m_dropAt->at.y - AREAR.y,
                                                                                            .oldW = AREAR.w,
                                                                                            .oldH = AREAR.h}};
                    trace(std::format("canvas drop (from the mosaic) at {}x{}", m_canvasDrop->drop.x, m_canvasDrop->drop.y));
                }
            }
            std::optional<MosaicPack::SCanvasDrop> drop;
            if (m_canvasDrop)
                if (const int I = indexOf(m_canvasDrop->target.lock()); I >= 0) {
                    drop      = m_canvasDrop->drop;
                    drop->idx = sc<size_t>(I);
                }
            const auto FOCUS = Desktop::focusState()->window();
            const auto OLDRECTS = m_canvas.rects;
            const auto OLDWHO   = m_canvasWho;
            m_canvas         = MosaicPack::packCanvasMemo(m_canvasMemo, sizes, AREAR, looseArea(), innerGap(), canvasAllDirections(), drop ? &*drop : nullptr);
            m_canvasWho.clear();
            for (const auto& E : m_entries)
                m_canvasWho.push_back(E.target);
            m_asCanvas = true;
            trace(std::format("canvas n={} screens={}x{} size={}x{}", m_entries.size(), m_canvas.screensX, m_canvas.screensY, m_canvas.w, m_canvas.h));
            if (!wasCanvas)
                m_canvasOffset = {};
            // NOTHING MOVED ON THE CANVAS (a re-layout Hyprland asked for, the
            // same windows packed the same way): only the view may move, and
            // it eases there (panTo) instead of every window riding Hyprland's
            // own move animation -- which the layout motion blur smeared.
            bool same = wasCanvas && OLDRECTS.size() == m_canvas.rects.size() && OLDWHO.size() == m_canvasWho.size();
            for (size_t i = 0; same && i < OLDRECTS.size(); ++i)
                same = OLDWHO[i].lock() == m_canvasWho[i].lock() && std::abs(OLDRECTS[i].x - m_canvas.rects[i].x) < 0.5 && std::abs(OLDRECTS[i].y - m_canvas.rects[i].y) < 0.5 &&
                    std::abs(OLDRECTS[i].w - m_canvas.rects[i].w) < 0.5 && std::abs(OLDRECTS[i].h - m_canvas.rects[i].h) < 0.5;
            const bool REVEAL = !wasCanvas || std::exchange(m_revealPending, false);
            if (same) {
                // A pan in flight keeps going: committing it here made it
                // finish in one frame whenever Hyprland re-laid the desktop
                // mid-pan.
                layoutCanvas(m_pan.on); // where everyone already is (mid-pan: this frame's place)
                if (REVEAL && FOCUS && holds(FOCUS))
                    revealWindow(FOCUS, true);
                return;
            }
            // The arrangement changed: the pan was aimed at the old one.
            m_rearrangedAt = std::chrono::steady_clock::now();
            // A window coming back from the dock re-arranged it: its flight
            // aims at where this pass lays it, so the pan is committed here
            // as it always was (the 2026-10-29 restore, revealWindow).
            const bool RESTORING = std::ranges::any_of(m_canvasWho, [](const auto& t) {
                const auto T = t.lock();
                return T && Hyprdrive::Flight::restoringForFlight(T->window());
            });
            if (m_pan.on && RESTORING) {
                m_canvasOffset = m_pan.to;
                m_pan.on       = false;
                m_panCarry.clear();
            }
            if (m_pan.on) {
                // A PAN UNDER WAY NEVER TURNS INSTANT (owner, 2026-11-11). It
                // used to be committed here -- the view at its end in one frame,
                // Hyprland's move animation sliding everyone there. It keeps
                // going, re-aimed for the new arrangement (the reveal below, or
                // its old end clamped to the new canvas), and carries every
                // window from where it is drawn into its new place on the way.
                captureCarry();
                const Vector2D TO = m_pan.to;
                if (REVEAL && FOCUS && holds(FOCUS))
                    revealWindow(FOCUS, true);
                else
                    panTo(TO);
                if (m_pan.on) {
                    layoutCanvas(true);
                    return;
                }
                m_panCarry.clear();
            }
            // The arrangement changed (a window came, went or moved): the focused
            // one stays in view -- aimed BEFORE the layout, so it is one move.
            if (REVEAL && FOCUS && holds(FOCUS))
                revealWindow(FOCUS, false);
            layoutCanvas(false);
        }

        // Every window at its canvas place for the current view. Not the one a
        // Super+drag is carrying: it is out of the layout until it is let go.
        void layoutCanvas(bool warp) {
            const auto VIEW = canvasViewNow();
            if (!VIEW || m_canvas.rects.size() != m_canvasWho.size())
                return;
            m_canvasOffset.x = std::clamp(m_canvasOffset.x, 0.0, VIEW->maxX);
            m_canvasOffset.y = std::clamp(m_canvasOffset.y, 0.0, VIEW->maxY);
            m_lastLaid       = m_canvasOffset;
            for (size_t i = 0; i < m_canvasWho.size(); ++i) {
                const auto T = m_canvasWho[i].lock();
                if (!T || indexOf(T) < 0)
                    continue;
                const auto& R = m_canvas.rects[i];
                CBox        BOX{std::round(VIEW->originX + R.x - m_canvasOffset.x), std::round(VIEW->originY + R.y - m_canvasOffset.y), R.w, R.h};
                // A pan carrying windows from where they stood (revealWindow,
                // just re-arranged): the rest of their gap, on the pan's curve.
                // Not a window popping in (followPan's), nor one in flight.
                const auto WIN = T->window();
                if (warp && m_pan.on && i < m_panCarry.size() && m_panCarry[i].on && WIN && !WIN->m_animatingIn && !Hyprdrive::Flight::hasFlight(WIN) &&
                    !Hyprdrive::Flight::restoringForFlight(WIN)) {
                    const double LEFT = 1.0 - std::clamp(m_panE, 0.0, 1.0);
                    const auto&  C    = m_panCarry[i];
                    const double X = std::round(BOX.x + C.pos.x * LEFT), Y = std::round(BOX.y + C.pos.y * LEFT);
                    if (std::isfinite(X) && std::isfinite(Y)) // position only: the size is the layout's, always
                        BOX.x = X, BOX.y = Y;
                }
                T->setPositionGlobal(Layout::STargetBox{.logicalBox = BOX, .visualBox = BOX});
                // A pan snaps everyone to this frame's place -- but not a
                // window still popping in (user, 2026-10-29: "it spawns the app
                // in a tiny square, the border is the correct window size").
                // The warp ended its open animation, which is what stretches a
                // client's first, smaller buffer to the box: the border jumped
                // to full size around that buffer drawn at its own size. Dwindle
                // never warps a new window, so its popin plays out.
                // Never while zoomed out: the zoom draws every window with its
                // size marked as animating too (MosaicDrive::drawAt), and the
                // guard took them all for new windows -- they jumped at a drop
                // and again as the glide ended (user, 2026-10-29: "what the hell
                // is going on with window rearrangement in zoomed out mode").
                if (warp && (m_zoomed || !MosaicDrive::followPan(T->window())))
                    T->warpPositionSize();
            }
            if (!m_pan.on)
                m_panCarry.clear();
        }

        // Where every window is drawn now, against its place at the current
        // offset: what a carried pan (revealWindow) closes on its curve.
        void captureCarry() {
            m_panCarry.assign(m_canvasWho.size(), {});
            const auto VIEW = canvasViewNow();
            if (!VIEW || m_canvas.rects.size() != m_canvasWho.size())
                return;
            for (size_t i = 0; i < m_canvasWho.size(); ++i) {
                const auto T = m_canvasWho[i].lock();
                const auto W = T ? T->window() : PHLWINDOW{};
                if (!W || !W->positionAnimation() || Hyprdrive::Flight::hasFlight(W) || Hyprdrive::Flight::restoringForFlight(W))
                    continue;
                const auto& R   = m_canvas.rects[i];
                const auto     POS = W->positionAnimation()->value();
                const Vector2D PLACE{std::round(VIEW->originX + R.x - m_canvasOffset.x), std::round(VIEW->originY + R.y - m_canvasOffset.y)};
                const Vector2D DP = POS - PLACE;
                if (!std::isfinite(DP.x) || !std::isfinite(DP.y))
                    continue;
                m_panCarry[i] = {.on = true, .pos = DP};
            }
            m_panE = 0.0;
        }

        // ---- the pan's and the zoom's frames (canvasTick)
        // One frame of whatever is moving; false when nothing is.
        bool tick(std::chrono::steady_clock::time_point now) {
            bool busy = false;
            // THE PUSH IS OVER (user, 2026-11-09: "I push into an app and for
            // me to actually focus it I need to either click it or move my
            // cursor"): the window it focused comes wholly into view, as a
            // click would bring it -- the pointer rides the pan, so it stays on
            // that window.
            if (const auto PF = m_pushFocused.lock()) {
                if (edgePanRecent())
                    busy = true;
                else {
                    m_pushFocused.reset();
                    m_pushedAway = false;
                    // The pointer left the push for another window: that one
                    // takes the focus, and the view stays.
                    if (pointerLeftPushFor(PF))
                        busy = true;
                    else if (Desktop::focusState()->window() == PF && holds(PF) && shownNow()) {
                        trace("the push is over: its window comes into view");
                        revealWindow(PF, true);
                        busy = true;
                    }
                }
            }
            if (m_pan.on) {
                const double T = m_pan.ms > 0 ? Hyprdrive::Motion::Clock::animMsBetween<double>(now, m_pan.start) / m_pan.ms : 1.0;
                if (T >= 1.0) {
                    m_pan.on       = false;
                    m_canvasOffset = m_pan.to;
                    m_panE         = 1.0;
                } else {
                    const double E = canvasEase(T);
                    m_panE         = E;
                    m_canvasOffset = m_pan.kicked() ? m_pan.at(T, E) : m_pan.from + (m_pan.to - m_pan.from) * E;
                    busy           = true;
                }
                const Vector2D WAS = m_lastLaid;
                layoutCanvas(true);
                rideCursor(WAS);
                if (!m_pan.on && m_pan.settle) {
                    m_pan.settle = false;
                    settleFocus();
                }
            }
            if (m_morph.on && !m_zoom.on) {
                const bool DONE = Hyprdrive::Motion::Clock::animMsBetween<double>(now, m_morph.start) >= m_morph.ms;
                drawZoom();
                if (DONE)
                    m_morph.on = false;
                else
                    busy = true;
            }
            if (m_zoom.on) {
                const double T = m_zoom.ms > 0 ? Hyprdrive::Motion::Clock::animMsBetween<double>(now, m_zoom.start) / m_zoom.ms : 1.0;
                m_zoomZ        = T >= 1.0 ? m_zoom.to : m_zoom.from + (m_zoom.to - m_zoom.from) * canvasEase(T);
                drawZoom();
                if (T >= 1.0) {
                    m_zoom.on = false;
                    if (m_zoom.to <= 0.0)
                        endZoomNow();
                    else
                        m_zoomShift = {}; // all the way out: no zoom in is aimed now
                } else
                    busy = true;
            }
            // A glide that waited for the zoom (a drop made while it was still
            // zooming out) runs from the next frame: the clock must not stop on
            // the zoom's last one (user, 2026-10-30: a drag started halfway
            // through the zoom-out, let go with Super held, "stops all windows
            // from settling").
            if (m_morph.on && !m_zoom.on)
                busy = true;
            return busy;
        }

        // ---- THE ZOOM (holding Super and moving the pointer)
        // WINDOW TITLES ON THE ZOOMED-OUT CANVAS (user, 2026-10-30: "an option
        // ... to show labels on either no window, hovered windows, or all
        // windows"). The overview's caption pill at the bottom of each window
        // as it is drawn (its real box IS the zoomed one, drawAt; mid-glide
        // too). The windows that want one go into `out`; renderZoomLabels
        // fades each, under the zoom's own fade (zoomLabelAlpha).
        void zoomLabelWindows(bool all, const PHLWINDOW& hovered, std::vector<PHLWINDOW>& out) const {
            if (zoomLabelAlpha() <= 0.F)
                return;
            const auto DRAG = m_zDragMoved ? m_zDrag.lock() : PHLWINDOW{};
            for (const auto& WT : m_canvasWho) {
                const auto T = WT.lock();
                const auto W = T ? T->window() : nullptr;
                if (!W || W == DRAG || (!all && W != hovered))
                    continue;
                out.push_back(W);
            }
        }
        // Fading in over the second half of the zoom.
        float zoomLabelAlpha() const {
            if (!m_zoomed || m_zoomZ <= 0.5)
                return 0.F;
            return std::clamp(sc<float>((m_zoomZ - 0.5) / 0.5), 0.F, 1.F);
        }
        bool zoomed() const {
            return m_zoomed;
        }
        bool viewMoving() const {
            return m_pan.on || m_zoom.on || m_zoomed;
        }
        // Where the whole canvas sits shrunk onto the screen: scale and the
        // screen position of canvas (0,0).
        std::optional<std::pair<double, Vector2D>> zoomFit() const {
            const auto AREA = area();
            if (!m_asCanvas || !AREA || m_canvas.w < 1 || m_canvas.h < 1)
                return std::nullopt;
            const double M = std::max(8.0, innerGap() * 2.0);
            const double S = std::min({1.0, (AREA->w - 2 * M) / m_canvas.w, (AREA->h - 2 * M) / m_canvas.h});
            return std::pair{S, Vector2D{AREA->x + (AREA->w - m_canvas.w * S) / 2.0, AREA->y + (AREA->h - m_canvas.h * S) / 2.0}};
        }
        // `w`'s box drawn at zoom `z` (0 = where the layout has it, 1 = the whole canvas on screen).
        std::optional<CBox> zoomBoxOf(const PHLWINDOW& w, double z) const {
            const auto FIT  = zoomFit();
            const auto VIEW = canvasViewNow();
            if (!FIT || !VIEW || !w)
                return std::nullopt;
            const CBox     LB = MosaicDrive::layoutBox(w);
            const Vector2D P = LB.pos(), SZ = LB.size();
            // Screen -> canvas -> the zoomed screen: canvas (0,0) is at origin - offset.
            const Vector2D C0{VIEW->originX - m_canvasOffset.x, VIEW->originY - m_canvasOffset.y};
            const auto&    [S, Z0] = *FIT;
            const CBox     ZB{Z0.x + (P.x - C0.x) * S, Z0.y + (P.y - C0.y) * S, SZ.x * S, SZ.y * S};
            // Zooming back in toward a view that has moved (zoomIn): the
            // layout still stands at the old one until the zoom is over, so the
            // unzoomed end is the layout box shifted by the move.
            const double   NX = P.x - m_zoomShift.x, NY = P.y - m_zoomShift.y;
            return CBox{NX + (ZB.x - NX) * z, NY + (ZB.y - NY) * z, SZ.x + (ZB.w - SZ.x) * z, SZ.y + (ZB.h - SZ.y) * z};
        }
        void drawZoom() {
            const auto  DRAGGED = m_zDrag.lock();
            const float ME      = m_morph.on ? canvasEase(std::clamp(Hyprdrive::Motion::Clock::animMsBetween<double>(std::chrono::steady_clock::now(), m_morph.start) /
                                                                         std::max(1.0, m_morph.ms),
                                                                     0.0, 1.0))
                                             : 1.F;
            for (const auto& WT : m_canvasWho)
                if (const auto T = WT.lock(); T && indexOf(T) >= 0)
                    if (const auto W = T->window()) {
                        auto B = zoomBoxOf(W, m_zoomZ);
                        if (!B)
                            continue;
                        if (W == DRAGGED && m_zDragMoved && m_zCorner != Layout::CORNER_NONE)
                            B = m_zBox; // being resized
                        else if (W == DRAGGED && m_zDragMoved) {
                            // Carried by the pointer, at its zoomed size.
                            const Vector2D P = g_pInputManager->getMouseCoordsInternal() - m_zGrab;
                            B                = CBox{P.x, P.y, m_zDragSize.x, m_zDragSize.y};
                        } else if (m_morph.on) {
                            // A drop re-packed the canvas: everyone glides from
                            // where they were drawn to their new places.
                            for (const auto& [FW, FB] : m_morph.from)
                                if (FW.lock() == W) {
                                    B = CBox{FB.x + (B->x - FB.x) * ME, FB.y + (B->y - FB.y) * ME, FB.w + (B->w - FB.w) * ME, FB.h + (B->h - FB.h) * ME};
                                    break;
                                }
                        }
                        MosaicDrive::drawAt(W, *B);
                    }
            m_zoomed = true;
            if (const auto WS = workspace())
                if (const auto MON = WS->m_monitor.lock())
                    g_pHyprRenderer->damageMonitor(MON);
        }
        bool zoomOut() {
            const auto VIEW = canvasViewNow();
            if (!VIEW || (VIEW->maxX <= 1.0 && VIEW->maxY <= 1.0))
                return false; // all of it is on screen already
            if (m_pan.on)
                setCanvasOffset(m_canvasOffset, true); // the pan stops where it is
            const double MS = canvasPanMs();
            m_zoom          = SZoom{.on = MS >= 1.0, .from = m_zoomZ, .to = 1.0, .start = std::chrono::steady_clock::now(), .ms = MS};
            m_zoomArea      = area();
            m_zoomN         = liveEntries();
            if (!m_zoom.on) {
                m_zoomZ = 1.0;
                drawZoom();
            } else
                armCanvasTick();
            return true;
        }
        // THE ZOOM UNDER THE FINGERS (user, 2026-11-09: "3 finger swipe down
        // ... zoom out in infinite canvas temporarily ... follow my finger
        // movements and if I change direction without lifting my fingers it
        // undoes the zoom out"). zoomFollowStart once, zoomFollow per update
        // with the share of the swipe done (0 = as it was, 1 = all of it), and
        // zoomSettle at the end: out, or back in on the focused window.
        bool zoomFollowStart() {
            if (!canZoomOut() && !m_zoomed)
                return false;
            if (m_pan.on)
                setCanvasOffset(m_canvasOffset, true); // the pan stops where it is
            m_zoom.on = false;
            if (!m_zoomed) {
                m_zoomArea = area();
                m_zoomN    = liveEntries();
            }
            return true;
        }
        void zoomFollow(double z) {
            m_zoom.on = false;
            m_zoomZ   = std::clamp(z, 0.0, 1.0);
            drawZoom();
        }
        void zoomSettle(bool out) {
            if (!out) {
                if (m_zoomed)
                    zoomIn();
                else
                    endZoomNow();
                return;
            }
            // Out again from a zoom in begun: its aim (aimZoomIn) still shapes
            // the boxes on the way, so it goes only once all the way out.
            const double MS = canvasPanMs() * std::clamp(1.0 - m_zoomZ, 0.25, 1.0);
            m_zoom          = SZoom{.on = MS >= 1.0, .from = m_zoomZ, .to = 1.0, .start = std::chrono::steady_clock::now(), .ms = MS};
            if (!m_zoom.on) {
                m_zoomZ     = 1.0;
                m_zoomShift = {};
                drawZoom();
            } else
                armCanvasTick();
        }
        double zoomLevel() const {
            return m_zoomZ;
        }
        // Zooming out, not drawn zoomed yet (the first tick is still to come).
        bool zoomPending() const {
            return m_zoom.on && m_zoom.to >= 1.0 && !m_zoomed;
        }
        // A canvas wider than the screen: there is a zoomed-out view of it.
        bool canZoomOut() const {
            const auto VIEW = canvasViewNow();
            return VIEW && (VIEW->maxX > 1.0 || VIEW->maxY > 1.0);
        }
        // Where a zoom back in lands: the view on the focused window. The LAYOUT
        // moves there only when the zoom is over (endZoomNow): moving it now,
        // under windows drawn zoomed out, put their shadows and borders at the
        // new boxes for a frame -- a dark flash of every window (2026-10-26
        // recording). Until then zoomBoxOf draws toward it.
        void aimZoomIn() {
            m_zoomShift = {};
            if (const auto F = Desktop::focusState()->window(); F && holds(F))
                if (const auto WANT = revealOffset(F, m_canvasOffset); WANT && (std::abs(WANT->x - m_canvasOffset.x) > 0.5 || std::abs(WANT->y - m_canvasOffset.y) > 0.5)) {
                    if (const auto VIEW = canvasViewNow())
                        m_zoomShift = Vector2D{std::clamp(WANT->x, 0.0, VIEW->maxX), std::clamp(WANT->y, 0.0, VIEW->maxY)} - m_canvasOffset;
                }
        }
        // Super let go: back in, with the view on the focused window.
        void zoomIn() {
            if (!m_zoomed)
                return;
            aimZoomIn();
            const double MS = canvasPanMs();
            m_zoom          = SZoom{.on = MS >= 1.0, .from = m_zoomZ, .to = 0.0, .start = std::chrono::steady_clock::now(), .ms = MS};
            if (!m_zoom.on)
                endZoomNow();
            else
                armCanvasTick();
        }
        // Every window drawn where the layout has it again, at once.
        void endZoomNow() {
            m_zoom.on  = false;
            m_zoomZ    = 0.0;
            m_morph.on = false;
            m_zDrag.reset();
            m_zDragMoved = false;
            m_zCorner    = Layout::CORNER_NONE;
            if (!m_zoomed)
                return;
            m_zoomed = false;
            // Zoomed back in toward a moved view: the layout goes there now.
            if (std::abs(m_zoomShift.x) > 0.5 || std::abs(m_zoomShift.y) > 0.5) {
                m_canvasOffset = m_canvasOffset + m_zoomShift;
                m_zoomShift    = {};
                layoutCanvas(true);
            }
            m_zoomShift = {};
            for (const auto& E : m_entries)
                if (const auto T = E.target.lock())
                    MosaicDrive::release(T->window());
            if (const auto WS = workspace())
                if (const auto MON = WS->m_monitor.lock())
                    g_pHyprRenderer->damageMonitor(MON);
        }
        // SUPER+DRAG IN THE ZOOMED-OUT VIEW (user, 2026-10-26: "holding super
        // then moving cursor so I can see every window in canvas (zoomed out)
        // i can't drag windows around (super+drag) because the super was
        // already pressed to zoom out"). Super is already down, so the press
        // itself is the grab: a window drawn small under the pointer. Moved,
        // it follows the pointer at its zoomed size and, let go, is dropped
        // there -- the canvas re-packs around it (SCanvasDrop) and everyone
        // glides to their new zoomed places, still zoomed out. Not moved, it is
        // the old click: pick it and zoom in.
        // RESIZING IN THE ZOOMED-OUT VIEW (user, 2026-10-27: "I also want to
        // drag to expand while I'm holding super in infinite canvas and the
        // canvas is zoomed out"): a zoomed window's corner or edge, as on the
        // desktop -- a band ZONE px each side of its drawn border.
        Layout::eRectCorner zoomCornerAt(const Vector2D& p, PHLWINDOW* who = nullptr) const {
            constexpr double ZONE = 8.0;
            for (const auto& WT : m_canvasWho)
                if (const auto T = WT.lock(); T && indexOf(T) >= 0)
                    if (const auto W = T->window())
                        if (const auto B = zoomBoxOf(W, m_zoomZ)) {
                            const bool LEFT = std::abs(p.x - B->x) <= ZONE, RIGHT = std::abs(p.x - (B->x + B->w)) <= ZONE;
                            const bool TOP = std::abs(p.y - B->y) <= ZONE, BOTTOM = std::abs(p.y - (B->y + B->h)) <= ZONE;
                            const bool SPANX = p.x > B->x - ZONE && p.x < B->x + B->w + ZONE, SPANY = p.y > B->y - ZONE && p.y < B->y + B->h + ZONE;
                            if (!SPANX || !SPANY)
                                continue;
                            const int C = (LEFT ? Layout::CORNER_LEFT : 0) | (RIGHT ? Layout::CORNER_RIGHT : 0) | (TOP ? Layout::CORNER_TOP : 0) |
                                (BOTTOM ? Layout::CORNER_BOTTOM : 0);
                            if (C == 0)
                                continue;
                            if (who)
                                *who = W;
                            return sc<Layout::eRectCorner>(C);
                        }
            return Layout::CORNER_NONE;
        }
        bool zoomResizing() const {
            return !m_zDrag.expired() && m_zCorner != Layout::CORNER_NONE;
        }
        Layout::eRectCorner zoomCorner() const {
            return m_zCorner;
        }
        // `anywhere`: SUPER + RIGHT-DRAG (user, 2026-10-27, from a tester:
        // "super + right click ... resize the focused window by default on
        // omarchy/hyprland"; "even with windows zoomed out right click should
        // resize windows from that view"). Pressed anywhere on a window, it
        // resizes from the corner nearest the press, as Hyprland's own
        // resizewindow does; an edge band still picks that edge.
        bool zoomPress(const Vector2D& p, bool anywhere = false) {
            PHLWINDOW  RW;
            auto       CORNER = zoomCornerAt(p, &RW);
            const auto W      = CORNER != Layout::CORNER_NONE ? RW : zoomedWindowAt(p);
            const auto B      = W ? zoomBoxOf(W, m_zoomZ) : std::nullopt;
            if (!W || !B)
                return false;
            if (anywhere && CORNER == Layout::CORNER_NONE)
                CORNER = sc<Layout::eRectCorner>((p.x < B->x + B->w / 2.0 ? Layout::CORNER_LEFT : Layout::CORNER_RIGHT) |
                                                 (p.y < B->y + B->h / 2.0 ? Layout::CORNER_TOP : Layout::CORNER_BOTTOM));
            m_zCorner    = CORNER;
            m_zStart     = *B;
            m_zBox       = *B;
            m_zDrag      = W;
            m_zPress     = p;
            m_zGrab      = p - B->pos();
            m_zDragSize  = B->size();
            m_zDragMoved = false;
            return true;
        }
        void zoomMove(const Vector2D& p) {
            if (m_zDrag.expired())
                return;
            const Vector2D D = p - m_zPress;
            if (!m_zDragMoved && D.x * D.x + D.y * D.y < 36.0)
                return;
            if (!m_zDragMoved)
                if (const auto W = m_zDrag.lock())
                    Desktop::windowState()->raise(W); // carried above the rest, not under them
            m_zDragMoved = true;
            if (m_zCorner != Layout::CORNER_NONE) {
                // The side or corner pressed follows the pointer, the opposite
                // one stays; no smaller than a window may be, no bigger than
                // the screen (the canvas shows a window at most screen sized).
                const auto FIT  = zoomFit();
                const auto AREA = area();
                if (!FIT || !AREA)
                    return;
                const double S = FIT->first;
                double       x0 = m_zStart.x, y0 = m_zStart.y, x1 = m_zStart.x + m_zStart.w, y1 = m_zStart.y + m_zStart.h;
                if (Layout::edgeLeft(m_zCorner))
                    x0 = std::clamp(x0 + D.x, x1 - AREA->w * S, x1 - MIN_SIDE * S);
                if (Layout::edgeRight(m_zCorner))
                    x1 = std::clamp(x1 + D.x, x0 + MIN_SIDE * S, x0 + AREA->w * S);
                if (Layout::edgeTop(m_zCorner))
                    y0 = std::clamp(y0 + D.y, y1 - AREA->h * S, y1 - MIN_SIDE * S);
                if (Layout::edgeBottom(m_zCorner))
                    y1 = std::clamp(y1 + D.y, y0 + MIN_SIDE * S, y0 + AREA->h * S);
                m_zBox = CBox{x0, y0, x1 - x0, y1 - y0};
            }
            drawZoom();
        }
        // The press is over: true when it was a drag (dropped), false for a click.
        bool zoomRelease(const Vector2D& p) {
            const auto W     = m_zDrag.lock();
            const bool MOVED = m_zDragMoved;
            if (!W || !MOVED) {
                m_zDrag.reset();
                m_zDragMoved = false;
                m_zCorner    = Layout::CORNER_NONE;
                return false;
            }
            const auto FIT = zoomFit();
            const int  I   = [&] {
                for (size_t i = 0; i < m_entries.size(); ++i)
                    if (const auto T = m_entries[i].target.lock(); T && T->window() == W)
                        return sc<int>(i);
                return -1;
            }();
            if (FIT && I >= 0) {
                // Where everyone is drawn now: the glide starts there.
                const bool RESIZED = m_zCorner != Layout::CORNER_NONE;
                const CBox HERE    = RESIZED ? m_zBox : CBox{p - m_zGrab, m_zDragSize};
                m_morph.from.clear();
                for (const auto& WT : m_canvasWho)
                    if (const auto T = WT.lock(); T && T->window())
                        m_morph.from.emplace_back(T->window(), T->window() == W ? HERE : zoomBoxOf(T->window(), m_zoomZ).value_or(CBox{}));
                const auto& [S, Z0] = *FIT;
                if (RESIZED) {
                    // Its new size is its mosaic size from now on, as a resize
                    // on the desktop leaves it.
                    auto& E   = m_entries[sc<size_t>(I)];
                    E.natural = Vector2D{std::max(MIN_SIDE, std::round(HERE.w / S)), std::max(MIN_SIDE, std::round(HERE.h / S))};
                    E.trusted = true;
                }
                // The window's centre, back on the canvas: it stays there and
                // the cluster packs around it.
                const Vector2D C = (HERE.pos() + HERE.size() / 2.0 - Z0) / S;
                forgetArrangement();
                m_canvasDrop = SCanvasDropAt{m_entries[sc<size_t>(I)].target, MosaicPack::SCanvasDrop{.x = C.x, .y = C.y, .oldW = m_canvas.w, .oldH = m_canvas.h}};
                m_zDrag.reset();
                m_zDragMoved = false;
                m_zCorner    = Layout::CORNER_NONE;
                m_keepZoom   = true;
                recalculate();
                m_keepZoom = false;
                // The window just moved is the one you are working with: it has
                // focus, and letting go of Super zooms in on it (the hover on
                // the way had handed focus to whatever the pointer crossed).
                // A drop is the hand's choice, not a hover: CLICK, so the
                // hover veto never refuses it (vetoHoverFocus).
                Desktop::focusState()->fullWindowFocus(W, Desktop::FOCUS_REASON_CLICK);
                if (m_asCanvas && m_zoomed) {
                    // The new places go to the layout at once (unseen: every
                    // window is drawn zoomed), then the glide.
                    layoutCanvas(true);
                    m_morph.start = std::chrono::steady_clock::now();
                    m_morph.ms    = std::max(1.0, canvasPanMs());
                    m_morph.on    = true;
                    drawZoom();
                    armCanvasTick();
                } else
                    endZoomNow();
            }
            m_zDrag.reset();
            m_zDragMoved = false;
            m_zCorner    = Layout::CORNER_NONE;
            return true;
        }
        // The window a zoomed-out drag is carrying (moved, not a corner being
        // resized); none otherwise.
        PHLWINDOW zoomCarried() const {
            return m_zDragMoved && m_zCorner == Layout::CORNER_NONE ? m_zDrag.lock() : nullptr;
        }
        // Where it is drawn, with the pointer at `p` (zoomMove's box).
        CBox zoomCarriedBox(const Vector2D& p) const {
            return CBox{p - m_zGrab, m_zDragSize};
        }
        // Super let go mid-drag: the drag is off, the window goes back.
        void zoomDragCancel() {
            m_zDrag.reset();
            m_zDragMoved = false;
            m_zCorner    = Layout::CORNER_NONE;
        }

        // The window drawn under `p` while zoomed out.
        PHLWINDOW zoomedWindowAt(const Vector2D& p) const {
            for (const auto& WT : m_canvasWho)
                if (const auto T = WT.lock(); T && indexOf(T) >= 0)
                    if (const auto W = T->window())
                        if (const auto B = zoomBoxOf(W, m_zoomZ); B && B->containsPoint(p))
                            return W;
            return nullptr;
        }

        void layoutTiles() {
            const auto AREA = tilesArea();
            if (!AREA)
                return;
            syncTree();
            for (const auto& [ID, R] : m_tree.layout(*AREA, treeOpts()))
                if (const auto T = targetOfId(ID))
                    T->setPositionGlobal(Layout::STargetBox{.logicalBox = CBox{R.x, R.y, R.w, R.h}, .visualBox = {}});
        }

        // The desktop the mosaic lays out in.
        size_t liveEntries() const {
            return sc<size_t>(std::ranges::count_if(m_entries, [](const SMosaicEntry& e) { return !e.target.expired(); }));
        }
        // A RE-LAYOUT THAT CHANGES NOTHING keeps the zoom (user, 2026-10-27:
        // "fix taking screenshots while zoomed out"): a screenshot's region
        // picker is a new layer, and Hyprland re-lays out the monitor as it
        // appears -- the same windows in the same area, so the zoomed-out
        // canvas stays for the shot. A window come or gone, or the work area
        // changed, still ends it.
        bool zoomSurvives() const {
            if (!m_zoomed || !m_zoomArea)
                return false;
            const auto A = area();
            return A && std::abs(A->x - m_zoomArea->x) < 0.5 && std::abs(A->y - m_zoomArea->y) < 0.5 && std::abs(A->w - m_zoomArea->w) < 0.5 &&
                std::abs(A->h - m_zoomArea->h) < 0.5 && liveEntries() == m_zoomN;
        }
        std::optional<CBox> area() const {
            if (!m_parent || !m_parent->space())
                return std::nullopt;
            const auto SPACE = m_parent->space();
            CBox       AREA  = SPACE->workArea();
            // THE DOCK'S BAND (dock:windows_make_room, autohide off): DockRoom
            // shortens any tile under the pill after the fact, which would squash
            // the mosaic's bottom row out of its proportions. The mosaic keeps
            // out of that band instead -- one tile gap above the pill, the gap
            // DockRoom leaves -- and centres in what is left.
            if (const auto MON = SPACE->workspace() ? SPACE->workspace()->m_monitor.lock() : PHLMONITOR{}) {
                const CBox PILL = Dock::roomPillGlobal(MON);
                if (PILL.w >= 1 && PILL.h >= 1) {
                    static auto  PBORDER = CConfigValue<::Config::INTEGER>("general:border_size");
                    const double LIMIT   = PILL.y - innerGap() - std::max<int64_t>(0, *PBORDER);
                    AREA.h               = std::min(AREA.h, LIMIT - AREA.y);
                }
            }
            if (AREA.w < 1 || AREA.h < 1)
                return std::nullopt;
            return AREA;
        }

        // THE EASED MARGIN (MosaicPack::packEased): the area with each
        // screen-edge margin eased from gaps_out down to gaps_in, tried only
        // when the windows do not fit area(). The dock's band stays out of
        // bounds: a bottom edge area() pulled up above the pill is not eased.
        std::optional<MosaicPack::SRect> looseArea() const {
            const auto AREA = area();
            if (!AREA || !m_parent || !m_parent->space())
                return std::nullopt;
            static auto PGAPSOUT = CConfigValue<::Config::IComplexConfigValue>("general:gaps_out");
            static auto PGAPSIN  = CConfigValue<::Config::IComplexConfigValue>("general:gaps_in");
            const auto* OUT      = sc<::Config::CCssGapData*>(PGAPSOUT.ptr());
            const auto* IN       = sc<::Config::CCssGapData*>(PGAPSIN.ptr());
            if (!OUT || !IN)
                return std::nullopt;
            const auto   EASE = [](int64_t out, int64_t in) { return sc<double>(std::max<int64_t>(0, out - in)); };
            const double L = EASE(OUT->m_left, IN->m_left), R = EASE(OUT->m_right, IN->m_right), T = EASE(OUT->m_top, IN->m_top);
            const CBox   WORK     = m_parent->space()->workArea();
            const bool   PILLCUT  = AREA->y + AREA->h < WORK.y + WORK.h - 0.5;
            const double B        = PILLCUT ? 0.0 : EASE(OUT->m_bottom, IN->m_bottom);
            if (L + R + T + B < 0.5)
                return std::nullopt;
            return MosaicPack::SRect{AREA->x - L, AREA->y - T, AREA->w + L + R, AREA->h + T + B};
        }

        // Two windows trade places: their slots in the order (the mosaic) AND
        // their cells in the tiles. A cell is a leaf of the dwindle tree,
        // keyed by the entry's id, so the ids stay where they were and the
        // windows -- with their own sizes -- move between them; swapping whole
        // entries carried each window's cell along and the tiles never moved.
        void tradePlaces(size_t a, size_t b) {
            if (a == b || a >= m_entries.size() || b >= m_entries.size())
                return;
            // THE MOSAIC is any-order: the order no longer IS the arrangement,
            // so a swap is the moved window -- the focused one of the two --
            // dropped where the other one stands (PackHint): it lands there,
            // the cluster stays one tight bunch.
            const auto             TA = m_entries[a].target.lock(), TB = m_entries[b].target.lock();
            const auto             FOCUS = Desktop::focusState()->window();
            std::optional<SDropAt> swapDrop;
            // The canvas packs the same way (one cluster): a swap there is the
            // same drop, read off the canvas (arrangeCanvas).
            if ((m_asMosaic || m_asCanvas) && TA && TB)
                swapDrop = FOCUS && TB->window() == FOCUS ? SDropAt{TB, TA->position().middle()} : SDropAt{TA, TB->position().middle()};
            std::swap(m_entries[a], m_entries[b]);
            std::swap(m_entries[a].id, m_entries[b].id);
            m_dropAt = swapDrop;
            m_canvasDrop.reset();
        }

        SP<Layout::ITarget> getNextCandidate(SP<Layout::ITarget> old) override {
            if (m_entries.empty())
                return nullptr;
            // In the tiles, dwindle's: the tile nearest where the old one was.
            if (const auto AREA = tilesArea(); AREA && !m_tree.empty() && old) {
                const auto M = old->position().middle();
                if (const auto T = targetOfId(m_tree.leafAt(M.x, M.y, *AREA, treeOpts(), idOf(old))); T && T != old)
                    return T;
            }
            // THE ONE UNDER THE POINTER (user, 2026-11-02: closing a window
            // focused "a window my cursor is not over ... if my cursor is not
            // on a window it should focus the window nearest to my cursor").
            // Hyprland asks after the closed window has left the arrangement,
            // so this used to be the last window in it, wherever that was. Now:
            // where the windows are going, the one the pointer is over, else
            // the nearest to it -- or, the pointer on another monitor, the
            // nearest to where the closed window was.
            const auto WS  = workspace();
            const auto MON = WS ? WS->m_monitor.lock() : PHLMONITOR{};
            Vector2D   at  = old ? old->position().middle() : (MON ? MON->middle() : Vector2D{});
            if (g_pInputManager && MON) {
                const auto POINTER = g_pInputManager->getMouseCoordsInternal();
                if (CBox{MON->m_position, MON->m_size}.containsPoint(POINTER))
                    at = POINTER;
            }
            SP<Layout::ITarget> nearest;
            double              best = std::numeric_limits<double>::max();
            for (const auto& E : m_entries) {
                const auto T = E.target.lock();
                const auto W = T ? T->window() : PHLWINDOW{};
                if (!T || T == old || !W || !W->m_isMapped || W->isHidden())
                    continue;
                const CBox   B  = T->position();
                const double DX = std::max({B.x - at.x, 0.0, at.x - (B.x + B.w)});
                const double DY = std::max({B.y - at.y, 0.0, at.y - (B.y + B.h)});
                if (DX * DX + DY * DY < best) {
                    best    = DX * DX + DY * DY;
                    nearest = T;
                }
            }
            if (nearest)
                return nearest;
            const int I = indexOf(old);
            if (I < 0)
                return m_entries.back().target.lock();
            return m_entries[(sc<size_t>(I) + 1) % m_entries.size()].target.lock();
        }

        void swapTargets(SP<Layout::ITarget> a, SP<Layout::ITarget> b) override {
            m_revealPending = true;
            ++m_edits;
            const int IA = indexOf(a), IB = indexOf(b);
            if (IA >= 0 && IB >= 0)
                tradePlaces(sc<size_t>(IA), sc<size_t>(IB));
            recalculate();
        }

        // Moving a window: it trades places with the nearest window in that
        // direction (the order is the arrangement). Past the edge, the monitor
        // in that direction takes it, as binds:window_direction_monitor_fallback
        // asks of every layout.
        void moveTargetInDirection(SP<Layout::ITarget> t, Math::eDirection dir, bool silent) override {
            m_revealPending = true;
            ++m_edits;
            (void)silent;
            const int I = indexOf(t);
            if (I < 0 || !t)
                return;
            // IN THE TILES, DWINDLE'S MOVE: the window leaves its tile and splits
            // the one at its focal point that way -- past the edge, the monitor
            // there (binds:window_direction_monitor_fallback) -- and lands on
            // the far side of a lone sibling it moves straight at.
            if (const auto AREA = tilesArea(); AREA && !m_tree.empty() && m_tree.has(m_entries[sc<size_t>(I)].id)) {
                static auto PFALLBACK = CConfigValue<::Config::INTEGER>("binds:window_direction_monitor_fallback");
                const auto  FOCAL     = focalPointForDir(t, dir);
                const auto  WSMON     = workspace() ? workspace()->m_monitor.lock() : PHLMONITOR{};
                const auto  FOCALMON  = State::monitorState()->query().vec(FOCAL.value_or(t->position().middle())).run();
                if (FOCALMON != WSMON) {
                    if (*PFALLBACK && FOCALMON && FOCALMON->m_activeWorkspace) {
                        if (t->window())
                            t->window()->setAnimationsToMove();
                        t->assignToSpace(FOCALMON->m_activeWorkspace->m_space, FOCAL);
                    }
                    return;
                }
                const long long ID = m_entries[sc<size_t>(I)].id;
                if (m_tree.towardLoneSibling(ID, sc<int>(dir), *AREA, treeOpts()))
                    m_overrideDir = dir;
                if (t->window())
                    t->window()->setAnimationsToMove();
                const Vector2D ORIGIN = t->position().middle();
                m_tree.remove(ID);
                placeInTiles(t, FOCAL.value_or(ORIGIN), false);
                recalculate();
                if (silent)
                    if (const auto BACK = targetOfId(m_tree.leafAt(ORIGIN.x, ORIGIN.y, *AREA, treeOpts())); BACK && BACK->window())
                        Desktop::focusState()->fullWindowFocus(BACK->window(), Desktop::FOCUS_REASON_KEYBIND);
                return;
            }
            const CBox FROM = t->position();
            const auto C0   = FROM.middle();
            int        best = -1;
            double     bestD = 1e18;
            for (size_t k = 0; k < m_entries.size(); ++k) {
                const auto O = m_entries[k].target.lock();
                if (!O || O == t)
                    continue;
                const auto C = O->position().middle();
                const auto D = C - C0;
                const bool AHEAD = (dir == Math::DIRECTION_LEFT && D.x < -1) || (dir == Math::DIRECTION_RIGHT && D.x > 1) || (dir == Math::DIRECTION_UP && D.y < -1) ||
                    (dir == Math::DIRECTION_DOWN && D.y > 1);
                if (!AHEAD)
                    continue;
                const double DIST = D.x * D.x + D.y * D.y;
                if (DIST < bestD) {
                    bestD = DIST;
                    best  = sc<int>(k);
                }
            }
            if (best >= 0) {
                tradePlaces(sc<size_t>(I), sc<size_t>(best));
                recalculate();
                return;
            }

            static auto PMONITORFALLBACK = CConfigValue<::Config::INTEGER>("binds:window_direction_monitor_fallback");
            if (!*PMONITORFALLBACK || !t->space() || !t->space()->workspace())
                return;
            const auto PMONITOR  = t->space()->workspace()->m_monitor.lock();
            const auto PMONINDIR = State::monitorState()->query().relativeTo(PMONITOR).inDirection(dir).run();
            if (PMONINDIR && PMONINDIR != PMONITOR && PMONINDIR->m_activeWorkspace) {
                if (t->window())
                    t->window()->setAnimationsToMove();
                t->assignToSpace(PMONINDIR->m_activeWorkspace->m_space, focalPointForDir(t, dir));
            }
        }

        std::optional<std::string> layoutName() const override {
            return std::string{"mosaic"};
        }

        // DWINDLE'S LAYOUTMSGS in the tiles: togglesplit, swapsplit,
        // rotatesplit [angle], movetoroot [window] [unstable], preselect <dir>,
        // splitratio <delta> [exact]. In the mosaic there are no splits: the
        // split ones do nothing (no error popup for a bind pressed there);
        // preselect is kept for the tiles.
        ::Config::ErrorResult layoutMsg(const std::string_view& sv) override {
            std::vector<std::string> args;
            {
                std::istringstream in{std::string{sv}};
                for (std::string w; in >> w;)
                    args.push_back(w);
            }
            const auto ARG = [&](size_t i) { return i < args.size() ? args[i] : std::string{}; };
            using ::Config::eConfigErrorCode;
            using ::Config::eConfigErrorLevel;
            const std::string CMD = ARG(0);

            if (CMD == "preselect") {
                const std::string D = ARG(1);
                if (D.empty())
                    return ::Config::configError("No direction for preselect", eConfigErrorLevel::ERROR, eConfigErrorCode::INVALID_ARGUMENT);
                switch (D.front()) {
                    case 'u':
                    case 't': m_overrideDir = Math::DIRECTION_UP; break;
                    case 'd':
                    case 'b': m_overrideDir = Math::DIRECTION_DOWN; break;
                    case 'r': m_overrideDir = Math::DIRECTION_RIGHT; break;
                    case 'l': m_overrideDir = Math::DIRECTION_LEFT; break;
                    default: m_overrideDir = Math::DIRECTION_DEFAULT; break;
                }
                return {};
            }
            const bool KNOWN = CMD == "togglesplit" || CMD == "swapsplit" || CMD == "rotatesplit" || CMD == "movetoroot" || CMD == "splitratio";
            if (!KNOWN)
                return ::Config::configError(std::format("Unknown mosaic layoutmsg: {}", sv), eConfigErrorLevel::ERROR, eConfigErrorCode::INVALID_ARGUMENT);
            const auto AREA = tilesArea();
            if (m_tree.empty() || !AREA)
                return {};

            long long id = -1;
            if (const auto F = Desktop::focusState()->window())
                for (const auto& E : m_entries)
                    if (const auto T = E.target.lock(); T && T->window() == F)
                        id = E.id;
            const auto O = treeOpts();
            if (const auto F = Desktop::focusState()->window(); F && Fullscreen::controller()->isFullscreen(F) && CMD != "splitratio")
                return ::Config::configError(std::format("can't {} in the current workspace", CMD), eConfigErrorLevel::WARNING, eConfigErrorCode::INVALID_STATE);

            if (CMD == "togglesplit") {
                if (id >= 0 && !m_tree.toggleSplit(id, *AREA, O))
                    return ::Config::configError("can't togglesplit in the current workspace", eConfigErrorLevel::WARNING, eConfigErrorCode::INVALID_STATE);
            } else if (CMD == "swapsplit") {
                if (id >= 0 && !m_tree.swapSplit(id))
                    return ::Config::configError("can't swapsplit in the current workspace", eConfigErrorLevel::WARNING, eConfigErrorCode::INVALID_STATE);
            } else if (CMD == "rotatesplit") {
                int angle = 90;
                if (!ARG(1).empty()) {
                    try {
                        angle = std::stoi(ARG(1));
                    } catch (...) { return ::Config::configError("Invalid angle argument", eConfigErrorLevel::ERROR, eConfigErrorCode::INVALID_ARGUMENT); }
                }
                if (id >= 0)
                    m_tree.rotateSplit(id, angle, *AREA, O);
            } else if (CMD == "movetoroot") {
                long long which = id;
                if (!ARG(1).empty())
                    if (const auto W = Desktop::viewState()->query().selector(ARG(1)).runWindow())
                        for (const auto& E : m_entries)
                            if (const auto T = E.target.lock(); T && T->window() == W)
                                which = E.id;
                if (!m_tree.moveToRoot(which, ARG(2).empty() || ARG(2) != "unstable"))
                    return ::Config::configError("can't movetoroot in the current workspace", eConfigErrorLevel::WARNING, eConfigErrorCode::INVALID_STATE);
            } else if (CMD == "splitratio") {
                const std::string R = ARG(1);
                if (R.empty())
                    return ::Config::configError("splitratio requires an arg", eConfigErrorLevel::ERROR, eConfigErrorCode::INVALID_ARGUMENT);
                double value = 0;
                try {
                    value = std::stod(R);
                } catch (...) { return ::Config::configError(std::format("failed to parse \"{}\" as a delta", R), eConfigErrorLevel::ERROR, eConfigErrorCode::INVALID_ARGUMENT); }
                if (id < 0 || !m_tree.splitRatio(id, value, ARG(2).starts_with("exact")))
                    return ::Config::configError("cannot alter split ratio on no / single node", eConfigErrorLevel::WARNING, eConfigErrorCode::INVALID_STATE);
            }
            recalculate();
            return {};
        }

        // ---- DESKTOP SLIDES (slideFrame / slideWatch, below the class)
        struct SSlide {
            bool     seen        = false; // seen sliding: if it stops off screen with windows still in view, the tail runs
            bool     tail        = false; // sliding on by itself (startSlideTail)
            bool     zoomLeaving = false; // left while zoomed out: the zoom ends once the slide is over
            bool     preZoomed   = false; // zoomed out ahead of a swipe bringing it in (slideWatch)
            Vector2D arriveSet;               // the offset preRender last set while it arrives (arrivalFrame)
            Vector2D leaveSet;                // ... and while a swipe takes it away
            bool     leaveScaled = false;     // a swipe took it away stretched: the release goes as far (arrivalFrame)
            bool     swipeScaled = false;     // this swipe stretched it (swipeReleased)
            Vector2D swipeSet;                // ... to here, last
            Vector2D tailGoal, tailLast;
            SP<Hyprutils::Animation::SAnimationPropertyConfig> prevConfig; // the desktop animation's own, back after the tail
            // The tail's: an animated variable holds its config WEAKLY, so this
            // keeps it alive (a local one expired at once -- "disabled", stuck).
            SP<Hyprutils::Animation::SAnimationPropertyConfig> tailConfig;
        } m_slide;
        // The tail is done or a new slide took the desktop: its own animation back.
        void endSlideTail() {
            const auto WS = workspace();
            if (WS && WS->m_renderOffset && m_slide.prevConfig)
                WS->m_renderOffset->setConfig(m_slide.prevConfig);
            m_slide.prevConfig.reset();
            m_slide.tailConfig.reset();
            m_slide.tail = false;
        }
        // THE TAIL (user, 2026-11-01: windows hanging over the edge "should
        // still show until the gesture/slide is complete, THEN slide off the
        // desktop themselves as if I were still doing a gesture"). The slide
        // took the desktop a screen away; a window that hung over its edge is
        // still partly on screen, over the desktop that arrived. The desktop
        // carries on the same way, with the desktop animation, until the last
        // of them is off -- then Hyprland stops drawing it as it always does.
        // Only windows that were on screen at home: one parked further out on
        // the canvas is not drawn in a slide at all (hideDuringSlide).
        // THE SLIDE CARRIES THEM OFF (user, 2026-11-01: the windows sliding away
        // "should happen at the same velocity of the desktop switch. right now
        // the animation finishes, then the slide away starts"). On the first
        // frame of a slide taking this desktop away, the slide itself is
        // extended to where its last window in view is off the screen: the
        // same curve, its duration stretched by the extra distance -- so it
        // leaves at the speed the other desktop arrives at and simply carries
        // on a little further, easing out, with no stop in between.
        bool extendSlide() {
            const auto WS  = workspace();
            const auto MON = WS ? WS->m_monitor.lock() : PHLMONITOR{};
            if (!WS || !MON || !WS->m_renderOffset || !m_asCanvas || m_zoomed || !WS->m_renderOffset->isBeingAnimated())
                return false;
            const Vector2D GOAL = WS->m_renderOffset->goal(), VAL = WS->m_renderOffset->value();
            const bool     XAX  = std::abs(GOAL.x) >= std::abs(GOAL.y);
            const double   D    = XAX ? GOAL.x : GOAL.y;
            const double   SPAN = XAX ? MON->m_size.x : MON->m_size.y;
            // Away: headed at least half a screen off, and further than now.
            if (std::abs(D) < SPAN / 2.0 || std::abs(D) <= std::abs(XAX ? VAL.x : VAL.y))
                return false;
            const double NEED = overhangAt(WS, MON, D, XAX);
            if (NEED < 1)
                return false;
            const double   STEP    = (D < 0 ? -1.0 : 1.0) * (NEED + 2.0);
            const Vector2D NEWGOAL = XAX ? Vector2D{GOAL.x + STEP, GOAL.y} : Vector2D{GOAL.x, GOAL.y + STEP};
            const double   DIST0 = std::abs(XAX ? GOAL.x - VAL.x : GOAL.y - VAL.y), DIST1 = std::abs(XAX ? NEWGOAL.x - VAL.x : NEWGOAL.y - VAL.y);
            if (DIST0 < 1)
                return false;
            const auto BASE = WS->m_renderOffset->getConfig().lock();
            const auto VALS = BASE && BASE->pValues ? BASE->pValues.lock() : BASE;
            if (!VALS)
                return false;
            auto CFG             = makeShared<Hyprutils::Animation::SAnimationPropertyConfig>();
            CFG->overridden      = true;
            CFG->internalBezier  = VALS->internalBezier;
            CFG->internalStyle   = VALS->internalStyle;
            CFG->internalEnabled = VALS->internalEnabled;
            // Stretched to keep the starting speed -- but never past the time a
            // slide that long takes (user, 2026-11-02: a short way back, a
            // peek let go, crawled for seconds when the stretch was the whole
            // overhang over a few pixels left to go).
            const double STRETCH = std::clamp(DIST1 / DIST0, 1.0, std::max(1.0, DIST1 / SPAN));
            CFG->internalSpeed   = VALS->internalSpeed * sc<float>(STRETCH);
            CFG->pValues         = CFG;
            trace(std::format("slide extended: {:.0f} -> {:.0f} (+{:.0f}), speed {:.1f} -> {:.1f}", XAX ? VAL.x : VAL.y, XAX ? NEWGOAL.x : NEWGOAL.y, NEED,
                              VALS->internalSpeed, CFG->internalSpeed));
            m_slide.prevConfig = BASE;
            m_slide.tailConfig = CFG;
            m_slide.tail       = true;
            m_slide.tailGoal   = NEWGOAL;
            m_slide.tailLast   = VAL;
            WS->m_renderOffset->setConfig(CFG);
            *WS->m_renderOffset = NEWGOAL;
            return true;
        }
        // How far its windows in view at home reach past the edge a slide brings
        // it in by -- the left one when it arrives from the right (`d` > 0).
        double nearOverhang(const PHLWORKSPACE& ws, const PHLMONITOR& mon, double d, bool xax) const {
            const CBox MB{mon->m_position, mon->m_size};
            double     over = 0;
            for (const auto& E : m_entries) {
                const auto T = E.target.lock();
                const auto W = T ? T->window() : PHLWINDOW{};
                if (!W || !W->m_isMapped || W->isHidden() || W->m_workspace != ws || W->m_isFloating)
                    continue;
                const auto B = W->getFullWindowBoundingBox();
                if (B.intersection(MB).empty())
                    continue; // off the view at home: not in the slide
                over = std::max(over, d > 0 ? (xax ? MB.x - B.x : MB.y - B.y) : (xax ? B.x + B.w - (MB.x + MB.w) : B.y + B.h - (MB.y + MB.h)));
            }
            return over;
        }
        // How far past the screen the windows that were in view at home still
        // reach with the desktop at offset `d` (along the slide's axis).
        double overhangAt(const PHLWORKSPACE& ws, const PHLMONITOR& mon, double d, bool xax) const {
            const CBox MB{mon->m_position, mon->m_size};
            double     need = 0;
            for (const auto& E : m_entries) {
                const auto T = E.target.lock();
                const auto W = T ? T->window() : PHLWINDOW{};
                if (!W || !W->m_isMapped || W->isHidden() || W->m_workspace != ws || W->m_isFloating)
                    continue;
                const auto B = W->getFullWindowBoundingBox();
                if (B.intersection(MB).empty())
                    continue; // off the view at home: never drawn in the slide
                const double LO = (xax ? B.x : B.y) + d, HI = LO + (xax ? B.w : B.h);
                const double SLO = xax ? MB.x : MB.y, SHI = SLO + (xax ? MB.w : MB.h);
                need = std::max(need, d < 0 ? HI - SLO : SHI - LO);
            }
            return need;
        }
        void startSlideTail() {
            const auto WS  = workspace();
            const auto MON = WS ? WS->m_monitor.lock() : PHLMONITOR{};
            if (!WS || !MON || !WS->m_renderOffset || !m_asCanvas || m_zoomed)
                return;
            const Vector2D OFF  = WS->m_renderOffset->value();
            const bool     XAX  = std::abs(OFF.x) >= std::abs(OFF.y);
            const double   D    = XAX ? OFF.x : OFF.y;
            if (std::abs(D) < 1)
                return;
            const CBox MB{MON->m_position, MON->m_size};
            double     need = 0;
            for (const auto& E : m_entries) {
                const auto T = E.target.lock();
                const auto W = T ? T->window() : PHLWINDOW{};
                if (!W || !W->m_isMapped || W->isHidden() || W->m_workspace != WS || W->m_isFloating)
                    continue;
                const auto B = W->getFullWindowBoundingBox();
                if (B.intersection(MB).empty())
                    continue; // off the view at home: never drawn in the slide
                const double LO = (XAX ? B.x : B.y) + D, HI = LO + (XAX ? B.w : B.h);
                const double SLO = XAX ? MB.x : MB.y, SHI = SLO + (XAX ? MB.w : MB.h);
                need = std::max(need, D < 0 ? HI - SLO : SHI - LO);
            }
            if (need < 1)
                return;
            const double   STEP = (D < 0 ? -1.0 : 1.0) * (need + 2.0);
            const Vector2D GOAL = XAX ? Vector2D{OFF.x + STEP, OFF.y} : Vector2D{OFF.x, OFF.y + STEP};
            // Its own motion: the slide eased to a stop, so the tail starts slow
            // and speeds away, over a time for the distance left -- the desktop
            // animation's own ease-out started at full speed, a kick.
            // Speed and on/off from the config the slide itself just ran with
            // (workspacesIn/Out, as Hyprland set it): the plain "workspaces"
            // node read as disabled, and the tail never moved.
            const auto BASE = WS->m_renderOffset->getConfig().lock();
            const auto VALS = BASE && BASE->pValues ? BASE->pValues.lock() : BASE;
            const float SPAN = std::max(1.F, sc<float>(XAX ? MB.w : MB.h));
            auto CFG             = makeShared<Hyprutils::Animation::SAnimationPropertyConfig>();
            CFG->overridden      = true;
            // CSS ease-in: it picks up from the slide's stop and leaves at
            // speed; the house accelerate curve sat still for ten frames.
            if (Animation::mgr() && !Animation::mgr()->bezierExists(SLIDE_TAIL_BEZIER_NAME))
                Animation::mgr()->addBezierWithName(SLIDE_TAIL_BEZIER_NAME, Vector2D{0.4, 0.0}, Vector2D{1.0, 1.0});
            CFG->internalBezier  = SLIDE_TAIL_BEZIER_NAME;
            CFG->internalSpeed   = (VALS ? VALS->internalSpeed : 6.F) * std::clamp(std::sqrt(sc<float>(need) / SPAN), 0.4F, 1.F);
            CFG->internalEnabled = VALS ? VALS->internalEnabled : 1;
            CFG->pValues         = CFG;
            trace(std::format("slide tail: {:.0f}x{:.0f} -> {:.0f}x{:.0f} speed {:.1f}", OFF.x, OFF.y, GOAL.x, GOAL.y, CFG->internalSpeed));
            m_slide.prevConfig = WS->m_renderOffset->getConfig().lock();
            m_slide.tailConfig = CFG;
            m_slide.tail       = true;
            m_slide.tailGoal   = GOAL;
            m_slide.tailLast   = OFF;
            WS->m_renderOffset->setConfig(CFG);
            *WS->m_renderOffset = GOAL;
        }

      private:
        static constexpr double MIN_SIDE  = 120.0;

        std::vector<SMosaicEntry> m_entries;
        bool                      m_asMosaic  = false; // the last arrangement was the mosaic, not its tiles
        bool                      m_tiledHold = false; // a resize went past its limit: tiles until the mosaic fits unshrunk
        bool                      m_asCanvas  = false; // the last arrangement was the infinite canvas
        MosaicPack::SCanvasPack   m_canvas;            // where everyone sits on it
        MosaicPack::SCanvasMemo   m_canvasMemo;        // the last packing and its inputs (packCanvasMemo)
        std::vector<WP<Layout::ITarget>> m_canvasWho;  // whose rect is which (m_canvas.rects' order)
        Vector2D                  m_canvasOffset;      // the view, 0..SCanvasView::max
        struct SPan {
            bool                                  on = false, settle = false;
            Vector2D                              from, to;
            std::chrono::steady_clock::time_point start;
            double                                ms = 0;
            // Re-aimed while moving: the new leg leaves at the speed the view
            // had (MotionRetarget.hpp), per axis; off for every other leg.
            Hyprdrive::Motion::SKick              kickX, kickY;
            bool                                  kicked() const {
                return kickX.on() || kickY.on();
            }
            // The kick of a leg aimed at `to` from `cur`, given the leg under
            // way (MotionRetarget.hpp): continuous in value, and in velocity as
            // far as "no stall, a reversal carries on at most CARRY px" allows.
            static constexpr double CARRY = 32.0;
            static void matchKick(SPan& next, const SPan& old, const Vector2D& cur, std::chrono::steady_clock::time_point now) {
                namespace M = Hyprdrive::Motion;
                next.kickX = next.kickY = {};
                if (!old.on || old.ms <= 0 || next.ms <= 0 || !(M::isReaim(old.to.x, next.to.x) || M::isReaim(old.to.y, next.to.y)))
                    return;
                const double U = std::clamp(M::Clock::animMsBetween<double>(now, old.start) / old.ms, 0.0, 1.0);
                if (U >= 1.0 || M::inWindow(old.kickX, U) || M::inWindow(old.kickY, U))
                    return;
                const auto   CURVE = [](float t) { return canvasEase(t); };
                const double S     = M::curveSlope(CURVE, U);
                const double PER   = next.ms / old.ms; // old velocity per old u -> per new u
                const M::SKickLimits LIM{.legMs = next.ms, .maxCarry = CARRY, .arrive = 2.0};
                next.kickX = M::matchKick(CURVE, next.to.x - cur.x, M::legSlope(old.to.x - old.from.x, S, old.kickX, U) * PER, LIM);
                next.kickY = M::matchKick(CURVE, next.to.y - cur.y, M::legSlope(old.to.y - old.from.y, S, old.kickY, U) * PER, LIM);
            }
            Vector2D at(double t, float e) const {
                const Vector2D V = from + (to - from) * e;
                return {Hyprdrive::Motion::withKick(V.x, kickX, t), Hyprdrive::Motion::withKick(V.y, kickY, t)};
            }
        } m_pan;
        struct SZoom {
            bool                                  on = false;
            double                                from = 0, to = 0;
            std::chrono::steady_clock::time_point start;
            double                                ms = 0;
        } m_zoom;
        Vector2D                  m_lastLaid;       // the offset the windows were last laid out at
        // A pan after a re-arrangement carries each window from where it was
        // drawn (revealWindow): its gap to its place, per m_canvasWho index,
        // closed on the pan's curve (m_panE, this frame's eased progress).
        struct SCarry {
            bool     on = false;
            Vector2D pos; // where it is drawn minus its place; never a size
        };
        std::vector<SCarry> m_panCarry;
        double              m_panE = 1.0;
        PHLWINDOWREF              m_lastFocus;      // this desktop's last focused window (smart pan's "from")
        PHLWINDOWREF              m_prevFocus;      // the one before it
        double                    m_zoomZ  = 0.0;   // 0 = the layout, 1 = the whole canvas on screen
        Vector2D                  m_zoomShift;      // zooming in toward a view this far from the layout's
        // A DRAG IN THE ZOOMED-OUT VIEW (zoomPress/Move/Release).
        PHLWINDOWREF              m_zDrag;
        Vector2D                  m_zGrab, m_zPress, m_zDragSize;
        bool                      m_zDragMoved = false;
        // A corner or edge of a zoomed window pressed: it is RESIZED, not carried.
        Layout::eRectCorner       m_zCorner = Layout::CORNER_NONE;
        CBox                      m_zStart, m_zBox; // its zoomed box at the press, and now
        // What the zoom was drawn from (zoomOut): a re-layout that changes
        // neither keeps it (zoomSurvives).
        std::optional<CBox>       m_zoomArea;
        size_t                    m_zoomN = 0;
        bool                      m_keepZoom   = false; // the re-pack a zoomed drop asks for keeps the zoom
        struct SMorph {
            bool                                         on = false;
            std::vector<std::pair<PHLWINDOWREF, CBox>>   from;
            std::chrono::steady_clock::time_point        start;
            double                                       ms = 0;
        } m_morph;
        bool                      m_zoomed = false; // some window is drawn off its layout box
        bool                      m_revealPending = false;
        PHLWINDOWREF                          m_pushFocused;        // the window a push focused, revealed when it ends (tick)
        bool                                  m_pushedAway = false; // the view was pushed off the focused window (backFromPush)
        std::chrono::steady_clock::time_point m_rearrangedAt{}; // the canvas's last new arrangement (revealWindow)
        CHyprSignalListener       m_focusHook;
        CHyprSignalListener       m_reloadHook;
        // THE USER'S ARRANGEMENT (2026-10-16): a drop (or a swap) put a window
        // where it is -- it keeps to that point (MosaicPack::PackHint) until a
        // window arrives or leaves; then the mosaic regroups tight.
        struct SDropAt {
            WP<Layout::ITarget> target;
            Vector2D            at; // global logical, where it was let go
        };
        std::optional<SDropAt>    m_dropAt;
        // THE DRAG HOLD (user, 2026-10-16: "when I start dragging a window to
        // move it around in the cluster, you should wait to rearrange other
        // windows until I drop the window or it moves to another desktop"). A
        // Super+drag floats the window out of the layout; while it is in the
        // air the others stay exactly where they are. It ends when the window
        // comes back (newTarget/movedTarget: the drop), lands on another
        // desktop, or the drag is over (Mosaic::onMove asks, releaseDragHold).
        WP<Layout::ITarget>       m_dragHold;
        void                      forgetArrangement() {
            m_dropAt.reset();
            m_canvasDrop.reset();
        }
        // The canvas's own record of the drop (arrangeCanvas).
        struct SCanvasDropAt {
            WP<Layout::ITarget>     target;
            MosaicPack::SCanvasDrop drop;
        };
        std::optional<SCanvasDropAt> m_canvasDrop;
        // A FULLSCREEN ROUND TRIP (holdForTrip / backFromTrip): this desktop as
        // the trip found it, and every edit since.
        struct STripHold {
            MosaicTripHold::SHold        keys;
            SMosaicEntry                 entry;
            MosaicPack::CDwindleTree     tree;
            bool                         tiledHold = false;
            std::optional<SDropAt>       dropAt;
            std::optional<SCanvasDropAt> canvasDrop;
        };
        std::optional<STripHold> m_tripHold;
        uint64_t                 m_edits = 0;
        MosaicPack::CDwindleTree  m_tree;              // the tiles; empty while the mosaic stands
        long long                 m_nextId = 0;
        long long                 m_dragLeaf = -1;     // the tile a Super+drag picked its window out of
        Math::eDirection          m_overrideDir = Math::DIRECTION_DEFAULT; // dwindle's preselect

        // TRACE: every window's mosaic size at each arrangement decision.
        void traceEntries(const char* what) const {
            if (!traceOn())
                return;
            std::string line = std::format("arrange {} hold={} area={}x{} n={}", what, m_tiledHold, area() ? area()->w : 0.0, area() ? area()->h : 0.0, m_entries.size());
            for (const auto& E : m_entries) {
                const auto T = E.target.lock();
                const auto W = T ? T->window() : PHLWINDOW{};
                line += std::format(" [{} {:x} natural={}x{}{}]", appOf(W), (uintptr_t)W.get(), E.natural.x, E.natural.y,
                                    T && T->isPseudo() ? std::format(" pseudo={}x{}", T->pseudoSize().x, T->pseudoSize().y) : std::string{});
            }
            trace(line);
        }

        // The tree follows the windows. Empty (the tiles just took over from
        // the mosaic): a balanced tree of everyone, in mosaic order. Otherwise
        // a window gone is removed (its sibling takes the space) and a window
        // new to the tiles splits the FOCUSED window's tile, as in dwindle.
        void syncTree() {
            // Tiles sized like the mosaic (CDwindleTree::buildWeighted): each
            // window weighs its mosaic area.
            const auto WEIGHT = [this](long long id) {
                for (const auto& E : m_entries)
                    if (E.id == id)
                        return E.natural.x * E.natural.y;
                return 0.0;
            };
            if (m_tree.empty()) {
                std::vector<long long> ids;
                std::vector<double>    weights;
                for (const auto& E : m_entries) {
                    ids.push_back(E.id);
                    weights.push_back(E.natural.x * E.natural.y);
                }
                m_tree.buildWeighted(ids, weights);
                return;
            }
            for (const auto ID : m_tree.ids())
                if (std::ranges::none_of(m_entries, [&](const SMosaicEntry& e) { return e.id == ID; }))
                    m_tree.remove(ID);
            long long focusedId = -1;
            if (const auto F = Desktop::focusState()->window())
                for (const auto& E : m_entries)
                    if (const auto T = E.target.lock(); T && T->window() == F)
                        focusedId = E.id;
            for (const auto& E : m_entries)
                if (!m_tree.has(E.id)) {
                    // The window that just arrived holds the focus by now; split
                    // the one focused before it, or the newest tile.
                    m_tree.insert(E.id, focusedId == E.id ? -1 : focusedId, WEIGHT);
                }
        }

        // How far past the calculated limit the pointer must ask before the
        // desktop darkens and letting go means tiles: enough that the limit
        // itself is a comfortable place to stop.
        static constexpr double PUSH_PAST = 48.0;
        static constexpr float  DIM       = 0.45F;

        struct SResize {
            WP<Layout::ITarget>                             target;
            Layout::eRectCorner                             corner = Layout::CORNER_NONE;
            CBox                                            start;     // the window's box when the drag began
            Vector2D                                        requested; // the size the pointer asks for
            Vector2D                                        shown;     // the size granted: requested, clamped to the limit
            Vector2D                                        grabOffset; // corner minus pointer, at the press
            bool                                            past  = false;
            bool                                            tiled = false; // began in the tiles: no limit, no dim
            bool                                            tileFrozen = false; // a tile dragged by a corner/edge: the rest stand still
            CBox                                            tileBox;            // ... and where it is drawn meanwhile
            Vector2D                                        reach;              // the view pushed past the screen's edge (canvas on)
            std::vector<std::pair<WP<Layout::ITarget>, CBox>> frozen; // every OTHER window, where it stands
        };
        std::optional<SResize> m_resize;

        // `pressAt`: where the pointer was when the drag began. Hyprland's first
        // resizeTarget comes after the pointer has already moved by `delta`,
        // so the grab is measured from there, not from the pointer now -- or the
        // window lags the pointer by that first step all the way (2026-10-27).
        void beginResize(const SP<Layout::ITarget>& target, Layout::eRectCorner corner, std::optional<Vector2D> pressAt = std::nullopt) {
            if (m_resize)
                finishResize();
            SResize r;
            r.target    = target;
            r.corner    = corner;
            r.tiled     = !m_asMosaic && !m_asCanvas;
            r.start     = target->position();
            r.requested = r.start.size();
            r.shown     = r.start.size();
            if (corner != Layout::CORNER_NONE) {
                const Vector2D CORNERPOS{Layout::edgeLeft(corner) ? r.start.x : r.start.x + r.start.w, Layout::edgeTop(corner) ? r.start.y : r.start.y + r.start.h};
                r.grabOffset = CORNERPOS - pressAt.value_or(g_pInputManager->getMouseCoordsInternal());
            }
            for (const auto& E : m_entries)
                if (const auto T = E.target.lock(); T && T != target)
                    r.frozen.emplace_back(E.target, T->position());
            m_resize = std::move(r);
        }

        void stepResize() {
            if (!m_resize)
                return;
            const auto TARGET = m_resize->target.lock();
            const auto AREA   = area();
            if (!TARGET || !AREA) {
                m_resize.reset();
                return;
            }
            // In the layout's own order: the arrangement the limit proves is the
            // one recalculate() will build from the same sizes on release.
            std::vector<MosaicPack::SRect> sizes;
            size_t                         idx = 0;
            for (const auto& E : m_entries) {
                const auto T = E.target.lock();
                if (!T)
                    continue;
                if (T == TARGET) {
                    idx = sizes.size();
                    sizes.push_back({0, 0, m_resize->start.w, m_resize->start.h});
                    continue;
                }
                CBox box = T->position();
                for (const auto& [WT, BOX] : m_resize->frozen)
                    if (WT.lock() == T)
                        box = BOX;
                sizes.push_back({0, 0, box.w, box.h});
            }
            const MosaicPack::SRect AREAR{AREA->x, AREA->y, AREA->w, AREA->h};
            // THE CANVAS has no limit: a window that outgrows the screen's
            // mosaic sends the desktop onto the canvas when it is let go.
            if (canvasOn()) {
                m_resize->shown = Vector2D{std::clamp(m_resize->requested.x, MIN_SIDE, std::max(MIN_SIDE, AREA->w)),
                                           std::clamp(m_resize->requested.y, MIN_SIDE, std::max(MIN_SIDE, AREA->h))};
                applyResize();
                return;
            }
            const auto LIMIT = MosaicPack::limitFor(sizes, idx, m_resize->requested.x, m_resize->requested.y, AREAR, innerGap(), MIN_SIDE, looseArea());
            m_resize->shown  = Vector2D{LIMIT.w, LIMIT.h};
            const bool PAST  = !LIMIT.reached && (m_resize->requested.x > LIMIT.w + PUSH_PAST || m_resize->requested.y > LIMIT.h + PUSH_PAST);
            trace(std::format("step n={} start={}x{} requested={}x{} limit={}x{} reached={} past={} area={}x{}", sizes.size(), m_resize->start.w, m_resize->start.h,
                              m_resize->requested.x, m_resize->requested.y, LIMIT.w, LIMIT.h, LIMIT.reached, PAST, AREA->w, AREA->h));
            if (PAST != m_resize->past) {
                m_resize->past = PAST;
                setDim(PAST, *m_resize);
            }
            applyResize();
        }

        // The frozen windows where they stand, the resized one grown from its
        // opposite corner and kept on the desktop.
        void applyResize() {
            if (!m_resize)
                return;
            const auto TARGET = m_resize->target.lock();
            const auto AREA   = area();
            if (!TARGET || !AREA)
                return;
            const Vector2D R = m_resize->reach;
            for (const auto& [WT, BOX] : m_resize->frozen)
                if (const auto T = WT.lock()) {
                    const CBox AT{BOX.x - R.x, BOX.y - R.y, BOX.w, BOX.h};
                    T->setPositionGlobal(Layout::STargetBox{.logicalBox = AT, .visualBox = AT});
                    if (R.x != 0 || R.y != 0)
                        T->warpPositionSize();
                }
            const CBox&    S    = m_resize->start;
            const Vector2D SIZE = m_resize->shown;
            const auto     C    = m_resize->corner;
            // The corner opposite the one dragged stays put (a keyboard resize
            // grows about the centre).
            double x = C == Layout::CORNER_NONE ? S.x + (S.w - SIZE.x) / 2.0 : Layout::edgeLeft(C) ? S.x + S.w - SIZE.x : S.x;
            double y = C == Layout::CORNER_NONE ? S.y + (S.h - SIZE.y) / 2.0 : Layout::edgeTop(C) ? S.y + S.h - SIZE.y : S.y;
            // On the screen's mosaic it stays on the screen; with the canvas on
            // it may grow past the edge (resizeReach).
            if (!m_asCanvas && !canvasOn()) {
                x = std::clamp(x, AREA->x, std::max(AREA->x, AREA->x + AREA->w - SIZE.x));
                y = std::clamp(y, AREA->y, std::max(AREA->y, AREA->y + AREA->h - SIZE.y));
            }
            const CBox BOX{std::round(x - R.x), std::round(y - R.y), std::round(SIZE.x), std::round(SIZE.y)};
            TARGET->setPositionGlobal(Layout::STargetBox{.logicalBox = BOX, .visualBox = BOX});
            if (R.x != 0 || R.y != 0)
                TARGET->warpPositionSize();
        }

        // A tile being dragged over its still neighbours (resizeTarget).
        void applyTileResize() {
            if (!m_resize)
                return;
            for (const auto& [WT, BOX] : m_resize->frozen)
                if (const auto T = WT.lock())
                    T->setPositionGlobal(Layout::STargetBox{.logicalBox = BOX, .visualBox = {}});
            if (const auto T = m_resize->target.lock()) {
                T->setPositionGlobal(Layout::STargetBox{.logicalBox = m_resize->tileBox, .visualBox = {}});
                T->warpPositionSize(); // under the pointer, no animation chasing it
            }
        }

        // Past the limit: every OTHER window dims, so the desktop reads as about
        // to change. Hyprland's own per-window dim, handed back to dim_inactive
        // and friends afterwards.
        void setDim(bool on, const SResize& r) {
            for (const auto& [WT, BOX] : r.frozen) {
                const auto T = WT.lock();
                const auto W = T ? T->window() : PHLWINDOW{};
                if (!W)
                    continue;
                if (on)
                    *W->m_dimPercent = DIM;
                else
                    W->updateDecorationValues();
            }
        }

        int indexOf(const SP<Layout::ITarget>& t) const {
            for (size_t i = 0; i < m_entries.size(); ++i)
                if (m_entries[i].target.lock() == t)
                    return sc<int>(i);
            return -1;
        }

        // The space between two neighbours: the same as between two tiles, each
        // of which is inset by its side's gaps_in.
        double innerGap() const {
            static auto PGAPSIN = CConfigValue<::Config::IComplexConfigValue>("general:gaps_in");
            const auto* GAPS    = sc<::Config::CCssGapData*>(PGAPSIN.ptr());
            return GAPS ? sc<double>(GAPS->m_left + GAPS->m_right) : 10.0;
        }

        // THE SIZE A WINDOW KEEPS, decided once, when it arrives
        // (MosaicPack::chooseNatural has the order). desiredGeometry() is the
        // window's CURRENT size, so it is only the client's own choice the
        // moment a NEW window maps; one moving in from another layout carries
        // that layout's tile -- which is why a window this session already
        // sized in a mosaic keeps that size first. Nothing: three fifths of the
        // desktop.
        MosaicPack::SNatural naturalFor(const SP<Layout::ITarget>& t, bool fresh) const {
            CBox AREA{0, 0, 1280, 720};
            if (m_parent && m_parent->space())
                AREA = m_parent->space()->workArea();
            const auto SZ = [](const Vector2D& v) { return MosaicPack::SSize{v.x, v.y}; };
            MosaicPack::SNaturalInputs in{.fallback = {std::round(AREA.w * 0.6), std::round(AREA.h * 0.6)}, .fresh = fresh};
            if (t) {
                if (const auto KEPT = rememberedWindow(t)) {
                    in.kept        = SZ(*KEPT);
                    in.keptTrusted = keptTrusted(t->window());
                }
                loadSizes();
                if (const auto IT = g_sizes.find(appOf(t->window())); IT != g_sizes.end())
                    in.app = SZ(IT->second);
                in.floating = SZ(t->lastFloatingSize());
                if (const auto D = t->desiredGeometry(); D)
                    in.desired = SZ(D->size);
            }
            return MosaicPack::chooseNatural(in, MIN_SIDE);
        }
    };

    // ---- shared by MosaicGestures.cpp, MosaicSlides.cpp and Mosaic.cpp
    namespace MosaicDetail {
        enum class ESuper : uint8_t { IDLE, ARMED, ZOOMED, SPENT };
        extern bool g_ourDrag;
        extern bool g_hoverSet;
        extern bool onMosaic(const PHLWINDOW& w);
        extern ESuper g_super;
        extern bool g_fingerZoom;
        extern Vector2D g_superAnchor;
        extern CMosaicAlgorithm* g_superCanvas;
        extern bool g_swallowRelease;
        extern bool g_zoomPressing;
        extern uint32_t g_zoomButton;
        extern PHLWINDOWREF g_labelHover;
        extern bool g_superLabels;
        struct SLabelFade {
            PHLWINDOWREF                          window;
            std::chrono::steady_clock::time_point since;
            float                                 from = 0.F; // the alpha the fade started at
            bool                                  out  = false;
        };
        extern std::vector<SLabelFade> g_labelFades;
    void armSlideWatch(); // DESKTOP SLIDES, below
        extern CMosaicAlgorithm* liveZoomed();
    CMosaicAlgorithm* canvasOf(const PHLWORKSPACE& ws);
        extern wl_event_source* g_canvasTimer;
        extern Vector2D g_edgePush;
        extern PHLWORKSPACEREF g_edgeWorkspace;
        extern std::chrono::steady_clock::time_point g_edgeLast;
        extern wl_event_source* g_slideTimer;
        extern std::unordered_set<uint32_t> g_zoomKeysTaken;
        extern std::unordered_set<uint32_t> g_zoomKeysSent;
    }
}
