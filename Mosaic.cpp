// The "mosaic" layout: see Mosaic.hpp and MosaicPack.hpp.
#include "Mosaic.hpp"
#include "MosaicPack.hpp"
#include "Dock.hpp"

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/layout/algorithm/Algorithm.hpp>
#include <hyprland/src/layout/algorithm/TiledAlgorithm.hpp>
#include <hyprland/src/layout/space/Space.hpp>
#include <hyprland/src/layout/target/Target.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/managers/fullscreen/handler/FullscreenHandler.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/state/ViewState.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/layout/supplementary/DragController.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/managers/KeybindManager.hpp>
#include <hyprland/src/pointer/cursor/CursorShapeOverrideController.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/config/shared/actions/ConfigActions.hpp>
#include <hyprland/src/event/EventBus.hpp>
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

namespace ScrollOverview::Mosaic {

    class CMosaicAlgorithm;
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
        // floats the window too, but at one size throughout, so it keeps its own.
        struct SKept {
            PHLWINDOWREF window;
            Vector2D     size;
            Vector2D     floatAtLeave;      // its floating size when it left a mosaic
            bool         floatedSince = false;
        };
        std::vector<SKept> g_windowSizes;

        SKept* keptOf(const PHLWINDOW& w) {
            for (auto& e : g_windowSizes)
                if (e.window && e.window.lock() == w)
                    return &e;
            return nullptr;
        }

        void rememberWindow(const PHLWINDOW& w, const Vector2D& size) {
            if (!w)
                return;
            std::erase_if(g_windowSizes, [](const SKept& e) { return !e.window; });
            if (auto* e = keptOf(w)) {
                e->size         = size;
                e->floatedSince = false;
                return;
            }
            g_windowSizes.push_back({.window = w, .size = size});
        }

        void leftMosaic(const SP<Layout::ITarget>& t) {
            if (auto* e = keptOf(t ? t->window() : PHLWINDOW{})) {
                e->floatAtLeave = t->lastFloatingSize();
                e->floatedSince = false;
            }
        }

        void onFloating(const PHLWINDOW& w) {
            if (w && w->m_isFloating)
                if (auto* e = keptOf(w))
                    e->floatedSince = true;
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
    };

    // TRACE, for the next odd recording: `touch /tmp/hyprdrive-mosaic.on` and
    // every resize step and arrangement decision lands in /tmp/hyprdrive-mosaic.log.
    static bool traceOn() {
        static bool                                  on      = false;
        static std::chrono::steady_clock::time_point checked = {};
        const auto                                   NOW     = std::chrono::steady_clock::now();
        if (NOW - checked > std::chrono::seconds(1)) {
            checked = NOW;
            std::error_code ec;
            on      = std::filesystem::exists("/tmp/hyprdrive-mosaic.on", ec);
        }
        return on;
    }
    static void trace(const std::string& line) {
        if (!traceOn())
            return;
        if (std::ofstream f("/tmp/hyprdrive-mosaic.log", std::ios::app); f)
            f << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count() % 100000000 << ' ' << line << '\n';
    }

    class CMosaicAlgorithm : public Layout::ITiledAlgorithm {
      public:
        CMosaicAlgorithm() {
            g_instances.push_back(this);
        }
        ~CMosaicAlgorithm() override {
            std::erase(g_instances, this);
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
        PHLWORKSPACE workspace() const {
            return m_parent && m_parent->space() ? m_parent->space()->workspace() : PHLWORKSPACE{};
        }
        std::optional<Vector2D> naturalOf(const PHLWINDOW& w) const {
            for (const auto& E : m_entries)
                if (const auto T = E.target.lock(); T && T->window() == w)
                    return E.natural;
            return std::nullopt;
        }

        void newTarget(SP<Layout::ITarget> target) override {
            m_tiledHold = false;
            arriveWithNatural(target, true);
            recalculate();
        }

        void movedTarget(SP<Layout::ITarget> target, std::optional<Vector2D> focalPoint = std::nullopt) override {
            (void)focalPoint;
            if (indexOf(target) < 0)
                arriveWithNatural(target, false);
            recalculate();
        }

        // A window this session already gave a mosaic size comes back at that
        // size, never shrunk as a newcomer: it is not new, and shrinking it
        // here would overwrite the size it is meant to keep.
        void arriveWithNatural(const SP<Layout::ITarget>& target, bool fresh) {
            const auto N = naturalFor(target, fresh);
            arrive(target, Vector2D{N.size.w, N.size.h}, !N.kept);
        }

        // A WINDOW ARRIVES (user, 2026-10-14: "opening new windows ... seems to
        // continue shrinking mosaic windows ... I said this should never
        // happen. Existing windows should not be resized"). Nobody already
        // there changes size. The NEWCOMER may come in smaller -- down to half
        // its own size, in 5% steps -- to fit beside them; if it still does not
        // fit, it keeps its size and the desktop tiles.
        void arrive(const SP<Layout::ITarget>& target, Vector2D want, bool mayShrink) {
            m_entries.push_back({target, want, ++m_nextId});
            trace(std::format("arrive {} want={}x{}{}", appOf(target ? target->window() : PHLWINDOW{}), want.x, want.y, mayShrink ? "" : " (kept)"));
            const auto AREA = area();
            if (!mayShrink || !AREA || fits())
                return;
            auto& n = m_entries.back().natural;
            for (double s = 0.95; s >= 0.5 - 1e-9; s -= 0.05) {
                n = Vector2D{std::max(MIN_SIDE, std::round(want.x * s)), std::max(MIN_SIDE, std::round(want.y * s))};
                if (fits()) {
                    trace(std::format("arrive shrunk newcomer to {}x{} ({:.2f})", n.x, n.y, s));
                    return;
                }
            }
            n = want;
            trace("arrive: no fit even at half size -> tiles");
        }

        // Does everyone fit as a mosaic at their own sizes?
        bool fits() const {
            const auto AREA = area();
            if (!AREA)
                return false;
            std::vector<MosaicPack::SRect> sizes;
            for (const auto& E : m_entries)
                sizes.push_back({0, 0, E.natural.x, E.natural.y});
            return MosaicPack::mosaic(sizes, MosaicPack::SRect{AREA->x, AREA->y, AREA->w, AREA->h}, innerGap()).has_value();
        }

        void removeTarget(SP<Layout::ITarget> target) override {
            if (m_resize) {
                setDim(false, *m_resize);
                m_resize.reset();
            }
            m_tiledHold = false;
            leftMosaic(target);
            std::erase_if(m_entries, [&](const SMosaicEntry& e) { return !e.target || e.target.lock() == target; });
            recalculate();
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
        // In the TILES the same freeze holds during a drag; letting go (or a
        // keyboard step) moves the window's dwindle split, and gives it that
        // size in the mosaic too -- so shrinking windows in the tiles is the
        // way back to the mosaic once everyone fits again.
        void resizeTarget(const Vector2D& delta, SP<Layout::ITarget> target, Layout::eRectCorner corner = Layout::CORNER_NONE) override {
            const int I = indexOf(target);
            if (I < 0)
                return;
            // The window grows toward the corner being dragged: a left or top
            // corner pulled outward is a BIGGER window, not a smaller one. No
            // corner (a keyboard resizeactive): the delta is the size change.
            Vector2D grow = delta;
            if (corner != Layout::CORNER_NONE) {
                grow.x = Layout::edgeLeft(corner) ? -delta.x : delta.x;
                grow.y = Layout::edgeTop(corner) ? -delta.y : delta.y;
            }

            if (!m_resize || m_resize->target.lock() != target)
                beginResize(target, corner);
            if (!m_resize)
                return;
            if (corner == Layout::CORNER_NONE)
                m_resize->requested = m_resize->requested + grow;
            else {
                // A DRAG follows the POINTER, measured from the corner that
                // stays put -- not a sum of deltas, which can only drift. The
                // grab's own offset from the corner is kept.
                const Vector2D P = g_pInputManager->getMouseCoordsInternal() + m_resize->grabOffset;
                const CBox&    S = m_resize->start;
                const double   AX = Layout::edgeLeft(corner) ? S.x + S.w : S.x, AY = Layout::edgeTop(corner) ? S.y + S.h : S.y;
                m_resize->requested = Vector2D{std::max(MIN_SIDE, Layout::edgeLeft(corner) ? AX - P.x : P.x - AX), std::max(MIN_SIDE, Layout::edgeTop(corner) ? AY - P.y : P.y - AY)};
            }
            stepResize();
            // A keyboard resize is one step: settle at once.
            if (corner == Layout::CORNER_NONE)
                finishResize();
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
            if (SESSION.tiled) {
                // A tile let go: its dwindle split moves to the size it was
                // dragged to, and that is its mosaic size too. If everyone now
                // fits, the mosaic comes back -- the way out of the tiles.
                if (I >= 0) {
                    m_entries[sc<size_t>(I)].natural = SESSION.shown;
                    if (const auto AREA = area())
                        m_tree.resize(m_entries[sc<size_t>(I)].id, SESSION.shown.x, SESSION.shown.y, MosaicPack::SRect{AREA->x, AREA->y, AREA->w, AREA->h});
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
                    // Past the limit: tiles, until shrinking a tile (or a window
                    // leaving or arriving) lets the mosaic fit again.
                    if (SESSION.past)
                        m_tiledHold = true;
                }
            }
            trace(std::format("finish past={} shown={}x{} requested={}x{}", SESSION.past, SESSION.shown.x, SESSION.shown.y, SESSION.requested.x, SESSION.requested.y));
            recalculate();
        }

        void recalculate(Layout::eRecalculateReason reason = Layout::RECALCULATE_REASON_UNKNOWN) override {
            (void)reason;
            std::erase_if(m_entries, [](const SMosaicEntry& e) { return !e.target; });
            // Every window's mosaic size outlives this layout (g_windowSizes).
            for (const auto& E : m_entries)
                if (const auto T = E.target.lock())
                    rememberWindow(T->window(), E.natural);
            if (m_entries.empty() || !m_parent || !m_parent->space())
                return;

            // A fullscreen window owns the desktop: everything else is the
            // fullscreen handler's to place, as in every built-in layout.
            const auto SPACE = m_parent->space();
            if (Fullscreen::controller()->hasFullscreen(SPACE->workspace(), true)) {
                m_defaultFullscreenHandler->syncTargetSizeAndPosition();
                return;
            }

            // Mid-resize, the frozen arrangement stands, whatever asked.
            if (m_resize) {
                applyResize();
                return;
            }

            const auto AREAOPT = area();
            if (!AREAOPT)
                return;
            const CBox AREA = *AREAOPT;

            std::vector<MosaicPack::SRect> sizes;
            sizes.reserve(m_entries.size());
            for (const auto& E : m_entries)
                sizes.push_back({0, 0, E.natural.x, E.natural.y});

            const MosaicPack::SRect AREAR{AREA.x, AREA.y, AREA.w, AREA.h};
            m_asMosaic = false;
            // After a resize pushed past its limit the desktop stays tiled until
            // a window arrives or leaves (m_tiledHold).
            const auto PLACED = m_tiledHold ? std::nullopt : MosaicPack::mosaic(sizes, AREAR, innerGap());
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

            // THE TILES (user, 2026-10-14): a dwindle tree (MosaicPack::
            // CDwindleTree), each split where the user left it. Plain cells, so Hyprland insets them by gaps_in exactly
            // as it does dwindle's. They never touch `natural`: the mosaic that
            // comes back has every window as it was.
            syncTree();
            const auto CELLS = m_tree.layout(AREAR);
            trace(std::format("tiles n={} hold={}", m_entries.size(), m_tiledHold));
            for (const auto& [ID, R] : CELLS)
                for (const auto& E : m_entries)
                    if (E.id == ID)
                        if (const auto T = E.target.lock())
                            T->setPositionGlobal(Layout::STargetBox{.logicalBox = CBox{R.x, R.y, R.w, R.h}, .visualBox = {}});
        }

        // The desktop the mosaic lays out in.
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

        SP<Layout::ITarget> getNextCandidate(SP<Layout::ITarget> old) override {
            if (m_entries.empty())
                return nullptr;
            const int I = indexOf(old);
            if (I < 0)
                return m_entries.back().target.lock();
            return m_entries[(sc<size_t>(I) + 1) % m_entries.size()].target.lock();
        }

        void swapTargets(SP<Layout::ITarget> a, SP<Layout::ITarget> b) override {
            const int IA = indexOf(a), IB = indexOf(b);
            if (IA >= 0 && IB >= 0)
                std::swap(m_entries[sc<size_t>(IA)], m_entries[sc<size_t>(IB)]);
            recalculate();
        }

        // Moving a window: it trades places with the nearest window in that
        // direction (the order is the arrangement). Past the edge, the monitor
        // in that direction takes it, as binds:window_direction_monitor_fallback
        // asks of every layout.
        void moveTargetInDirection(SP<Layout::ITarget> t, Math::eDirection dir, bool silent) override {
            (void)silent;
            const int I = indexOf(t);
            if (I < 0 || !t)
                return;
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
                std::swap(m_entries[sc<size_t>(I)], m_entries[sc<size_t>(best)]);
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

      private:
        static constexpr double MIN_SIDE  = 120.0;

        std::vector<SMosaicEntry> m_entries;
        bool                      m_asMosaic  = false; // the last arrangement was the mosaic, not its tiles
        bool                      m_tiledHold = false; // a resize went past its limit: tiles until the mosaic fits unshrunk
        MosaicPack::CDwindleTree  m_tree;              // the tiles; empty while the mosaic stands
        long long                 m_nextId = 0;

        // TRACE: every window's mosaic size at each arrangement decision.
        void traceEntries(const char* what) const {
            if (!traceOn())
                return;
            std::string line = std::format("arrange {} hold={} area={}x{} n={}", what, m_tiledHold, area() ? area()->w : 0.0, area() ? area()->h : 0.0, m_entries.size());
            for (const auto& E : m_entries) {
                const auto T = E.target.lock();
                const auto W = T ? T->window() : PHLWINDOW{};
                line += std::format(" [{} {:x} natural={}x{}]", appOf(W), (uintptr_t)W.get(), E.natural.x, E.natural.y);
            }
            trace(line);
        }

        // The tree follows the windows. Empty (the tiles just took over from
        // the mosaic): a balanced tree of everyone, in mosaic order. Otherwise
        // a window gone is removed (its sibling takes the space) and a window
        // new to the tiles splits the FOCUSED window's tile, as in dwindle.
        void syncTree() {
            if (m_tree.empty()) {
                std::vector<long long> ids;
                for (const auto& E : m_entries)
                    ids.push_back(E.id);
                m_tree.buildBalanced(ids);
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
                    m_tree.insert(E.id, focusedId == E.id ? -1 : focusedId);
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
            std::vector<std::pair<WP<Layout::ITarget>, CBox>> frozen; // every OTHER window, where it stands
        };
        std::optional<SResize> m_resize;

        void beginResize(const SP<Layout::ITarget>& target, Layout::eRectCorner corner) {
            if (m_resize)
                finishResize();
            SResize r;
            r.target    = target;
            r.corner    = corner;
            r.tiled     = !m_asMosaic;
            r.start     = target->position();
            r.requested = r.start.size();
            r.shown     = r.start.size();
            if (corner != Layout::CORNER_NONE) {
                const Vector2D CORNERPOS{Layout::edgeLeft(corner) ? r.start.x : r.start.x + r.start.w, Layout::edgeTop(corner) ? r.start.y : r.start.y + r.start.h};
                r.grabOffset = CORNERPOS - g_pInputManager->getMouseCoordsInternal();
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
            if (m_resize->tiled) {
                // In the tiles: the window follows the pointer, kept on the
                // desktop; the others hold still until the drag ends.
                m_resize->shown = Vector2D{std::clamp(m_resize->requested.x, MIN_SIDE, AREA->w), std::clamp(m_resize->requested.y, MIN_SIDE, AREA->h)};
                trace(std::format("step tiled requested={}x{}", m_resize->requested.x, m_resize->requested.y));
                applyResize();
                return;
            }
            const auto LIMIT = MosaicPack::limitFor(sizes, idx, m_resize->requested.x, m_resize->requested.y, AREAR, innerGap(), MIN_SIDE);
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
            for (const auto& [WT, BOX] : m_resize->frozen)
                if (const auto T = WT.lock())
                    T->setPositionGlobal(Layout::STargetBox{.logicalBox = BOX, .visualBox = BOX});
            const CBox&    S    = m_resize->start;
            const Vector2D SIZE = m_resize->shown;
            const auto     C    = m_resize->corner;
            // The corner opposite the one dragged stays put (a keyboard resize
            // grows about the centre).
            double x = C == Layout::CORNER_NONE ? S.x + (S.w - SIZE.x) / 2.0 : Layout::edgeLeft(C) ? S.x + S.w - SIZE.x : S.x;
            double y = C == Layout::CORNER_NONE ? S.y + (S.h - SIZE.y) / 2.0 : Layout::edgeTop(C) ? S.y + S.h - SIZE.y : S.y;
            x = std::clamp(x, AREA->x, std::max(AREA->x, AREA->x + AREA->w - SIZE.x));
            y = std::clamp(y, AREA->y, std::max(AREA->y, AREA->y + AREA->h - SIZE.y));
            const CBox BOX{std::round(x), std::round(y), std::round(SIZE.x), std::round(SIZE.y)};
            TARGET->setPositionGlobal(Layout::STargetBox{.logicalBox = BOX, .visualBox = BOX});
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
                if (const auto KEPT = rememberedWindow(t))
                    in.kept = SZ(*KEPT);
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

        // The corner of `w` under `p`, or CORNER_NONE.
        Layout::eRectCorner cornerAt(const PHLWINDOW& w, const Vector2D& p) {
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
            return Layout::CORNER_NONE;
        }

        const char* cursorFor(Layout::eRectCorner c) {
            switch (c) {
                case Layout::CORNER_TOPLEFT: return "nw-resize";
                case Layout::CORNER_TOPRIGHT: return "ne-resize";
                case Layout::CORNER_BOTTOMLEFT: return "sw-resize";
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
                return cornerAt(w, p);
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

    bool onButton(const IPointer::SButtonEvent& e, bool blocked) {
        const bool PRESSED = e.state == WL_POINTER_BUTTON_STATE_PRESSED;
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

    void onMove(bool blocked) {
        if (std::ranges::any_of(g_instances, [](const CMosaicAlgorithm* a) { return a->resizing(); }))
            checkResizeEnded();
        const bool DRAGGING = g_layoutManager && g_layoutManager->dragController()->target();
        Layout::eRectCorner corner = Layout::CORNER_NONE;
        if (!blocked && !DRAGGING) {
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
                if (A->arrangedAsMosaic())
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
    static SDispatchResult onFocusDispatcher(std::string arg) {
        const char C = arg.empty() ? 'l' : arg[0];
        const auto DIR = C == 'r' ? Math::DIRECTION_RIGHT : C == 'u' ? Math::DIRECTION_UP : C == 'd' ? Math::DIRECTION_DOWN : Math::DIRECTION_LEFT;
        const auto FOCUSED = Desktop::focusState()->window();
        if (FOCUSED && onMosaic(FOCUSED) && FOCUSED->m_workspace) {
            const Vector2D D  = DIR == Math::DIRECTION_RIGHT ? Vector2D{1, 0} : DIR == Math::DIRECTION_LEFT ? Vector2D{-1, 0} : DIR == Math::DIRECTION_UP ? Vector2D{0, -1} : Vector2D{0, 1};
            const auto     C0 = FOCUSED->middle();
            PHLWINDOW      best;
            double         bestScore = 1e18;
            for (const auto& W : Desktop::windowState()->windows()) {
                if (!W || W == FOCUSED || !W->m_isMapped || W->isHidden() || W->m_workspace != FOCUSED->m_workspace || !onMosaic(W))
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
            if (best) {
                (void)::Config::Actions::focus(best);
                return {};
            }
        }
        (void)::Config::Actions::moveFocus(DIR);
        return {};
    }

    // A WINDOW THAT OPENS FLOATING (user, 2026-10-14: "windows that already
    // come up as floating do not get properly included into the mosaic group").
    // A float -- by a window rule, or because the app asked -- belongs to
    // Hyprland's floating layout, which the mosaic never sees, so it landed on
    // top of the cluster instead of in it. On a mosaic desktop, where every
    // window keeps its own size anyway, it joins the mosaic. Real dialogs stay
    // afloat: anything with a parent window (a transient, an X11 child), a
    // modal, an override-redirect popup, a pinned or fullscreen window.
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
        trace(std::format("adopt floating window {:x} into the mosaic", (uintptr_t)w.get()));
        (void)::Config::Actions::floatWindow(::Config::Actions::TOGGLE_ACTION_DISABLE, w);
    }

    void registerLayout(HANDLE handle) {
        HyprlandAPI::addTiledAlgo(handle, "mosaic", &typeid(CMosaicAlgorithm), [] { return makeUnique<CMosaicAlgorithm>(); });
        ScrollOverview::Config::registerDispatcher("focus", onFocusDispatcher);
        // After the window's rules are in (openLate). Directly, never a
        // doLater: a deferred call still queued when the plugin unloads jumps
        // into unmapped code (the unload SEGVs, 2026-09).
        g_openLateHook = Event::bus()->m_events.window.openLate.listen([](PHLWINDOW w) { adoptFloating(w); });
        g_floatingHook = Event::bus()->m_events.window.floating.listen([](PHLWINDOW w) { onFloating(w); });
    }

    void unregisterLayout(HANDLE handle) {
        g_openLateHook.reset();
        g_floatingHook.reset();
        HyprlandAPI::removeAlgo(handle, "mosaic");
    }
}
