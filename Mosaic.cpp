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
#include <linux/input-event-codes.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
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
    }

    // One window of the layout, in arrival order (the order the rows read).
    struct SMosaicEntry {
        WP<Layout::ITarget> target;
        Vector2D            natural; // the size the window keeps in the mosaic
    };

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
        std::optional<Vector2D> naturalOf(const PHLWINDOW& w) const {
            for (const auto& E : m_entries)
                if (const auto T = E.target.lock(); T && T->window() == w)
                    return E.natural;
            return std::nullopt;
        }

        void newTarget(SP<Layout::ITarget> target) override {
            m_entries.push_back({target, naturalFor(target, true)});
            recalculate();
        }

        void movedTarget(SP<Layout::ITarget> target, std::optional<Vector2D> focalPoint = std::nullopt) override {
            (void)focalPoint;
            if (indexOf(target) < 0)
                m_entries.push_back({target, naturalFor(target, false)});
            recalculate();
        }

        void removeTarget(SP<Layout::ITarget> target) override {
            std::erase_if(m_entries, [&](const SMosaicEntry& e) { return !e.target || e.target.lock() == target; });
            recalculate();
        }

        // A resize changes the size the window keeps: dragging its edge in the
        // mosaic is choosing a new natural size.
        void resizeTarget(const Vector2D& delta, SP<Layout::ITarget> target, Layout::eRectCorner corner = Layout::CORNER_NONE) override {
            const int I = indexOf(target);
            if (I < 0)
                return;
            // The window grows toward the corner being dragged: a left or top
            // corner pulled outward is a BIGGER window, not a smaller one.
            // Doubled, because a mosaic window grows evenly about its centre,
            // so the grabbed corner keeps up with the pointer. No corner (a
            // keyboard resizeactive): the delta is the size change itself.
            Vector2D grow = delta;
            if (corner != Layout::CORNER_NONE) {
                grow.x = 2.0 * (Layout::edgeLeft(corner) ? -delta.x : delta.x);
                grow.y = 2.0 * (Layout::edgeTop(corner) ? -delta.y : delta.y);
            }
            auto& n = m_entries[sc<size_t>(I)].natural;
            n       = Vector2D{std::max(MIN_SIDE, n.x + grow.x), std::max(MIN_SIDE, n.y + grow.y)};
            recalculate();
        }

        void recalculate(Layout::eRecalculateReason reason = Layout::RECALCULATE_REASON_UNKNOWN) override {
            (void)reason;
            std::erase_if(m_entries, [](const SMosaicEntry& e) { return !e.target; });
            if (m_entries.empty() || !m_parent || !m_parent->space())
                return;

            // A fullscreen window owns the desktop: everything else is the
            // fullscreen handler's to place, as in every built-in layout.
            const auto SPACE = m_parent->space();
            if (Fullscreen::controller()->hasFullscreen(SPACE->workspace(), true)) {
                m_defaultFullscreenHandler->syncTargetSizeAndPosition();
                return;
            }

            CBox AREA = SPACE->workArea();
            // THE DOCK'S BAND (dock:windows_make_room, autohide off): DockRoom
            // shortens any tile under the pill after the fact, which would squash
            // the mosaic's bottom row out of its proportions. The mosaic keeps
            // out of that band instead -- one tile gap above the pill, the gap
            // DockRoom leaves -- and centres in what is left.
            if (const auto MON = SPACE->workspace() ? SPACE->workspace()->m_monitor.lock() : PHLMONITOR{}) {
                const CBox PILL = Dock::roomPillGlobal(MON);
                if (PILL.w >= 1 && PILL.h >= 1) {
                    static auto  PBORDER = CConfigValue<Config::INTEGER>("general:border_size");
                    const double LIMIT   = PILL.y - innerGap() - std::max<int64_t>(0, *PBORDER);
                    AREA.h               = std::min(AREA.h, LIMIT - AREA.y);
                }
            }
            if (AREA.w < 1 || AREA.h < 1)
                return;

            std::vector<MosaicPack::SRect> sizes;
            sizes.reserve(m_entries.size());
            for (const auto& E : m_entries)
                sizes.push_back({0, 0, E.natural.x, E.natural.y});

            const MosaicPack::SRect AREAR{AREA.x, AREA.y, AREA.w, AREA.h};
            m_asMosaic = false;
            if (const auto PLACED = MosaicPack::mosaicScaled(sizes, AREAR, innerGap(), MIN_SCALE)) {
                m_asMosaic = true;
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

            // THE TILES: plain cells, so Hyprland insets them by gaps_in exactly as
            // it does dwindle's -- including the flush edges against gaps_out.
            const auto CELLS = MosaicPack::tiles(m_entries.size(), AREAR);
            for (size_t i = 0; i < m_entries.size() && i < CELLS.size(); ++i)
                if (const auto T = m_entries[i].target.lock())
                    T->setPositionGlobal(Layout::STargetBox{.logicalBox = CBox{CELLS[i].x, CELLS[i].y, CELLS[i].w, CELLS[i].h}, .visualBox = {}});
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

            static auto PMONITORFALLBACK = CConfigValue<Config::INTEGER>("binds:window_direction_monitor_fallback");
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
        // How far the whole mosaic may shrink before it tiles (MosaicPack::mosaicScaled).
        static constexpr double MIN_SCALE = 0.75;

        std::vector<SMosaicEntry> m_entries;
        bool                      m_asMosaic = false; // the last arrangement was the mosaic, not its tiles

        int indexOf(const SP<Layout::ITarget>& t) const {
            for (size_t i = 0; i < m_entries.size(); ++i)
                if (m_entries[i].target.lock() == t)
                    return sc<int>(i);
            return -1;
        }

        // The space between two neighbours: the same as between two tiles, each
        // of which is inset by its side's gaps_in.
        double innerGap() const {
            static auto PGAPSIN = CConfigValue<Config::IComplexConfigValue>("general:gaps_in");
            const auto* GAPS    = sc<Config::CCssGapData*>(PGAPSIN.ptr());
            return GAPS ? sc<double>(GAPS->m_left + GAPS->m_right) : 10.0;
        }

        // THE SIZE A WINDOW KEEPS, decided once, when it arrives. desiredGeometry()
        // is the window's CURRENT size, so it is only the client's own choice the
        // moment a NEW window maps; one moving in from another layout carries
        // that layout's tile, so its remembered floating size wins when it has
        // one. Neither: three fifths of the desktop.
        Vector2D naturalFor(const SP<Layout::ITarget>& t, bool fresh) const {
            CBox AREA{0, 0, 1280, 720};
            if (m_parent && m_parent->space())
                AREA = m_parent->space()->workArea();
            const Vector2D FALLBACK{std::round(AREA.w * 0.6), std::round(AREA.h * 0.6)};
            if (!t)
                return FALLBACK;

            const auto valid = [](const Vector2D& v) { return v.x >= MIN_SIDE && v.y >= MIN_SIDE; };
            // The size this app had when it last closed in a mosaic wins.
            loadSizes();
            if (const auto IT = g_sizes.find(appOf(t->window())); IT != g_sizes.end() && valid(IT->second))
                return IT->second;
            const auto FLOAT = t->lastFloatingSize();
            if (!fresh && valid(FLOAT))
                return FLOAT;
            if (const auto D = t->desiredGeometry(); D && valid(D->size))
                return D->size;
            if (valid(FLOAT))
                return FLOAT;
            return FALLBACK;
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
            static auto PBORDER = CConfigValue<Config::INTEGER>("general:border_size");
            static auto PEXTEND = CConfigValue<Config::INTEGER>("general:extend_border_grab_area");
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

    bool onButton(const IPointer::SButtonEvent& e, bool blocked) {
        const bool PRESSED = e.state == WL_POINTER_BUTTON_STATE_PRESSED;
        if (!PRESSED) {
            if (g_ourDrag && e.button == BTN_LEFT) {
                g_ourDrag = false;
                if (g_layoutManager && g_layoutManager->dragController()->target())
                    g_layoutManager->endDragTarget();
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

    void registerLayout(HANDLE handle) {
        HyprlandAPI::addTiledAlgo(handle, "mosaic", &typeid(CMosaicAlgorithm), [] { return makeUnique<CMosaicAlgorithm>(); });
    }

    void unregisterLayout(HANDLE handle) {
        HyprlandAPI::removeAlgo(handle, "mosaic");
    }
}
