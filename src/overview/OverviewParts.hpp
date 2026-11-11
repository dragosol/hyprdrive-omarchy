#pragma once
// COverview parts: the include block of the original scrollOverview.cpp, verbatim,
// plus what the parts share. Included only by the src/overview/Overview.cpp parts.

#include <numeric>
#include <hyprland/src/desktop/state/FadingOutState.hpp>
#include <hyprland/src/protocols/PointerConstraints.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/pointer/PointerController.hpp>
#include <hyprland/src/render/pass/RendererHintsPassElement.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include "ShowDesktop.hpp"
#include "OverviewInternal.hpp"
#include "DebugPaths.hpp"
#include "Mosaic.hpp"
#include "FlightBlur.hpp"
#include "OpenProf.hpp"
#include "FramePacing.hpp"
#include "DockStacks.hpp"
#include "SearchLetters.hpp"
#include "KeyboardCat.hpp"
#include "Motion.hpp"
#include "BorderFade.hpp"
#include "DesktopOrder.hpp"
#include "FullscreenDesktop.hpp"
#include "SubmapBorrow.hpp"

#include "Compat.hpp"
#include "Scratchpad.hpp"
#include "Placement.hpp"
#include "BuiltinIcons.hpp"
#include "WorkspacePlan.hpp"
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/config/shared/workspace/WorkspaceRuleManager.hpp>

// The detent tick spawns through CProcess::runAsync. Included explicitly rather
// than leaned on transitively: this is the only user in this TU, and a silent
// loss of the transitive path would fail at the one call site that matters.
#include <hyprutils/os/Process.hpp>
#include <unistd.h> // access(X_OK) on the detent sound before spawning it
using namespace OvInternal; // opted in here rather than leaked from the header

namespace OverviewDetail {

// THE WALLPAPER CACHES OUTLIVE THE OVERVIEW (2026-09-30). The blurred
// backdrop and the card wallpaper FB used to die with each overview, so every
// open re-rendered the wallpaper and re-ran the 4-pass full-resolution kawase
// on its third frame -- ~12 ms of GPU plus ~16 ms more for the card fill and
// the allocations, in the middle of the open animation, which is where the
// dropped frames at the start of an open came from (openprof). Now a closing
// overview hands them here and the next one on that monitor takes them back.
// While no overview is up, the stash listens to the wallpaper layers' own
// commits (the damageSurface hook that does this during an open is only
// installed during one -- relying on it left a changed wallpaper stale); the
// signature check covers everything else, exactly as it did within one open. ~2 full-screen FBs per monitor stay allocated.
    struct SWallpaperStash {
        SP<Render::IFramebuffer> backdropFB, cardWallpaperFB;
        bool                     backdropDirty = true, cardWallpaperDirty = true;
        uint64_t                 lastBackdropSig = 0, lastCardWallpaperSig = 0;
        int                      lastBackdropWallpaperMode = -1;
        // The wallpaper layers' own commits, while no overview is up to hear
        // them (the damageSurface hook is installed only while one is).
        std::vector<CHyprSignalListener> listeners;
    };

    // WHAT THE FROST BEHIND THE CARDS SHOWS (user, 2026-11-03: "when we have a
    // live wallpaper or moving background ... I want the blurred darkened
    // background in overview to be a static screenshot, or a live blur").
    // plugin:hyprdrive:backdrop. A live wallpaper (linux-wallpaperengine,
    // mpvpaper) is a BOTTOM layer committing 30-60 times a second; the frost
    // used to show only BACKGROUND, so it never saw it. Every mode but LIVE is
    // frozen while the overview is up -- the commits of a moving wallpaper
    // never re-run the full-screen kawase -- and the cards are untouched: they
    // stream the live wallpaper as before.
    using Hyprdrive::Config::EBackdrop;

    // How often each monitor's wallpaper layers commit, for AUTO: the commits
    // of the last whole second, or of the second running if it is already past.
    struct SWallpaperRate {
        Time::steady_tp windowStart{};
        int             count = 0;
        double          rate  = 0.0;
    };

    // While an overview is up: the same commits the stash hears while closed.
    struct SOpenListen {
        uint64_t                         layerSig = 0;
        std::vector<CHyprSignalListener> listeners;
    };
}

// Omarchy lockscreen (shell/plugins/lock/LockView.qml MultiEffect):
//   blur: 1.0, blurMax: 128, blurMultiplier: 1.25, contrast: -0.08
// Dual-kawase size * 2^passes ≈ 128px; contrast 0.92 ≈ 1 - 0.08; no grain.
// Independent of decoration:blur:enabled — the lockscreen blurs even when
// Omarchy windows do not.
// The overview's 80% black over the blurred backdrop (baked into the cache).
constexpr float BACKDROP_DIM             = 0.80F;
constexpr int   LOCKSCREEN_BLUR_SIZE     = 8;
constexpr int   LOCKSCREEN_BLUR_PASSES   = 4;
constexpr float LOCKSCREEN_BLUR_CONTRAST = 0.92F;
// DEPTH (cardDepth): how much smaller a card behind another is drawn, fully under it.
static constexpr float DEPTH_SHRINK = 0.12F;

namespace OverviewDetail {

// WHAT CADENCE THE OVERVIEW IS ACTUALLY DRAWN AT, per output, kept after it
// closes so the question can be asked from a terminal (user, 2026-09-19: the
// search field "expanding at a weird framerate"). An animation that is smooth
// in arithmetic and choppy on screen is a frame-supply problem, and the two
// have opposite fixes -- this is how they are told apart, and it costs one
// timestamp per frame.
struct SFrameCadence {
    std::array<float, 120> gaps{};
    size_t                 count = 0;
    size_t                 next  = 0;
    Time::steady_tp        last{};
};
// THE PILL PLAYS (user, 2026-11-03: "grow it a little when I press it, just a
// playful organic grow (and shrink on release) ... no function just fun";
// "when I hover over it it just grows wider to show the whole text but when I
// click it it grows until I release"; then: "instead of growing it should
// shrink ... except get wider on hover to show the full text anyway", and
// pressed "it should say 'Just type something!'" -- you never need to click it
// to type). Hovered with nothing typed, the field widens on its own width
// spring to show the whole placeholder (drawSearch sets the content it fits).
// Pressed, the whole pill squeezes -- the longer it is held the smaller, down
// to 1 - PRESS_MAX -- while the placeholder crossfades to the hint, and on
// release it springs back past rest and settles: an underdamped spring on the
// wall clock, stepped where the field's springs are (renderSearch), drawn as a
// scale about the pill's centre.
    struct SPillPress {
        static constexpr float K = 260.F, C = 13.F, PRESS_MAX = 0.12F, PRESS_TAU_S = 0.55F;
        bool                           down = false;
        Time::steady_tp                downAt{};
        float                          x = 1.F, v = 0.F;
        std::optional<Time::steady_tp> last;
        bool                           moving = false;
        float target(const Time::steady_tp& now) const {
            if (!down)
                return 1.F;
            const float HELD = SearchLetters::msSince(now, downAt) / 1000.F;
            return 1.F - 0.04F - (PRESS_MAX - 0.04F) * (1.F - std::exp(-HELD / PRESS_TAU_S));
        }
        void step(const Time::steady_tp& now) {
            if (!last) {
                last = now;
                return;
            }
            float dt = std::clamp(SearchLetters::msSince(now, *last) / 1000.F, 0.F, 0.05F);
            last     = now;
            const float T = target(now);
            while (dt > 0.F) {
                const float H = std::min(dt, 1.F / 240.F);
                v += (-K * (x - T) - C * v) * H;
                x += v * H;
                dt -= H;
            }
            moving = down || std::abs(x - T) > 0.0005F || std::abs(v) > 0.005F;
            if (!moving) {
                x = T;
                v = 0.F;
            }
        }
    };
}

// Shared between the parts: the state lives in OverviewState.cpp (one TU, original order).
extern std::chrono::steady_clock::time_point g_pillReleasedAt;

namespace OverviewDetail {
    extern std::unordered_map<MONITORID, SWallpaperStash> g_wallpaperStash;
    extern uint64_t g_wallpaperCommits;
    extern std::unordered_map<MONITORID, SWallpaperRate> g_wallpaperRate;
    extern std::unordered_map<MONITORID, uint64_t> g_wallpaperSeq;
    extern std::unordered_map<MONITORID, SOpenListen> g_openListen;
    extern std::unordered_map<const COverview*, std::vector<PHLWORKSPACEREF>> g_phantomsByOverview;
    extern std::unordered_map<std::string, std::pair<uint64_t, uint64_t>> g_unsuspendCount;
    extern std::unordered_map<std::string, std::pair<uint64_t, uint64_t>> g_frameSent;
    extern std::unordered_map<MONITORID, SFrameCadence> g_frameCadence;
    extern std::unordered_map<std::string, const char*> g_frameGateReason;
    extern std::unordered_map<std::string, const char*> g_damageGateReason;
    extern int g_searchLitNow, g_searchLitPrev;
    extern Time::steady_tp g_searchLitAt;
    extern bool g_pillHovered;
    extern SPillPress g_pillPress;
    extern SearchLetters::SAnimator g_placeholderLetters;
    extern std::vector<WP<SKeybind>> g_passthroughBinds;
    extern uint64_t g_warpSeq;
    extern bool g_handoffShutdown;
    extern bool g_warpCentred;

    // Helpers defined in one part and called from another.
    EBackdrop resolvedBackdrop(MONITORID id);
    CBox depthScaled(const CBox& box, const Vector2D& centre, float k);
    WORKSPACEID workspaceKey(const PHLWORKSPACE& ws);
// WorkspacePlan facts: which empty desktops each overview holds as phantoms.
    void forgetPhantomsOf(const COverview* overview);
    Hyprdrive::WorkspacePlan::SRow workspacePlanFor(const PHLMONITOR& monitor);
    void noteFrame(MONITORID id);
    bool pillPlaying();
    bool searchHoverEasing();
    PHLWORKSPACE overviewShownSpecialOn(const PHLMONITOR& monitor);
    bool onShownSpecial(const PHLWINDOW& window, const PHLMONITOR& monitor);
    PHLWINDOW overviewSushiWindowOn(const PHLMONITOR& monitor);
}
