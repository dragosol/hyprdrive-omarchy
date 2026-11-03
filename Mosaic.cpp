// The "mosaic" layout: see Mosaic.hpp and MosaicPack.hpp.
#include "Mosaic.hpp"

#include "DesktopStrip.hpp"
#include "DebugPaths.hpp"
#include "MosaicPack.hpp"
#include "MosaicCanvas.hpp"
#include "MosaicDrive.hpp"
#include "Motion.hpp"
#include "globals.hpp"
#include "IOverview.hpp"
#include "Dock.hpp"
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

// main.cpp: Super held to pan the canvas is not a Super tap.
namespace ScrollOverview {
    void cancelSuperTap();
}

namespace ScrollOverview::Mosaic {

    class CMosaicAlgorithm;
    static bool pickerOpen(); // a screenshot's region picker is up (below)
    // Every live mosaic layout -- one per desktop on mosaic. Hyprland keeps the
    // algorithm behind a desktop private, so "is this window on a mosaic
    // desktop" is answered from here instead.
    static std::vector<CMosaicAlgorithm*> g_instances;

    // ---------------------------------------------------------- remembered sizes --
    // AN APP'S MOSAIC SIZE (user, 2026-10-11: "if i close a particular app at
    // that size DURING MOSAIC then that becomes the app's default size when
    // mosaic is open. dwindle and scrolling modes don't apply"). Keyed on the
    // app's class; learned only when a window CLOSES while its desktop is in the
    // mosaic arrangement (not its tiles), and used only by mosaic. One line per
    // app: class<TAB>width<TAB>height, in $XDG_STATE_HOME/hyprdrive/mosaic-sizes.
    namespace {
        std::unordered_map<std::string, Vector2D> g_sizes;
        bool                                      g_sizesLoaded = false;

        std::filesystem::path sizesPath() {
            if (const char* S = std::getenv("XDG_STATE_HOME"); S && *S)
                return std::filesystem::path{S} / "hyprdrive" / "mosaic-sizes";
            const char* HOME = std::getenv("HOME");
            return std::filesystem::path{HOME ? HOME : "/tmp"} / ".local" / "state" / "hyprdrive" / "mosaic-sizes";
        }

        void loadSizes() {
            if (g_sizesLoaded)
                return;
            g_sizesLoaded = true;
            std::ifstream in(sizesPath());
            std::string   line;
            while (std::getline(in, line)) {
                const auto T1 = line.find('\t'), T2 = line.find('\t', T1 == std::string::npos ? 0 : T1 + 1);
                if (T1 == std::string::npos || T2 == std::string::npos)
                    continue;
                try {
                    g_sizes[line.substr(0, T1)] = Vector2D{std::stod(line.substr(T1 + 1, T2 - T1 - 1)), std::stod(line.substr(T2 + 1))};
                } catch (...) {}
            }
        }

        void saveSize(const std::string& app, const Vector2D& size) {
            loadSizes();
            const auto OLD = g_sizes.find(app);
            if (OLD != g_sizes.end() && std::abs(OLD->second.x - size.x) < 1 && std::abs(OLD->second.y - size.y) < 1)
                return; // nothing new: no write
            g_sizes[app] = size;
            std::error_code ec;
            std::filesystem::create_directories(sizesPath().parent_path(), ec);
            std::ostringstream out;
            for (const auto& [K, V] : g_sizes)
                out << K << '\t' << std::lround(V.x) << '\t' << std::lround(V.y) << '\n';
            const auto TMP = sizesPath().string() + ".tmp";
            if (std::ofstream f(TMP); f) {
                f << out.str();
                f.close();
                std::filesystem::rename(TMP, sizesPath(), ec);
            }
        }

        std::string appOf(const PHLWINDOW& w) {
            if (!w)
                return {};
            return !w->m_initialClass.empty() ? w->m_initialClass : w->m_class;
        }

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
        std::vector<SKept> g_windowSizes;

        SKept* keptOf(const PHLWINDOW& w) {
            for (auto& e : g_windowSizes)
                if (e.window && e.window.lock() == w)
                    return &e;
            return nullptr;
        }

        void rememberWindow(const PHLWINDOW& w, const Vector2D& size, bool trusted = true) {
            if (!w)
                return;
            std::erase_if(g_windowSizes, [](const SKept& e) { return !e.window; });
            if (auto* e = keptOf(w)) {
                e->size         = size;
                e->floatedSince = false;
                e->trusted      = trusted;
                return;
            }
            g_windowSizes.push_back({.window = w, .size = size, .trusted = trusted});
        }
        bool keptTrusted(const PHLWINDOW& w) {
            const auto* e = keptOf(w);
            return !e || e->trusted;
        }

        void leftMosaic(const SP<Layout::ITarget>& t) {
            if (auto* e = keptOf(t ? t->window() : PHLWINDOW{})) {
                e->floatAtLeave = t->lastFloatingSize();
                e->floatedSince = false;
            }
        }

        void onFloating(const PHLWINDOW& w) {
            if (!w || !w->m_isFloating)
                return;
            // A Super+drag floats the window for the drag: that is not the
            // user floating it (user, 2026-10-15, fedora: dragging Steam --
            // an X11 client that resizes itself when floated -- came back at
            // 490x308, its kept 511x926 thrown away as "floated and resized").
            if (g_layoutManager)
                if (const auto DRAGGED = g_layoutManager->dragController()->target(); DRAGGED && DRAGGED->window() == w)
                    return;
            if (auto* e = keptOf(w))
                e->floatedSince = true;
        }

        // ACROSS A PLUGIN RELOAD (user, 2026-10-15: after a reload desktop 1
        // went to the tiles and never came back). Unloading hands every desktop
        // to dwindle, and a reloaded plugin starts with no kept sizes, so each
        // window re-arrived at its DWINDLE TILE as its own size (Pear Messages:
        // 1903x2094, full height) and the mosaic could never fit again. The
        // kept sizes now go to a file in XDG_RUNTIME_DIR on unload, stamped
        // with this Hyprland's instance, and come back on load before any
        // window re-arrives. Window addresses hold across a plugin reload.
        std::string keptPath() {
            const char* RT = std::getenv("XDG_RUNTIME_DIR");
            return std::string(RT && *RT ? RT : "/tmp") + "/hyprdrive-mosaic-kept";
        }
        std::string keptStamp() {
            const char* SIG = std::getenv("HYPRLAND_INSTANCE_SIGNATURE");
            return SIG ? SIG : "";
        }
        void saveKept() {
            std::ofstream f(keptPath(), std::ios::trunc);
            if (!f)
                return;
            f << keptStamp() << '\n';
            for (const auto& e : g_windowSizes)
                if (const auto W = e.window.lock())
                    f << std::format("{:x} {} {} {}\n", (uintptr_t)W.get(), e.size.x, e.size.y, e.trusted ? 1 : 0);
        }
        void loadKept() {
            const auto     PATH = keptPath();
            std::ifstream  f(PATH);
            std::string    stamp;
            if (!f || !std::getline(f, stamp) || stamp != keptStamp() || stamp.empty())
                return;
            std::unordered_map<uintptr_t, std::pair<Vector2D, bool>> saved;
            std::string                                              line;
            while (std::getline(f, line)) {
                std::istringstream in(line);
                std::string        addr;
                double             w = 0, h = 0;
                int                trusted = 1;
                if (in >> addr >> w >> h) {
                    in >> trusted;
                    saved[std::stoull(addr, nullptr, 16)] = {Vector2D{w, h}, trusted != 0};
                }
            }
            for (const auto& W : Desktop::windowState()->windows())
                if (W)
                    if (const auto IT = saved.find((uintptr_t)W.get()); IT != saved.end())
                        rememberWindow(W, IT->second.first, IT->second.second);
            std::error_code ec;
            std::filesystem::remove(PATH, ec);
        }

        std::optional<Vector2D> rememberedWindow(const SP<Layout::ITarget>& t) {
            const auto* e = keptOf(t ? t->window() : PHLWINDOW{});
            if (!e)
                return std::nullopt;
            const auto F = t->lastFloatingSize();
            if (e->floatedSince && (std::abs(F.x - e->floatAtLeave.x) > 2 || std::abs(F.y - e->floatAtLeave.y) > 2))
                return std::nullopt; // floated and resized there: its floating size decides
            return e->size;
        }
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

    // TRACE, for the next odd recording: `touch $XDG_RUNTIME_DIR/hyprdrive-mosaic.on`
    // and every resize step and arrangement decision lands in
    // $XDG_RUNTIME_DIR/hyprdrive-mosaic.log (DebugPaths.hpp: ours only, 0600).
    static bool traceOn() {
        static bool                                  on      = false;
        static std::chrono::steady_clock::time_point checked = {};
        const auto                                   NOW     = std::chrono::steady_clock::now();
        if (NOW - checked > std::chrono::seconds(1)) {
            checked = NOW;
            on      = ScrollOverview::DebugPaths::flagOn("hyprdrive-mosaic.on");
        }
        return on;
    }
    static void trace(const std::string& line) {
        if (!traceOn())
            return;
        if (FILE* const F = ScrollOverview::DebugPaths::openLog("hyprdrive-mosaic.log")) {
            std::fprintf(F, "%lld %s\n",
                         static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count() % 100000000),
                         line.c_str());
            std::fclose(F);
        }
    }

    // THE INFINITE CANVAS (MosaicCanvas.hpp; user, 2026-10-26). Off: a mosaic
    // that no longer fits tiles, as before. On: it spills onto a canvas the view
    // pans across -- a strip (horizontal) or a grid (all directions).
    static bool canvasOn() {
        return ScrollOverview::Config::getValue<bool>("plugin:hyprdrive:mosaic:infinite_canvas");
    }
    static bool canvasAllDirections() {
        return ScrollOverview::Config::getValue<std::string>("plugin:hyprdrive:mosaic:canvas_directions") == "all";
    }
    // THE PAN'S MOTION (user, 2026-10-26: "when I focus on a window it pans it
    // into view smoothly with a serpantinum curve, adjustable in the animation
    // tab"): animation:canvas_curve (a Motion.hpp preset, its DECEL) over
    // animation:canvas_pan_ms, scaled by the global speed (anim_scale; 0 snaps).
    static double canvasPanMs() {
        const double MS = sc<double>(ScrollOverview::Config::getValue<Hyprlang::INT>("plugin:hyprdrive:animation:canvas_pan_ms"));
        return std::max(0.0, MS * ScrollOverview::Config::animScale());
    }
    static float canvasEase(double t) {
        return ScrollOverview::Motion::easeNamed(ScrollOverview::Config::getValue<std::string>("plugin:hyprdrive:animation:canvas_curve"),
                                                 ScrollOverview::Motion::ERole::DECEL, sc<float>(t));
    }
    // A swipe or a Super-pan is moving the view: focus landing on a window
    // under the pointer must not pull the view back to it mid-pan.
    static bool g_canvasPanning = false;
    // rideCursor is warping the pointer: that motion is not the hand's.
    static bool g_cursorRiding = false;
    // The canvas's frame clock (pans, zooms, a drag pushing at the screen's
    // edge): an event-loop timer, never a request from inside the render pass.
    static void armCanvasTick();
    // A plain click: the canvas window under the pointer pans into view.
    static void canvasClick();

    // A window's title pill at the bottom of the box it is drawn at (zoomed or
    // where it is): the overview's caption pill (OverviewRender::labelPill).
    static void drawWindowLabel(const PHLMONITOR& monitor, const PHLWINDOW& w, float alpha) {
        const float    SCALE = std::max(monitor->m_scale, 0.01F);
        const Vector2D P = w->positionAnimation()->value(), S = w->sizeAnimation()->value();
        if (S.x < 48 || S.y < 32)
            return;
        const double CX     = (P.x + S.x / 2.0 - monitor->m_position.x) * SCALE;
        const double BOTTOM = (P.y + S.y - monitor->m_position.y - 14.0) * SCALE;
        OverviewRender::labelPill(monitor, w->m_title.empty() ? w->m_class : w->m_title, CX, BOTTOM, alpha, SCALE);
    }

    // The slide tail's curve (CMosaicAlgorithm::startSlideTail).
    inline constexpr const char* SLIDE_TAIL_BEZIER_NAME = "hyprdriveSlideTail";

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
                const bool HOVER = FFM && !g_cursorRiding;
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
                m_pan.from  = m_canvasOffset;
                m_pan.to    = want;
                m_pan.start = std::chrono::steady_clock::now();
                m_pan.ms    = MS;
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
            const auto WANT = revealOffset(w, panTarget(), from);
            if (!WANT)
                return;
            // JUST RE-ARRANGED (user, 2026-10-29: the window spawn animation
            // "is buggy looking"): a window arrived or left, so everyone is
            // already sliding to a new place -- and the new window's focus,
            // a frame later, asked for a pan. A pan lays the windows out at
            // each frame's offset outright, so they leapt into the new
            // arrangement and only then panned. Instead the view goes to its
            // end at once and the slides that are under way are re-aimed there:
            // one move per window, from where it stands.
            // Never for a window flying back from the dock: its flight aims at
            // where the layout had it, so the view jumping under it landed it
            // off screen and slid it in (user, 2026-10-29: "you replaced my
            // maximize from dock animation"). It pans, as before.
            if (animate && std::chrono::steady_clock::now() - m_rearrangedAt < std::chrono::milliseconds(600) && !ScrollOverview::Flight::hasFlight(w)) {
                trace(std::format("reveal after a re-arrangement: {}x{} -> {}x{} as window moves", m_canvasOffset.x, m_canvasOffset.y, WANT->x, WANT->y));
                m_pan.on       = false;
                m_canvasOffset = *WANT;
                layoutCanvas(false);
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

        void newTarget(SP<Layout::ITarget> target) override {
            m_revealPending = true;
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
            recalculate();
        }

        void movedTarget(SP<Layout::ITarget> target, std::optional<Vector2D> focalPoint = std::nullopt) override {
            m_revealPending = true;
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
            if (m_pan.on) {
                m_canvasOffset = m_pan.to;
                m_pan.on       = false;
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
                const CBox  BOX{std::round(VIEW->originX + R.x - m_canvasOffset.x), std::round(VIEW->originY + R.y - m_canvasOffset.y), R.w, R.h};
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
        }

        // ---- the pan's and the zoom's frames (canvasTick)
        // One frame of whatever is moving; false when nothing is.
        bool tick(std::chrono::steady_clock::time_point now) {
            bool busy = false;
            if (m_pan.on) {
                const double T = m_pan.ms > 0 ? std::chrono::duration<double, std::milli>(now - m_pan.start).count() / m_pan.ms : 1.0;
                if (T >= 1.0) {
                    m_pan.on       = false;
                    m_canvasOffset = m_pan.to;
                } else {
                    const double E = canvasEase(T);
                    m_canvasOffset = m_pan.from + (m_pan.to - m_pan.from) * E;
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
                const bool DONE = std::chrono::duration<double, std::milli>(now - m_morph.start).count() >= m_morph.ms;
                drawZoom();
                if (DONE)
                    m_morph.on = false;
                else
                    busy = true;
            }
            if (m_zoom.on) {
                const double T = m_zoom.ms > 0 ? std::chrono::duration<double, std::milli>(now - m_zoom.start).count() / m_zoom.ms : 1.0;
                m_zoomZ        = T >= 1.0 ? m_zoom.to : m_zoom.from + (m_zoom.to - m_zoom.from) * canvasEase(T);
                drawZoom();
                if (T >= 1.0) {
                    m_zoom.on = false;
                    if (m_zoom.to <= 0.0)
                        endZoomNow();
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
            const float ME      = m_morph.on ? canvasEase(std::clamp(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - m_morph.start).count() /
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
        // Zooming out, not drawn zoomed yet (the first tick is still to come).
        bool zoomPending() const {
            return m_zoom.on && m_zoom.to >= 1.0 && !m_zoomed;
        }
        // A canvas wider than the screen: there is a zoomed-out view of it.
        bool canZoomOut() const {
            const auto VIEW = canvasViewNow();
            return VIEW && (VIEW->maxX > 1.0 || VIEW->maxY > 1.0);
        }
        // Super let go: back in, with the view on the focused window.
        void zoomIn() {
            if (!m_zoomed)
                return;
            // Aimed at the focused window. The LAYOUT moves there only when the
            // zoom is over (endZoomNow): moving it now, under windows drawn
            // zoomed out, put their shadows and borders at the new boxes for
            // a frame -- a dark flash of every window (2026-10-26 recording).
            m_zoomShift = {};
            if (const auto F = Desktop::focusState()->window(); F && holds(F))
                if (const auto WANT = revealOffset(F, m_canvasOffset); WANT && (std::abs(WANT->x - m_canvasOffset.x) > 0.5 || std::abs(WANT->y - m_canvasOffset.y) > 0.5)) {
                    if (const auto VIEW = canvasViewNow())
                        m_zoomShift = Vector2D{std::clamp(WANT->x, 0.0, VIEW->maxX), std::clamp(WANT->y, 0.0, VIEW->maxY)} - m_canvasOffset;
                }
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
                Desktop::focusState()->fullWindowFocus(W, Desktop::FOCUS_REASON_FFM);
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
        } m_pan;
        struct SZoom {
            bool                                  on = false;
            double                                from = 0, to = 0;
            std::chrono::steady_clock::time_point start;
            double                                ms = 0;
        } m_zoom;
        Vector2D                  m_lastLaid;       // the offset the windows were last laid out at
        PHLWINDOWREF              m_lastFocus;      // this desktop's last focused window (smart pan's "from")
        PHLWINDOWREF              m_prevFocus;      // the one before it
        bool                      g_cursorRiding = false; // rideCursor is warping the pointer
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

    // ------------------------------------------------------------ corner grab --
    // CORNER RESIZE (user, 2026-10-11: "if I hold down super and go to the
    // corner of a window i can resize it, that or corner resize is always
    // available in mosaic"). Both:
    //   - on a mosaic desktop, a plain left press on a window's corner resizes
    //     it, and the pointer shows the resize cursor over a corner;
    //   - with Super held, the same press resizes on ANY layout, in place of
    //     Omarchy's Super+drag move -- which still moves from anywhere else.
    // The resize is Hyprland's own drag (beginDragTarget with the corner
    // forced), so tiled layouts get resizeTarget and floating windows their
    // usual live resize. The press is claimed, so Hyprland's bind handler never
    // sees it and would never end the drag: the matching release ends it here.
    namespace {
        bool        g_ourDrag   = false;
        bool        g_hoverSet  = false;
        // How far INSIDE a window a corner reaches. Outside, it is Hyprland's
        // own border grab width; inside, only this much, so an app's own
        // controls in its corners keep their clicks.
        constexpr double INNER = 6.0;

        bool onMosaic(const PHLWINDOW& w) {
            return w && std::ranges::any_of(g_instances, [&](const CMosaicAlgorithm* a) { return a->holds(w); });
        }

        // The corner -- or EDGE -- of `w` under `p`, or CORNER_NONE. EDGES (user,
        // 2026-10-15: "drag edges to resize in mosaic ... it only works on
        // corners and not on top/bottom edges"): a side reaches out into the
        // gap like a corner, but INSIDE the window only with Super held, so an
        // app's own top row (a browser's tabs) keeps its clicks.
        Layout::eRectCorner cornerAt(const PHLWINDOW& w, const Vector2D& p, bool superHeld) {
            static auto PBORDER = CConfigValue<::Config::INTEGER>("general:border_size");
            static auto PEXTEND = CConfigValue<::Config::INTEGER>("general:extend_border_grab_area");
            const double OUT    = std::max<double>(8.0, sc<double>(*PBORDER + *PEXTEND));
            const CBox   B      = w->geometricBox(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
            const bool   LEFT   = p.x >= B.x - OUT && p.x <= B.x + INNER;
            const bool   RIGHT  = p.x >= B.x + B.w - INNER && p.x <= B.x + B.w + OUT;
            const bool   TOP    = p.y >= B.y - OUT && p.y <= B.y + INNER;
            const bool   BOTTOM = p.y >= B.y + B.h - INNER && p.y <= B.y + B.h + OUT;
            if (TOP && LEFT)
                return Layout::CORNER_TOPLEFT;
            if (TOP && RIGHT)
                return Layout::CORNER_TOPRIGHT;
            if (BOTTOM && LEFT)
                return Layout::CORNER_BOTTOMLEFT;
            if (BOTTOM && RIGHT)
                return Layout::CORNER_BOTTOMRIGHT;
            const double IN    = superHeld ? INNER : 0.0;
            const bool   SPANX = p.x >= B.x && p.x <= B.x + B.w, SPANY = p.y >= B.y && p.y <= B.y + B.h;
            if (SPANX && p.y >= B.y - OUT && p.y <= B.y + IN)
                return Layout::CORNER_TOP;
            if (SPANX && p.y >= B.y + B.h - IN && p.y <= B.y + B.h + OUT)
                return Layout::CORNER_BOTTOM;
            if (SPANY && p.x >= B.x - OUT && p.x <= B.x + IN)
                return Layout::CORNER_LEFT;
            if (SPANY && p.x >= B.x + B.w - IN && p.x <= B.x + B.w + OUT)
                return Layout::CORNER_RIGHT;
            return Layout::CORNER_NONE;
        }

        const char* cursorFor(Layout::eRectCorner c) {
            switch (c) {
                case Layout::CORNER_TOPLEFT: return "nw-resize";
                case Layout::CORNER_TOPRIGHT: return "ne-resize";
                case Layout::CORNER_BOTTOMLEFT: return "sw-resize";
                case Layout::CORNER_TOP: return "n-resize";
                case Layout::CORNER_BOTTOM: return "s-resize";
                case Layout::CORNER_LEFT: return "w-resize";
                case Layout::CORNER_RIGHT: return "e-resize";
                default: return "se-resize";
            }
        }

        // Which window's corner the pointer is on, if any. The corner zone
        // reaches past the window into the gap, where the hit test finds no
        // window -- so every window of the visible desktops is asked, the
        // focused one first (floating windows overlap).
        std::pair<PHLWINDOW, Layout::eRectCorner> cornerTarget(const Vector2D& p, bool superHeld) {
            const auto TRY = [&](const PHLWINDOW& w) -> Layout::eRectCorner {
                if (!w || !w->m_isMapped || w->isHidden() || !w->m_workspace || !w->m_workspace->isVisible() || Fullscreen::controller()->isFullscreen(w) ||
                    (w->m_isX11 && w->isX11OverrideRedirect()) || !w->layoutTarget())
                    return Layout::CORNER_NONE;
                // Without Super only a TILED window of a mosaic desktop has corners.
                if (!superHeld && (w->layoutTarget()->floating() || !onMosaic(w)))
                    return Layout::CORNER_NONE;
                return cornerAt(w, p, superHeld);
            };
            if (const auto F = Desktop::focusState()->window(); F)
                if (const auto C = TRY(F); C != Layout::CORNER_NONE)
                    return {F, C};
            for (const auto& W : Desktop::windowState()->windows())
                if (const auto C = TRY(W); C != Layout::CORNER_NONE)
                    return {W, C};
            return {nullptr, Layout::CORNER_NONE};
        }

        // Plain (no modifier) or Super alone; anything else is someone's chord.
        int modsNow() {
            return sc<int>(g_pInputManager->getModsFromAllKBs());
        }
    }

    // A mosaic resize ends when the drag controller lets the window go --
    // whoever started the drag (our corner grab, or Super + right-drag). Asked
    // when our own drag ends, and on every pointer motion.
    static void checkResizeEnded() {
        const auto DRAGGED = g_layoutManager ? g_layoutManager->dragController()->target() : SP<Layout::ITarget>{};
        for (auto* A : g_instances)
            if (A->resizing() && !(DRAGGED && A->resizingTarget(DRAGGED)))
                A->finishResize();
    }

    static void canvasClick() {
        const auto W = Desktop::viewState()->hitTest().windowAt(g_pInputManager->getMouseCoordsInternal(),
                                                                 Desktop::View::RESERVED_EXTENTS | Desktop::View::INPUT_EXTENTS | Desktop::View::ALLOW_FLOATING);
        if (!W)
            return;
        for (auto* A : g_instances)
            if (A->holds(W)) {
                A->clickReveal(W);
                return;
            }
    }

    // ------------------------------------------------ Super held, pointer moved --
    // What a held Super is doing on the canvas. ARMED: Super is down, nothing
    // decided yet. A button pressed then (Super+drag, Super+click) SPENDS it:
    // no zoom until Super is let go -- the window being aimed at stays
    // put, and so does the canvas after the drop (user, 2026-10-26: Super+drag
    // "is buggy"; the pan used to fire on the way to the window and again
    // after the drop). Moving SUPER_SLOP px with no button ENGAGES it: the
    // whole canvas shrinks onto the screen, hovering a window focuses it, and
    // letting go of Super zooms back in on it. (A second mode, where the
    // pointer's place on the screen was the view's place on the canvas, was
    // retired: user, 2026-10-29. mosaic:canvas_super_move is still accepted
    // so older configs load, and ignored.)
    namespace {
        enum class ESuper : uint8_t { IDLE, ARMED, ZOOMED, SPENT };
        ESuper            g_super = ESuper::IDLE;
        Vector2D          g_superAnchor;
        CMosaicAlgorithm* g_superCanvas = nullptr; // the zoomed one
        bool              g_swallowRelease = false;
        bool              g_zoomPressing   = false; // a button went down in the zoomed-out view
        uint32_t          g_zoomButton     = 0;     // which: BTN_LEFT carries or resizes, BTN_RIGHT resizes
        constexpr double  SUPER_SLOP = 32.0;
        PHLWINDOWREF      g_labelHover; // the window whose title the hover shows (mosaic:zoom_labels)
        bool              g_superLabels = false; // Super held and the pointer moved: titles up

        // THE TITLES FADE (user, 2026-11-01: "those labels should adopt the
        // same fade in/out animations as the rest of the labels in
        // hyprdrive"): the dock tooltip's clock and curves (Dock.cpp
        // TOOLTIP_FADE_MS / _OUT_MS, houseEase), one per window. A title wanted
        // again mid-exit turns around from the alpha it had, no dip to zero.
        constexpr float LABEL_FADE_MS     = 150.F;
        constexpr float LABEL_FADE_OUT_MS = 110.F; // exits ~25% quicker, house rule
        struct SLabelFade {
            PHLWINDOWREF                          window;
            std::chrono::steady_clock::time_point since;
            float                                 from = 0.F; // the alpha the fade started at
            bool                                  out  = false;
        };
        std::vector<SLabelFade> g_labelFades;

        float labelFadeProgress(const SLabelFade& f) {
            const float MS = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - f.since).count();
            return std::clamp(MS / (f.out ? LABEL_FADE_OUT_MS : LABEL_FADE_MS), 0.F, 1.F);
        }
        float labelFadeAlpha(const SLabelFade& f) {
            const float P = labelFadeProgress(f);
            return f.out ? f.from * (1.F - houseEase(false, P)) : f.from + (1.F - f.from) * houseEase(true, P);
        }
        void labelFadeTo(SLabelFade& f, bool out) {
            f.from  = labelFadeAlpha(f);
            f.out   = out;
            f.since = std::chrono::steady_clock::now();
        }
    }

    // A SCREEN PICKER IS UP (user, 2026-10-27: "fix taking screenshots while
    // zoomed out"): a region picker (slurp, hyprpicker's freeze) is a layer
    // surface over a whole monitor on the overlay layer. While one is up the
    // zoomed-out canvas stays drawn for the screenshot, but the pointer and
    // its buttons are the picker's.
    static bool pickerOpen() {
        for (const auto& M : State::monitorState()->monitors())
            for (const auto& LSREF : M->m_layerSurfaceLayers[ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY]) {
                const auto LS = LSREF.lock();
                if (!Desktop::View::validMapped(LS))
                    continue;
                const auto& G = LS->m_geometry;
                if (G.w >= M->m_size.x - 1 && G.h >= M->m_size.y - 1)
                    return true;
            }
        return false;
    }

    // The titles come down (Super let go, or spent on a click or a drag).
    static void dropSuperLabels() {
        if (!g_superLabels)
            return;
        g_superLabels = false;
        g_labelHover.reset();
        // Their exits start now, not at the next frame drawn without the
        // overview: one that opens on this release must not leave a stale
        // title to fade out when it closes.
        for (auto& f : g_labelFades)
            if (!f.out)
                labelFadeTo(f, true);
        for (const auto& M : State::monitorState()->monitors())
            if (M)
                g_pHyprRenderer->damageMonitor(M);
    }

    static void armSlideWatch(); // DESKTOP SLIDES, below

    static CMosaicAlgorithm* liveZoomed() {
        return g_superCanvas && std::ranges::find(g_instances, g_superCanvas) != g_instances.end() ? g_superCanvas : nullptr;
    }

    // The gesture is over (Super let go, another modifier joined, unload).
    static void superEnd(bool superStillDown) {
        dropSuperLabels();
        if (auto* A = liveZoomed()) {
            // Let go of Super mid-drag: the drag is off; the button's release,
            // still to come, is ours.
            if (g_zoomPressing) {
                A->zoomDragCancel();
                g_zoomPressing   = false;
                g_swallowRelease = true;
            }
            A->zoomIn();
        }
        g_superCanvas = nullptr;
        if (g_super == ESuper::ZOOMED)
            g_canvasPanning = false;
        g_super = superStillDown ? ESuper::SPENT : ESuper::IDLE;
        // Desktops kept zoomed out off screen for a swipe end now (slideWatch).
        armSlideWatch();
    }

    void onSuperKey(bool pressed) {
        if (pressed) {
            if (g_super == ESuper::IDLE) {
                g_super       = ESuper::ARMED;
                g_superAnchor = g_pInputManager->getMouseCoordsInternal();
            }
            return;
        }
        superEnd(false);
    }

    void renderZoomLabels(PHLMONITOR monitor) {
        if (!monitor)
            return;
        auto*      A      = liveZoomed();
        const auto WS     = A ? A->workspace() : nullptr;
        const bool ZOOMED = WS && monitor->m_activeWorkspace == WS && !monitor->m_activeSpecialWorkspace;
        const auto SHOWN  = monitor->m_activeSpecialWorkspace ? monitor->m_activeSpecialWorkspace : monitor->m_activeWorkspace;

        // The windows that want a title this frame. None once Super is let go:
        // the titles still up fade out below.
        std::vector<PHLWINDOW> want;
        const auto             MODE = ScrollOverview::Config::getValue<std::string>("plugin:hyprdrive:mosaic:zoom_labels");
        if (g_superLabels && g_super != ESuper::IDLE && g_super != ESuper::SPENT && !openScrollOverviewForMonitor(monitor) && (MODE == "hover" || MODE == "all")) {
            // Looked up fresh, not g_labelHover (that only asks for the redraws):
            // a desktop switch under a still pointer changes the window under it.
            const Vector2D P     = g_pInputManager->getMouseCoordsInternal();
            const auto     HOVER = ZOOMED ? A->zoomedWindowAt(P) :
                                            Desktop::viewState()->hitTest().windowAt(P, Desktop::View::RESERVED_EXTENTS | Desktop::View::INPUT_EXTENTS | Desktop::View::ALLOW_FLOATING);
            // The zoomed-out canvas: on the zoomed boxes.
            if (ZOOMED)
                A->zoomLabelWindows(MODE == "all", HOVER, want);
            // Any other desktop, any layout: the windows where they are. Under a
            // fullscreen window only it and what floats over it are seen.
            else if (SHOWN) {
                const auto FS = Fullscreen::controller()->getFullscreenWindow(SHOWN);
                for (const auto& W : Desktop::windowState()->windows()) {
                    if (!W || !W->m_isMapped || W->isHidden() || W->m_workspace != SHOWN || (FS && W != FS && !W->m_isFloating))
                        continue;
                    if (MODE == "hover" && W != HOVER)
                        continue;
                    want.push_back(W);
                }
            }
        }

        // Each title in or out on its own fade. Only this monitor's windows:
        // another monitor's render decides theirs.
        std::erase_if(g_labelFades, [](const SLabelFade& f) { return !f.window.lock() || (f.out && labelFadeProgress(f) >= 1.F); });
        for (const auto& W : want) {
            auto it = std::ranges::find_if(g_labelFades, [&](const SLabelFade& f) { return f.window.lock() == W; });
            if (it == g_labelFades.end())
                g_labelFades.push_back({.window = W, .since = std::chrono::steady_clock::now()});
            else if (it->out)
                labelFadeTo(*it, false);
        }
        for (auto& f : g_labelFades) {
            const auto W = f.window.lock();
            if (!f.out && W->m_monitor == monitor && std::ranges::find(want, W) == want.end())
                labelFadeTo(f, true);
        }

        for (const auto& f : g_labelFades) {
            const auto W = f.window.lock();
            if (W->m_monitor != monitor || !W->m_isMapped)
                continue;
            // On the zoomed canvas under the zoom's own fade; elsewhere only a
            // window on the desktop shown, as it is.
            const bool ONZOOM = ZOOMED && A->holds(W);
            if (!ONZOOM && (W->isHidden() || W->m_workspace != SHOWN))
                continue;
            const float ALPHA = labelFadeAlpha(f) * (ONZOOM ? A->zoomLabelAlpha() : 1.F);
            if (ALPHA > 0.005F)
                drawWindowLabel(monitor, W, ALPHA);
        }
    }

    // One frame past the end too, so the last one drawn is the settled alpha
    // (CScrollOverview::stripTipAnimating).
    bool labelsAnimating() {
        const auto NOW = std::chrono::steady_clock::now();
        return std::ranges::any_of(g_labelFades, [&](const SLabelFade& f) {
            return f.window.lock() && std::chrono::duration<float, std::milli>(NOW - f.since).count() < (f.out ? LABEL_FADE_OUT_MS : LABEL_FADE_MS) + 16.F;
        });
    }

    PHLWINDOW zoomCarried() {
        auto* A = liveZoomed();
        return A && g_zoomPressing ? A->zoomCarried() : nullptr;
    }

    // A ZOOMED-OUT DRAG HELD AT THE SCREEN'S EDGE (user, 2026-10-29: pushing a
    // window to the next desktop "doesn't work while zoomed out in infinite
    // canvas"; "take into account when I'm pushing the window to a desktop
    // that isn't set to mosaic"). The zoomed drag is the canvas's own, and a
    // switch needs a window Hyprland is moving (Spring: the pin carries it,
    // the release is finished across the switch). So the hold, complete, hands
    // it over: the zoom ends where it is and the window becomes an ordinary
    // Super+drag under the hand -- which any desktop, mosaic or not, takes as
    // it takes any dragged window, and can be pushed on again.
    bool handZoomDragOff() {
        auto*      A = liveZoomed();
        const auto W = zoomCarried();
        if (!A || !W || !W->layoutTarget() || !g_layoutManager || g_layoutManager->dragController()->target())
            return false;
        const CBox HERE = A->zoomCarriedBox(g_pInputManager->getMouseCoordsInternal());
        A->zoomDragCancel();
        g_zoomPressing = false;
        A->endZoomNow();
        g_superCanvas   = nullptr;
        g_canvasPanning = false;
        g_super         = ESuper::SPENT; // Super still down: no new zoom until it is let go
        dropSuperLabels();
        g_layoutManager->beginDragTarget(W->layoutTarget(), MBIND_MOVE);
        // It grows from where it was drawn, under the hand, to its own size
        // there -- not from its place on the canvas (off the screen, as often
        // as not), which the zoom's end had put it back to.
        if (W->positionAnimation() && W->sizeAnimation() && HERE.w >= 1 && HERE.h >= 1) {
            W->positionAnimation()->setValue(HERE.pos());
            W->sizeAnimation()->setValue(HERE.size());
        }
        // Its release is ours (onButton), as a corner resize's is.
        g_ourDrag = g_layoutManager->dragController()->target() != nullptr;
        return g_ourDrag;
    }

    // ALL DIRECTIONS (user, 2026-10-26: "I can hold down super and my cursor
    // movement maps to the entire collection of the screens so that ... the
    // top/bottom portions of the screen result in my cursor being at the
    // top/bottom of the canvas"). Super alone held, no button: the pointer's
    // place on its monitor is the view's place on the canvas, both axes.
    bool onButton(const IPointer::SButtonEvent& e, bool blocked) {
        const bool PRESSED = e.state == WL_POINTER_BUTTON_STATE_PRESSED;
        // THE SUPER GESTURE. Zoomed out, a click picks the window under it and
        // zooms back in on it -- the windows are drawn small, the app would
        // get the click at the wrong place. Armed, a press is a Super+click
        // or Super+drag: no pan or zoom until Super is let go.
        // Zoomed out, the LEFT button grabs the window under it (zoomPress):
        // dragged, it is dropped where it is let go; a click picks it.
        // The RIGHT button resizes the window under it from its nearest
        // corner (Super + right-drag, as on the desktop).
        const bool ZOOMBUTTON = e.button == BTN_LEFT || e.button == BTN_RIGHT;
        if (g_super == ESuper::ZOOMED && !g_zoomPressing && !g_swallowRelease && pickerOpen())
            return false; // a screenshot's region picker: its click
        if (ZOOMBUTTON && PRESSED && g_super == ESuper::ZOOMED && !g_zoomPressing)
            if (auto* A = liveZoomed(); A && A->zoomPress(g_pInputManager->getMouseCoordsInternal(), e.button == BTN_RIGHT)) {
                g_zoomPressing = true;
                g_zoomButton   = e.button;
                return true;
            }
        if (!PRESSED && g_zoomPressing && e.button == g_zoomButton) {
            g_zoomPressing = false;
            auto*          A = liveZoomed();
            const Vector2D P = g_pInputManager->getMouseCoordsInternal();
            // A right click that never moved resizes nothing: still zoomed out.
            if (A && !A->zoomRelease(P) && e.button == BTN_LEFT) {
                if (const auto W = A->zoomedWindowAt(P))
                    Desktop::focusState()->fullWindowFocus(W, Desktop::FOCUS_REASON_FFM);
                superEnd(true);
            }
            return true;
        }
        if (PRESSED && g_super == ESuper::ZOOMED) {
            if (auto* A = liveZoomed()) {
                if (const auto W = A->zoomedWindowAt(g_pInputManager->getMouseCoordsInternal()))
                    Desktop::focusState()->fullWindowFocus(W, Desktop::FOCUS_REASON_FFM);
            }
            superEnd(true);
            g_swallowRelease = true;
            return true;
        }
        if (!PRESSED && g_swallowRelease) {
            g_swallowRelease = false;
            return true;
        }
        if (PRESSED && g_super == ESuper::ARMED) {
            g_super = ESuper::SPENT;
            dropSuperLabels(); // a Super+click or Super+drag: no titles over it
        }
        // A plain left click on a canvas window pans it into view (clickReveal).
        if (PRESSED && !blocked && e.button == BTN_LEFT && modsNow() == 0 && !(g_layoutManager && g_layoutManager->dragController()->target()))
            canvasClick();
        if (!PRESSED) {
            if (g_ourDrag && e.button == BTN_LEFT) {
                g_ourDrag = false;
                if (g_layoutManager && g_layoutManager->dragController()->target())
                    g_layoutManager->endDragTarget();
                // Our drag is over now: settle the resize at once. (A drag
                // Hyprland started -- Super+right-drag -- ends after this hook;
                // the next pointer motion settles it. No doLater: one still
                // queued at unload jumps into unmapped code.)
                checkResizeEnded();
                return true;
            }
            return false;
        }
        if (blocked || e.button != BTN_LEFT || !g_layoutManager || g_layoutManager->dragController()->target())
            return false;
        const int MODS = modsNow();
        if (MODS != 0 && MODS != HL_MODIFIER_META)
            return false;
        const auto [W, CORNER] = cornerTarget(g_pInputManager->getMouseCoordsInternal(), MODS == HL_MODIFIER_META);
        if (!W || CORNER == Layout::CORNER_NONE)
            return false;
        g_layoutManager->beginDragTarget(W->layoutTarget(), MBIND_RESIZE, CORNER);
        g_ourDrag = g_layoutManager->dragController()->target() != nullptr;
        return g_ourDrag;
    }

    // ------------------------------------------------------------ the canvas view --
    static CMosaicAlgorithm* canvasOf(const PHLWORKSPACE& ws) {
        if (!ws)
            return nullptr;
        for (auto* A : g_instances)
            if (A->workspace() == ws && A->arrangedAsCanvas())
                return A;
        return nullptr;
    }

    std::optional<CBox> canvasBox(const PHLWORKSPACE& ws) {
        const auto* A = canvasOf(ws);
        const auto  V = A ? A->canvasViewNow() : std::nullopt;
        const auto  S = A ? A->canvasSize() : Vector2D{};
        if (!V || S.x < 1 || S.y < 1)
            return std::nullopt;
        return CBox{V->originX - A->canvasOffset().x, V->originY - A->canvasOffset().y, S.x, S.y};
    }

    // ---------------------------------------------------- the canvas's clock --
    namespace {
        wl_event_source*  g_canvasTimer = nullptr;
        constexpr int     CANVAS_TICK_MS = 8; // half a 60 Hz frame: always one booked
        // A Super+drag pushing at the screen's edge pans the canvas under it
        // (user, 2026-10-26: Super+drag on the canvas "doesn't behave ... like
        // regular mosaic"): the far groups are otherwise out of reach.
        Vector2D          g_edgePush;       // -1/0/1 per axis while pushing
        PHLWORKSPACEREF   g_edgeWorkspace;
        std::chrono::steady_clock::time_point g_edgeLast;
        constexpr double  EDGE_ZONE  = 4.0;    // px from the outer edge
        constexpr double  EDGE_SPEED = 1400.0; // canvas px per second
    }
    static bool edgeTick(std::chrono::steady_clock::time_point now);

    static int canvasTickCb(void*) {
        const auto NOW  = std::chrono::steady_clock::now();
        bool       busy = false;
        // A copy: a tick can re-lay a desktop out, never add or drop a layout,
        // but be safe against it anyway.
        for (auto* A : std::vector<CMosaicAlgorithm*>{g_instances})
            busy |= A->tick(NOW);
        busy |= edgeTick(NOW);
        if (busy && g_canvasTimer)
            wl_event_source_timer_update(g_canvasTimer, CANVAS_TICK_MS);
        return 0;
    }

    static void armCanvasTick() {
        if (!g_canvasTimer)
            g_canvasTimer = wl_event_loop_add_timer(g_pCompositor->m_wlEventLoop, canvasTickCb, nullptr);
        if (g_canvasTimer)
            wl_event_source_timer_update(g_canvasTimer, 1);
    }

    static CMosaicAlgorithm* reachingOn(const PHLWORKSPACE& ws);

    // ONLY AS FAR AS THE HAND MOVES (user, 2026-10-27: "it shouldn't just keep
    // expanding as long as my drag is held against the corner, it leads to
    // uncontrollable resizing. It should only expand as far as I'm moving the
    // cursor"). The pointer stops at the screen's edge; what the mouse moved
    // past it -- `delta` minus what the pointer actually moved -- is what the
    // resize reaches past the edge by. Held still, nothing grows.
    void onPointerDelta(const Vector2D& delta, const Vector2D& moved) {
        const Vector2D LOST = delta - moved;
        if (std::abs(LOST.x) < 0.01 && std::abs(LOST.y) < 0.01)
            return;
        const Vector2D P   = g_pInputManager->getMouseCoordsInternal();
        const auto     MON = State::monitorState()->query().vec(P).run();
        if (!MON || MON->m_activeSpecialWorkspace)
            return;
        auto* A = reachingOn(MON->m_activeWorkspace);
        if (!A)
            return;
        // Only past an outer edge (no monitor that way) the pointer is at.
        const CBox B     = MON->logicalBox();
        const auto OUTER = [&](Math::eDirection d) {
            const auto M = State::monitorState()->query().relativeTo(MON).inDirection(d).run();
            return !M || M == MON;
        };
        Vector2D r;
        if (LOST.x > 0 && P.x >= B.x + B.w - 2 && OUTER(Math::DIRECTION_RIGHT))
            r.x = LOST.x;
        else if (LOST.x < 0 && P.x <= B.x + 1 && OUTER(Math::DIRECTION_LEFT))
            r.x = LOST.x;
        if (canvasAllDirections()) {
            if (LOST.y > 0 && P.y >= B.y + B.h - 2 && OUTER(Math::DIRECTION_DOWN))
                r.y = LOST.y;
            else if (LOST.y < 0 && P.y <= B.y + 1 && OUTER(Math::DIRECTION_UP))
                r.y = LOST.y;
        }
        if (r.x != 0 || r.y != 0)
            A->resizeReach(r);
    }

    // The desktop on `ws` whose resize may reach past the screen's edge.
    static CMosaicAlgorithm* reachingOn(const PHLWORKSPACE& ws) {
        if (!ws)
            return nullptr;
        for (auto* A : g_instances)
            if (A->workspace() == ws && A->resizeCanReach())
                return A;
        return nullptr;
    }

    static bool edgeTick(std::chrono::steady_clock::time_point now) {
        if (g_edgePush.x == 0 && g_edgePush.y == 0)
            return false;
        const auto DRAGGED = g_layoutManager ? g_layoutManager->dragController()->target() : SP<Layout::ITarget>{};
        auto*      A       = canvasOf(g_edgeWorkspace.lock());
        // A resize is never panned by the clock: it reaches past the edge by
        // the motion the edge swallows (onPointerDelta).
        if (!DRAGGED || !A || reachingOn(g_edgeWorkspace.lock())) {
            g_edgePush = {};
            return false;
        }
        const double DT = std::min(0.05, std::chrono::duration<double>(now - g_edgeLast).count());
        g_edgeLast      = now;
        const auto O    = A->canvasOffset();
        A->setCanvasOffset(O + g_edgePush * (EDGE_SPEED * DT), true);
        return true;
    }

    bool canvasMotion(const PHLWINDOW& w) {
        if (!w || !w->m_workspace)
            return false;
        for (const auto* A : g_instances)
            if (A->holds(w))
                return (A->arrangedAsCanvas() || A->resizeCanReach()) &&
                    (A->viewMoving() || g_canvasPanning || ((g_edgePush.x != 0 || g_edgePush.y != 0) && g_edgeWorkspace.lock() == w->m_workspace));
        return false;
    }

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
    static wl_event_source* g_slideTimer = nullptr;
    static int              slideWatch(void*);

    static void armSlideWatch() {
        if (!g_pCompositor || !g_pCompositor->m_wlEventLoop)
            return;
        if (!g_slideTimer)
            g_slideTimer = wl_event_loop_add_timer(g_pCompositor->m_wlEventLoop, slideWatch, nullptr);
        if (g_slideTimer)
            wl_event_source_timer_update(g_slideTimer, 16);
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
            trace(std::format("swipe release goes past the overhang: {:.0f} -> {:.0f}", G, G * K));
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

    // Is the pointer pushing at an outer edge of its monitor (no monitor
    // beyond it) while a Super+drag is over a canvas desktop?
    static void updateEdgePush() {
        const auto DRAGGED = g_layoutManager ? g_layoutManager->dragController()->target() : SP<Layout::ITarget>{};
        Vector2D   push;
        PHLWORKSPACE ws;
        if (DRAGGED && DRAGGED->window()) {
            const Vector2D P   = g_pInputManager->getMouseCoordsInternal();
            const auto     MON = State::monitorState()->query().vec(P).run();
            if (MON && !MON->m_activeSpecialWorkspace && canvasOf(MON->m_activeWorkspace) && !reachingOn(MON->m_activeWorkspace)) {
                const CBox B       = MON->logicalBox();
                // An outer edge: no monitor that way (vec() would answer the
                // CLOSEST monitor, this one, for a point past the edge).
                const auto OUTER = [&](Math::eDirection d) {
                    const auto M = State::monitorState()->query().relativeTo(MON).inDirection(d).run();
                    return !M || M == MON;
                };
                if (P.x <= B.x + EDGE_ZONE && OUTER(Math::DIRECTION_LEFT))
                    push.x = -1;
                else if (P.x >= B.x + B.w - EDGE_ZONE && OUTER(Math::DIRECTION_RIGHT))
                    push.x = 1;
                if (canvasAllDirections()) {
                    if (P.y <= B.y + EDGE_ZONE && OUTER(Math::DIRECTION_UP))
                        push.y = -1;
                    else if (P.y >= B.y + B.h - EDGE_ZONE && OUTER(Math::DIRECTION_DOWN))
                        push.y = 1;
                }
                ws = MON->m_activeWorkspace;
            }
        }
        const bool START = (push.x != 0 || push.y != 0) && g_edgePush.x == 0 && g_edgePush.y == 0;
        g_edgePush       = push;
        g_edgeWorkspace  = ws;
        if (START) {
            g_edgeLast = std::chrono::steady_clock::now();
            armCanvasTick();
        }
    }

    // The Super gesture, on every pointer motion: the canvas shrinks onto the
    // screen once, and hovering a window focuses it.
    static void superMove(bool blocked) {
        const int MODS = modsNow();
        if (!(MODS & HL_MODIFIER_META)) {
            if (g_super != ESuper::IDLE)
                superEnd(false);
            return;
        }
        if (MODS != HL_MODIFIER_META) {
            // Another modifier joined: someone's chord, not this. Zoomed out,
            // the view stays until Super is let go: Super+Shift+S takes a
            // screenshot of the zoomed-out canvas, not of the view behind it.
            if (g_super != ESuper::IDLE && g_super != ESuper::SPENT && g_super != ESuper::ZOOMED)
                superEnd(true);
            return;
        }
        const Vector2D P = g_pInputManager->getMouseCoordsInternal();
        if (g_super == ESuper::IDLE) {
            // Super went down before we heard it (held across a reload).
            g_super       = ESuper::ARMED;
            g_superAnchor = P;
        }
        if (g_super == ESuper::SPENT || blocked || g_cursorRiding || (g_layoutManager && g_layoutManager->dragController()->target()))
            return;
        const auto MON = State::monitorState()->query().vec(P).run();
        if (!MON || openScrollOverviewForMonitor(MON))
            return;
        // TITLES WHILE SUPER IS HELD (user, 2026-10-30: "have them appear when
        // I'm holding super and moving my mouse, even if it doesn't zoom out,
        // and have the labels work in any desktop mode"): past the slop they
        // come up on any desktop, canvas or not; the zoom below is the
        // canvas's own. mosaic:zoom_labels picks none, the hovered window, or
        // all. The hovered one is tracked here, and a change redraws the whole
        // screen so the last pill goes with it.
        if (!g_superLabels) {
            const Vector2D D = P - g_superAnchor;
            if (g_super == ESuper::ZOOMED || D.x * D.x + D.y * D.y >= SUPER_SLOP * SUPER_SLOP) {
                g_superLabels = true;
                g_pHyprRenderer->damageMonitor(MON);
            }
        }
        if (g_superLabels) {
            const auto* Z = liveZoomed();
            const auto  W = Z ? Z->zoomedWindowAt(P) :
                                Desktop::viewState()->hitTest().windowAt(P, Desktop::View::RESERVED_EXTENTS | Desktop::View::INPUT_EXTENTS | Desktop::View::ALLOW_FLOATING);
            if (W != g_labelHover.lock()) {
                g_labelHover = W;
                g_pHyprRenderer->damageMonitor(MON);
            }
        }
        if (MON->m_activeSpecialWorkspace)
            return;
        auto*      A = canvasOf(MON->m_activeWorkspace);
        const auto V = A ? A->canvasViewNow() : std::nullopt;
        if (!V || (V->maxX <= 1.0 && V->maxY <= 1.0))
            return;

        if (g_super == ESuper::ARMED) {
            const Vector2D D = P - g_superAnchor;
            if (D.x * D.x + D.y * D.y < SUPER_SLOP * SUPER_SLOP)
                return;
            if (!A->zoomOut())
                return;
            g_super       = ESuper::ZOOMED;
            g_superCanvas = A;
            // The window under the pointer at full size lets go of it
            // (zoomOwnsPointer: no motion reaches the clients while zoomed).
            g_pSeatManager->setPointerFocus(nullptr, {});
            g_canvasPanning = true;
            // Super was held for this, not tapped: no overview on its release.
            ScrollOverview::cancelSuperTap();
        }

        if (g_super == ESuper::ZOOMED && g_zoomPressing) {
            A->zoomMove(P); // carrying a window (zoomPress)
            return;
        }
        if (g_super == ESuper::ZOOMED && pickerOpen())
            return; // the picker has the pointer: no hover picks under it
        if (g_super == ESuper::ZOOMED) {
            // Hovering a window focuses it (as focus follows the mouse, but
            // whatever input:follow_mouse says: this is the zoom's own pick).
            if (const auto W = A->zoomedWindowAt(P); W && W != Desktop::focusState()->window())
                Desktop::focusState()->fullWindowFocus(W, Desktop::FOCUS_REASON_FFM);
            return;
        }
    }

    // THE PANEL TURNS THE CANVAS ON OR OFF LIVE (user, 2026-10-26: "when
    // turning off infinite canvas in settings, it should immediately go back
    // to regular mosaic"): the panel applies a key with `hyprctl eval
    // hl.config(...)`, which is no config reload, so the reload hook never
    // heard it. Two cached reads per pointer motion (the hand that just
    // clicked the switch moves on) see the change instead.
    static void watchCanvasKeys() {
        if (!ScrollOverview::Config::g_pluginKeysRegistered)
            return;
        static auto&       ON   = ScrollOverview::Config::valueRef<Hyprlang::INT>("plugin:hyprdrive:mosaic:infinite_canvas");
        static auto&       DIRS = ScrollOverview::Config::valueRef<std::string>("plugin:hyprdrive:mosaic:canvas_directions");
        static int64_t     lastOn   = -1;
        static std::string lastDirs;
        const int64_t      NOWON    = *ON;
        const std::string& NOWDIRS  = *DIRS;
        if (NOWON == lastOn && NOWDIRS == lastDirs)
            return;
        const bool FIRST = lastOn < 0;
        lastOn           = NOWON;
        lastDirs         = NOWDIRS;
        if (FIRST)
            return;
        for (auto* A : std::vector<CMosaicAlgorithm*>{g_instances})
            A->canvasSettingsChanged();
    }

    // THE ZOOMED CANVAS OWNS THE POINTER (user, 2026-10-27: "when I hold super
    // it keeps selecting the browser even though my cursor is hovering over
    // another window"). The windows are only DRAWN small: at their real, full
    // sized places the pointer was over the browser, so Hyprland's own focus
    // follows mouse focused it on every motion, against the zoom's hover pick
    // (zoomedWindowAt), and the browser got the hover (its sidebar opened).
    // THE USER'S SCREENSHOT KEY, ZOOMED OUT (user, 2026-10-27: "I meant
    // whatever the user has for screenshots in Omarchy. For example I do 3
    // fingers press on track pad or press print screen"). Super is held for
    // the zoom, so Print arrives as Super+Print -- Omarchy's colour picker.
    // While zoomed out, a key whose Super chord is not a screenshot bind but
    // whose plain chord (the same mods without Super) is, runs that
    // screenshot. A screenshot bind is one whose description or command says
    // so; whatever keys the user gave it, they work.
    namespace {
        std::unordered_set<uint32_t> g_zoomKeysTaken; // presses taken: their releases too
        std::unordered_set<uint32_t> g_zoomKeysSent;  // presses handed to the picker: their releases too
        bool isScreenshotBind(const SP<SKeybind>& k) {
            std::string s = k->description + " " + k->arg;
            std::ranges::transform(s, s.begin(), [](unsigned char c) { return sc<char>(std::tolower(c)); });
            for (const char* W : {"screenshot", "hyprshot", "grimblast", "flameshot", "grim "})
                if (s.find(W) != std::string::npos)
                    return true;
            return false;
        }
        SP<SKeybind> bindFor(uint32_t mods, xkb_keysym_t sym) {
            if (!g_pKeybindManager)
                return nullptr;
            for (const auto& K : g_pKeybindManager->m_keybinds) {
                if (!K || !K->enabled || K->mouse || K->release || !K->submap.name.empty() || K->modmask != mods || K->key.empty())
                    continue;
                if (xkb_keysym_from_name(K->key.c_str(), XKB_KEYSYM_CASE_INSENSITIVE) == sym)
                    return K;
            }
            return nullptr;
        }
    }

    bool onZoomKey(uint32_t keycode, bool pressed, uint32_t timeMs) {
        if (!pressed) {
            if (g_zoomKeysSent.erase(keycode) > 0) {
                if (g_pSeatManager)
                    g_pSeatManager->sendKeyboardKey(timeMs, keycode, WL_KEYBOARD_KEY_STATE_RELEASED);
                return true;
            }
            return g_zoomKeysTaken.erase(keycode) > 0;
        }
        if (g_super != ESuper::ZOOMED || !liveZoomed())
            return false;
        const auto KB = g_pSeatManager ? g_pSeatManager->m_keyboard.lock() : nullptr;
        if (!KB || !KB->m_xkbState)
            return false;
        const xkb_keysym_t SYM  = xkb_state_key_get_one_sym(KB->m_xkbState, keycode + 8);
        const uint32_t     MODS = g_pInputManager->getModsFromAllKBs();
        if (SYM == XKB_KEY_NoSymbol || !(MODS & HL_MODIFIER_META))
            return false;
        // THE PICKER'S OWN KEYS (user, 2026-10-28: Escape out of the picker
        // opened the System menu): while it is up, Super is only held for the
        // zoom, so a key is what it would be without Super -- the picker's
        // binds (Omarchy's Return, Tab, arrows), or else the key itself, sent
        // to the picker (Escape cancels it), never a Super bind.
        if (pickerOpen()) {
            if (!bindFor(MODS, SYM))
                return false; // no Super bind to steal it: Hyprland delivers it
            if (const auto PLAIN = bindFor(MODS & ~HL_MODIFIER_META, SYM)) {
                const auto IT = g_pKeybindManager->m_dispatchers.find(PLAIN->handler);
                if (IT != g_pKeybindManager->m_dispatchers.end()) {
                    trace(std::format("zoomed picker key {} -> {} {}", PLAIN->key, PLAIN->handler, PLAIN->description));
                    IT->second(PLAIN->arg);
                    g_zoomKeysTaken.insert(keycode);
                    return true;
                }
            }
            trace(std::format("zoomed picker key {} -> the picker", keycode));
            g_pSeatManager->sendKeyboardKey(timeMs, keycode, WL_KEYBOARD_KEY_STATE_PRESSED);
            g_zoomKeysSent.insert(keycode);
            return true;
        }
        if (const auto OWN = bindFor(MODS, SYM); OWN && isScreenshotBind(OWN))
            return false; // the Super chord is a screenshot itself (Super+Shift+S): Hyprland runs it
        const auto PLAIN = bindFor(MODS & ~HL_MODIFIER_META, SYM);
        if (!PLAIN || !isScreenshotBind(PLAIN))
            return false;
        const auto IT = g_pKeybindManager->m_dispatchers.find(PLAIN->handler);
        if (IT == g_pKeybindManager->m_dispatchers.end())
            return false;
        trace(std::format("zoomed screenshot key {} -> {} {}", PLAIN->key, PLAIN->handler, PLAIN->description));
        IT->second(PLAIN->arg);
        g_zoomKeysTaken.insert(keycode);
        return true;
    }

    bool zoomOwnsPointer() {
        const auto* A = liveZoomed();
        return A && A->zoomed() && !pickerOpen();
    }

    void onMove(bool blocked) {
        watchCanvasKeys();
        superMove(blocked);
        updateEdgePush();
        if (std::ranges::any_of(g_instances, [](const CMosaicAlgorithm* a) { return a->resizing(); }))
            checkResizeEnded();
        // A drag hold whose window was let go elsewhere, or carried to another
        // desktop: close up now.
        for (auto* A : g_instances)
            if (A->holdingFor() && !A->dragHoldLive())
                A->releaseDragHold();
        const bool DRAGGING = g_layoutManager && g_layoutManager->dragController()->target();
        Layout::eRectCorner corner = Layout::CORNER_NONE;
        if (const auto* Z = liveZoomed(); Z && Z->zoomed()) {
            // Zoomed out: the corners are the drawn ones (and a resize under
            // way keeps its cursor).
            corner = Z->zoomResizing() ? Z->zoomCorner() : Z->zoomCornerAt(g_pInputManager->getMouseCoordsInternal());
        } else if (!blocked && !DRAGGING) {
            const int MODS = modsNow();
            if (MODS == 0 || MODS == HL_MODIFIER_META)
                corner = cornerTarget(g_pInputManager->getMouseCoordsInternal(), MODS == HL_MODIFIER_META).second;
        }
        if (corner != Layout::CORNER_NONE) {
            Pointer::Cursor::overrideController->setOverride(cursorFor(corner), Pointer::Cursor::CURSOR_OVERRIDE_WINDOW_EDGE);
            g_hoverSet = true;
        } else if (g_hoverSet && !DRAGGING) {
            Pointer::Cursor::overrideController->unsetOverride(Pointer::Cursor::CURSOR_OVERRIDE_WINDOW_EDGE);
            g_hoverSet = false;
        }
    }

    void shutdown() {
        if (g_slideTimer)
            wl_event_source_remove(g_slideTimer);
        g_slideTimer = nullptr;
        // Nothing left drawn off its box, no clock left to fire into unmapped code.
        for (auto* A : g_instances)
            A->endZoomNow();
        g_superCanvas    = nullptr;
        g_super          = ESuper::IDLE;
        g_labelFades.clear();
        g_zoomPressing   = false;
        g_swallowRelease = false;
        g_edgePush       = {};
        if (g_canvasTimer) {
            wl_event_source_remove(g_canvasTimer);
            g_canvasTimer = nullptr;
        }
        if (g_ourDrag && g_layoutManager && g_layoutManager->dragController()->target())
            g_layoutManager->endDragTarget();
        g_ourDrag = false;
        if (g_hoverSet)
            Pointer::Cursor::overrideController->unsetOverride(Pointer::Cursor::CURSOR_OVERRIDE_WINDOW_EDGE);
        g_hoverSet = false;
    }

    void onWindowClose(const PHLWINDOW& w) {
        const auto APP = appOf(w);
        if (APP.empty())
            return;
        for (const auto* A : g_instances)
            if (A->holds(w)) {
                // Only a size the app or the user chose (SNatural::trusted):
                // never a tile or a size inherited from another layout.
                if ((A->arrangedAsMosaic() || A->arrangedAsCanvas()) && A->trustedOf(w))
                    if (const auto N = A->naturalOf(w))
                        saveSize(APP, *N);
                return;
            }
    }

    // FOCUS BY DIRECTION (user, 2026-10-14: "I need super+arrowkeys to be able
    // to switch focus between windows in mosaic"). Hyprland's own movefocus
    // looks for a window whose edge lines up with the focused one's across the
    // gap -- what dwindle and scrolling always have, and a mosaic's centred,
    // bunched cluster mostly does not. hl.plugin.hyprdrive.focus("l"|"r"|"u"|"d"),
    // bound to Super+arrows while Mosaic is on (the panel's fragment): on a
    // mosaic desktop, the window whose centre lies that way, nearest along the
    // direction and closest across it; anywhere else -- or with nothing that
    // way -- exactly Hyprland's movefocus, so dwindle and scrolling (and the
    // move to the next monitor at the edge) are unchanged.
    static Math::eDirection dirOf(const std::string& arg) {
        const char C = arg.empty() ? 'l' : arg[0];
        return C == 'r' ? Math::DIRECTION_RIGHT : C == 'u' ? Math::DIRECTION_UP : C == 'd' ? Math::DIRECTION_DOWN : Math::DIRECTION_LEFT;
    }

    // The window that way from `from` on its mosaic desktop: its centre lies
    // that way, nearest along the direction and closest across it. Null off a
    // mosaic desktop, or with nothing that way.
    // In the TILES the windows share edges, as in dwindle: Hyprland's own
    // movefocus/swapwindow are dwindle's, so the mosaic's centre rule stands
    // aside there (user, 2026-10-23: the tiles "should behave exactly like
    // dwindle").
    static PHLWINDOW mosaicNeighbour(const PHLWINDOW& from, Math::eDirection dir) {
        if (!from || !from->m_workspace ||
            !std::ranges::any_of(g_instances, [&](const CMosaicAlgorithm* a) { return a->holds(from) && (a->arrangedAsMosaic() || a->arrangedAsCanvas()); }))
            return nullptr;
        const Vector2D D  = dir == Math::DIRECTION_RIGHT ? Vector2D{1, 0} : dir == Math::DIRECTION_LEFT ? Vector2D{-1, 0} : dir == Math::DIRECTION_UP ? Vector2D{0, -1} : Vector2D{0, 1};
        const auto     C0 = from->middle();
        PHLWINDOW      best;
        double         bestScore = 1e18;
        for (const auto& W : Desktop::windowState()->windows()) {
            if (!W || W == from || !W->m_isMapped || W->isHidden() || W->m_workspace != from->m_workspace || !onMosaic(W))
                continue;
            const auto   V     = W->middle() - C0;
            const double ALONG = V.x * D.x + V.y * D.y;
            if (ALONG <= 1.0)
                continue;
            const double ACROSS = std::abs(V.x * D.y - V.y * D.x);
            const double SCORE  = ALONG + 2.0 * ACROSS;
            if (SCORE < bestScore) {
                bestScore = SCORE;
                best      = W;
            }
        }
        return best;
    }

    static SDispatchResult onFocusDispatcher(std::string arg) {
        const auto DIR = dirOf(arg);
        if (const auto BEST = mosaicNeighbour(Desktop::focusState()->window(), DIR)) {
            (void)::Config::Actions::focus(BEST);
            return {};
        }
        (void)::Config::Actions::moveFocus(DIR);
        return {};
    }

    // SWAP BY DIRECTION (user, 2026-10-16: "I also need windows to be switchable
    // with keyboard commands like in pure dwindle, while in mosaic").
    // hl.plugin.hyprdrive.swap("l"|"r"|"u"|"d"), bound to Super+Shift+arrows
    // with the focus binds: Hyprland's swapwindow looks for the window across a
    // shared edge, as movefocus does, and finds none in a bunched cluster. On a
    // mosaic desktop the focused window trades places with the window focus
    // would move to (Hyprland's own swap, so the layout's swapTargets: the
    // focused window is dropped where the other stood -- MosaicPack::PackHint
    // -- still one cluster with no holes);
    // anywhere else, exactly Hyprland's swapwindow.
    static SDispatchResult onSwapDispatcher(std::string arg) {
        const auto DIR     = dirOf(arg);
        const auto FOCUSED = Desktop::focusState()->window();
        if (const auto BEST = mosaicNeighbour(FOCUSED, DIR)) {
            (void)::Config::Actions::swapWith(BEST, FOCUSED);
            return {};
        }
        (void)::Config::Actions::swapInDirection(DIR);
        return {};
    }

    // A WINDOW THAT OPENS FLOATING (user, 2026-10-14: "windows that already
    // come up as floating do not get properly included into the mosaic group").
    // A float -- by a window rule, or because the app asked -- belongs to
    // Hyprland's floating layout, which the mosaic never sees, so it landed on
    // top of the cluster instead of in it. On a mosaic desktop, where every
    // window keeps its own size anyway, it joins the mosaic. Real dialogs stay
    // afloat: anything with a parent window (a transient, an X11 child), a
    // modal, an override-redirect popup, a pinned or fullscreen window. So
    // does a float a window rule tags `hyprdrive-float` (user, 2026-10-15: a
    // Sushi preview joined the mosaic and every window re-packed around it,
    // then back when it closed): a preview or a picker is a passing overlay,
    // not a member of the desktop. 25-rules.lua tags Sushi; a user tags any
    // other app the same way.
    static CHyprSignalListener g_openLateHook;
    static CHyprSignalListener g_floatingHook; // g_windowSizes: a window floated

    static bool onMosaicWorkspace(const PHLWORKSPACE& ws) {
        return ws && std::ranges::any_of(g_instances, [&](const CMosaicAlgorithm* a) { return a->workspace() == ws; });
    }

    static void adoptFloating(const PHLWINDOW& w) {
        if (!w || !w->m_isMapped || !w->m_isFloating || !onMosaicWorkspace(w->m_workspace))
            return;
        if (w->parent() || w->isModal() || w->m_pinned || Fullscreen::controller()->isFullscreen(w) || (w->m_isX11 && (w->isX11OverrideRedirect() || w->x11Parent())))
            return;
        if (w->m_ruleApplicator && w->m_ruleApplicator->m_tagKeeper.isTagged("hyprdrive-float"))
            return;
        trace(std::format("adopt floating window {:x} into the mosaic", (uintptr_t)w.get()));
        (void)::Config::Actions::floatWindow(::Config::Actions::TOGGLE_ACTION_DISABLE, w);
    }

    static CHyprSignalListener g_desktopShownHook; // a held Super's zoom follows the desktop
    // HELD SUPER CARRIES THE ZOOM ACROSS DESKTOPS (user, 2026-10-30: "when I
    // super+move mouse and canvas zooms out, then I scroll (switches desktops
    // while I hold super), any other canvas that can zoom out should also be
    // zoomed out"). The one arriving zooms out if it is a canvas wider than
    // the screen. The desktop left behind slides away AS IT IS, zoomed out,
    // and drops the zoom once it is off screen (slideWatch): zooming back in
    // while it slid made its windows lag behind the slide and vanish at its
    // end (user, 2026-11-01).
    static void zoomFollowsDesktop(const PHLWORKSPACE& ws) {
        if (g_super != ESuper::ZOOMED || g_zoomPressing || !ws || ws->m_isSpecialWorkspace)
            return;
        auto* A = canvasOf(ws);
        if (A)
            A->m_slide.zoomLeaving = false; // back before it was off screen: it stays zoomed out
        if (auto* OLD = liveZoomed(); OLD && OLD != A) {
            OLD->m_slide.zoomLeaving = true;
            armSlideWatch();
        }
        if (A && A == liveZoomed())
            return;
        g_superCanvas = nullptr;
        if (A && A->zoomOut()) {
            g_superCanvas = A;
            // As when the zoom engaged: no motion reaches the clients while zoomed.
            g_pSeatManager->setPointerFocus(nullptr, {});
        }
    }

    void registerLayout(HANDLE handle) {
        // Before the algo exists: windows re-arrive inside addTiledAlgo.
        loadKept();
        HyprlandAPI::addTiledAlgo(handle, "mosaic", &typeid(CMosaicAlgorithm), [] { return makeUnique<CMosaicAlgorithm>(); });
        ScrollOverview::Config::registerDispatcher("focus", onFocusDispatcher);
        ScrollOverview::Config::registerDispatcher("swap", onSwapDispatcher);
        // After the window's rules are in (openLate). Directly, never a
        // doLater: a deferred call still queued when the plugin unloads jumps
        // into unmapped code (the unload SEGVs, 2026-09).
        g_openLateHook = Event::bus()->m_events.window.openLate.listen([](PHLWINDOW w) { adoptFloating(w); });
        g_desktopShownHook = Event::bus()->m_events.workspace.active.listen([](PHLWORKSPACE ws) { zoomFollowsDesktop(ws); });
        g_floatingHook = Event::bus()->m_events.window.floating.listen([](PHLWINDOW w) { onFloating(w); });
    }

    void unregisterLayout(HANDLE handle) {
        g_openLateHook.reset();
        g_desktopShownHook.reset();
        g_floatingHook.reset();
        // Before the algo goes: removing it hands the desktops to dwindle.
        saveKept();
        HyprlandAPI::removeAlgo(handle, "mosaic");
    }
}
