#pragma once

// Internal to the dock: the parts of what was one Dock.cpp share these. Not an
// API -- include it only from src/dock/Dock*.cpp parts. The include block is
// the original file's, unchanged, and must stay first in every part.
// NO variable is defined here: they all live in DockState.cpp, in their
// original order, and are declared extern below.

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

#include "PreviewGrid.hpp"
#include "BorderFade.hpp"
#include <cairo/cairo.h>
#include <hyprland/src/render/pass/TexPassElement.hpp>
#include <hyprland/src/render/gl/GLTexture.hpp>
#include "Dock.hpp"
#include "FramePump.hpp"
#include "Mosaic.hpp"
#include "DesktopSwitch.hpp"
#include "Motion.hpp"
#include "MotionMath.hpp"
#include "MotionTween.hpp"
#include "MotionCurves.hpp"
#include "MotionTimings.hpp"
#include "MotionRetarget.hpp"
#include "ConfigureHold.hpp"

#include "Compat.hpp"

#include "AppSearch.hpp"
#include "globals.hpp"
#include "Config.hpp"
#include "DropDecision.hpp" // V3: where a released icon drag goes, as a pure function
#include "IOverview.hpp"
#include <hyprland/src/pointer/PointerController.hpp>
#include "LauncherEntry.hpp"
#include "StackReader.hpp"
#include "TextInput.hpp"
#include "BuiltinIcons.hpp"
#include "IconOverrides.hpp"
#include "DockDrop.hpp"
#include "DockMenuModel.hpp"
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
#include <hyprland/src/config/shared/actions/ConfigActions.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/protocols/PointerConstraints.hpp>
#include <hyprland/src/helpers/MiscFunctions.hpp>
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
// rules as OverviewChrome above; Overview.hpp is already in this TU.
#include "GridLayout.hpp"

// The dock's FLIGHT subsystem -- the whole Z4-Z20 saga -- is its own module now:
// an explicit state machine with named phases instead of five sentinel fields.
// Included AFTER the private-access blocks, like OverviewChrome, because the
// flight code needs the same m_Value / m_realPosition access this TU has.
#include "FlightController.hpp"
#include "FlightBlur.hpp"
#include "Scratchpad.hpp"
#include "AppInfo.hpp"
#include <cstring>
#include <hyprutils/os/Process.hpp>
#include <unistd.h>
#include <csignal>
#include "Placement.hpp"

namespace Hyprdrive::Dock {
namespace DockDetail {}
using namespace DockDetail;
namespace DockDetail {



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
constexpr float  MENU_EDGE_MARGIN = 8.F; // logical px the menu keeps from every screen edge
constexpr float  MENU_GAP       = 8.F;
constexpr float  MENU_SEP_H     = 9.F;
constexpr float  MENU_MIN_W     = 170.F;
constexpr float  MENU_HOVER_MS  = 140.F; // a row easing into (and out of) its hover
constexpr size_t MENU_LABEL_MAX = 44; // bytes; window titles are arbitrarily long

// OVERVIEW_CPP_SPEC 13.12 X47. The hover preview panel. Windows' taskbar for
// placement and trigger, this rice for looks: the same frost, the same plate and
// the same house curves the X28 menu already uses, because it is the same class
// of floating chrome one gap above the pill.
constexpr float  PANEL_GAP      = 8.F;   // above the pill, matching MENU_GAP
constexpr float  PANEL_PAD      = 10.F;
constexpr float  PANEL_CELL_H   = 132.F; // logical px, the preview row's height
constexpr float  PANEL_CELL_GAP = 10.F;
constexpr float  PANEL_RADIUS   = 12.F; // the hover plate's floor (CONCENTRIC CORNERS)
constexpr float  PANEL_RADIUS_MAX = 24.F;
constexpr float  PANEL_IN_MS    = 170.F;
constexpr float  PANEL_OUT_MS   = 130.F; // exits ~25% quicker, house rule
constexpr float  PANEL_SLIDE_MS = 160.F; // travel from one icon to the next
// A short hold before the dismissal starts, so crossing the gap between two
// icons does not flicker the panel away and back.
constexpr float  PANEL_GRACE_MS = 180.F;
// ponytail: past this the cells are too narrow to read anyway. Scroll or a
// second row if an app with a dozen windows ever needs them all.
constexpr size_t PANEL_MAX      = 64; // a guard on draw cost only: past half the screen the previews wrap into rows (PreviewGrid)
constexpr float  PANEL_ROW_SHARE = 0.5F; // a row of previews wraps past this share of the monitor's width
constexpr float  PANEL_MAX_H_SHARE = 0.6F; // rows taller than this share of the monitor's height shrink their cells

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
// DockFullscreen.cpp: the dock over a fullscreen window (owner, 2026-11-12).
constexpr const char* KEY_SHOW_OVER_FS  = "plugin:hyprdrive:dock:show_over_fullscreen";
constexpr const char* KEY_FS_GAMES_ONLY = "plugin:hyprdrive:dock:fullscreen_hide_games_only";
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
// FOLDERS ANYWHERE (user, 2026-11-05): off, the folders are one group after
// the apps; on, a folder can be dragged anywhere among them -- stored as an
// IconOverrides::folderId entry in the favourites list, so the apps' own
// reorder (pinAt) moves both. Drives, the Trash and Show Overview stay last.
constexpr const char* KEY_FOLDERS_ANYWHERE = "plugin:hyprdrive:dock:folders_anywhere";
extern bool foldersAnywhere();
extern const Stacks::SStack* placedFolder(const std::string& id);
extern bool drawnFavorite(const std::string& id);
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
constexpr const char* KEY_ISOLATE_OV    = "plugin:hyprdrive:dock:isolate_in_overview";
constexpr const char* KEY_MONITOR       = "plugin:hyprdrive:dock:monitor";
constexpr const char* KEY_ISOLATE_LOCATIONS = "plugin:hyprdrive:dock:isolate_locations";
constexpr const char* KEY_BUILTIN_ICONS     = "plugin:hyprdrive:dock:builtin_icons";
constexpr const char* KEY_TRASH_ICON        = "plugin:hyprdrive:dock:trash_icon";
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
extern Time::steady_tp g_overlayUntil;
extern bool overlayHoldsDock();

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
extern SDockFlight g_flight;
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
constexpr float BOUNCE_HEIGHT   = Hyprdrive::Motion::HOP_HEIGHT;   // of one icon height (MotionTimings.hpp)
constexpr float BOUNCE_UP_SHARE = Hyprdrive::Motion::HOP_UP_SHARE; // the hop is quicker up than down

struct SMetrics {
    float icon   = 0.F;
    float width  = 0.F;
    float height = 0.F;
};
extern SMetrics metrics(size_t slots, float units, float iconMax, float maxWidth);
extern std::filesystem::path favoritesPath();

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
extern SFavoritesStore& favoritesStore();
extern std::vector<std::string>& favorites();
extern void writeFavorites();

struct SEmptyDesktopStore {
    std::vector<std::string>        ids;
    std::filesystem::file_time_type stamp{};
    bool                            present = false;
};
extern bool opensInEmptyDesktop(const std::string& id);
extern void toggleEmptyDesktop(const std::string& id);
extern bool opensInFullscreen(const std::string& id);
extern void toggleOpenFullscreen(const std::string& id);
// HIDE DOCK IN FULLSCREEN, per app (owner, 2026-11-12: "an option under dock
// context menu in get info for specific apps to not show dock in fullscreen"):
// `hide-dock-fullscreen-apps`, beside the other two lists.
extern bool hidesDockInFullscreen(const std::string& id);
extern void toggleHideDockInFullscreen(const std::string& id);
// DockFullscreen.cpp: the monitor's desktop shows a fullscreen window the dock
// keeps away from (the two settings and the app's Get Info flag). Read by
// updateAutohide every call; what it reads was settled at the last event.
extern bool fullscreenKeepsDockAway(PHLMONITOR monitor);
extern void toggleFavorite(const std::string& id);

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
extern SInterestCfg interestCfg();
extern bool interesting(const PHLWINDOW& window, const SInterestCfg& cfg);

// Y2/press/tilt. Defined with the tooltip much further down; declared here
// because the item builder stamps every item with its subject as it goes.
std::string tooltipSubject(const SItem& item);
extern uint64_t g_itemsGeneration;
extern std::unordered_map<std::string, Time::steady_tp> g_pendingLaunches;
constexpr float                                  LAUNCH_PENDING_MS   = 30000.F;
constexpr float                                  LOCATION_PENDING_MS = 1500.F;
extern std::string pendingLaunchKey(const SItem& item);

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
    uint64_t  dropGap    = 0; // DockDrop's placeholder (the Moses gap)
    uint64_t  themes     = 0; // icon themes + the Trash's look
    uint64_t  inventory  = 0;
    uint64_t  icons      = 0;
    uint64_t  overrides  = 0; // the settings panel's per-app icons
    long long activeWs   = 0;
    uint32_t  config     = 0;

    bool operator==(const SItemsKey&) const = default;
};
extern const std::vector<SItem>& dockItems();
extern uint64_t itemsGeneration();
extern float previewAspect(const PHLWINDOW& window);
extern CHyprColor background();
extern void forcePreviewVisible(const PHLWINDOW& window);
extern void presentPreview(const PHLWINDOW& window, PHLMONITOR monitor, const Time::steady_tp& now);

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
extern std::unordered_map<MONITORID, SDockVisualState> g_visualStates;

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
extern std::unordered_map<MONITORID, SReveal> g_reveal;

// The barrier is the monitor's last row, and the cursor is clamped there -- but
// a clamp can leave it a pixel short, so the band is not exactly one row.
constexpr float EDGE_BAND = 2.F;

// The band that opens the dock on POSITION alone, deliberately a little wider
// than EDGE_BAND -- "on or near the bottom edge".
constexpr float REVEAL_BAND = 4.F;
extern float elapsedMs(const Time::steady_tp& since);
extern bool classUrgent(const std::string& wmClass);

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
extern STally buildTally();
extern size_t itemWindowCount(const SItem& item, const STally& tally);
extern bool itemUrgent(const SItem& item, const STally& tally);
extern long long itemBadge(const SItem& item, bool badgesOn);
extern bool badgesEnabled();
extern bool anyUrgent(const std::vector<SItem>& items, const STally& tally);
extern const Time::steady_tp& urgentEpoch();
extern float wiggleRadians();

// THE HOP (user, 2026-09-24: "we shouldn't have the dock staying revealed to get
// your attention - instead the shaking item should also jump a little when the
// dock is out of view (little macos inspiration)"). While the dock is hidden,
// an urgent app's icon pops up from the bottom edge -- two bounces, the second
// smaller, like the macOS dock's -- once per wiggle period, on the wiggle's
// clock. The dock itself stays where autohide put it.
constexpr float URGENT_HOP_MS   = 900.F; // both bounces
constexpr float URGENT_HOP_LIFT = 0.62F; // of the icon's height, at the top of the first bounce
extern float urgentHopLift();
extern float msToNextHop();
extern wl_event_source* g_hopTimer;
extern void armHopTimer(float ms);

const SLayout& baseLayout(PHLMONITOR monitor); // defined below; slidPill needs the resting box
extern bool autohideAnimating(PHLMONITOR monitor);

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
extern std::unordered_map<MONITORID, SBaseLayoutCache> g_baseLayouts;

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
// B6: a turned glide carries on the old way at most this far (and a tenth of its leg).
constexpr double ARRANGE_CARRY_PX = 16.0;
// ...and re-arrangements this close (animated ms) after the last are one event.
constexpr float ARRANGE_BURST_MS = 40.F;

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
    // B6: re-arranged while gliding, a box leaves at the speed it had
    // (MotionRetarget.hpp): a short correction per box, absent for a glide
    // that started from rest -- that one is the plain lerp, to the bit.
    Motion::SBoxKick                                  pillKick{}, showAppsKick{};
    std::unordered_map<std::string, Motion::SBoxKick> kicks; // by subject
};
extern std::unordered_map<MONITORID, SArrange> g_arrange;
extern CBox scaledAboutCentre(const CBox& box, float s);
extern SLayout arranged(MONITORID id, const SLayout& resting);
extern bool arrangeAnimating(MONITORID id);
extern const SLayout& baseLayout(PHLMONITOR monitor);

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
extern void updateAutohide(PHLMONITOR monitor, const STally& tally);
extern CBox hoverBox(const CBox& iconBox, float scale);
extern void queueDockFrost(PHLMONITOR monitor, const CBox& box, int rounding, float alpha, const CBox& clip, const CRegion* shape, bool currentPass, float roundingPower);

std::vector<PHLWINDOW> itemWindows(const SItem& item);

// GNOME's dash label does not appear, it FADES: `_showLabel` eases opacity over
// DASH_ITEM_LABEL_SHOW_TIME. Ours arrived at full alpha, which is what the user
// reported missing. House entrance curve, because this is an entrance.
// The numbers are the shared label fade (MotionTimings.hpp).
constexpr float TOOLTIP_FADE_MS     = Hyprdrive::Motion::LABEL_FADE_IN_MS;
constexpr float TOOLTIP_FADE_OUT_MS = Hyprdrive::Motion::LABEL_FADE_OUT_MS; // exits are ~25% quicker, house rule

// The plate outlives the hover so it can fade AWAY; the caller passes an empty
// name when nothing is hovered and this keeps drawing the last one until it has
// gone. Anchor is remembered for the same reason -- the icon it belonged to is
// no longer under the pointer.
struct STooltip {
    std::string     name;
    std::string     subject; // icon or window, not the text; titles can tick without this moving
    CBox            anchor;
    float           bottomY = 0.F; // Y2; 0 = hang off the pill
    // The plate's alpha: up to 1 on the entrance curve, down to 0 on the exit
    // curve, each leg from the alpha AS DRAWN when it turned (inventory D5: a
    // hover that came back mid-fade used to pop to 0, a leave mid-entrance to 1).
    // A new subject still has its own entrance, from 0.
    Hyprdrive::Motion::STween<Time::steady_tp> fade{.from = 0.F, .to = 1.F, .start = Time::steadyNow()};
    bool out() const {
        return fade.to < 0.5F;
    }
    // The monitor it is on. ONE tooltip exists and every monitor's render()
    // calls renderTooltip: the monitor without the pointer asked for a fade-out
    // and then drew the plate at the other monitor's pixel coordinates -- the
    // label on the wrong screen (user, 2026-09-14). Only this monitor touches it.
    MONITORID       monitor = MONITOR_INVALID;
    // The real windows the hover is about, labelled inside themselves (see
    // renderWindowPeek). Kept with the plate so they fade out with it.
    std::vector<PHLWINDOWREF> peek;
};
extern STooltip g_tip;

// "App · Title" INSIDE THE WINDOW (user, 2026-09-24: "a little tooltip inside
// the window when I'm hovering the dock ... that way I know which terminal
// window I'm chatting with ... centered at the top inside the app window. This
// is not meant for overview"). Hovering an icon labels every one of the app's
// windows on screen; hovering a preview labels that one window. Same pill as
// the dock's own tooltip, on the same fade, so the two read as one gesture.
constexpr float PEEK_TOP_GAPL  = 10.F;
constexpr size_t PEEK_TITLE_MAX = 72;
extern std::string tooltipSubject(const PHLWINDOW& window);
extern std::string tooltipSubject(const SItem& item);
extern std::vector<PHLWINDOW> locationWindows(const SItem& item);
extern std::vector<PHLWINDOW> itemWindows(const SItem& item);
extern PHLWINDOW recentWindow(const std::vector<PHLWINDOW>& windows);
extern bool g_dockSeatHeld;
extern void stealDockSeat();
extern void restoreDockSeat();

// THE APP YOU ARE IN (user, 2026-11-09: clicking the icon of the app in use
// switched back to it instead of minimizing). With focus following the mouse
// the pointer hands the focus to every window it crosses on its way down to
// the dock, so by the click the focus was on one of those, and the click read
// as "take me back to this app". A focus that has not lasted PASSING is a
// pass-over: the one before it, which did last, is the app you were in.
// ONLY A HOVER PASSES OVER (2026-11-09): a focus by a key, Alt+Tab, a click or
// the dock is where the user went, settled the moment it lands. Read as a
// pass-over, clicking that app's icon within PASSING of it switched "back" to
// the window already focused -- nothing happened, instead of a minimize.
    constexpr auto                        PASSING = std::chrono::milliseconds(700);
extern PHLWINDOWREF g_focusNow, g_focusSettled;
extern std::chrono::steady_clock::time_point g_focusSince;
extern bool g_focusMeant;
extern void activateWindow(const PHLWINDOW& window);

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
extern std::optional<SCycle> g_cycle;
extern void cycleOrActivate(const SItem& item, bool reversed);
extern void moveToEmptyWorkspace(PHLMONITOR monitor, const PHLWINDOW& WINDOW);

std::string itemDesktopId(const SItem& item);
extern void activateItem(PHLMONITOR monitor, const SItem& item, bool shift, bool ctrl, bool emptyWorkspace);
extern void applyClickAction(PHLMONITOR monitor, const SItem& item, const std::string& action, bool shift);

// ---------------------------------------------------------------- X28 menu --

enum class ERow : uint8_t {
    Separator,
    AlwaysEmptyWorkspace,
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
    OpenEmptyWorkspace,
    MoveEmptyWorkspace,
    // user, 2026-10-13: the same two, onto the scratchpad.
    OpenScratchpad,
    MoveScratchpad,
    // user, 2026-10-15: "move to this workspace (only visible when window is
    // not in current workspace)".
    MoveHere,
    MinimizeAll,
    RestoreAll,
    ForceQuit,
    // Get Info's own view (user, 2026-09-24). The menu swaps to these in place.
    InfoLine,   // "Version  1.3.1": click copies the value (kept whole in `exec`)
    InfoReveal, // Show in Files
    InfoIcon,   // App Icon: the settings panel's Icons section, on this app (exec = desktop id)
    InfoHideDockFs, // Hide Dock in Fullscreen: a check, flips in place (exec = desktop id; owner, 2026-11-12)
    InfoCopyAll,
    InfoBack,
    // The overview, filtered on the app (user, 2026-09-24).
    ShowWindows,
    // Dock stacks A: the stack's own menu.
    StackRecent,      // exec = the file's path
    StackSortKey,     // exec = sort name
    StackDisplayKey,  // exec = "stack" | "folder"
    StackViewKey,     // exec = "auto" | "fan" | "grid" | "files"
    StackShowParent,
    StackRename,
    StackRenameField,
    StackTrashNewest,
    StackEmptyAsk,
    StackEmptyConfirm,
    StackEmptyCancel,
    EmptyTrashConfirm,
    // user, 2026-11-11: "add a maximize and fullscreen option and always open
    // in fullscreen in the context menu ... intuitive and space saving". Two
    // SEGMENTED rows (SRow::segmented): the window's Maximize | Fullscreen, and
    // "Always Open in" with its Empty Workspace | Fullscreen chips.
    WindowSize,       // the split row; its segments are Maximize and Fullscreen
    Maximize,         // segment: toggles FSMODE_MAXIMIZED on the row's window
    Fullscreen,       // segment: toggles FSMODE_FULLSCREEN on the row's window
    AlwaysOpenIn,     // the chips row; its chips are AlwaysEmptyWorkspace and AlwaysFullscreen
    AlwaysFullscreen, // chip: the app's main windows open fullscreen
    // user, 2026-11-11: "add a close glyph next to the 'move to scratchpad'
    // button", and the window menu's Close row: asks THE row's window to close
    // (sendClose, as the overview's close-X), never a kill.
    CloseWindow,
};
namespace MenuGlyph {

// OMARCHY-STYLE ROWS: a Nerd Font glyph column, the label, and a dim hint on the
// right -- the way the Omarchy menu lays its own rows out. The glyphs are all
// from the Font Awesome block, which every Nerd Font release carries.
    inline constexpr const char* NEW      = "\uf067"; // plus
    inline constexpr const char* ACTION   = "\uf054"; // chevron-right
    inline constexpr const char* WINDOW   = "\uf2d0"; // window-maximize
    inline constexpr const char* MINIMIZE = "\uf2d1"; // window-minimize
    inline constexpr const char* RESTORE  = "\uf2d2"; // window-restore
    inline constexpr const char* DETAILS  = "\uf05a"; // info-circle
    inline constexpr const char* FAV_ADD  = "\uf005"; // star
    inline constexpr const char* FAV_DEL  = "\uf006"; // star-o
    inline constexpr const char* QUIT     = "\uf00d"; // times
    inline constexpr const char* CLOSE    = "\uf00d"; // times: one window's close
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
    inline constexpr const char* IMAGE    = "\uf03e"; // image
    inline constexpr const char* STASH    = "\uf187"; // archive: the scratchpad's box
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
    inline constexpr const char* MOVE     = "\uf047"; // arrows (move)
    inline constexpr const char* HERE     = "\uf0ab"; // arrow-circle-down: onto this desktop
    inline constexpr const char* UP       = "\uf077"; // chevron-up
    inline constexpr const char* DOWN     = "\uf078"; // chevron-down
    inline constexpr const char* EXPAND   = "\uf065"; // expand: fullscreen
    inline constexpr const char* COMPRESS = "\uf066"; // compress: exit fullscreen
}

struct SRow {
    ERow         kind = ERow::Separator;
    std::string  label;
    std::string  exec;   // ERow::Action
    PHLWINDOWREF window; // ERow::Window, ERow::MoveEmptyWorkspace
    CBox         box;    // monitor-local RENDER px, filled by renderMenu
    std::string  glyph;          // Nerd Font glyph for the leading column; empty = none
    std::string  hint;           // dim, right-aligned: a window's desktop, say
    bool         danger = false; // Force Quit: drawn in the warning colour
    bool         sub    = false; // belongs to the row above: indented one glyph column
    bool         disabled = false; // greyed out and not clickable (Empty Trash on an empty Trash)
    // A window row's own moves, as small buttons at its right end (user,
    // 2026-11-03: an app with seven windows ran the menu off the screen at
    // four rows a window). Each acts as the row it replaced would have.
    struct SInline {
        ERow        kind = ERow::Separator;
        std::string glyph;
        std::string label; // shown in the hint's place while the button is hovered
        CBox        box;   // filled by renderMenu
        bool        on = false; // a segmented row's segment: lit (maximized, fullscreen, the chip set)
    };
    std::vector<SInline> inlines;
    // SEGMENTED (ERow::WindowSize, ERow::AlwaysOpenIn): `inlines` are wide
    // segments drawn with their glyph and label. Without a label of its own the
    // row is split into equal halves; with one, the segments are chips at its
    // right end. A segment acts as its kind; the row itself does nothing.
    bool segmented = false;
    // Minimize / Show for `window` alone (the window menu) rather than the
    // whole item's: the same rows, scoped. A scoped row whose window has gone
    // does nothing -- never the whole app.
    bool oneWindow = false;
};

// THE PLATE'S MOTION, one implementation for the dock's hover pane (SPanel)
// and the app menu's row preview (SMenu::SRowPeek; owner, 2026-11-11: "have
// them transition and fade away the same way they transform/transition when
// im moving my cursor over to the next app on the dock"). It grows out of the
// edge it belongs to (the pane: the dock's top; the row preview: the menu's
// side), decelerating and fading in; given another target it glides there
// from where it is drawn (PANEL_SLIDE_MS, velocity-matched: B6) while the
// contents swap at once and ride the box; with nothing wanted it waits
// PANEL_GRACE_MS (crossing a gap) and then shrinks back to its edge,
// accelerating and fading out. DockPreview.cpp: plateEnter / plateShow /
// plateAway / plateBoxAt / plateAlpha.
struct SPlateTravel {
    CBox            target;                          // where it settles
    CBox            from;                            // where the current move started
    Time::steady_tp moveSince = Time::steadyNow();
    float           moveMs    = PANEL_IN_MS;
    bool            decel     = true;                // entrances decelerate, exits do not
    // B6: moved on while still moving, the plate leaves at the speed it had
    // (MotionRetarget.hpp); none for a move that started from rest.
    Hyprdrive::Motion::SBoxKick kick{};
    bool            out       = false;
    bool            leaving   = false;               // inside the grace period
    Time::steady_tp leftAt    = Time::steadyNow();
    // The plate's fade, on its own clock (user, 2026-10-16: the preview
    // panel's padding "glitching on overview reveal/close"). It used to ride
    // the move clock above, and every retarget restarts that clock: the dock
    // moving under an overview reveal or close retargets the panel every
    // frame, so the plate sat at alpha 0 -- previews with no plate under them.
    // In on the entrance, out on the dismissal, from wherever it was; a slide
    // leaves it alone.
    Hyprdrive::Motion::STween<Time::steady_tp> fade{.from = 0.F, .to = 1.F, .start = Time::steadyNow()};
    float                                      fadeMs = PANEL_IN_MS;
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
    // The window list scrolls when the menu would not fit above the dock: the
    // first window row shown (renderMenu clamps it).
    int scrollTop = 0;
    // THE HOVER EASES (user, 2026-11-03: "a little hover effect on menu items
    // (grow, or something fancy)"). Which row -- and which of its buttons, or
    // -1 for the row itself -- the pointer is on, and the eases: `lit` per
    // (row, button) target -- the row's own highlight or one button's chip --
    // and `lift` per row (it stays lifted while the pointer is on its
    // buttons). Each turns from the level it has (inventory D8: the old
    // now/prev pair on one clock restarted both). renderMenu reads the clock;
    // needsFramesReason keeps frames coming while either runs.
    int                                         hoverRow = -1, hoverInl = -1;
    Hyprdrive::Motion::SLitSet<Time::steady_tp> lit, lift;
    // (row, button) -> one SLitSet key; button -1 is the row itself.
    static long litKey(int row, int inl) {
        return row < 0 ? Hyprdrive::Motion::SLitSet<Time::steady_tp>::NONE : (static_cast<long>(row) << 16) + (inl + 1);
    }
    // When it opened: the plate grows out of its icon from here (menuOpenP).
    Time::steady_tp openedAt{};

    // THE WINDOW'S OWN MENU (user, 2026-11-11), opened by a right-click on a
    // preview in the hover pane: the same plate, rows and motion, its rows for
    // `scope` alone (buildWindowRows). It hangs off the preview (`anchor` is
    // the cell) above the pane, which stays up under it; `bottom` is where its
    // plate's bottom edge sat last frame, kept for a frame with no pane.
    bool         windowScope = false;
    PHLWINDOWREF scope;
    double       bottom = -1.0;
    // A window of a row closed: the rows are rebuilt at the next draw.
    bool stale = false;
    // A row's close button was clicked: the row buttons are inert until the
    // pointer moves off `guardAt` or the hold runs out (MenuModel::buttonsArmed;
    // menuButtonsArmed clears it).
    bool            guarded = false;
    Time::steady_tp guardSince{};
    Vector2D        guardAt;

    // THE ROW'S PREVIEW (user, 2026-11-11: "if I hover over a context menu row
    // for a particular window ... show me the window preview next to the
    // row"), moving as the hover pane does between apps (owner: "it should
    // behave the same way when i hover another window entry in the context
    // menu"): ONE plate (SPlateTravel) that glides from row to row. peekWant
    // is the window of the row the pointer is on (none: the pane's grace,
    // then it leaves), set by the menu's hover; renderRowPeek moves and draws
    // it.
    struct SRowPeek : SPlateTravel {
        PHLWINDOWREF window;       // shown (the last row's while it waits or leaves)
        CBox         row;          // the row it is beside
        bool         left = false; // the side: picked as it appears, kept until it is gone
        CBox         drawn;        // this frame's plate (debug dockmenu)
    };
    PHLWINDOWREF            peekWant;
    std::optional<SRowPeek> rowPeek;

    // Dock stacks A: Rename in Dock turns the menu into a one-line field.
    bool        editing = false;
    std::string editText;
    bool        editFresh = false; // the name is "selected": the first key replaces it
};

// The menu's hover eases (SMenu::lit / lift): smoothstep over MENU_HOVER_MS on
// the dock's animation clock, and whether any of them still needs frames.
inline float menuHoverEased(const Hyprdrive::Motion::STween<Time::steady_tp>& fade) {
    return Hyprdrive::Motion::smoothstep(std::clamp(elapsedMs(fade.start) / MENU_HOVER_MS, 0.F, 1.F));
}
inline bool menuHoverRunning(const SMenu& menu) {
    const auto RUNNING = [](const Hyprdrive::Motion::STween<Time::steady_tp>& fade) { return elapsedMs(fade.start) < MENU_HOVER_MS + 20.F; };
    return menu.lit.anyRunning(RUNNING) || menu.lift.anyRunning(RUNNING);
}
// Frames while the row preview is on screen at all (it is live, and moves).
inline bool menuPeekShowing(const SMenu& menu) {
    return menu.rowPeek.has_value();
}
// Whether the menu's row buttons answer the pointer at `point` (its render
// px); clears an expired guard.
bool menuButtonsArmed(SMenu& menu, const Vector2D& point, float scale);
extern std::optional<SMenu> g_menu;

// THE MENU EXPANDS LIKE THE SEARCH RESULTS (user, 2026-11-03: "a consistent
// design language across the interface"). Opening, the plate grows out of its
// icon -- the width from about the icon's, the height from nothing, its bottom
// edge where it will stay -- on the decelerating curve, the rows fading in
// clipped to it; closing, it collapses back on the accelerating one. The close
// is drawn from a GHOST: closeMenu() hands the menu over and every query keeps
// seeing a closed menu at once, so nothing else waits on the animation.
struct SMenuGhost {
    SMenu           menu;
    Time::steady_tp closedAt{};
    float           from = 1.F; // how open it was when it closed
};
extern std::optional<SMenuGhost> g_menuGhost;
extern bool menuAnimOn();
extern float menuOpenP(const SMenu& M);
extern bool menuOpening(const SMenu& M);
extern std::string itemDesktopId(const SItem& item);
extern std::vector<SRow> buildRows(const SItem& item, const PHLMONITOR& monitor);
extern std::vector<SRow> buildWindowRows(const SItem& item, const PHLWINDOW& window, const PHLMONITOR& monitor);
// One window's preview beside a menu row, as the hover pane draws a cell
// (DockPreview.cpp). Never hit-tested: nothing records where it is.
extern void renderRowPeek(PHLMONITOR monitor, SMenu& menu, const CBox& plate, bool live, const Time::steady_tp& now);
extern std::vector<SRow> infoRows(const SItem& item);
extern void activateRow(PHLMONITOR monitor, const SItem& item, const SRow& row);
extern void renderMenu(PHLMONITOR monitor, const SLayout& layout);

// ------------------------------------------------ X32 reorder, G12 drop --


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
extern SDockDragState g_dockDrag;
extern float dragThresholdPx(PHLMONITOR monitor);
extern int caretIndex();
extern CBox ghostBoxAt(PHLMONITOR monitor, const CBox& home, const Vector2D& grab, const Vector2D& at);
extern bool ghostShowing();
extern bool ghostFlying();

// V3. The return flight is an EXIT -- the gesture is over and the icon is
// leaving -- so it accelerates, and at the short end of the house band. Erased
// by the render that finds it expired, exactly like a bounce or a launch zoom.
// (The ghost's OPACITY is Dock::DRAG_GHOST_ALPHA in the header: a stack's item
// drag draws the same kind of clone and must not pick its own number.)
constexpr float GHOST_HOME_MS = 220.F;
extern CBox caretBox(const SLayout& layout, int index, float scale);
extern bool pointerFollowsClick();
extern void pointerToWindow(const PHLWINDOW& window);
}
extern int stackDropIndexAt(PHLMONITOR monitor, const Vector2D& point);
extern bool g_dockNativeDrag;

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
extern std::optional<SPress> g_press;
extern float pressDepth(MONITORID monitor, const std::string& subject);
extern Vector2D pressAt(MONITORID monitor, const std::string& subject);
extern bool pressAnimating(MONITORID monitor);
extern std::optional<Vector2D> pointerIn(const CBox& icon, const Vector2D& mouse, double scale);

// Defined down with the panel code (it needs panelContains/panelCellAt), but
// dragRelease sits above that and has to hand a live tear-out off to it.
bool panelDragRelease(PHLMONITOR monitor, const Vector2D& point);
namespace DockDetail {
extern wl_event_source* g_infoPoll;

// Defined in the unnamed namespace further down, with the rest of the hover
// panel. Declared INSIDE one here so it is the same entity: at namespace scope
// this would be a second, external-linkage function and every call below would
// be ambiguous.
bool panelNeedsFramesOn(const PHLMONITOR& monitor);

// How much of `monitor` one of the dock's own animation frames has to repaint,
// in monitor-local RENDER px -- or nullopt for "all of it". Defined with the
// hover panel much further down, because the panel's drawn box is one of the
// things the band has to reach over.
std::optional<CBox> dockDamageRegion(PHLMONITOR monitor, const SLayout& layout);
}

// The dock's name for the frame pump, which lives in core/FramePump.cpp now
// with its state and the notes on why it coalesces and why its mark is erased.
inline void pumpFrame(PHLMONITOR monitor, std::optional<CBox> regionRenderPx = std::nullopt) {
    FramePump::request(std::move(monitor), regionRenderPx);
}
namespace DockDetail {



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
extern std::vector<std::pair<PHLWINDOWREF, Time::steady_tp>> g_previewed;


// The panel GROWS OUT OF THE ICON rather than fading in on the spot -- the same
// trick X54's flight uses, and it is why this needs no alpha on the window
// content: renderOverviewWindow has no alpha parameter, but it scales, so the
// entrance is a box lerp and the previews ride it.
struct SPanel : SPlateTravel {
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
extern SDockHoverPanel g_hover;
extern void forgetPanelGeometry();
extern bool panelOn(const PHLMONITOR& monitor);
extern bool panelContains(const PHLMONITOR& monitor, const Vector2D& point);
extern std::optional<std::pair<CBox, PHLWINDOW>> panelCellAt(const PHLMONITOR& monitor, const Vector2D& point);

// The panel's rows (PreviewGrid): the cells at the settled height, a row
// wrapping past PANEL_ROW_SHARE of the monitor's width, split evenly. Both the
// settled box and every drawn frame ask this, so they agree on the rows.
struct SPanelRows {
    std::vector<size_t> counts; // cells per row, top first
    float               cellH = 0.F;
    float               width = 0.F; // the widest row at cellH
};

// A 1px sliver on the dock's TOP edge, centred on the icon. The panel grows
// from here instead of from the icon box itself, so the lerp never paints
// live window thumbnails over the rest of the pill.
// THE ORGANIC PLATE (user, 2026-10-16: "you need to have reverse rounding so
// that it integrates beautifully into further rows, doesn't look organic at
// all"). The bands of a multi-row panel (one per row, its row's width) as ONE
// outline: convex corners where the outline turns outward, a concave fillet
// where it steps in or out between rows, so a narrower row flows into a wider
// one. Hyprland's rect and blur passes round outward only, so the outline is
// drawn with cairo: the tint as a texture, the frost through the same shape as
// a region. Built while the panel is changing (the rise), cached once it
// settles.
struct SPanelBand {
    double x0, x1, y0, y1; // render px
};
struct SOrganicPlate {
    std::string          key;
    SP<Render::ITexture> tex;
    CRegion              region;
    CBox                 box;
};
extern SOrganicPlate g_organicPlate;
extern void renderHoverPreview(PHLMONITOR monitor, const SLayout& layout, const CBox& anchor, const std::string& wmClass, const std::string& desktopId, const std::string& scratchpadName, const Time::steady_tp& now);
extern bool panelHoldsDock(PHLMONITOR monitor);
extern bool panelNeedsFramesOn(const PHLMONITOR& monitor);
extern std::optional<CBox> dockDamageRegion(PHLMONITOR monitor, const SLayout& layout);
extern uint64_t g_dockSeatDoLaterSeq;
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
// Y11. Asked by COverview::shouldAllowSurfaceFrame. See the comment there
// for why a preview cannot keep itself alive without it.
// Longer than PREVIEWING_MS: that one covers a preview being actively redrawn
// every frame, so it is refreshed constantly. This fires ONCE at the drop and
// has to outlast a browser's reflow, which is several frames, not one.
constexpr float                                       RETILED_MS = 1500.F;
extern std::vector<std::pair<PHLWINDOWREF, Time::steady_tp>> g_retiled;
extern bool panelDragRelease(PHLMONITOR monitor, const Vector2D& point);
namespace DockDetail {

// ------------------------------------------------------------ spring loading
//
// SPRING-LOADED DOCK (user, 2026-10-07: "if I drag a file to the dock ... and I
// hold it over an app for 2 seconds, it should flash the app icon twice (like
// on the mac) and then switch to that app. or if I hold it over any live
// preview from that app for 2 seconds, it should take me to the app"). The
// drag is a client's Wayland DnD; it is never touched -- the app just comes
// forward under it, so the file can be dropped straight into it. Folders are
// the dock's own business (pinning, stacks) and never spring.

    constexpr float SPRING_FLASH_MS = 520.F; // two pulses, then the switch

    struct SSpring {
        std::string                    key;     // "app:<subject>" | "win:<addr>"
        std::string                    subject; // the icon that flashes
        PHLWINDOWREF                   window;  // what it opens
        MONITORID                      monitor = MONITOR_INVALID;
        std::optional<Time::steady_tp> since, flashAt;
        bool                           done = false; // sprung; wait for the pointer to leave
    };
extern SSpring g_spring;
}
}
