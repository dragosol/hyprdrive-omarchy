// Z19: this block must be FIRST. m_Value is private in hyprutils, and a flight
// drives the RENDERED box only -- setValueAndWarp moves the GOAL, and goal
// changes configure the client: a ~60-configure storm per flight, whose late
// acks from a slow client (Zen) land after the flight and clobber the geometry.
// The std headers are included clean first so private=public covers ONLY the
// hyprutils header whose guard we claim here; every later inclusion is a no-op.
// Mirrored from Dock.cpp verbatim -- the flight code moved here writes
// window->m_realPosition->m_Value, so this TU needs the same access in the same
// order.
#include <sstream>
#include <functional>
#include <memory>
#include <chrono>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>
#include <any>
#include <cstdio>
#define private public
#define protected public
#include <hyprutils/animation/AnimatedVariable.hpp>
#undef protected
#undef private

#include "FlightController.hpp"

#include "AppSearch.hpp"
#include "Config.hpp"
#include "Dock.hpp"
#include "FlightBlur.hpp"
#include "ConfigureHold.hpp"
#include "IOverview.hpp"
#include "Minimize.hpp"
#include "OverviewPassElement.hpp"
#include "Window.hpp"
#include "globals.hpp"

#include <optional>
#include <ranges>
#include <utility>

#define private public
#define protected public
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/view/WLSurface.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/state/WorkspaceState.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/pass/BorderPassElement.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/state/WorkspaceState.hpp>
#undef protected
#undef private

// Z16: the flight proxy carries the same card shadow the overview draws. AFTER
// the protected=public block, like Dock.cpp: this header pulls Renderer /
// Monitor / Window, and ahead of the block those classes would be defined with
// protected intact and this TU could not reach m_realPosition / m_realSize.
#include "OverviewChrome.hpp"

namespace ScrollOverview::Flight {

using ScrollOverview::Dock::SItem;
using ScrollOverview::Dock::SLayout;

namespace {

constexpr const char* KEY_MINIMIZE_MS = "plugin:hyprdrive:dock:minimize_ms";
constexpr const char* KEY_FLIGHT_LOG  = "plugin:hyprdrive:dock:flight_log";

// Z4: restore-then-erase in ONE frame left a frame with neither proxy nor real
// window on screen; the proxy holds its landing box while the real window works
// its way back through moveWindowToWorkspace + retile + damage, and one held
// frame still dipped to 7% of the window's area at the z4 gate's frame 232.
constexpr int LAND_HOLD_FRAMES = 2;
// How long past its duration a flight may wait for its own monitor to land it
// before any monitor lands it instead. Far longer than a stalled frame.
constexpr float FLIGHT_ORPHAN_GRACE_MS = 1500.F;

// The last frame a flight actually put on screen. THIS is what a reversal
// continues from, and its absence is what "Armed" means -- there is no -1 alpha
// and no empty box standing in for "nothing drawn yet" any more.
struct SDrawn {
    CBox  box;
    float alpha = 0.F;
};

// ---------------------------------------------------------------------------
// The flight log. Plain fopen because the plugin's Log::logger is dead (a
// separate inline instance under -fno-gnu-unique). scripts/zflight_gate.py
// reads this file: the z5log arm grades the DRAWN BOXES around each reversal,
// and the z5phase arm grades the TRANSITIONS below.
static void zdbg(const char* fmt, ...) {
    if (!Config::getValue<bool>(KEY_FLIGHT_LOG))
        return;
    if (FILE* F = fopen("/tmp/ovdbg.log", "a")) {
        va_list ap;
        va_start(ap, fmt);
        vfprintf(F, fmt, ap);
        va_end(ap);
        fclose(F);
    }
}

// Every flight gets an id, so a log line can be attributed when two windows of
// one app fly at once (Y4 made "minimize the whole app" the default gesture, so
// a three-window app is three simultaneous flights).
uint64_t g_nextId = 1;

}

const char* phaseName(const EFlightPhase phase) {
    switch (phase) {
        case EFlightPhase::Armed: return "Armed";
        case EFlightPhase::Flying: return "Flying";
        case EFlightPhase::LandingBridge: return "LandingBridge";
        case EFlightPhase::Done: return "Done";
    }
    return "?";
}

const char* kindName(const EFlightKind kind) {
    switch (kind) {
        case EFlightKind::MinimizeProxy: return "MinimizeProxy";
        case EFlightKind::RestoreProxy: return "RestoreProxy";
        case EFlightKind::RestoreDriven: return "RestoreDriven";
    }
    return "?";
}

namespace {

// ---------------------------------------------------------------------------
// Z9r5. A window on the overview path is HIDDEN for the length of its flight:
// the compositor must not draw the real window beside the proxy, and the
// plugin's own preview paths ignore m_hidden so the proxy is the only image on
// screen. The danger is the other half of that sentence -- a window whose
// flight is dropped without the unhide is hidden, unfocusable and gone.
//
// The lease makes that unrepresentable: the hidden flag is SET by taking one
// and CLEARED by dropping it, and a flight owns its lease by value, so every
// path that destroys a flight -- landing, reversal, abort, PLUGIN_EXIT, the
// vector being cleared -- unhides. There is no path that forgets, because there
// is no path that could.
//
// Z20 rides along: setHidden fires no compositor event, so an overview's draw
// list is never told and the window becomes a clickable, invisible tile. The
// release is the only place that unhide happens, so the refresh belongs here.
class CHiddenLease {
  public:
    CHiddenLease() = default;
    CHiddenLease(const PHLWINDOW& window, PHLMONITOR monitor) : m_window(window), m_monitor(monitor) {
        if (!window || window->isHidden())
            return;
        window->setHidden(true);
        Membership::bump("flight hide");
        m_held = true;
    }
    CHiddenLease(const CHiddenLease&)            = delete;
    CHiddenLease& operator=(const CHiddenLease&) = delete;
    CHiddenLease(CHiddenLease&& other) noexcept { *this = std::move(other); }
    CHiddenLease& operator=(CHiddenLease&& other) noexcept {
        if (this != &other) {
            release();
            m_window       = other.m_window;
            m_monitor      = other.m_monitor;
            m_held         = other.m_held;
            other.m_held   = false;
        }
        return *this;
    }
    ~CHiddenLease() {
        release();
    }

    bool held() const {
        return m_held;
    }

    // Idempotent, and the landing bridge calls it explicitly: the unhide has to
    // happen on the FIRST held frame, two frames before the flight is erased.
    void release() {
        if (!m_held)
            return;
        m_held = false;

        const auto WINDOW = m_window.lock();
        if (!WINDOW)
            return;
        if (WINDOW->isHidden()) {
            WINDOW->setHidden(false);
            Membership::bump("flight unhide");
        }

        if (const auto MONITOR = m_monitor.lock()) {
            if (const auto OVERVIEW = scrollOverviewForMonitor(MONITOR))
                OVERVIEW->refreshWindows();
            return;
        }
        // No monitor left to ask (abort during a hotplug): tell every overview,
        // since a stale draw list is what Z20 is.
        for (const auto& OVERVIEW : scrollOverviews()) {
            if (OVERVIEW)
                OVERVIEW->refreshWindows();
        }
    }

  private:
    PHLWINDOWREF  m_window;
    PHLMONITORREF m_monitor;
    bool          m_held = false;
};

// ---------------------------------------------------------------------------
struct SFlight {
    uint64_t        id = 0;
    PHLWINDOWREF    window;
    // THE MONITOR IT FLIES ON. Every box here is in its pixels, and the icon it
    // aims at is in its dock. Every monitor's dock runs renderFlights, and with
    // no owner each one advanced and drew every flight: a second copy on the
    // wrong screen at the wrong coordinates, a driven restore whose real window
    // was placed by each monitor's offset and scale in turn -- jumping between
    // two positions every frame -- and a landing hold counted twice a frame
    // (user, 2026-09-14: minimize and maximize glitch on multiple monitors).
    MONITORID       monitor = MONITOR_INVALID;
    CBox            from;  // the WINDOW end of the motion, monitor-local RENDER px
    EFlightKind     kind = EFlightKind::MinimizeProxy;
    Time::steady_tp startedAt;

    // Z5. A click mid-flight REVERSES the motion from wherever the proxy
    // visually is -- restarting from the icon read as a teleport under a spam of
    // clicks. `start`/`alphaFrom` are the seed a reversal hands the new flight;
    // absent means "the kind's own default end", which is exactly the case a
    // reversal of a never-drawn flight produces.
    std::optional<CBox>  start;
    std::optional<float> alphaFrom;
    std::optional<SDrawn> drawn;
    // The eased progress the previous frame drew at. Motion blur smears along
    // THIS flight's path from there to now -- never from `drawn`, which can be a
    // box off another path (a drop seeds `start` at the drop point, so the last
    // drawn box was the window's own; blurring that jump smeared the whole
    // window down into the icon: user, 2026-09-30).
    std::optional<float> lastE;

    EFlightPhase phase          = EFlightPhase::Armed;
    int          landHeldFrames = 0;
    CHiddenLease hidden;

    bool isRestore() const { return kind != EFlightKind::MinimizeProxy; }
    bool isDriven() const { return kind == EFlightKind::RestoreDriven; }
};

// Y4/Y5. ONE FLIGHT PER WINDOW, not one flight: "minimize the whole app" is the
// default click, and the old single-slot version would teleport two windows of
// a three-window app on an ordinary gesture.
std::vector<SFlight>  g_flights;
std::vector<uint64_t> g_flightDoLaterSeqs;

void transition(SFlight& flight, const EFlightPhase to, const char* reason) {
    const auto FROM = flight.phase;
    flight.phase    = to;
    zdbg("phase id=%llu kind=%s %s->%s reason=%s held=%d drawn=%d\n", (unsigned long long)flight.id, kindName(flight.kind), phaseName(FROM), phaseName(to), reason,
         flight.landHeldFrames, (int)flight.drawn.has_value());
}

std::vector<SFlight>::iterator find(const PHLWINDOW& window) {
    return std::ranges::find_if(g_flights, [&window](const SFlight& f) { return f.window.lock() == window; });
}

// Z13. The tile a window occupied when it was minimized WITH THE OVERVIEW OPEN.
// Grid slots are recomputed when the window rejoins the workspace, so aiming a
// restore at the scaled desktop box lands wrong and the tile then snaps into the
// grid ("teleports among the distribution"). The slot it HAD is the best
// predictor: same window set, same slot. Used once, then dropped.
std::vector<std::pair<PHLWINDOWREF, CBox>> g_minimizeTiles;

void rememberMinimizeTile(const PHLWINDOW& window, const CBox& tile) {
    std::erase_if(g_minimizeTiles, [&](const auto& e) { return !e.first.lock() || e.first.lock() == window; });
    g_minimizeTiles.emplace_back(window, tile);
}

CBox takeMinimizeTile(const PHLWINDOW& window) {
    const auto IT = std::ranges::find_if(g_minimizeTiles, [&](const auto& e) { return e.first.lock() == window; });
    if (IT == g_minimizeTiles.end())
        return {};
    const CBox TILE = IT->second;
    g_minimizeTiles.erase(IT);
    return TILE;
}

// Z4. Everything the compositor wants to ANIMATE about a just-restored window --
// geometry easing from its stash box, the MOVE_TO_WORKSPACE fade -- must be
// warped to its goal: the flight already was the animation, and each of these
// measurably painted its own artifact frame (a transient small box; a full-size
// window at 7% alpha).
void warpRestoredWindowAnims(const PHLWINDOW& window) {
    if (!window)
        return;
    window->m_realPosition->warp();
    window->m_realSize->warp();
    for (uint8_t i = 0; i < Desktop::View::WINDOW_ALPHA_LAST; ++i)
        window->alpha()[i]->warp();
}

// Y5. GNOME's MINIMIZE_WINDOW_ANIMATION_MODE is Clutter's EASE_OUT_EXPO, which
// is 1 - 2^(-10t) snapped to exactly 1 at the end. Extracted from the host's
// windowManager.js, not recalled -- see spec 13.14's reference-discipline note.
float easeOutExpo(float t) {
    return t >= 1.F ? 1.F : 1.F - std::pow(2.F, -10.F * t);
}

float flightProgress(const SFlight& flight) {
    const int DURATION = Config::getValue<int>(KEY_MINIMIZE_MS);
    if (DURATION <= 0)
        return 1.F;

    return std::clamp(ScrollOverview::Config::animElapsedMs(flight.startedAt) / sc<float>(DURATION), 0.F, 1.F);
}

CBox lerpBox(const CBox& a, const CBox& b, float t) {
    return CBox{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.w + (b.w - a.w) * t, a.h + (b.h - a.h) * t};
}

// The window as it sits on screen, in the monitor-local RENDER px the flight
// works in.
CBox windowRenderBox(PHLMONITOR monitor, const PHLWINDOW& window) {
    if (!monitor || !window)
        return {};

    const auto POS   = window->m_realPosition->value();
    const auto SIZE  = window->m_realSize->value();
    const auto SCALE = std::max(monitor->m_scale, 0.01F);

    return CBox{(POS.x - monitor->m_position.x) * SCALE, (POS.y - monitor->m_position.y) * SCALE, SIZE.x * SCALE, SIZE.y * SCALE};
}

// Y10. The window END of a flight, which is NOT always the window's desktop
// geometry. With the overview open the user is looking at the TILE, so that is
// what the animation has to leave from and arrive at -- reading the real box
// there made a restore grow out of the icon into a rectangle nothing was drawn
// in. A window whose desktop is scrolled off screen has no tile at all, so the
// flight aims past the edge on the side that desktop lies and the motion still
// reads as "it went that way" rather than collapsing onto a point.
CBox offscreenAimBox(PHLMONITOR monitor, const PHLWINDOW& window) {
    const float SCALE = std::max(monitor->m_scale, 0.01F);
    const float W     = sc<float>(monitor->m_size.x) * SCALE;
    const float H     = sc<float>(monitor->m_size.y) * SCALE;

    // A minimized window sits on the stash, so its own m_workspace cannot say
    // where it came from -- Minimize recorded that and is the only honest source.
    WORKSPACEID home = Minimize::originalWorkspace(window);
    if (home == WORKSPACE_INVALID && window->m_workspace)
        home = window->m_workspace->m_id;

    const auto ACTIVE = monitor->m_activeWorkspace;
    const bool LEFT   = ACTIVE && home != WORKSPACE_INVALID && home < ACTIVE->m_id;

    const float BW = W * 0.25F;
    const float BH = H * 0.25F;

    return CBox{LEFT ? -BW : W, (H - BH) / 2.F, BW, BH};
}

CBox flightAnchorBox(PHLMONITOR monitor, const PHLWINDOW& window) {
    if (!monitor || !window)
        return {};

    if (const auto OVERVIEW = scrollOverviewForMonitor(monitor)) {
        if (CBox tile = OVERVIEW->tileBox(window); tile.width >= 1 && tile.height >= 1)
            return tile;

        // Z8 (user video): a MINIMIZED window has no tile unless Y9's filter is
        // on, so restores fell through to the offscreen aim -- whose home==active
        // case is not LEFT, i.e. it aimed at the NEXT desktop for a window
        // belonging to the very card the user is looking at. Aim at the recorded
        // pre-minimize box transformed into its home card.
        const auto HOMEWS = Minimize::originalWorkspace(window);
        const CBox HOME   = Minimize::originalBox(window);
        if (HOMEWS != WORKSPACE_INVALID && HOME.width >= 1) {
            if (CBox b = OVERVIEW->overviewBoxForGlobal(HOMEWS, HOME); b.width >= 1 && b.height >= 1)
                return b;
        }

        return offscreenAimBox(monitor, window);
    }

    return windowRenderBox(monitor, window);
}

// Z18. "An overview object exists" is NOT "the overview is open": objects
// persist while closing (that is what reopen() is for), and a stuck close leaves
// a zombie registered.
SP<IOverview> overviewOpenFor(PHLMONITOR monitor) {
    return openScrollOverviewForMonitor(monitor);
}

// ---------------------------------------------------------------------------
// THE LANDING, and there is only one of it.
//
// Z17/Z18: ANY restoring flight that ends -- landing, lost icon, mode
// confusion, a reversal that arrived too late, a monitor being unplugged, a
// config reload, the plugin being unloaded -- leaves the window SANE: unhidden,
// full alpha, geometry at the target. A driven flight owns its geometry (warp to
// the frozen target); a proxy flight's goals were never touched (warp value to
// goal).
//
// The check at the end is the invariant spelled out: after this runs, the
// window's animated value IS its goal, and for a floating window the layout
// target agrees with both. It is REPORTED, not aborted on: a failed invariant
// in a compositor plugin means one window is wrong, and taking the session down
// with an assert would mean every window is. The gates read the line.
void landingSanity(SFlight& flight, PHLMONITOR monitor, const char* reason) {
    const auto WINDOW = flight.window.lock();
    if (!WINDOW) {
        zdbg("land id=%llu reason=%s window=gone\n", (unsigned long long)flight.id, reason);
        return;
    }

    // The lease is the unhide, and it is also Z20's draw-list refresh.
    flight.hidden.release();
    // A driven flight drew the window "animating in" (renderFlights).
    if (flight.isDriven())
        WINDOW->m_animatingIn = false;

    if (!flight.isRestore()) {
        // A MINIMIZE landed. The window kept focus for the whole flight so the
        // flying proxy stayed bright (Zen no longer darkens mid-flight, and a
        // spam of clicks no longer flickers it); hand the keyboard off now
        // (deferred X33).
        Minimize::handoffMinimizedFocus(WINDOW);
        zdbg("land id=%llu kind=%s reason=%s minimize handoff\n", (unsigned long long)flight.id, kindName(flight.kind), reason);
        return;
    }

    const auto MON = monitor ? monitor : WINDOW->m_monitor.lock();

    if (flight.isDriven() && MON) {
        const float SCALE = std::max(MON->m_scale, 0.01F);
        const Vector2D POS{flight.from.x / SCALE + MON->m_position.x, flight.from.y / SCALE + MON->m_position.y};
        const Vector2D SZ{std::max(4.0, flight.from.width / SCALE), std::max(4.0, flight.from.height / SCALE)};
        WINDOW->m_realPosition->m_Value = POS;
        WINDOW->m_realSize->m_Value     = SZ;
        // Z18b: the LAYOUT TARGET too. Minimize captures TARGET->position(), not
        // the anim vars -- landing without this write-back left the target at its
        // pre-flight record, and the SECOND cycle restored to stale geometry
        // (900x600 on screen, 500x495 captured).
        if (WINDOW->m_isFloating) {
            if (const auto TARGET = WINDOW->layoutTarget(); TARGET) {
                TARGET->setPositionGlobal(CBox{POS.x, POS.y, SZ.x, SZ.y});
                TARGET->warpPositionSize();
            }
        }
    } else
        warpRestoredWindowAnims(WINDOW);

    // A TILED window lands where the layout wants it NOW (user, 2026-09-25:
    // spam-minimizing two tiled windows left one drawn full-width while
    // Hyprland reported it half-width). Its flight froze the layout's box at
    // click time -- full width while its sibling was still stashed -- and the
    // write above put that into the VALUE while the sibling's return had moved
    // the GOAL to half: value != goal with no animation left to close the gap,
    // so it stayed. Warp to the goal and make the client match.
    if (!WINDOW->m_isFloating) {
        WINDOW->m_realPosition->setValueAndWarp(WINDOW->m_realPosition->goal());
        WINDOW->m_realSize->setValueAndWarp(WINDOW->m_realSize->goal());
        WINDOW->sendWindowSize(true);
    }

    WINDOW->alpha()[Desktop::View::WINDOW_ALPHA_FADE]->setValueAndWarp(1.F);

    // THE INVARIANT. value == goal for position and size, on a floating window
    // the layout target as well, and the window is not hidden. Checked for both
    // float and tiled: a tiled window's target belongs to the layout, so what is
    // assertable there is that the flight left no animation mid-flight.
    const auto  PV      = WINDOW->m_realPosition->value();
    const auto  PG      = WINDOW->m_realPosition->goal();
    const auto  SV      = WINDOW->m_realSize->value();
    const auto  SG      = WINDOW->m_realSize->goal();
    const float EPS     = 1.F;
    bool        ok      = !WINDOW->isHidden() && std::abs(PV.x - PG.x) <= EPS && std::abs(PV.y - PG.y) <= EPS && std::abs(SV.x - SG.x) <= EPS && std::abs(SV.y - SG.y) <= EPS;
    float       targetDx = 0.F, targetDy = 0.F;
    if (WINDOW->m_isFloating) {
        if (const auto TARGET = WINDOW->layoutTarget(); TARGET) {
            const auto TB = TARGET->position();
            targetDx      = sc<float>(std::abs(TB.x - PV.x));
            targetDy      = sc<float>(std::abs(TB.y - PV.y));
            ok            = ok && targetDx <= EPS && targetDy <= EPS;
        }
    }

    zdbg("land id=%llu kind=%s reason=%s float=%d hidden=%d ok=%d value=%.1f,%.1f goal=%.1f,%.1f size=%.1f,%.1f/%.1f,%.1f target=%.1f,%.1f\n", (unsigned long long)flight.id,
         kindName(flight.kind), reason, (int)WINDOW->m_isFloating, (int)WINDOW->isHidden(), (int)ok, PV.x, PV.y, PG.x, PG.y, SV.x, SV.y, SG.x, SG.y, targetDx, targetDy);
}

// Land it NOW and take it out of the list. Used by a reversal that arrives
// after the flight has begun landing (there is nothing left to reverse, and
// dropping it silently is how a landing's geometry write-back went missing) and
// by abortAll.
void completeAndErase(std::vector<SFlight>::iterator it, PHLMONITOR monitor, const char* reason) {
    transition(*it, EFlightPhase::Done, reason);
    landingSanity(*it, monitor, reason);
    g_flights.erase(it);
}

// Z9 round five. The click-time restore treatment: the window comes off the
// stash FOR REAL -- real focus from focusState, which is why nothing steals the
// keyboard back when other windows exist (round four's seat-only focus was a
// squatter; focusState re-asserted the OTHER window and the client went dark
// again). The overview path then hides it for the flight, under a lease.
CBox restoreForFlight(PHLMONITOR monitor, const PHLWINDOW& window, const CBox& iconBox, CHiddenLease& lease) {
    Minimize::restore(window);
    warpRestoredWindowAnims(window);
    window->updateDecorationValues();
    if (window->m_borderFadeAnimationProgress)
        window->m_borderFadeAnimationProgress->warp();
    if (window->m_dimPercent)
        window->m_dimPercent->warp();

    if (const auto OVERVIEW = overviewOpenFor(monitor)) {
        // Overview path: proxy machinery, window hidden for the flight (the tile
        // and the proxy share a render pipeline, so that seam is clean).
        lease = CHiddenLease{window, monitor};
        if (g_pEventLoopManager) {
            const uint64_t seq = g_pEventLoopManager->doLater([W = PHLWINDOWREF{window}] {
                if (const auto WIN = W.lock())
                    warpRestoredWindowAnims(WIN);
            });
            g_flightDoLaterSeqs.push_back(seq);
        }
        return {};
    }

    // Capture the layout's target BEFORE parking -- setValueAndWarp moves the
    // GOAL too (hyprutils keeps m_Value private, so there is no value-only
    // write), and reading goal() after parking chased the parked box itself
    // (measured: 121 drive frames pinned at the icon). The flight freezes this
    // target in SFlight.from; a retile mid-flight lands via the closing warp.
    const auto  GOALPOS = window->m_realPosition->goal();
    const auto  GOALSZ  = window->m_realSize->goal();
    const float SCALE   = std::max(monitor->m_scale, 0.01F);
    const CBox  TARGET{(GOALPOS.x - monitor->m_position.x) * SCALE, (GOALPOS.y - monitor->m_position.y) * SCALE, GOALSZ.x * SCALE, GOALSZ.y * SCALE};

    // Z17 (user: "are we not using the real windows, like the rest of the
    // overview"). Desktop path: NO proxy, no hidden flag. The real window is the
    // animation -- parked at the icon at alpha 0, then its animated variables are
    // driven along the flight curve each frame while the compositor renders it
    // natively: real shadow, real border, real blur, focused. A proxy hand-off
    // cannot be seamless because the two renderings differ (measured: a
    // 3.2M-luminance seam frame at landing); a window that was never swapped has
    // no seam.
    if (iconBox.width >= 1) {
        window->m_realPosition->m_Value = Vector2D{iconBox.x / SCALE + monitor->m_position.x, iconBox.y / SCALE + monitor->m_position.y};
        window->m_realSize->m_Value     = Vector2D{std::max(4.0, iconBox.width / SCALE), std::max(4.0, iconBox.height / SCALE)};
    }
    window->alpha()[Desktop::View::WINDOW_ALPHA_FADE]->setValueAndWarp(0.F);
    return TARGET;
}

// What a reversal did, because "false" used to mean two different things and one
// of them was a strand.
enum class EReverse {
    NoFlight,   // nothing to reverse; the caller starts a fresh flight
    Swapped,    // direction flipped in place, continuous with what was drawn
    Completed,  // it was already landing: landed NOW, caller starts fresh
};

// Z5. If this window is mid-flight, flip the flight's direction in place: the
// new motion starts at the proxy's last drawn box and alpha, so a spam of clicks
// reads as one object changing its mind, which is what GNOME does.
//
// It NEVER bails silently. Two bails lived here. The first -- "not drawn a frame
// yet" -- was killed in 5623364: the caller went on to start a fresh flight
// without this one being erased, so two flights for one window fought in the
// render loop and the toggle ended in the wrong state, which is exactly what
// three fast clicks on a slow output produce. The second -- "already landing" --
// is killed here: the caller erased the landing flight anyway when it armed its
// own, so the landing's geometry write-back (Z18b) never ran and the NEXT cycle
// restored to stale geometry. A flight that is landing is now LANDED, properly,
// before the fresh one is armed.
EReverse reverse(PHLMONITOR monitor, const PHLWINDOW& window, bool restoring) {
    // Whatever happens next, the old flight's smear is over.
    FlightBlur::reset(window);
    const auto IT = find(window);
    if (IT == g_flights.end()) {
        zdbg("reverse(%d) found=0\n", (int)restoring);
        return EReverse::NoFlight;
    }

    zdbg("reverse(%d) found=1 id=%llu phase=%s drawn=%d a=%f\n", (int)restoring, (unsigned long long)IT->id, phaseName(IT->phase), (int)IT->drawn.has_value(),
         IT->drawn ? IT->drawn->alpha : -1.F);

    if (IT->phase == EFlightPhase::LandingBridge || IT->phase == EFlightPhase::Done) {
        completeAndErase(IT, monitor, "reversed-while-landing");
        return EReverse::Completed;
    }

    SFlight old = std::move(*IT);
    g_flights.erase(IT);

    // Z9r5: state flips WITH the direction. A restoring flight's window is
    // already real -- sending it back means stash (plus the unhide the lease
    // owns). The minimize->restore direction is handled by the caller via
    // restoreForFlight, which needs the monitor.
    if (!restoring && !Minimize::isMinimized(window)) { // reversing back to a minimize
        // Z17: a DRIVEN flight moved goal AND value to the mid-air box; stashing
        // now would record that as the window's geometry and a later restore
        // would bring it back shrunken. Re-goal to the flight's true target
        // (old.from, render px) before the stash captures it.
        //
        // The layout's goal NOW, not old.from: old.from was frozen when this
        // window came back, and in a whole-app restore the first window back is
        // alone in the layout -- its frozen target was the whole monitor. Pinned
        // there on the stash, its client was told full screen on every fast
        // re-click (user, 2026-09-30: prompts walking down the terminal). By the
        // next click all its siblings are back, so the goal is its real tile.
        if (!window->isHidden() && window->m_realSize->goal().x >= 1) {
            window->m_realPosition->m_Value = window->m_realPosition->goal();
            window->m_realSize->m_Value     = window->m_realSize->goal();
        } else if (const auto MON = window->m_monitor.lock(); MON && old.from.width >= 1 && !window->isHidden()) {
            const float SCALE = std::max(MON->m_scale, 0.01F);
            window->m_realPosition->m_Value = Vector2D{old.from.x / SCALE + MON->m_position.x, old.from.y / SCALE + MON->m_position.y};
            window->m_realSize->m_Value     = Vector2D{std::max(4.0, old.from.width / SCALE), std::max(4.0, old.from.height / SCALE)};
        }
        window->alpha()[Desktop::View::WINDOW_ALPHA_FADE]->setValueAndWarp(1.F);
        if (old.isDriven())
            window->m_animatingIn = false;
        old.hidden.release();
        Minimize::minimize(window, false); // keep focus: the flight hands off at landing
        // Z14, for the reversal too (user, 2026-09-30: spam-minimizing left the
        // window drawn at full size behind every other flight). The stash move
        // arms Hyprland's own fades and geometry animations on the REAL window,
        // which then played in place while the proxy flew; the fresh minimize
        // path warps them, this one never did.
        warpRestoredWindowAnims(window);
        window->m_realPosition->m_bIsBeingAnimated = false;
        window->m_realSize->m_bIsBeingAnimated     = false;
    }

    // A reversed flight is always a proxy (never driven): the desktop-driven
    // restore only starts from restore(), not from a reversal.
    SFlight next{.id        = g_nextId++,
                 .window    = window,
                 .monitor   = old.monitor,
                 .from      = old.from,
                 .kind      = restoring ? EFlightKind::RestoreProxy : EFlightKind::MinimizeProxy,
                 .startedAt = Time::steadyNow()};
    // The seed. An Armed flight has drawn nothing, so there is nothing to be
    // continuous with and the new flight starts from its own default end -- what
    // the render loop always did when `start` was an empty box, now said out
    // loud instead of encoded in a width of 0.
    if (old.drawn) {
        next.start     = old.drawn->box;
        next.alphaFrom = old.drawn->alpha;
    }
    next.hidden = std::move(old.hidden);
    zdbg("phase id=%llu kind=%s ->Armed reason=reversal-of-%llu seeded=%d\n", (unsigned long long)next.id, kindName(next.kind), (unsigned long long)old.id,
         (int)next.start.has_value());
    g_flights.emplace_back(std::move(next));
    return EReverse::Swapped;
}

}

// ---------------------------------------------------------------------------
bool anyOn(PHLMONITOR monitor) {
    return monitor && std::ranges::any_of(g_flights, [&](const SFlight& f) { return f.monitor == monitor->m_id; });
}

bool any() {
    return !g_flights.empty();
}

bool hasFlight(const PHLWINDOW& window) {
    return find(window) != g_flights.end();
}

bool isRestoreFlight(const PHLWINDOW& window) {
    const auto IT = find(window);
    return IT != g_flights.end() && IT->isRestore();
}

void minimize(PHLMONITOR monitor, PHLWINDOW window, bool bounceOnLand, std::optional<CBox> startBox) {
    ConfigureHold::CHold oneResize;
    // Z5/Z9r5. Mid-restore click: put it back from mid-air. The window is REAL
    // (hidden) during a restore flight now, so the gate is the flight's
    // existence, and reverse() performs the unhide + stash.
    if (window && reverse(monitor, window, false) == EReverse::Swapped)
        return;

    // The window's own monitor, whichever dock or keybind asked: the flight
    // leaves from where the window IS and lands in that monitor's dock.
    if (window && !Minimize::isMinimized(window)) {
        if (const auto WINDOWMON = window->m_monitor.lock(); WINDOWMON && WINDOWMON->m_enabled)
            monitor = WINDOWMON;
    }

    const int  DURATION = Config::getValue<int>(KEY_MINIMIZE_MS);
    const bool ANIMATE  = monitor && window && DURATION > 0 && !Minimize::isMinimized(window);

    // Must be read before the stash, which moves the window off its workspace and
    // so destroys both the tile and the real box.
    CBox from;
    if (ANIMATE)
        from = flightAnchorBox(monitor, window);

    // Keep focus only when a flight will actually play (ANIMATE); the flight
    // hands off at its landing. With no flight there is nothing to stay bright
    // for, so hand off immediately like any other minimize.
    Minimize::minimize(window, !ANIMATE);
    // Z14 (user): "the window shadows do not follow it down" -- the stash move
    // arms MOVE_FROM_WORKSPACE / fade animations that draw the real window
    // (shadow included) fading at its OLD rect while the proxy departs. The
    // flight is the animation; warp them all.
    if (ANIMATE)
        warpRestoredWindowAnims(window);
    zdbg("minimizeImpl animate=%d from=%f,%f %fx%f\n", (int)ANIMATE, from.x, from.y, from.width, from.height);

    ScrollOverview::Dock::dismissHoverPanelNow(); // Z15
    if (ANIMATE && from.width >= 1 && from.height >= 1) {
        if (overviewOpenFor(monitor))
            rememberMinimizeTile(window, from); // Z13: `from` IS the tile here
        ScrollOverview::Dock::revealNow(monitor);
        // Z10 (user, overriding Y8's "macOS bounces what arrives"): the hop is
        // the ACKNOWLEDGEMENT of the drop, so it fires now, while the window is
        // still flying in -- not at the landing.
        if (bounceOnLand)
            ScrollOverview::Dock::armBounce(AppSearch::identityClass(window->m_class, window->m_title));
        std::erase_if(g_flights, [&window](const SFlight& f) { return f.window.lock() == window; });
        SFlight next{.id = g_nextId++, .window = window, .monitor = monitor->m_id, .from = from, .kind = EFlightKind::MinimizeProxy, .startedAt = Time::steadyNow()};
        if (startBox && startBox->width >= 1 && startBox->height >= 1)
            next.start = *startBox;
        zdbg("phase id=%llu kind=%s ->Armed reason=minimize\n", (unsigned long long)next.id, kindName(next.kind));
        g_flights.emplace_back(std::move(next));
    }
}

// X54. The mirror of the minimize, and it has to keep the window STASHED for the
// length of the flight: restoring first would put the real window on screen
// instantly and the proxy would be a second copy of it. The actual restore
// happens when the flight lands -- see renderFlights.
void restore(PHLMONITOR monitor, PHLWINDOW window) {
    ConfigureHold::CHold oneResize;
    // Z5. Mid-minimize click: bring it straight back from mid-air -- and give it
    // the same click-time restore treatment a fresh flight gets (Z9r5).
    if (window && reverse(monitor, window, true) == EReverse::Swapped) {
        if (monitor && Minimize::isMinimized(window)) {
            const auto IT = find(window);
            if (IT != g_flights.end()) {
                const CBox TARGET = restoreForFlight(monitor, window, CBox{}, IT->hidden);
                // restoreForFlight may have left the real window on screen
                // (desktop path): upgrade the reversed proxy to a driven flight
                // to match.
                IT->kind = window->isHidden() ? EFlightKind::RestoreProxy : EFlightKind::RestoreDriven;
                if (TARGET.width >= 1)
                    IT->from = TARGET;
            }
        }
        return;
    }

    // The monitor it goes BACK to -- its home desktop's -- whichever dock or
    // keybind asked. Its minimized copy sits on the stash, which lives on one
    // monitor only and says nothing about where the window belongs.
    if (window && Minimize::isMinimized(window)) {
        const auto HOME = Minimize::originalWorkspace(window);
        for (const auto& w : State::workspaceState()->workspaces()) {
            const auto WS = w.lock();
            if (!WS || WS->m_id != HOME || WS->m_isSpecialWorkspace)
                continue;
            if (const auto HOMEMON = WS->m_monitor.lock(); HOMEMON && HOMEMON->m_enabled)
                monitor = HOMEMON;
            break;
        }
    }

    const int DURATION = Config::getValue<int>(KEY_MINIMIZE_MS);
    zdbg("restoreImpl dur=%d mon=%d min=%d\n", DURATION, (int)(bool)monitor, window ? (int)Minimize::isMinimized(window) : -9);

    if (!monitor || !window || DURATION <= 0 || !Minimize::isMinimized(window)) {
        Minimize::restore(window);
        return;
    }

    // Z6 (user, 2026-08-07): a restore to ANOTHER desktop used to aim the flight
    // off the screen edge and let Minimize::restore snap the workspace at landing
    // -- "maximizing in the wrong place". GNOME switches first, so switch NOW and
    // fly to the real box while the workspace slide plays underneath; both are
    // monitor-anchored, so they compose. The overview path keeps its tile aim --
    // there the user is looking at the cards, and the strip is the thing that
    // says which desktop a tile is on.
    if (!overviewOpenFor(monitor)) {
        const auto HOME = Minimize::originalWorkspace(window);
        if (HOME != WORKSPACE_INVALID && monitor->m_activeWorkspace && monitor->m_activeWorkspace->m_id != HOME)
            Minimize::showOriginalWorkspace(window);
    }

    // Y10/Z13. With the overview up, aim at the tile the window occupied when it
    // was minimized (recorded) -- overviewBoxForGlobal was the DESKTOP box scaled
    // into the card, which is not where the grid puts a tile, so the landing
    // snapped. On the DESKTOP path, flightAnchorBox would read the stash
    // workspace's own retile -- aim at the recorded pre-minimize box instead
    // ("slight bug on first maximize", user video 2026-08-07 11:02). The landing
    // hold covers whatever the layout re-decides.
    CBox TO{};
    if (overviewOpenFor(monitor))
        TO = takeMinimizeTile(window);
    if (TO.width < 1 || TO.height < 1)
        TO = flightAnchorBox(monitor, window);
    if (!scrollOverviewForMonitor(monitor)) {
        const CBox HOME = Minimize::originalBox(window); // global logical
        if (HOME.width >= 1 && HOME.height >= 1) {
            const float SCALE = std::max(monitor->m_scale, 0.01F);
            TO = CBox{(HOME.x - monitor->m_position.x) * SCALE, (HOME.y - monitor->m_position.y) * SCALE, HOME.width * SCALE, HOME.height * SCALE};
        }
    }
    if (TO.width < 1 || TO.height < 1) {
        Minimize::restore(window);
        return;
    }

    ScrollOverview::Dock::dismissHoverPanelNow(); // Z15

    CHiddenLease lease;
    {
        const float SCALE = std::max(monitor->m_scale, 0.01F);
        const float MW    = sc<float>(monitor->m_size.x) * SCALE;
        const float MH    = sc<float>(monitor->m_size.y) * SCALE;
        // parked box: the dock band's centre. It is on screen for 0-1 frames at
        // alpha 0 before the first driven frame -- placement is cosmetic.
        const CBox TARGET = restoreForFlight(monitor, window, CBox{MW / 2.F - 27.F, MH - 80.F, 54.F, 54.F}, lease);
        if (TARGET.width >= 1)
            TO = TARGET;
    }

    ScrollOverview::Dock::revealNow(monitor);
    // Desktop path (window not hidden) drives the real window natively (Z17);
    // overview path (hidden) draws a proxy. The kind is frozen here for the whole
    // flight -- Z18: re-deriving it per frame flipped it mid-flight.
    std::erase_if(g_flights, [&window](const SFlight& f) { return f.window.lock() == window; });
    SFlight next{.id        = g_nextId++,
                 .window    = window,
                 .monitor   = monitor->m_id,
                 .from      = TO,
                 .kind      = window->isHidden() ? EFlightKind::RestoreProxy : EFlightKind::RestoreDriven,
                 .startedAt = Time::steadyNow()};
    next.hidden = std::move(lease);
    zdbg("phase id=%llu kind=%s ->Armed reason=restore\n", (unsigned long long)next.id, kindName(next.kind));
    g_flights.emplace_back(std::move(next));
}

// ---------------------------------------------------------------------------
// X46. A minimize flies into the app's OWN ICON. It used to fly into the preview
// slot the minimize itself created, which is the entry X46 deletes; the icon is
// where the window goes and where a click gets it back, so it is the honest
// target. Ends when the clock runs out, or immediately if the icon went away
// (app closed mid-flight).
//
// Y5. GNOME's minimize at GNOME's numbers -- 400ms on EASE_OUT_EXPO, with the
// window FADING as it scales, because `_minimizeWindow` eases opacity to 0
// inside the same tween as scale and position.
//
// THE DRAWING IS INLINE, one flight at a time, exactly as it was when this loop
// lived in Dock::render. See the header: the version of this that mutated every
// flight and then drew every flight is the version that broke z5.
void renderFlights(PHLMONITOR monitor, const SLayout& layout, const Time::steady_tp& now, const bool ownsPass) {
    // Several windows of one app land in the same frame: retile once for all.
    ConfigureHold::CHold oneResize;
    for (auto it = g_flights.begin(); it != g_flights.end();) {
        // Another monitor's flight: that monitor advances and draws it. Two
        // ways it could never get there, both settled from here with the same
        // landing guarantees rather than left holding a hidden window forever:
        // its monitor is gone, or the flight has been over for a good while and
        // nothing on its monitor rendered it home.
        if (!monitor || it->monitor != monitor->m_id) {
            const auto OWNER = State::monitorState()->query().id(it->monitor).run();
            const int  DURATION = Config::getValue<int>(KEY_MINIMIZE_MS);
            if (!OWNER || ScrollOverview::Config::realElapsedMs(it->startedAt) > sc<float>(std::max(0, DURATION)) + FLIGHT_ORPHAN_GRACE_MS) {
                completeAndErase(it, nullptr, OWNER ? "orphaned" : "monitor-gone");
                continue;
            }
            ++it;
            continue;
        }

        const auto  WINDOW = it->window.lock();
        const float T      = flightProgress(*it);
        const auto  CLASS  = WINDOW ? AppSearch::identityClass(WINDOW->m_class, WINDOW->m_title) : std::string{};
        const auto  ITEM   = WINDOW ? std::ranges::find_if(layout.items, [&](const SItem& item) { return item.wmClass == CLASS; }) : layout.items.end();

        if (!WINDOW || T >= 1.F || ITEM == layout.items.end()) {
            zdbg("flightEnd id=%llu win=%d T=%f item=%d restoring=%d held=%d phase=%s\n", (unsigned long long)it->id, (int)(bool)WINDOW, T, ITEM != layout.items.end(),
                 (int)it->isRestore(), it->landHeldFrames, phaseName(it->phase));

            // Z4. Restore-then-erase in ONE frame left a frame with neither proxy
            // nor real window on screen -- the restore lands the window in the
            // NEXT frame's window pass at the earliest, and the user read the
            // hole as "it's an image, not the window". So: restore, hold the
            // proxy at its landing box this frame, erase on the next pass, when
            // the real window overlaps it exactly (same box, same content -- the
            // seam is invisible).
            if (WINDOW && it->isRestore() && it->hidden.held() && it->landHeldFrames < LAND_HOLD_FRAMES) {
                if (it->phase != EFlightPhase::LandingBridge) { // the first landing frame does the unhide
                    transition(*it, EFlightPhase::LandingBridge, "reached-target");
                    // The lease is the unhide AND Z20's draw-list refresh: without
                    // it the overview's list never re-adds the window and it
                    // becomes a clickable, invisible tile.
                    it->hidden.release();
                    warpRestoredWindowAnims(WINDOW);
                    WINDOW->alpha()[Desktop::View::WINDOW_ALPHA_FADE]->setValueAndWarp(1.F);
                }
                ++it->landHeldFrames;
                ScrollOverview::Dock::drawFlightSurface(monitor, WINDOW, it->from, 1.F, now, ownsPass);
                ++it;
                continue;
            }

            transition(*it, EFlightPhase::Done, WINDOW ? (T >= 1.F ? "reached-target" : "icon-gone") : "window-gone");
            landingSanity(*it, monitor, "landed");
            it = g_flights.erase(it);
            continue;
        }

        const float E = easeOutExpo(T);
        CBox        box;
        float       alpha;

        if (it->isDriven() && !Minimize::isMinimized(WINDOW)) {
            // Z17: the DESKTOP restore is the real window, driven natively -- no
            // proxy draw at all. The target is the layout's live goal, so a
            // retile mid-flight retargets the flight for free.
            const float SCALE = std::max(monitor->m_scale, 0.01F);
            const CBox  START = it->start.value_or(ITEM->iconBox);
            box               = lerpBox(START, it->from, E);
            alpha             = it->alphaFrom.value_or(0.F) * (1.F - E) + E;
            // Half as long (FlightBlur::fadeMotion), by the flight's own clock.
            const CBox PREVBOX = FlightBlur::fadeMotion(lerpBox(START, it->from, it->lastE.value_or(E)), box, T);
            it->lastE          = E;

            WINDOW->m_realPosition->m_Value = Vector2D{box.x / SCALE + monitor->m_position.x, box.y / SCALE + monitor->m_position.y};
            WINDOW->m_realSize->m_Value     = Vector2D{std::max(4.0, box.width / SCALE), std::max(4.0, box.height / SCALE)};
            // AS ANIMATING, or Hyprland will not scale the window's SUB-surfaces
            // with it (CSurfacePassElement squishes a subsurface only while the
            // size animates): Zen's page stayed full size inside a shrinking
            // frame, and the two flew apart (user, 2026-09-30, spam-minimizing).
            // Landing warps both vars, which clears the flag again.
            WINDOW->m_realPosition->m_bIsBeingAnimated = true;
            WINDOW->m_realSize->m_bIsBeingAnimated     = true;
            // And "animating in": without it Hyprland CROPS a surface drawn
            // smaller than the client's size (calculateUVForSurface) instead of
            // scaling it -- the same bit the overview's tiles set. Cleared at
            // landing (landingSanity) and on a reversal.
            WINDOW->m_animatingIn = true;
            WINDOW->alpha()[Desktop::View::WINDOW_ALPHA_FADE]->setValueAndWarp(alpha);
            // Motion blur rides the real window's next draw (FlightBlur.hpp).
            FlightBlur::recordDriven(monitor, WINDOW, PREVBOX, box);

            if (it->phase == EFlightPhase::Armed)
                transition(*it, EFlightPhase::Flying, "first-frame");
            it->drawn = SDrawn{.box = box, .alpha = alpha};
            zdbg("drive r=1 box=%f,%f %fx%f a=%f\n", box.x, box.y, box.width, box.height, alpha);
            ++it;
            continue;
        }

        CBox PREVBOX;
        if (it->isRestore()) {
            // Overview path: proxy machinery (Y5 divergence: GNOME does NOT fade
            // a window back in; the user asked for "fade out/in").
            const CBox START = it->start.value_or(ITEM->iconBox);
            box              = lerpBox(START, it->from, E);
            alpha            = it->alphaFrom.value_or(0.F) * (1.F - E) + E;
            PREVBOX          = lerpBox(START, it->from, it->lastE.value_or(E));
        } else {
            const CBox START = it->start.value_or(it->from);
            box              = lerpBox(START, ITEM->iconBox, E);
            alpha            = it->alphaFrom.value_or(1.F) * (1.F - E);
            PREVBOX          = lerpBox(START, ITEM->iconBox, it->lastE.value_or(E));
        }
        it->lastE = E;
        PREVBOX   = FlightBlur::fadeMotion(PREVBOX, box, T); // half as long, by the flight's own clock

        if (it->phase == EFlightPhase::Armed)
            transition(*it, EFlightPhase::Flying, "first-frame");
        it->drawn = SDrawn{.box = box, .alpha = alpha};
        {
            const auto& GC = WINDOW->m_realBorderColor.m_colors;
            zdbg("draw r=%d box=%f,%f %fx%f a=%f bc=%02x%02x%02x\n", (int)it->isRestore(), box.x, box.y, box.width, box.height, alpha, GC.empty() ? 0 : sc<int>(GC[0].r * 255.F),
                 GC.empty() ? 0 : sc<int>(GC[0].g * 255.F), GC.empty() ? 0 : sc<int>(GC[0].b * 255.F));
        }
        // Z16 (user): "shadows and the thin window outline do not stay on the
        // window as it maximizes". The proxy is a bare surface; a real window
        // carries Hyprland's shadow and border. Same shadow pass the overview
        // cards use, and the window's own border gradient (focused by Z9, so the
        // active colours), both riding the flight's box and alpha.
        const auto drawProxy = [&](bool passIsOurs) {
            OvInternal::renderOverviewWorkspaceShadow(monitor, box, 1.F, false, alpha);
            ScrollOverview::Dock::drawFlightSurface(monitor, WINDOW, box, alpha, now, passIsOurs);
            const int BORDERSIZE = std::max(0, ScrollOverview::Config::getValue<int>("general:border_size"));
            if (BORDERSIZE > 0 && alpha > 0.01F) {
                CBorderPassElement::SBorderData border;
                border.box        = box;
                border.grad1      = WINDOW->m_realBorderColor;
                border.a          = alpha;
                border.borderSize = BORDERSIZE;
                border.round      = sc<int>(sc<float>(ScrollOverview::Config::themeRounding()) * monitor->m_scale);
                g_pHyprRenderer->addPassElement(makeUnique<CBorderPassElement>(border));
            }
        };
        // Motion blur (FlightBlur.hpp): the whole proxy -- shadow, window,
        // border -- smears as one. Drawn into a nested pass, so it must not
        // flush the real one (passIsOurs = false).
        if (!FlightBlur::drawBlurred(monitor, WINDOW, PREVBOX, box, [&] { drawProxy(false); }))
            drawProxy(ownsPass);
        ++it;
    }

    // A landed / aborted / reversed-into-proxy flight must not leave its window
    // rendering through the blur transformer.
    FlightBlur::sweep([](const PHLWINDOW& w) {
        const auto IT = find(w);
        return IT != g_flights.end() && IT->isDriven();
    });
}

std::optional<SDrawnFlight> drawnFor(const PHLWINDOW& window) {
    const auto IT = find(window);
    if (IT == g_flights.end() || !IT->drawn)
        return std::nullopt;
    return SDrawnFlight{.box = IT->drawn->box, .alpha = IT->drawn->alpha, .progress = flightProgress(*IT)};
}

void retargetRestores() {
    for (auto& f : g_flights) {
        if (!f.isDriven())
            continue;
        const auto W   = f.window.lock();
        const auto MON = W ? W->m_monitor.lock() : PHLMONITOR{};
        if (!W || !MON)
            continue;
        const float    SCALE = std::max(MON->m_scale, 0.01F);
        const Vector2D POS   = W->m_realPosition->goal();
        const Vector2D SZ    = W->m_realSize->goal();
        f.from               = CBox{(POS.x - MON->m_position.x) * SCALE, (POS.y - MON->m_position.y) * SCALE, SZ.x * SCALE, SZ.y * SCALE};
    }
}

void abortAll(const std::string& reason) {
    if (g_pEventLoopManager) {
        for (auto seq : g_flightDoLaterSeqs) {
            if (seq)
                g_pEventLoopManager->removeDoLater(seq);
        }
    }
    g_flightDoLaterSeqs.clear();

    if (g_flights.empty())
        return;

    zdbg("abortAll reason=%s flights=%zu\n", reason.c_str(), g_flights.size());

    // Deliberately EVERY flight, not just one monitor's. By monitor.removed
    // Hyprland has already moved the workspaces off it, so a window can no longer
    // be attributed to the output its flight was aimed at; settling a flight on a
    // surviving output costs one skipped animation during a hotplug, which is the
    // cheaper of the two wrong answers.
    //
    // No landing hold either -- Z4's two held frames bridge one render to the
    // next, and this runs between frames with no next one promised.
    while (!g_flights.empty())
        completeAndErase(g_flights.begin(), nullptr, reason.c_str());
}

}
