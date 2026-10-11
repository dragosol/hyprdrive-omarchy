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

#include <algorithm>
#include <cmath>
#include <typeinfo>
#include <vector>

namespace ScrollOverview::Mosaic {

    // One window of the layout, in arrival order (the order the rows read).
    struct SMosaicEntry {
        WP<Layout::ITarget> target;
        Vector2D            natural; // the size the window keeps in the mosaic
    };

    class CMosaicAlgorithm : public Layout::ITiledAlgorithm {
      public:
        CMosaicAlgorithm() = default;
        ~CMosaicAlgorithm() override = default;

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
            (void)corner;
            const int I = indexOf(target);
            if (I < 0)
                return;
            auto& n   = m_entries[sc<size_t>(I)].natural;
            n         = Vector2D{std::max(MIN_SIDE, n.x + delta.x), std::max(MIN_SIDE, n.y + delta.y)};
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
            if (const auto PLACED = MosaicPack::mosaicScaled(sizes, AREAR, innerGap(), MIN_SCALE)) {
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

    void registerLayout(HANDLE handle) {
        HyprlandAPI::addTiledAlgo(handle, "mosaic", &typeid(CMosaicAlgorithm), [] { return makeUnique<CMosaicAlgorithm>(); });
    }

    void unregisterLayout(HANDLE handle) {
        HyprlandAPI::removeAlgo(handle, "mosaic");
    }
}
