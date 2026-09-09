// Z19's rule, same as Dock.cpp: the private=public block covers ONLY the
// hyprutils header whose guard we claim here, so the std headers go in clean
// first. This TU reads m_realPosition / m_realSize off a live window.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "WindowPill.hpp"

#include "Config.hpp"
#include "Dock.hpp"
#include "IOverview.hpp"
#include "Minimize.hpp"
#include "OverviewRender.hpp"
#include "globals.hpp"

#include <hyprland/src/helpers/time/Time.hpp>

#define private public
#define protected public
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/config/shared/actions/ConfigActions.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/pass/RectPassElement.hpp>
#include <hyprland/src/render/pass/TexPassElement.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#undef protected
#undef private

#include <linux/input-event-codes.h>

namespace ScrollOverview::WindowPill {

namespace {

// ---------------------------------------------------------------- geometry --
//
// THE STRADDLE, in two numbers, and they are the whole of the user's sentence:
// "not entirely over the window... but not entirely outside of it".
//
//   STRADDLE_Y = 0.5   the capsule's own horizontal centre line lies ON the
//                      window's top edge, so exactly half of its height hangs
//                      above the window and half sits over it. Vertically it is
//                      cut in two by the edge -- that is the straddle.
//   OVERHANG_X = 0.5   the capsule's RIGHT edge sits half a button past the
//                      window's right edge. A fraction of a BUTTON rather than
//                      of the capsule on purpose: the capsule is 112 px wide and
//                      centring it on the corner would leave 56 px of it out in
//                      the wallpaper, which is a pill NEXT TO the window rather
//                      than one on its corner. Half a button is the same
//                      quantity the vertical overhang is measured in (half of
//                      the thing the finger aims at) and it reads as a corner
//                      badge on a 400 px window and on a 2000 px one alike.
//
// So the capsule overhangs on BOTH axes -- it is off the window at the top and
// off it on the right -- and is over the window everywhere else. X55 made the
// same call for the overview's close-X ("ON the corner, not inside it") after
// the inset version was rejected; this is that decision at pill scale.
//
// Everything is in LOGICAL px and multiplied by the monitor scale at use, like
// every other constant in this rice.
constexpr float BTN        = 26.F;  // button diameter
constexpr float BTN_GAP    = 8.F;   // between two buttons
constexpr float PAD_X      = 9.F;   // capsule padding, left and right
constexpr float PAD_Y      = 7.F;   // capsule padding, top and bottom
constexpr float STRADDLE_Y = 0.5F;
constexpr float OVERHANG_X = 0.5F;

// The glyph inside a button, as a fraction of it. X55 measured GNOME's
// `.window-close`: StIcon 24 in a 32 box, 0.75.
constexpr float GLYPH      = 0.75F;

// Slop around a button's hit area. HALF the gap between two buttons, so the
// three targets grow until they touch and never overlap -- an overlap would
// make the order they are tested in decide which button you pressed.
constexpr float HIT_SLOP   = BTN_GAP / 2.F;

// ---------------------------------------------------------------- motion ----
//
// V2's own gate is the ceiling here: "pill appears <=150ms after SUPER down".
// That is FASTER than the rice's 200-400ms band, and deliberately so -- this is
// a modifier readout, not a transition, and a modifier readout that arrives a
// third of a second after the modifier reads as lag. The band's shape is kept:
// the entrance decelerates (houseEase decel), the exit accelerates and runs 25%
// shorter, and neither overshoots. Recorded as a spec-mandated deviation rather
// than as a house-rule exemption, because the number comes from the user's own
// acceptance criterion in the same document.
constexpr float IN_MS  = 140.F;
constexpr float OUT_MS = 105.F;

// A second press on the same window inside this, without the pointer wandering,
// is a double-click. GTK's default double-click time is 400ms and Hyprland has
// no setting of its own to read; the radius is the same "did the pointer stay
// put" test the overview's click recovery makes, at tap scale.
constexpr float DOUBLE_MS   = 400.F;
constexpr float DOUBLE_SLOP = 14.F;

// The three glyphs, left to right. Adwaita ships all three as symbolic artwork
// and the icon cache resolves them the same way the close-X already is.
constexpr const char* ICON_MINIMIZE = "window-minimize-symbolic";
constexpr const char* ICON_MAXIMIZE = "window-maximize-symbolic";
constexpr const char* ICON_CLOSE    = "window-close-symbolic";

// ----------------------------------------------------------------- state ----
bool            g_super = false;

struct SReveal {
    float           from  = 0.F;
    float           to    = 0.F;
    float           ms    = 1.F;
    Time::steady_tp start = {};
};

SReveal g_reveal;

// The press the pill is holding: WHICH button of WHICH window went down. The
// release only acts if it comes back up on the same one -- a press dragged off
// its button is a cancel, which is what every button on every desktop does.
PHLWINDOWREF g_pressWindow;
EButton      g_pressButton = EButton::None;

// The last unclaimed primary press over a window, for the double-click.
PHLWINDOWREF    g_lastClickWindow;
Vector2D        g_lastClickAt;
Time::steady_tp g_lastClickWhen = {};

// A press we swallowed owes its release: letting the release through after
// eating the press is how a pointer ends up stuck (the same rule DockInput
// states for the dock pill -- both edges, or neither).
bool g_swallowRelease = false;

float ease() {
    const float ELAPSED = Config::animElapsedMs(g_reveal.start);
    if (ELAPSED >= g_reveal.ms)
        return g_reveal.to;

    const float T = std::clamp(ELAPSED / std::max(1.F, g_reveal.ms), 0.F, 1.F);
    return g_reveal.from + (g_reveal.to - g_reveal.from) * houseEase(g_reveal.to > g_reveal.from, T);
}

// The window's rect as it sits on screen, in the monitor-local RENDER px
// everything here works in. Same conversion FlightController::windowRenderBox
// makes; not shared with it because that one lives behind the flight's own
// private-access block and this TU is not the flight.
CBox frameOf(PHLMONITOR monitor, const PHLWINDOW& window) {
    if (!monitor || !window)
        return {};

    const auto  POS   = window->m_realPosition->value();
    const auto  SIZE  = window->m_realSize->value();
    const float SCALE = std::max(monitor->m_scale, 0.01F);

    return CBox{(POS.x - monitor->m_position.x) * SCALE, (POS.y - monitor->m_position.y) * SCALE, SIZE.x * SCALE, SIZE.y * SCALE};
}

// Every window that gets a pill on the plain desktop, BOTTOM OF THE STACK
// FIRST -- Desktop::windowState() keeps its list in z-order with the topmost
// last (CWindowState::moveToTop rotates to the back). Draw in this order and
// hit-test in reverse, so the pill you can click is the pill drawn on top.
std::vector<PHLWINDOW> desktopWindows(PHLMONITOR monitor) {
    std::vector<PHLWINDOW> out;
    if (!monitor)
        return out;

    const auto WORKSPACE = monitor->m_activeWorkspace;
    if (!WORKSPACE)
        return out;

    for (const auto& WINDOW : Desktop::windowState()->windows()) {
        if (!WINDOW || !WINDOW->m_isMapped || WINDOW->isHidden())
            continue;
        if (WINDOW->m_workspace != WORKSPACE)
            continue;
        // A stashed window lives on the minimize workspace, which is nobody's
        // active one -- but Y11 forces m_hidden false on one being previewed, so
        // ask the ledger rather than trusting the flags.
        if (Minimize::isMinimized(WINDOW))
            continue;
        // A window BEHIND a fullscreen one is still mapped and still not
        // hidden: the compositor buries it by driving its fullscreen alpha to
        // zero, which is a property of how it is DRAWN and not of the flags
        // this loop reads. Without this the pills of every window on the
        // desktop float over the fullscreen window -- chrome for windows the
        // user cannot see, on top of the one thing they asked to see alone.
        if (WINDOW->isBlockedByFullscreen())
            continue;

        const auto BOX = frameOf(monitor, WINDOW);
        if (BOX.w < 1 || BOX.h < 1)
            continue;

        out.push_back(WINDOW);
    }

    return out;
}

// The centre of button `i` (0..2) inside a laid-out capsule.
Vector2D buttonCentre(const CBox& pill, float scale, int i) {
    const float S = std::max(scale, 0.01F);
    return Vector2D{pill.x + (PAD_X + BTN / 2.F + sc<float>(i) * (BTN + BTN_GAP)) * S, pill.y + pill.h / 2.0};
}

void forgetPress() {
    g_pressWindow.reset();
    g_pressButton    = EButton::None;
    g_swallowRelease = false;
}

}

// ------------------------------------------------------------------ state ----

void setSuperHeld(bool held) {
    if (g_super == held)
        return;

    g_super = held;

    // From WHEREVER it is now, so tapping SUPER twice in quick succession does
    // not restart the pill from nothing -- the same retarget every ease in this
    // codebase makes.
    g_reveal.from  = ease();
    g_reveal.to    = held ? 1.F : 0.F;
    g_reveal.ms    = held ? IN_MS : OUT_MS;
    g_reveal.start = Time::steadyNow();

    if (!held)
        forgetPress();

    // ASK FOR THE FIRST FRAME. SUPER arrives with no pointer event attached, on
    // a desktop where nothing else is repainting -- and inside an overview whose
    // scheduleFrame hook throttles plain damage, so there it has to be asked the
    // overview's way. Dock::needsFrames sustains the chain from here.
    for (const auto& MONITOR : State::monitorState()->monitors()) {
        if (!MONITOR)
            continue;

        if (const auto OVERVIEW = scrollOverviewForMonitor(MONITOR)) {
            OVERVIEW->damage();
            OVERVIEW->requestInputFrame();
            continue;
        }

        g_pHyprRenderer->damageMonitor(MONITOR);
    }
}

bool superHeld() {
    return g_super;
}

float progress() {
    return std::clamp(ease(), 0.F, 1.F);
}

bool animating() {
    return Config::animElapsedMs(g_reveal.start) < g_reveal.ms;
}

// --------------------------------------------------------------- geometry ----

CBox box(PHLMONITOR monitor, const CBox& frame, float scale) {
    if (frame.w < 1 || frame.h < 1)
        return {};

    const float S = std::max(scale, 0.01F);
    const float W = (3.F * BTN + 2.F * BTN_GAP + 2.F * PAD_X) * S;
    const float H = (BTN + 2.F * PAD_Y) * S;

    double x = frame.x + frame.w - W + BTN * OVERHANG_X * S;
    double y = frame.y - H * STRADDLE_Y;

    // A window against the top of the screen would put half its pill off the
    // output, where it is neither visible nor clickable. Clamped HERE, in the
    // one function both the draw and the hit test call, so they cannot end up
    // disagreeing about where the pill actually is.
    if (monitor) {
        const double MW = monitor->m_size.x * S;
        const double MH = monitor->m_size.y * S;
        x = std::clamp(x, 0.0, std::max(0.0, MW - W));
        y = std::clamp(y, 0.0, std::max(0.0, MH - H));
    }

    return CBox{x, y, W, H};
}

EButton buttonAt(PHLMONITOR monitor, const CBox& frame, float scale, const Vector2D& point) {
    const auto PILL = box(monitor, frame, scale);
    if (PILL.empty())
        return EButton::None;

    const float S    = std::max(scale, 0.01F);
    const float HALF = BTN * S / 2.F + HIT_SLOP * S;

    for (int i = 0; i < 3; ++i) {
        const auto C = buttonCentre(PILL, S, i);
        if (CBox{C.x - HALF, C.y - HALF, HALF * 2.F, HALF * 2.F}.containsPoint(point))
            return sc<EButton>(sc<uint8_t>(EButton::Minimize) + i);
    }

    return EButton::None;
}

// ----------------------------------------------------------------- render ----

void render(PHLMONITOR monitor, const CBox& frame, float scale, float alpha) {
    const float A = std::clamp(alpha, 0.F, 1.F) * progress();
    if (A <= 0.01F)
        return;

    const auto PILL = box(monitor, frame, scale);
    if (PILL.empty())
        return;

    const float S     = std::max(scale, 0.01F);
    const int   ROUND = sc<int>(std::round(PILL.h / 2.0));

    // THE SAME GLASS THE DOCK WEARS, through the dock's own call: live
    // dual-kawase of whatever is behind it, inheriting decoration:blur:* through
    // the -1 sentinels (X71) unless the dock keys override them. Frost is on by
    // default on every surface in this rice, and a second frost implementation
    // is a second thing to re-theme.
    Dock::frostedPlate(monitor, PILL, ROUND, A);

    // The BUTTONS are X55's close-X three times over, and the treatment is
    // copied rather than improved on for a reason it already records: the
    // symbolic glyphs ship as near-black artwork, this render path cannot
    // recolour a texture, and the plate's hue is whatever the theme made it
    // (matugen fills dock:background_color, which is a LIGHT grey by default and
    // a dark one under some wallpapers). A light disc under a dark glyph is the
    // one combination that stays legible whichever way the theme goes.
    const char* const ICONS[3] = {ICON_MINIMIZE, ICON_MAXIMIZE, ICON_CLOSE};

    for (int i = 0; i < 3; ++i) {
        const auto  C = buttonCentre(PILL, S, i);
        const float D = BTN * S;

        CRectPassElement::SRectData disc;
        disc.box           = CBox{C.x - D / 2.0, C.y - D / 2.0, D, D};
        disc.color         = CHyprColor{0.93F, 0.93F, 0.93F, 0.95F * A};
        disc.round         = sc<int>(std::round(D / 2.0));
        disc.roundingPower = 2.F;
        g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(disc));

        if (const auto TEX = OverviewRender::appIcon(ICONS[i])) {
            const float SIDE = D * GLYPH;

            CTexPassElement::SRenderData glyph;
            glyph.tex = TEX;
            glyph.box = CBox{C.x - SIDE / 2.0, C.y - SIDE / 2.0, SIDE, SIDE};
            glyph.box.round();
            glyph.a = A;
            g_pHyprRenderer->m_renderPass.add(makeUnique<CTexPassElement>(glyph));
        }
    }
}

void renderDesktop(PHLMONITOR monitor) {
    if (!monitor || progress() <= 0.01F)
        return;

    for (const auto& WINDOW : desktopWindows(monitor))
        render(monitor, frameOf(monitor, WINDOW), monitor->m_scale);
}

// ----------------------------------------------------------------- actions ---

void activate(PHLMONITOR monitor, const PHLWINDOW& window, EButton button) {
    if (!window)
        return;

    switch (button) {
        case EButton::Minimize:
            // X54's law: everything that minimizes goes through the dock, or it
            // teleports instead of flying. The dock is the target, so the dock
            // owns the animation. No bounce -- Y8 hangs that off a DROP, and a
            // button press is its own acknowledgement.
            Dock::minimizeWithFlight(monitor ? monitor : window->m_monitor.lock(), window);
            return;

        case EButton::Maximize:
            // TOGGLE, and FSMODE_MAXIMIZED rather than FSMODE_FULLSCREEN: the
            // glyph is `window-maximize-symbolic` and the user's word is
            // "maximize", which on every desktop this rice imitates means "fill
            // the work area, keep the bar and the dock" -- not "take the whole
            // output and hide everything". layoutAware, so a tiled window's
            // layout gets to handle it the way `fullscreenstate` does.
            // (void): the action reports a config-level error the compositor's
            // own dispatchers surface to the user, and there is no user here to
            // surface it to -- a button press that the layout refuses simply
            // leaves the window as it was. Same treatment setSubmap gets.
            (void)::Config::Actions::fullscreenWindow(::Fullscreen::controller()->isFullscreen(window, ::Fullscreen::FSMODE_MAXIMIZED) ? ::Fullscreen::FSMODE_NONE :
                                                                                                                                        ::Fullscreen::FSMODE_MAXIMIZED,
                                                      true, window);
            return;

        case EButton::Close:
            // GRACEFUL: the client is asked, not killed. Same call the overview's
            // close-X makes.
            window->sendClose();
            return;

        case EButton::None: return;
    }
}

// ---------------------------------------------------------------- desktop ----

DockInput::SClaim handleDesktop(PHLMONITOR monitor, const Vector2D& local, uint32_t button, bool pressed) {
    DockInput::SClaim out;

    if (button != BTN_LEFT)
        return out;

    // A release owed from a press we ate. Ahead of the SUPER test on purpose:
    // SUPER can come up between the two edges, and a release let through after a
    // swallowed press is exactly the stuck-pointer case DockInput warns about.
    if (!pressed && g_swallowRelease) {
        const auto WINDOW = g_pressWindow.lock();
        const auto BTN    = g_pressButton;
        forgetPress();

        // The release ACTS, not the press: a button that fires on the way down
        // cannot be aborted by sliding off it, and every button on every desktop
        // can be. Only if it comes back up on the same button of the same window.
        if (WINDOW && BTN != EButton::None && buttonAt(monitor, frameOf(monitor, WINDOW), monitor ? monitor->m_scale : 1.F, local) == BTN)
            activate(monitor, WINDOW, BTN);

        out.claimed = true;
        return out;
    }

    if (!g_super || !monitor)
        return out;

    if (!pressed)
        return out;

    // TOPMOST FIRST: desktopWindows() hands the stack back bottom-up, and the
    // pill that answers a click has to be the pill that is drawn on top.
    const auto WINDOWS = desktopWindows(monitor);
    const float SCALE  = monitor->m_scale;

    for (auto it = WINDOWS.rbegin(); it != WINDOWS.rend(); ++it) {
        const auto BUTTON = buttonAt(monitor, frameOf(monitor, *it), SCALE, local);
        if (BUTTON == EButton::None)
            continue;

        g_pressWindow    = *it;
        g_pressButton    = BUTTON;
        g_swallowRelease = true;
        out.claimed      = true;
        return out;
    }

    // NOT A BUTTON, SO NOT OURS. This is the load-bearing non-claim: the press
    // falls through to CInputManager, which is where `SUPER + mouse:272 ->
    // window.drag` is dispatched, and dragging a window with SUPER held has to
    // keep working with the pill on screen. It covers the capsule between the
    // buttons too -- V2: "the pill never steals a click that misses its buttons".

    // ...but a SECOND press on the same window, soon enough and near enough, is
    // a double-click, and V2 gives that its own meaning: toggle the window
    // between floating and tiled. Acted on the PRESS, the way every double-click
    // is, and BOTH its edges are then eaten so the gesture cannot also start a
    // drag of the window it has just re-laid-out.
    PHLWINDOW under;
    for (auto it = WINDOWS.rbegin(); it != WINDOWS.rend(); ++it) {
        if (frameOf(monitor, *it).containsPoint(local)) {
            under = *it;
            break;
        }
    }

    if (!under) {
        g_lastClickWindow.reset();
        return out;
    }

    const bool DOUBLE = g_lastClickWindow.lock() == under && Config::realElapsedMs(g_lastClickWhen) <= DOUBLE_MS &&
        g_lastClickAt.distanceSq(local) <= sc<double>(DOUBLE_SLOP * SCALE) * sc<double>(DOUBLE_SLOP * SCALE);

    if (DOUBLE) {
        g_lastClickWindow.reset();
        (void)::Config::Actions::floatWindow(::Config::Actions::TOGGLE_ACTION_TOGGLE, under);
        g_swallowRelease = true; // both edges, or neither
        out.claimed      = true;
        return out;
    }

    g_lastClickWindow = under;
    g_lastClickAt     = local;
    g_lastClickWhen   = Time::steadyNow();
    return out;
}

}
