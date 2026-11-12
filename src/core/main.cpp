#include "PluginParts.hpp"
#include "FunctionHooks.hpp"
#include "WindowPlacement.hpp"
#include "Workers.hpp"
#include "TerminateLog.hpp"
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
using namespace PluginDetail;

namespace PluginDetail {
// Methods
CFunctionHook* g_pScrollRenderWorkspaceHook = nullptr;
CFunctionHook* g_pScrollAddDamageHookA      = nullptr;
CFunctionHook* g_pScrollAddDamageHookB      = nullptr;
CFunctionHook* g_pScrollDamageSurfaceHook   = nullptr;
CFunctionHook* g_pScrollScheduleFrameHook   = nullptr;
CFunctionHook* g_pScrollSendFrameEventsHook = nullptr;
CFunctionHook* g_pScrollSurfaceFrameHook    = nullptr;
CFunctionHook* g_pScrollMouseMovedHook      = nullptr;

bool g_unloading = false;
} // namespace PluginDetail

// Do NOT change this function.
APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

namespace PluginDetail {
bool renderingOverview = false;
bool damageFromSurface = false;
PHLMONITORREF renderingOverviewMonitor;
bool g_scrollOverviewHooksActive = false;

// X30. The autohide barrier needs the RELATIVE motion, which only this call
// carries: the bus's mouse.move gives the absolute position, and at the bottom
// edge the cursor is clamped, so the position stops changing while the user is
// still pushing. Fed AFTER the original, so the position the barrier tests
// against is the one this motion produced.
// The arrow over the dock's chrome on the desktop (see g_pDockHoverHook).
bool g_dockCursor = false;

// ConfigureHold.hpp: a gesture's in-between sizes never reach the client.
CFunctionHook* g_pSendWindowSizeHook = nullptr;

// Mosaic.hpp hideDuringSlide: a canvas window off its view stays off the
// desktop slide.
CFunctionHook* g_pShouldRenderWindowHook = nullptr;
} // namespace PluginDetail

// SizeAck.hpp: Hyprland's CWindow::onAck, minus its bug. Never calls the original.
static CFunctionHook* g_pOnAckHook = nullptr;

namespace PluginDetail {
// DockRoom.hpp: tiled windows make room for a dock that does not hide.
// OwnSizeTile.hpp, inside it: a fixed-size window's tile has room for it.
CFunctionHook* g_pTargetPlaceHook = nullptr;

// Keycodes, not a count: Super-tap key-repeat used to increment a counter
// without a matching release, after which searchKeyFor refused every letter
// for the rest of the session.
std::unordered_set<uint32_t> g_heldGuardKeys;

// ---------------------------------------------------------------------------
// THE LETTER TICK (user, 2026-09-19: the pop is "sometimes smooth sometimes
// it's not", worst on "the trailing characters bouncing").
//
// The field's animation was kept alive from INSIDE the render pass: renderSearch
// ended in `damage(); requestInputFrame();`, and requestInputFrame is a
// scheduleFrame issued while a frame is already being composited, where it is
// liable to be absorbed by the one in flight instead of booking the next one.
//
// MEASURED, not guessed (49s screen recording plus the in-compositor profile,
// 3140 frames). Frames dropped, by position in the pop:
//
//     f=2-4   (first 65ms)      0.0 - 0.5%
//     f=6-8   (100-135ms)      15   - 17%
//     f=9-12                   22   - 29%
//     f=15-21 (250-350ms)      36   - 39%
//
// The head of every pop is clean because the compositor's OWN animation
// variables -- resultsWidth, searchDock, resultsBand -- are still running and
// schedule those frames as a side effect. Once they settle, the letters are the
// only thing still moving and the in-pass request is all that is left, so the
// tail of every pop loses better than one frame in three. That is why it got
// worse the more letters were on screen (more tail), why it was periodic rather
// than constant, and why a held backspace stayed smooth until the end -- the
// same coupling Overview.cpp's frame-policy comment records from the
// earlier incarnation, where letters froze outright once the animvars settled.
//
// Ticking from the event loop instead: half a 60Hz frame, so a request is
// always pending before the next vblank, self-sustaining while anything in the
// field still moves and stopping on its own when nothing does. renderSearch
// keeps its own call as a belt-and-braces fallback; it is harmless when a tick
// has already booked the frame.
wl_event_source*  g_letterTickTimer = nullptr;

// OVERVIEW_CPP_SPEC 5 F5. The overview is not a wayland client. Seat keyboard
// focus is cleared while it is open, and the query is assembled from the raw
// key stream with those events cancelled so they cannot reach a window or bar.
//
// This used to be a pure isSearchKey(keycode) predicate, consulted separately
// for the press and the release on the promise that both would agree. COMPOSE
// IS STATEFUL AND BREAKS THAT PROMISE: a sequence can complete between a press
// and its release, so the predicate flips underneath and leaks a stray release
// into the window below. The decision is made ONCE, on the press, and the
// release is consumed iff that press was.
std::unordered_set<uint32_t> g_consumedPresses;

// Hyprland's key-repeat timer only fires for focused clients. Search steals
// seat focus, so Backspace would delete once and then stall until another
// physical press. Drive repeats ourselves off the compositor loop.
wl_event_source* g_searchRepeatTimer = nullptr;
uint32_t         g_searchRepeatKey   = ~0u;
} // namespace PluginDetail

static int              g_fileWakeFd     = -1;
static wl_event_source* g_fileWakeSource = nullptr;

namespace PluginDetail {
CHyprSignalListener g_pRawKeyHook;
} // namespace PluginDetail

static CHyprSignalListener g_pSessionLockHook;

namespace PluginDetail {
CHyprSignalListener g_pTapSuperButtonHook;
} // namespace PluginDetail

static CHyprSignalListener g_pRenderPreHook;
static CHyprSignalListener g_pMembOpenHook;
static CHyprSignalListener g_pMembLateHook;
static CHyprSignalListener g_pMembCloseHook;
static CHyprSignalListener g_pMembKillHook;
static CHyprSignalListener g_pDockFocusHook; // Dock::noteFocus
static CHyprSignalListener g_pMembGoneHook;
static CHyprSignalListener g_pMembPinHook;
static CHyprSignalListener g_pMembFloatHook;
static CHyprSignalListener g_pMembTitleHook;
static CHyprSignalListener g_pMembClassHook;
static CHyprSignalListener g_pMembWsHook;
static CHyprSignalListener g_pMembFullHook;
static CHyprSignalListener g_pDockConfigHook;
static CHyprSignalListener g_pDockHoverHook;
static CHyprSignalListener g_pDockClickHook;
static CHyprSignalListener g_pDockScrollHook;
static CHyprSignalListener g_pDockRenderHook;
static CHyprSignalListener g_pDockStandDownHook;
static CHyprSignalListener g_pDockFsWsHook;
static CHyprSignalListener g_pDockFsSpecialHook;
// The doLater the stand-down queued; 0 = none (cancelled at unload).
static uint32_t            g_dockStandDownSeq = 0;
static CHyprSignalListener g_pScratchExitRenderHook;
static CHyprSignalListener g_pMonitorGoneHook;
static CHyprSignalListener g_pScrollCameraActiveHook;
static CHyprSignalListener g_pScrollCameraMovedHook;
static CHyprSignalListener g_pClickPreReloadHook;

namespace PluginDetail {
bool                g_tapSuperCandidate = false;
uint32_t            g_tapSuperPressedAt = 0;
} // namespace PluginDetail

// Mosaic.cpp: Super held down to pan an infinite canvas was not a tap.
namespace Hyprdrive {
    void cancelSuperTap() {
        g_tapSuperCandidate = false;
    }
}

namespace PluginDetail {
bool                g_tapShiftCandidate = false;
uint32_t            g_tapShiftPressedAt = 0;
// Release time of the last completed SHIFT tap; 0 = no pair in flight.
uint32_t            g_tapShiftLastTapMs = 0;
bool                g_tapCtrlCandidate  = false;
uint32_t            g_tapCtrlPressedAt  = 0;
uint32_t            g_tapCtrlLastTapMs  = 0;
} // namespace PluginDetail

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    HYPRDRIVE_HANDLE = handle;

    // Every worker thread is started through Workers (Workers.hpp). Accepting
    // again matters only to an image an earlier unload had to keep loaded.
    Hyprdrive::Workers::setLog([](const std::string& line) { Log::logger->log(Log::ERR, "{}", line); });
    Hyprdrive::Workers::reopen();

    // The debug logs older versions left in /tmp, readable by everyone at
    // umask 022: ours are removed at every load (DebugPaths::removeLegacy).
    Hyprdrive::DebugPaths::removeLegacy();

    // Before anything can record a use of a pre-rename name. ONE RELEASE.
    Hyprdrive::Compat::openLog();

    const std::string HASH        = __hyprland_api_get_hash();
    const std::string CLIENT_HASH = __hyprland_api_get_client_hash();

    if (HASH != CLIENT_HASH) {
        failNotif("Version mismatch (headers ver is not equal to running hyprland ver)");
        throw std::runtime_error("[he] Version mismatch");
    }

    g_pScrollRenderWorkspaceHook = Hyprdrive::FunctionHooks::required("main", "renderWorkspace", {"CHyprRenderer::renderWorkspace(", "IHyprRenderer::renderWorkspace("}, rc<void*>(hkRenderWorkspace));

    g_pScrollScheduleFrameHook = Hyprdrive::FunctionHooks::required("main", "_ZN7Monitor8CMonitor13scheduleFrameEN10Aquamarine7IOutput19scheduleFrameReasonE", {""}, rc<void*>(hkScheduleFrame));

    g_pScrollDamageSurfaceHook = Hyprdrive::FunctionHooks::required("main", "damageSurface", {"CHyprRenderer::damageSurface(", "IHyprRenderer::damageSurface("}, rc<void*>(hkDamageSurface));

    g_pScrollSendFrameEventsHook = Hyprdrive::FunctionHooks::required("main", "sendFrameEventsToWorkspace", {"CHyprRenderer::sendFrameEventsToWorkspace(", "IHyprRenderer::sendFrameEventsToWorkspace("}, rc<void*>(hkSendFrameEventsToWorkspace));

    g_pScrollSurfaceFrameHook = Hyprdrive::FunctionHooks::required("main", "_ZN18CWLSurfaceResource5frameERKNSt6chrono10time_pointINS0_3_V212steady_clockENS0_8durationIlSt5ratioILl1ELl1000000000EEEEEE", {""}, rc<void*>(hkSurfaceFrame));

    g_pScrollAddDamageHookB = Hyprdrive::FunctionHooks::required("main", "addDamageEPK15pixman_region32", {"CMonitor::addDamage"}, rc<void*>(hkAddDamageB));

    g_pScrollAddDamageHookA = Hyprdrive::FunctionHooks::required("main", "_ZN7Monitor8CMonitor9addDamageERKN9Hyprutils4Math4CBoxE", {""}, rc<void*>(hkAddDamageA));

    // X30. Hooked HERE and not through ensureScrollOverviewHooks(): those go on
    // and off with the overview, and the autohide barrier lives on the plain
    // desktop, which is exactly where they are not installed.
    g_pScrollMouseMovedHook = Hyprdrive::FunctionHooks::required("main", "onMouseMoved", {"CInputManager::onMouseMoved("}, rc<void*>(hkOnMouseMoved));
    g_pScrollMouseMovedHook->hook();

    g_pSendWindowSizeHook = Hyprdrive::FunctionHooks::required("main", "sendWindowSize", {"CWindow::sendWindowSize("}, rc<void*>(hkSendWindowSize));
    g_pSendWindowSizeHook->hook();

    // ONE hook that DockRoom and OwnSizeTile both ride (Hooks.cpp, hkTargetPlace).
    g_pTargetPlaceHook = Hyprdrive::FunctionHooks::required("main", "setPositionGlobal", {"CWindowTarget::setPositionGlobal("}, rc<void*>(hkTargetPlace));
    g_pTargetPlaceHook->hook();
    Hyprdrive::ConfigureHold::setSender([](Desktop::View::CWindow* window, bool force) {
        if (g_pSendWindowSizeHook)
            rc<origSendWindowSize>(g_pSendWindowSizeHook->m_original)(window, force);
    });
    g_pOnAckHook = Hyprdrive::FunctionHooks::required("main", "onAck", {"CWindow::onAck("}, rc<void*>(hkOnAck));
    g_pOnAckHook->hook();
    g_pShouldRenderWindowHook = Hyprdrive::FunctionHooks::required("main", "shouldRenderWindow", {"shouldRenderWindow(Hyprutils::Memory::CSharedPointer<Desktop::View::CWindow>, Hyprutils::Memory::CSharedPointer<Monitor::CMonitor>)"}, rc<void*>(hkShouldRenderWindow));
    g_pShouldRenderWindowHook->hook();

    CursorShape::init();
    OpenProf::init();
    // NOT FramePacing::apply() here: plugin keys are registered further down
    // (registerConfig), and reading an unregistered key aborts Hyprland in
    // CConfigValue::bind -- which is exactly how 2026-09-29's build crash-looped
    // every login. The config.reloaded hook and every overview open apply it.
    g_pRenderPreHook = Event::bus()->m_events.render.pre.listen([](PHLMONITOR monitor) {
        // Throttled inside to a few stat()s every couple of seconds.
        AppSearch::pollForChanges();

        if (const auto overview = scrollOverviewForMonitor(monitor))
            overview->onPreRender();

        Hyprdrive::Dock::damageIfChanged(monitor);

        // The desktop strip's place for this frame (DesktopStrip.hpp), then a
        // canvas desktop a swipe brings in starts past its overhang (Mosaic.cpp).
        Hyprdrive::DesktopSwitch::preRender(monitor);
        Hyprdrive::Mosaic::preRender(monitor);
        Hyprdrive::FullscreenDesktop::preRender(monitor);
        Hyprdrive::Intent::preRender(monitor);
        // After Mosaic's: a live swipe's place, for its release (DesktopStrip.hpp).
        Hyprdrive::DesktopStrip::sampleSwipe(monitor);
        // Show desktop's slide, after every module that places windows (it has
        // the last word on where they are drawn) and before the motion blur
        // reads where they went (ShowDesktop.hpp).
        Hyprdrive::ShowDesktop::preRender(monitor);

        // Layout motion blur (FlightBlur.hpp). The overview draws its own
        // windows; there, only the sweep runs.
        Hyprdrive::DockRoom::watch(monitor);

        const auto FLYING = [](const PHLWINDOW& w) { return Hyprdrive::Flight::hasFlight(w); };
        if (!scrollOverviewForMonitor(monitor))
            Hyprdrive::FlightBlur::trackLayoutMotion(monitor, FLYING);
        else {
            // The overview draws its own tiles; only a special workspace (the
            // scratchpad opening or closing over it) is drawn natively, through
            // the renderer the blur rides (user, 2026-09-30).
            Hyprdrive::FlightBlur::trackLayoutMotion(monitor, [&FLYING](const PHLWINDOW& w) {
                return FLYING(w) || !w->m_workspace || !w->m_workspace->m_isSpecialWorkspace;
            });
        }
    });

    // X27. NOT the renderWorkspace hook: that one is installed on demand by
    // ensureScrollOverviewHooks() and dropped again by removeOverview(), so it
    // does not exist on a plain desktop -- which is exactly where a dock lives.
    // POST_WINDOWS also puts the dock above windows and below the bar's top
    // layer.
    //
    // §10 clause 6, pinned-in-overview: this stage fires far below the overview's
    // own pass, so the pill was buried under it. The overview now draws the dock
    // itself at the end of its render, and this hook stands down for that monitor
    // -- otherwise the pill is drawn twice and its translucent background blends
    // over itself.
    // The dock's hover highlight on a PLAIN desktop. render.pre only fires when a
    // frame is already happening, so on an idle desktop a pointer move over the
    // pill damages nothing and the highlight never appears. The overview has its
    // own mouse hook and calls this from there.
    // X30. Flipping dock-fixed at runtime damages nothing by itself: with the
    // pointer still and no window repainting, there is no frame for the dock to
    // be drawn into and the change would not show until something else moved.
    // P6i1 SHADOW. Everything the compositor tells us that can change the
    // membership gate's answer: what windows exist, and the four properties the
    // gate reads off one. The plugin's own mutators -- setHidden for a flight,
    // minimize/restore, the search query -- bump at their own call sites, since
    // no event announces them.
    g_pMembOpenHook  = Event::bus()->m_events.window.open.listen([](PHLWINDOW w) {
        Membership::bump("window.open");
        Hyprdrive::Dock::onWindowOpen(w);
        Hyprdrive::Dock::noteFullscreenCover();
    });
    g_pMembLateHook  = Event::bus()->m_events.window.openLate.listen([](PHLWINDOW) { Membership::bump("window.openLate"); });
    g_pMembCloseHook = Event::bus()->m_events.window.close.listen([](PHLWINDOW w) {
        Membership::bump("window.close");
        Hyprdrive::Mosaic::onWindowClose(w); // its mosaic size, remembered per app
        Hyprdrive::Dock::onWindowClose(w);   // a menu row for it leaves the menu
        Hyprdrive::Dock::noteFullscreenCover();
    });
    g_pMembKillHook  = Event::bus()->m_events.window.kill.listen([](PHLWINDOW) { Membership::bump("window.kill"); });
    g_pDockFocusHook = Event::bus()->m_events.window.active.listen([](PHLWINDOW w, Desktop::eFocusReason reason) {
        Hyprdrive::Dock::noteFocus(w, reason);
        Hyprdrive::Dock::noteFullscreenCover();
    });
    g_pMembGoneHook  = Event::bus()->m_events.window.destroy.listen([](PHLWINDOWREF) { Membership::bump("window.destroy"); });
    g_pMembPinHook   = Event::bus()->m_events.window.pin.listen([](PHLWINDOW) { Membership::bump("window.pin"); });
    g_pMembFloatHook = Event::bus()->m_events.window.floating.listen([](PHLWINDOW) { Membership::bump("window.floating"); });
    g_pMembTitleHook = Event::bus()->m_events.window.title.listen([](PHLWINDOW) { Membership::bump("window.title"); });
    g_pMembClassHook = Event::bus()->m_events.window.class_.listen([](PHLWINDOW) { Membership::bump("window.class"); });
    g_pMembWsHook    = Event::bus()->m_events.window.moveToWorkspace.listen([](PHLWINDOW, PHLWORKSPACE) {
        Membership::bump("window.moveToWorkspace");
        Hyprdrive::Dock::noteFullscreenCover();
    });
    g_pMembFullHook  = Event::bus()->m_events.window.fullscreen.listen([](PHLWINDOW) {
        Membership::bump("window.fullscreen");
        Hyprdrive::Dock::noteFullscreenCover();
    });
    // The dock over a fullscreen window (DockFullscreen.cpp): what a monitor
    // shows changes with its desktop too.
    g_pDockFsWsHook      = Event::bus()->m_events.workspace.active.listen([](PHLWORKSPACE) { Hyprdrive::Dock::noteFullscreenCover(); });
    g_pDockFsSpecialHook = Event::bus()->m_events.workspace.specialActive.listen([](PHLWORKSPACE, PHLMONITOR) { Hyprdrive::Dock::noteFullscreenCover(); });

    // The dock's keys as they stood BEFORE this reload parsed anything. See the
    // abortAll below for what it is compared against.
    static uint64_t s_dockKeysBeforeReload = 0;

    g_pClickPreReloadHook = Event::bus()->m_events.config.preReload.listen([] {
        clearClickGestures();
        s_dockKeysBeforeReload = Hyprdrive::Config::dockKeyFingerprint();
    });

    g_pDockConfigHook = Event::bus()->m_events.config.reloaded.listen([] {
        // A no-op unless the pointer set changed -- a reload is not a reason to
        // drop a touchpad gesture that is still under the hand.
        attachPadFingerListeners();
        Hyprdrive::FingerSwipe::rescan();
        Hyprdrive::Dock::noteFullscreenCover(); // DockFullscreen.cpp
        // FIRST, before anything in this plugin reads a config value this pass:
        // a value the user set under the old plugin:scrolloverview: name is
        // copied onto its plugin:hyprdrive: twin here and nowhere else, so the
        // rest of the codebase only ever knows the new name. ONE RELEASE.
        Hyprdrive::Compat::adoptLegacyValues();
        // A reload writes the user's render:new_render_scheduling back.
        Hyprdrive::FramePacing::apply();
        // A reload can drop plugin-registered beziers, and may have changed the
        // animation:curve preset: point the house names at it again.
        Hyprdrive::Motion::sync();
        // The overview's tile order (GridLayout.hpp): read here, not per frame.
        OvInternal::gridTileOrderReload();
        healStuckOverviewSubmap();

        // `omarchy theme set` writes colors.toml and reloads Hyprland, so the
        // reload IS the theme change as far as this plugin is concerned. Drop
        // the palette and font caches here and the new theme is on the next
        // frame rather than up to half a second later.
        Hyprdrive::Config::invalidateThemeCache();

        // A reload rebuilds the dock's whole layout under whatever is flying,
        // and a flight's boxes are the OLD layout's -- its icon target can move,
        // shrink, or stop existing. Land them first, with the landing every
        // other end of a flight gets, rather than let one chase an icon that is
        // no longer there.
        //
        // ONLY when a dock key actually moved, though. Every theme switch and
        // every unrelated `hyprctl reload` came through here and snapped an
        // in-flight minimize to its end, for a reload that cannot have touched
        // the layout the flight is aiming at. Taken AFTER adoptLegacyValues so a
        // value arriving under the old name counts as the change it is.
        if (const auto DOCKKEYS = Hyprdrive::Config::dockKeyFingerprint(); DOCKKEYS != s_dockKeysBeforeReload) {
            s_dockKeysBeforeReload = DOCKKEYS;
            Hyprdrive::Flight::abortAll("config-reload");
        }

        for (const auto& MONITOR : State::monitorState()->monitors())
            Hyprdrive::Dock::damageIfChanged(MONITOR);

        // The reload just overwrote the overrides an OPEN overview installed
        // (workspace animations, follow_mouse, no_warps) -- without this, a
        // workspace switch inside the overview plays Hyprland's slide again.
        // Only the shared-state owner acts; the method self-guards.
        for (const auto& OVERVIEW : scrollOverviews()) {
            if (const auto SCROLL = OVERVIEW ? dynamic_cast<COverview*>(OVERVIEW.get()) : nullptr)
                SCROLL->reapplyConfigOverridesAfterReload();
        }
    });

    g_pDockHoverHook = Event::bus()->m_events.input.mouse.move.listen([](Vector2D, Event::SCallbackInfo& info) {
        const auto HIT = monitorUnderCursor();

        // The scratchpad's drag-out zone rides the same motion (it is a desktop
        // surface like the dock, and a drag is nothing but motion).
        Hyprdrive::ScratchpadExit::onPointerMove(HIT ? HIT->first : PHLMONITOR{});
        // Dock stacks B: a drag of files over the dock (prefetch + target).
        Hyprdrive::DockDrop::onPointerMove(HIT ? HIT->first : PHLMONITOR{}, HIT ? HIT->second : Vector2D{});
        // A fan item pulled out becomes a real drag (DockDrop::startFileDrag).
        Hyprdrive::Stacks::onPointerMove();
        // A held pointer (a game's lock or confinement) owns the cursor
        // wherever its lock point sits, and a dock that is not drawn (a
        // fullscreen window on the solitary path) is not there to be on.
        // Taking the seat here, or cancelling this motion, skips Hyprland's
        // own lock code -- the game lost its keys and its mouse (issue #11).
        const bool LOCKED = HIT ? Hyprdrive::Dock::inputInert(HIT->first) : Hyprdrive::Dock::pointerHeldByClient();
        // Mosaic's corner resize: the resize cursor over a grabbable corner.
        Hyprdrive::Mosaic::onMove(!HIT || LOCKED || Hyprdrive::Dock::occupiesPointer(HIT->first, HIT->second) || Hyprdrive::Stacks::ownsHover(HIT->first, HIT->second) ||
                                       g_pSessionLockManager->isSessionLocked());
        // The zoomed-out canvas (Mosaic::zoomOwnsPointer): no window under the
        // pointer's full-size place gets the motion or focus, and the arrow
        // (no client's text cursor) while it lasts.
        // Through the bar: the window under it takes focus (Mosaic.hpp).
        if (HIT && !g_pSessionLockManager->isSessionLocked())
            Hyprdrive::Mosaic::focusUnderBar();
        if (Hyprdrive::Mosaic::zoomOwnsPointer()) {
            info.cancelled = true;
            setDockCursor(true);
            return;
        }

        if (!HIT || LOCKED) {
            Hyprdrive::Dock::releasePointerFocus();
            setDockCursor(false);
            if (HIT)
                Hyprdrive::Dock::damageIfChanged(HIT->first); // a pill that is out slides away
            return;
        }
        const auto& [MONITOR, LOCAL] = *HIT;

        // X32. The desktop dock reorders too, and its drag needs motion for
        // the same reason the highlight does: on an idle desktop nothing
        // else is asking for a frame.
        if (Hyprdrive::Dock::dragPending())
            Hyprdrive::Dock::dragMove(MONITOR, LOCAL);

        Hyprdrive::Dock::syncPointerFocus(MONITOR, LOCAL);
        Hyprdrive::Dock::damageIfChanged(MONITOR);

        // HOVER MUST NOT PASS THROUGH (user, 2026-09-29): over the dock's
        // chrome -- pill, hover previews, menu, a stack's fan/pane -- the window
        // underneath got the motion anyway (link highlights, hover states),
        // because this is a listener and Hyprland went on to focus the surface
        // under the cursor. Cancelling here returns from mouseMoveUnified before
        // that (the cursor itself has already moved). Not with the overview up
        // (it owns its own input), and not during a drag-and-drop, whose target
        // Hyprland must keep tracking.
        // An open fan's gaps count too (Stacks::ownsHover): crossing between
        // its papers must not focus the window under them.
        const bool ON_CHROME = !scrollOverviewForMonitor(MONITOR) && !Hyprdrive::DockDrop::dndInProgress() &&
            (Hyprdrive::Dock::occupiesPointer(MONITOR, LOCAL) || Hyprdrive::Stacks::ownsHover(MONITOR, LOCAL));
        if (ON_CHROME)
            info.cancelled = true;
        // The cancel also skips Hyprland's cursor-shape update, so a pointer that
        // came off a text field kept the I-beam over the dock. The dock owns the
        // lowest override group (UNKNOWN -- Hyprland itself never sets it), so a
        // resize edge, a drag or the overview's own override still win.
        setDockCursor(ON_CHROME);
    });

    // X24 dock-side. The overview installs its own button hook; this one is the
    // plain desktop, where nothing else claims a click on the pill. Both edges
    // are swallowed over the pill: the dock is not a window, so a press that
    // reached the window underneath would focus it, and a release that leaked
    // after a swallowed press would leave the pointer stuck.
    g_pDockClickHook = Event::bus()->m_events.input.mouse.button.listen([](IPointer::SButtonEvent event, Event::SCallbackInfo& info) {
        if (handleClickGestureButton(event)) {
            info.cancelled = true;
            return;
        }

        // Mosaic's corner resize (Mosaic.hpp). Not over the dock or an open
        // overview (monitorUnderCursor skips those monitors), not while locked.
        {
            const auto HITM = monitorUnderCursor();
            if (Hyprdrive::Mosaic::onButton(event, !HITM || Hyprdrive::Dock::occupiesPointer(HITM->first, HITM->second) || g_pSessionLockManager->isSessionLocked())) {
                info.cancelled = true;
                return;
            }
        }

        const auto HIT = monitorUnderCursor();
        if (!HIT)
            return;
        const auto& [MONITOR, LOCAL] = *HIT;

        // Dock stacks B: a drag of files released over the dock is the dock's.
        // Runs before Hyprland's own drop listener (registered at drag start).
        if (Hyprdrive::DockDrop::onButton(MONITOR, LOCAL, event.state == WL_POINTER_BUTTON_STATE_PRESSED)) {
            info.cancelled = true;
            return;
        }

        // The plain desktop's button map is fixed: right opens the menu, middle
        // launches, left drives the pill. (The overview honours the left-handed
        // config; the desktop dock never has.) The ladder and the reasons for its
        // order live in DockInput -- this is the same dock the overview drives.
        const auto ROLE = event.button == BTN_RIGHT  ? Hyprdrive::DockInput::EButton::Menu
                        : event.button == BTN_MIDDLE ? Hyprdrive::DockInput::EButton::Launch
                        : event.button == BTN_LEFT   ? Hyprdrive::DockInput::EButton::Main
                        : Hyprdrive::DockInput::EButton::Other;
        const bool PRESSED = event.state == WL_POINTER_BUTTON_STATE_PRESSED;

        // A window dragged back onto the dock goes home the way it does in the
        // overview: the whole minimize FLIGHT -- shrink into the icon, bounce on
        // landing -- and not a silent snap. This is new because the premise
        // below used to hold: before the dock tear-out there was no such thing
        // as a window drag on the plain desktop, so there was nothing to catch.
        if (!PRESSED && ROLE == Hyprdrive::DockInput::EButton::Main && Hyprdrive::Dock::dockOwnsNativeDrag() && g_layoutManager &&
            g_layoutManager->dragController()) {
            const auto TARGET  = g_layoutManager->dragController()->target();
            const auto DRAGGED = TARGET ? TARGET->window() : PHLWINDOW{};
            const bool ONDOCK  = Hyprdrive::Dock::pillContains(MONITOR, LOCAL);

            // END IT OURSELVES. The dock swallowed the press that started this
            // drag, so Hyprland never entered its own button state machine for
            // it and nothing on its side will let go -- the button came up and
            // the window kept following the pointer until the NEXT click.
            // ONLY WITH A TARGET: Hyprland's dragEnd dereferences it unchecked
            // (DragController.cpp: m_target.lock()->window()), and the drag may
            // have ended already or its window gone -- releasing then SIGSEGV'd
            // the compositor (XPS, 2026-11-06 11:19, the whole session lost).
            if (TARGET)
                g_layoutManager->dragController()->dragEnd();
            Hyprdrive::Dock::clearDockNativeDrag();
            const auto ANCHOR = Hyprdrive::Placement::takeDragAnchor();

            // Dropped back onto the dock: the whole minimize flight, same as the
            // overview does, rather than a silent snap.
            if (ONDOCK && DRAGGED)
                Hyprdrive::Flight::minimize(MONITOR, DRAGGED, true); // bounce at the drop, Z10
            // Anywhere else: dragEnd just tiled it off the CURSOR (the window under
            // it, the side it was on). Re-file it the way opening it would --
            // AFTER dragEnd, which is also what has lowered the drag flag.
            else if (DRAGGED)
                Hyprdrive::Placement::retileAsNew(DRAGGED, ANCHOR);

            info.cancelled = true;
            return;
        }

        // A pointer lock's click, or a click where the dock is not drawn, is
        // the window's (Dock::inputInert): the pill's ladder below would act on
        // an icon the user cannot see, or under a game's lock point.
        if (Hyprdrive::Dock::inputInert(MONITOR))
            return;

        // Desktop never has a window drag in flight, and lets a menu miss through
        // (its menu may have no drawn geometry to hit-test); a let-through miss
        // then falls into the pill ladder, exactly as the overview's own dock does.
        if (Hyprdrive::DockInput::handleMenu(MONITOR, LOCAL, ROLE, PRESSED, false, true).claimed) {
            info.cancelled = true;
            return;
        }
        if (Hyprdrive::DockInput::handlePill(MONITOR, LOCAL, ROLE, PRESSED, false).claimed) {
            info.cancelled = true;
            return;
        }

        // Hold-Super window pills are disabled on this fork.
    });

    // X33. The wheel over the pill, on the PLAIN desktop only.
    //
    // Deliberate divergence, and the one place X33 does not copy the reference:
    // with the overview open the reference scrolls the dash's icon to
    // `this.app.activate()`, which would close the overview on a wheel graze.
    // The pill is pinned OVER the overview here (§10 clause 6) and the wheel is
    // the overview's primary navigation, so inside it the overview keeps the
    // tick. Recorded rather than silently dropped.
    g_pDockScrollHook = Event::bus()->m_events.input.mouse.axis.listen([](IPointer::SAxisEvent event, Event::SCallbackInfo& info) {
        // Vertical only. A horizontal wheel has no reference behaviour.
        // Wheel, finger, and continuous (touchpad) all count: SOURCE_WHEEL
        // alone left laptop two-finger scrolls going to the window under the
        // pill. Dead time in Dock::scroll turns a finger stream into steps.
        if (event.axis != WL_POINTER_AXIS_VERTICAL_SCROLL || event.delta == 0.0)
            return;
        if (event.source == WL_POINTER_AXIS_SOURCE_WHEEL_TILT)
            return;

        const auto HIT = monitorUnderCursor();
        if (!HIT || Hyprdrive::Dock::inputInert(HIT->first)) // a game's wheel is the game's; so is one over an undrawn dock
            return;
        const auto& [MONITOR, LOCAL] = *HIT;

        if (Hyprdrive::Dock::occupiesPointer(MONITOR, LOCAL))
            Hyprdrive::Dock::syncPointerFocus(MONITOR, LOCAL);

        Hyprdrive::Stacks::noteAxis(event.delta, event.source == WL_POINTER_AXIS_SOURCE_WHEEL);
        if (Hyprdrive::Dock::scroll(MONITOR, LOCAL, event.delta < 0.0))
            info.cancelled = true;
    });

    g_pDockRenderHook = Event::bus()->m_events.render.stage.listen([](eRenderStage stage) {
        if (stage != RENDER_POST_WINDOWS)
            return;

        const auto MONITOR = g_pHyprRenderer->m_renderData.pMonitor.lock();
        if (scrollOverviewForMonitor(MONITOR))
            return;

        // Y7. No overview means no window drag, and this is also what clears
        // the flag if the overview closed while one was in flight.
        Hyprdrive::Dock::setWindowDragActive(false);

        Hyprdrive::Mosaic::renderZoomLabels(MONITOR);
        Hyprdrive::Dock::render(MONITOR);
        // A file dragged out of a fan: its icon under the pointer, over the dock.
        Hyprdrive::DockDrop::renderGhost(MONITOR);
    });

    // THE SEAT GOES BACK WHEN THE DOCK STANDS DOWN, not on the next motion
    // (issue #11). The pointer resting on the dock holds the seat
    // (syncPointerFocus); a window that then goes fullscreen on the solitary
    // path, or takes a pointer lock, leaves it held -- under a lock no motion
    // ever reaches the dock's test again, so the game had no keyboard and no
    // mouse until the focus moved. render.preChecks fires for every frame,
    // direct scanout included (RENDER_BEGIN does not), and is cheap here: two
    // flags and a pointer compare unless the seat is held. The release itself
    // runs outside the frame.
    g_pDockStandDownHook = Event::bus()->m_events.render.preChecks.listen([](PHLMONITOR monitor) {
        if (g_dockStandDownSeq || !Hyprdrive::Dock::seatHeld() || !g_pEventLoopManager || !Hyprdrive::Dock::inputInert(monitor))
            return;
        g_dockStandDownSeq = g_pEventLoopManager->doLater([] {
            g_dockStandDownSeq = 0;
            const auto HIT = monitorUnderCursor();
            if (!Hyprdrive::Dock::seatHeld() || !(HIT ? Hyprdrive::Dock::inputInert(HIT->first) : Hyprdrive::Dock::pointerHeldByClient()))
                return;
            Hyprdrive::Dock::releasePointerFocus();
            setDockCursor(false);
        });
    });

    // The scratchpad's drag-out zone draws at the LAST moment, not with the
    // dock: it sits at the very top of the screen (user, 2026-09-24: "bump it
    // all the way to the top"), where the bar lives, and POST_WINDOWS is below
    // the top layer -- the bar would paint over it.
    g_pScratchExitRenderHook = Event::bus()->m_events.render.stage.listen([](eRenderStage stage) {
        if (stage != RENDER_LAST_MOMENT)
            return;
        const auto MONITOR = g_pHyprRenderer->m_renderData.pMonitor.lock();
        if (!MONITOR || scrollOverviewForMonitor(MONITOR))
            return;
        Hyprdrive::ScratchpadExit::render(MONITOR);
    });

    // P0. An overview must not outlive its output. Nothing used to tear one
    // down when its monitor went away, so it stayed in the registry holding a
    // dead PHLMONITORREF and the next `overview off` dereferenced it inside
    // close() -- a SIGSEGV, which gdm then papered over by bringing the session
    // straight back WITHOUT the user's config, so the damage outlived the crash.
    // `hyprctl output remove` and a user unplugging a display raise this same
    // event, and it is the one Hyprland's own monitor-reference holders listen
    // to. It is deliberately NOT emitted while the compositor is shutting down,
    // which is what keeps this from queueing a deferred destroy that would never
    // run -- PLUGIN_EXIT owns teardown there.
    //
    // Flights first: they are settled while the overview is still registered, so
    // an unhidden window is re-added to the draw list it is about to leave with
    // rather than after it.
    // Before the desktop is drawn, not after: corrected on the first frame it
    // would slide the window across the screen instead of simply being right.
    // See normalizeFittingScrollingOffset.
    g_pScrollCameraActiveHook = Event::bus()->m_events.workspace.active.listen([](PHLWORKSPACE workspace) { normalizeScrollingCamera(workspace); });
    g_pScrollCameraMovedHook  = Event::bus()->m_events.workspace.moveToMonitor.listen([](PHLWORKSPACE workspace, PHLMONITOR) { normalizeScrollingCamera(workspace); });
    // And whatever is already stuck when the plugin comes up.
    normalizeAllScrollingCameras();

    g_pMonitorGoneHook = Event::bus()->m_events.monitor.removed.listen([](PHLMONITOR monitor) {
        Hyprdrive::Flight::abortAll("monitor-removed");
        dropScrollOverviewsForMonitor(monitor);
        if (monitor) {
            Hyprdrive::Dock::forgetMonitor(monitor->m_id);
            if (const auto OPENGL = g_pHyprRenderer ? g_pHyprRenderer->glBackend() : WP<Render::GL::CHyprOpenGLImpl>{})
                OPENGL->makeEGLCurrent();
            Hyprdrive::releaseWallpaperStash(monitor->m_id);
        }
        // A stack popup on the monitor that went away must not keep the keyboard.
        if (monitor && Hyprdrive::Stacks::popupMonitor() == monitor->m_id)
            Hyprdrive::Stacks::close();
    });

    Hyprdrive::Config::registerDispatcher("overview", ::onOverviewDispatcher);
    Hyprdrive::Config::registerDispatcher("navigate", ::onNavigateDispatcher);
    Hyprdrive::Config::registerDispatcher("window", ::onWindowDispatcher);
    Hyprdrive::Config::registerDispatcher("minimize", ::onMinimizeDispatcher);
    Hyprdrive::Config::registerDispatcher("hotkey", ::onHotkeyDispatcher);
    Hyprdrive::Config::registerDispatcher("stack", ::onStackDispatcher);
    Hyprdrive::Config::registerDispatcher("gesture", ::onGestureDispatcher);
    Hyprdrive::Config::registerDispatcher("debug", ::onDebugDispatcher);
    // Super+D: the desktop's windows out of view and back (ShowDesktop.hpp).
    Hyprdrive::Config::registerDispatcher("show_desktop", Hyprdrive::ShowDesktop::dispatch);
    Hyprdrive::Config::registerGesture(::onRegisterOverviewGesture, ::overviewGestureKeyword);
    Hyprdrive::Config::registerConfig();
    // Its keys exist now: the overview's tile order (also on every reload).
    OvInternal::gridTileOrderReload();
    // The "mosaic" layout (Mosaic.hpp). Registered whether or not the panel has
    // enabled it: a desktop whose saved layout is mosaic must find it at login.
    Hyprdrive::Mosaic::registerLayout(HYPRDRIVE_HANDLE);
    Hyprdrive::Intent::init();
    Hyprdrive::GameApps::init();
    // Which app each window is (IdentityLive.hpp): its worker builds the
    // Steam and Lutris indexes and resolves every window off this thread.
    Hyprdrive::AppIdentity::init();
    // Each monitor's row of desktops (Omarchy's workspace blocks): before
    // anything renumbers.
    Hyprdrive::Rows::init();
    Hyprdrive::FullscreenDesktop::init();
    // Super+scroll's dispatcher and its wheel filter, and the desktop-change
    // animation switches (DesktopSwitch.hpp). Reads keys: after registerConfig().
    Hyprdrive::DesktopSwitch::init();
    Hyprdrive::XGeometry::installStateRequestFix();
    Hyprdrive::FullscreenGuard::installFocusHook();
    Hyprdrive::ShowDesktop::init();
    // Super+T on a fixed-size window: a tile at its own size (OwnSizeTile.hpp).
    Hyprdrive::OwnSizeTile::init();
    // Where every new window goes: the one openEarly decision (Placement.hpp).
    // After Intent, FullscreenDesktop and OwnSizeTile, which it calls.
    Hyprdrive::WindowPlacement::init();
    // Super+F / Super+W on the hovered overview tile or dock preview (HoverActions.hpp).
    Hyprdrive::HoverActions::init();
    // The animation:curve preset onto Hyprland's named house beziers -- only
    // now that the key exists (Config.hpp: an earlier read aborts Hyprland).
    Hyprdrive::Motion::sync();
    // A previous instance unloaded mid-overview can leave Hyprland in its submap.
    healStuckOverviewSubmap();
    // After a reload, windows are still parked on the stash with no ledger entry
    // behind them; without this they can never be restored.
    Hyprdrive::Minimize::adoptStashedWindows();
    registerRawKeyHooks();
    if (g_pSessionLockManager)
        g_pSessionLockHook = g_pSessionLockManager->m_events.lock.listen([] { onSessionLock(); });
    attachPadFingerListeners();
    // Both read their keys at once: only after registerConfig() above.
    Hyprdrive::FingerSwipe::init();
    Hyprdrive::SpringLoad::init();
    Hyprdrive::HotCorner::init();
    Hyprdrive::Ripple::init();
    Hyprdrive::FlightBlur::init();

    // Reload can drop live window alphas until decorations are recomputed.
    // Re-apply default-opacity (0.985 / 0.85) to already-mapped windows.
    if (Desktop::globalWindowController())
        Desktop::globalWindowController()->updateAllWindowsDecorations();

    // ONE RELEASE. Pinned dock apps are user data; they do not get to disappear
    // because the plugin changed its name. Both are one-shot and no-ops once
    // the config and the store have been migrated.
    Hyprdrive::Compat::adoptLegacyFavorites();
    Hyprdrive::Compat::adoptLegacyValues();

    if (g_pCompositor && g_pCompositor->m_wlEventLoop) {
        g_fileWakeFd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        if (g_fileWakeFd >= 0) {
            g_fileWakeSource = wl_event_loop_add_fd(g_pCompositor->m_wlEventLoop, g_fileWakeFd, WL_EVENT_READABLE, onFileResultsReady, nullptr);
            SearchBackend::setResultsCallback([]() {
                if (g_fileWakeFd >= 0) {
                    uint64_t one = 1;
                    ::write(g_fileWakeFd, &one, sizeof(one));
                }
            });
            // Stack folder reads and trash jobs land off-thread too; same wake.
            Hyprdrive::Thumbs::setReadyCallback([]() {
                if (g_fileWakeFd >= 0) {
                    uint64_t one = 1;
                    ::write(g_fileWakeFd, &one, sizeof(one));
                }
            });
            Hyprdrive::StackReader::setReadyCallback([]() {
                if (g_fileWakeFd >= 0) {
                    uint64_t one = 1;
                    ::write(g_fileWakeFd, &one, sizeof(one));
                }
            });
            // The dictionary/translator answers off-thread too; same wake.
            ToolSearch::setReadyCallback([]() {
                if (g_fileWakeFd >= 0) {
                    uint64_t one = 1;
                    ::write(g_fileWakeFd, &one, sizeof(one));
                }
            });
        }
    }

    // X51/X52. Build the icon index at LOAD, where a one-off ~10ms pass is
    // invisible, rather than letting the first search pay for it inside a frame
    // that is already trying to paint results.
    // Y13. ASYNC. Building this inline cost 4237ms of PARSE TIME, which is
    // time the compositor spends before exec-once and therefore before the
    // wallpaper can even be asked for.
    AppSearch::warmIconIndexAsync();
    // Decode icons ahead of the first open / first keystroke (OverviewRender.hpp).
    OverviewRender::startIconWarmer();

    // LAST: a std::terminate while the plugin is loaded leaves its exception
    // and a backtrace in $XDG_STATE_HOME/hyprdrive/terminate.log, then aborts
    // as before (TerminateLog.hpp). Not earlier: an init that throws is
    // dlclosed with no PLUGIN_EXIT, which would leave the handler in unmapped code.
    Hyprdrive::TerminateLog::install();

    return {Hyprdrive::Compat::NAME, "Mission-control overview + dock for Hyprland", "Vaxry, yayuuu, dragos", SCROLLOVERVIEW_VERSION};
}

APICALL EXPORT void PLUGIN_EXIT() {
    // Before any module's shutdown: no new worker starts from here on, every
    // worker is asked to stop, and the joins below share one deadline
    // (Workers.hpp: one that misses it is left running in code kept loaded).
    Hyprdrive::Workers::beginShutdown();
    // First: no window may stay hidden behind a plugin that is gone.
    Hyprdrive::ShowDesktop::shutdown();
    Hyprdrive::WindowPlacement::shutdown();
    Hyprdrive::OwnSizeTile::shutdown();
    // FIRST: Hyprland hands every mosaic desktop back to its default layout and
    // destroys our algorithm objects -- while their code is still mapped.
    Hyprdrive::Intent::shutdown();
    Hyprdrive::FullscreenDesktop::shutdown();
    Hyprdrive::Rows::shutdown(); // its inotify fd, timer and listeners
    Hyprdrive::AppIdentity::shutdown();
    Hyprdrive::GameApps::shutdown();
    Hyprdrive::Mosaic::shutdown();
    Hyprdrive::Mosaic::unregisterLayout(HYPRDRIVE_HANDLE);
    Hyprdrive::DesktopSwitch::shutdown();
    Hyprdrive::XGeometry::shutdown();
    Hyprdrive::KeyboardCat::shutdown(); // ffmpeg killed, its reader joined, before the code goes
    Hyprdrive::BorderFade::clear(); // drops its hold on Hyprland's `border` bezier
    stopSearchRepeat();
    if (g_searchRepeatTimer) {
        wl_event_source_remove(g_searchRepeatTimer);
        g_searchRepeatTimer = nullptr;
    }
    if (g_letterTickTimer) {
        wl_event_source_remove(g_letterTickTimer);
        g_letterTickTimer = nullptr;
    }
    SearchBackend::setResultsCallback(nullptr);
    ToolSearch::shutdown();
    // BEFORE the wake fd below is closed: the reader's worker writes to it when
    // a job lands, and a job landing after the close would write to a reused fd
    // (review 2026-09-24).
    Hyprdrive::Stacks::shutdown();
    Hyprdrive::StackReader::shutdown();
    Hyprdrive::Thumbs::shutdown(); // before the wake fd closes: its worker writes to it
    if (g_fileWakeSource) {
        wl_event_source_remove(g_fileWakeSource);
        g_fileWakeSource = nullptr;
    }
    // Not while a worker abandoned above may still write to it (Workers.hpp).
    if (g_fileWakeFd >= 0 && Hyprdrive::Workers::abandoned() == 0) {
        close(g_fileWakeFd);
        g_fileWakeFd = -1;
    }
    g_pRawKeyHook.reset();
    g_pSessionLockHook.reset();
    g_pTapSuperButtonHook.reset();
    g_pRenderPreHook.reset();
    CursorShape::shutdown();
    Hyprdrive::FramePacing::shutdown();
    OpenProf::shutdown();
    OvInternal::cancelPointerHandoff();
    g_pMembOpenHook.reset();
    g_pMembLateHook.reset();
    g_pMembCloseHook.reset();
    g_pMembKillHook.reset();
    g_pDockFocusHook.reset();
    g_pMembGoneHook.reset();
    g_pMembPinHook.reset();
    g_pMembFloatHook.reset();
    g_pMembTitleHook.reset();
    g_pMembClassHook.reset();
    g_pMembWsHook.reset();
    g_pMembFullHook.reset();
    g_pDockConfigHook.reset();
    g_pDockHoverHook.reset();
    g_pDockClickHook.reset();
    Hyprdrive::DockDrop::shutdown();
    g_pDockScrollHook.reset();
    g_pDockRenderHook.reset();
    g_pDockStandDownHook.reset();
    g_pDockFsWsHook.reset();
    g_pDockFsSpecialHook.reset();
    Hyprdrive::Dock::forgetFullscreenCovers();
    if (g_pEventLoopManager && g_dockStandDownSeq)
        g_pEventLoopManager->removeDoLater(g_dockStandDownSeq);
    g_dockStandDownSeq = 0;
    g_pScratchExitRenderHook.reset();
    g_pMonitorGoneHook.reset();
    g_pScrollCameraActiveHook.reset();
    g_pScrollCameraMovedHook.reset();
    // The config.preReload listener: HyprlandAPI::reloadConfig() at the end of
    // this function would otherwise still call into it (clearClickGestures,
    // the dock key fingerprint) with the plugin half torn down.
    g_pClickPreReloadHook.reset();

    g_unloading = true;
    setDockCursor(false);
    Hyprdrive::Dock::releasePointerFocus();
    // FIRST: a flight in the air at unload is a window that is hidden, stashed,
    // or mid-geometry with nothing left to finish it -- the plugin's code IS the
    // thing that would have landed it. hyprpm reloads this plugin in-process, so
    // "the session is ending anyway" is not true here.
    Hyprdrive::Flight::abortAll("plugin-exit");
    // The D-Bus worker outlives the overview otherwise, and hyprpm reloads the
    // plugin in-process.
    AppSearch::shutdownIconIndex();
    SearchProviders::shutdown();
    SearchBackend::shutdown();
    Hyprdrive::LauncherEntry::stop();
    clearScrollOverviews();
    // AGAIN, after the overviews are gone: their teardown hands focus back and
    // can queue work (a cursor warp, a flight, a dock-seat refresh) after the
    // first round of cancels above -- a doLater left in Hyprland's event loop
    // is a call into unloaded code when that loop is destroyed.
    OvInternal::cancelPointerHandoff();
    Hyprdrive::Flight::abortAll("plugin-exit");
    // A flying window may hold the flight's blur transformer; its destructor is
    // this plugin's code, so it goes before the dlclose (Hyprland's own exit too).
    Hyprdrive::FlightBlur::shutdown();
    Hyprdrive::FlightBlur::detachAll();
    OverviewRender::stopIconWarmer();
    // Nothing may stay held once the hook goes with the plugin.
    Hyprdrive::ConfigureHold::flush();
    Hyprdrive::DockRoom::shutdown();
    Hyprdrive::Dock::releasePointerFocus();
    // Outlives every overview on purpose (Window.cpp), so it is freed here.
    if (const auto OPENGL = g_pHyprRenderer ? g_pHyprRenderer->glBackend() : WP<Render::GL::CHyprOpenGLImpl>{})
        OPENGL->makeEGLCurrent();
    OverviewWindow::releaseTexturePrefilterCache();
    Hyprdrive::releaseWallpaperStash(std::nullopt);
    // The text textures outlive a close now (X51), so unload is where they go --
    // same moment the last overview's destructor used to drop them, and hyprpm
    // reloads this plugin in-process.
    OverviewRender::clearTextCache();
    // The drag-out zone's drift timer and stripe textures: the timer would fire
    // into unloaded code, and the textures need the context made current above.
    Hyprdrive::ScratchpadExit::shutdown();
    shutdownClickGestures();
    // Get Info: its poll timer would fire into unloaded code, and its worker
    // must be joined (with any package query killed) before the code goes.
    Hyprdrive::Dock::stopInfoPoll();
    Hyprdrive::Dock::stopUrgentHopTimer();
    AppInfo::shutdown();
    disableScrollOverviewHooks();

    // Before the gesture teardown below: a swipe the plugin is driving ends
    // through it, and the spring timer must not fire into unloaded code.
    Hyprdrive::SpringLoad::shutdown();
    Hyprdrive::HotCorner::shutdown();
    Hyprdrive::FingerSwipe::shutdown();

    if (g_pTrackpadGestures)
    {
        // The swipe in progress first: clearGestures() does not touch it, and it
        // may be one of ours (session abort mid-swipe, 2026-09-29).
        Hyprdrive::endActiveTrackpadGesture();
        g_pTrackpadGestures->clearGestures();
    }

    // LAST, and it is the difference between unloading and killing the session.
    //
    // The types below are defined IN THIS SHARED OBJECT (OverviewPassElement.hpp, DockTilt.hpp).
    // A queued one that outlives the dlclose leaves Hyprland's render pass holding a
    // vtable in freed memory, and the next frame's CRenderPass::clear() runs its
    // destructor through that vtable. `hyprctl plugin unload` SEGV'd the compositor
    // twice on 2026-09-12 doing exactly this -- both cores identical, and both name
    // it outright: beginRender -> Render::CRenderPass::clear() -> a call to an
    // address in no mapped region, with hyprdrive.so already gone from the map.
    //
    // Hyprland cannot know these types, so the plugin has to take them back out
    // before its code goes away. The trackpad gestures above are the same hazard,
    // and clearGestures() is what already answers it for them.
    if (g_pHyprRenderer) {
        for (const char* TYPE : {"COverviewBlurPassElement", "COverviewSplitBorderPassElement", "COverviewShadowPassElement", "CDockLivePreviewPassElement",
                                 "CDockTiltPassElement", "CStackBlurPassElement", Hyprdrive::Ripple::PASS_NAME})
            g_pHyprRenderer->m_renderPass.removeAllOfType(TYPE);
    }

    // After the purge: nothing queued can reach the tilt shader any more, and
    // its GL program has to go while this object's code is still mapped.
    Hyprdrive::DockTilt::shutdown();
    Hyprdrive::StackBlur::shutdown();
    Hyprdrive::Ripple::shutdown();

    // Whatever no module joined (Empty Trash), within the same deadline. No
    // thread of ours may outlive this function in code about to be unmapped.
    Hyprdrive::Workers::finishShutdown();

    HyprlandAPI::reloadConfig(); // re-adds built-in gestures cleared above

    // The very last thing: a terminate anywhere in the exit above is still
    // logged. The previous handler goes back unless another was installed
    // over ours meanwhile, which is then left as it is (and said so).
    if (!Hyprdrive::TerminateLog::uninstall())
        Log::logger->log(Log::ERR, "[hyprdrive] another std::terminate handler was installed over hyprdrive's: left in place, and the plugin's code kept loaded for it");
}
