// Z19: this block must be FIRST. m_Value is private in hyprutils, and the
// flight drives the RENDERED box only -- setValueAndWarp moves the GOAL, and
// goal changes configure the client: a ~60-configure storm per flight, whose
// late acks from a slow client (Zen) land after the flight and clobber the
// geometry. The std headers are included clean first so private=public covers
// ONLY the hyprutils header whose guard we claim here; every later inclusion
// is a no-op.
#include <sstream>
#include <functional>
#include <memory>
#include <chrono>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>
#include <any>
#define private public
#define protected public
#include <hyprutils/animation/AnimatedVariable.hpp>
#undef protected
#undef private

#include <hyprland/src/desktop/state/GlobalWindowController.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>

#include "Dock.hpp"

#include "Compat.hpp"

#include "AppSearch.hpp"
#include "globals.hpp"
#include "Config.hpp"
#include "DropDecision.hpp" // V3: where a released icon drag goes, as a pure function
#include "IOverview.hpp"
#include "LauncherEntry.hpp"
#include "StackReader.hpp"
#include "TextInput.hpp"
#include "DockStacks.hpp"
#include "DockTilt.hpp"
#include "Locations.hpp"
#include "Minimize.hpp"
#include "OverviewPassElement.hpp"
#include "OverviewRender.hpp"
#include "Window.hpp"
#include "WindowPill.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <ranges>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#define private public
#define protected public
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/history/WindowHistoryTracker.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/view/WLSurface.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/protocols/core/Compositor.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/pass/BorderPassElement.hpp>
#include <hyprland/src/render/pass/RectPassElement.hpp>
#include <hyprland/src/render/pass/TexPassElement.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/state/WorkspaceState.hpp>
#undef protected
#undef private

// Z16: the flight proxy carries the same card shadow the overview draws. Comes
// AFTER the protected=public block above: this header pulls Renderer/Monitor/
// Window, and if it landed before the block those classes would be defined with
// protected intact and Dock could not reach m_realPosition/m_realSize. These
// helpers used to force the whole ~1900-line OverviewInternal.hpp (and with it
// Dock.hpp, an include knot) into this TU; they now live in their own header.
#include "OverviewChrome.hpp"

// V6: the ONE answer to "which desktop does this window live on". A stashed
// window's own m_workspace is the minimize stash, so only this knows it belongs
// to the desktop it will come back to (GridLayout.hpp, 46bccac). Same include
// rules as OverviewChrome above; scrollOverview.hpp is already in this TU.
#include "GridLayout.hpp"

// The dock's FLIGHT subsystem -- the whole Z4-Z20 saga -- is its own module now:
// an explicit state machine with named phases instead of five sentinel fields.
// Included AFTER the private-access blocks, like OverviewChrome, because the
// flight code needs the same m_Value / m_realPosition access this TU has.
#include "FlightController.hpp"
#include "Scratchpad.hpp"
#include "AppInfo.hpp"
#include <cstring>
#include <hyprutils/os/Process.hpp>
#include <unistd.h>
#include <csignal>
#include "Placement.hpp"

namespace ScrollOverview::Dock {

namespace {

// OVERVIEW_CPP_SPEC §13.9 X27. Every proportion below is READ off the reference
// implementation -- the user's own Dash to Dock, `#dashtodockContainer.bottom`
// in its stylesheet.css -- not eyeballed off a screenshot:
//
//   .dash-background          margin-bottom 4, border-radius 24
//   #dashtodockDashContainer  padding 10 (left/right; top and bottom are 0)
//   .dash-item-container      margin 0 2
//   .app-well-app             padding-top 10, padding-bottom 14
//
// The app-well-app's 2px HORIZONTAL padding is deliberately not counted: D2D
// wraps one app per .dash-item-container, so the `:first-child`/`:last-child`
// rules that zero padding-left/right match every item, and the 2px container
// margin is what actually spaces the icons ("we handle them here", per the
// stylesheet's own comment).
//
// (user, 2026-08-03): D2D's 10/14 split is NOT symmetric -- it reserves the extra
// 4px at the bottom for the running dot -- and the icons read as sitting high in
// the pill. Split the same 24px evenly instead, so the icon is equidistant from
// the pill's top and bottom. The total is unchanged, so every X27 proportion
// still measures the same; only the icon's y moves. X31's dot overlays the
// bottom padding when it lands rather than owning it.
// X53. Icon spacing, measured off the host's dash-to-dock -- with ONE
// deliberate divergence, requested directly: the icon sits the SAME distance
// from every pill edge.
//
// The host does NOT do this. Measured off the user's recording, its icon is 18
// from the top, 20 from the bottom and 16 from the sides, because its item
// padding is 10 left / 12 top / 14 bottom and the extra band at the bottom is
// room for the running dot. The user looked at that, correctly saw the icon
// sitting low, and asked for 18 on all four edges instead. So:
//
//   KEPT from the host   icon 54, halo 6 (highlight 66), icon gap 16,
//                        and therefore 4px between adjacent highlights
//   DIVERGES             icon-to-pill is ICON_PAD on every edge, not 10/12/14
//
// Do not "restore parity" here: the asymmetry is the thing that was rejected.
constexpr float HOVER_PAD = 6.F;   // the halo the highlight adds around an icon
constexpr float ICON_GAP  = 16.F;  // icon edge to icon edge, at rest (host's)
constexpr float ICON_PAD  = 14.F;  // icon to pill, EVERY edge (the divergence)

// Derived, so the three numbers above stay the only things anyone edits.
constexpr float HIGHLIGHT_GAP   = ICON_GAP - 2.F * HOVER_PAD;
constexpr float HL_PAD          = ICON_PAD - HOVER_PAD; // highlight to pill, every edge
constexpr float ITEM_MARGIN_X   = ICON_GAP / 2.F;
constexpr float CONTAINER_PAD_X = ICON_PAD - ITEM_MARGIN_X;
constexpr float ITEM_PAD_TOP    = ICON_PAD;
constexpr float ITEM_PAD_BOTTOM = ICON_PAD;
constexpr float EDGE_MARGIN     = 4.F;

// The rule as arithmetic, so a build that breaks it cannot compile.
static_assert(HIGHLIGHT_GAP > 0.F, "X53: a hover highlight must not reach into the icon next to it");
static_assert(2.F * ITEM_MARGIN_X == ICON_GAP, "X53: icon edge to icon edge must be ICON_GAP");
static_assert(CONTAINER_PAD_X + ITEM_MARGIN_X == ICON_PAD, "X53: the outer icons must sit ICON_PAD from the pill's sides");
static_assert(ITEM_PAD_TOP == ICON_PAD && ITEM_PAD_BOTTOM == ICON_PAD, "X53: the icon must sit ICON_PAD from the pill's top and bottom");
static_assert(ITEM_PAD_TOP == ITEM_PAD_BOTTOM, "X53: equidistant top and bottom -- this is the divergence the user asked for");
static_assert(CONTAINER_PAD_X + ITEM_MARGIN_X - HOVER_PAD == HL_PAD, "X53: the highlight therefore clears every pill edge by HL_PAD");

// X25. A preview is as TALL as an icon -- it shares the icon row -- and as wide
// as the window's own aspect makes it. The clamp is what stops one ultrawide or
// one tall-and-thin window from owning the pill; it is not a look, it is a
// bound, which is why it is a constant and not a config key.
constexpr float PREVIEW_MIN_ASPECT = 0.60F;
constexpr float PREVIEW_MAX_ASPECT = 2.20F;

// (user, 2026-08-03) the tooltip: a dark plate above the dock carrying the
// app's name. How it LOOKS is not decided here any more -- X57 established that
// the host styles its dash label and its overview window caption with one css
// rule, so the plate is drawn by the shared OverviewRender::labelPill and the
// font, padding and background alpha live there, in one copy. This file had a
// second set of all four with the same values; they were dead once the pill
// landed and are gone.
//
// How far above the pill the plate floats is OverviewRender::LABEL_PILL_GAP,
// shared with the Scratchpad label in the workspace strip.

// X28. The app-icon menu. Same font as the tooltip -- it is the same chrome,
// one plate up -- with GNOME's roomier row padding.
constexpr float  MENU_FONT      = 13.F;
constexpr float  MENU_PAD_X     = 14.F;
constexpr float  MENU_ROW_PAD_Y = 7.F;
constexpr float  MENU_RADIUS    = 10.F;
constexpr float  MENU_GAP       = 8.F;
constexpr float  MENU_SEP_H     = 9.F;
constexpr float  MENU_MIN_W     = 170.F;
constexpr size_t MENU_LABEL_MAX = 44; // bytes; window titles are arbitrarily long

// OVERVIEW_CPP_SPEC 13.12 X47. The hover preview panel. Windows' taskbar for
// placement and trigger, this rice for looks: the same frost, the same plate and
// the same house curves the X28 menu already uses, because it is the same class
// of floating chrome one gap above the pill.
constexpr float  PANEL_GAP      = 8.F;   // above the pill, matching MENU_GAP
constexpr float  PANEL_PAD      = 10.F;
constexpr float  PANEL_CELL_H   = 132.F; // logical px, the preview row's height
constexpr float  PANEL_CELL_GAP = 10.F;
constexpr float  PANEL_RADIUS   = 12.F;
constexpr float  PANEL_IN_MS    = 170.F;
constexpr float  PANEL_OUT_MS   = 130.F; // exits ~25% quicker, house rule
constexpr float  PANEL_SLIDE_MS = 160.F; // travel from one icon to the next
// A short hold before the dismissal starts, so crossing the gap between two
// icons does not flicker the panel away and back.
constexpr float  PANEL_GRACE_MS = 180.F;
// ponytail: past this the cells are too narrow to read anyway. Scroll or a
// second row if an app with a dozen windows ever needs them all.
constexpr size_t PANEL_MAX      = 6;

// X31 RUNNING INDICATOR. `running-indicator-style` is DEFAULT in the reference,
// and DEFAULT is not a Dash to Dock drawing at all: appIconIndicators.js's
// RunningIndicatorDefault only adds a css class and leaves GNOME SHELL's own dot
// showing -- `_updateDefaultDot` shows `source._dot`, and only the DOTS style
// ever calls `_hideDefaultDot`. So the shape to match is the SHELL's, read out
// of gnome-shell-dark.css and D2D's `.bottom` override:
//
//   .app-grid-running-dot   height 5, width 5, border-radius 5, #fafafb
//   #dashtodockContainer.bottom ... .app-grid-running-dot   margin-bottom 2
//
// ONE dot, not one per window. The shell's `_dot` is a single actor, and D2D's
// `running1..running4` classes are css hooks the default theme never styles.
// (RICE_PROGRESS.md's "DEFAULT (dots)" is a plural the source does not support.)
// Confirmed against the recording: every dot on the host measures exactly 5px
// wide, and no icon carries two.
//
// The gap between the ICON BOX and the dot, MEASURED, not the css margin-bottom
// above -- that one is the space under the dot. On the host the dot's first
// (antialiased) row lands 2px past the icon's last row, i.e. 1px past the icon
// box, and 2 here put it a pixel low.
constexpr float DOT_MARGIN_B = 1.F;

// X31 NOTIFICATION COUNTER. `.notification-badge` in the reference's own
// stylesheet.css: white on red, padding 0.2em 0.5em, border-radius 1em,
// font-weight bold, margin 2px -- pinned to the icon's TOP-RIGHT, which is the
// St.Bin xAlign END / yAlign START in `_updateNotificationsBadge`.
constexpr float BADGE_PAD_X_EM = 0.5F;
constexpr float BADGE_PAD_Y_EM = 0.2F;
constexpr float BADGE_MARGIN   = 2.F;
constexpr int   BADGE_WEIGHT   = 700; // `font-weight: bold`

// `_updateNotificationBadgeStyle`, followed verbatim rather than converted: the
// host's `font-name` is "Ubuntu 11", points not absolute, so 11 * 0.9 / 0.75,
// then scaled by iconSize / 48. 48 is the SCHEMA default of `dash-max-icon-size`
// -- which is what `get_default_value` reads -- and NOT the user's effective 54.
constexpr float BADGE_FONT_PT      = 11.F;
constexpr float BADGE_FONT_RATIO   = 0.9F;
constexpr float BADGE_DEFAULT_ICON = 48.F;

// X31 URGENT, `dance-urgent-applications` true. The reference's IconAnimator
// (docking.js:2612-2635) runs ONE 3000ms timeline on repeat and sets
// rotation_angle_z = progress < 1/6 ? 15 * sin(progress * 24 * PI) : 0. So the
// icon wiggles for the first 500ms of every 3s and is still for the other 2500,
// two full sine periods per burst (24*PI * 1/6 = 4*PI). One shared clock, so
// two urgent apps wiggle together instead of each on its own phase.
constexpr float WIGGLE_PERIOD_MS = 3000.F;
constexpr float WIGGLE_FRACTION  = 1.F / 6.F;
constexpr float WIGGLE_DEGREES   = 15.F;

constexpr const char* KEY_ENABLED    = "plugin:hyprdrive:dock:enabled";
constexpr const char* KEY_ICON_SIZE  = "plugin:hyprdrive:dock:icon_size";
constexpr const char* KEY_FRACTION   = "plugin:hyprdrive:dock:height_fraction";
constexpr const char* KEY_BLUR_SIZE   = "plugin:hyprdrive:dock:blur_size";
constexpr const char* KEY_BLUR_PASSES = "plugin:hyprdrive:dock:blur_passes";
constexpr const char* KEY_BLUR_BRIGHT = "plugin:hyprdrive:dock:blur_brightness";
constexpr const char* KEY_BLUR_NOISE  = "plugin:hyprdrive:dock:blur_noise";
constexpr const char* KEY_BLUR_CONTR  = "plugin:hyprdrive:dock:blur_contrast";
constexpr const char* KEY_BLUR_VIBR   = "plugin:hyprdrive:dock:blur_vibrancy";
// dock:minimize_ms and dock:flight_log moved to FlightController.cpp with the
// code that reads them. scripts/keycheck.sh checks the whole tree, so a key
// crossing a file boundary is still covered.

// X30, all four read off the reference: `autohide` true / `dock-fixed` false,
// `require-pressure-to-show` true at `pressure-threshold` 100.0, `show-delay`
// 0.25, `hide-delay` 0.20, `animation-time` 0.20.
constexpr const char* KEY_AUTOHIDE     = "plugin:hyprdrive:dock:autohide";
constexpr const char* KEY_PRESSURE     = "plugin:hyprdrive:dock:pressure_threshold";
constexpr const char* KEY_REQ_PRESSURE = "plugin:hyprdrive:dock:require_pressure";
constexpr const char* KEY_SHOW_DELAY   = "plugin:hyprdrive:dock:show_delay_ms";
constexpr const char* KEY_HIDE_DELAY   = "plugin:hyprdrive:dock:hide_delay_ms";
constexpr const char* KEY_AUTOHIDE_MS  = "plugin:hyprdrive:dock:autohide_ms";

// X31, one key per reference setting: `show-running`, the two that gate the
// badge (`show-icons-notifications-counter` AND `show-icons-emblems` -- D2D
// needs both, the emblem switch is what builds the UnityIndicator at all),
// `dance-urgent-applications` and `show-dock-urgent-notify`. The dot's size and
// colour are keys too because `custom-theme-customize-running-dots` is FALSE in
// the reference, which means "take the theme's numbers" -- so the theme's
// numbers are the defaults and the key is where a different theme would land.
constexpr const char* KEY_SHOW_RUNNING  = "plugin:hyprdrive:dock:running_indicator";
constexpr const char* KEY_DOT_SIZE      = "plugin:hyprdrive:dock:indicator_size";
constexpr const char* KEY_BADGE         = "plugin:hyprdrive:dock:notification_counter";
constexpr const char* KEY_EMBLEMS       = "plugin:hyprdrive:dock:emblems";
constexpr const char* KEY_DANCE_URGENT  = "plugin:hyprdrive:dock:dance_urgent";
constexpr const char* KEY_URGENT_NOTIFY = "plugin:hyprdrive:dock:urgent_notify";

// X32, one key per reference setting: `show-favorites`, `show-show-apps-button`,
// `show-apps-at-top` and `show-apps-always-in-the-edge`. The last two are a PAIR
// and the reference sets them false/true, which on a bottom dock puts the button
// at the right-hand edge, past everything else.
constexpr const char* KEY_SHOW_FAVORITES = "plugin:hyprdrive:dock:show_favorites";
constexpr const char* KEY_SHOW_APPS      = "plugin:hyprdrive:dock:show_apps_button";
constexpr const char* KEY_SHOW_APPS_TOP  = "plugin:hyprdrive:dock:show_apps_at_top";
constexpr const char* KEY_SHOW_APPS_EDGE = "plugin:hyprdrive:dock:show_apps_always_in_the_edge";

// GNOME's own Show Applications artwork. A key rather than a constant because it
// is the one part of the button a theme changes.
constexpr const char* KEY_SHOW_APPS_ICON = "plugin:hyprdrive:dock:show_apps_icon";

// X33. Scroll, middle-click, and the workspace scoping X31 left alone.
constexpr const char* KEY_SCROLL_ACTION = "plugin:hyprdrive:dock:scroll_action";
constexpr const char* KEY_MIDDLE_CLICK  = "plugin:hyprdrive:dock:middle_click_action";
constexpr const char* KEY_SHIFT_MIDDLE  = "plugin:hyprdrive:dock:shift_middle_click_action";
constexpr const char* KEY_ISOLATE_WS    = "plugin:hyprdrive:dock:isolate_workspaces";
constexpr const char* KEY_ISOLATE_LOCATIONS = "plugin:hyprdrive:dock:isolate_locations";
constexpr const char* KEY_AGNOSTIC_URG  = "plugin:hyprdrive:dock:workspace_agnostic_urgent";

// X34, one key per reference setting: `hot-keys`, `hotkeys-overlay`,
// `hotkeys-show-dock`, `shortcut-timeout` 2.0. The ACCELERATORS themselves are
// deliberately not keys here -- the reference stores `<Super>1`..`<Super>0` in
// gsettings because GNOME's `Main.wm.addKeybinding` reads them from there, and
// under Hyprland the config's own `bind` lines ARE that store. A second copy
// inside the plugin could only disagree with the one the compositor obeys.
constexpr const char* KEY_HOT_KEYS       = "plugin:hyprdrive:dock:hot_keys";
constexpr const char* KEY_HOTKEY_OVERLAY = "plugin:hyprdrive:dock:hotkeys_overlay";
constexpr const char* KEY_HOTKEY_DOCK    = "plugin:hyprdrive:dock:hotkeys_show_dock";
constexpr const char* KEY_SHORTCUT_SECS  = "plugin:hyprdrive:dock:shortcut_timeout";

// `const NUM_HOTKEYS = 10` -- the reference binds ten keys and numbers ten
// icons, and the eleventh icon onwards gets neither.
constexpr int NUM_HOTKEYS = 10;

// `updateNumberOverlay`: `font-size: max(12, 0.3 * natWidth)` logical px against
// the icon, and a box `1.2` times that, square. `border-radius: iconSize` is far
// past half the box, and CSS clamps a radius to half -- so the shape is a circle
// and half the box is the number that stays one at any icon size.
constexpr float NUMBER_FONT_MIN   = 12.F;
constexpr float NUMBER_FONT_RATIO = 0.3F;
constexpr float NUMBER_BOX_RATIO  = 1.2F;

// `.number-overlay`: `color: white; background-color: rgba(0, 0, 0, 0.8)`.
constexpr float NUMBER_BG_ALPHA = 0.8F;

// The reference keeps one timeout id per dock and restarts it on every press
// (`_showOverlay`: "Restart the counting if the shortcut is pressed again"). A
// DEADLINE is the same thing with no timer to leak: restarting is one
// assignment and asking whether the overlay is up is one comparison. ONE
// deadline rather than one per monitor because there is one keyboard --
// `_showOverlay` loops over `allDocks` and hands every dock the same timeout.
Time::steady_tp g_overlayUntil = Time::steadyNow();

bool overlayUp() {
    return Config::getValue<bool>(KEY_HOT_KEYS) && Config::getValue<bool>(KEY_HOTKEY_OVERLAY) && Time::steadyNow() < g_overlayUntil;
}

// X34. Reveal the dock while the overlay is up, on the same `want` the pressure
// barrier feeds. The reference calls `dock._show()` and then leans on the
// timeout's `_updateDashVisibility()` to put it back -- so the hold ENDS with
// the overlay rather than being undone separately, which is what a deadline
// read once per frame gives for free.
//
// Gated on `hotkeys-show-dock`, and the reference gates it again on the dock
// being hideable at all (`_intellihideIsEnabled || _autohideIsEnabled`) -- with
// autohide off the pill is already up and there is nothing to reveal.
bool overlayHoldsDock() {
    return Config::getValue<bool>(KEY_HOT_KEYS) && Config::getValue<bool>(KEY_HOTKEY_DOCK) && Config::getValue<bool>(KEY_AUTOHIDE) && Time::steadyNow() < g_overlayUntil;
}

// The reference's own two numbers, hardcoded because the reference hardcodes
// them: 250ms of dead time between wheel events, so one flick is one step rather
// than ten (`_optionalScrollCycleWindowsDeadTimeId`), and 3000ms of memory for
// the cycle's window list, because activating a window reorders the stack the
// list came from (`MEMORY_TIME` in `_cycleThroughWindows`).
constexpr float SCROLL_DEAD_MS  = 250.F;
constexpr float CYCLE_MEMORY_MS = 3000.F;

// [FLIGHT] EFlightKind / EFlightPhase / SFlight / LAND_HOLD_FRAMES moved to
// FlightController.{hpp,cpp}, where the phases became explicit (Armed,
// Flying, LandingBridge, Done) and the sentinels -- lastAlpha < 0, an empty
// start box, alphaFrom -1 -- became optionals.


// Y6. GNOME's launch feedback, from iconGrid.js `zoomOutActorAtPos`: a CLONE of
// the icon scales to 3x about its own centre while easing opacity to nothing.
// The real icon underneath is untouched, which is why this is a separate list
// and not a transform on the item.
struct SLaunchZoom {
    std::string     icon;
    // Whose dock it grew out of: `from` is in that monitor's pixels, and every
    // monitor's dock draws this list (user, 2026-09-14, the multi-monitor sweep).
    MONITORID       monitor = MONITOR_INVALID;
    CBox            from;
    Time::steady_tp startedAt;
};

// Y8. macOS hops the icon a thing LANDS in -- once, not a decaying train, and
// on arrival rather than on release.
struct SBounce {
    std::string     wmClass;
    Time::steady_tp startedAt;
};

// EVERYTHING THE DOCK IS ANIMATING, and the one piece of state that decides
// whether it should be: five loose g_ globals scattered over 100 lines, each
// with its own helpers between them, so "what is the dock in the middle of"
// had no single answer to read. Same lists, same names minus the prefix.
//
// Z13's minimizeTiles is in here rather than beside the minimize path because
// it is the memory of an animation that has not happened yet: the tile a
// restore will aim at.
struct SDockFlight {
    // [FLIGHT] the flight list and Z13's minimizeTiles moved to
    // FlightController.cpp: they are the state machine's, and nothing else
    // in the dock ever read them.
    std::vector<SLaunchZoom> launchZooms;
    std::vector<SBounce>     bounces;
    // Y7. Set by the overview while a window drag is in the air, so the pill can
    // draw the drop glow. Not a flight, but the same question: is the dock in
    // the middle of something.
    bool                     windowDragActive = false;
};

SDockFlight g_flight;
// [FLIGHT] rememberMinimizeTile / takeMinimizeTile / warpRestoredWindowAnims
// moved to FlightController.cpp -- every caller was flight code.

// [FLIGHT] easeOutExpo moved to FlightController.cpp (only the flight used it).

// Y6. GNOME's launch feedback, from iconGrid.js `zoomOutActorAtPos`: a CLONE of
// the icon scales to 3x about its own centre (pivot 0.5, 0.5) while easing
// opacity to nothing, CLAMPED so the blow-up cannot leave the monitor. The real
// icon underneath is untouched, which is why this is a separate list and not a
// transform on the item.
constexpr float LAUNCH_ZOOM_SCALE = 3.F;   // APPICON_ANIMATION_OUT_SCALE
constexpr float LAUNCH_ZOOM_MS    = 250.F; // APPICON_ANIMATION_OUT_TIME


// `Clutter.AnimationMode.EASE_OUT_QUAD`.
float easeOutQuad(float t) {
    return 1.F - (1.F - t) * (1.F - t);
}

// Y7. The user's own threshold: "within a threshold, say, bottom 30% of the
// screen". Zero at the top of that band and full at the pill, CONTINUOUS in
// between -- a glow that switches on at the boundary is not "correlated to the
// distance", which is what was asked for.
constexpr float DRAG_GLOW_BAND  = 0.30F;
constexpr float DRAG_GLOW_PX    = 18.F; // how far the halo reaches past the pill
constexpr float DRAG_GLOW_ALPHA = 0.55F;

// Y8. macOS hops the icon a thing LANDS in -- once, not a decaying train, and
// on arrival rather than on release. Up decelerates and down accelerates, which
// is both the house rule and what falling actually does. The DURATION is
// Dock::HOP_MS in the header, because V4's stack cells hop too and there is only
// one hop; the shape below is what HOP_MS is the length of.
constexpr float BOUNCE_HEIGHT   = 0.34F; // of one icon height
constexpr float BOUNCE_UP_SHARE = 0.42F; // the hop is quicker up than down


float bounceCurve(float t) {
    if (t <= 0.F || t >= 1.F)
        return 0.F;

    return t < BOUNCE_UP_SHARE ? houseEase(true, t / BOUNCE_UP_SHARE) : 1.F - houseEase(false, (t - BOUNCE_UP_SHARE) / (1.F - BOUNCE_UP_SHARE));
}

// [FLIGHT] flightProgress moved to FlightController.cpp. lerpBox stays below --
// the hover panel eases with it, and the flight module keeps its own copy.

CBox lerpBox(const CBox& a, const CBox& b, float t) {
    return CBox{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.w + (b.w - a.w) * t, a.h + (b.h - a.h) * t};
}

struct SMetrics {
    float icon   = 0.F;
    float width  = 0.F;
    float height = 0.F;
};

// Pure, so the proportions can be asserted without a compositor. `icon-size-fixed`
// is false in the reference, which is what licenses the shrink: over the
// height-fraction cap D2D makes the icons smaller rather than overflowing.
//
// `units` is the width of every slot expressed in icon-widths: 1.0 per app icon,
// the clamped aspect ratio per preview. With no previews it equals `slots` and
// this is byte-for-byte the X27 formula.
SMetrics metrics(size_t slots, float units, float iconMax, float maxWidth) {
    if (slots == 0)
        return {};

    const float N      = sc<float>(slots);
    const float CHROME = 2.F * CONTAINER_PAD_X + 2.F * ITEM_MARGIN_X * N;
    const float AVAIL  = maxWidth - CHROME;

    SMetrics out;
    out.icon   = std::max(8.F, std::min(iconMax, AVAIL / std::max(units, 0.01F)));
    out.width  = CHROME + out.icon * units;
    out.height = ITEM_PAD_TOP + out.icon + ITEM_PAD_BOTTOM;
    return out;
}

// -------------------------------------------------------------- X32 pins --

// Favourites are a LIST OF DESKTOP IDS, the same unit GNOME's `favorite-apps`
// key uses. A plain file, because a plugin cannot write a Hyprland config key
// back out and this is user data rather than a tunable.
std::filesystem::path favoritesPath() {
    if (const char* STATE = std::getenv("XDG_STATE_HOME"); STATE && *STATE)
        return std::filesystem::path{STATE} / ScrollOverview::Compat::NAME / "favorites";

    if (const char* HOME = std::getenv("HOME"); HOME && *HOME)
        return std::filesystem::path{HOME} / ".local" / "state" / ScrollOverview::Compat::NAME / "favorites";

    return {};
}

// Seeds an EMPTY store only, and only with neutral entries every desktop is
// likely to have -- an id that no desktop entry claims is dropped at layout
// time, not here. Real dock contents are whatever the user pins; the store
// under ~/.local/state/hyprdrive/favorites wins from the first write on.
constexpr std::array<const char*, 3> DEFAULT_FAVORITES = {
    "org.gnome.Nautilus",
    "org.gnome.TextEditor",
    "org.gnome.Ptyxis",
};

// How often the store's timestamp is looked at, at most. `favorites()` is on
// the layout path (~10 calls a frame through baseLayout), so an unthrottled
// stat would run a few hundred times a second to notice a file that changes
// about once a month; twice a second is under any hand's reaction time and
// costs two statx calls.
constexpr float FAVORITES_POLL_MS = 500.F;

// The cache AND its freshness stamp in one place, so writeFavorites() can adopt
// the mtime of the file it just produced instead of reading its own output back.
struct SFavoritesStore {
    std::vector<std::string>        ids;
    std::filesystem::file_time_type stamp{};
    Time::steady_tp                 checkedAt{};
    bool                            loaded = false;
    // THE LIST, AS A NUMBER. dockItems() is memoized on a key and this is the
    // favourites half of it (user, 2026-09-14): it moves when the file is
    // re-read into something new, and when pinAt/unpin/toggleFavorite edit the
    // vector in place -- which writeFavorites() then stamps, so a poll would
    // never see it.
    uint64_t                        generation = 1;
};

SFavoritesStore& favoritesStore() {
    static SFavoritesStore store;
    return store;
}

// The store is the source of truth and the plugin is NOT its only writer: a
// profile revert (hdprof), an editor, or a second session all change the file
// under a running dock. Reading it ONCE at first use made every such write
// silently invisible until the next compositor start -- which is how X34's
// slot 1 stopped being the app the store's first line names, with the hotkey
// itself working perfectly (measured 2026-08-13, zrig/danglingC/x34-*).
std::vector<std::string>& favorites() {
    auto& STORE = favoritesStore();

    const auto NOW = Time::steadyNow();
    if (STORE.loaded && std::chrono::duration<float, std::milli>(NOW - STORE.checkedAt).count() < FAVORITES_POLL_MS)
        return STORE.ids;

    STORE.checkedAt = NOW;

    // A MISSING file seeds; an EMPTY one does not. Unpinning the last
    // favourite is a thing the user did on purpose, and re-seeding it on the
    // next start would undo them silently. A store that is DELETED while the
    // dock runs keeps what it had rather than re-seeding underneath the user.
    std::error_code ec;
    const auto      PATH = favoritesPath();
    if (PATH.empty() || !std::filesystem::exists(PATH, ec)) {
        if (!STORE.loaded) {
            STORE.loaded = true;
            STORE.ids.assign(DEFAULT_FAVORITES.begin(), DEFAULT_FAVORITES.end());
            ++STORE.generation;
        }
        return STORE.ids;
    }

    // A file that has not moved since the last read is not re-read: the vector
    // this returns is MUTATED in place by pinAt/unpin/toggleFavorite before
    // writeFavorites() puts it back, and a reload underneath one of those would
    // drop the edit on the floor.
    const auto WRITTEN = std::filesystem::last_write_time(PATH, ec);
    if (ec || (STORE.loaded && WRITTEN == STORE.stamp))
        return STORE.ids;

    STORE.loaded = true;
    STORE.stamp  = WRITTEN;
    STORE.ids.clear();
    ++STORE.generation;

    std::ifstream file(PATH);
    std::string   line;
    while (std::getline(file, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
            line.pop_back();
        if (!line.empty())
            STORE.ids.emplace_back(line);
    }

    return STORE.ids;
}

void writeFavorites() {
    // FIRST, and before the early return: the caller has already edited the
    // vector in place, so the list this process serves has changed whether or
    // not the file can be written. dockItems() is memoized on this number.
    ++favoritesStore().generation;

    const auto PATH = favoritesPath();
    if (PATH.empty())
        return;

    std::error_code ec;
    std::filesystem::create_directories(PATH.parent_path(), ec);

    {
        std::ofstream out(PATH, std::ios::trunc);
        for (const auto& F : favorites())
            out << F << "\n";
    }

    // Adopt the mtime of what we just wrote, or the next poll reads our own
    // output back in -- harmless in content, but it would clear and refill the
    // cache on every pin.
    auto& STORE = favoritesStore();
    STORE.stamp = std::filesystem::last_write_time(PATH, ec);
}

void toggleFavorite(const std::string& id) {
    if (id.empty())
        return;

    auto& FAV = favorites();
    if (const auto IT = std::ranges::find(FAV, id); IT != FAV.end())
        FAV.erase(IT);
    else
        FAV.emplace_back(id);

    writeFavorites();
}

// X33. The reference's `getInterestingWindows()`: the one filter every list the
// dock builds runs through, so the icons, the dot, the menu, the cycle and the
// quit all agree on which windows exist.
//
// `isolate-workspaces` is FALSE in the reference, so by default this is every
// mapped window and the dock stays workspace-agnostic exactly as X27 left it.
// Turned on, it is the CURRENT workspace only -- the focused monitor's, because
// `isolate-monitors` is false and the reference then asks the global workspace
// manager, not a per-monitor one.
//
// Two exemptions, both load-bearing:
//   * `workspace-agnostic-urgent-windows` (TRUE) keeps an urgent window listed
//     wherever it is, which is the whole point of the key: an app demanding
//     attention on another desktop must still be reachable from the dock.
//   * a MINIMIZED window is always interesting. Ours are stashed on a special
//     workspace that is never any monitor's active one, so isolation would hide
//     every minimized window and X24's click-to-restore would have nothing left
//     to restore. Its real home is the workspace it will come back to.
// Everything the filter reads that is NOT the window, read ONCE. getValue hashes
// its key string on every call and focusState()->monitor() is a lock; the filter
// runs per window, and the walks that use it run several times per pointer
// motion event (user, 2026-09-14).
struct SInterestCfg {
    bool         isolate         = false;
    bool         agnosticUrgent  = false;
    PHLWORKSPACE activeWorkspace = nullptr;
};

SInterestCfg interestCfg() {
    SInterestCfg cfg;
    cfg.isolate = Config::getValue<bool>(KEY_ISOLATE_WS);
    if (!cfg.isolate)
        return cfg; // nothing below is read

    cfg.agnosticUrgent = Config::getValue<bool>(KEY_AGNOSTIC_URG);
    if (const auto MONITOR = Desktop::focusState()->monitor(); MONITOR)
        cfg.activeWorkspace = MONITOR->m_activeWorkspace;

    return cfg;
}

bool interesting(const PHLWINDOW& window, const SInterestCfg& cfg) {
    if (!window || !window->m_isMapped)
        return false;

    if (!cfg.isolate)
        return true;

    if (Minimize::isMinimized(window))
        return true;

    if (window->m_isUrgent && cfg.agnosticUrgent)
        return true;

    return cfg.activeWorkspace && window->m_workspace == cfg.activeWorkspace;
}

// X27 showed what is RUNNING and nothing else. X32 keeps that list and puts the
// FAVOURITES in front of it: the compositor's own window order, which is stable
// across frames -- an MRU order here would make the dock reshuffle on every
// focus.
std::vector<std::string> runningClasses() {
    std::vector<std::string>        out;
    std::unordered_set<std::string> seen;

    const auto CFG = interestCfg();
    for (const auto& windowRef : Desktop::windowState()->windows()) {
        const auto& WINDOW = windowRef;
        if (!interesting(WINDOW, CFG) || WINDOW->m_class.empty())
            continue;

        const auto CLASS = AppSearch::identityClass(WINDOW->m_class, WINDOW->m_title);
        if (seen.insert(CLASS).second)
            out.emplace_back(CLASS);
    }

    return out;
}

// Y2/press/tilt. Defined with the tooltip much further down; declared here
// because the item builder stamps every item with its subject as it goes.
std::string tooltipSubject(const SItem& item);

// Moves every time the item list is actually rebuilt. The base layout hangs its
// own cache off this rather than off the vector's contents: comparing two
// hundred strings per call would cost more than the layout it is saving.
uint64_t g_itemsGeneration = 1;

// X32. What the pill lists, in order, with no geometry on it yet: every
// favourite first -- running or not -- then everything running that no favourite
// already covers.
//
// The two lists are keyed on DIFFERENT things, which is the whole difficulty. A
// favourite is a desktop id and owns no window until it is launched; a running
// app is a window class and may have no desktop entry at all. `idForClass` is
// the only bridge, so it is asked once per running class and the answer decides
// which list the class belongs to.
//
// Named build* because it is no longer what callers reach for: dockItems()
// below memoizes this behind a generation key, and that is the entry point.
// Z11's spam guard (see launchApp). An entry used to live out its full 30
// seconds even after the window had come and gone, so "open Trash, close it,
// click Trash" was swallowed for the rest of the half-minute (user,
// 2026-09-24). An entry now ends when its launch LANDS -- the item turns
// running -- and a location, which is not marked running unless
// isolate_locations is on, only guards against the double-click itself:
// Nautilus opening a folder is quick and never "cold".
std::unordered_map<std::string, Time::steady_tp> g_pendingLaunches;
constexpr float                                  LAUNCH_PENDING_MS   = 30000.F;
constexpr float                                  LOCATION_PENDING_MS = 1500.F;

std::string pendingLaunchKey(const SItem& item) {
    return !item.uri.empty() ? item.uri : (item.desktopId.empty() ? item.wmClass : item.desktopId);
}

std::vector<SItem> buildDockItems() {
    const auto RUNNING = runningClasses();

    std::vector<std::string> runningIds;
    runningIds.reserve(RUNNING.size());
    for (const auto& CLASS : RUNNING)
        runningIds.emplace_back(AppSearch::idForClass(CLASS));

    std::vector<SItem> out;
    std::unordered_set<size_t> claimed;

    if (Config::getValue<bool>(KEY_SHOW_FAVORITES)) {
        for (const auto& ID : favorites()) {
            const auto* ENTRY = AppSearch::entryForId(ID);
            // A favourite nothing on this machine provides is dropped rather
            // than drawn as a blank slot: the seed is the user's HOST list and
            // the test VM has a fraction of it installed.
            if (!ENTRY)
                continue;

            SItem item;
            item.desktopId = ID;
            item.icon      = ENTRY->icon;
            item.favorite  = true;

            // Every running class that resolves to this id owns the slot, so a
            // pinned app that IS running is one icon, not two -- including a
            // Chromium webapp whose live class is chrome-host_path-Default and
            // a second profile of the same app (another class, same desktop id).
            // itemWindows then finds every window of those classes.
            for (size_t i = 0; i < RUNNING.size(); ++i) {
                if (runningIds[i] != ID || claimed.contains(i))
                    continue;

                claimed.insert(i);
                if (item.wmClass.empty())
                    item.wmClass = RUNNING[i];
                item.running = true;
            }

            out.emplace_back(std::move(item));
        }
    }

    for (size_t i = 0; i < RUNNING.size(); ++i) {
        if (claimed.contains(i))
            continue;

        // No desktop entry claims the class: ask for an icon NAMED after it
        // (plenty of apps install exactly that), which resolveIconPath answers
        // with the generic application tile when the theme has none. The slot
        // used to be left blank -- a running app with nothing to click on.
        auto icon = AppSearch::iconForClass(RUNNING[i]);
        if (icon.empty() && !RUNNING[i].empty()) {
            icon = RUNNING[i];
            std::ranges::transform(icon, icon.begin(), [](unsigned char c) { return sc<char>(std::tolower(c)); });
        }

        out.emplace_back(SItem{
            .wmClass   = RUNNING[i],
            .icon      = icon,
            .desktopId = runningIds[i],
            .running   = true,
        });
    }

    // The scratchpad sits after the apps and BEFORE the locations: it holds
    // windows, so it belongs with the app icons rather than out among the
    // shelf. One slot per populated scratchpad, and it disappears with its last
    // window -- the same "only what is running" rule the app icons follow.
    for (const auto& WORKSPACE : Scratchpad::populated()) {
        out.emplace_back(SItem{
            .icon           = Scratchpad::iconName(),
            .running        = true,
            .locationName   = Scratchpad::label(WORKSPACE),
            .scratchpad     = true,
            .scratchpadName = Scratchpad::shortName(WORKSPACE),
        });
    }

    // DOCK ORDER (dock stacks A, 2026-09-24): apps | Scratchpad | stacks |
    // removable drives | Trash. Stacks used to come after the locations, which
    // put the user's own shelf behind Trash.
    //
    // A stack carries `uri` as well as `stack`, so the `!uri.empty()` branches
    // this file already has treat it as the not-an-app it is. Its icon, label
    // and is-a-directory answer are resolved when the STORE is read
    // (DockStacks.cpp, decorate), not here.
    for (const auto& STACK : Stacks::all()) {
        out.emplace_back(SItem{
            .icon         = STACK.icon,
            .uri          = "file://" + STACK.path,
            .locationName = STACK.label,
            .stack        = true,
            .stackPath    = STACK.path,
            .stackDir     = STACK.isDir,
        });
    }

    // X35. Locations: mounts first, then everything else (Trash), so Trash
    // stays the last cell. `isolate_locations` TRUE makes a location's running
    // state its own -- asked of the file manager's open windows.
    const bool ISOLATE = Config::getValue<bool>(KEY_ISOLATE_LOCATIONS);
    for (const bool MOUNTS : {true, false}) {
        for (const auto& LOCATION : Locations::items()) {
            if (LOCATION.mount != MOUNTS)
                continue;
            out.emplace_back(SItem{
                .icon         = LOCATION.icon,
                .running      = ISOLATE && Locations::openWindows(LOCATION.uri),
                .uri          = LOCATION.uri,
                .locationName = LOCATION.name,
                .mount        = LOCATION.mount,
                .canEject     = LOCATION.canEject,
                .mounted      = LOCATION.mounted,
                .trashEmpty   = LOCATION.empty,
            });
        }
    }

    // ONE PASS, at the end, so no branch above can forget it. See SItem::subject.
    for (auto& item : out)
        item.subject = tooltipSubject(item);

    for (const auto& ITEM : out)
        if (ITEM.running && !ITEM.scratchpad)
            g_pendingLaunches.erase(pendingLaunchKey(ITEM));

    return out;
}

// ---------------------------------------------------------------------------
// THE ITEM MODEL IS BUILT ONCE PER CHANGE, NOT ONCE PER CALLER.
//
// buildDockItems() above walks every window (with a config lookup per window
// inside interesting()), asks AppSearch for a desktop id per running class,
// polls the locations and the stacks, and produces a vector of items carrying
// eight strings each. It was being run between four and ten times per POINTER
// MOTION EVENT -- feedPointerMotion calls damageIfChanged for every monitor,
// each of which lays out twice through updateAutohide and once more for the
// signature, and the hover hook adds occupiesPointer -> pillContains -> layout
// on top -- and two or three times per frame per monitor on top of that (user,
// 2026-09-14). Motion events arrive far faster than frames.
//
// So it is memoized behind a KEY, and the key is the honest list of everything
// the answer depends on:
//
//   * Membership's generation, which every window open/close/kill/destroy/
//     class/title/workspace/minimize/restore already bumps (Membership.cpp);
//   * a digest of the window list itself -- pointer, mapped, urgent, workspace
//     -- for the two properties no event announces (a map, an urgent flag) and
//     as a second opinion on the rest. Allocation-free, one pass, no strings;
//   * the favourites store's generation, the locations poll's, the stacks
//     store's, and the size of the app inventory;
//   * the four config keys the build reads;
//   * and the focused monitor's active workspace, but only when
//     `isolate_workspaces` makes the answer depend on it.
//
// A key that has not moved means the vector cannot have: every one of those is
// on the way in to something buildDockItems() reads.
struct SItemsKey {
    uint64_t  membership = 0;
    uint64_t  windows    = 0;
    uint64_t  favorites  = 0;
    uint64_t  locations  = 0;
    uint64_t  stacks     = 0;
    uint64_t  inventory  = 0;
    uint64_t  icons      = 0;
    long long activeWs   = 0;
    uint32_t  config     = 0;

    bool operator==(const SItemsKey&) const = default;
};

// FNV-1a over the four window properties the item list can see. Deliberately
// not a hash of anything allocated: this runs on the motion path.
uint64_t windowDigest() {
    uint64_t       h   = 1469598103934665603ULL;
    const auto     mix = [&h](uint64_t v) {
        h ^= v;
        h *= 1099511628211ULL;
    };

    for (const auto& WINDOW : Desktop::windowState()->windows()) {
        if (!WINDOW) {
            mix(0);
            continue;
        }

        mix(sc<uint64_t>(reinterpret_cast<uintptr_t>(WINDOW.get())));
        mix((WINDOW->m_isMapped ? 1ULL : 0ULL) | (WINDOW->m_isUrgent ? 2ULL : 0ULL));
        mix(WINDOW->m_workspace ? sc<uint64_t>(WINDOW->m_workspace->m_id) : 0ULL);
    }

    return h;
}

SItemsKey itemsKey() {
    SItemsKey key;
    key.membership = Membership::generation();
    key.windows    = windowDigest();
    key.favorites  = favoritesStore().generation;
    key.locations  = Locations::generation();
    key.stacks     = Stacks::generation();
    // The GENERATION: a rescan that swaps one entry for another keeps the size.
    key.inventory  = AppSearch::inventoryGeneration();
    key.icons      = AppSearch::iconIndexGeneration();

    const bool ISOLATE_WS = Config::getValue<bool>(KEY_ISOLATE_WS);
    key.config            = (Config::getValue<bool>(KEY_SHOW_FAVORITES) ? 1U : 0U) |      //
        (ISOLATE_WS ? 2U : 0U) |                                                          //
        (Config::getValue<bool>(KEY_AGNOSTIC_URG) ? 4U : 0U) |                            //
        (Config::getValue<bool>(KEY_ISOLATE_LOCATIONS) ? 8U : 0U);

    // Only with isolation on does WHICH desktop is focused change the list.
    // Reading it unconditionally would throw the cache away on every workspace
    // switch for a dock that is deliberately workspace-agnostic.
    if (ISOLATE_WS) {
        if (const auto MONITOR = Desktop::focusState()->monitor(); MONITOR && MONITOR->m_activeWorkspace)
            key.activeWs = MONITOR->m_activeWorkspace->m_id;
    }

    return key;
}

const std::vector<SItem>& dockItems() {
    static std::vector<SItem> cache;
    static SItemsKey          cachedKey;
    static bool               filled = false;

    if (const auto KEY = itemsKey(); !filled || !(KEY == cachedKey)) {
        cachedKey = KEY;
        filled    = true;
        cache     = buildDockItems();
        ++g_itemsGeneration;
    }

    return cache;
}

// The item generation, REFRESHED. Reading g_itemsGeneration on its own would
// hand back the number from before the rebuild the very next line triggers.
uint64_t itemsGeneration() {
    dockItems();
    return g_itemsGeneration;
}

float previewAspect(const PHLWINDOW& window) {
    const auto SIZE = window->m_realSize->value();
    if (SIZE.x < 1.0 || SIZE.y < 1.0)
        return 1.F;

    return std::clamp(sc<float>(SIZE.x / SIZE.y), PREVIEW_MIN_ASPECT, PREVIEW_MAX_ASPECT);
}

CHyprColor background() {
    return ScrollOverview::Config::dockBackgroundColor();
}

// X25. A minimized window lives on a special workspace nobody shows, so every
// one of the compositor's "is this worth drawing?" answers is no. The overview
// solves the same problem by saving the old values and restoring them on close;
// here there is nothing to restore to -- while the window is minimized, VISIBLE
// is its state -- so this just asserts it each frame.
void forcePreviewVisible(const PHLWINDOW& window) {
    window->m_hidden = false;

    // The one that actually froze the preview: a window on a hidden workspace
    // gets the xdg_toplevel `suspended` state, and a client that honours it
    // stops drawing at all -- measured 0 frame callbacks and 0 commits over 5s
    // minimized against 52/52 visible, which no amount of acking frames on our
    // side can fix. GlobalWindowController re-asserts suspension on every
    // visibility change, so this is re-checked each frame rather than set once.
    if (window->m_suspended)
        window->setSuspended(false);
    window->alpha(Desktop::View::WINDOW_ALPHA_FULLSCREEN)->setValueAndWarp(1.F);

    if (const auto WORKSPACE = window->m_workspace; WORKSPACE && WORKSPACE->m_alpha) {
        WORKSPACE->m_alpha->setValueAndWarp(1.F);
        *WORKSPACE->m_alpha = 1.F;
    }
}

// The whole difference between a live preview and a stale one. A client draws
// the next frame when the last one is acked; nobody acks a window on a hidden
// workspace, so without this the preview freezes on whatever it happened to
// have committed when it was minimized -- which the spec calls a defect.
void presentPreview(const PHLWINDOW& window, PHLMONITOR monitor, const Time::steady_tp& now) {
    std::pair<PHLMONITOR, Time::steady_tp> data = {monitor, now};

    window->wlSurface()->resource()->breadthfirst(
        [](SP<CWLSurfaceResource> surface, const Vector2D&, void* data) {
            const auto [MONITOR, NOW] = *sc<std::pair<PHLMONITOR, Time::steady_tp>*>(data);
            surface->presentFeedback(NOW, MONITOR, false);
        },
        &data);
}

// WHAT THE DOCK LOOKS LIKE THIS FRAME, as a value.
//
// damageIfChanged only asks for a repaint when this changes, so anything that
// can alter a pixel WITHOUT moving a box -- a badge arriving, an app going
// urgent, `user-trash` becoming `user-trash-full`, the hovered row of a context
// menu -- has to be a field in here or the frame that would draw it is never
// asked for. Every one of those is a bug this tree has actually shipped, which
// is what the numbered comments on the fields are.
//
// It used to be a hand-built string, and the two reasons it is not any more are
// bugs waiting rather than tidiness. Concatenation ALIASES: the old
// `desktopId + "/" + wmClass + uri + icon` spells the same thing for
// ("a", "b") as for ("ab", "") -- rare, silent, and indistinguishable from a
// dropped frame. And a new visual feature had to REMEMBER to append itself. A
// struct with a defaulted operator== cannot alias, and the compiler carries the
// reminder instead of a comment: add a field, and the comparison picks it up.
struct SDockItemState {
    std::string desktopId, wmClass, uri, icon;
    bool        running = false;
    int         badge   = 0;
    bool        urgent  = false;

    bool operator==(const SDockItemState&) const = default;
};

struct SDockVisualState {
    // V11 V4. The stack popup's whole appearance as one string -- see
    // Stacks::visualState. A string rather than fields because the popup's
    // contents are a directory listing and the dock has no business modelling
    // one twice.
    std::string stacks;
    // No pill -- dock off, or fully hidden. Everything else is then moot, and
    // the default-constructed state is exactly "nothing to draw", which is what
    // the first call on a monitor has to compare equal to.
    bool pillUp = false;
    // pillY is in here because during the autohide slide it is the ONLY thing
    // that changes.
    int                         pillX = 0, pillY = 0, pillW = 0;
    std::vector<SDockItemState> items;
    // The hovered cell is part of what the pill LOOKS like, so a bare pointer
    // move over an otherwise idle dock has to repaint it -- nothing else on the
    // desktop damages the monitor for a motion event.
    int  hovered       = -1;
    bool hoverShowApps = false;
    // DockTilt. The hover lean follows the pointer WITHIN a cell, so while it is
    // on, a move that stays on one icon still changes what the icon looks like.
    int tiltX = -1, tiltY = -1;
    // X32/G12. The insertion caret moves on a bare pointer move exactly as the
    // hover highlight does, and nothing else asks for a frame during a drag.
    int caret = -1;
    // V3. And once the drag leaves the pill the caret is gone, so the icon under
    // the pointer is the ONLY thing changing between two motion events: without
    // its position in here the ghost is drawn once and then frozen while the
    // pointer walks away from it (the desktop's hook damages from this diff and
    // nothing else).
    int ghostX = -1, ghostY = -1;
    // X34. The hotkey overlay's numbers arrive and leave without a single box
    // moving, so the frame that draws them -- and the one that clears them --
    // has to be asked for from here.
    bool overlay = false;
    // X28. The menu is part of what the dock looks like, and its hovered row
    // moves with a bare pointer move exactly as the icon highlight does.
    bool        menuUp = false;
    std::string menuClass, menuUri;
    int         menuRow = -1;

    bool operator==(const SDockVisualState&) const = default;
};

std::unordered_map<MONITORID, SDockVisualState> g_visualStates;

// X30. AUTOHIDE. The pill is away by default (`autohide` true, `dock-fixed`
// false) and is revealed by pushing the pointer into the bottom edge until the
// barrier's pressure passes `pressure-threshold`. Once up it stays up while the
// pointer is on it, and the overview pins it out the way clause 6 requires.
//
// `intellihide` is FALSE in the reference and its mode is inert while off, so
// there is deliberately no window-overlap hiding here -- that is the setting
// being honoured, not a gap.
//
// `autohide-in-fullscreen` is TRUE, and that is why there is no fullscreen
// branch either: the barrier keeps working under a fullscreen window instead of
// the dock locking itself away, and the pill is drawn at RENDER_POST_WINDOWS so
// a revealed dock lands over the fullscreen window. The behaviour is the
// absence of a special case; the test is what proves it.
struct SReveal {
    bool            shown        = false;
    Time::steady_tp changedAt    = Time::steadyNow(); // start of the slide toward `shown`
    bool            pending      = false;             // a flip is waiting out its delay
    Time::steady_tp pendingSince = Time::steadyNow();
    float           pressure     = 0.F;               // px pushed into the bottom edge
    // X75. The hidden fraction AT THE MOMENT OF THE FLIP. The slide used to be
    // computed from elapsed time alone, so reversing mid-slide restarted the
    // curve at the far end and the pill JUMPED there before travelling back.
    // dash-to-dock has no such seam: `_animateIn`/`_animateOut` are Clutter
    // eases on the slider, and an ease always departs from wherever the value
    // currently is.
    float           from         = 1.F;
};

std::unordered_map<MONITORID, SReveal> g_reveal;

// The barrier is the monitor's last row, and the cursor is clamped there -- but
// a clamp can leave it a pixel short, so the band is not exactly one row.
constexpr float EDGE_BAND = 2.F;

// The band that opens the dock on POSITION alone, deliberately a little wider
// than EDGE_BAND -- "on or near the bottom edge".
constexpr float REVEAL_BAND = 4.F;

float elapsedMs(const Time::steady_tp& since) {
    // Hover panel, tooltip fade, bounce, launch zoom. Autohide reveal uses
    // realElapsedMs so Hyprdrive motion off still slides the pill in from the
    // bottom edge.
    return ScrollOverview::Config::animElapsedMs(since);
}

// ---- X31 -------------------------------------------------------------------

// `workspace-agnostic-urgent-windows` is TRUE, so this asks about the APP and
// never about a workspace: any mapped window of the class, anywhere, counts.
// The LauncherEntry half is the same question asked over D-Bus -- an app can
// demand attention without owning an urgent surface (`urgent` in the protocol),
// and the reference reads both.
bool classUrgent(const std::string& wmClass) {
    for (const auto& WINDOW : Desktop::windowState()->windows()) {
        if (WINDOW && WINDOW->m_isMapped && WINDOW->m_isUrgent && AppSearch::identityClass(WINDOW->m_class, WINDOW->m_title) == wmClass)
            return true;
    }

    return LauncherEntry::state(AppSearch::idForClass(wmClass)).urgent;
}

// ---------------------------------------------------------------------------
// ONE WALK OF THE WINDOW LIST PER PASS, not one per item.
//
// classUrgent() and itemWindows() each walk every window, and both were asked
// ONCE PER ITEM -- from damageIfChanged's signature loop, from the running dot,
// from the urgent wiggle and from anyUrgent, which is itself asked twice per
// damageIfChanged. Thirty icons and forty windows is twelve hundred window
// visits for an answer that is one pass of the same list (user, 2026-09-14).
//
// TWO maps and not one, deliberately. A window class and a desktop id are
// different namespaces that overlap in spelling all the time -- `kitty` is both
// -- so a single table keyed on "a string" would either merge two questions or
// double-count one window under a key it matches twice. They are kept apart, and
// each reader then asks the question it was already asking:
//
//   byClass  interesting windows whose CLASS is the key      -- itemWindows'
//            first arm, and the only arm an item with no desktop id has
//   byId     interesting windows whose desktop ID is the key -- itemWindows'
//            second arm, and a SUPERSET of the first for any item dockItems
//            built, because that is how an item gets a class and an id at all
//            (the class was claimed by the id)
//   urgent   the class of every MAPPED urgent window, "" included, which is
//            exactly the set classUrgent's loop was testing against
struct STally {
    std::unordered_map<std::string, size_t> byClass;
    std::unordered_map<std::string, size_t> byId;
    std::unordered_set<std::string>         urgent;
};

STally buildTally() {
    STally out;

    const auto CFG = interestCfg();
    for (const auto& WINDOW : Desktop::windowState()->windows()) {
        if (!WINDOW || !WINDOW->m_isMapped)
            continue;

        const auto CLASS = AppSearch::identityClass(WINDOW->m_class, WINDOW->m_title);

        if (WINDOW->m_isUrgent)
            out.urgent.insert(CLASS);

        if (!interesting(WINDOW, CFG))
            continue;

        ++out.byClass[CLASS];

        // Only when it differs: for most apps the desktop id IS the class, and
        // incrementing both would count the window twice under one key.
        if (const auto ID = AppSearch::idForClass(CLASS); !ID.empty() && ID != CLASS)
            ++out.byId[ID];
    }

    // A class that IS its own id has to answer the id question too.
    for (const auto& [CLASS, COUNT] : out.byClass) {
        if (CLASS.empty())
            continue;
        if (AppSearch::idForClass(CLASS) == CLASS)
            out.byId[CLASS] += COUNT;
    }

    return out;
}

// itemWindows(item).size(), read off the tally. The union the real function
// computes collapses to ONE of the two maps: an item that carries a desktop id
// got its class from that id, so every window of the class is already in the id
// bucket.
size_t itemWindowCount(const SItem& item, const STally& tally) {
    if (item.wmClass.empty() && item.desktopId.empty())
        return 0; // itemWindows' own early return

    const auto& MAP = item.desktopId.empty() ? tally.byClass : tally.byId;
    const auto& KEY = item.desktopId.empty() ? item.wmClass : item.desktopId;

    const auto  IT  = MAP.find(KEY);
    return IT == MAP.end() ? 0 : IT->second;
}

// classUrgent(item.wmClass), read off the tally. The D-Bus half keys on the
// item's OWN desktop id rather than resolving its class again: dockItems set
// the one from the other, so they are the same string.
bool itemUrgent(const SItem& item, const STally& tally) {
    return tally.urgent.contains(item.wmClass) || LauncherEntry::state(item.desktopId).urgent;
}

// `application-counter-overrides-notifications` is TRUE, so a LauncherEntry
// count wins outright whenever the app publishes one -- which under Hyprland is
// the whole answer, because the other half of the reference's sum is GNOME
// Shell's message tray and there is no tray here to read. Named in
// RICE_PROGRESS.md as an open decision, not silently folded to zero.
//
// Keyed on the item's OWN desktop id: dockItems set that id FROM the class, so
// resolving the class again here was a lookup for a string already in hand.
long long itemBadge(const SItem& item, bool badgesOn) {
    if (!badgesOn)
        return 0;

    const auto STATE = LauncherEntry::state(item.desktopId);
    return STATE.countVisible ? std::max<long long>(0, STATE.count) : 0;
}

// The two switches itemBadge gates on, read ONCE per pass by the caller rather
// than twice per icon (user, 2026-09-14).
bool badgesEnabled() {
    return Config::getValue<bool>(KEY_BADGE) && Config::getValue<bool>(KEY_EMBLEMS);
}

bool anyUrgent(const std::vector<SItem>& items, const STally& tally) {
    // X32: a pinned app with no window has no class, and an urgent test keyed on
    // "" would ask LauncherEntry about the empty id once per item per frame.
    // X35: and a location has no class either, nor any way to go urgent.
    return std::ranges::any_of(items, [&](const SItem& item) { return item.running && item.uri.empty() && itemUrgent(item, tally); });
}

// Radians, because CBox::rot is; the reference's degrees are converted at the
// last moment so the numbers above stay the ones that were read off it. The
// epoch is shared and set once, which is what keeps two urgent icons in phase.
// ONE clock for every urgent animation, so the wiggle (dock showing) and the
// hop (dock hidden) keep the same rhythm, and two urgent apps move together.
const Time::steady_tp& urgentEpoch() {
    static const Time::steady_tp EPOCH = Time::steadyNow();
    return EPOCH;
}

float wiggleRadians() {
    const float PROGRESS = std::fmod(elapsedMs(urgentEpoch()), WIGGLE_PERIOD_MS) / WIGGLE_PERIOD_MS;
    if (PROGRESS >= WIGGLE_FRACTION)
        return 0.F;

    const float DEGREES = WIGGLE_DEGREES * std::sin(PROGRESS * 24.F * sc<float>(M_PI));
    return DEGREES * sc<float>(M_PI) / 180.F;
}

// THE HOP (user, 2026-09-24: "we shouldn't have the dock staying revealed to get
// your attention - instead the shaking item should also jump a little when the
// dock is out of view (little macos inspiration)"). While the dock is hidden,
// an urgent app's icon pops up from the bottom edge -- two bounces, the second
// smaller, like the macOS dock's -- once per wiggle period, on the wiggle's
// clock. The dock itself stays where autohide put it.
constexpr float URGENT_HOP_MS   = 900.F; // both bounces
constexpr float URGENT_HOP_LIFT = 0.62F; // of the icon's height, at the top of the first bounce

// 0..1 of the lift now, or -1 between hops.
float urgentHopLift() {
    const float PHASE = std::fmod(elapsedMs(urgentEpoch()), WIGGLE_PERIOD_MS);
    if (PHASE >= URGENT_HOP_MS)
        return -1.F;
    const float T = PHASE / URGENT_HOP_MS;
    if (T < 0.55F)
        return std::sin(sc<float>(M_PI) * T / 0.55F);
    return 0.5F * std::sin(sc<float>(M_PI) * (T - 0.55F) / 0.45F);
}

float msToNextHop() {
    return WIGGLE_PERIOD_MS - std::fmod(elapsedMs(urgentEpoch()), WIGGLE_PERIOD_MS);
}

// Between hops nothing on the desktop asks for frames, so the next hop is woken
// by a timer rather than by pumping 60 frames a second for an icon that is
// still for two seconds in three -- an app can stay urgent for hours. Removed
// from PLUGIN_EXIT (stopUrgentHopTimer).
wl_event_source* g_hopTimer = nullptr;

int hopTimerCb(void*) {
    for (const auto& MON : State::monitorState()->monitors())
        if (MON)
            g_pHyprRenderer->damageMonitor(MON);
    return 0;
}

void armHopTimer(float ms) {
    if (!g_hopTimer)
        g_hopTimer = wl_event_loop_add_timer(g_pCompositor->m_wlEventLoop, hopTimerCb, nullptr);
    if (g_hopTimer)
        wl_event_source_timer_update(g_hopTimer, std::max(1, sc<int>(std::ceil(ms))));
}

// ----------------------------------------------------------------------------

// X75 (user, 2026-08-06: "behave exactly like the autohide behavior in the
// dash to dock gnome extension"). dash-to-dock eases the slider with
// Clutter's EASE_OUT_QUAD in BOTH directions -- `_animateIn` and `_animateOut`
// pass the same mode -- so the hide does NOT accelerate away the way this
// rice's house exit curve does. Matching the reference means dropping the
// house asymmetry here; that is the instruction, not an oversight.
//
// Direction is no longer a parameter, because the curve no longer depends on it.
float revealEase(float t) {
    const float INV = 1.F - std::clamp(t, 0.F, 1.F);
    return 1.F - (INV * INV);
}

// 0 = fully out, 1 = fully tucked below the bottom edge.
float hiddenFraction(PHLMONITOR monitor) {
    if (!monitor)
        return 0.F;

    // Overview owns the slide while it exists: the 3-finger swipe warps
    // openProgress every update, so the pill tracks the fingers the same way
    // the cards do. Super-tap uses the scale animation, same number.
    if (const auto OV = scrollOverviewForMonitor(monitor); OV && Config::getValue<bool>(KEY_AUTOHIDE))
        return 1.F - OV->openProgress();

    const auto& R  = g_reveal[monitor->m_id];
    const int   MS = std::max(1, Config::getValue<int>(KEY_AUTOHIDE_MS));
    const float E  = revealEase(std::clamp(ScrollOverview::Config::realElapsedMs(R.changedAt) / sc<float>(MS), 0.F, 1.F));

    // From wherever it WAS to wherever it is going, which is what makes a
    // reversal continuous instead of a jump to the far end.
    return R.from + ((R.shown ? 0.F : 1.F) - R.from) * E;
}

const SLayout& baseLayout(PHLMONITOR monitor); // defined below; slidPill needs the resting box

// How far down the pill currently sits, mid-slide included. Shared with
// layout() below so the HIT TEST and the RENDER can never disagree about where
// the pill is -- which is the whole bug X77 fixes.
double slideOffsetPx(PHLMONITOR monitor, double pillHeight) {
    if (!monitor)
        return 0.0;

    return std::round((pillHeight + EDGE_MARGIN * monitor->m_scale) * hiddenFraction(monitor));
}

// The pill WHERE IT IS, not where it rests. Empty once it is fully away.
CBox slidPill(PHLMONITOR monitor) {
    auto pill = baseLayout(monitor).pill;
    if (pill.empty() || hiddenFraction(monitor) >= 1.F)
        return CBox{};

    pill.y += slideOffsetPx(monitor, pill.height);
    return pill;
}

bool autohideAnimating(PHLMONITOR monitor) {
    if (!monitor)
        return false;

    const auto& R = g_reveal[monitor->m_id];

    // AN IDLE OVERVIEW IS NOT AN ANIMATION. While an overview is open over this
    // monitor the pill's position is openProgress, not this timer -- and
    // updateAutohide stamps changedAt on EVERY call to keep the close hand-off
    // continuous. The elapsed test below therefore read "still sliding" forever,
    // so an overview left sitting open pumped a frame per monitor per frame,
    // for as long as it was open (user, 2026-09-14). The overview's own scale
    // animation asks for the frames openProgress actually needs while it moves;
    // a flight still asks for its own.
    if (const auto OV = scrollOverviewForMonitor(monitor); OV && Config::getValue<bool>(KEY_AUTOHIDE))
        return R.pending || Flight::anyOn(monitor);

    // X54. A flight counts as animating: the window is mid-air toward the icon
    // and nothing else on an idle desktop would ask for the frames it needs.
    return R.pending || Flight::anyOn(monitor) || ScrollOverview::Config::realElapsedMs(R.changedAt) < sc<float>(std::max(1, Config::getValue<int>(KEY_AUTOHIDE_MS)));
}

// THE RESTING PILL, CACHED PER MONITOR.
//
// Its inputs are the item list, the output's geometry, and six config keys --
// none of which move between two frames of a still dock, and all of which are
// cheap to compare. layout() still applies the SLIDE on top of this every call,
// so the autohide animation stays live; what is cached is the part that does
// not move (user, 2026-09-14).
struct SBaseLayoutKey {
    uint64_t items    = 0;
    double   width    = 0.0;
    double   height   = 0.0;
    double   scale    = 0.0;
    int      iconSize = 0;
    int      radius   = 0;
    float    fraction = 0.F;
    uint32_t flags    = 0;

    bool operator==(const SBaseLayoutKey&) const = default;
};

struct SBaseLayoutCache {
    SBaseLayoutKey key;
    SLayout        layout;
    bool           filled = false;
};

std::unordered_map<MONITORID, SBaseLayoutCache> g_baseLayouts;

// THE ARRANGEMENT (user, 2026-09-19: "when new apps open on the dock, no smooth
// expansion or app icon spawn animation exists"). A change to the item list
// used to swap one resting layout for the next in a single frame: the pill
// jumped to its new width and every icon teleported to its new slot. Now the
// displayed layout glides from whatever was ON SCREEN at that moment -- so a
// second app opening mid-glide continues from where things are, not from
// where they were -- to the new resting one. Icons that exist on both sides
// slide; a new one grows out of its own slot and fades in while its
// neighbours make room; one that left shrinks away where it stood (drawn by
// render() alone, never hit-tested, so a vanishing icon cannot be clicked).
//
// Items are matched on `subject`, the one string everything else that
// remembers an item keys on already. Only an ITEM change animates: a resize,
// a scale or an icon-size change is a new geometry, not a new arrangement,
// and snaps as it always did.
constexpr float ARRANGE_MS = 400.F; // 320 + two 40ms steps (user, 2026-09-19)
constexpr float EXIT_MS    = 280.F; // 220 + two 30ms steps

struct SArrangeExit {
    SItem           item; // as it was last displayed
    Time::steady_tp since;
};

struct SArrange {
    bool                                  active = false;
    Time::steady_tp                       since;
    CBox                                  fromPill, fromShowApps;
    std::unordered_map<std::string, CBox> fromBoxes; // by subject
    std::vector<SArrangeExit>             exits;
};

std::unordered_map<MONITORID, SArrange> g_arrange;

float arrangeEase(float t) { // ease-out cubic: fast away, soft landing
    t = std::clamp(t, 0.F, 1.F);
    const float U = 1.F - t;
    return 1.F - U * U * U;
}

float spawnEase(float t) { // ease-out back, a touch of overshoot so it lands
    t              = std::clamp(t, 0.F, 1.F);
    constexpr float C1 = 1.75F; // 1.25 + two 0.25 steps: peak overshoot ~6% -> ~10%
    constexpr float C3 = C1 + 1.F;
    const float     U  = t - 1.F;
    return 1.F + C3 * U * U * U + C1 * U * U;
}

CBox scaledAboutCentre(const CBox& box, float s) {
    const double W = box.width * s, H = box.height * s;
    return CBox{box.x + (box.width - W) / 2.0, box.y + (box.height - H) / 2.0, W, H};
}

// The resting layout as it is to be DISPLAYED right now. Pure apart from
// retiring an arrangement that has finished.
SLayout arranged(MONITORID id, const SLayout& resting) {
    const auto IT = g_arrange.find(id);
    if (IT == g_arrange.end() || !IT->second.active)
        return resting;

    auto&       A = IT->second;
    const float T = ScrollOverview::Config::realElapsedMs(A.since) / ARRANGE_MS;
    if (T >= 1.F) {
        A.active = false;
        A.fromBoxes.clear();
        return resting;
    }

    const float E   = arrangeEase(T);
    SLayout     out = resting;
    out.pill        = lerpBox(A.fromPill, resting.pill, E);
    if (!A.fromShowApps.empty() && !resting.showApps.empty())
        out.showApps = lerpBox(A.fromShowApps, resting.showApps, E);

    for (auto& item : out.items) {
        if (const auto FROM = A.fromBoxes.find(item.subject); FROM != A.fromBoxes.end()) {
            item.iconBox = lerpBox(FROM->second, item.iconBox, E);
            continue;
        }
        const float S = std::max(0.F, spawnEase(T));
        item.iconBox  = scaledAboutCentre(item.iconBox, S);
        item.appear   = std::clamp(T * 2.F, 0.F, 1.F); // opaque by half-way
    }

    return out;
}

// Called by baseLayout() with the layout that is ABOUT TO BE REPLACED, before
// it is: what is on screen is computed from the old resting layout.
void beginArrangement(MONITORID id, const SLayout& oldResting, const SLayout& newResting) {
    if (oldResting.pill.empty() || newResting.pill.empty())
        return; // from or to nothing is the dock appearing, which has its own slide

    const SLayout SHOWN = arranged(id, oldResting);

    // Most item-list changes move nothing: a window changing desktop bumps the
    // generation with the same icons in the same slots. Asking for 320ms of
    // frames to lerp a box onto itself is waste, so only a real difference
    // starts one.
    const auto SAMEBOX = [](const CBox& a, const CBox& b) {
        return std::abs(a.x - b.x) < 0.5 && std::abs(a.y - b.y) < 0.5 && std::abs(a.width - b.width) < 0.5 && std::abs(a.height - b.height) < 0.5;
    };
    if (SAMEBOX(SHOWN.pill, newResting.pill) && SHOWN.items.size() == newResting.items.size() &&
        std::ranges::equal(SHOWN.items, newResting.items, [&](const SItem& a, const SItem& b) { return a.subject == b.subject && SAMEBOX(a.iconBox, b.iconBox); }))
        return;

    auto& A        = g_arrange[id];
    A.active       = true;
    A.since        = Time::steadyNow();
    A.fromPill     = SHOWN.pill;
    A.fromShowApps = SHOWN.showApps;
    A.fromBoxes.clear();

    std::unordered_set<std::string> staying;
    for (const auto& ITEM : newResting.items)
        staying.insert(ITEM.subject);

    for (const auto& ITEM : SHOWN.items) {
        if (ITEM.subject.empty())
            continue;
        if (staying.contains(ITEM.subject))
            A.fromBoxes[ITEM.subject] = ITEM.iconBox;
        else if (ITEM.appear > 0.05F && !ITEM.icon.empty())
            A.exits.push_back({ITEM, Time::steadyNow()});
    }
}

bool arrangeAnimating(MONITORID id) {
    const auto IT = g_arrange.find(id);
    if (IT == g_arrange.end())
        return false;
    auto& A = IT->second;
    std::erase_if(A.exits, [](const SArrangeExit& X) { return ScrollOverview::Config::realElapsedMs(X.since) >= EXIT_MS; });
    return (A.active && ScrollOverview::Config::realElapsedMs(A.since) < ARRANGE_MS) || !A.exits.empty();
}

SLayout buildBaseLayout(PHLMONITOR monitor) {
    SLayout out;

    if (!monitor || !Config::getValue<bool>(KEY_ENABLED) || monitor->m_size.x < 1 || monitor->m_size.y < 1)
        return out;

    auto items = dockItems();

    // X46. A minimized window used to be appended to the pill as a preview slot
    // of its own. It is not any more: the pill's cells, centres and width must
    // not move when something is minimized, because the user asked for the
    // window to go INTO its icon rather than to sprout a second entry. Live
    // previews still exist -- X47 raises them on HOVER, out of its own layout.
    //
    // The slot's machinery outlived the decision by an empty vector: baseLayout
    // built `previews` from a list that was `const std::vector<PHLWINDOW>` and
    // never assigned, so every reader of SLayout::previews -- the slide offset,
    // the click hit-test, the render, the frame-pump predicate -- was walking a
    // container that could not have an element in it. Deleted, with the struct.

    // X32. The button is a slot of its own, so it widens the pill and competes
    // for icon size with everything else -- and it is the ONE thing that can put
    // a pill on screen by itself, on a desktop with nothing running and nothing
    // pinned. That is the reference's behaviour: `show-show-apps-button` does
    // not go away when the dash is empty.
    const bool SHOWAPPS = Config::getValue<bool>(KEY_SHOW_APPS);
    if (items.empty() && !SHOWAPPS)
        return out;

    const float SLOTS    = sc<float>(items.size()) + (SHOWAPPS ? 1.F : 0.F);
    const float SCALE    = std::max(monitor->m_scale, 0.01F);
    const float ICONMAX  = sc<float>(std::clamp(Config::getValue<int>(KEY_ICON_SIZE), 8, 512));
    const float FRACTION = std::clamp(Config::getValue<float>(KEY_FRACTION), 0.05F, 1.F);
    const auto  M        = metrics(items.size() + (SHOWAPPS ? 1 : 0), SLOTS, ICONMAX, FRACTION * sc<float>(monitor->m_size.x));

    // `extend-height` false and `always-center-icons` false: a floating pill
    // sized to its contents, centred on the monitor, clear of the bottom edge.
    const float XL = (sc<float>(monitor->m_size.x) - M.width) / 2.F;
    const float YL = sc<float>(monitor->m_size.y) - EDGE_MARGIN - M.height;

    out.pill = CBox{XL * SCALE, YL * SCALE, M.width * SCALE, M.height * SCALE};
    out.pill.round();
    out.rounding = std::max(0, sc<int>(std::round(sc<float>(ScrollOverview::Config::dockCornerRadius()) * SCALE)));

    float cursor = XL + CONTAINER_PAD_X + ITEM_MARGIN_X;
    out.items = std::move(items);
    for (auto& item : out.items) {
        item.iconBox = CBox{cursor * SCALE, (YL + ITEM_PAD_TOP) * SCALE, M.icon * SCALE, M.icon * SCALE};
        item.iconBox.round();

        cursor += M.icon + 2.F * ITEM_MARGIN_X;
    }

    // X32 Show Applications. `show-apps-at-top` FALSE puts it at the far end of
    // a bottom dock, and `show-apps-always-in-the-edge` TRUE is what keeps it
    // there rather than letting it float in with the apps -- so it lands after
    // the previews, which is the last slot the cursor has reached.
    //
    // `at_top` true is honoured by laying it out FIRST instead. The pair is the
    // reference's, and one without the other would be half a setting.
    if (SHOWAPPS) {
        const bool  FIRST = Config::getValue<bool>(KEY_SHOW_APPS_TOP) || !Config::getValue<bool>(KEY_SHOW_APPS_EDGE);
        const float X     = FIRST ? XL + CONTAINER_PAD_X + ITEM_MARGIN_X : cursor;

        out.showApps = CBox{X * SCALE, (YL + ITEM_PAD_TOP) * SCALE, M.icon * SCALE, M.icon * SCALE};
        out.showApps.round();

        // Laid out first: everything else slides one slot right, rather than
        // being overdrawn by it.
        if (FIRST) {
            const double SHIFT = (M.icon + 2.F * ITEM_MARGIN_X) * SCALE;
            for (auto& item : out.items)
                item.iconBox.x += SHIFT;
        }
    }

    return out;
}

const SLayout& baseLayout(PHLMONITOR monitor) {
    // An output that is not there has no pill, and it must not get a cache row
    // either -- MONITOR_INVALID is a key a real output could later be handed.
    static const SLayout EMPTY;
    if (!monitor)
        return EMPTY;

    // BEFORE the key is built, not inside buildBaseLayout: the key consults
    // dockItems(), which scans the app inventory and walks every window. A dock
    // that is switched off must not pay for an item model nobody will draw --
    // and damageIfChanged still runs on every pointer motion (user,
    // 2026-09-14).
    if (!Config::getValue<bool>(KEY_ENABLED) || monitor->m_size.x < 1 || monitor->m_size.y < 1)
        return EMPTY;

    SBaseLayoutKey key;
    key.items    = itemsGeneration();
    key.width    = monitor->m_size.x;
    key.height   = monitor->m_size.y;
    key.scale    = monitor->m_scale;
    key.iconSize = Config::getValue<int>(KEY_ICON_SIZE);
    key.radius   = ScrollOverview::Config::dockCornerRadius();
    key.fraction = Config::getValue<float>(KEY_FRACTION);
    key.flags    = (Config::getValue<bool>(KEY_ENABLED) ? 1U : 0U) |          //
        (Config::getValue<bool>(KEY_SHOW_APPS) ? 2U : 0U) |                   //
        (Config::getValue<bool>(KEY_SHOW_APPS_TOP) ? 4U : 0U) |               //
        (Config::getValue<bool>(KEY_SHOW_APPS_EDGE) ? 8U : 0U);

    // std::unordered_map, so the reference this hands back survives a later
    // call for a DIFFERENT monitor inserting its own row.
    auto& SLOT = g_baseLayouts[monitor->m_id];
    if (!SLOT.filled || !(SLOT.key == key)) {
        auto       fresh    = buildBaseLayout(monitor);
        // Only the ITEMS changed: that is a new arrangement, and it glides.
        // Anything else is a new geometry and snaps (see the arrangement note).
        auto       sameGeom = SLOT.key;
        sameGeom.items      = key.items;
        if (SLOT.filled && sameGeom == key)
            beginArrangement(monitor->m_id, SLOT.layout, fresh);
        else if (const auto IT = g_arrange.find(monitor->m_id); IT != g_arrange.end())
            IT->second = SArrange{};

        SLOT.key    = key;
        SLOT.filled = true;
        SLOT.layout = std::move(fresh);
    }

    return SLOT.layout;
}

// X30's state machine. Runs from damageIfChanged, so it advances on every frame
// the dock asks for and on every pointer motion, which is every moment either
// timer can expire.
// Y1. Defined with the panel further down; declared here because the autohide
// has to ask the same question the render does -- a panel the pointer is inside
// must hold the dock out, exactly as an open menu already does.
bool panelHoldsDock(PHLMONITOR monitor);

// V3. Is an icon on its way back to its cell after a missed drop? Declared here
// for the same reason panelHoldsDock is: the autohide has to ask, and the state
// it reads lives with the drag much further down.
bool ghostFlying();

void updateAutohide(PHLMONITOR monitor, const STally& tally) {
    if (!monitor)
        return;

    auto& R = g_reveal[monitor->m_id];

    // Pinned: autohide switched off, or the overview is open over this monitor.
    // Neither waits out `show-delay` -- that delay belongs to the pressure
    // barrier, and making the overview's own dock crawl in behind it would be
    // reading the reference wrong.
    // While the overview exists the visual is driven by openProgress, not this
    // timer. Keep `from` at the current fraction so a later handoff (overview
    // gone) continues from wherever the swipe or scale animation left the pill.
    if (const auto OV = scrollOverviewForMonitor(monitor); OV && Config::getValue<bool>(KEY_AUTOHIDE)) {
        R.from = 1.F - OV->openProgress();
        // Visual is driven by openProgress while the overview exists. Pinning
        // shown=true here made a gesture close hand off as "hidden, but going
        // visible", so the pill bounced up once then autohide hid it again.
        R.shown     = false;
        R.changedAt = Time::steadyNow();
        R.pending   = false;
        R.pressure  = 0.F;
        return;
    }

    if (!Config::getValue<bool>(KEY_AUTOHIDE)) {
        if (!R.shown) {
            R.from      = hiddenFraction(monitor);
            R.shown     = true;
            R.changedAt = Time::steadyNow();
        }
        R.pending  = false;
        R.pressure = 0.F;
        return;
    }

    // X31 `show-dock-urgent-notify` true: an app demanding attention brings the
    // dock out on its own. It is ORed into BOTH branches below, not just the
    // hidden one -- an urgent app also has to stop the pointer leaving from
    // sliding the dock away again, or the wiggle would play to nobody.
    //
    // ...WITHDRAWN (user, 2026-09-24): an urgent app no longer pins the dock
    // out. It hops from the edge instead (renderUrgentHop), and the dock stays
    // wherever autohide has it.

    bool       want      = false;
    // A reversal that catches the pill still on screen does not wait out a
    // delay; see the X77 note below.
    bool       immediate = false;

    const auto BOX   = monitor->logicalBox();
    const auto POINT = g_pInputManager->getMouseCoordsInternal();
    const bool ATEDGE = BOX.containsPoint(POINT) && POINT.y >= BOX.y + BOX.h - REVEAL_BAND;

    if (R.shown) {
        // Up already: it stays up while the pointer is on it, or while its
        // right-click menu is -- the menu floats ABOVE the pill, so reaching for
        // a row takes the pointer off the pill and would otherwise slide the
        // dock out from under the menu the user is aiming at.
        // Y1. The hover preview panel holds the dock for the same reason the
        // menu does, and more sharply: the panel floats a gap ABOVE the pill, so
        // reaching into it necessarily takes the pointer off the pill. Without
        // this the dock slides out from under the previews the user is reaching
        // for, which is the second half of the reported bug.
        // V3. And an icon being dragged holds it too, for the third time and
        // the sharpest reason: pulling one OFF the pill takes the pointer off
        // the pill by definition, so without this the dock slides away under a
        // gesture that started on it -- and render() returns early on an empty
        // pill, so the icon under the pointer would go with it. The return
        // flight holds it for the other half: an icon cannot land in a cell
        // that has left the screen.
        //
        // Position at the bottom edge of the monitor holds the dock as well:
        // the user can trigger the reveal anywhere along the bottom edge, and
        // holding the cursor there outside the physical dock pill must keep it
        // visible rather than oscillating (hide/show loop).
        if (menuOpen() || panelHoldsDock(monitor) || Stacks::holdsDock(monitor) || dragging() || ghostFlying() || ATEDGE)
            want = true;
        else if (CBox reach = slidPill(monitor); !reach.empty()) {
            // X77: the SLID pill, not the resting one. While it is still easing
            // out it sits BELOW its resting box, so a pointer on the part that
            // is actually on screen fell outside the resting box and read as
            // "left the dock" -- which started a hide the user never asked for.
            reach.height += EDGE_MARGIN * monitor->m_scale; // the margin under the pill still counts as on it
            want = reach.containsPoint((POINT - monitor->m_position) * monitor->m_scale);
        }
    } else {
        // `require-pressure-to-show`, plus a POSITION test that every pointer
        // can satisfy.
        //
        // The pressure barrier alone was measured dead on this guest, and the
        // reason generalises: its feed hooks `CInputManager::onMouseMoved`,
        // which is the RELATIVE motion path, while the pointer here is a
        // `qemu-qemu-usb-tablet` -- an ABSOLUTE device. No relative event ever
        // arrives, so pressure never accumulates and the dock never comes out.
        // (An earlier test "passed" only because ydotool synthesised relative
        // deltas, which is not how the user's pointer moves.) Position is the
        // one thing every device agrees on; `show_delay_ms` is what keeps it
        // from being twitchy, and pressure still works for a relative mouse.
        const bool PRESSED = R.pressure >= Config::getValue<float>(KEY_PRESSURE);
        want               = Config::getValue<bool>(KEY_REQ_PRESSURE) ? (ATEDGE && PRESSED) : ATEDGE;

        // X77 (user, 2026-08-06): "if I move my cursor off it, it starts hiding
        // and I can't unhide until it's done hiding".
        //
        // `shown` flips FALSE the moment the hide begins, which switched this
        // test to the 4px screen-edge band -- while the pill was still sitting
        // on screen under the cursor, 60-odd px above that band. So coming back
        // to a dock that is visibly right there could not re-show it, and the
        // user had to wait out the slide and push into the very bottom edge
        // again. dash-to-dock has no such window: its hover lives on the actor,
        // which stays interactive for the whole slide.
        //
        // Being ON the pill that is still on screen counts, and it counts
        // IMMEDIATELY -- `show-delay` belongs to the pressure barrier, not to a
        // pointer that never really left.
        if (ATEDGE && hiddenFraction(monitor) < 1.F)
            immediate = true;

        if (!want) {
            if (CBox reach = slidPill(monitor); !reach.empty()) {
                reach.height += EDGE_MARGIN * monitor->m_scale;
                if (reach.containsPoint((POINT - monitor->m_position) * monitor->m_scale)) {
                    want      = true;
                    immediate = true;
                }
            }
        }
    }

    // X34 `hotkeys-show-dock`, ORed in beside URGENT for the same reason: it has
    // to HOLD the pill up as well as bring it out, or the pointer sitting
    // anywhere else would slide it away under the numbers the press just drew.
    // X54. A flight HOLDS the dock out, for the same reason X34's hotkey overlay
    // does: outside the overview the pill is normally hidden, so a minimize had
    // no icon to fly to -- the target lookup failed and the flight was dropped
    // on its first frame, which is why it teleported. The dock now comes out for
    // the flight and slides away after it.
    want = want || overlayHoldsDock() || Flight::anyOn(monitor);

    if (want == R.shown) {
        R.pending = false;
        return;
    }

    if (!R.pending) {
        R.pending      = true;
        R.pendingSince = Time::steadyNow();
    }

    if (!immediate && ScrollOverview::Config::realElapsedMs(R.pendingSince) < sc<float>(std::max(0, Config::getValue<int>(want ? KEY_SHOW_DELAY : KEY_HIDE_DELAY))))
        return;

    R.from      = hiddenFraction(monitor);
    R.shown     = want;
    R.changedAt = Time::steadyNow();
    R.pending   = false;
    R.pressure  = 0.F;
}

}

SLayout layout(PHLMONITOR monitor) {
    SLayout out = monitor ? arranged(monitor->m_id, baseLayout(monitor)) : baseLayout(monitor);
    if (out.pill.empty())
        return out;

    const float F = hiddenFraction(monitor);
    if (F <= 0.F)
        return out;

    // Fully away: no pill and, because everything else asks this function, no
    // hit area either -- so a click lands on the window underneath instead of
    // being swallowed by a dock that is not on screen.
    if (F >= 1.F)
        return SLayout{};

    const double DY = slideOffsetPx(monitor, out.pill.height);
    out.pill.y += DY;
    for (auto& item : out.items)
        item.iconBox.y += DY;
    if (!out.showApps.empty())
        out.showApps.y += DY;

    return out;
}

namespace {

// How far the hover cell sits inside the pill, on every side. The icon is
// ITEM_PAD_TOP from the pill's top and CONTAINER_PAD_X + ITEM_MARGIN_X from its
// left, and those are equal by construction, so one number covers all four.
float hoverInset() {
    return ITEM_PAD_TOP - HOVER_PAD;
}

// The hovered cell, not the icon: GNOME's highlight is the .app-well-app box,
// which is the icon plus its padding. Symmetric now that the vertical padding is.
CBox hoverBox(const CBox& iconBox, float scale) {
    return iconBox.copy().expand(HOVER_PAD * std::max(scale, 0.01F)).round();
}

// The app's display name, over a dark rounded plate, centred on the icon and
// sitting above the pill.
// ponytail: no delay and no fade -- it appears with the highlight it belongs to.
// GNOME staggers it by ~500ms; add that if sweeping the dock reads as noisy.
// X31. The dot says RUNNING, and as of X32 that is a real distinction rather
// than a tautology: a pinned favourite with no window is an icon with no dot,
// and the dot is the only thing on the pill that tells the two apart. The
// caller gates on SItem::running.
//
// It hangs off the ICON, DOT_MARGIN_B below it -- NOT off the pill's bottom
// edge, which is where the first cut anchored it. Measured on the host: icon
// bottom 176, dot 178-182, so the dot's own bottom lands flush with the bottom
// of the hover highlight. Anchoring to the pill put it ITEM_PAD_BOTTOM -
// HOVER_PAD = 8px too low, and that gap is what read as "not 1:1".

// X87. Dock frost is LIVE (xray=false), desktop and overview, and only when
// Hyprland window blur is on. One surface, so it does not reintroduce the
// multi-card compound bug that forced shared sampling for strip/search.
// decoration:blur:* still applies when the dock keys are -1 (X71). Sampling
// m_blurFB here was wrong for the desktop path (no behind-chrome pre-blur
// exists) and broke once the shared draw path was rewritten.
static void queueDockFrost(PHLMONITOR monitor, const CBox& box, int rounding, float alpha) {
    if (!Stacks::drawableBox(box) || alpha <= 0.F) // box.empty() lets a NEGATIVE box through
        return;

    // Same switch window frost and overview chrome capture use. Dock keys of
    // -1 already mean "follow decoration:blur:*"; with the theme blur off a
    // frost pass would still run and look like a dock-only effect.
    if (!ScrollOverview::Config::getValue<bool>("decoration:blur:enabled"))
        return;

    COverviewBlurPassElement::SData blur;
    blur.box            = box;
    blur.rounding       = rounding;
    blur.roundingPower  = 2.F;
    blur.alpha          = alpha;
    blur.xray           = false; // live dual-kawase of whatever is behind the pill
    // -1 on these keys means leave decoration:blur:* alone (X71).
    blur.blurSize       = Config::getValue<int>(KEY_BLUR_SIZE);
    blur.blurPasses     = Config::getValue<int>(KEY_BLUR_PASSES);
    blur.blurBrightness = Config::getValue<float>(KEY_BLUR_BRIGHT);
    blur.blurNoise      = Config::getValue<float>(KEY_BLUR_NOISE);
    blur.blurContrast   = Config::getValue<float>(KEY_BLUR_CONTR);
    blur.blurVibrancy   = Config::getValue<float>(KEY_BLUR_VIBR);
    g_pHyprRenderer->m_renderPass.add(makeUnique<COverviewBlurPassElement>(blur));
}

std::vector<PHLWINDOW> itemWindows(const SItem& item);

// `dotSize` and `dotColor` are the caller's: both were read per ITEM -- a config
// lookup that hashes its key string, and a theme colour that re-reads the
// palette -- inside a loop over every icon of every monitor every frame (user,
// 2026-09-14). They do not vary within a pass.
void renderRunningDot(PHLMONITOR monitor, const SLayout& layout, const SItem& item, const STally& tally, int dotSize, const CHyprColor& dotColor) {
    const float SCALE = std::max(monitor->m_scale, 0.01F);
    const float SIZE  = std::max(1.F, sc<float>(dotSize) * SCALE);
    const float CX    = sc<float>(item.iconBox.x) + sc<float>(item.iconBox.width) / 2.F;
    const float TOP   = sc<float>(item.iconBox.y) + sc<float>(item.iconBox.height) + DOT_MARGIN_B * SCALE;

    const CHyprColor DOTCOL = dotColor;

    // Z12 (user, 2026-08-07, overrides X31's measured single dot): one dot per
    // WINDOW. Row one keeps today's exact position and grows outward from the
    // icon's centre; overflow wraps to a second row underneath; two rows max,
    // and windows beyond that are simply not shown.
    //
    // V6 scoped the CLICK to the active desktop and left this GLOBAL, on
    // purpose: the dots answer "is this app running, and how much of it" --
    // which is what makes it worth walking to another desktop for -- while the
    // click answers "do something here". A dot count that emptied out as you
    // walked away would be the reference's `isolate-workspaces`, which is a
    // config key of its own (dock:isolate_workspaces, default false) and is not
    // what the amendment asked for. `interesting()` is what that key moves, so
    // turning it on scopes both together, exactly as the reference does.
    size_t count = std::max<size_t>(itemWindowCount(item, tally), 1);

    const float  GAP    = 3.F * SCALE;
    const size_t PERROW = std::max<size_t>(1, sc<size_t>((sc<float>(item.iconBox.width) + GAP) / (SIZE + GAP)));
    count               = std::min(count, PERROW * 2);
    const size_t ROW1   = std::min(count, PERROW);
    const size_t ROW2   = count - ROW1;

    const auto drawRow = [&](size_t n, float top) {
        const float SPAN = sc<float>(n) * SIZE + sc<float>(n - 1) * GAP;
        for (size_t i = 0; i < n; ++i) {
            CRectPassElement::SRectData dot;
            dot.box   = CBox{std::round(CX - SPAN / 2.F + sc<float>(i) * (SIZE + GAP)), std::round(top), std::round(SIZE), std::round(SIZE)};
            dot.color = DOTCOL;
            // `border-radius: 5px` on a 5px box: a circle. Truncated, not
            // rounded -- a radius even slightly OVER half the box collapses the
            // circle into a diamond (seen on screen 2026-08-03).
            dot.round         = sc<int>(SIZE / 2.F);
            dot.roundingPower = 2.F;
            g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(dot));
        }
    };

    drawRow(ROW1, TOP);
    if (ROW2)
        drawRow(ROW2, TOP + SIZE + std::max(1.F, 2.F * SCALE));
}

// `_notificationBadgeCountToText`, same thresholds and the same rounding.
std::string badgeText(long long count) {
    if (count <= 9999)
        return std::to_string(count);

    char        buf[32] = {};
    const auto  N       = sc<double>(count);
    if (count < 100'000)
        std::snprintf(buf, sizeof(buf), "%.1fk", N / 1e3);
    else if (count < 1'000'000)
        std::snprintf(buf, sizeof(buf), "%.0fk", N / 1e3);
    else if (count < 100'000'000)
        std::snprintf(buf, sizeof(buf), "%.1fM", N / 1e6);
    else if (count < 1'000'000'000)
        std::snprintf(buf, sizeof(buf), "%.0fM", N / 1e6);
    else
        std::snprintf(buf, sizeof(buf), "%.1fB", N / 1e9);

    return buf;
}

void renderBadge(PHLMONITOR monitor, const SItem& item, bool badgesOn) {
    if (!item.uri.empty()) // X35: a location has no class to key a LauncherEntry on
        return;

    const auto COUNT = itemBadge(item, badgesOn);
    if (COUNT <= 0)
        return;

    const float SCALE = std::max(monitor->m_scale, 0.01F);
    const float ICON  = sc<float>(item.iconBox.width) / SCALE; // logical px: what the reference scales by

    // `_updateNotificationBadgeStyle`. The small-icon branch is not dead code --
    // the pill shrinks its icons rather than overflow once it hits the width cap,
    // which is exactly the case D2D's `iconSize < defaultIconSize` covers.
    const float MULT = ICON < BADGE_DEFAULT_ICON ? std::max(24.F, std::min(ICON + ICON * 0.3F, BADGE_DEFAULT_ICON)) / BADGE_DEFAULT_ICON : ICON / BADGE_DEFAULT_ICON;
    const float FONT = std::max(1.F, std::round(MULT * (BADGE_FONT_PT * BADGE_FONT_RATIO / 0.75F)));

    const auto  TEX = OverviewRender::text(badgeText(COUNT), CHyprColor{1.F, 1.F, 1.F, 1.F}, std::max(1, sc<int>(std::round(FONT * SCALE))), false, "", 0, BADGE_WEIGHT);
    if (!TEX || !TEX->ok())
        return;

    const float PADX = std::round(BADGE_PAD_X_EM * FONT * SCALE);
    const float PADY = std::round(BADGE_PAD_Y_EM * FONT * SCALE);
    const float W    = sc<float>(TEX->m_size.x) + 2.F * PADX;
    const float H    = sc<float>(TEX->m_size.y) + 2.F * PADY;

    const float X    = sc<float>(item.iconBox.x) + sc<float>(item.iconBox.width) - W - BADGE_MARGIN * SCALE;
    const float Y    = sc<float>(item.iconBox.y) + BADGE_MARGIN * SCALE;

    CRectPassElement::SRectData bg;
    bg.box   = CBox{std::round(X), std::round(Y), std::round(W), std::round(H)};
    bg.color = CHyprColor{1.F, 0.F, 0.F, 1.F}; // `background-color: red`
    // `border-radius: 1em` -- 1em is the font size, which on a badge this short
    // is always at least half its height, and css clamps a radius to that. So the
    // shape is a capsule, and H/2 is the number that stays one at any count.
    bg.round         = sc<int>(std::round(H / 2.F));
    bg.roundingPower = 2.F;
    g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(bg));

    CTexPassElement::SRenderData text;
    text.tex = TEX;
    text.box = CBox{std::round(X + PADX), std::round(Y + PADY), sc<float>(TEX->m_size.x), sc<float>(TEX->m_size.y)};
    text.a   = 1.F;
    g_pHyprRenderer->m_renderPass.add(makeUnique<CTexPassElement>(text));
}

// X34. The Hot-Key visual aid. `_updateNumberOverlay` numbers the icons 1..9,
// gives the TENTH a `0` -- the digit on its key, not its ordinal -- and leaves
// every icon past that without one.
//
// Reference divergence worth naming: the reference numbers `getAppIcons()`
// (every child with an `.icon`) but ACTIVATES out of a list filtered on
// `.child.app`, so once X35 puts trash and mounts in the dash the number drawn
// on an icon and the slot its key reaches can drift apart. Here both come from
// `LAYOUT.items`, which is ONE list, so they cannot.
void renderNumberOverlay(PHLMONITOR monitor, const SItem& item, int index) {
    if (!overlayUp() || index >= NUM_HOTKEYS)
        return;

    const int   DIGIT = (index + 1) % 10; // the tenth slot is `0`
    const float SCALE = std::max(monitor->m_scale, 0.01F);
    const float ICON  = sc<float>(item.iconBox.width) / SCALE; // logical px, as the reference measures

    const float FONT  = std::round(std::max(NUMBER_FONT_MIN, NUMBER_FONT_RATIO * ICON));
    const float SIZE  = std::round(FONT * NUMBER_BOX_RATIO * SCALE);

    const auto  TEX = OverviewRender::text(std::to_string(DIGIT), CHyprColor{1.F, 1.F, 1.F, 1.F}, std::max(1, sc<int>(std::round(FONT * SCALE))));
    if (!TEX || !TEX->ok())
        return;

    // `ActorAlign.START` on both axes and the CSS adds no margin: flush into the
    // icon's top-left corner.
    const float X = sc<float>(item.iconBox.x);
    const float Y = sc<float>(item.iconBox.y);

    CRectPassElement::SRectData bg;
    bg.box   = CBox{std::round(X), std::round(Y), SIZE, SIZE};
    bg.color = CHyprColor{0.F, 0.F, 0.F, NUMBER_BG_ALPHA};
    // Truncated for the same reason the running dot is: a radius even slightly
    // over half the box collapses the circle into a diamond.
    bg.round         = sc<int>(SIZE / 2.F);
    bg.roundingPower = 2.F;
    g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(bg));

    // `text-align: center`, and vertically too -- the label fills the bin.
    CTexPassElement::SRenderData text;
    text.tex = TEX;
    text.box = CBox{std::round(X + (SIZE - sc<float>(TEX->m_size.x)) / 2.F), std::round(Y + (SIZE - sc<float>(TEX->m_size.y)) / 2.F), sc<float>(TEX->m_size.x),
                    sc<float>(TEX->m_size.y)};
    text.a   = 1.F;
    g_pHyprRenderer->m_renderPass.add(makeUnique<CTexPassElement>(text));
}

// X32. A pinned favourite that is not running has no window class, so the class
// lookup that used to be the whole of this cannot name it -- its desktop entry
// is the only source. Ordered so a running app is still named by its class,
// which is what X27 measured.
std::string itemName(const SItem& item) {
    // The SCRATCHPAD carries no class, no desktop id and no uri, so every lookup
    // below answered with an empty string and its icon was the one slot in the
    // pill that showed no label at all on hover (user, 2026-09-12). Its name is
    // its own, the same way a location's is.
    if (item.scratchpad)
        return item.locationName;

    // X35. A location's name is its own -- no class to look it up by, and the
    // desktop-entry fallback below would answer "Files" for every one of them.
    if (!item.uri.empty())
        return item.locationName;

    if (const auto FROMCLASS = AppSearch::appNameForClass(item.wmClass); !FROMCLASS.empty())
        return FROMCLASS;

    if (const auto* ENTRY = AppSearch::entryForId(item.desktopId); ENTRY && !ENTRY->name.empty())
        return ENTRY->name;

    return item.wmClass;
}

// GNOME's dash label does not appear, it FADES: `_showLabel` eases opacity over
// DASH_ITEM_LABEL_SHOW_TIME. Ours arrived at full alpha, which is what the user
// reported missing. House entrance curve, because this is an entrance.
constexpr float TOOLTIP_FADE_MS     = 150.F;
constexpr float TOOLTIP_FADE_OUT_MS = 110.F; // exits are ~25% quicker, house rule

// The plate outlives the hover so it can fade AWAY; the caller passes an empty
// name when nothing is hovered and this keeps drawing the last one until it has
// gone. Anchor is remembered for the same reason -- the icon it belonged to is
// no longer under the pointer.
struct STooltip {
    std::string     name;
    std::string     subject; // icon or window, not the text; titles can tick without this moving
    CBox            anchor;
    float           bottomY = 0.F; // Y2; 0 = hang off the pill
    Time::steady_tp since   = Time::steadyNow();
    bool            out     = false;
    // The monitor it is on. ONE tooltip exists and every monitor's render()
    // calls renderTooltip: the monitor without the pointer asked for a fade-out
    // and then drew the plate at the other monitor's pixel coordinates -- the
    // label on the wrong screen (user, 2026-09-14). Only this monitor touches it.
    MONITORID       monitor = MONITOR_INVALID;
    // The real windows the hover is about, labelled inside themselves (see
    // renderWindowPeek). Kept with the plate so they fade out with it.
    std::vector<PHLWINDOWREF> peek;
};
STooltip g_tip;

// "App · Title" INSIDE THE WINDOW (user, 2026-09-24: "a little tooltip inside
// the window when I'm hovering the dock ... that way I know which terminal
// window I'm chatting with ... centered at the top inside the app window. This
// is not meant for overview"). Hovering an icon labels every one of the app's
// windows on screen; hovering a preview labels that one window. Same pill as
// the dock's own tooltip, on the same fade, so the two read as one gesture.
constexpr float PEEK_TOP_GAPL  = 10.F;
constexpr size_t PEEK_TITLE_MAX = 72;

std::vector<PHLWINDOW> peekWindowsOn(PHLMONITOR monitor, const std::vector<PHLWINDOW>& windows) {
    std::vector<PHLWINDOW> out;
    if (!monitor)
        return out;
    for (const auto& WINDOW : windows) {
        if (!WINDOW || !WINDOW->m_isMapped || WINDOW->m_monitor != monitor || !WINDOW->m_workspace || Minimize::isMinimized(WINDOW))
            continue;
        if (WINDOW->m_workspace == monitor->m_activeWorkspace || WINDOW->m_workspace == monitor->m_activeSpecialWorkspace)
            out.emplace_back(WINDOW);
    }
    return out;
}

void renderWindowPeek(PHLMONITOR monitor, float alpha) {
    if (alpha <= 0.F || g_tip.peek.empty() || scrollOverviewForMonitor(monitor))
        return;

    const float S = std::max(monitor->m_scale, 0.01F);
    for (const auto& REF : g_tip.peek) {
        const auto WINDOW = REF.lock();
        if (!WINDOW || !WINDOW->m_isMapped || Minimize::isMinimized(WINDOW) || WINDOW->m_monitor != monitor)
            continue;

        std::string app = AppSearch::appNameForClass(AppSearch::identityClass(WINDOW->m_class, WINDOW->m_title));
        if (app.empty())
            app = WINDOW->m_class;
        std::string title = WINDOW->m_title;
        if (title.size() > PEEK_TITLE_MAX) {
            size_t cut = PEEK_TITLE_MAX - 1;
            while (cut > 0 && (sc<unsigned char>(title[cut]) & 0xC0) == 0x80)
                --cut;
            title = title.substr(0, cut) + "…";
        }
        const std::string TEXT = title.empty() || title == app ? app : app + "  \u00b7  " + title;

        const auto  POS = WINDOW->m_realPosition->value() - monitor->m_position;
        const auto  SZ  = WINDOW->m_realSize->value();
        const CBox  BOX{POS.x * S, POS.y * S, SZ.x * S, SZ.y * S};
        const CBox  PLATE = OverviewRender::labelPillBox(monitor, TEXT, 0.0, 0.0, S);
        if (PLATE.height < 1 || PLATE.width > BOX.width || PLATE.height + PEEK_TOP_GAPL * S > BOX.height)
            continue; // a window too small to hold its own label keeps it off
        OverviewRender::labelPill(monitor, TEXT, BOX.x + BOX.width / 2.0, BOX.y + PEEK_TOP_GAPL * S + PLATE.height, alpha, S);
    }
}

std::string tooltipSubject(const PHLWINDOW& window) {
    return "w:" + std::to_string(reinterpret_cast<uintptr_t>(window.get()));
}

std::string tooltipSubject(const SItem& item) {
    if (!item.uri.empty())
        return "u:" + item.uri;
    if (!item.desktopId.empty())
        return "i:" + item.desktopId;
    if (!item.wmClass.empty())
        return "i:" + item.wmClass;
    return "i:" + item.icon;
}

// Y2. `bottomY` is the monitor-local RENDER y the plate's underside sits on. 0
// means "the pill", which is every caller that labels an icon. The hover panel
// passes its own top instead, so a preview's title rides above the previews
// rather than behind them.
// `subject` is the hovered icon or window. The fade restarts when THAT changes
// (sliding to the next icon), not when the same window's title ticks -- a
// streaming Grok/browser title used to punch the plate back to zero alpha
// every update.
void renderTooltip(PHLMONITOR monitor, const SLayout& layout, const CBox& anchor, const std::string& NAME, float bottomY = 0.F, const std::string& subject = {},
                   const std::vector<PHLWINDOW>& peek = {}) {
    const bool LIVE = !subject.empty() || !NAME.empty();

    // Nothing to show here and the tooltip is another monitor's: leave it alone.
    if (!LIVE && g_tip.monitor != monitor->m_id)
        return;

    if (LIVE) {
        const std::string& KEY = !subject.empty() ? subject : NAME;
        // New hover target, coming back from a fade-out, or arriving from
        // another monitor's dock: own entrance. Same target, new text: swap the
        // string, keep the alpha.
        if (KEY != g_tip.subject || g_tip.out || g_tip.monitor != monitor->m_id) {
            g_tip.monitor = monitor->m_id;
            g_tip.subject = KEY;
            g_tip.name    = NAME;
            g_tip.since   = Time::steadyNow();
            g_tip.out     = false;
        } else if (!NAME.empty())
            g_tip.name = NAME;
        g_tip.anchor = anchor;
        // Y2. Remembered with the anchor and for the same reason: the plate
        // outlives the hover so it can fade away, and by then the cell it was
        // labelling may be gone.
        g_tip.bottomY = bottomY;
        g_tip.peek.clear();
        for (const auto& WINDOW : peek)
            g_tip.peek.emplace_back(WINDOW);
    } else {
        if (g_tip.name.empty())
            return;
        if (!g_tip.out) {
            g_tip.out   = true;
            g_tip.since = Time::steadyNow();
        }
    }

    if (g_tip.name.empty())
        return;

    const float T = std::clamp(elapsedMs(g_tip.since) / (g_tip.out ? TOOLTIP_FADE_OUT_MS : TOOLTIP_FADE_MS), 0.F, 1.F);
    if (g_tip.out && T >= 1.F) {
        g_tip.name.clear();
        g_tip.subject.clear();
        g_tip.peek.clear();
        return;
    }

    // Entrances decelerate, exits accelerate: the house rule, both directions.
    const float ALPHA = g_tip.out ? 1.F - houseEase(false, T) : houseEase(true, T);

    renderWindowPeek(monitor, ALPHA);

    // The frames this fade needs are asked for by needsFrames(), which reads the
    // same g_tip clock this line does. A pointer resting on an icon emits no
    // motion, so nothing else on the desktop would ask for them.

    const float SCALE = std::max(monitor->m_scale, 0.01F);
    const float CX    = sc<float>(g_tip.anchor.x) + sc<float>(g_tip.anchor.width) / 2.F;

    // X57. Drawn by the SHARED pill, the same one the overview tile caption
    // uses -- the host styles both with one css rule and this is that decision
    // in code.
    const float BOTTOM = g_tip.bottomY > 0.F ? g_tip.bottomY : sc<float>(layout.pill.y);
    OverviewRender::labelPill(monitor, g_tip.name, CX, BOTTOM - OverviewRender::LABEL_PILL_GAP * SCALE, ALPHA, SCALE);
}

// [FLIGHT] revealNow STAYS in the dock -- it drives g_reveal, which autohide
// owns -- but the flight module calls it, so it needs external linkage: the
// definition now sits with the other cross-module entry points at the bottom
// of this file, in the named namespace.

// [FLIGHT] windowRenderBox / offscreenAimBox / flightAnchorBox moved to
// FlightController.cpp -- they answer "where does this flight end", which is
// the state machine's question.

// [FLIGHT] overviewOpenFor, restoreForFlight, the flight log's zdbg,
// reverseFlight and the minimize/restore impls moved to
// FlightController.cpp -- they are Flight::minimize / Flight::restore and the
// FSM's own reverse() now. dismissHoverPanelNow STAYS in the dock (it drives
// g_panel / g_tip) and is declared in Dock.hpp for the flight module.

// X24's click policy, straight off the reference config: `click-action`
// minimize, `activate-single-window` true, `minimize-shift` true,
// `shift-click-action` minimize.
//
// X32 adds the LAUNCH branch, which X27 could not have: a pinned favourite owns
// no window, so `click-action` has nothing to minimize and the reference falls
// through to launching the app.
// A dock icon is a favourite desktop id, a live window class, or both. Exact
// class match is not enough: a pinned Chromium webapp's window is
// `chrome-{host}_{path}-Default` while the favourite's id is the .desktop
// stem, and a second Chrome profile is a second class of the same id. Clicking
// the icon with those windows on THIS desktop must find them -- otherwise the
// ladder sees "none here" and launches a duplicate.
bool windowMatchesItem(const PHLWINDOW& window, const SItem& item, const SInterestCfg& cfg) {
    if (!interesting(window, cfg))
        return false;

    const auto CLASS = AppSearch::identityClass(window->m_class, window->m_title);

    if (!item.wmClass.empty() && CLASS == item.wmClass)
        return true;

    if (!item.desktopId.empty() && AppSearch::idForClass(CLASS) == item.desktopId)
        return true;

    return false;
}

std::vector<PHLWINDOW> itemWindows(const SItem& item) {
    if (item.wmClass.empty() && item.desktopId.empty())
        return {};

    const auto             CFG = interestCfg();
    std::vector<PHLWINDOW> out;
    for (const auto& WINDOW : Desktop::windowState()->windows()) {
        if (windowMatchesItem(WINDOW, item, CFG))
            out.emplace_back(WINDOW);
    }

    return out;
}

// The app's windows on the ACTIVE DESKTOP. A minimized window counts toward
// the desktop it will COME BACK TO: its own m_workspace is the stash
// (`special:minimized`) and answers no useful question, so this goes through
// the overview's one home-desktop helper rather than inventing a second answer.
//
// SCOPED IS THE CLICK, not the dock. runningClasses/dockItems stay global: an
// app running on another desktop keeps its icon and its running DOT.
std::vector<PHLWINDOW> itemWindowsHere(const SItem& item, PHLMONITOR monitor) {
    const auto ACTIVE = monitor ? monitor->m_activeWorkspace : nullptr;
    auto       all    = itemWindows(item);
    if (!ACTIVE)
        return all;

    std::vector<PHLWINDOW> out;
    for (const auto& WINDOW : all) {
        if (OvInternal::overviewGridHomeWorkspace(WINDOW) == ACTIVE->m_id)
            out.emplace_back(WINDOW);
    }

    return out;
}

std::string itemCycleKey(const SItem& item) {
    return !item.wmClass.empty() ? item.wmClass : item.desktopId;
}

PHLWINDOW frontWindow(const std::vector<PHLWINDOW>& windows) {
    for (const auto& WINDOW : windows) {
        if (WINDOW && !Minimize::isMinimized(WINDOW))
            return WINDOW;
    }

    return windows.empty() ? nullptr : windows.front();
}

// The one of `windows` focused most recently, by Hyprland's own focus history
// (newest last); frontWindow when none of them is in it yet.
PHLWINDOW recentWindow(const std::vector<PHLWINDOW>& windows) {
    const auto& HISTORY = Desktop::History::windowTracker()->fullHistory();
    for (auto it = HISTORY.rbegin(); it != HISTORY.rend(); ++it) {
        if (const auto WINDOW = it->lock(); WINDOW && std::ranges::find(windows, WINDOW) != windows.end())
            return WINDOW;
    }
    return frontWindow(windows);
}

// Seat is held while the pointer is on dock chrome so axis events cannot
// reach the window Hyprland hit-tests underneath (the pill is not a surface).
bool g_dockSeatHeld = false;

void stealDockSeat() {
    g_dockSeatHeld = true;
    if (!g_pSeatManager)
        return;
    g_pSeatManager->setPointerFocus(nullptr, {});
    g_pSeatManager->setKeyboardFocus(nullptr);
}

void restoreDockSeat() {
    if (!g_dockSeatHeld)
        return;
    g_dockSeatHeld = false;

    const auto WINDOW = Desktop::focusState()->window();
    if (!WINDOW || !WINDOW->m_isMapped)
        return;

    SP<CWLSurfaceResource> surf;
    if (WINDOW->wlSurface())
        surf = WINDOW->wlSurface()->resource();
    if (g_pSeatManager && surf)
        g_pSeatManager->setKeyboardFocus(surf);
}

// Go to a window on the desktop it belongs to. Preview clicks and "show me
// that window" must switch there -- never fly the window onto the current
// desktop (the restore flight comes up from the dock) and never toggle
// special:minimized (Omarchy slides that workspace in vertically).
void activateWindow(const PHLWINDOW& window) {
    if (!window)
        return;

    if (Minimize::isMinimized(window)) {
        Minimize::showOriginalWorkspace(window);
        Minimize::restore(window);
        return;
    }

    // A window parked on a SCRATCHPAD: pull that workspace up first. Focusing
    // it with the scratchpad down left the window focused but invisible, so the
    // icon (and a preview click) looked like it had done nothing at all.
    if (const auto SCRATCH = Scratchpad::workspaceFor(window))
        Scratchpad::show(SCRATCH);

    if (const auto WORKSPACE = window->m_workspace; WORKSPACE && !WORKSPACE->m_isSpecialWorkspace) {
        if (const auto MONITOR = WORKSPACE->m_monitor.lock(); MONITOR && MONITOR->m_activeWorkspace != WORKSPACE)
            MONITOR->changeWorkspace(WORKSPACE, false, true, false);
    }

    Desktop::windowState()->raise(window);
    Desktop::focusState()->fullWindowFocus(window, Desktop::FOCUS_REASON_SWITCH_TO_WINDOW_HARD);

    // Activate restores seat onto the window; take it back if the pointer is
    // still on the dock so the next tick cycles instead of scrolling the client.
    if (g_dockSeatHeld)
        stealDockSeat();
}

// X33 `scroll-action='cycle-windows'`, transcribed from `_cycleThroughWindows`.
//
// The list is REMEMBERED for CYCLE_MEMORY_MS rather than recomputed per tick,
// and that is the entire subtlety of the feature: activating a window reorders
// the compositor's stack, so a cycle driven off a freshly-read order would step
// between the same two windows forever. The memory is dropped when the app
// changes or the app's window COUNT changes -- the reference's own staleness
// test, which deliberately does not notice a reorder.
struct SCycle {
    std::string               wmClass;
    std::vector<PHLWINDOWREF> windows;
    int                       index = 0;
    Time::steady_tp           at;
};

std::optional<SCycle> g_cycle;

// Scroll (and CTRL+click) cycle. The list is explicit so the click can hand
// this desktop's windows and the wheel can hand the whole app. Walking to
// another desktop changes the count, which drops the memory and restarts the
// cycle at the top of the new set.
void cycleThroughWindows(const std::string& wmClass, bool reversed, const std::vector<PHLWINDOW>& WINDOWS) {
    if (WINDOWS.empty())
        return;

    const bool STALE = !g_cycle || g_cycle->wmClass != wmClass || g_cycle->windows.size() != WINDOWS.size() || elapsedMs(g_cycle->at) > CYCLE_MEMORY_MS;

    if (STALE) {
        g_cycle = SCycle{.wmClass = wmClass, .index = 0};
        g_cycle->windows.reserve(WINDOWS.size());
        for (const auto& WINDOW : WINDOWS)
            g_cycle->windows.emplace_back(WINDOW);
    }

    g_cycle->at = Time::steadyNow();

    const int COUNT = sc<int>(g_cycle->windows.size());
    g_cycle->index  = reversed ? (g_cycle->index - 1 + COUNT) % COUNT : (g_cycle->index + 1) % COUNT;

    // A window can die while the memory still holds it. Dropping the memory
    // rather than skipping the slot keeps the index and the list in step.
    const auto WINDOW = g_cycle->windows[g_cycle->index].lock();
    if (!WINDOW) {
        g_cycle.reset();
        return;
    }

    activateWindow(WINDOW);
}

// The reference's shared entry point for a cycle: if the app already has focus
// and nothing of it is shouting, step to its next window; otherwise the gesture
// is simply "bring this app forward", which is what an urgent window wants.
// Scroll on the icon uses this (global, every window of the app).
void cycleOrActivate(const SItem& item, bool reversed) {
    const auto WINDOWS = itemWindows(item);
    if (WINDOWS.empty())
        return;

    const auto FOCUSED   = Desktop::focusState()->window();
    const bool OWNSFOCUS = FOCUSED && windowMatchesItem(FOCUSED, item, interestCfg());

    if (OWNSFOCUS && !classUrgent(item.wmClass))
        cycleThroughWindows(itemCycleKey(item), reversed, WINDOWS);
    else
        activateWindow(frontWindow(WINDOWS));
}

// The overview gets out of the way, because a window that maps behind an open
// overview is a window the user cannot see they just started.
//
// X33 gave this a second caller and with it a second way to be asked: a RUNNING
// app middle-clicked for a new window owns a class but need not own a favourite,
// so `desktopId` can be empty where `idForClass` still answers.
// Z11 (user): spam-clicking a launcher must REPLAY the blow-up but exec ONE
// instance. The vulnerable window is exec -> first map: once a window exists,
// clicks take the toggle branch and never reach here. 30s covers a cold app;
// a slower one gets a deliberate second chance.
// ponytail: plain timestamps, no startup-notification protocol.
// (g_pendingLaunches and its window live above buildDockItems, which clears
// an entry once its launch has landed.)

// V3 widened this in two places, both in its signature, and both because a
// DRAG is a different statement from a click:
//   `onWorkspace`  where the app is to start. Null is "here", which is every
//                  caller that existed before -- a plain spawn lands on the
//                  active desktop and the overview gets out of its way.
//                  Non-null goes out through the exec rule and is SILENT, so
//                  the overview stays up and the session does not move.
//   `deliberate`   skip Z11's spam guard. Z11 exists because a launcher can be
//                  double-CLICKED faster than an app can map, and the second
//                  click is not a second intention. A drag is not that: it is a
//                  press, a journey and a release aimed at a named desktop, and
//                  "drop kitty on 3, then drop kitty on 5" must give two
//                  kittys. The guard's other half is untouched -- an app with
//                  windows already never reaches it (FIRSTINSTANCE), so this
//                  only differs for the app that is still coming up.
void launchApp(PHLMONITOR monitor, const SItem& item, PHLWORKSPACE onWorkspace = nullptr, bool deliberate = false) {
    // Y6. Armed here rather than in activateItem so every path that actually
    // starts something gets the blow-up -- a click on a closed app, a hotkey,
    // and `middle-click-action=launch` alike. GNOME calls animateLaunch() from
    // the same place, on activation rather than on the branch that chose it.
    if (!item.icon.empty() && item.iconBox.width >= 1 && item.iconBox.height >= 1)
        g_flight.launchZooms.emplace_back(SLaunchZoom{.icon = item.icon, .monitor = monitor ? monitor->m_id : MONITOR_INVALID, .from = item.iconBox, .startedAt = Time::steadyNow()});

    const bool  FIRSTINSTANCE = item.uri.empty() ? itemWindows(item).empty() : true;
    const auto  PENDKEY       = pendingLaunchKey(item);
    const float PENDING_MS    = item.uri.empty() ? LAUNCH_PENDING_MS : LOCATION_PENDING_MS;
    if (!deliberate && FIRSTINSTANCE && !PENDKEY.empty()) {
        if (const auto IT = g_pendingLaunches.find(PENDKEY); IT != g_pendingLaunches.end() && elapsedMs(IT->second) < PENDING_MS)
            return; // the zoom above replays; the exec does not
        g_pendingLaunches[PENDKEY] = Time::steadyNow();
    }

    // A SILENT launch leaves the overview where it is. The close below exists
    // because "a window that maps behind an open overview is a window the user
    // cannot see they just started" -- and that is exactly backwards for a drop
    // onto another desktop's card, where the overview IS how they see it happen.
    const bool SILENT = onWorkspace && onWorkspace != (monitor ? monitor->m_activeWorkspace : PHLWORKSPACE{});
    const auto closeOverview = [&] {
        if (SILENT)
            return;
        if (const auto OVERVIEW = scrollOverviewForMonitor(monitor); OVERVIEW)
            OVERVIEW->close();
    };

    // X35. `new-window` on a location is opening it -- the reference's fake
    // ShellApp answers `open_new_window` with the same `launch` either way.
    if (!item.uri.empty()) {
        Locations::activate(item.uri);
        closeOverview();
        return;
    }

    const auto  ID    = item.desktopId.empty() ? AppSearch::idForClass(item.wmClass) : item.desktopId;
    const auto* ENTRY = AppSearch::entryForId(ID);
    if (!ENTRY)
        return;

    // `workspace <name> silent`: Hyprland's own exec rule, matched against the
    // window when it maps through a token in the child's environment. The NAME
    // rather than the id, because that is what a workspace rule matches on and
    // what a named desktop would need.
    AppSearch::launchWithRule(*ENTRY, Config::getTerminal(), SILENT ? "workspace " + onWorkspace->m_name + " silent" : "");

    closeOverview();
}

void activateItem(PHLMONITOR monitor, const SItem& item, bool shift, bool ctrl = false) {
    // X35. A location has no windows to minimize: it is opened. Nautilus raises
    // the window it already has for a location rather than opening a second
    // one, so this is the reference's "focus it" too.
    // V11 V4. A STACK opens its fan or its grid rather than launching --
    // tested before the location branch, because a stack carries a uri too.
    // A pinned FILE has nothing to fan, and Stacks::open opens it instead.
    // The scratchpad's own icon: toggling the workspace is the WHOLE action.
    // There is no app to launch, nothing to stash, and no second instance to
    // spawn -- tested first because it carries neither a class nor a uri and
    // would otherwise fall through every branch below to launchApp.
    if (item.scratchpad) {
        const auto WORKSPACE = Scratchpad::byShortName(item.scratchpadName);

        // ...unless the overview is up, when it is NOT a toggle. The overview
        // is covering the scratchpad, so a click on the icon is the user asking
        // to SEE it -- and toggling would answer that by pulling it down, which
        // reads as the click doing nothing at all (user, 2026-09-22).
        //
        // The overview has to COMMIT to a scratchpad window, not merely close
        // with the scratchpad shown. Closing hands focus back to its own
        // selection twice -- once in close(), and again when the close animation
        // ends and restoreOverviewKeyboard refocuses closeOnWindow -- and
        // focusing a window outside an open special workspace makes Hyprland
        // hide it. The first version did show()-then-close() and the scratchpad
        // flashed up for the length of the animation and went (user,
        // 2026-09-23: "just closes overview"). selectAndClose makes a window on
        // the scratchpad the thing both refocuses land on.
        if (const auto OVERVIEW = openScrollOverviewForMonitor(monitor); OVERVIEW) {
            Scratchpad::show(WORKSPACE);
            if (const auto WINDOWS = Scratchpad::windowsOn(WORKSPACE); !WINDOWS.empty())
                OVERVIEW->selectAndClose(WINDOWS.front());
            else
                OVERVIEW->close();
            return;
        }

        Scratchpad::toggle(WORKSPACE);
        return;
    }

    if (item.stack) {
        Stacks::open(monitor, item, item.iconBox);
        return;
    }

    if (!item.uri.empty()) {
        launchApp(monitor, item);
        return;
    }

    // Scroll cycles. Click does not. CTRL+click is the leftover cycle from Y4
    // (scoped to this desktop when the app is here, otherwise the whole app).
    const auto here = itemWindowsHere(item, monitor);
    if (ctrl) {
        const auto CYCLE = !here.empty() ? here : itemWindows(item);
        cycleThroughWindows(itemCycleKey(item), false, CYCLE);
        return;
    }

    // SWITCH BACK FIRST (user, 2026-09-24: "I have multiple ghostty windows
    // open. I switch to zen. I then click on the ghostty icon in dock. First
    // click on that should immediately switch me back to the last used window
    // of that process, same with every other app"). While another app has the
    // focus, a click is "take me back to this app": its most recently used
    // window, wherever that is -- another desktop, the scratchpad, minimized.
    // Only once the app HAS the focus do the rules below (minimize/restore the
    // windows here) apply, so a second click still puts it away.
    if (const auto ALL = itemWindows(item); !ALL.empty()) {
        const auto FOCUSED = Desktop::focusState()->window();
        if (!FOCUSED || std::ranges::find(ALL, FOCUSED) == ALL.end()) {
            const auto TARGET = recentWindow(ALL);
            if (const auto OVERVIEW = scrollOverviewForMonitor(monitor); OVERVIEW && !Minimize::isMinimized(TARGET))
                OVERVIEW->selectAndClose(TARGET);
            else
                activateWindow(TARGET);
            return;
        }
    }

    // "Click it again and it goes away", for an app that lives in the
    // scratchpad. Its windows are not on any desktop, so `here` is empty and
    // the branches below would focus a window that is already up -- the click
    // read as doing nothing. Scoped to the scratchpad SHOWN on this monitor and
    // to an app entirely inside it: an app with a window out on a desktop too
    // keeps the ordinary rules, because that window is what a click is about.
    if (here.empty()) {
        if (const auto SHOWN = Scratchpad::shownOn(monitor)) {
            const auto ALL = itemWindows(item);
            if (!ALL.empty() && std::ranges::all_of(ALL, [&](const auto& window) { return Scratchpad::workspaceFor(window) == SHOWN; })) {
                Scratchpad::hide(SHOWN);
                return;
            }
        }
    }

    // Window on THIS desktop: minimize/maximize that window. Never spawn a
    // second instance of an app you can already see (or just hid) here.
    // Visible -> minimize; hidden (or mid-restore flight) -> restore.
    // Scroll, not click, is what cycles when there is more than one.
    if (!here.empty()) {
        std::vector<PHLWINDOW> visible;
        for (const auto& WINDOW : here) {
            if (!Minimize::isMinimized(WINDOW))
                visible.emplace_back(WINDOW);
        }

        // Z5. A window mid-RESTORE-flight still reads as minimized (it stays
        // stashed until landing), so without this a spam of clicks re-restored
        // forever instead of toggling. Count it as visible; the next minimize
        // then reverses its flight in place.
        for (const auto& WINDOW : here) {
            if (Flight::hasFlight(WINDOW) && Flight::isRestoreFlight(WINDOW) && Minimize::isMinimized(WINDOW))
                visible.emplace_back(WINDOW);
        }

        if (visible.empty()) {
            for (const auto& WINDOW : here)
                Flight::restore(monitor, WINDOW);
            return;
        }

        for (const auto& WINDOW : visible)
            Flight::minimize(monitor, WINDOW);
        return;
    }

    // Running only on another desktop: go there and focus it. Launching a
    // fresh window here is what left the original stashed on special:minimized
    // after a desktop switch ("locked regardless of desktop").
    if (const auto ALL = itemWindows(item); !ALL.empty()) {
        activateWindow(frontWindow(ALL));
        return;
    }

    launchApp(monitor, item);
}

// X33. `middle-click-action` and `shift-middle-click-action`, both `launch` in
// the reference.
//
// The structure is the reference's, not a switch of my own: the whole action
// table lives inside `if (this.running)`, and a NOT-running app ignores the
// setting entirely and just launches. That is why the empty check comes first
// and takes no notice of `action`.
//
// `minimize` routes back through activateItem so the two buttons cannot drift
// apart -- it is the left button's action, and a user who sets middle to
// `minimize` means that one.
//
// V6 scoped the left button, and this table is deliberately NOT scoped with it.
// The amendment is about "i click its icon"; these are the reference's actions
// over an app, and `quit` that killed only the windows on this desktop, or
// `skip` that could not reach the window you can see running elsewhere, would be
// a different feature nobody asked for. `minimize` is the one action V6 names,
// and it inherits the scoping through activateItem, which is the point of
// routing it there.
void applyClickAction(PHLMONITOR monitor, const SItem& item, const std::string& action, bool shift) {
    // X35. Every one of the reference's five actions is about an app's windows,
    // and a location has none -- `launch` is the only one it can answer, and
    // opening it is what that means here.
    if (!item.uri.empty()) {
        activateItem(monitor, item, shift);
        return;
    }

    const auto WINDOWS = itemWindows(item);

    if (WINDOWS.empty()) {
        launchApp(monitor, item);
        return;
    }

    if (action == "launch")
        launchApp(monitor, item);
    else if (action == "minimize")
        activateItem(monitor, item, shift);
    else if (action == "cycle-windows")
        cycleOrActivate(item, false);
    else if (action == "quit") {
        for (const auto& WINDOW : WINDOWS)
            WINDOW->sendClose();
    } else if (action == "skip")
        activateWindow(WINDOWS.front());
}

// ---------------------------------------------------------------- X28 menu --

enum class ERow : uint8_t {
    Separator,
    Action,
    Window,
    Details,
    Favorite,
    Quit,
    EmptyTrash,
    Mount,
    Unmount,
    Eject,
    StackView,
    StackSort,
    StackOpen,
    StackUnpin,
    // user, 2026-09-22: "add a force quit option ... add quality of life features".
    NewWindow,
    MinimizeAll,
    RestoreAll,
    ForceQuit,
    // Get Info's own view (user, 2026-09-24). The menu swaps to these in place.
    InfoLine,   // "Version  1.3.1": click copies the value (kept whole in `exec`)
    InfoReveal, // Show in Files
    InfoCopyAll,
    InfoBack,
    // The overview, filtered on the app (user, 2026-09-24).
    ShowWindows,
    // Dock stacks A: the stack's own menu.
    StackRecent,      // exec = the file's path
    StackSortKey,     // exec = sort name
    StackDisplayKey,  // exec = "stack" | "folder"
    StackViewKey,     // exec = "auto" | "grid"
    StackShowParent,
    StackRename,
    StackRenameField,
    StackTrashNewest,
    StackEmptyAsk,
    StackEmptyConfirm,
    StackEmptyCancel,
};

// OMARCHY-STYLE ROWS: a Nerd Font glyph column, the label, and a dim hint on the
// right -- the way the Omarchy menu lays its own rows out. The glyphs are all
// from the Font Awesome block, which every Nerd Font release carries.
namespace MenuGlyph {
    inline constexpr const char* NEW      = "\uf067"; // plus
    inline constexpr const char* ACTION   = "\uf054"; // chevron-right
    inline constexpr const char* WINDOW   = "\uf2d0"; // window-maximize
    inline constexpr const char* MINIMIZE = "\uf2d1"; // window-minimize
    inline constexpr const char* RESTORE  = "\uf2d2"; // window-restore
    inline constexpr const char* DETAILS  = "\uf05a"; // info-circle
    inline constexpr const char* FAV_ADD  = "\uf005"; // star
    inline constexpr const char* FAV_DEL  = "\uf006"; // star-o
    inline constexpr const char* QUIT     = "\uf00d"; // times
    inline constexpr const char* KILL     = "\uf05e"; // ban
    inline constexpr const char* TRASH    = "\uf1f8"; // trash
    inline constexpr const char* DRIVE    = "\uf0a0"; // hdd
    inline constexpr const char* EJECT    = "\uf052"; // eject
    inline constexpr const char* FOLDER   = "\uf07c"; // folder-open
    inline constexpr const char* GRID     = "\uf00a"; // th
    inline constexpr const char* SORT     = "\uf0dc"; // sort
    inline constexpr const char* UNPIN    = "\uf08d"; // thumb-tack
    inline constexpr const char* BACK     = "\uf053"; // chevron-left
    inline constexpr const char* COPY     = "\uf0c5"; // copy
    inline constexpr const char* PACKAGE  = "\uf1b2"; // cube
    inline constexpr const char* TAG      = "\uf02b"; // tag
    inline constexpr const char* TERMINAL = "\uf120"; // terminal
    inline constexpr const char* FILE     = "\uf15b"; // file
    inline constexpr const char* LINK     = "\uf0c1"; // link
    inline constexpr const char* CLOCK    = "\uf017"; // clock
    inline constexpr const char* DISK     = "\uf1c0"; // database
    inline constexpr const char* SOURCE   = "\uf0e8"; // sitemap
    inline constexpr const char* TEXT     = "\uf036"; // align-left
    inline constexpr const char* CHECK    = "\uf00c"; // check
    inline constexpr const char* PENCIL   = "\uf040"; // pencil
}

struct SRow {
    ERow         kind = ERow::Separator;
    std::string  label;
    std::string  exec;   // ERow::Action
    PHLWINDOWREF window; // ERow::Window
    CBox         box;    // monitor-local RENDER px, filled by renderMenu
    std::string  glyph;          // Nerd Font glyph for the leading column; empty = none
    std::string  hint;           // dim, right-aligned: a window's desktop, say
    bool         danger = false; // Force Quit: drawn in the warning colour
};

struct SMenu {
    MONITORID monitor = MONITOR_INVALID;

    // Get Info: the menu is showing the app's info rows, rebuilt whenever the
    // package lookup lands (AppInfo::generation() moves past infoGen).
    bool     infoView = false;
    uint64_t infoGen  = 0;

    // X35. The whole item, not just its class: a location has no class, and the
    // rows it opens are decided by its uri/mount/trashEmpty instead.
    SItem             item;
    CBox              anchor; // the icon cell the menu hangs off
    CBox              box;    // the whole plate, filled by renderMenu
    std::vector<SRow> rows;

    // Dock stacks A: Rename in Dock turns the menu into a one-line field.
    bool        editing = false;
    std::string editText;
    bool        editFresh = false; // the name is "selected": the first key replaces it
};

std::optional<SMenu> g_menu;

// Back off to a lead byte so a truncated title cannot end mid-codepoint and
// hand the text renderer a broken string.
std::string elide(const std::string& text) {
    if (text.size() <= MENU_LABEL_MAX)
        return text;

    size_t cut = MENU_LABEL_MAX;
    while (cut > 0 && (sc<unsigned char>(text[cut]) & 0xC0) == 0x80)
        --cut;

    return text.substr(0, cut) + "…";
}

// V11 V4. What the stack's own store says about it, for the menu labels --
// the row has to name the state it will move TO, so it has to read the state
// it is in.

// Spec §13.9 X28, in the order the spec lists them: .desktop Actions, window
// list, Show Details, Add/Remove from Favorites, Quit.
// A pinned app that is not running has a desktop id and no window class, so
// every lookup keyed on the class came back empty for it.
std::string itemDesktopId(const SItem& item) {
    return !item.desktopId.empty() ? item.desktopId : AppSearch::idForClass(item.wmClass);
}

std::vector<SRow> buildRows(const SItem& item) {
    std::vector<SRow> rows;

    // X35. A location's menu is its `list_actions()` and nothing else -- no
    // window list, no favourite, no Quit: none of those has an answer for a
    // thing that is not an app. An empty trash lists no action at all, so its
    // menu is a bare plate, which is what the reference shows too.
    // V11 V4. A stack's settings ARE this menu -- the spec is explicit that
    // there is no external prefs panel, which is the reference's one real
    // design mistake (its stack icon has no context menu at all, and a folder
    // can only be unpinned from a GTK preferences window two clicks away).
    if (item.stack) {
        // Dock stacks A: the stack's menu, in eight groups.
        const Stacks::SStack* S = nullptr;
        for (const auto& X : Stacks::all())
            if (X.path == item.stackPath)
                S = &X;
        if (item.stackDir && S) {
            // 1. Recent items: the 5 newest files, with how long ago they arrived.
            auto L = StackReader::listing(S->path);
            std::erase_if(L.entries, [](const StackModel::SEntry& e) { return e.dir; });
            StackModel::sortEntries(L.entries, StackModel::ESort::Added);
            const int64_t NOW = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            for (size_t i = 0; i < std::min<size_t>(5, L.entries.size()); ++i)
                rows.emplace_back(SRow{.kind  = ERow::StackRecent,
                                       .label = elide(L.entries[i].name),
                                       .exec  = L.entries[i].path,
                                       .glyph = MenuGlyph::FILE,
                                       .hint  = StackModel::relativeAge(NOW, L.entries[i].btime)});
            if (!L.entries.empty())
                rows.emplace_back(SRow{.kind = ERow::Separator});

            // 2-4. Sort by / Display as / View as, the current one checked.
            const auto CHECKED = [](bool on) { return on ? std::string{MenuGlyph::CHECK} : std::string{}; };
            static const char* SORTNAMES[] = {"Name", "Date Added", "Date Modified", "Kind"};
            for (const auto K : {StackModel::ESort::Name, StackModel::ESort::Added, StackModel::ESort::Modified, StackModel::ESort::Kind})
                rows.emplace_back(SRow{.kind  = ERow::StackSortKey,
                                       .label = std::string("Sort by ") + SORTNAMES[sc<int>(K)],
                                       .exec  = StackModel::sortName(K),
                                       .glyph = CHECKED(S->sort == K)});
            rows.emplace_back(SRow{.kind = ERow::Separator});
            rows.emplace_back(SRow{.kind = ERow::StackDisplayKey, .label = "Display as Stack", .exec = "stack", .glyph = CHECKED(S->display == StackModel::EDisplay::Stack)});
            rows.emplace_back(SRow{.kind = ERow::StackDisplayKey, .label = "Display as Folder", .exec = "folder", .glyph = CHECKED(S->display == StackModel::EDisplay::Folder)});
            rows.emplace_back(SRow{.kind = ERow::StackViewKey, .label = "View as Automatic", .exec = "auto", .glyph = CHECKED(S->view == StackModel::EView::Auto)});
            rows.emplace_back(SRow{.kind = ERow::StackViewKey, .label = "View as Grid", .exec = "grid", .glyph = CHECKED(S->view == StackModel::EView::Grid)});
            rows.emplace_back(SRow{.kind = ERow::Separator});
        }
        // 5-6.
        rows.emplace_back(SRow{.kind = ERow::StackOpen, .label = "Open in Files", .glyph = MenuGlyph::FOLDER});
        rows.emplace_back(SRow{.kind = ERow::StackShowParent, .label = "Show in Files", .glyph = MenuGlyph::FOLDER});
        rows.emplace_back(SRow{.kind = ERow::StackRename, .label = "Rename in Dock\u2026", .glyph = MenuGlyph::PENCIL});
        // 7. Both trash, never delete; in the warning colour.
        if (item.stackDir) {
            rows.emplace_back(SRow{.kind = ERow::Separator});
            rows.emplace_back(SRow{.kind = ERow::StackTrashNewest, .label = "Move Newest to Trash", .glyph = MenuGlyph::TRASH, .danger = true});
            rows.emplace_back(SRow{.kind = ERow::StackEmptyAsk, .label = "Empty Folder\u2026", .glyph = MenuGlyph::TRASH, .danger = true});
        }
        // 8. Removing a stack never touches the folder.
        rows.emplace_back(SRow{.kind = ERow::Separator});
        rows.emplace_back(SRow{.kind = ERow::StackUnpin, .label = "Remove from Dock", .glyph = MenuGlyph::UNPIN});
        return rows;
    }

    if (!item.uri.empty()) {
        if (item.mount) {
            // An fstab point that is not up yet offers the opposite action --
            // and no Eject, because there is nothing mounted to eject.
            if (!item.mounted)
                rows.emplace_back(SRow{.kind = ERow::Mount, .label = "Mount", .glyph = MenuGlyph::DRIVE});
            else {
                rows.emplace_back(SRow{.kind = ERow::Unmount, .label = "Unmount", .glyph = MenuGlyph::DRIVE});
                if (item.canEject)
                    rows.emplace_back(SRow{.kind = ERow::Eject, .label = "Eject", .glyph = MenuGlyph::EJECT});
            }
        } else if (!item.trashEmpty)
            rows.emplace_back(SRow{.kind = ERow::EmptyTrash, .label = "Empty Trash", .glyph = MenuGlyph::TRASH});

        return rows;
    }

    const auto& wmClass = item.wmClass;
    const auto  WINDOWS = itemWindows(item);
    const auto  ID      = itemDesktopId(item);

    // New Window first when there is something to launch: the one thing a
    // running app's icon cannot do on a click, which focuses instead.
    if (!ID.empty() && !WINDOWS.empty())
        rows.emplace_back(SRow{.kind = ERow::NewWindow, .label = "New Window", .glyph = MenuGlyph::NEW});

    for (const auto& ACTION : AppSearch::actionsForId(ID))
        rows.emplace_back(SRow{.kind = ERow::Action, .label = ACTION.name, .exec = ACTION.exec, .glyph = MenuGlyph::ACTION});

    if (!rows.empty() && !WINDOWS.empty())
        rows.emplace_back(SRow{.kind = ERow::Separator});

    // Each window with WHERE it is: its desktop, the scratchpad, or minimized.
    // A list of identical app names with nothing to tell them apart was the
    // commonest reason to open the hover panel instead.
    size_t minimized = 0;
    for (const auto& WINDOW : WINDOWS) {
        std::string where;
        if (Minimize::isMinimized(WINDOW)) {
            where = "minimized";
            ++minimized;
        } else if (const auto SCRATCH = Scratchpad::workspaceFor(WINDOW))
            where = Scratchpad::label(SCRATCH);
        else if (WINDOW->m_workspace)
            where = WINDOW->m_workspace->m_name;
        rows.emplace_back(SRow{.kind = ERow::Window, .label = elide(WINDOW->m_title.empty() ? wmClass : WINDOW->m_title), .window = WINDOW, .glyph = MenuGlyph::WINDOW, .hint = where});
    }

    // Every window of the app at once, where they can be compared: the overview
    // filtered on the app's name.
    if (!WINDOWS.empty())
        rows.emplace_back(SRow{.kind = ERow::ShowWindows, .label = "Show Windows", .glyph = MenuGlyph::GRID});

    // Whole-app minimize / restore, offered in whichever direction does
    // something. The flight into (and out of) the icon is the dock's own.
    if (!WINDOWS.empty()) {
        if (minimized < WINDOWS.size())
            rows.emplace_back(SRow{.kind = ERow::MinimizeAll, .label = WINDOWS.size() > 1 ? "Minimize All" : "Minimize", .glyph = MenuGlyph::MINIMIZE});
        if (minimized > 0)
            rows.emplace_back(SRow{.kind = ERow::RestoreAll, .label = minimized > 1 ? "Show All" : "Show", .glyph = MenuGlyph::RESTORE});
    }

    // A class no desktop entry claims has nothing to show the details of and
    // nothing to favourite by id, so both rows are left out rather than drawn
    // dead. GNOME does the same with an unmatched window.
    if (!ID.empty()) {
        if (!rows.empty())
            rows.emplace_back(SRow{.kind = ERow::Separator});

        const bool FAV = std::ranges::contains(favorites(), ID);
        rows.emplace_back(SRow{.kind = ERow::Favorite, .label = FAV ? "Remove from Favorites" : "Add to Favorites", .glyph = FAV ? MenuGlyph::FAV_DEL : MenuGlyph::FAV_ADD});
        rows.emplace_back(SRow{.kind = ERow::Details, .label = "Get Info", .glyph = MenuGlyph::DETAILS});
    }

    if (!WINDOWS.empty()) {
        if (!rows.empty())
            rows.emplace_back(SRow{.kind = ERow::Separator});
        rows.emplace_back(SRow{.kind = ERow::Quit, .label = WINDOWS.size() > 1 ? "Quit " + std::to_string(WINDOWS.size()) + " Windows" : "Quit", .glyph = MenuGlyph::QUIT});
        // The app asked nicely and did not go (a hung client, a modal it is
        // stuck behind): SIGKILL every process behind its windows. Last, and in
        // the warning colour, because it loses unsaved work.
        rows.emplace_back(SRow{.kind = ERow::ForceQuit, .label = "Force Quit", .glyph = MenuGlyph::KILL, .danger = true});
    }

    return rows;
}

// GET INFO's rows: the .desktop entry's own answers straight away, the package
// manager's as soon as AppInfo has them. Each value row carries the WHOLE value
// in `exec` (the hint is elided to fit) so a click copies all of it.
std::vector<SRow> infoRows(const SItem& item) {
    std::vector<SRow> rows;
    const auto*       ENTRY = AppSearch::entryForId(itemDesktopId(item));

    const auto line = [&](const char* glyph, const std::string& key, std::string value) {
        if (value.empty())
            return;
        // Paths read from home, the way a person would say them.
        if (const char* HOME = std::getenv("HOME"); HOME && *HOME && value.starts_with(HOME))
            value = "~" + value.substr(std::strlen(HOME));
        std::string shown = value;
        if (shown.size() > 46) {
            size_t cut = 45;
            while (cut > 0 && (sc<unsigned char>(shown[cut]) & 0xC0) == 0x80)
                --cut;
            shown = shown.substr(0, cut) + "…";
        }
        rows.emplace_back(SRow{.kind = ERow::InfoLine, .label = key, .exec = value, .glyph = glyph, .hint = shown});
    };

    if (ENTRY) {
        line(MenuGlyph::TAG, "Name", ENTRY->name);
        line(MenuGlyph::TEXT, "About", ENTRY->comment.empty() ? ENTRY->generic : ENTRY->comment);
    } else
        line(MenuGlyph::TAG, "Class", item.wmClass);

    rows.emplace_back(SRow{.kind = ERow::Separator});

    const auto INFO = ENTRY ? AppInfo::lookup(ENTRY->path, ENTRY->exec) : AppInfo::SInfo{.ready = true, .fields = {}};
    if (!INFO.ready)
        rows.emplace_back(SRow{.kind = ERow::InfoLine, .label = "Looking up the package…", .glyph = MenuGlyph::PACKAGE});
    for (const auto& [KEY, VALUE] : INFO.fields) {
        const char* G = KEY == "Version" ? MenuGlyph::TAG :
            KEY == "Source"              ? MenuGlyph::SOURCE :
            KEY == "Package"             ? MenuGlyph::PACKAGE :
            KEY == "Size"                ? MenuGlyph::DISK :
            KEY == "Installed"           ? MenuGlyph::CLOCK :
                                           MenuGlyph::LINK;
        line(G, KEY, VALUE);
    }

    if (ENTRY) {
        // Exec= without its field codes (%U, %f, ...), which are placeholders,
        // not part of the command anyone would type.
        std::string cmd;
        for (size_t i = 0; i < ENTRY->exec.size(); ++i) {
            if (ENTRY->exec[i] == '%' && i + 1 < ENTRY->exec.size()) {
                ++i;
                continue;
            }
            cmd += ENTRY->exec[i];
        }
        while (!cmd.empty() && cmd.back() == ' ')
            cmd.pop_back();
        line(MenuGlyph::TERMINAL, "Command", cmd);
        line(MenuGlyph::FILE, "Desktop file", ENTRY->path);
    }

    // Clickable, unlike the lines around it: it opens the windows it counts.
    if (const auto N = itemWindows(item).size(); N > 0)
        rows.emplace_back(SRow{.kind = ERow::ShowWindows, .label = "Open windows", .glyph = MenuGlyph::WINDOW, .hint = std::to_string(N) + "  \uf054"});

    rows.emplace_back(SRow{.kind = ERow::Separator});
    if (ENTRY)
        rows.emplace_back(SRow{.kind = ERow::InfoReveal, .label = "Show in Files", .exec = ENTRY->path, .glyph = MenuGlyph::FOLDER});
    rows.emplace_back(SRow{.kind = ERow::InfoCopyAll, .label = "Copy Info", .glyph = MenuGlyph::COPY});
    rows.emplace_back(SRow{.kind = ERow::Separator});
    rows.emplace_back(SRow{.kind = ERow::InfoBack, .label = "Back", .glyph = MenuGlyph::BACK});
    return rows;
}

void activateRow(PHLMONITOR monitor, const SItem& item, const SRow& row) {
    const auto& wmClass = item.wmClass;

    switch (row.kind) {
        case ERow::Separator: return;

        // V11 V4. Per-STACK, and written straight to the store: the toggle is
        // the setting, so there is nowhere else for it to live.
        case ERow::StackRecent: Locations::activate("file://" + row.exec); return;
        case ERow::StackSortKey:
            if (const auto K = StackModel::sortFromName(row.exec))
                Stacks::setSort(item.stackPath, *K);
            return;
        case ERow::StackDisplayKey: Stacks::setDisplay(item.stackPath, row.exec == "folder" ? StackModel::EDisplay::Folder : StackModel::EDisplay::Stack); return;
        case ERow::StackViewKey: Stacks::setViewMode(item.stackPath, row.exec == "grid" ? StackModel::EView::Grid : StackModel::EView::Auto); return;
        case ERow::StackShowParent:
            // FileManager1.ShowItems: the parent folder, this one selected.
            Hyprutils::OS::CProcess("gdbus", {"call", "--session", "--dest", "org.freedesktop.FileManager1", "--object-path", "/org/freedesktop/FileManager1", "--method",
                                              "org.freedesktop.FileManager1.ShowItems", "['file://" + item.stackPath + "']", ""})
                .runAsync();
            return;
        case ERow::StackTrashNewest: {
            auto L = StackReader::listing(item.stackPath);
            std::erase_if(L.entries, [](const StackModel::SEntry& e) { return e.dir; });
            StackModel::sortEntries(L.entries, StackModel::ESort::Added);
            if (!L.entries.empty())
                Stacks::trashFrom(item.stackPath, {L.entries.front().path});
            return;
        }
        case ERow::StackEmptyConfirm: {
            std::vector<std::string> paths;
            for (const auto& E : StackReader::listing(item.stackPath).entries)
                paths.push_back(E.path);
            Stacks::trashFrom(item.stackPath, std::move(paths));
            return;
        }
        // In-place views: handled in menuClick, never activated.
        case ERow::StackRename:
        case ERow::StackRenameField:
        case ERow::StackEmptyAsk:
        case ERow::StackEmptyCancel: return;
        case ERow::StackOpen: Locations::activate(item.uri); return;
        case ERow::StackUnpin: Stacks::unpin(item.stackPath); return; // "Remove from Dock": the folder is untouched

        case ERow::EmptyTrash: Locations::emptyTrash(); return;
        case ERow::Mount: Locations::mount(item.uri); return;
        case ERow::Unmount: Locations::unmount(item.uri, false); return;
        case ERow::Eject: Locations::unmount(item.uri, true); return;

        case ERow::Action: AppSearch::launchExec(row.exec); return;

        case ERow::Window: {
            const auto WINDOW = row.window.lock();
            if (!WINDOW)
                return;

            // Same as clicking its hover-panel preview: go to that window on
            // its desktop. Inside the overview, commit it as the selection so
            // close() does not overwrite the focus.
            if (const auto OVERVIEW = scrollOverviewForMonitor(monitor); OVERVIEW)
                OVERVIEW->selectAndClose(WINDOW);
            else
                activateWindow(WINDOW);
            return;
        }

        // Get Info and Back swap the menu's rows in place (menuClick); they only
        // reach here if something activates a row without a menu to swap.
        case ERow::Details:
        case ERow::InfoBack: return;

        case ERow::InfoLine:
            if (!row.exec.empty())
                Hyprutils::OS::CProcess("/usr/bin/wl-copy", {row.exec}).runAsync();
            return;

        case ERow::InfoCopyAll: {
            std::string all;
            for (const auto& R : infoRows(item))
                if (R.kind == ERow::InfoLine && !R.exec.empty())
                    all += R.label + ": " + R.exec + "\n";
            if (!all.empty())
                Hyprutils::OS::CProcess("/usr/bin/wl-copy", {all}).runAsync();
            return;
        }

        case ERow::InfoReveal:
            // FileManager1.ShowItems: the freedesktop call every file manager
            // answers, so this is not Nautilus-only.
            if (!row.exec.empty())
                Hyprutils::OS::CProcess("gdbus", {"call", "--session", "--dest", "org.freedesktop.FileManager1", "--object-path", "/org/freedesktop/FileManager1",
                                                  "--method", "org.freedesktop.FileManager1.ShowItems", "['file://" + row.exec + "']", ""})
                    .runAsync();
            return;

        case ERow::Favorite: toggleFavorite(itemDesktopId(item)); return;

        case ERow::ShowWindows: {
            const auto* ENTRY = AppSearch::entryForId(itemDesktopId(item));
            openScrollOverviewWithQuery(monitor, ENTRY && !ENTRY->name.empty() ? ENTRY->name : wmClass);
            return;
        }

        case ERow::Quit:
            for (const auto& WINDOW : itemWindows(item))
                WINDOW->sendClose();
            return;

        case ERow::NewWindow: launchApp(monitor, item, nullptr, true); return;

        case ERow::MinimizeAll:
            for (const auto& WINDOW : itemWindows(item))
                if (!Minimize::isMinimized(WINDOW))
                    Flight::minimize(monitor, WINDOW, false);
            return;

        case ERow::RestoreAll:
            for (const auto& WINDOW : itemWindows(item))
                if (Minimize::isMinimized(WINDOW))
                    Flight::restore(monitor, WINDOW);
            return;

        case ERow::ForceQuit: {
            // Each process once; never pid 0/1 and never the compositor itself,
            // whatever a client claims its pid is.
            std::unordered_set<pid_t> pids;
            for (const auto& WINDOW : itemWindows(item))
                if (const pid_t PID = WINDOW->getPID(); PID > 1 && PID != getpid())
                    pids.insert(PID);
            for (const pid_t PID : pids)
                kill(PID, SIGKILL);
            return;
        }
    }
}

// Lays the rows out AND draws them: a row's height is its text's height, and
// the only way to learn that is to build the texture. Doing both here is what
// keeps the hit-test boxes and the pixels the user sees the same numbers.
void renderMenu(PHLMONITOR monitor, const SLayout& layout) {
    if (!g_menu || !monitor || g_menu->monitor != monitor->m_id)
        return;

    // The package lookup landed: the info rows grow their answers.
    if (g_menu->infoView && AppInfo::generation() != g_menu->infoGen) {
        g_menu->infoGen = AppInfo::generation();
        g_menu->rows    = infoRows(g_menu->item);
    }

    const float SCALE  = std::max(monitor->m_scale, 0.01F);
    const int   FONTPX = std::max(1, sc<int>(std::round(MENU_FONT * SCALE)));

    // The warning colour for Force Quit: a desaturated red that still reads as
    // part of a dark Omarchy plate rather than as an alert box.
    const CHyprColor LABEL_COLOR{1.F, 1.F, 1.F, 0.95F};
    const CHyprColor DANGER_COLOR{1.F, 0.50F, 0.47F, 0.97F};
    const CHyprColor DIM_COLOR{1.F, 1.F, 1.F, 0.45F};

    struct SRowTex {
        SP<Render::ITexture> label, glyph, hint;
    };
    std::vector<SRowTex> textures;
    textures.reserve(g_menu->rows.size());

    // One glyph column for the whole plate, so every label starts at the same x
    // -- present only when some row has a glyph.
    const bool  GLYPHS    = std::ranges::any_of(g_menu->rows, [](const SRow& r) { return !r.glyph.empty(); });
    const float GLYPHCOLW = GLYPHS ? std::round(sc<float>(FONTPX) * 1.9F) : 0.F;
    const float HINTGAP   = std::round(18.F * SCALE);

    float width  = MENU_MIN_W * SCALE;
    float height = 0.F;
    for (const auto& ROW : g_menu->rows) {
        if (ROW.kind == ERow::Separator) {
            textures.emplace_back();
            height += MENU_SEP_H * SCALE;
            continue;
        }

        SRowTex t;
        t.label = OverviewRender::text(ROW.label, ROW.danger ? DANGER_COLOR : LABEL_COLOR, FONTPX, false, "", 0, 500);
        if (!t.label || !t.label->ok()) {
            textures.emplace_back();
            height += MENU_SEP_H * SCALE;
            continue;
        }
        if (!ROW.glyph.empty())
            t.glyph = OverviewRender::text(ROW.glyph, ROW.danger ? DANGER_COLOR : DIM_COLOR.modifyA(0.8F), FONTPX, false, "JetBrainsMono Nerd Font", 0, 400);
        if (!ROW.hint.empty())
            t.hint = OverviewRender::text(ROW.hint, DIM_COLOR, FONTPX, false, "", 0, 400);

        const float HINTW = t.hint && t.hint->ok() ? HINTGAP + sc<float>(t.hint->m_size.x) : 0.F;
        width             = std::max(width, 2.F * MENU_PAD_X * SCALE + GLYPHCOLW + sc<float>(t.label->m_size.x) + HINTW);
        height += sc<float>(t.label->m_size.y) + 2.F * MENU_ROW_PAD_Y * SCALE;
        textures.emplace_back(std::move(t));
    }

    if (height < 1.F)
        return;

    const float CX = sc<float>(g_menu->anchor.x) + sc<float>(g_menu->anchor.width) / 2.F;
    CBox        plate{std::round(CX - width / 2.F), std::round(sc<float>(layout.pill.y) - MENU_GAP * SCALE - height), std::round(width), std::round(height)};
    plate.x = std::clamp(plate.x, 0.0, std::max(0.0, monitor->m_size.x * SCALE - plate.width));
    plate.y = std::max(0.0, plate.y);

    g_menu->box = plate;

    // X13/X29/X86: menu frost -- shared chrome sample in overview, live on desktop.
    frostedPlate(monitor, plate, sc<int>(std::round(MENU_RADIUS * SCALE)), 1.F);

    const auto MOUSE = (g_pInputManager->getMouseCoordsInternal() - monitor->m_position) * monitor->m_scale;

    float y = sc<float>(plate.y);
    for (size_t i = 0; i < g_menu->rows.size(); ++i) {
        auto&       row = g_menu->rows[i];
        const auto& TEX = textures[i];

        if (!TEX.label) {
            row.box = CBox{plate.x, std::round(y), plate.width, std::round(MENU_SEP_H * SCALE)};

            CRectPassElement::SRectData sep;
            sep.box   = CBox{std::round(plate.x + MENU_PAD_X * SCALE), std::round(y + MENU_SEP_H * SCALE / 2.F),
                             std::round(plate.width - 2.F * MENU_PAD_X * SCALE), sc<double>(std::max(1.F, std::round(SCALE)))};
            sep.color = CHyprColor{1.F, 1.F, 1.F, 0.15F};
            g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(sep));

            y += MENU_SEP_H * SCALE;
            continue;
        }

        const float ROWH = sc<float>(TEX.label->m_size.y) + 2.F * MENU_ROW_PAD_Y * SCALE;
        row.box          = CBox{plate.x, std::round(y), plate.width, std::round(ROWH)};

        if (row.box.containsPoint(MOUSE)) {
            // X56: the context menu's row highlight follows the theme too --
            // except on the destructive row, which lights in its own colour so
            // the pointer landing on it is noticed.
            const auto ROWACCENT = row.danger ? DANGER_COLOR : ScrollOverview::Config::highlightColor();

            CRectPassElement::SRectData fill;
            fill.box           = row.box;
            fill.color         = ROWACCENT.modifyA(row.danger ? 0.18F : 0.26F);
            fill.round         = 0;
            fill.roundingPower = 2.F;
            g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(fill));
        }

        const float TEXTX = std::round(plate.x + MENU_PAD_X * SCALE);
        const float TEXTY = std::round(y + MENU_ROW_PAD_Y * SCALE);
        const float LH    = sc<float>(TEX.label->m_size.y);

        if (TEX.glyph && TEX.glyph->ok()) {
            CTexPassElement::SRenderData g;
            g.tex = TEX.glyph;
            // Centred in the column and on the label's line.
            g.box = CBox{std::round(TEXTX + (GLYPHCOLW - sc<float>(TEX.glyph->m_size.x)) / 2.F - GLYPHCOLW * 0.15F),
                         std::round(TEXTY + (LH - sc<float>(TEX.glyph->m_size.y)) / 2.F), sc<float>(TEX.glyph->m_size.x), sc<float>(TEX.glyph->m_size.y)};
            g.a   = 1.F;
            g_pHyprRenderer->m_renderPass.add(makeUnique<CTexPassElement>(g));
        }

        CTexPassElement::SRenderData text;
        text.tex = TEX.label;
        text.box = CBox{TEXTX + GLYPHCOLW, TEXTY, sc<float>(TEX.label->m_size.x), LH};
        text.a   = 1.F;
        g_pHyprRenderer->m_renderPass.add(makeUnique<CTexPassElement>(text));

        if (TEX.hint && TEX.hint->ok()) {
            CTexPassElement::SRenderData h;
            h.tex = TEX.hint;
            h.box = CBox{std::round(plate.x + plate.width - MENU_PAD_X * SCALE - sc<float>(TEX.hint->m_size.x)), std::round(TEXTY + (LH - sc<float>(TEX.hint->m_size.y)) / 2.F),
                         sc<float>(TEX.hint->m_size.x), sc<float>(TEX.hint->m_size.y)};
            h.a   = 1.F;
            g_pHyprRenderer->m_renderPass.add(makeUnique<CTexPassElement>(h));
        }

        y += ROWH;
    }
}

} // namespace

void feedPointerMotion(const Vector2D& delta) {
    const auto POINT = g_pInputManager->getMouseCoordsInternal();

    for (const auto& MONITOR : State::monitorState()->monitors()) {
        if (!MONITOR)
            continue;

        auto&      R   = g_reveal[MONITOR->m_id];
        const auto BOX = MONITOR->logicalBox();

        // Pressure does not carry: leave the barrier, or the monitor, and the
        // push starts again from zero.
        if (!BOX.containsPoint(POINT) || POINT.y < BOX.y + BOX.h - EDGE_BAND)
            R.pressure = 0.F;
        else
            R.pressure += sc<float>(std::max(0.0, delta.y)); // only DOWNWARD motion presses a bottom edge

        // Not updateAutohide: this must also pump the frame that the flip it may
        // have just armed needs, and damageIfChanged is where that lives.
        damageIfChanged(MONITOR);
    }
}

// ------------------------------------------------ X32 reorder, G12 drop --

namespace {

// The dock's own drag, expressed in the store's unit: a press over an icon with
// no desktop id (a preview, the button, the pill's padding, a running app no
// entry claims) records nothing and can never become a drag, because there
// would be nothing to write.
struct SDockDrag {
    std::string id;
    MONITORID   monitor = MONITOR_INVALID; // the dock it was picked up from; every box below is in its pixels
    Vector2D    start;
    bool        live   = false;
    int         insert = -1;

    // V3. What it takes to DRAW the thing being dragged, recorded at the press
    // because the layout under it will move: pulling an icon out of the pill
    // reflows the neighbours, and by release its cell is not where it was.
    // `grab` is the offset from the cell's origin to the press point, so the
    // ghost stays under the same part of the icon the user picked it up by
    // rather than snapping its corner to the pointer.
    std::string icon;
    CBox        home;
    Vector2D    grab;
    Vector2D    at;

    // A STACK being reordered among the stacks (dock stacks A); empty for an app.
    std::string stackPath;
};

// V3. The cancel: an icon released on nothing FLIES BACK to its cell instead of
// vanishing under the pointer. Same shape as every other thing the dock
// animates (a box, a start time, a curve) and deliberately NOT a Flight:: --
// a flight owns a window, hides it, and has a landing contract about focus and
// geometry; this owns a texture and owes nothing to anybody when it ends.
struct SDockGhostFlight {
    std::string     icon;
    MONITORID       monitor = MONITOR_INVALID;
    CBox            from;
    CBox            home;
    Time::steady_tp startedAt;
};

// The two things that can be moving an icon: the dock's own drag, and G12's
// caret -- a foreign drag keeps its state in the search panel and only ever
// hands the dock an index to draw, so the two sources cannot both think they
// own the gesture. One struct, so a reader can see there are exactly two.
// Tearing a LIVE PREVIEW out of the hover panel. Separate from SDockDrag on
// purpose: that one carries a desktop id and can end in a reorder or a launch,
// this one carries a WINDOW that already exists and can only ever end in that
// window landing on a desktop. `home` is the cell it was picked up from, which
// is also the ghost's size -- the preview keeps the size it had in the panel
// all the way to the drop, so the growth starts from something the eye has
// already been looking at.
struct SPanelDrag {
    PHLWINDOWREF window;
    Vector2D     start;
    CBox         home;
    Vector2D     grab;
    Vector2D     at;
    bool         live = false;
    // ONE RENDER OF ONE SURFACE. A live preview is the real window scaled into
    // the cell at draw time (X91/dockLivePreview), so a dock-drawn ghost would
    // be a SECOND render of the same surface while the window is also drawn as
    // itself -- the thing the previews were built to avoid, and what made an
    // earlier ghost jitter. There is no ghost now: past the threshold the drag
    // IS the window, owned by the overview drag or by Hyprland's own.
};

struct SDockDragState {
    std::optional<SDockDrag>        drag;
    std::optional<SPanelDrag>       panel;
    std::optional<SDockGhostFlight> ghostHome; // V3, the cancel above
    int                             foreignInsert = -1;
};

SDockDragState g_dockDrag;

// Deliberately the overview's own threshold: two drags on one screen that start
// at different distances would read as one of them being sticky.
float dragThresholdPx(PHLMONITOR monitor) {
    return Config::getDragThreshold() * std::max(monitor ? sc<float>(monitor->m_scale) : 1.F, 0.01F);
}

// The favourite slots, in draw order. Only these take part in a reorder: a
// running app that is not pinned has no stored position to move, and the
// stretch of pill it occupies means "after the last favourite" -- which is what
// size() is.
std::vector<size_t> favoriteSlots(const SLayout& layout) {
    std::vector<size_t> out;
    for (size_t i = 0; i < layout.items.size(); ++i)
        if (layout.items[i].favorite)
            out.emplace_back(i);
    return out;
}

std::vector<size_t> stackSlots(const SLayout& layout) {
    std::vector<size_t> out;
    for (size_t i = 0; i < layout.items.size(); ++i)
        if (layout.items[i].stack)
            out.emplace_back(i);
    return out;
}

bool draggingStack() {
    return g_dockDrag.drag && !g_dockDrag.drag->stackPath.empty();
}

int caretIndex() {
    return g_dockDrag.drag && g_dockDrag.drag->live ? g_dockDrag.drag->insert : g_dockDrag.foreignInsert;
}

// V3. The dragged icon's box THIS FRAME: the cell it came out of, carried to
// where the pointer is and clamped to the monitor so it cannot be dragged off
// the screen edge and become impossible to see coming back.
CBox ghostBoxAt(PHLMONITOR monitor, const CBox& home, const Vector2D& grab, const Vector2D& at) {
    if (!monitor || home.width < 1)
        return {};

    const float MONW = sc<float>(monitor->m_size.x) * monitor->m_scale;
    const float MONH = sc<float>(monitor->m_size.y) * monitor->m_scale;
    const float W    = sc<float>(home.width);
    const float H    = sc<float>(home.height);

    return CBox{std::clamp(sc<float>(at.x - grab.x), 0.F, std::max(0.F, MONW - W)), std::clamp(sc<float>(at.y - grab.y), 0.F, std::max(0.F, MONH - H)), W, H}.round();
}

// V3. Is a dragged icon being drawn under the pointer right now? Only once it
// is OFF the pill: over the pill the gesture is X32's reorder, which already
// says what it is doing with the insertion caret, and a second icon floating
// over the row it is being inserted into would be two answers to one question.
bool ghostShowing() {
    return g_dockDrag.drag && g_dockDrag.drag->live && g_dockDrag.drag->insert < 0 && !g_dockDrag.drag->icon.empty();
}

bool ghostFlying() {
    return g_dockDrag.ghostHome.has_value();
}

// V3. The return flight is an EXIT -- the gesture is over and the icon is
// leaving -- so it accelerates, and at the short end of the house band. Erased
// by the render that finds it expired, exactly like a bounce or a launch zoom.
// (The ghost's OPACITY is Dock::DRAG_GHOST_ALPHA in the header: a stack's item
// drag draws the same kind of clone and must not pick its own number.)
constexpr float GHOST_HOME_MS = 220.F;

// Where the caret sits for an insertion index: hard against the left edge of
// the icon that would follow it, or past the last favourite when it is the end.
CBox caretBox(const SLayout& layout, int index, float scale) {
    // A stack drag's caret runs over the stack cells, not the favourites.
    const auto SLOTS = draggingStack() ? stackSlots(layout) : favoriteSlots(layout);
    if (index < 0 || SLOTS.empty())
        return {};

    const float W = std::max(2.F, std::round(3.F * scale));
    const auto& REF = layout.items[SLOTS[std::min(sc<size_t>(index), SLOTS.size() - 1)]].iconBox;
    const bool  END = sc<size_t>(index) >= SLOTS.size();

    const double X = END ? REF.x + REF.width + ITEM_MARGIN_X * scale - W / 2.0 : REF.x - ITEM_MARGIN_X * scale - W / 2.0;
    return CBox{X, REF.y, W, REF.height}.round();
}

}

bool pillContains(PHLMONITOR monitor, const Vector2D& point) {
    if (!monitor)
        return false;

    const auto PILL = layout(monitor).pill;
    return PILL.width >= 1 && PILL.height >= 1 && PILL.containsPoint(point);
}

bool uiContains(PHLMONITOR monitor, const Vector2D& point) {
    // The hover-preview plate is only reached after the pointer is already
    // on the pill, so the pill is the linger-cancel surface.
    return pillContains(monitor, point);
}

bool scroll(PHLMONITOR monitor, const Vector2D& point, bool up) {
    if (!pillContains(monitor, point))
        return occupiesPointer(monitor, point); // hover plate / menu: swallow, do not leak

    const auto ACTION = Config::getValue<std::string>(KEY_SCROLL_ACTION);
    if (ACTION != "cycle-windows" && ACTION != "switch-workspace")
        return true; // `do-nothing` still SWALLOWS: the pill is not a window.

    // The dead time is checked before the hit-test and after the action, so a
    // swallowed tick does not start the clock and `do-nothing` never does. One
    // clock for both actions, as in the reference -- they can never both be on.
    static Time::steady_tp s_last{};
    static bool            s_seen = false;

    if (s_seen && elapsedMs(s_last) < SCROLL_DEAD_MS)
        return true;

    s_last = Time::steadyNow();
    s_seen = true;

    if (ACTION == "switch-workspace") {
        // `activeWs.get_neighbor(direction)`: up is the PREVIOUS workspace, and
        // an edge is a no-op rather than a wrap -- get_neighbor hands back the
        // same workspace when there is nothing past it.
        const auto ACTIVE = monitor->m_activeWorkspace;
        if (!ACTIVE)
            return true;

        const WORKSPACEID WANTED = ACTIVE->m_id + (up ? -1 : 1);
        for (const auto& CANDIDATE : State::workspaceState()->workspaces()) {
            const auto TARGET = CANDIDATE.lock();
            if (TARGET && TARGET->m_id == WANTED && TARGET != ACTIVE) {
                monitor->changeWorkspace(TARGET, false, true, true);
                break;
            }
        }

        return true;
    }

    // `cycle-windows` is an ICON action: the reference binds it to the AppIcon,
    // so the pill's own background does nothing under this setting. It still
    // swallowed the tick above, which is the point of the pill.
    const auto LAYOUT = layout(monitor);
    const auto ITEM   = std::ranges::find_if(LAYOUT.items, [&](const SItem& item) { return hoverBox(item.iconBox, monitor->m_scale).containsPoint(point); });
    if (ITEM == LAYOUT.items.end())
        return true;

    cycleOrActivate(*ITEM, up);
    if (g_dockSeatHeld)
        stealDockSeat();
    return true;
}

void showOverlay() {
    if (!Config::getValue<bool>(KEY_HOT_KEYS))
        return;

    // `_optionalNumberOverlay`: the extra shortcut is registered at all only
    // when one of the two is on, and each effect is gated again on its own key
    // inside `_showOverlay`. With both off this is a no-op, which is why the
    // deadline is not moved rather than moved and then ignored twice.
    if (!Config::getValue<bool>(KEY_HOTKEY_OVERLAY) && !Config::getValue<bool>(KEY_HOTKEY_DOCK))
        return;

    const float SECS = std::max(0.F, Config::getValue<float>(KEY_SHORTCUT_SECS));
    g_overlayUntil   = Time::steadyNow() + std::chrono::milliseconds(sc<int>(std::round(SECS * 1000.F)));

    // A keypress is not a frame, and render.pre only fires inside one, so the
    // first repaint has to be asked for here. Every monitor, because the
    // reference shows the numbers on every dock it has, not just the focused
    // one.
    for (const auto& MONITOR : State::monitorState()->monitors()) {
        if (MONITOR)
            damageIfChanged(MONITOR);
    }
}

void hotkey(int number, bool shift, bool ctrl) {
    if (!Config::getValue<bool>(KEY_HOT_KEYS))
        return;

    // `_showOverlay()` runs beside `_activateApp`, not after a successful one,
    // and `_activateApp` no-ops past the end of the dash. An empty slot
    // therefore still draws the numbers -- which is how you see it is empty.
    showOverlay();

    const auto MONITOR = Desktop::focusState()->monitor();
    if (!MONITOR)
        return;

    const auto LAYOUT = layout(MONITOR);
    const int  INDEX  = number - 1;
    if (INDEX < 0 || INDEX >= sc<int>(LAYOUT.items.size()))
        return;

    const auto& ITEM = LAYOUT.items[INDEX];

    // The three modifiers are three branches of ONE `AppIcon.activate(button=1)`
    // reading the KEY event's state, not three behaviours of their own -- which
    // is why the reference binds all thirty accelerators to one callback.
    // CONTROL short-circuits to GNOME's own activate (a new window of a running
    // app, a launch of a stopped one); SHIFT picks `shift-click-action`; neither
    // reaches `clickAction`.
    if (ctrl)
        launchApp(MONITOR, ITEM);
    else
        activateItem(MONITOR, ITEM, shift);
}

bool click(PHLMONITOR monitor, const Vector2D& point, bool shift, bool middle, bool ctrl) {
    if (!monitor)
        return false;

    const auto LAYOUT = layout(monitor);
    if (LAYOUT.pill.width < 1 || LAYOUT.pill.height < 1)
        return false;

    // X33. The middle button reaches ICONS only. A preview is one window and
    // "launch a new one" is not a thing to do to it, and Show Applications is a
    // button rather than an app -- the reference's middle-click lives on the
    // AppIcon and nowhere else.
    if (middle) {
        const auto ITEM =
            std::ranges::find_if(LAYOUT.items, [&](const SItem& item) { return hoverBox(item.iconBox, monitor->m_scale).containsPoint(point); });
        if (ITEM == LAYOUT.items.end())
            return false;

        applyClickAction(monitor, *ITEM, Config::getValue<std::string>(shift ? KEY_SHIFT_MIDDLE : KEY_MIDDLE_CLICK), shift);
        return true;
    }

    // X32 Show Applications. Ahead of the icons because its cell is laid out
    // like one and would otherwise have to be excluded from their search.
    if (!LAYOUT.showApps.empty() && hoverBox(LAYOUT.showApps, monitor->m_scale).containsPoint(point)) {
        // The app GRID this opens is G5's, and G5 is not built. What X32 owns is
        // the button: its slot, its edge, and the fact that it opens the surface
        // apps are searched from. So on the desktop it opens the overview; with
        // the overview already up there is nothing further to show yet, and it
        // deliberately does NOT toggle it shut -- a button that closes the thing
        // it is supposed to open would be worse than one that waits for G5.
        //
        // A fresh open from the dock starts CLEAN (orchestrator, 2026-08-11).
        // The button is the other way into the surface the dispatcher opens,
        // and it was the only one that did not begin a new search session --
        // so a query typed into a previous open survived a close and came back
        // with it. The reset belongs here, at the entry point, and NOT inside
        // openOverview(): that is also the reopen path for a toggle landing on
        // a closing overview, where dropping the filter would pop the windows
        // it was hiding back into a close animation already under way.
        // Gate: scripts/gates/x32_showapps_fresh.
        if (!scrollOverviewForMonitor(monitor)) {
            resetSearchSession();
            openScrollOverview(monitor);
        }

        return true;
    }

    // Hit-test the same cell the hover highlight lights up, so what looks
    // clickable is what is clickable.
    const auto ITEM = std::ranges::find_if(LAYOUT.items, [&](const SItem& item) { return hoverBox(item.iconBox, monitor->m_scale).containsPoint(point); });
    if (ITEM == LAYOUT.items.end())
        return false;

    activateItem(monitor, *ITEM, shift, ctrl);
    return true;
}

int dropIndexAt(PHLMONITOR monitor, const Vector2D& point) {
    if (!monitor)
        return -1;

    const auto LAYOUT = layout(monitor);
    if (LAYOUT.pill.width < 1 || !LAYOUT.pill.containsPoint(point))
        return -1;

    const auto SLOTS = favoriteSlots(LAYOUT);
    for (size_t k = 0; k < SLOTS.size(); ++k) {
        // The MIDPOINT, not the edge. Dropping on an icon's left half puts the
        // dragged app before it; an edge test leaves a dead strip between icons
        // where the drop has no answer at all.
        const auto& BOX = LAYOUT.items[SLOTS[k]].iconBox;
        if (point.x < BOX.x + BOX.width / 2.0)
            return sc<int>(k);
    }

    return sc<int>(SLOTS.size());
}

// Where a dragged STACK would land among the stacks: the same midpoint rule as
// dropIndexAt, over the stack cells only. -1 off the pill.
int stackDropIndexAt(PHLMONITOR monitor, const Vector2D& point) {
    if (!monitor)
        return -1;
    const auto LAYOUT = layout(monitor);
    if (LAYOUT.pill.width < 1 || !LAYOUT.pill.containsPoint(point))
        return -1;
    const auto SLOTS = stackSlots(LAYOUT);
    for (size_t k = 0; k < SLOTS.size(); ++k) {
        const auto& BOX = LAYOUT.items[SLOTS[k]].iconBox;
        if (point.x < BOX.x + BOX.width / 2.0)
            return sc<int>(k);
    }
    return sc<int>(SLOTS.size());
}

bool showAppsContains(PHLMONITOR monitor, const Vector2D& point) {
    if (!monitor)
        return false;

    const auto LAYOUT = layout(monitor);
    return !LAYOUT.showApps.empty() && hoverBox(LAYOUT.showApps, monitor->m_scale).containsPoint(point);
}

void pinAt(const std::string& desktopId, int index) {
    if (desktopId.empty() || index < 0)
        return;

    auto& FAV = favorites();

    // Where the id sits TODAY, counted the way `index` is counted. Removing the
    // dragged icon shifts everything after it one place left, so a move to a
    // later slot would otherwise land one short of the caret the user saw.
    int from = -1;
    for (int seen = 0; const auto& ID : FAV) {
        if (!AppSearch::entryForId(ID))
            continue;
        if (ID == desktopId) {
            from = seen;
            break;
        }
        ++seen;
    }

    if (from >= 0) {
        std::erase(FAV, desktopId);
        if (from < index)
            --index;
    }

    // `index` counts DRAWN favourites, but the store also holds ids this machine
    // does not provide -- the seed is the user's HOST list. Walk the store to
    // find the position the index-th drawn favourite occupies, so the undrawn
    // ones keep their places instead of being silently reordered around.
    size_t at = FAV.size();
    for (size_t i = 0, seen = 0; i < FAV.size(); ++i) {
        if (!AppSearch::entryForId(FAV[i]))
            continue;
        if (seen == sc<size_t>(index)) {
            at = i;
            break;
        }
        ++seen;
    }

    FAV.insert(FAV.begin() + sc<long>(at), desktopId);
    writeFavorites();
}

void unpin(const std::string& desktopId) {
    if (desktopId.empty())
        return;

    if (std::erase(favorites(), desktopId) > 0)
        writeFavorites();
}

// No overview open: the desktop half of the tear-out. Move the window to the
// desktop being looked at, then hand the gesture to HYPRLAND'S OWN window drag
// -- the same MBIND_MOVE a SUPER+drag starts -- so from the hand holding the
// button it simply is that gesture from here on.
//
// The window is parked at the cell's size first so it GROWS into full size as
// the drag pulls away from the dock, which is the dock's shrink played
// backwards. m_Value and not the goal, for restoreForFlight's reason: the
// layout's target has to stay where the drag is going to put it.
// Set while a desktop drag started life as a dock tear-out. See Dock.hpp.
bool g_dockNativeDrag = false;

void beginDesktopTearOut(PHLMONITOR monitor, const PHLWINDOW& window, const CBox& home) {
    if (!monitor || !window)
        return;

    // FIRST, before the move and the focus below change it: the window the user
    // was working in is what the drop tiles beside (Placement.hpp).
    Placement::noteDragAnchor(Desktop::focusState()->window());

    if (const auto ACTIVE = monitor->m_activeWorkspace; ACTIVE && window->m_workspace != ACTIVE)
        Desktop::globalWindowController()->moveWindowToWorkspace(window, ACTIVE);

    const float SCALE = std::max(monitor->m_scale, 0.01F);
    if (home.width >= 1.0 && home.height >= 1.0 && window->m_realSize && window->m_realPosition) {
        window->m_realPosition->m_Value = Vector2D{home.x / SCALE + monitor->m_position.x, home.y / SCALE + monitor->m_position.y};
        window->m_realSize->m_Value     = Vector2D{std::max(4.0, home.width / SCALE), std::max(4.0, home.height / SCALE)};
    }

    Desktop::focusState()->fullWindowFocus(window, Desktop::FOCUS_REASON_DESKTOP_STATE_CHANGE);

    if (const auto TARGET = window->layoutTarget(); TARGET && g_layoutManager && g_layoutManager->dragController()) {
        g_layoutManager->dragController()->dragBegin(TARGET, MBIND_MOVE);
        g_dockNativeDrag = true;
    }
}

// PRESS FEEDBACK (Dock.hpp). The icon, and only the icon, presses in while the
// button is held and springs back on release -- the running dot and the badge
// stay where they are, the same stance Y8's hop takes.
//
// Tuned for FEEL (user, 2026-09-12: "deeper, a more satisfying curve, and tilt
// where I'm clicking"):
//   * the press eases hard into the bottom (quintic ease-out), so it lands like
//     a key reaching the end of its travel rather than drifting there;
//   * the release is a damped SPRING, not an ease: it pops back a few percent
//     past full size and settles, which is what makes a click feel springy;
//   * the icon itself -- its lean, its squish depth and its glare -- is drawn
//     by DockTilt, in real perspective (user, 2026-09-14: "a 3d tilt like on
//     the cards on dragos.shastasia.com"). This file only owns the CURVE: how
//     deep the press is at any moment, rebound included.
//
// A QUICK CLICK still shows the whole squish: the spring-back waits for the
// press-in to reach the bottom (upAt), so a tap reads as a full click.
constexpr float PRESS_IN_MS    = 110.F;

// The release spring, as a closed-form step response rather than an integrator,
// so the curve is a pure function of time and identical at any frame rate.
// Damping 0.42 overshoots by ~23% of the press depth -- about 5% past full size
// at DockTilt's press depth -- and is inside 2% of rest by ~390ms.
constexpr float RELEASE_ZETA      = 0.42F;
constexpr float RELEASE_OMEGA     = 24.F;  // natural frequency, rad/s
constexpr float RELEASE_SETTLE_MS = 560.F; // past this the response is below a pixel
// One frame's grace past the settle before the state is dropped, so the last
// frame drawn is the one at rest, not the one just short of it.
constexpr float PRESS_TAIL_MS = 34.F;

struct SPress {
    std::string     subject; // tooltipSubject() of the pressed icon; "show-apps" for that button
    // WHOSE DOCK WAS PRESSED. Every monitor's dock draws the same icons, and a
    // press keyed on the subject alone squished the icon on BOTH of them -- and
    // asked both for the frames the spring-back needs (user, 2026-09-14). The
    // squish belongs to the pill the finger landed on.
    MONITORID       monitor  = MONITOR_INVALID;
    Time::steady_tp since    = Time::steadyNow();
    bool            released = false;
    Time::steady_tp upAt     = Time::steadyNow(); // when the spring-back starts
    Vector2D        at;                           // where it was pressed: each axis -1..1 from the icon's centre
};

std::optional<SPress> g_press;

float easeOutQuint(float t) {
    const float INV = 1.F - std::clamp(t, 0.F, 1.F);
    return 1.F - INV * INV * INV * INV * INV;
}

// A damped spring released from full depth at rest: 1 at t=0 with zero
// velocity, swinging through 0 into a small negative overshoot and settling.
float releaseResponse(float ms) {
    if (ms <= 0.F)
        return 1.F;
    if (ms >= RELEASE_SETTLE_MS)
        return 0.F;

    const float T     = ms / 1000.F;
    const float ROOT  = std::sqrt(1.F - RELEASE_ZETA * RELEASE_ZETA);
    const float OMEGD = RELEASE_OMEGA * ROOT;
    return std::exp(-RELEASE_ZETA * RELEASE_OMEGA * T) * (std::cos(OMEGD * T) + (RELEASE_ZETA / ROOT) * std::sin(OMEGD * T));
}

// 1 fully pressed, 0 at rest, NEGATIVE during the release overshoot.
float pressDepth(MONITORID monitor, const std::string& subject) {
    if (!g_press || g_press->monitor != monitor || g_press->subject != subject)
        return 0.F;

    const float IN = easeOutQuint(elapsedMs(g_press->since) / PRESS_IN_MS);
    if (!g_press->released)
        return IN;

    // upAt can still be in the future: a quick click finishing its press-in.
    const float SINCEUP = elapsedMs(g_press->upAt);
    return SINCEUP < 0.F ? IN : releaseResponse(SINCEUP);
}

Vector2D pressAt(MONITORID monitor, const std::string& subject) {
    return g_press && g_press->monitor == monitor && g_press->subject == subject ? g_press->at : Vector2D{};
}

bool pressAnimating(MONITORID monitor) {
    if (!g_press)
        return false;

    // THE EXPIRY IS NOT GATED ON THE MONITOR, and the order matters. Whichever
    // dock renders next retires a press that has finished, so a press cannot
    // outlive an output that has gone quiet -- which would leave that pill
    // holding a squish nothing was left to spring back.
    if (g_press->released && elapsedMs(g_press->upAt) >= RELEASE_SETTLE_MS + PRESS_TAIL_MS) {
        g_press.reset();
        return false;
    }

    // ...but only the pill that was pressed is the one moving.
    if (g_press->monitor != monitor)
        return false;

    // Held and fully in: nothing moves, so nothing needs a frame.
    if (!g_press->released)
        return elapsedMs(g_press->since) < PRESS_IN_MS;

    return true;
}

// Normalised against the icon itself, clamped: the hover box is a little larger
// than the icon, and a point in its margin leans fully, not beyond.
Vector2D normalizedIn(const CBox& icon, const Vector2D& point) {
    return {std::clamp((point.x - (icon.x + icon.width / 2.0)) / std::max(icon.width / 2.0, 1.0), -1.0, 1.0),
            std::clamp((point.y - (icon.y + icon.height / 2.0)) / std::max(icon.height / 2.0, 1.0), -1.0, 1.0)};
}

// The pointer as DockTilt's hover wants it: over this cell, or not at all.
std::optional<Vector2D> pointerIn(const CBox& icon, const Vector2D& mouse, double scale) {
    if (icon.width < 1 || icon.height < 1 || !hoverBox(icon, scale).containsPoint(mouse))
        return std::nullopt;
    return normalizedIn(icon, mouse);
}

void pressBegin(PHLMONITOR monitor, const Vector2D& point) {
    g_press.reset();
    if (!monitor)
        return;

    const auto LAYOUT = layout(monitor);

    std::string subject;
    CBox        icon;
    if (const auto ITEM = std::ranges::find_if(LAYOUT.items, [&](const SItem& item) { return hoverBox(item.iconBox, monitor->m_scale).containsPoint(point); });
        ITEM != LAYOUT.items.end()) {
        subject = ITEM->subject;
        icon    = ITEM->iconBox;
    } else if (!LAYOUT.showApps.empty() && hoverBox(LAYOUT.showApps, monitor->m_scale).containsPoint(point)) {
        subject = "show-apps";
        icon    = LAYOUT.showApps;
    }

    if (subject.empty() || icon.width < 1 || icon.height < 1)
        return;

    g_press = SPress{.subject = subject, .monitor = monitor->m_id, .since = Time::steadyNow(), .at = normalizedIn(icon, point)};
}

void pressEnd() {
    if (!g_press || g_press->released)
        return;

    g_press->released = true;
    g_press->upAt     = std::max(Time::steadyNow(), g_press->since + std::chrono::milliseconds(sc<int>(PRESS_IN_MS)));
}

bool dragBegin(PHLMONITOR monitor, const Vector2D& point) {
    g_dockDrag.drag.reset();

    if (!monitor)
        return false;

    const auto LAYOUT = layout(monitor);
    const auto ITEM   = std::ranges::find_if(LAYOUT.items, [&](const SItem& item) { return hoverBox(item.iconBox, monitor->m_scale).containsPoint(point); });
    if (ITEM == LAYOUT.items.end() || (ITEM->desktopId.empty() && !ITEM->stack))
        return false;

    g_dockDrag.drag = SDockDrag{.id        = ITEM->desktopId,
                                .monitor   = monitor->m_id,
                                .start     = point,
                                .icon      = ITEM->icon,
                                .home      = ITEM->iconBox,
                                .grab      = point - Vector2D{ITEM->iconBox.x, ITEM->iconBox.y},
                                .at        = point,
                                .stackPath = ITEM->stack ? ITEM->stackPath : std::string{}};
    return true;
}

bool dragMove(PHLMONITOR monitor, const Vector2D& point) {
    if (!monitor)
        return false;

    // A preview being torn out owns the motion; it goes live on the same
    // threshold the icon drag uses, so the two gestures cannot feel different.
    if (g_dockDrag.panel) {
        g_dockDrag.panel->at = point;
        if (!g_dockDrag.panel->live) {
            const float T = dragThresholdPx(monitor);
            if (g_dockDrag.panel->start.distanceSq(point) <= T * T)
                return false;

            // HAND OFF, and stop owning the gesture. Past the threshold this is
            // the REAL window being dragged -- not a ghost of it -- so the dock
            // gets out of the way entirely and the drag machinery that already
            // knows how to shrink at the dock boundary and minimize on a drop
            // back into it takes over. The dock's job was to say which window
            // and from which box.
            const auto WINDOW = g_dockDrag.panel->window.lock();
            const CBox HOME   = g_dockDrag.panel->home;
            g_dockDrag.panel.reset();
            if (!WINDOW)
                return false;

            dismissHoverPanelNow();

            // A MINIMIZED window is stashed: it sits on the stash workspace with
            // no tile, so the overview's drag refuses it (shouldShowOverviewWindow
            // / layoutTarget both fail) and the desktop's has no target to hand
            // Hyprland -- which is why tearing one out did nothing whatsoever.
            // Un-stash it first and the rest of the gesture is the ordinary one.
            // The dock panel lists minimized windows on purpose (itemWindows
            // includes them, so a minimized window is reachable from its icon),
            // so this is a case the tear-out has to answer for, not reject.
            if (Minimize::isMinimized(WINDOW))
                Minimize::restore(WINDOW);

            if (const auto OVERVIEW = scrollOverviewForMonitor(monitor)) {
                // Same story for a window the SEARCH hides (user, 2026-09-24:
                // dragging another app's preview under Show Windows "refuses to
                // do it"): the filter goes, and the drag is the ordinary one.
                overviewAdmitWindow(WINDOW);
                OVERVIEW->beginWindowDragFrom(WINDOW, HOME); // grows out of the cell
                // IMMEDIATELY, not on the next frame. The overview syncs this
                // flag once per frame (scrollOverview.cpp, end of render), and a
                // release that lands before that frame would be claimed by the
                // dock's own ladder instead of ending the drag -- leaving
                // drag.activeWindow set forever. A window held in that state is
                // excluded from the overview's tiles (it is meant to be drawn as
                // the drag proxy), so it goes invisible with no way back.
                setWindowDragActive(true);
            } else
                beginDesktopTearOut(monitor, WINDOW, HOME);
            return true;
        }
        return true;
    }

    if (!g_dockDrag.drag)
        return false;

    g_dockDrag.drag->at = point;

    if (!g_dockDrag.drag->live) {
        const float T = dragThresholdPx(monitor);
        if (g_dockDrag.drag->start.distanceSq(point) <= T * T)
            return false;

        g_dockDrag.drag->live = true;
        // Past the threshold it is a DRAG: the ghost carries the icon now, and
        // the cell it left must not stay pressed in behind it.
        pressEnd();
    }

    g_dockDrag.drag->insert = g_dockDrag.drag->stackPath.empty() ? dropIndexAt(monitor, point) : stackDropIndexAt(monitor, point);
    return true;
}

// Defined down with the panel code (it needs panelContains/panelCellAt), but
// dragRelease sits above that and has to hand a live tear-out off to it.
bool panelDragRelease(PHLMONITOR monitor, const Vector2D& point);

// V3. The release, resolved. Everything below the ladder is the SAME code X32
// ran; what is new is that a drop which leaves the pill now has somewhere to go
// other than nowhere.
//
// Returns whether the gesture was a DROP. False means "this was a click" and
// the caller runs its click ladder -- which is the X16 recovery X32 wrote, kept
// where it was in the order.
bool dragRelease(PHLMONITOR monitor, const Vector2D& point) {
    if (g_dockDrag.panel)
        return panelDragRelease(monitor, point);

    if (!g_dockDrag.drag)
        return false;

    const auto DRAG = *g_dockDrag.drag;
    g_dockDrag.drag.reset();

    // A STACK being reordered (dock stacks A): only among the stacks, only a
    // live drag that ends on the pill. A release near the press is a click --
    // it opens the stack.
    if (!DRAG.stackPath.empty()) {
        const float CLICKTOL = 3.F * dragThresholdPx(monitor);
        if (!DRAG.live || DRAG.start.distanceSq(point) <= CLICKTOL * CLICKTOL)
            return false;
        const int INDEX = stackDropIndexAt(monitor, point);
        if (INDEX < 0)
            return true; // off the pill: nothing moves (removing is the menu's job)
        // INDEX counts the dragged stack itself; a later slot lands one short otherwise.
        size_t from = 0;
        for (const auto& S : Stacks::all()) {
            if (S.path == DRAG.stackPath)
                break;
            ++from;
        }
        const size_t TO = sc<size_t>(INDEX) > from ? sc<size_t>(INDEX) - 1 : sc<size_t>(INDEX);
        Stacks::moveTo(DRAG.stackPath, TO);
        return true;
    }

    // USER-REPORTED P0 2026-08-06: "I cant use the dock clicking it does
    // nothing". This is X16's lesson arriving on the dock, and it is why the
    // scripted rig could not see it -- that rig jitters 4px, under the 10px
    // getDragThreshold, so it never went down this path at all.
    //
    // A drag goes live the moment the pointer passes that threshold, and a real
    // hand wobbles 10-20px between press and release. The release then landed
    // HERE, reordered the favourite, and returned true -- which swallowed the
    // click, so the app never minimized or launched. To the user that is a dead
    // dock, and Y4 made it worse by making the click the primary gesture.
    //
    // A release that lands near where the press did is a CLICK, whatever the
    // pointer did in between. Same 3x tolerance X16 settled on for window tiles.
    const float CLICKTOL = 3.F * dragThresholdPx(monitor);

    // IN TWO STAGES, for the reason DropDecision.hpp gives for the window
    // drop's own ladder: the gathering is not free, and resolveIcon is total at
    // every stage. Stage one is what the dock already knew, and the two
    // conclusions it can reach on its own -- a click and a reorder -- are the
    // COMMON path. Every dock click in the session comes through here, so
    // walking the overview's strip and rebuilding the item model to answer
    // "was that a click" would be a cost paid by the gesture that needs none
    // of it.
    const int        INDEX = dropIndexAt(monitor, point);
    Drop::SIconFacts facts{
        .haveItem  = !DRAG.id.empty(),
        .live      = DRAG.live,
        .nearStart = DRAG.start.distanceSq(point) <= CLICKTOL * CLICKTOL,
        .onPill    = INDEX >= 0,
    };

    switch (Drop::resolveIcon(facts)) {
        case Drop::EIconAction::Click: return false;

        // X32's reorder, unchanged: a favourite moved to the slot the caret drew.
        case Drop::EIconAction::Reorder: pinAt(DRAG.id, INDEX); return true;

        default: break;
    }

    // Stage two: the overview, and where in it the button came up.
    const auto OVERVIEW = openScrollOverviewForMonitor(monitor);
    const auto STRIPWS  = OVERVIEW ? OVERVIEW->stripWorkspaceAt(point) : PHLWORKSPACE{};
    const auto ACTIVEWS = monitor ? monitor->m_activeWorkspace : PHLWORKSPACE{};

    facts.overviewOpen      = OVERVIEW != nullptr;
    facts.onStripCard       = STRIPWS != nullptr;
    facts.stripCardIsActive = STRIPWS && STRIPWS == ACTIVEWS;

    // The dragged item, found again by the id the press recorded. The layout has
    // reflowed since (an app can have opened or closed mid-drag), so this is a
    // fresh lookup and not the SItem copy the press held -- what is carried over
    // from the press is the drag's own memory: the icon and the cell it left.
    const auto LAYOUT = layout(monitor);
    const auto ITEM   = std::ranges::find_if(LAYOUT.items, [&](const SItem& item) { return item.desktopId == DRAG.id; });

    switch (Drop::resolveIcon(facts)) {
        // V3 (spec V3): "anywhere on the active desktop -> launch on the active
        // workspace". Focused, and the overview (if this was its own card) closes
        // behind it -- it is where the user is, and they have just said so twice.
        case Drop::EIconAction::LaunchHere:
            if (ITEM != LAYOUT.items.end())
                launchApp(monitor, *ITEM, nullptr, true);
            return true;

        // "...onto any workspace card in the overview strip -> launch on THAT
        // workspace". SILENT, because the user is looking at a strip of desktops
        // and pointed at one that is not this one: stealing the session to it
        // would be answering a different question.
        case Drop::EIconAction::LaunchThere:
            if (ITEM != LAYOUT.items.end())
                launchApp(monitor, *ITEM, STRIPWS, true);
            return true;

        // Missed everything. X26's near miss, in the other direction: the icon
        // goes home rather than being lost or silently unpinned.
        default:
            if (!DRAG.icon.empty())
                g_dockDrag.ghostHome = SDockGhostFlight{.icon      = DRAG.icon,
                                                        .monitor   = DRAG.monitor,
                                                        .from      = ghostBoxAt(monitor, DRAG.home, DRAG.grab, point),
                                                        .home      = ITEM != LAYOUT.items.end() ? ITEM->iconBox : DRAG.home,
                                                        .startedAt = Time::steadyNow()};
            return true;
    }
}

void dragCancel() {
    pressEnd();
    g_dockDrag.drag.reset();
    g_dockDrag.panel.reset();
}

bool dragPending() {
    return g_dockDrag.drag.has_value() || g_dockDrag.panel.has_value();
}

bool dragging() {
    return (g_dockDrag.drag && g_dockDrag.drag->live) || (g_dockDrag.panel && g_dockDrag.panel->live);
}

void setForeignInsert(int index) {
    g_dockDrag.foreignInsert = index;
}

bool openMenu(PHLMONITOR monitor, const Vector2D& point) {
    closeMenu();

    if (!monitor)
        return false;

    const auto LAYOUT = layout(monitor);
    if (LAYOUT.pill.empty())
        return false;

    // The same cell the hover highlight and the left-click use, so the three
    // gestures cannot disagree about which icon is under the pointer.
    const auto ITEM = std::ranges::find_if(LAYOUT.items, [&](const SItem& item) { return hoverBox(item.iconBox, monitor->m_scale).containsPoint(point); });
    if (ITEM == LAYOUT.items.end())
        return false;

    g_menu = SMenu{.monitor = monitor->m_id, .item = *ITEM, .anchor = ITEM->iconBox, .box = {}, .rows = buildRows(*ITEM)};
    return true;
}

bool menuOpen() {
    return g_menu.has_value();
}

void closeMenu() {
    g_menu.reset();
}

bool menuWantsKeyboard() {
    return g_menu && g_menu->editing;
}

// The Rename field's keys. Enter saves (an unchanged or empty name restores the
// folder's own), Esc cancels, and every other key belongs to the field while it
// is up -- nothing typed at it reaches the window underneath.
bool menuHandleKey(uint32_t keycode) {
    if (!menuWantsKeyboard())
        return false;
    auto& M = *g_menu;
    if (keycode == 28) { // ENTER
        const auto BASE = Stacks::labelFor(M.item.stackPath);
        Stacks::setLabel(M.item.stackPath, M.editText == BASE ? std::string{} : M.editText);
        closeMenu();
        return true;
    }
    if (keycode == 1) { // ESC
        closeMenu();
        return true;
    }
    switch (keycode) {
        case 29: case 42: case 54: case 56: case 97: case 100: case 125: case 126: return false; // modifiers
        default: break;
    }
    if (keycode == 14) { // BACKSPACE: on the untouched name, clears it all
        if (M.editFresh)
            M.editText.clear();
        else
            TextInput::popCodepoint(M.editText);
    } else if (const auto K = TextInput::keyFor(overviewActiveXkbState(), keycode); K.consume) {
        if (M.editFresh)
            M.editText.clear(); // typing over the selected name, as any rename field does
        M.editText += K.text;
    } else
        return true;
    M.editFresh = false;
    M.rows.front().label = M.editText + "\u2502";
    for (const auto& MON : State::monitorState()->monitors())
        if (MON && MON->m_id == M.monitor)
            g_pHyprRenderer->damageMonitor(MON);
    return true;
}

bool menuContains(PHLMONITOR monitor, const Vector2D& point) {
    return g_menu && monitor && g_menu->monitor == monitor->m_id && !g_menu->box.empty() && g_menu->box.containsPoint(point);
}

// While Get Info waits on the package lookup, ask for a frame now and then so
// the answer appears without the pointer having to move. A main-thread timer,
// not a frame request from inside the render pass.
namespace {
    wl_event_source* g_infoPoll = nullptr;

    int infoPollCb(void*) {
        if (!g_menu || !g_menu->infoView)
            return 0;
        for (const auto& MON : State::monitorState()->monitors())
            if (MON && MON->m_id == g_menu->monitor)
                g_pHyprRenderer->damageMonitor(MON);
        if (AppInfo::generation() == g_menu->infoGen && g_infoPoll)
            wl_event_source_timer_update(g_infoPoll, 60);
        return 0;
    }
}

void armInfoPoll() {
    if (!g_infoPoll)
        g_infoPoll = wl_event_loop_add_timer(g_pCompositor->m_wlEventLoop, infoPollCb, nullptr);
    if (g_infoPoll)
        wl_event_source_timer_update(g_infoPoll, 60);
}

void stopUrgentHopTimer() {
    if (g_hopTimer) {
        wl_event_source_remove(g_hopTimer);
        g_hopTimer = nullptr;
    }
}

void stopInfoPoll() {
    if (g_infoPoll) {
        wl_event_source_remove(g_infoPoll);
        g_infoPoll = nullptr;
    }
}

bool menuClick(PHLMONITOR monitor, const Vector2D& point) {
    if (!menuContains(monitor, point))
        return false;

    const auto ROW = std::ranges::find_if(g_menu->rows, [&](const SRow& row) { return row.kind != ERow::Separator && !row.box.empty() && row.box.containsPoint(point); });
    if (ROW == g_menu->rows.end())
        return false; // the plate, between rows: swallowed by the caller, menu stays up

    // Get Info / Back: the menu stays up and changes what it shows.
    // Dock stacks A: Empty Folder asks first, in place; Rename becomes a field.
    if (ROW->kind == ERow::StackEmptyAsk) {
        const auto N = StackReader::listing(g_menu->item.stackPath).entries.size();
        g_menu->rows = {
            SRow{.kind = ERow::InfoLine, .label = "Move " + std::to_string(N) + (N == 1 ? " item" : " items") + " to Trash?", .glyph = MenuGlyph::TRASH},
            SRow{.kind = ERow::Separator},
            SRow{.kind = ERow::StackEmptyCancel, .label = "Cancel", .glyph = MenuGlyph::BACK},
            SRow{.kind = ERow::StackEmptyConfirm, .label = "Move to Trash", .glyph = MenuGlyph::TRASH, .danger = true},
        };
        return true;
    }
    if (ROW->kind == ERow::StackEmptyCancel) {
        g_menu->rows = buildRows(g_menu->item);
        return true;
    }
    if (ROW->kind == ERow::StackRename) {
        g_menu->editing   = true;
        g_menu->editText  = g_menu->item.locationName;
        g_menu->editFresh = true;
        g_menu->rows     = {SRow{.kind = ERow::StackRenameField, .label = g_menu->editText + "\u2502", .glyph = MenuGlyph::PENCIL, .hint = "type to rename \u00b7 \u21b5 save"}};
        return true;
    }
    if (ROW->kind == ERow::StackRenameField || ROW->kind == ERow::InfoLine)
        return true; // the field and the question are not buttons

    if (ROW->kind == ERow::Details || ROW->kind == ERow::InfoBack) {
        const bool INFO    = ROW->kind == ERow::Details;
        const auto ITEM    = g_menu->item;
        g_menu->infoView   = INFO;
        g_menu->infoGen    = AppInfo::generation();
        g_menu->rows       = INFO ? infoRows(ITEM) : buildRows(ITEM);
        if (INFO)
            armInfoPoll();
        return true;
    }

    // Copied before the menu goes away: activating a row can close the overview,
    // and g_menu must not be read through after that.
    const auto ITEM = g_menu->item;
    const auto PICK = *ROW;
    closeMenu();

    activateRow(monitor, ITEM, PICK);
    return true;
}

// Defined in the unnamed namespace further down, with the rest of the hover
// panel. Declared INSIDE one here so it is the same entity: at namespace scope
// this would be a second, external-linkage function and every call below would
// be ambiguous.
namespace {
bool panelNeedsFramesOn(const PHLMONITOR& monitor);

// How much of `monitor` one of the dock's own animation frames has to repaint,
// in monitor-local RENDER px -- or nullopt for "all of it". Defined with the
// hover panel much further down, because the panel's drawn box is one of the
// things the band has to reach over.
std::optional<CBox> dockDamageRegion(PHLMONITOR monitor, const SLayout& layout);
}

// ONE OUTSTANDING FRAME REQUEST PER MONITOR, and what it has to damage.
//
// The region is accumulated rather than taken from the first caller: within one
// event-loop iteration the dock's own animation and a live preview on another
// output can both ask, and a request that kept only the first box would leave
// the second one's pixels stale. `full` wins over any box, which is what a
// caller that cannot describe its damage asks for.
struct SPumpRequest {
    bool full    = false;
    bool haveBox = false;
    CBox box; // monitor-local RENDER px, the union of every region asked for
};

std::unordered_map<MONITORID, SPumpRequest> g_pumpPending;
std::unordered_map<MONITORID, uint64_t>     g_pumpSeqs;

// A PUMP THAT OUTLIVES THE PLUGIN IS A CALL INTO UNMAPPED MEMORY.
//
// The doLater a pump queues runs on the NEXT event-loop iteration, and
// PLUGIN_EXIT is followed straight away by the dlclose. releasePointerFocus()
// used to take every outstanding request out, which is part of why main.cpp
// calls it first at unload -- but it is also called from the pointer hook and
// from an overview opening, where dropping another monitor's pump froze its
// dock mid-animation (user, 2026-09-14). So the guarantee moves here, where no
// input event can trigger it: a static whose destructor runs when this object is
// unloaded. Declared AFTER the two maps, so it is destroyed BEFORE them.
//
// Sound whichever way the host unloads: with a dlclose this runs and the
// requests are cancelled; without one the code stays mapped and a late callback
// is merely a damaged monitor.
struct SPumpReaper {
    ~SPumpReaper() {
        if (!g_pEventLoopManager)
            return;

        for (auto [id, seq] : g_pumpSeqs) {
            if (seq)
                g_pEventLoopManager->removeDoLater(seq);
        }
        g_pumpSeqs.clear();
        g_pumpPending.clear();
    }
};

SPumpReaper g_pumpReaper;

// ASK FOR ANOTHER FRAME. Every animation the dock owns runs on a desktop where
// nothing else is moving, so nothing else damages the monitor -- and a damage
// requested from INSIDE the render pass is cleared with that pass, so it has to
// be posted for after it. doLater is that, and it both starts and sustains the
// loop.
//
// ONE REQUEST PER FRAME, and that is not tidiness either. damageIfChanged runs
// on every pointer MOTION event, and motion events arrive far faster than
// frames do -- so a predicate that is true while the pointer merely rests on a
// dock icon (the hover panel is up, a bounce is running) posts a doLater per
// motion, each of which damages the monitor, each of which causes a frame,
// whose render.pre calls damageIfChanged again. Measured 2026-08-14: without
// this guard zflight goes from 8/8 green to z4/z4c/z5/z6 red with the dock
// missing clicks outright (zrig/danglingC2/zflight-pump-flood.log). The
// collapse is exact rather than approximate: two damageMonitor calls in one
// event-loop iteration are the same region twice, so dropping the second
// changes nothing but the flood.
//
// THE MARK IS ERASED, NOT SET FALSE, and the difference is a latch. `PENDING`
// is the only thing standing between a motion event and another doLater, so a
// mark that survives its callback is a dock that never asks for a frame again:
// the entry reads true, every pumpFrame returns early, and the reveal, the
// bounce and the flights all stop mid-animation on that output with nothing in
// any log. The callback erases, and forgetMonitor() erases on the way out --
// because a doLater queued against a monitor that is then removed is dropped
// with the loop, and its callback is exactly what would have cleared the mark.
void pumpFrame(PHLMONITOR monitor, std::optional<CBox> regionRenderPx = std::nullopt) {
    if (!monitor)
        return;

    const bool ALREADY = g_pumpPending.contains(monitor->m_id);

    auto&      REQ     = g_pumpPending[monitor->m_id];
    if (!regionRenderPx)
        REQ.full = true;
    else if (!REQ.full && !regionRenderPx->empty()) {
        if (!REQ.haveBox) {
            REQ.box     = *regionRenderPx;
            REQ.haveBox = true;
        } else {
            // The union, spelled out: CBox has no "grow to contain" of its own.
            const double X0 = std::min(REQ.box.x, regionRenderPx->x);
            const double Y0 = std::min(REQ.box.y, regionRenderPx->y);
            const double X1 = std::max(REQ.box.x + REQ.box.width, regionRenderPx->x + regionRenderPx->width);
            const double Y1 = std::max(REQ.box.y + REQ.box.height, regionRenderPx->y + regionRenderPx->height);
            REQ.box         = CBox{X0, Y0, X1 - X0, Y1 - Y0};
        }
    }

    if (ALREADY)
        return;

    if (g_pEventLoopManager) {
        if (const auto IT = g_pumpSeqs.find(monitor->m_id); IT != g_pumpSeqs.end() && IT->second) {
            g_pEventLoopManager->removeDoLater(IT->second);
            g_pumpSeqs.erase(IT);
        }
        g_pumpSeqs[monitor->m_id] = g_pEventLoopManager->doLater([MONITOR = PHLMONITORREF{monitor}, ID = monitor->m_id] {
            SPumpRequest request;
            if (const auto IT = g_pumpPending.find(ID); IT != g_pumpPending.end())
                request = IT->second;
            g_pumpPending.erase(ID);
            g_pumpSeqs.erase(ID);
            if (!MONITOR)
                return;

            const auto MON = MONITOR.lock();
            if (request.full || !request.haveBox || request.box.empty()) {
                g_pHyprRenderer->damageMonitor(MON);
                return;
            }

            // damageBox wants GLOBAL LOGICAL coordinates and everything the dock
            // holds is monitor-local render px: divide out the scale, add the
            // output's position. Rounded OUTWARD, because a box that lands half
            // a pixel inside its own damage leaves a stale row.
            const double SCALE = std::max(MON->m_scale, 0.01F);
            CBox         box{request.box.x / SCALE + MON->m_position.x, request.box.y / SCALE + MON->m_position.y, request.box.width / SCALE, request.box.height / SCALE};
            box.expand(1.0);
            g_pHyprRenderer->damageBox(box.round());
        });
    }
}

// AN OUTPUT THAT IS GONE OWNS NO STATE. The dock keeps three maps keyed by
// monitor id -- the visual state damageIfChanged diffs against, the autohide
// reveal, and the frame-pump mark -- and none of them had anything that removed
// a row. Two of the three merely leak a handful of bytes per hotplug; the third
// is the latch described above. A stale row is not only waste either: nothing
// here promises a MONITORID is never handed out twice, and a row that outlives
// its output is the state a later one would be judged against.
//
// Called from the monitor.removed handler beside Flight::abortAll -- the same
// place, and for the same reason, as P0's overview teardown.
void forgetMonitor(MONITORID id) {
    g_visualStates.erase(id);
    g_reveal.erase(id);
    g_pumpPending.erase(id);
    g_baseLayouts.erase(id);
    g_arrange.erase(id);
    DockTilt::forget(id);
    if (g_pEventLoopManager) {
        if (const auto IT = g_pumpSeqs.find(id); IT != g_pumpSeqs.end() && IT->second) {
            g_pEventLoopManager->removeDoLater(IT->second);
            g_pumpSeqs.erase(IT);
        }
    }
}

// IS ANYTHING ON THE DOCK STILL MOVING? The one list, in one place.
//
// It was three: an inline `autohideAnimating || WIGGLING || overlayUp` in
// damageIfChanged, a `T < 1` inside renderTooltip, and a four-term early return
// at the bottom of render(). Each had its own copy of the same doLater, and a
// new animation had to find and join all three -- which is how the tooltip fade
// ended up with a pump of its own that nothing else knew about.
//
// It is still called from TWO places, and that is deliberate rather than a job
// half done. damageIfChanged runs on render.pre AND on pointer motion, so it is
// the only one reached while the dock is fully away (render() returns on an
// empty pill) -- that is what lets a reveal start at all. The render tail runs
// after the flight loop has advanced, which is where the flight animations must
// be asked from: moving that request to before the tick is the exact reordering
// that made three reversals in a row fire against flights that had never drawn
// a frame (see the note above Flight::renderFlights).
const char* needsFramesReason(PHLMONITOR monitor, const SLayout& layout, const STally& tally) {
    // X30. A flip waiting out its delay looks EXACTLY like the frame before it,
    // so the visual-state check in damageIfChanged would stop the chain of
    // frames dead and the dock would freeze half-way -- or never start.
    if (autohideAnimating(monitor))
        return "autohide";

    // X31. The wiggle's rotation changes every frame while it runs.
    //
    // Gated on the pill being ON SCREEN for this monitor (user, 2026-09-14): a
    // wiggle nobody can see is not worth a frame, and `layout` here is the SLID
    // layout, which is empty once the dock is fully away.
    //
    // ponytail, STILL OPEN: this pumps frames for the 2500ms of every 3000ms the
    // urgent icon is deliberately STILL, as well as the 500 it moves. Waking on
    // a timer for the next burst was measured and then NOT built: CEventLoopManager
    // holds its timers by strong SP, nothing in this file is reachable from
    // PLUGIN_EXIT to remove one, and a timer that outlives the dlclose is the
    // exact crash shape main.cpp's removeAllOfType note describes. It needs an
    // unload hook first.
    if (!layout.pill.empty() && Config::getValue<bool>(KEY_DANCE_URGENT) && anyUrgent(layout.items, tally))
        return "urgent-dance";

    // The hop, dock hidden: frames only while it is in the air (plus one to
    // land it); the timer wakes the next one.
    if (layout.pill.empty() && Config::getValue<bool>(KEY_AUTOHIDE) && Config::getValue<bool>(KEY_URGENT_NOTIFY) && anyUrgent(baseLayout(monitor).items, tally) &&
        std::fmod(elapsedMs(urgentEpoch()), WIGGLE_PERIOD_MS) < URGENT_HOP_MS + 50.F)
        return "urgent-hop";

    // X34. The hotkey overlay fades.
    if (overlayUp())
        return "hotkey-overlay";

    // The press squish, and above all its spring-back, which runs after the
    // button came up with no pointer motion left to repaint it.
    if (pressAnimating(monitor->m_id))
        return "icon-press";

    // The hover lean follows the pointer on a spring, and springs back after it
    // leaves -- again with no motion left to ask for the frames.
    if (DockTilt::animating(monitor->m_id))
        return "icon-tilt";

    // Y2. A pointer resting on an icon emits no motion, so nothing else would
    // ask for the frames the tooltip fade needs. renderTooltip clears the name
    // when its exit finishes, so a settled tooltip answers false here.
    if (!g_tip.name.empty() && g_tip.monitor == monitor->m_id && elapsedMs(g_tip.since) < (g_tip.out ? TOOLTIP_FADE_OUT_MS : TOOLTIP_FADE_MS))
        return "tooltip-fade";

    // V2. The SUPER-hold pill eases in and out over windows on a desktop where
    // nothing else is repainting, exactly like everything above it. It is asked
    // here rather than pumped from its own module for the reason this function
    // exists at all: one list, in one place, so a new animation cannot join two
    // of the three pumps and be forgotten by the third.
    if (WindowPill::animating())
        return "window-pill";

    // V3. The icon flying home after a missed drop. It runs after the button
    // came up, so no pointer motion is left to repaint it -- this is the only
    // thing asking for those frames, in both contexts (the overview throttles
    // plain damage, which is why the ONE list is also the right list here).
    if (ghostFlying())
        return "ghost-flight";

    // X25/X47/Y6/Y8. A minimized window's commits damage no monitor, the hover
    // panel's entrance and live previews need frames of their own, and a bounce
    // or a launch zoom runs on a dock nothing else is touching.
    // V11 V4. The stack popup's entrance, its drag ghost, and the hop or the
    // shake that acknowledges a move. Asked here rather than pumped from its
    // own module for the reason stated at the top of this function: ONE list.
    if (Stacks::needsFrames(monitor->m_id))
        return "stacks";

    // The arrangement glides on a desktop nothing else is repainting.
    if (arrangeAnimating(monitor->m_id))
        return "arrange";

    if (Flight::anyOn(monitor))
        return "flight";
    if (panelNeedsFramesOn(monitor))
        return "hover-panel";
    if (!g_flight.bounces.empty())
        return "bounce";
    if (!g_flight.launchZooms.empty())
        return "launch-zoom";

    return nullptr;
}

// The bool the render path asks for, and the NAME of whichever term answered,
// for `hyprdrive:debug frames`. One chain, so the answer and the explanation
// can never disagree (2026-09-15).
bool needsFrames(PHLMONITOR monitor, const SLayout& layout, const STally& tally) {
    return needsFramesReason(monitor, layout, tally) != nullptr;
}

bool damageIfChanged(PHLMONITOR monitor) {
    if (!monitor)
        return false;

    // X31. The bus connection is opened by the first frame that could use it, so
    // a dock switched off -- or one with both badge and urgent-notify off --
    // never opens one at all. Idempotent.
    const bool ENABLED = Config::getValue<bool>(KEY_ENABLED);
    if (ENABLED && (Config::getValue<bool>(KEY_BADGE) || Config::getValue<bool>(KEY_URGENT_NOTIFY)))
        LauncherEntry::start();

    // ONE walk of the window list for this whole call: updateAutohide's urgent
    // test, needsFrames' wiggle test and the per-item signature below all read
    // it instead of each walking the list for themselves. Skipped outright with
    // the dock off -- this function still runs on every pointer motion event.
    const auto TALLY  = ENABLED ? buildTally() : STally{};
    const bool BADGES = ENABLED && badgesEnabled();

    updateAutohide(monitor, TALLY);

    // X30. A flip waiting out its delay looks EXACTLY like the frame before it,
    // so the signature check below would stop the chain of frames dead and the
    // dock would freeze half-way -- or never start. render.pre only fires when a
    // frame is already happening, so the next one has to be asked for here, and
    // after the current frame (doLater) for the same reason the live previews
    // are: damage asked for during the pass is cleared with the pass.
    // X31's wiggle is in here for the same reason: its rotation changes every
    // frame while it runs, and nothing else on an idle desktop asks for one.
    //
    // ponytail: this pumps frames for the 2500ms of every 3000ms the urgent icon
    // is deliberately STILL, as well as the 500 it moves. Waking on a timer for
    // the next burst instead would save them; measure it before building it.
    // One layout, used by both the wiggle test and the signature below.
    // layout() rebuilds the whole item model -- a scan of every window, a set of
    // running classes, and a fresh vector of SItems carrying several strings
    // each -- and this function runs on every frame AND on every pointer motion
    // event, which arrive far faster than frames do. It was being built twice.
    const auto LAYOUT = layout(monitor);

    if (needsFrames(monitor, LAYOUT, TALLY))
        pumpFrame(monitor, dockDamageRegion(monitor, LAYOUT));

    SDockVisualState state;
    if (!LAYOUT.pill.empty()) {
        state.pillUp = true;
        state.pillX  = sc<int>(LAYOUT.pill.x);
        state.pillY  = sc<int>(LAYOUT.pill.y);
        state.pillW  = sc<int>(LAYOUT.pill.width);

        for (const auto& ITEM : LAYOUT.items) {
            // X32. The desktop id is carried as well as the class because a
            // pinned favourite HAS no class: without it, launching a pinned app
            // -- or pinning one while another is already pinned -- leaves the
            // state identical and the running dot never appears.
            // X35. The icon NAME, because the trash swaps `user-trash` for
            // `user-trash-full` without moving a box. Ceiling: the swap is seen
            // when the poll next runs AND something asks for a frame, so on a
            // wholly idle desktop the new icon can lag the deletion by a beat.
            auto& ITEMSTATE = state.items.emplace_back(SDockItemState{ITEM.desktopId, ITEM.wmClass, ITEM.uri, ITEM.icon, ITEM.running});

            // A location has no class, so every badge lookup below would key on
            // the empty string -- and a location has no notifications anyway.
            if (!ITEM.running || !ITEM.uri.empty())
                continue;

            // X31. A badge arriving, or an app going urgent, changes what the
            // pill LOOKS like without moving a single box.
            ITEMSTATE.badge  = itemBadge(ITEM, BADGES);
            ITEMSTATE.urgent = itemUrgent(ITEM, TALLY);
        }

        const auto MOUSE = (g_pInputManager->getMouseCoordsInternal() - monitor->m_position) * monitor->m_scale;
        for (size_t i = 0; i < LAYOUT.items.size(); ++i) {
            if (hoverBox(LAYOUT.items[i].iconBox, monitor->m_scale).containsPoint(MOUSE)) {
                state.hovered = sc<int>(i);
                break;
            }
        }

        // V11 V4. Everything the popup draws, in one string. Without it an
        // open fan is invisible to the comparison and its entrance stops on
        // its first frame -- the same failure the hover panel had.
        state.stacks = Stacks::visualState();

        state.hoverShowApps = !LAYOUT.showApps.empty() && hoverBox(LAYOUT.showApps, monitor->m_scale).containsPoint(MOUSE);
        if ((state.hovered >= 0 || state.hoverShowApps) && DockTilt::hoverEnabled()) {
            state.tiltX = sc<int>(std::round(MOUSE.x));
            state.tiltY = sc<int>(std::round(MOUSE.y));
        }
        state.caret         = caretIndex();
        state.overlay       = overlayUp();

        if (ghostShowing()) {
            const auto GHOST = ghostBoxAt(monitor, g_dockDrag.drag->home, g_dockDrag.drag->grab, g_dockDrag.drag->at);
            state.ghostX     = sc<int>(GHOST.x);
            state.ghostY     = sc<int>(GHOST.y);
        }

        if (g_menu && g_menu->monitor == monitor->m_id) {
            state.menuUp    = true;
            state.menuClass = g_menu->item.wmClass;
            state.menuUri   = g_menu->item.uri;
            for (size_t i = 0; i < g_menu->rows.size(); ++i) {
                if (!g_menu->rows[i].box.empty() && g_menu->rows[i].box.containsPoint(MOUSE)) {
                    state.menuRow = sc<int>(i);
                    break;
                }
            }
        }
    }

    auto& LAST = g_visualStates[monitor->m_id];
    if (LAST == state)
        return false;

    LAST = std::move(state);
    g_pHyprRenderer->damageMonitor(monitor);
    return true;
}

// X54. The PUBLIC way to minimize or restore. Everything must come through
// these: onMinimizeDispatcher called Minimize::minimize straight through, so a
// keybind-driven minimize never armed a flight at all -- which is why every rig
// pointed at the dispatcher measured "no animation", and why the diagnostic log
// came back EMPTY rather than wrong.
bool dockOwnsNativeDrag() {
    return g_dockNativeDrag;
}

void clearDockNativeDrag() {
    g_dockNativeDrag = false;
}

void setWindowDragActive(bool active) {
    g_flight.windowDragActive = active;
}

// THE ONE FROSTED PLATE. Every plate in this rice is the same two draws in the
// same order -- the live blur behind it, then the theme's tint on top, both at
// the caller's alpha -- and they were five copies of that pair before V2 and V4
// each added a sixth. See the note in Dock.hpp.
// The theme's accent: Hyprland's active border, the colour labelPill's ring uses.
CHyprColor accentColor() {
    if (auto* const ACTIVE = sc<::Config::CGradientValueData*>(ScrollOverview::Config::valueRef<::Config::IComplexConfigValue>("general:col.active_border").ptr());
        ACTIVE && !ACTIVE->m_colors.empty())
        return ACTIVE->m_colors[0];
    return ScrollOverview::Config::themeForeground();
}

void frostedPlate(PHLMONITOR monitor, const CBox& box, int rounding, float alpha) {
    // Every caller (the dock, the stacks' fan/pane/field, the menu) comes
    // through here: a zero or negative box would RASSERT in Hyprland's renderer
    // and take the compositor down (review 2026-09-24: the pane's search field
    // on its first frame, with a small dock icon).
    if (!Stacks::drawableBox(box) || alpha <= 0.F)
        return;
    queueDockFrost(monitor, box, rounding, alpha);

    const auto                  TINT = background();
    CRectPassElement::SRectData bg;
    bg.box           = box;
    bg.color         = CHyprColor{TINT.r, TINT.g, TINT.b, TINT.a * alpha};
    bg.round         = rounding;
    bg.roundingPower = 2.F;
    g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(bg));
}

void minimizeWithFlight(PHLMONITOR monitor, PHLWINDOW window, bool bounceOnLand) {
    Flight::minimize(monitor, window, bounceOnLand);
}

void restoreWithFlight(PHLMONITOR monitor, PHLWINDOW window) {
    Flight::restore(monitor, window);
}

// ---- what the FLIGHT module reaches back into the dock for -----------------
//
// Three pieces of dock state outlive the extraction because they are the dock's,
// not the flight's: the autohide reveal, the Y8 bounce list, and the Y1 hover
// panel. They are named (external linkage) so FlightController.cpp can call
// them; each is a mutator of a g_ that stays here.

// X54. Bring the pill out NOW, skipping show_delay. A flight is 180ms and the
// delay alone is 250, so "ask nicely and wait" meant the animation was over
// before the dock arrived -- and Dock::render returns early on an empty pill, so
// the flight was never drawn at all. The autohide slide still plays; the flight
// simply targets the icon while it moves, which is the right look anyway.
void revealNow(PHLMONITOR monitor) {
    if (!monitor)
        return;

    auto& R = g_reveal[monitor->m_id];
    if (R.shown)
        return;

    R.shown     = true;
    R.pending   = false;
    R.changedAt = Time::steadyNow();
}

// Y8/Z10. The icon hops when the thing it is receiving is acknowledged.
void armBounce(const std::string& wmClass) {
    g_flight.bounces.emplace_back(SBounce{.wmClass = wmClass, .startedAt = Time::steadyNow()});
}


namespace {

// OVERVIEW_CPP_SPEC 13.12 X47. HOVER RAISES A LIVE PREVIEW.
//
// The previews are REAL, LIVE, compositor-rendered windows -- X25's requirement
// carried over unchanged into this new home. What X47 changes is WHEN they are
// drawn: hover, rather than a permanent slot in the pill (which X46 deleted).
// That is also what makes this cheaper than the inline previews it replaces --
// 13.9's live-on-demand constraint says a window nobody renders gets no frame
// callbacks, and hover is the natural on-demand signal.
//
// X25's drawing path, lifted out of render()'s lambda so the panel can use it
// too. Force visibility for anything NOT currently shown on this monitor's
// active desktop: minimized (special stash workspace) AND windows on other
// workspaces. On-screen active-desktop windows need none of it — asserting
// their alpha would stomp compositor animations. X99: other-desktop previews
// went blank after X91 because only minimized was forced.
// Y5. `alpha` is the flight's fade. It is applied AFTER forcePreviewVisible,
// which warps the window's alpha to 1 as part of making a stashed window
// drawable at all -- setting it before would simply be overwritten.
// `asOverviewTile` picks WHICH real-window path this draws through, and it
// matters more than it looks. Both render the ACTUAL window -- neither is a
// screenshot -- but the dock path (X91) early-returns after a scale+translate
// modif, BEFORE the block that scales blur to the render scale and forces LIVE
// blur. That is right for a 130px thumbnail and wrong for the flight, which is
// a full-size window shrinking across the screen: on the dock path its frost is
// neither scaled nor live, so it carries stale blur for the whole animation.
// The flight therefore goes through the OVERVIEW path, exactly as a tile does.
// Y11. Every window the dock has drawn a live preview of recently, so the
// OVERVIEW can be told not to starve it of frame callbacks.
//
// Time-stamped rather than cleared on a frame boundary: render() runs once per
// monitor and the compositor's frame-event pass can fall on either side of it,
// so a set that was emptied at the top of one monitor's render would answer
// "no" for windows the other monitor is previewing. A window drawn within
// PREVIEWING_MS is being previewed; nothing else has to be true.
constexpr float                                            PREVIEWING_MS = 250.F;
std::vector<std::pair<PHLWINDOWREF, Time::steady_tp>>      g_previewed;

// REAL milliseconds, deliberately NOT the dock's elapsedMs. That one divides by
// plugin:scrolloverview:anim_scale, which is right for every animation in this
// file and wrong here: this is a staleness window on a frame-callback policy,
// not a curve. At anim_scale 10 it would hold entries for 2.5s, and at 0.05 it
// would expire them in 12ms and starve the previews it exists to keep alive.
float realElapsedMs(const Time::steady_tp& since) {
    return sc<float>(std::chrono::duration<float, std::milli>(Time::steadyNow() - since).count());
}

void notePreviewing(const PHLWINDOW& window) {
    const auto NOW = Time::steadyNow();
    std::erase_if(g_previewed, [&](const auto& e) { return !e.first.lock() || realElapsedMs(e.second) >= PREVIEWING_MS; });

    for (auto& [REF, AT] : g_previewed) {
        if (REF.lock() == window) {
            AT = NOW;
            return;
        }
    }

    g_previewed.emplace_back(PHLWINDOWREF{window}, NOW);
}

void renderLivePreview(PHLMONITOR monitor, const PHLWINDOW& window, const CBox& box, const Time::steady_tp& now, float alpha = 1.F, bool asOverviewTile = false,
                       bool ownsRenderPass = true) {
    // BEFORE notePreviewing, which is what refreshes the entry -- asking after
    // it would always answer "healthy". See Membership::previewShadowEnabled.
    if (Membership::previewShadowEnabled()) {
        static std::unordered_map<const void*, Time::steady_tp> LASTDRAWN;
        const auto IT   = LASTDRAWN.find(window.get());
        const float GAP = IT == LASTDRAWN.end() ? -1.F : realElapsedMs(IT->second);
        Membership::previewGap(window->m_class, GAP, !isPreviewing(window), window->m_suspended);
        LASTDRAWN[window.get()] = Time::steadyNow();
        if (LASTDRAWN.size() > 64)
            LASTDRAWN.clear();
    }

    notePreviewing(window);

    const bool STASHED = Minimize::isMinimized(window);
    const bool OFF_ACTIVE =
        !window->m_workspace || !monitor || window->m_workspace != monitor->m_activeWorkspace;
    // Up on ANOTHER monitor right now: that monitor draws it and paces its frames.
    // Forcing it as well unhid group members, undid fullscreen hiding and its
    // workspace fades there, and a second presentation feedback on this
    // monitor's clock stole the client's frame timing from the one it is really
    // on (user, 2026-09-14: previews "flaky" for apps on the other monitor).
    const auto WINDOWMONITOR = window->m_monitor.lock();
    const bool SHOWN_ELSEWHERE = !STASHED && WINDOWMONITOR && WINDOWMONITOR != monitor && window->m_workspace && !window->isHidden() &&
        (window->m_workspace == WINDOWMONITOR->m_activeWorkspace || window->m_workspace == WINDOWMONITOR->m_activeSpecialWorkspace);
    // Minimized always; otherwise any window not on this monitor's active ws,
    // unless another monitor is already showing it.
    const bool NEED_FORCE = !SHOWN_ELSEWHERE && (STASHED || OFF_ACTIVE);

    bool       wasWsVisible = false;
    bool       wasWsForce   = false;
    PHLWORKSPACE ws;
    if (NEED_FORCE) {
        forcePreviewVisible(window);
        ws = window->m_workspace;
        if (ws) {
            wasWsVisible         = ws->m_visible;
            wasWsForce           = ws->m_forceRendering;
            ws->m_visible        = true;
            ws->m_forceRendering = true;
        }
    }

    const auto  SIZE        = window->m_realSize->value();
    const float RENDERSCALE = SIZE.y >= 1.0 ? sc<float>(box.height / (SIZE.y * monitor->m_scale)) : 1.F;

    OverviewWindow::renderOverviewWindow({
        .monitor                 = monitor,
        .window                  = window,
        .windowBox               = box,
        .renderScale             = RENDERSCALE,
        .now                     = now,
        // X90: dock THUMBNAILS must not touch decoration:blur:size / m_blurFB.
        // The flight is not a thumbnail and takes the overview path instead, so
        // its blur scales and stays live like every other real window.
        .scaleBlurToRenderScale  = asOverviewTile,
        // X91: do not rewrite real window geometry (ghost mini outline).
        .dockLivePreview         = !asOverviewTile,
        .alpha                   = alpha,
        .ownsRenderPass          = ownsRenderPass,
    });

    if (ws) {
        ws->m_visible        = wasWsVisible;
        ws->m_forceRendering = wasWsForce;
    }

    // Frame callbacks for anything that would otherwise freeze (hidden ws).
    if (NEED_FORCE)
        presentPreview(window, monitor, now);

    // A window living on another monitor also needs THAT monitor to present: a
    // fifo client's next commit is held until an output it is on does, and an
    // idle monitor never would.
    //
    // JUST THE WINDOW'S OWN BOX. This asked for the whole output, and it asks
    // once per frame for as long as the preview is up -- so previewing one
    // window of the other screen kept that entire screen, every window on it and
    // every blur behind them, repainting at its refresh rate (user,
    // 2026-09-14). Releasing a held commit needs a frame on that output, not a
    // full redraw of it, and the window's own rectangle is a frame.
    if (WINDOWMONITOR && WINDOWMONITOR != monitor && !asOverviewTile) {
        const double WSCALE = std::max(WINDOWMONITOR->m_scale, 0.01F);
        const auto   POS    = window->m_realPosition->value();
        const auto   WSIZE  = window->m_realSize->value();

        CBox         local{(POS.x - WINDOWMONITOR->m_position.x) * WSCALE, (POS.y - WINDOWMONITOR->m_position.y) * WSCALE, WSIZE.x * WSCALE, WSIZE.y * WSCALE};
        // A window's decorations and shadow live outside m_realSize; the pump
        // only has to produce a frame, but damaging a little wide costs nothing
        // and keeps a border from going stale if one ever rides this path.
        local.expand(32.0 * WSCALE);

        pumpFrame(WINDOWMONITOR, local);
    }
}

// The panel GROWS OUT OF THE ICON rather than fading in on the spot -- the same
// trick X54's flight uses, and it is why this needs no alpha on the window
// content: renderOverviewWindow has no alpha parameter, but it scales, so the
// entrance is a box lerp and the previews ride it.
struct SPanel {
    std::string     wmClass;
    std::string     desktopId;
    // Non-empty when the panel belongs to the SCRATCHPAD's slot rather than an
    // app's. That slot has no class to remember, and the distinction also
    // decides whether the cells wear the "in scratchpad" label: on the
    // scratchpad's own panel every cell would say it.
    std::string     scratchpadName;
    // The monitor whose dock it grew out of. ONE panel exists, and every
    // monitor's render() used to drive it: the monitor the pointer was NOT on
    // saw no hover, marked it leaving, and redrew it with its own pixel space
    // -- the flicker, the stray dismissals and the preview on the wrong screen
    // (user, 2026-09-14). Only this monitor touches it now.
    MONITORID       monitor = MONITOR_INVALID;
    CBox            anchor;                          // the icon it belongs to
    CBox            target;                          // where it settles
    CBox            from;                            // where the current move started
    Time::steady_tp moveSince = Time::steadyNow();
    float           moveMs    = PANEL_IN_MS;
    bool            decel     = true;                // entrances decelerate, exits do not
    bool            out       = false;
    bool            leaving   = false;               // inside the grace period
    Time::steady_tp leftAt    = Time::steadyNow();
};

// How far a parked window's preview is darkened, and the label's point size.
// The dim is deep enough to read as "not here" against the live thumbnails
// beside it without hiding which window it is.
constexpr float PANEL_PARKED_DIM = 0.45F;
constexpr float PANEL_PARKED_PT  = 11.F;

// The hover panel: the state it is in, and -- Y1/Y2 -- the geometry it was
// actually DRAWN at last frame, plus the cell each window landed in.
// Hit-testing has to use the drawn boxes rather than the settled target:
// during the entrance and the slide the plate on screen is somewhere else
// entirely, and a pointer resting on what it can see must not fall through a
// box that has not arrived yet. Same reasoning the X28 menu already documents
// for its rows.
struct SDockHoverPanel {
    std::optional<SPanel>                      panel;
    CBox                                       drawn; // in the panel's own monitor's pixels
    std::vector<std::pair<CBox, PHLWINDOWREF>> cells;
};

SDockHoverPanel g_hover;

// [FLIGHT] dismissHoverPanelNow moved out of the anonymous namespace -- the
// flight module calls it across a TU boundary now -- and is defined below,
// where this anonymous block closes. g_hover / g_tip stay here.


void forgetPanelGeometry() {
    g_hover.drawn = CBox{};
    g_hover.cells.clear();
}

// Y1. Inside the plate that is actually on screen -- and not while it is
// leaving, or a dismissal already under way would be resurrected by a pointer
// that merely happens to be where the shrinking plate still is.
//
// `point` is in `monitor`'s pixels, and the panel lives on one monitor only: the
// same pixel on the other one is not inside it.
bool panelOn(const PHLMONITOR& monitor) {
    return monitor && g_hover.panel && g_hover.panel->monitor == monitor->m_id;
}

bool panelContains(const PHLMONITOR& monitor, const Vector2D& point) {
    return panelOn(monitor) && !g_hover.panel->out && g_hover.drawn.width >= 1 && g_hover.drawn.height >= 1 && g_hover.drawn.containsPoint(point);
}

// Y2. The preview cell under the pointer and the window in it. The BOX comes
// back with the window because the title plate is centred on the cell, not on
// the icon the panel belongs to.
std::optional<std::pair<CBox, PHLWINDOW>> panelCellAt(const PHLMONITOR& monitor, const Vector2D& point) {
    if (!panelOn(monitor))
        return std::nullopt;

    for (const auto& [BOX, REF] : g_hover.cells) {
        if (!BOX.containsPoint(point))
            continue;

        if (const auto WINDOW = REF.lock(); WINDOW)
            return std::make_pair(BOX, WINDOW);
    }

    return std::nullopt;
}

bool sameBox(const CBox& a, const CBox& b) {
    return sc<int>(a.x) == sc<int>(b.x) && sc<int>(a.y) == sc<int>(b.y) && sc<int>(a.width) == sc<int>(b.width) && sc<int>(a.height) == sc<int>(b.height);
}

// The windows an icon previews. itemWindows already includes minimized ones,
// which is what makes a minimized window reachable from its own icon.
std::vector<PHLWINDOW> panelWindows(const std::string& wmClass, const std::string& desktopId = {}, const std::string& scratchpadName = {}) {
    std::vector<PHLWINDOW> out;

    // The scratchpad's slot previews a WORKSPACE. Its windows have nothing in
    // common but where they are parked, so there is no class to ask for them.
    if (!scratchpadName.empty())
        out = Scratchpad::windowsOn(Scratchpad::byShortName(scratchpadName));
    else if (!wmClass.empty() || !desktopId.empty()) {
        SItem item;
        item.wmClass   = wmClass;
        item.desktopId = desktopId;
        out            = itemWindows(item);
    } else
        return {};

    std::erase_if(out, [](const auto& window) { return !window || !window->m_isMapped || !window->wlSurface() || !window->wlSurface()->resource(); });
    if (out.size() > PANEL_MAX)
        out.resize(PANEL_MAX);

    return out;
}

// One cell per window, side by side, each keeping its window's aspect.
CBox panelTarget(PHLMONITOR monitor, const SLayout& layout, const std::vector<PHLWINDOW>& windows, const CBox& anchor) {
    const float SCALE = std::max(monitor->m_scale, 0.01F);
    const float CELLH = PANEL_CELL_H * SCALE;

    float content = 0.F;
    for (size_t i = 0; i < windows.size(); ++i)
        content += CELLH * previewAspect(windows[i]) + (i ? PANEL_CELL_GAP * SCALE : 0.F);

    const float WIDTH  = std::round(content + 2.F * PANEL_PAD * SCALE);
    const float HEIGHT = std::round(CELLH + 2.F * PANEL_PAD * SCALE);
    const float CX     = sc<float>(anchor.x) + sc<float>(anchor.width) / 2.F;

    // Clamped to the monitor: an icon near either end would otherwise push half
    // the panel off screen, which is where a taskbar preview stops being one.
    const float MARGIN = PANEL_PAD * SCALE;
    const float MAXX   = sc<float>(monitor->m_size.x) * SCALE - WIDTH - MARGIN;
    const float X      = std::clamp(CX - WIDTH / 2.F, MARGIN, std::max(MARGIN, MAXX));

    return CBox{std::round(X), std::round(sc<float>(layout.pill.y) - PANEL_GAP * SCALE - HEIGHT), WIDTH, HEIGHT};
}

// A 1px sliver on the dock's TOP edge, centred on the icon. The panel grows
// from here instead of from the icon box itself, so the lerp never paints
// live window thumbnails over the rest of the pill.
CBox panelRiseOrigin(const SLayout& layout, const CBox& anchor) {
    CBox origin = anchor;
    origin.height = 1;
    origin.y      = sc<float>(layout.pill.y) - 1;
    origin.round();
    return origin;
}

// Pass an empty class when nothing is hovered; like the tooltip, the panel
// outlives the hover so that it can animate AWAY instead of blinking out.
void renderHoverPreview(PHLMONITOR monitor, const SLayout& layout, const CBox& anchor, const std::string& wmClass, const std::string& desktopId,
                        const std::string& scratchpadName, const Time::steady_tp& now) {
    auto       windows = panelWindows(wmClass, desktopId, scratchpadName);
    const bool WANT    = !windows.empty();

    if (WANT) {
        const CBox TARGET = panelTarget(monitor, layout, windows, anchor);

        if (!g_hover.panel) {
            // Rises off the dock's top edge, decelerating into place. Growing
            // from the icon box itself painted the live thumbnails over the pill.
            g_hover.panel = SPanel{.wmClass = wmClass, .desktopId = desktopId, .scratchpadName = scratchpadName, .monitor = monitor->m_id, .anchor = anchor, .target = TARGET, .from = panelRiseOrigin(layout, anchor), .moveSince = now, .moveMs = PANEL_IN_MS, .decel = true};
        } else {
            g_hover.panel->leaving = false;
            g_hover.panel->wmClass = wmClass;
            g_hover.panel->desktopId = desktopId;
            g_hover.panel->scratchpadName = scratchpadName;
            g_hover.panel->anchor  = anchor;

            // Caught mid-dismissal, or moved to another icon: in both cases the
            // move restarts from WHERE IT IS, which is what makes it read as one
            // panel following the pointer rather than two panels swapping.
            if (g_hover.panel->out || !sameBox(g_hover.panel->target, TARGET)) {
                const float T = std::clamp(elapsedMs(g_hover.panel->moveSince) / std::max(1.F, g_hover.panel->moveMs), 0.F, 1.F);

                g_hover.panel->from      = lerpBox(g_hover.panel->from, g_hover.panel->target, houseEase(g_hover.panel->decel, T));
                g_hover.panel->target    = TARGET;
                g_hover.panel->moveSince = now;
                g_hover.panel->moveMs    = g_hover.panel->out ? PANEL_IN_MS : PANEL_SLIDE_MS;
                g_hover.panel->decel     = true;
                g_hover.panel->out       = false;
            }
        }
    } else if (g_hover.panel) {
        if (!g_hover.panel->out && !g_hover.panel->leaving) {
            g_hover.panel->leaving = true;
            g_hover.panel->leftAt  = now;
        }

        // The grace expires: shrink back to the dock's top edge, accelerating.
        // Wall-clock, not animElapsedMs: at anim_scale 0 that clock is already
        // finished, so leaving an icon for the 8px gap above the pill dismissed
        // the panel in the same frame and the dock hid under the previews.
        if (g_hover.panel->leaving && ScrollOverview::Config::realElapsedMs(g_hover.panel->leftAt) >= PANEL_GRACE_MS) {
            const float T = std::clamp(elapsedMs(g_hover.panel->moveSince) / std::max(1.F, g_hover.panel->moveMs), 0.F, 1.F);

            g_hover.panel->from      = lerpBox(g_hover.panel->from, g_hover.panel->target, houseEase(g_hover.panel->decel, T));
            g_hover.panel->target    = panelRiseOrigin(layout, g_hover.panel->anchor);
            g_hover.panel->moveSince = now;
            g_hover.panel->moveMs    = PANEL_OUT_MS;
            g_hover.panel->decel     = false;
            g_hover.panel->out       = true;
            g_hover.panel->leaving   = false;
        }

        // The panel keeps drawing the LAST app's windows while it dismisses --
        // the pointer has already left, so nothing else names them.
        windows = panelWindows(g_hover.panel->wmClass, g_hover.panel->desktopId, g_hover.panel->scratchpadName);
        if (windows.empty()) {
            g_hover.panel.reset();
            forgetPanelGeometry();
            return;
        }
    } else {
        forgetPanelGeometry();
        return;
    }

    const float T = std::clamp(elapsedMs(g_hover.panel->moveSince) / std::max(1.F, g_hover.panel->moveMs), 0.F, 1.F);
    if (g_hover.panel->out && T >= 1.F) {
        g_hover.panel.reset();
        forgetPanelGeometry();
        return;
    }

    CBox        DRAWN = lerpBox(g_hover.panel->from, g_hover.panel->target, houseEase(g_hover.panel->decel, T));
    const float SCALE = std::max(monitor->m_scale, 0.01F);
    // Never let the growing plate (or its live window thumbnails) eat the pill.
    // The rise origin sits on the dock's top edge; this is the belt.
    if (const double LIMIT = layout.pill.y; DRAWN.y + DRAWN.height > LIMIT)
        DRAWN.height = std::max(0.0, LIMIT - DRAWN.y);
    DRAWN.round();

    if (DRAWN.width < 2.0 || DRAWN.height < 2.0) {
        forgetPanelGeometry();
        return;
    }

    // The plate fades with the growth so the frost does not slam on at full
    // strength over the icon it is still the size of.
    const float ALPHA   = g_hover.panel->out ? 1.F - houseEase(false, T) : houseEase(true, T);
    const int   ROUNDED = sc<int>(std::round(PANEL_RADIUS * SCALE));

    // X13/X29/X86: panel frost shares overview chrome blur when Super is open.
    frostedPlate(monitor, DRAWN, ROUNDED, ALPHA);

    // Cells are laid out inside the box that is actually on screen this frame,
    // not inside the settled one -- otherwise the contents would overflow the
    // plate for the whole of the entrance.
    const float PAD   = PANEL_PAD * SCALE;
    const float CELLH = sc<float>(DRAWN.height) - 2.F * PAD;
    const float GAPS  = PANEL_CELL_GAP * SCALE * sc<float>(windows.size() - 1);
    const float AVAIL = sc<float>(DRAWN.width) - 2.F * PAD - GAPS;
    if (CELLH < 1.F || AVAIL < 1.F) {
        forgetPanelGeometry();
        return;
    }

    float sum = 0.F;
    for (const auto& WINDOW : windows)
        sum += previewAspect(WINDOW);

    // Y1/Y2. Published for the pointer, which is why it is the DRAWN plate and
    // the DRAWN cells rather than the settled ones. Stretch the hit box down
    // through PANEL_GAP to the pill so moving from an icon into the previews
    // is never a frame of "left the dock". Cells and frost stay on DRAWN.
    g_hover.drawn = DRAWN;
    if (!g_hover.panel->out) {
        if (const double LIMIT = layout.pill.y; g_hover.drawn.y + g_hover.drawn.height < LIMIT)
            g_hover.drawn.height = LIMIT - g_hover.drawn.y;
    }
    g_hover.cells.clear();
    g_hover.cells.reserve(windows.size());

    const auto  MOUSE      = (g_pInputManager->getMouseCoordsInternal() - monitor->m_position) * monitor->m_scale;
    const int   BORDERSIZE = std::max(1, ScrollOverview::Config::getValue<int>("general:border_size"));
    const auto  ACCENT     = ScrollOverview::Config::highlightColor();
    const float ROUNDPWR   = ScrollOverview::Config::themeRoundingPower();

    float x = sc<float>(DRAWN.x) + PAD;
    for (const auto& WINDOW : windows) {
        const float WIDTH = AVAIL * previewAspect(WINDOW) / std::max(sum, 0.01F);

        CBox cell{std::round(x), std::round(sc<float>(DRAWN.y) + PAD), std::round(WIDTH), std::round(CELLH)};
        renderLivePreview(monitor, WINDOW, cell, now, 1.F, false, false);
        g_hover.cells.emplace_back(cell, PHLWINDOWREF{WINDOW});

        // A window parked on a SCRATCHPAD is not one of the app's ordinary
        // windows, and the preview says so rather than leaving the user to
        // wonder why clicking it slides a workspace in. Suppressed on the
        // scratchpad's own panel, where every cell would carry the same label.
        if (g_hover.panel->scratchpadName.empty() && Scratchpad::holdsWindow(WINDOW)) {
            const int DIMROUND = std::clamp(sc<int>(std::round(ScrollOverview::Config::themeRounding() * SCALE)), 0,
                                            std::max(0, sc<int>(std::min(cell.width, cell.height) / 2.0) - BORDERSIZE));

            CRectPassElement::SRectData dim;
            dim.box           = cell.copy().round();
            dim.color         = CHyprColor{0.F, 0.F, 0.F, PANEL_PARKED_DIM * ALPHA};
            dim.round         = DIMROUND;
            dim.roundingPower = ROUNDPWR;
            g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(dim));

            // Deliberately NOT a theme colour, and that is the whole point of
            // the label: it is read over a DARKENED thumbnail, and a dark
            // theme's own foreground is the one colour that disappears there.
            if (const auto TEX = OverviewRender::text("in scratchpad", CHyprColor{1.F, 1.F, 1.F, 0.96F}, std::max(1, sc<int>(std::round(PANEL_PARKED_PT * SCALE))),
                                                      false, "", sc<int>(cell.width));
                TEX && TEX->ok()) {
                CTexPassElement::SRenderData label;
                label.tex = TEX;
                label.box = CBox{std::round(sc<float>(cell.x) + (sc<float>(cell.width) - sc<float>(TEX->m_size.x)) / 2.F),
                                 std::round(sc<float>(cell.y) + (sc<float>(cell.height) - sc<float>(TEX->m_size.y)) / 2.F), sc<float>(TEX->m_size.x),
                                 sc<float>(TEX->m_size.y)};
                label.a   = ALPHA;
                g_pHyprRenderer->m_renderPass.add(makeUnique<CTexPassElement>(label));
            }
        }

        // Theme inactive_border is often fully transparent (Omarchy border_size
        // 0), which is why Gemini's ring was queued and still invisible. Same
        // accent the pill uses, thinner when the cell is not hovered.
        const bool IS_HOVERED = !g_hover.panel->out && cell.containsPoint(MOUSE);
        const int  ROUND      = std::clamp(sc<int>(std::round(ScrollOverview::Config::themeRounding() * SCALE)), 0,
                                           std::max(0, sc<int>(std::min(cell.width, cell.height) / 2.0) - BORDERSIZE));
        CBorderPassElement::SBorderData border;
        border.box = cell.copy().round();
        border.grad1.reset(ACCENT.modifyA(IS_HOVERED ? ALPHA : ALPHA * 0.55F));
        border.a             = 1.F;
        border.borderSize    = BORDERSIZE;
        border.round         = ROUND;
        border.roundingPower = ROUNDPWR;
        g_pHyprRenderer->m_renderPass.add(makeUnique<CBorderPassElement>(border));

        x += WIDTH + PANEL_CELL_GAP * SCALE;
    }
}

// Y1. The autohide's question, answered against the plate on screen. Declared
// far above, beside updateAutohide, which runs long before any of this is in
// scope.
bool panelHoldsDock(PHLMONITOR monitor) {
    if (!monitor || !g_hover.panel)
        return false;

    return panelContains(monitor, (g_pInputManager->getMouseCoordsInternal() - monitor->m_position) * monitor->m_scale);
}

// Whether `monitor` has frames to draw for the panel: it is the panel's monitor,
// or -- a preview is only live if its window keeps committing -- it is where a
// previewed window lives. A client on a fifo surface only gets its next commit
// released when a monitor it is on presents, and nothing else would make an
// otherwise idle monitor present.
bool panelNeedsFramesOn(const PHLMONITOR& monitor) {
    // ITS OWN MONITOR ONLY. This used to answer true for the monitor a
    // PREVIEWED WINDOW happens to live on as well, which kept that whole output
    // repainting at its refresh rate for a panel it was not drawing (user,
    // 2026-09-14). The fifo client still gets unblocked: renderLivePreview
    // damages that window's own box on that output every frame it draws it,
    // which is all a held commit needs and a thousandth of the pixels.
    return monitor && g_hover.panel && g_hover.panel->monitor == monitor->m_id;
}

// HOW MUCH OF THE OUTPUT ONE DOCK ANIMATION FRAME HAS TO REPAINT.
//
// pumpFrame damaged the WHOLE monitor for every frame of every dock animation,
// which on the plain desktop means re-rendering every window and every blur
// behind them so an icon can lean two degrees (user, 2026-09-14). The dock draws
// in a band along the bottom edge, so the band is what gets damaged -- FULL
// WIDTH, deliberately: the pill already spans most of the output, and the caret,
// the badge, the tooltip and the hover highlight all move sideways within it, so
// horizontal precision buys nothing and can only be wrong.
//
// nullopt means "damage everything", and it is the answer for anything that
// leaves the band or whose box this function has not been handed. Being wrong
// here is a STALE ROW on screen, so every unknown resolves that way.
std::optional<CBox> dockDamageRegion(PHLMONITOR monitor, const SLayout& layout) {
    if (!monitor)
        return std::nullopt;

    // WITHDRAWN (user, 2026-09-15): "going through previews it slices half of
    // the image off my windows". The band below is computed from boxes the LAST
    // frame published -- g_hover.drawn above all -- so every frame of the hover
    // panel's entrance damages the box the panel had BEFORE it grew, and the
    // strip of panel above that line keeps whatever was under it. The same hole
    // exists for anything that moves or grows between the damage decision and
    // the draw, which is most of what the dock animates.
    //
    // Damaging what the NEXT frame will draw is only knowable after laying that
    // frame out -- the region would have to come out of render(), not out of
    // damageIfChanged. Until then the dock damages its whole output, as it
    // always did: the saving was real, but it was never the reason the overview
    // stutters (that is the GPU batch, measured 2026-09-15), and correctness is
    // not the thing to trade for it.
    return std::nullopt;

    // The overview repaints the whole output every frame regardless.
    if (scrollOverviewForMonitor(monitor))
        return std::nullopt;

    // Everything that leaves the dock's neighbourhood. A flight crosses the
    // screen, an icon flies home from wherever it was dropped, a launch blow-up
    // grows to 3x and drifts upward, the V2 window pill sits on a window
    // anywhere, and a window drag paints a halo the pill's own box does not
    // describe. None of them is bounded by the band.
    if (Flight::anyOn(monitor) || ghostShowing() || ghostFlying() || !g_flight.launchZooms.empty() || !g_flight.bounces.empty() || g_flight.windowDragActive ||
        WindowPill::animating())
        return std::nullopt;

    // A stack fan or grid: transient, and its plate is laid out inside its own
    // module. Not worth a second geometry API to save a handful of frames.
    if (Stacks::popupMonitor() == monitor->m_id)
        return std::nullopt;

    const double SCALE = std::max(monitor->m_scale, 0.01F);
    const double H     = sc<double>(monitor->m_size.y) * SCALE;

    // The RESTING pill, not the slid one: while the dock is on its way out the
    // slid box is lower (or gone entirely) and the pixels being cleared are the
    // ones it used to occupy, up at the resting height.
    const auto& BASE = baseLayout(monitor);
    if (BASE.pill.empty())
        return std::nullopt; // nothing to reason about; the caller gets the lot

    double top = std::min(sc<double>(BASE.pill.y), layout.pill.empty() ? sc<double>(BASE.pill.y) : sc<double>(layout.pill.y));

    // The hover panel and the context menu float ABOVE the pill and are much
    // taller than it. Both publish the box they were actually drawn at, and
    // both start life with no box at all -- on that first frame there is
    // nothing to reach over with, so the whole output is the honest answer.
    if (g_hover.panel && g_hover.panel->monitor == monitor->m_id) {
        if (g_hover.drawn.width < 1 || g_hover.drawn.height < 1)
            return std::nullopt;
        top = std::min(top, sc<double>(g_hover.drawn.y));
    }

    if (g_menu && g_menu->monitor == monitor->m_id) {
        if (g_menu->box.empty())
            return std::nullopt;
        top = std::min(top, sc<double>(g_menu->box.y));
    }

    // GENEROUS, on purpose. Above the topmost box there is still the plate's
    // drop shadow and the frost's sampling margin, the tooltip -- which hangs
    // over the pill and has no remembered box of its own -- the bounce lifting
    // an icon out of the cell, and DockTilt's projection throwing a leaning
    // icon's corner past its rectangle. All of them are small; the band is
    // cheap; so the margin is one whole dock's worth rather than a measured
    // minimum that a future surface would quietly outgrow.
    constexpr double MARGIN = 140.0; // logical px
    top -= MARGIN * SCALE;

    // Down to the bottom edge, and past it: the autohide tucks the pill BELOW
    // the monitor and the last frames of that slide clear rows at the very
    // bottom.
    return CBox{0.0, std::max(0.0, top), sc<double>(monitor->m_size.x) * SCALE, H - std::max(0.0, top)};
}

}

// Z15 (user): a flight click kills the Y1 hover panel INSTANTLY. Leaving it
// alive re-eased it from a stale parked box, which read as "a panel dragged from
// the bottom of the screen" under the restore. The tooltip goes with it: it is a
// separate element and lingered into the flight ("sh" bubble, frame 279 of the
// verification film), and the fade-out path only runs while a name is
// remembered. Named, because FlightController.cpp calls it.
void dismissHoverPanelNow() {
    g_hover.panel.reset();
    g_tip = {};
}

bool occupiesPointer(PHLMONITOR monitor, const Vector2D& point) {
    if (!monitor)
        return false;
    return pillContains(monitor, point) || panelContains(monitor, point) || menuContains(monitor, point);
}

static uint64_t g_dockSeatDoLaterSeq = 0;

void syncPointerFocus(PHLMONITOR monitor, const Vector2D& point) {
    if (monitor && occupiesPointer(monitor, point)) {
        g_dockSeatHeld = true;
        stealDockSeat();
        if (g_pEventLoopManager) {
            if (g_dockSeatDoLaterSeq) {
                g_pEventLoopManager->removeDoLater(g_dockSeatDoLaterSeq);
                g_dockSeatDoLaterSeq = 0;
            }
            g_dockSeatDoLaterSeq = g_pEventLoopManager->doLater([] {
                g_dockSeatDoLaterSeq = 0;
                if (g_dockSeatHeld)
                    stealDockSeat();
            });
        }
        return;
    }
    restoreDockSeat();
}

void releasePointerFocus() {
    // ONLY THE SEAT. This used to cancel every monitor's outstanding frame
    // request as well, and it is called from the pointer hook on every motion
    // over a monitor with an overview on it -- so moving the mouse on one
    // output silently dropped the OTHER output's pump and froze its dock
    // mid-animation (user, 2026-09-14). The pump marks are self-clearing (see
    // pumpFrame) and forgetMonitor takes them out when an output goes; nothing
    // about the seat needs them cancelled.
    if (g_pEventLoopManager && g_dockSeatDoLaterSeq) {
        g_pEventLoopManager->removeDoLater(g_dockSeatDoLaterSeq);
        g_dockSeatDoLaterSeq = 0;
    }
    restoreDockSeat();
}

// Z2. A CLICK on a preview goes to that window. The panel floats ABOVE the pill,
// so every one of these clicks failed the `pillContains` gate both button hooks
// open with and fell through to whatever window was underneath -- the panel was
// readable and dead, which is Y1's bug one layer up. Y2 already hit-tests these
// same cells for the title, so the geometry is there; only the button was
// missing. `release` because the dock's own gesture is press-remembers /
// release-decides, and a preview must not be the one place a press acts.
//
// Claiming the press as well is what stops it starting an icon drag or reaching
// the tile below; the caller swallows either edge on a true.
//
// OUTSIDE the anonymous namespace above, and that is not a style point: put in
// it, this compiles and links, and then Hyprland dies with `undefined symbol`
// the first time a mouse button is pressed -- a plugin's unresolved symbol is
// not a load-time error, it is a lazy-binding abort at the first call, so it
// took the compositor down mid-session rather than failing the build.
// Y11. Asked by CScrollOverview::shouldAllowSurfaceFrame. See the comment there
// for why a preview cannot keep itself alive without it.
// Longer than PREVIEWING_MS: that one covers a preview being actively redrawn
// every frame, so it is refreshed constantly. This fires ONCE at the drop and
// has to outlast a browser's reflow, which is several frames, not one.
constexpr float                                       RETILED_MS = 1500.F;
std::vector<std::pair<PHLWINDOWREF, Time::steady_tp>> g_retiled;

void noteRetiled(const PHLWINDOW& window) {
    if (!window)
        return;

    std::erase_if(g_retiled, [&](const auto& e) { return !e.first.lock() || realElapsedMs(e.second) >= RETILED_MS; });
    for (auto& e : g_retiled) {
        if (e.first.lock() == window) {
            e.second = Time::steadyNow();
            return;
        }
    }
    g_retiled.emplace_back(PHLWINDOWREF{window}, Time::steadyNow());
}

bool isRetiling(const PHLWINDOW& window) {
    if (!window)
        return false;

    return std::ranges::any_of(g_retiled, [&](const auto& e) { return e.first.lock() == window && realElapsedMs(e.second) < RETILED_MS; });
}

void pumpRetiled(const PHLWINDOW& window, PHLMONITOR monitor, const Time::steady_tp& now) {
    if (!window || !monitor || !isRetiling(window))
        return;

    if (!window->wlSurface() || !window->wlSurface()->resource())
        return;

    presentPreview(window, monitor, now);
}

bool isPreviewing(const PHLWINDOW& window) {
    if (!window)
        return false;

    return std::ranges::any_of(g_previewed, [&](const auto& e) { return e.first.lock() == window && realElapsedMs(e.second) < PREVIEWING_MS; });
}

// A live preview let go of. Near where it was picked up it is still the click
// it always was; anywhere off the dock it is the window itself, landing.
bool panelDragRelease(PHLMONITOR monitor, const Vector2D& point) {
    if (!g_dockDrag.panel)
        return false;

    const auto DRAG = *g_dockDrag.panel;
    g_dockDrag.panel.reset();

    const auto WINDOW = DRAG.window.lock();
    if (!WINDOW || !monitor)
        return true;

    // The only release that still reaches here is a press that never became a
    // drag -- past the threshold the gesture was handed to the window drag and
    // this state was dropped with it. So this is the click it always was.

    // MINIMIZED IS NOT AN ORDINARY SELECTION. A stashed window lives on
    // special:minimized, which is not one of the overview's cards -- so
    // selectAndClose falls into its "this window is nowhere in the overview"
    // branch, sets closeOnWindow and closes; the close then reads
    // closeOnWindow->m_workspace and lands the viewport ON special:minimized,
    // which Omarchy slides in VERTICALLY. That is the "comes up from below the
    // screen" this looked like, and it is exactly the hazard activateWindow's
    // own comment warns about ("never toggle special:minimized"). activateWindow
    // guards it; selectAndClose never learned to. Un-stash through the guarded
    // path first, then just shut the overview.
    if (Minimize::isMinimized(WINDOW)) {
        activateWindow(WINDOW); // showOriginalWorkspace + restore, onto a real desktop
        if (const auto OVERVIEW = scrollOverviewForMonitor(monitor))
            OVERVIEW->close();
        return true;
    }

    if (const auto OVERVIEW = scrollOverviewForMonitor(monitor); OVERVIEW)
        OVERVIEW->selectAndClose(WINDOW);
    else
        activateWindow(WINDOW);
    return true;
}

bool panelClick(PHLMONITOR monitor, const Vector2D& point, bool release) {
    if (!monitor)
        return false;

    // A live tear-out answers its own release wherever it lands -- ahead of the
    // panelContains gate below, because the whole point is that it ends up
    // somewhere the panel is not.
    if (release && g_dockDrag.panel)
        return panelDragRelease(monitor, point);

    if (!panelContains(monitor, point))
        return false;

    if (!release) {
        // Record what it would take to tear this preview out. NOTHING is
        // committed here: the drag only goes live once the pointer passes the
        // threshold, so a press that does not move is still a click.
        if (const auto CELL = panelCellAt(monitor, point)) {
            g_dockDrag.panel = SPanelDrag{.window = CELL->second,
                                          .start  = point,
                                          .home   = CELL->first,
                                          .grab   = point - Vector2D{CELL->first.x, CELL->first.y},
                                          .at     = point};
        }
        return true;
    }

    const auto CELL = panelCellAt(monitor, point);
    if (!CELL)
        return true;

    const auto WINDOW = CELL->second;

    // Always go to the window on its own desktop. A restore-flight from here
    // would pull it onto the current desktop from the dock (below the screen);
    // switching first is what "click the preview of that window" means.
    if (const auto OVERVIEW = scrollOverviewForMonitor(monitor); OVERVIEW)
        OVERVIEW->selectAndClose(WINDOW);
    else
        activateWindow(WINDOW);

    return true;
}

// Y8's hop, evaluated. Exported (Dock.hpp) because a stack cell hops with the
// same motion off a ledger of its own -- the curve and HOP_MS are shared, the
// ledgers are not.
float hopLift(float t, float iconHeight) {
    return bounceCurve(std::clamp(t, 0.F, 1.F)) * BOUNCE_HEIGHT * iconHeight;
}

// Y8. How far this app's icon is lifted out of the pill right now. Reads the
// largest of any overlapping hops rather than summing them, so two windows
// landing together is one bounce and not a double-height launch.
float bounceLift(const std::string& wmClass, float iconHeight) {
    float lift = 0.F;
    for (const auto& BOUNCE : g_flight.bounces) {
        if (BOUNCE.wmClass != wmClass)
            continue;

        lift = std::max(lift, hopLift(elapsedMs(BOUNCE.startedAt) / HOP_MS, iconHeight));
    }

    return lift;
}

// Y7. Zero at the top of the bottom-30% band and one at the pill, continuous
// between. Uses the pill as laid out THIS frame, so a dock still sliding out
// glows against where it actually is rather than where it will settle.
float dragGlow(PHLMONITOR monitor, const SLayout& layout, const Vector2D& mouse) {
    if (!g_flight.windowDragActive || layout.pill.empty())
        return 0.F;

    const float H       = sc<float>(monitor->m_size.y) * monitor->m_scale;
    const float BANDTOP = H * (1.F - DRAG_GLOW_BAND);
    const float PILLTOP = sc<float>(layout.pill.y);

    if (sc<float>(mouse.y) <= BANDTOP || PILLTOP <= BANDTOP)
        return 0.F;

    return std::clamp((sc<float>(mouse.y) - BANDTOP) / (PILLTOP - BANDTOP), 0.F, 1.F);
}

// [FLIGHT] settleFlights is gone, and it is not a rename: P0's monitor-removed
// settle WAS a second landing path, written beside the first and already
// missing pieces of it (a driven restore's geometry write-back, Z18b). It is
// Flight::abortAll(reason) now -- the same landing every other end of a
// flight runs, with its invariant checked, called from monitor.removed,
// config.reloaded and PLUGIN_EXIT.

const char* frameReason(PHLMONITOR monitor) {
    if (!monitor)
        return nullptr;

    const auto LAYOUT = layout(monitor);
    const auto TALLY  = buildTally();
    return needsFramesReason(monitor, LAYOUT, TALLY);
}

// The hop's drawing: each urgent app's icon, at its RESTING x, rising out of the
// bottom edge by the hop's lift. Only ever called with the dock fully away.
void renderUrgentHop(PHLMONITOR monitor) {
    if (!Config::getValue<bool>(KEY_AUTOHIDE) || !Config::getValue<bool>(KEY_URGENT_NOTIFY))
        return;

    const auto  TALLY = buildTally();
    const auto& BASE  = baseLayout(monitor);
    std::vector<const SItem*> urgent;
    for (const auto& ITEM : BASE.items)
        if (ITEM.running && ITEM.uri.empty() && itemUrgent(ITEM, TALLY))
            urgent.push_back(&ITEM);
    if (urgent.empty())
        return;

    const float LIFT = urgentHopLift();
    if (LIFT < 0.F) {
        armHopTimer(msToNextHop());
        return;
    }

    const float EDGE = sc<float>(monitor->m_size.y * monitor->m_scale); // monitor-local render px
    for (const SItem* ITEM : urgent) {
        const auto TEX = OverviewRender::appIcon(ITEM->icon);
        if (!TEX || !TEX->ok())
            continue;
        const CBox& IB = ITEM->iconBox;
        const float UP = std::round(LIFT * URGENT_HOP_LIFT * sc<float>(IB.height));
        if (UP < 1.F)
            continue;
        CTexPassElement::SRenderData d;
        d.tex = TEX;
        d.box = CBox{IB.x, EDGE - UP, IB.width, IB.height};
        d.a   = 1.F;
        g_pHyprRenderer->m_renderPass.add(makeUnique<CTexPassElement>(d));
    }
}

void render(PHLMONITOR monitor) {
    // Y6/Y8. Retire what has run its course before anything asks whether the
    // dock still needs frames.
    std::erase_if(g_flight.bounces, [](const SBounce& b) { return elapsedMs(b.startedAt) >= HOP_MS; });
    std::erase_if(g_flight.launchZooms, [](const SLaunchZoom& z) { return elapsedMs(z.startedAt) >= LAUNCH_ZOOM_MS; });
    // V3. Same retirement, same place: the icon that flew home has arrived.
    if (g_dockDrag.ghostHome && elapsedMs(g_dockDrag.ghostHome->startedAt) >= GHOST_HOME_MS)
        g_dockDrag.ghostHome.reset();

    const auto LAYOUT = layout(monitor);
    if (LAYOUT.pill.empty()) {
        renderUrgentHop(monitor);
        return;
    }

    // A pointer on ANOTHER monitor is nowhere on this dock. Mapped into this
    // monitor's pixels it is merely off-screen today, but nothing guarantees an
    // arrangement where it cannot land on this pill, and every hover below --
    // panel, tooltip, highlight -- has to agree on which dock the pointer is at.
    const auto POINTER    = g_pInputManager->getMouseCoordsInternal();
    const auto MOUSELOCAL = monitor->logicalBox().containsPoint(POINTER) ? (POINTER - monitor->m_position) * monitor->m_scale : Vector2D{-1e6, -1e6};
    const auto& MOUSE      = MOUSELOCAL;
    const auto HOVEREDITEM = std::ranges::find_if(LAYOUT.items, [&](const SItem& item) { return hoverBox(item.iconBox, monitor->m_scale).containsPoint(MOUSE); });
    const bool HASHOVER    = HOVEREDITEM != LAYOUT.items.end();
    const bool HOVERAPPS   = !LAYOUT.showApps.empty() && hoverBox(LAYOUT.showApps, monitor->m_scale).containsPoint(MOUSE);

    // Hover previews BEFORE the pill. Live window thumbnails that leak downward
    // used to paint over the icons (flushPass composites them on top of the
    // already-queued pill). The panel still sits above the dock; the pill
    // covers anything that spills into its box.
    {
        const bool OVERPANEL   = panelContains(monitor, MOUSE);
        // The ICON drag hides the panel from its first press (it is pulling the
        // very icon the panel belongs to). A PREVIEW drag must not -- until it
        // goes live the gesture is still a click, and a panel that blinks away
        // under the press would make every preview click look broken.
        const bool ICONDRAG    = g_dockDrag.drag.has_value();
        const bool TEAROUT     = g_dockDrag.panel && g_dockDrag.panel->live;
        const bool PANELWANTED = (HASHOVER || OVERPANEL) && !g_menu && !ICONDRAG && !TEAROUT;
        CBox        panelAnchor;
        std::string panelClass;
        std::string panelDesktopId;
        std::string panelScratchpad;
        if (PANELWANTED) {
            const bool FROMICON = HASHOVER;
            panelAnchor         = FROMICON ? HOVEREDITEM->iconBox : g_hover.panel->anchor;
            panelClass          = FROMICON ? HOVEREDITEM->wmClass : g_hover.panel->wmClass;
            panelDesktopId      = FROMICON ? HOVEREDITEM->desktopId : g_hover.panel->desktopId;
            panelScratchpad     = FROMICON ? HOVEREDITEM->scratchpadName : g_hover.panel->scratchpadName;
        }
        // The panel belongs to one monitor. Hovering here while it is up on
        // another moves it here -- at once, since one panel cannot be leaving
        // there and arriving here -- and a monitor that neither wants it nor
        // owns it leaves it strictly alone.
        if (PANELWANTED && g_hover.panel && g_hover.panel->monitor != monitor->m_id) {
            g_hover.panel.reset();
            forgetPanelGeometry();
        }
        if (PANELWANTED || !g_hover.panel || g_hover.panel->monitor == monitor->m_id)
            renderHoverPreview(monitor, LAYOUT, panelAnchor, panelClass, panelDesktopId, panelScratchpad, Time::steadyNow());

    }

    // Y7. Drawn FIRST so the pill's own frost and background land on top of it
    // and the halo only shows where it reaches past the pill's edges.
    if (const float GLOW = dragGlow(monitor, LAYOUT, MOUSELOCAL); GLOW > 0.001F) {
        const auto ACCENT = ScrollOverview::Config::highlightColor();

        // A stack of expanded rounded rects with a falling alpha. There is no
        // soft-shadow primitive to hand, and a real blur for a halo would be a
        // third full-monitor kawase every frame of a drag; three rings read as a
        // glow and cost three rects.
        // ponytail: swap for a proper falloff if this ever looks banded.
        constexpr int RINGS = 3;
        for (int i = RINGS; i >= 1; --i) {
            const float SPREAD = DRAG_GLOW_PX * monitor->m_scale * sc<float>(i) / sc<float>(RINGS);

            CRectPassElement::SRectData ring;
            ring.box           = LAYOUT.pill.copy().expand(SPREAD);
            ring.color         = ACCENT.modifyA(GLOW * DRAG_GLOW_ALPHA / sc<float>(i * i));
            ring.round         = LAYOUT.rounding + sc<int>(std::round(SPREAD));
            ring.roundingPower = 2.F;
            g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(ring));
        }
    }

    // X13/X29/X86: dock pill. Live on the desktop (single surface). In overview
    // it samples the same decoration:blur:* behind-chrome buffer as the strip
    // so strength cannot drift with search-result count. Frost itself is
    // skipped when decoration:blur:enabled is off, matching Omarchy windows.
    frostedPlate(monitor, LAYOUT.pill, LAYOUT.rounding, 1.F);

    // Same outline the workspace cards use: theme accent, Hyprland border
    // weight, floored at 1 so a theme with border_size 0 still frames the pill.
    {
        const int  BORDERSIZE = std::max(1, ScrollOverview::Config::getValue<int>("general:border_size"));
        const auto ACCENT     = ScrollOverview::Config::highlightColor();

        CBorderPassElement::SBorderData border;
        border.box           = LAYOUT.pill.copy().round();
        border.grad1.reset(ACCENT.modifyA(1.F));
        border.a             = 1.F;
        border.borderSize    = BORDERSIZE;
        border.round         = LAYOUT.rounding;
        border.roundingPower = 2.F;
        g_pHyprRenderer->m_renderPass.add(makeUnique<CBorderPassElement>(border));
    }

    // (user, 2026-08-03): GNOME lights the hovered app cell with a translucent
    // lighter fill and a slightly stronger outline, and labels it. Mouse coords
    // are global logical; the layout is monitor-local RENDER px.
    if (HASHOVER || HOVERAPPS) {
        // (user, 2026-08-03) A flat fill, no outline. The radius is CONCENTRIC
        // with the pill rather than a constant: the cell is inset from the pill
        // by the same amount on every side, so its curve only reads as parallel
        // to the dock's if the radius drops by that inset too.
        // X56 (user, 2026-08-06): themed, so the dock re-colours with
        // everything else when the Hyprland theme changes. The alpha is the
        // tuned value and stays -- only the hue comes from the config.
        const auto ACCENT = ScrollOverview::Config::highlightColor();

        CRectPassElement::SRectData fill;
        fill.box           = hoverBox(HASHOVER ? HOVEREDITEM->iconBox : LAYOUT.showApps, monitor->m_scale);
        fill.color         = ACCENT.modifyA(0.22F);
        fill.round         = std::max(0, LAYOUT.rounding - sc<int>(std::round(hoverInset() * monitor->m_scale)));
        fill.roundingPower = 2.F;
        g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(fill));
    }

    // X32/G12. Where the dragged app would land. Drawn under the icons so a
    // caret at the very first slot is not sitting on top of one.
    if (const CBox CARET = caretBox(LAYOUT, caretIndex(), monitor->m_scale); !CARET.empty()) {
        CRectPassElement::SRectData caret;
        caret.box           = CARET;
        caret.color         = CHyprColor{1.F, 1.F, 1.F, 0.90F};
        caret.round         = sc<int>(std::round(CARET.width / 2.F));
        caret.roundingPower = 2.F;
        g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(caret));
    }

    // ONE walk of the window list for the whole icon row, and one read of each
    // config key and theme colour the row needs -- all four were being asked
    // per ITEM, per monitor, per frame (user, 2026-09-14).
    const auto       TALLY    = buildTally();
    const bool       SHOWDOT  = Config::getValue<bool>(KEY_SHOW_RUNNING);
    const int        DOTSIZE  = Config::getValue<int>(KEY_DOT_SIZE);
    const CHyprColor DOTCOLOR = ScrollOverview::Config::dockIndicatorColor();
    const bool       DANCE    = Config::getValue<bool>(KEY_DANCE_URGENT);
    const bool       BADGES   = badgesEnabled();

    for (const auto& ITEM : LAYOUT.items) {
        // Whitened for the SCRATCHPAD, for the same reason the user asked for it
        // on Show Applications (2026-08-06, below): the only icon that suits it
        // is symbolic -- near-black artwork on a dark frosted pill, beside a row
        // of full-colour app icons. Every other item here brings its own colour.
        if (const auto TEX = OverviewRender::appIcon(ITEM.icon, ITEM.scratchpad); TEX) {
            CTexPassElement::SRenderData data;
            data.tex = TEX;
            data.box = ITEM.iconBox;

            // Y8. The ICON leaves the pill, the running dot and the badge stay
            // where they are -- which is what macOS does and what keeps the hop
            // from reading as the whole cell coming loose.
            data.box.y -= bounceLift(ITEM.wmClass, sc<float>(ITEM.iconBox.height));

            // V11 V4. A stack has no window class, so Y8's hop cannot key on
            // one -- its acknowledgement and its refusal are keyed on the path
            // instead. Both are additive with the app hop above, which cannot
            // be running on the same cell.
            if (ITEM.stack) {
                data.box.y -= Stacks::bounceLift(ITEM.stackPath, sc<float>(ITEM.iconBox.height));
                data.box.x += Stacks::shakeOffset(ITEM.stackPath, sc<float>(ITEM.iconBox.width));
            }

            // X31 `dance-urgent-applications`. CBox::rot is radians about the
            // box's own centre, which is the reference's pivot (0.5, 0.5) --
            // so the wiggle is one assignment, not a transform stack.
            if (ITEM.running && ITEM.uri.empty() && DANCE && itemUrgent(ITEM, TALLY))
                data.box.rot = wiggleRadians();

            // The hover lean, the press, and the light on the icon's face, in
            // perspective -- DockTilt. The squish is keyed on the same subject
            // the tooltip uses, so a scratchpad slot and an app never share one.
            // Dock stacks A: a stack draws its own cell -- the pile of its newest
            // items, the folder, or a dimmed missing folder -- without the tilt.
            const auto& PRESSKEY = ITEM.subject;
            if (!(ITEM.stack && Stacks::drawCell(monitor, ITEM, data.box, std::clamp(ITEM.appear, 0.F, 1.F))))
                DockTilt::queue(TEX, data.box, std::clamp(ITEM.appear, 0.F, 1.F),
                            DockTilt::pose(monitor->m_id, PRESSKEY, pointerIn(ITEM.iconBox, MOUSE, monitor->m_scale), pressDepth(monitor->m_id, PRESSKEY),
                                           pressAt(monitor->m_id, PRESSKEY)));
        }

        // Both of these are deliberately OUTSIDE the texture check: an app with
        // no desktop entry still runs and can still have a count, and losing its
        // indicators as well as its icon would hide it twice over.
        //
        // X32 gates them on RUNNING: a pinned favourite with no window is the
        // one icon in the pill that must NOT get a dot, and that absence is the
        // only thing distinguishing it from the app next to it.
        if (ITEM.running) {
            if (SHOWDOT)
                renderRunningDot(monitor, LAYOUT, ITEM, TALLY, DOTSIZE, DOTCOLOR);

            renderBadge(monitor, ITEM, BADGES);
        }

        // X34. Over the badge and the dot, because it is transient and it is the
        // thing being read for as long as it is up. OUTSIDE the running gate:
        // a pinned app that is not running still has a key.
        renderNumberOverlay(monitor, ITEM, sc<int>(&ITEM - LAYOUT.items.data()));
    }

    // THE ARRANGEMENT's leavers: an app that closed shrinks and fades where it
    // stood while its neighbours close the gap over it. Only here -- they are
    // not in LAYOUT.items, so nothing can hover or click them on the way out.
    // Shifted by the same autohide slide the live items got.
    if (const auto ARR = g_arrange.find(monitor->m_id); ARR != g_arrange.end() && !ARR->second.exits.empty()) {
        const double DY = LAYOUT.pill.y - arranged(monitor->m_id, baseLayout(monitor)).pill.y;
        for (const auto& EXIT : ARR->second.exits) {
            const float T = std::clamp(ScrollOverview::Config::realElapsedMs(EXIT.since) / EXIT_MS, 0.F, 1.F);
            if (T >= 1.F)
                continue;
            const auto TEX = OverviewRender::appIcon(EXIT.item.icon, EXIT.item.scratchpad);
            if (!TEX)
                continue;
            const float U = 1.F - T;
            CTexPassElement::SRenderData data;
            data.tex = TEX;
            data.box = scaledAboutCentre(EXIT.item.iconBox, 0.2F + 0.8F * U * U).translate({0.0, DY}).round();
            data.a   = U * U;
            g_pHyprRenderer->m_renderPass.add(makeUnique<CTexPassElement>(data));
        }
    }

    // X32 Show Applications, drawn last of the icon row so its cell is complete
    // before the plates go over the top.
    if (!LAYOUT.showApps.empty()) {
        // User, 2026-08-06: "make the Show Applications icon white, it's too
        // dark". view-app-grid-symbolic is near-black artwork sitting on a dark
        // frosted pill; every other icon in the dock is a full-colour app icon,
        // so this is the only one that needed it.
        if (const auto TEX = OverviewRender::appIcon(Config::getValue<std::string>(KEY_SHOW_APPS_ICON), true); TEX) {
            DockTilt::queue(TEX, LAYOUT.showApps, 1.F,
                            DockTilt::pose(monitor->m_id, "show-apps", pointerIn(LAYOUT.showApps, MOUSE, monitor->m_scale), pressDepth(monitor->m_id, "show-apps"),
                                           pressAt(monitor->m_id, "show-apps")));
        }
    }

    // V3. THE ICON THE POINTER IS CARRYING, and the one flying home after a
    // miss. Both are the same clone of the same texture at the same cell size,
    // which is the point: the thing that lands back in the pill has to be the
    // thing that left it. Drawn over the whole icon row (it passes across it on
    // the way out) and under the launch blow-up (which is the answer to a drop
    // that DID land, and belongs on top of the one that did not).
    //
    // No frost: this is an icon clone, exactly as the launch zoom is, not a
    // plate. The dock's glass is the surface it comes off and goes back to.
    if (const auto TEX = ghostShowing() && g_dockDrag.drag->monitor == monitor->m_id ? OverviewRender::appIcon(g_dockDrag.drag->icon) : nullptr; TEX) {
        CTexPassElement::SRenderData data;
        data.tex = TEX;
        data.box = ghostBoxAt(monitor, g_dockDrag.drag->home, g_dockDrag.drag->grab, g_dockDrag.drag->at);
        data.a   = DRAG_GHOST_ALPHA;
        g_pHyprRenderer->m_renderPass.add(makeUnique<CTexPassElement>(data));
    }

    if (g_dockDrag.ghostHome && g_dockDrag.ghostHome->monitor == monitor->m_id) {
        const float E = houseEase(false, std::clamp(elapsedMs(g_dockDrag.ghostHome->startedAt) / GHOST_HOME_MS, 0.F, 1.F));

        if (const auto TEX = OverviewRender::appIcon(g_dockDrag.ghostHome->icon); TEX) {
            CTexPassElement::SRenderData data;
            data.tex = TEX;
            data.box = lerpBox(g_dockDrag.ghostHome->from, g_dockDrag.ghostHome->home, E).round();
            // Fades as it arrives, so the clone and the real icon underneath it
            // are never two solid icons in one cell for a frame.
            data.a = DRAG_GHOST_ALPHA * (1.F - E);
            g_pHyprRenderer->m_renderPass.add(makeUnique<CTexPassElement>(data));
        }
    }

    // Y6. GNOME's launch blow-up, drawn over the icon row so the growing clone
    // is not clipped by the neighbours it passes over.
    for (const auto& ZOOM : g_flight.launchZooms) {
        if (ZOOM.monitor != MONITOR_INVALID && ZOOM.monitor != monitor->m_id)
            continue;
        const auto TEX = OverviewRender::appIcon(ZOOM.icon);
        if (!TEX)
            continue;

        const float E     = easeOutQuad(std::clamp(elapsedMs(ZOOM.startedAt) / LAUNCH_ZOOM_MS, 0.F, 1.F));
        const float GROWN = 1.F + (LAUNCH_ZOOM_SCALE - 1.F) * E;
        const float W     = sc<float>(ZOOM.from.width) * GROWN;
        const float H     = sc<float>(ZOOM.from.height) * GROWN;

        // pivot_point (0.5, 0.5): it grows about its own centre.
        float x = sc<float>(ZOOM.from.x) - (W - sc<float>(ZOOM.from.width)) / 2.F;
        float y = sc<float>(ZOOM.from.y) - (H - sc<float>(ZOOM.from.height)) / 2.F;

        // Z11 (user): GNOME's blow-up drifts UPWARD off a bottom dock. Its
        // clamp is baked into the tween's END STATE and EASED from frame one
        // (`translation_y: containedY - scaledY` inside the same ease) --
        // per-frame clamping only engages once the box already hits the edge,
        // which read as "expands in all directions".
        const float MONW = sc<float>(monitor->m_size.x) * monitor->m_scale;
        const float MONH = sc<float>(monitor->m_size.y) * monitor->m_scale;
        const float FW   = sc<float>(ZOOM.from.width) * LAUNCH_ZOOM_SCALE;
        const float FH   = sc<float>(ZOOM.from.height) * LAUNCH_ZOOM_SCALE;
        const float FX   = sc<float>(ZOOM.from.x) - (FW - sc<float>(ZOOM.from.width)) / 2.F;
        const float FY   = sc<float>(ZOOM.from.y) - (FH - sc<float>(ZOOM.from.height)) / 2.F;
        x += (std::clamp(FX, 0.F, std::max(0.F, MONW - FW)) - FX) * E;
        y += (std::clamp(FY, 0.F, std::max(0.F, MONH - FH)) - FY) * E;

        CTexPassElement::SRenderData data;
        data.tex = TEX;
        data.box = CBox{std::round(x), std::round(y), std::round(W), std::round(H)};
        data.a   = 1.F - E;
        g_pHyprRenderer->m_renderPass.add(makeUnique<CTexPassElement>(data));
    }

    // Above the pill by construction (MENU_GAP), so the previews below cannot
    // overdraw it and it does not have to wait for them.
    // V11 V4. Over the icon row, under the menu: the menu is modal and the
    // popup is not, so a menu opened over an open fan must be on top of it.
    // It reads its own clock, like every other easing surface here: the popup
    // runs off wall time rather than off the frame stamp, so there is nothing
    // for a caller to hand it.
    Stacks::render(monitor, LAYOUT);

    renderMenu(monitor, LAYOUT);

    const auto NOW = Time::steadyNow();

    // Y5. The flight is a real window at real SIZE, not a thumbnail, so where it
    // is legal it draws the way the overview draws a tile -- live blur, frost
    // scaled to the render scale, real decorations.
    //
    // It takes that path EVERYWHERE, desktop included -- which is the point of
    // the user's instruction. What differs off the overview is only PASS
    // OWNERSHIP: inside the overview the plugin owns the render pass and the
    // path may flush it mid-draw, while at RENDER_POST_WINDOWS a flush renders
    // and CLEARS everything Hyprland queued for the rest of the frame. Measured:
    // flushing there wedged the frame loop and stranded a restoring window on
    // the stash permanently. Blur scaling and forced live blur do not depend on
    // the flush, so the frost is correct in both.
    // The flight draws through drawFlightSurface (below, named so
    // FlightController.cpp can call it); this is the pass-ownership fact it
    // needs, computed once per frame here where the render context is.
    const bool OWNSPASS = scrollOverviewForMonitor(monitor) != nullptr;

    // Hover previews were drawn under the pill, above.

    // The menu says the app's name louder than the tooltip does, and the two
    // plates would otherwise sit on top of each other.
    //
    // A preview under the pointer names ITS OWN window -- the full title, not
    // the app -- one gap above the panel. The icon still names the APP: with
    // the panel up that plate sits above the previews, centred on the icon,
    // so the wall of thumbnails does not bury "what is this". Empty-name is
    // the fade-OUT: without it the plate would pop off instead of leaving.
    const auto CELL = panelCellAt(monitor, MOUSE);

    if (g_menu)
        renderTooltip(monitor, LAYOUT, CBox{}, "");
    else if (CELL)
        renderTooltip(monitor, LAYOUT, CELL->first, CELL->second->m_title, sc<float>(g_hover.drawn.y), tooltipSubject(CELL->second),
                      peekWindowsOn(monitor, {CELL->second}));
    else if (HASHOVER) {
        const float ABOVE = (panelOn(monitor) && g_hover.drawn.height >= 1) ? sc<float>(g_hover.drawn.y) : 0.F;
        // Dock stacks A: a stack's tooltip says when its folder is gone (and,
        // later, a trash error) after its name.
        std::string tipName = itemName(*HOVEREDITEM);
        if (HOVEREDITEM->stack)
            if (const auto SUFFIX = Stacks::tooltipSuffix(HOVEREDITEM->stackPath); !SUFFIX.empty())
                tipName += "  \u00b7  " + SUFFIX;
        renderTooltip(monitor, LAYOUT, HOVEREDITEM->iconBox, tipName, ABOVE, HOVEREDITEM->subject,
                      HOVEREDITEM->uri.empty() && !HOVEREDITEM->scratchpad ? peekWindowsOn(monitor, itemWindows(*HOVEREDITEM)) : std::vector<PHLWINDOW>{});
    } else if (HOVERAPPS)
        renderTooltip(monitor, LAYOUT, LAYOUT.showApps, "Show Applications", 0.F, "show-apps");
    else
        renderTooltip(monitor, LAYOUT, CBox{}, "");

    // X46/Y5/Y4/Z16. The flight loop lives in FlightController.cpp now -- WHOLE,
    // drawing and all. It advances every flight one frame and paints it in this
    // pass, one flight at a time, in the order it always did.
    //
    // It is deliberately not a tick() that returns a draw list. That version was
    // written, compiled, and passed z4/z4c/z6/z7 before z5 caught it:
    // mutate-every-flight-then-draw-every-flight moved when the dock damages
    // itself relative to when its flights advance, and three reversals in a row
    // then fired against flights that had never drawn a frame. The extraction is
    // the state machine; the loop is a loop.
    Flight::renderFlights(monitor, LAYOUT, NOW, OWNSPASS);

    // ASKED FOR HERE, after the flight loop has advanced -- see the note on
    // needsFrames for why this site cannot be folded into the other one.
    // The ponytail note that used to sit here -- "whole-monitor damage at the
    // refresh rate, the pill's rectangle would do" -- is answered by
    // dockDamageRegion, which is that conversion.
    if (needsFrames(monitor, LAYOUT, TALLY))
        pumpFrame(monitor, dockDamageRegion(monitor, LAYOUT));
}

// Y5. The flight is a real window at real SIZE, not a thumbnail, so where it is
// legal it draws the way the overview draws a tile -- live blur, frost scaled to
// the render scale, real decorations. It takes that path EVERYWHERE, desktop
// included, which is the point of the user's instruction. What differs off the
// overview is only PASS OWNERSHIP: inside the overview the plugin owns the
// render pass and the path may flush it mid-draw, while at RENDER_POST_WINDOWS a
// flush renders and CLEARS everything Hyprland queued for the rest of the frame.
// Measured: flushing there wedged the frame loop and stranded a restoring window
// on the stash permanently. Blur scaling and forced live blur do not depend on
// the flush, so the frost is correct in both.
//
// This was a lambda inside render(). It is a named function because the flight
// LOOP moved to FlightController.cpp and the drawing did not: the dock owns the
// render context, so the flight module hands it back the window, the box, the
// alpha and this frame's `now`, and the pixels are produced by exactly the code
// that produced them before.
void drawFlightSurface(PHLMONITOR monitor, const PHLWINDOW& window, const CBox& box, float alpha, const Time::steady_tp& now, bool ownsPass) {
    if (!window)
        return;

    // Z9r5: a window hidden for its flight is skipped by the surface machinery
    // (validMapped), so the proxy rendered as border + tint with no content --
    // measured, frame 130 of the zen film. Unhide strictly around this draw: the
    // compositor's own window pass ran before the dock's chrome pass this frame,
    // so it never observes the flip.
    const bool REHIDE = window->isHidden();
    if (REHIDE) {
        window->setHidden(false);
        Membership::bump("flight draw unhide");
    }
    renderLivePreview(monitor, window, box, now, alpha, true, ownsPass);
    if (REHIDE) {
        window->setHidden(true);
        Membership::bump("flight draw rehide");
    }
}

}

// The runnable check for X27 is scripts/x27_gate.py: it measures the pill that
// actually landed on screen against the reference numbers. A pure-math assert
// was considered and dropped -- it would have had to link the whole plugin, and
// the proportions are exactly the thing a pixel measurement proves better.
//
// X25's is scripts/x25_gate.py, and it asks the one question a still frame
// cannot: two frames of the same minimized window, and the preview must have
// CHANGED between them.
