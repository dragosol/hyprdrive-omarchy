#include "ConfigParts.hpp"
#include "MotionClock.hpp"
using namespace ConfigDetail;

namespace ConfigDetail {

Hyprdrive::Config::TGestureRegistrar g_gestureRegistrar   = nullptr;

} // namespace ConfigDetail

namespace {

// ONE registration body, laid down TWICE: once under plugin:hyprdrive: and once
// under the deprecated plugin:scrolloverview: prefix. Registering the alias from
// the same expression is the point -- a hand-copied twin drifts, and a default
// or a range that drifts between the two names is a compat layer that lies.
bool                    g_legacyPass = false;
std::deque<std::string> g_keyNames;   // stable: IValue keeps the char*, it does not copy it

std::vector<SP<::Config::Values::IValue>> g_modernValues, g_legacyValues;

const char* KEY(const std::string_view tail) {
    const auto* PREFIX = g_legacyPass ? Hyprdrive::Compat::LEGACY_KEY_PREFIX : Hyprdrive::Compat::KEY_PREFIX;
    return g_keyNames.emplace_back(std::string{PREFIX} + std::string{tail}).c_str();
}

// A formatted description, kept alive the way KEY keeps names: values keep
// the const char* they are given.
const char* stableText(std::string text) {
    return g_keyNames.emplace_back(std::move(text)).c_str();
}

void addKey(SP<::Config::Values::IValue> value) {
    HyprlandAPI::addConfigValueV2(HYPRDRIVE_HANDLE, value);
    (g_legacyPass ? g_legacyValues : g_modernValues).emplace_back(value);
}

}

namespace Hyprdrive::Config {

using namespace ConfigPart;

static void registerConfigValuesFor() {
    using namespace ::Config::Values;

    addKey(makeShared<CIntValue>(KEY("gesture_distance"), "gesture distance in pixels", 200, SIntValueOptions{.min = 1}));
    // MAGNETIC DETENT on the 3-finger workspace drag. The accumulator stays
    // linear in finger travel; only the value handed to viewOffset is warped,
    // by g(u) = u - (K/2pi)*sin(2pi*u) across each card-to-card interval.
    // g(0) = 0 and g(1) = 1 EXACTLY, so a boundary still costs exactly one
    // interval of pad travel -- the curve redistributes motion inside a cell
    // and never adds any. That property is the whole point rather than a nicety:
    // a detent that charged extra travel per workspace would put the far
    // workspaces off the end of a 110mm pad.
    // g'(u) = 1 - K*cos(2pi*u), so at K = 0.35 a card centre moves at 0.65x (it
    // clings) and the midpoint at 1.35x (it releases). Monotonic for |K| < 1,
    // which is why max is 0.9: at K = 1 the derivative touches zero and the
    // cards stall dead under a moving finger.
    addKey(makeShared<CFloatValue>(KEY("gesture_detent"), "magnetic detent strength on the 3-finger workspace drag; 0 = linear", 0.35F,
                                   SFloatValueOptions{.min = 0.F, .max = 0.9F}));
    addKey(makeShared<CIntValue>(KEY("gesture_detent_label"), "show the workspace name pill on the card the workspace drag is settling on", 1,
                                 SIntValueOptions{.min = 0, .max = 1}));
    // Fired once per boundary crossed, through CProcess::runAsync -- never a
    // fork on the compositor thread.
    //
    // The default names Omarchy's keystroke click, so a machine that has one
    // gets the detent tick with no config at all and reuses the sound its
    // capacitive top row already plays rather than introducing a second one.
    // The path is only spawned if it exists and is executable, so this is
    // silent rather than noisy-on-stderr everywhere else. Set empty to disable.
    addKey(makeShared<CStringValue>(KEY("gesture_detent_sound"),
                                    "executable run once per workspace boundary crossed during the drag; empty = silent, missing file = silent",
                                    Hyprlang::STRING{"~/.local/bin/keystroke-click"}));
    // Default alpha 0 = unset; highlightColor() then falls back to the theme's
    // border accent, which is what every build before this one used.
    addKey(makeShared<CGradientValue>(KEY("col_highlight"),
                                      "colour of the selection/hover highlights; unset falls back to general:col.active_border",
                                      CHyprColor{0.F, 0.F, 0.F, 0.F}));
    addKey(makeShared<CFloatValue>(KEY("scale"), "overview scale", 0.82F, SFloatValueOptions{.min = 0.1F, .max = 0.9F}));
    // GLOBAL ANIMATION SPEED. 1 = shipped timings, higher = slower, and it
    // reaches EVERYTHING through animElapsedMs: the overview's open/close, the
    // dock's hover panel, the minimize flight, tooltips, the bounce, the launch
    // zoom. For watching a frame at a time, and for the stress rig.
    addKey(makeShared<CFloatValue>(KEY("anim_scale"), "global animation slowdown; 1 = normal, 10 = ten times slower, 0 = snap", 1.F,
                                   SFloatValueOptions{.min = 0.F, .max = 100.F}));

    // The overview used to borrow `windowsMove` for its own open and close,
    // which meant one curve for both directions and no way to shape either
    // without also changing how every window on the desk moves. These are in
    // Hyprland's animation units -- tenths of a second, so 3.0 is 300ms.
    //
    // The defaults are the house motion rule: an entrance decelerates, an exit
    // accelerates and is ~25% shorter, and neither overshoots.
    addKey(makeShared<CFloatValue>(KEY("open_speed"), "overview open duration, tenths of a second", 2.6F,
                                   SFloatValueOptions{.min = 0.1F, .max = 20.F}));
    addKey(makeShared<CFloatValue>(KEY("close_speed"), "overview close duration, tenths of a second", 1.9F,
                                   SFloatValueOptions{.min = 0.1F, .max = 20.F}));
    addKey(makeShared<CIntValue>(KEY("workspace_gap"), "gap between overview workspaces", 40, SIntValueOptions{.min = 0}));
    addKey(makeShared<CStringValue>(KEY("layout"), "overview layout", Hyprlang::STRING{"vertical"}));
    // The overview search's dictionary and translator (user, 2026-09-22). OFF by
    // default: they send what is typed to a third-party service, and a
    // compositor plugin must not start doing that on its own.
    addKey(makeShared<CBoolValue>(KEY("search:dictionary"), "`define <word>` in the overview search looks the word up on Wiktionary (sends the word to en.wiktionary.org)", false));
    // MOSAIC (Mosaic.hpp). The layout itself is always registered; these two are
    // the panel's: enabled puts it on Super+L's cycle, default makes it the
    // layout of desktops with none of their own.
    // GENERAL (user, 2026-11-04): the file manager the dock's folders, stacks
    // and "Show in Files" open in (Locations::activate / reveal).
    // A FULLSCREEN APP GETS ITS OWN DESKTOP (FullscreenDesktop.hpp, user 2026-11-05).
    addKey(makeShared<CStringValue>(KEY("general:fullscreen_desktop"),
                                    "where an app going fullscreen goes: next (a desktop of its own right after yours; the ones after move along), free (the next free "
                                    "desktop) or stay (where it is). An app alone on its desktop stays in any case",
                                    Hyprlang::STRING{"next"}));
    addKey(makeShared<CStringValue>(KEY("general:file_manager"),
                                    "the file manager folders open in: a desktop entry such as org.kde.dolphin.desktop, or empty for the system's default for folders",
                                    Hyprlang::STRING{""}));
    // WINDOWS FLOAT ONLY WHEN YOU FLOAT THEM (WindowPlacement.cpp; owner,
    // 2026-11-10/11: "add an option to never load windows in floating mode to
    // general"; off by default, always on on Mosaic desktops).
    addKey(makeShared<CBoolValue>(KEY("general:open_tiled"),
                                  "windows float only when you float them: a window the app opens floating (a dialog, a fixed-size window, Wine and Proton's too) is "
                                  "tiled, on every desktop -- Mosaic desktops always do this; float rules, the hyprdrive-float tag, splash screens and tool palettes "
                                  "still float",
                                  false));
    addKey(makeShared<CBoolValue>(KEY("mosaic:enabled"), "mosaic: Super+L cycles Dwindle -> Scrolling -> Mosaic (the panel asks before turning it on)", false));
    addKey(makeShared<CBoolValue>(KEY("mosaic:default"), "mosaic: new desktops use the Mosaic layout (needs mosaic:enabled)", false));
    // THE INFINITE CANVAS (MosaicCanvas.hpp, user 2026-10-26). Opt-in; once on, all
    // directions and the Super zoom-out by default (user, 2026-10-27).
    addKey(makeShared<CBoolValue>(KEY("mosaic:infinite_canvas"),
                                  "mosaic: windows that no longer fit the screen spill onto a canvas you pan across, at their own sizes, instead of tiling", false));
    // WINDOWS YOU DID NOT ASK FOR (Intent.hpp, user 2026-11-04).
    addKey(makeShared<CBoolValue>(KEY("mosaic:quiet_new_windows"),
                                  "mosaic: a window you did not open (a loading screen, an app reopening itself) opens without taking the focus, and the pointer stays on your window",
                                  true));
    addKey(makeShared<CBoolValue>(KEY("mosaic:spawn_at_cursor"),
                                  "mosaic: a new window opens where the pointer is, the others packed around it, on the mosaic and the infinite canvas", true));
    // PUSH THROUGH THE EDGE (Mosaic.cpp pushThrough; user, 2026-11-09).
    addKey(makeShared<CBoolValue>(KEY("mosaic:edge_push"),
                                  "mosaic: on the infinite canvas, pushing the pointer firmly against a screen edge slides the canvas to show the windows hidden past it",
                                  true));
    addKey(makeShared<CIntValue>(KEY("mosaic:edge_push_resistance"),
                                 "mosaic: how far (px) the pointer must keep pushing into the edge before the canvas gives way (the bottom edge, where the dock is, takes half again "
                                 "as much); lower = easier",
                                 400, SIntValueOptions{.min = 50, .max = 1500}));
    addKey(makeShared<CStringValue>(KEY("mosaic:canvas_directions"),
                                    "mosaic: the infinite canvas grows horizontal (a strip) or all (a grid); on "
                                    "either, hold Super and move the pointer to zoom out to every window, hover one to focus it, let go of Super to zoom back in",
                                    Hyprlang::STRING{"all"}));
    // RETIRED (user, 2026-10-29): holding Super always zooms out. Still registered
    // so a config or settings-panel fragment that sets it loads; nothing reads it.
    addKey(makeShared<CStringValue>(KEY("mosaic:canvas_super_move"), "mosaic: retired and ignored (holding Super on the canvas always zooms out)",
                                    Hyprlang::STRING{"zoom"}));
    // Window titles while Super is held (user, 2026-10-30): any desktop, any
    // layout; kept in the Mosaic settings.
    addKey(makeShared<CStringValue>(KEY("mosaic:zoom_labels"),
                                    "window titles while Super is held and the pointer moves, on any desktop (the zoomed-out canvas too): none, hover (the window "
                                    "under the pointer) or all",
                                    Hyprlang::STRING{"hover"}));
    addKey(makeShared<CBoolValue>(KEY("search:browse_on_click"),
                                  "clicking the overview's search pill with nothing typed opens the results: the menu, folder shortcuts and AI & Tools rows", false));
    addKey(makeShared<CBoolValue>(KEY("search:easter_egg"), "overview search: type a letter and backspace five times in four seconds and the pill plays search:easter_egg_video (off unless you turn it on)", false));
    addKey(makeShared<CStringValue>(KEY("search:easter_egg_video"), "the video the overview's easter egg plays, with its sound (needs ffmpeg); a missing file turns the egg off",
                                    Hyprlang::STRING{"~/.local/share/hyprdrive/easter-egg.mp4"}));
    addKey(makeShared<CBoolValue>(KEY("search:translate"), "`tr <lang> <text>` in the overview search translates it (sends the text to the translate endpoint)", false));
    addKey(makeShared<CStringValue>(KEY("search:translate_endpoint"), "LibreTranslate-compatible base URL for `tr`; empty uses the free MyMemory API",
                                    Hyprlang::STRING{""}));
    addKey(makeShared<CStringValue>(KEY("search:translate_target"), "language `translate <text>` translates into when none is given; empty uses $LANG",
                                    Hyprlang::STRING{""}));
    addKey(makeShared<CStringValue>(KEY("terminal"), "terminal used to launch Terminal=true desktop entries; empty falls back to $TERMINAL",
                                    Hyprlang::STRING{""}));
    addKey(makeShared<CIntValue>(KEY("input:scroll_event_delay"), "minimum delay (ms) between discrete scroll steps (wheel workspace nav and trackpad focus stepping)", 200, SIntValueOptions{.min = 0}));
    addKey(makeShared<CFloatValue>(KEY("input:touchpad_scroll_factor"), "overview touchpad scroll factor", 1.F,
                                   SFloatValueOptions{.min = 0.F}));
    addKey(makeShared<CIntValue>(KEY("input:left_handed"), "overview left handed mouse buttons, 2 follows input:left_handed", 2,
                                 SIntValueOptions{.min = 0, .max = 2}));
    addKey(makeShared<CIntValue>(KEY("input:scrolling_mode"), "overview mouse wheel behavior", 0,
                                 SIntValueOptions{.min = 0, .max = 3}));
    addKey(makeShared<CIntValue>(KEY("input:drag_mode"), "overview mouse drag behavior", 0,
                                 SIntValueOptions{.min = 0, .max = 1}));
    addKey(makeShared<CIntValue>(KEY("input:drag_threshold"), "overview drag threshold", 10,
                                 SIntValueOptions{.min = 0}));
    addKey(makeShared<CFloatValue>(KEY("drag_mini_scale"),
                                   "size of the drag proxy while it hovers another workspace, as a fraction of its tile size", 0.45F,
                                   SFloatValueOptions{.min = 0.05F, .max = 1.F}));
    addKey(makeShared<CIntValue>(KEY("drag_mini_ms"), "ms for the drag proxy to shrink to, or grow back from, mini scale", 150,
                                 SIntValueOptions{.min = 0}));
    addKey(makeShared<CIntValue>(KEY("drag_recentre_ms"),
                                 "ms for the drag proxy's grab point to slide to its centre while over a drop target", 160,
                                 SIntValueOptions{.min = 0}));
    addKey(makeShared<CIntValue>(KEY("wallpaper"), "wallpaper mode", 0, SIntValueOptions{.min = 0, .max = 2}));
    // Kept so an old `blur = ...` line still parses; nothing reads it (the frost
    // was always on). plugin:hyprdrive:backdrop picks what it shows.
    addKey(makeShared<CBoolValue>(KEY("blur"), "unused: see plugin:hyprdrive:backdrop", false));
    addKey(makeShared<CStringValue>(KEY("backdrop"),
                                    "what the overview blurs behind the desktop cards: off (the wallpaper darkened, no blur), wallpaper (the static "
                                    "wallpaper), snapshot (a live wallpaper, frozen as the overview opens), live (re-blurred as it moves), auto (live "
                                    "while it changes rarely, snapshot while it animates)",
                                    Hyprlang::STRING{"snapshot"}));

    // OVERVIEW_CPP_SPEC §13.9. Defaults are the user's dumped Dash to Dock
    // config, not upstream defaults. Real keys from day one so hyprsettings
    // (X36-X40) has something to bind to.
    addKey(makeShared<CBoolValue>(KEY("dock:enabled"), "hyprgnomedock: draw the dock", true));
    addKey(makeShared<CIntValue>(KEY("dock:icon_size"), "hyprgnomedock: max icon size (dash-max-icon-size)", 54,
                                 SIntValueOptions{.min = 8, .max = 512}));
    addKey(makeShared<CFloatValue>(KEY("dock:height_fraction"),
                                   "hyprgnomedock: max fraction of the monitor's long axis the dock may span", 0.90F,
                                   SFloatValueOptions{.min = 0.05F, .max = 1.F}));
    addKey(makeShared<CIntValue>(KEY("dock:corner_radius"), "hyprgnomedock: pill corner radius", 24,
                                 SIntValueOptions{.min = 0}));
    // 400ms, both directions: GNOME's MINIMIZE_WINDOW_ANIMATION_TIME, extracted
    // from the host's windowManager.js, and the number the SPEC settles on.
    //
    // The comment that used to sit here said "220ms because the standing
    // Apple-motion rule puts entrances at ~300ms and makes exits 20-30%
    // faster", and it was X24's, written before Y5. Spec 13.14 Y5 supersedes it
    // in as many words -- *"This SUPERSEDES X54(a) 'it wants to be shorter':
    // expo covers ~97% of the travel in the first third of its duration, so
    // 400ms expo is TIGHTER in hand than the 220ms bezier it replaces while
    // being the host's own number. The two requests are not in conflict once
    // the curve is the host's."* The user's own words behind Y5 are "same
    // animations, same fade out/in, same speed... as gnome", which is what
    // decides a tie with a general rule. 400 is also inside the standing
    // 200-400ms band.
    //
    // So the number stays and the note is the thing that was wrong. It cost a
    // round: the key was changed to 220 on the strength of this comment, and
    // the spec says the comment is the stale half. x24_flight_gate.py P7
    // asserts THIS number, from the spec, for the same reason.
    addKey(makeShared<CIntValue>(KEY("dock:minimize_ms"),
                                 "hyprgnomedock: minimize/restore flight duration, 0 to teleport (Y5: GNOME's MINIMIZE_WINDOW_ANIMATION_TIME)", 400,
                                 SIntValueOptions{.min = 0, .max = 2000}));
    // X30. The reference's autohide numbers, verbatim. `intellihide` is false
    // there and inert while off, so it is not a key here.
    addKey(makeShared<CBoolValue>(KEY("dock:autohide"), "hyprgnomedock: hide the dock until the bottom edge is pressed (D2D autohide)", true));
    // The owner, 2026-11-12 (issues #11, #12): "setting for first option with a
    // sub-setting for games and screensaver only", and 2026-11-13: "by default
    // dock should be 'only keep it away from games and screensaver' and the
    // first setting on hiding it from everything fullscreen should be off".
    // An app checked in its Get Info keeps it away whatever these say
    // (DockFullscreen.cpp).
    addKey(makeShared<CBoolValue>(KEY("dock:hide_over_fullscreen"), "the dock keeps away while any fullscreen window is shown", false));
    addKey(makeShared<CBoolValue>(KEY("dock:hide_over_games"),
                                  "the dock keeps away while a fullscreen game or the screensaver is shown (implied by hide_over_fullscreen)", true));
    addKey(makeShared<CFloatValue>(KEY("dock:pressure_threshold"),
                                   "hyprgnomedock: px of motion pushed into the bottom edge before the dock reveals (D2D pressure-threshold)", 100.F,
                                   SFloatValueOptions{.min = 0.F, .max = 2000.F}));
    // FALSE where the reference is true, and the divergence is deliberate: the
    // pressure feed is the RELATIVE motion path, and an absolute pointer never
    // emits one, so `true` is a dock that never comes out on that hardware. See
    // the note at the reveal test in Dock.cpp.
    addKey(makeShared<CBoolValue>(KEY("dock:require_pressure"),
                                  "hyprgnomedock: the bottom edge must be PUSHED past pressure_threshold, not merely touched (D2D require-pressure-to-show)",
                                  false));
    addKey(makeShared<CIntValue>(KEY("dock:show_delay_ms"), "hyprgnomedock: delay from the barrier trigger to the reveal (D2D show-delay)", 250,
                                 SIntValueOptions{.min = 0, .max = 5000}));
    addKey(makeShared<CIntValue>(KEY("dock:hide_delay_ms"), "hyprgnomedock: delay from the pointer leaving to the hide (D2D hide-delay)", 200,
                                 SIntValueOptions{.min = 0, .max = 5000}));
    addKey(makeShared<CIntValue>(KEY("dock:autohide_ms"), "hyprgnomedock: slide duration (D2D animation-time)", 200,
                                 SIntValueOptions{.min = 0, .max = 2000}));
    // X31. The dot's two numbers are GNOME SHELL's, not D2D's: the reference runs
    // `running-indicator-style` DEFAULT, which draws no dot of its own and leaves
    // the shell's own showing -- `.app-grid-running-dot`, 5px, #fafafb.
    addKey(makeShared<CBoolValue>(KEY("dock:running_indicator"), "hyprgnomedock: dot under a running app's icon (D2D show-running)", true));
    addKey(makeShared<CColorValue>(KEY("dock:indicator_color"),
                                   "hyprgnomedock: running-dot colour (alpha 0 = follow general:col.active_border)",
                                   (Hyprlang::INT)0));
    addKey(makeShared<CIntValue>(KEY("dock:indicator_size"), "hyprgnomedock: running-dot diameter in px", 5,
                                 SIntValueOptions{.min = 1, .max = 64}));
    addKey(makeShared<CBoolValue>(KEY("dock:notification_counter"),
                                  "hyprgnomedock: red count badge on the icon (D2D show-icons-notifications-counter)", true));
    addKey(makeShared<CBoolValue>(KEY("dock:emblems"), "hyprgnomedock: draw Unity LauncherEntry emblems at all (D2D show-icons-emblems)", true));
    // DockTilt (user, 2026-09-14): the card lean from dragos.shastasia.com, in
    // two halves the panel switches separately. Both off leaves the flat squish.
    addKey(makeShared<CBoolValue>(KEY("dock:hover_tilt"), "dock icons lean toward the pointer in 3D while hovered, and spring flat on leave", true));
    addKey(makeShared<CBoolValue>(KEY("dock:menu_animation"), "the dock's context menu grows out of its icon and collapses back, like the search results", true));
    addKey(makeShared<CIntValue>(KEY("dock:menu_anim_ms"), "the dock context menu's expand time in ms (its collapse is ~25% shorter); scaled by anim_scale", 180,
                                 SIntValueOptions{.min = 0, .max = 2000}));
    addKey(makeShared<CBoolValue>(KEY("dock:press_tilt"), "a pressed dock icon leans further in toward the press point, with a glare where it is pressed", true));
    addKey(makeShared<CBoolValue>(KEY("dock:dance_urgent"), "hyprgnomedock: wiggle an urgent app's icon (D2D dance-urgent-applications)", true));
    addKey(makeShared<CBoolValue>(KEY("dock:urgent_notify"),
                                  "an urgent app's icon hops up from the screen edge while the dock is hidden (it no longer reveals the dock)", true));
    // X32. `show-favorites`, `show-show-apps-button`, `show-apps-at-top` and
    // `show-apps-always-in-the-edge`, at the reference's values. The last two are
    // a pair: false/true is what puts the button at the right-hand end of a
    // bottom dock rather than in among the apps.
    addKey(makeShared<CBoolValue>(KEY("dock:flight_log"), "flight lifecycle log to $XDG_RUNTIME_DIR/ovdbg.log, for scripts/z_rig.sh + zflight_gate.py", false));
    addKey(makeShared<CBoolValue>(KEY("dock:show_favorites"), "hyprgnomedock: pinned apps in the dock whether running or not (D2D show-favorites)", true));
    addKey(makeShared<CBoolValue>(KEY("dock:show_apps_button"), "hyprgnomedock: the Show Applications button (D2D show-show-apps-button)", true));
    addKey(makeShared<CBoolValue>(KEY("dock:show_apps_at_top"), "hyprgnomedock: Show Applications at the START of the dock (D2D show-apps-at-top)", false));
    addKey(makeShared<CBoolValue>(KEY("dock:show_apps_always_in_the_edge"),
                                  "hyprgnomedock: keep Show Applications hard against the dock's edge (D2D show-apps-always-in-the-edge)", true));
    addKey(makeShared<CStringValue>(KEY("dock:show_apps_icon"), "hyprgnomedock: Show Applications artwork", Hyprlang::STRING{"view-app-grid-symbolic"}));

    // X33. Scroll and middle-click.
    //
    // `scroll-switch-workspace` is NOT a key here, and that is a finding rather
    // than an omission: it is VESTIGIAL in the reference. It survives in the
    // gschema but nothing reads it -- grepped docking.js, appIcons.js, dash.js
    // and prefs.js at master. The switch is `scroll-action='switch-workspace'`,
    // the SAME setting that turns icon cycling off
    // (`_optionalScrollWorkspaceSwitch` binds its handler only when scrollAction
    // is SWITCH_WORKSPACE, `_optionalScrollCycleWindows` only when it is
    // CYCLE_WINDOWS), so the two behaviours are mutually exclusive and one enum
    // is the whole setting.
    //
    // `scroll-to-focused-application` is not a key either: it scrolls a dash
    // that has OVERFLOWED so the focused icon stays in view. This pill never
    // overflows -- the icon size shrinks against height_fraction until it fits --
    // so the requirement holds by construction and a key would be a no-op.
    addKey(makeShared<CBoolValue>(KEY("dock:pointer_to_window"),
                                  "dock: clicking a window preview, or Ctrl+clicking an app, puts the pointer on the middle of the window it brings forward (a plain click on an app leaves the pointer on the dock)", true));
    addKey(makeShared<CStringValue>(KEY("dock:scroll_action"),
                                    "hyprgnomedock: wheel over the dock: cycle-windows | switch-workspace | do-nothing (D2D scroll-action)",
                                    Hyprlang::STRING{"cycle-windows"}));
    addKey(makeShared<CStringValue>(KEY("dock:middle_click_action"),
                                    "hyprgnomedock: middle click on an icon: launch | minimize | cycle-windows | quit | skip (D2D middle-click-action)",
                                    Hyprlang::STRING{"launch"}));
    addKey(makeShared<CStringValue>(KEY("dock:shift_middle_click_action"),
                                    "hyprgnomedock: shift+middle click on an icon (D2D shift-middle-click-action)", Hyprlang::STRING{"launch"}));
    addKey(makeShared<CBoolValue>(KEY("dock:isolate_workspaces"),
                                  "hyprgnomedock: the dock lists only windows on the current workspace (D2D isolate-workspaces)", false));
    // Asked for by a user (2026-10-07): the same scoping, but only while the
    // overview is open -- the desktop's dock stays workspace-agnostic.
    addKey(makeShared<CBoolValue>(KEY("dock:isolate_in_overview"),
                                  "while the overview is open, the dock lists only windows on the current workspace (isolate_workspaces does it everywhere)", false));
    addKey(makeShared<CStringValue>(KEY("dock:monitor"),
                                    "draw the dock on one monitor only: its name (DP-1) or desc:<description prefix>; empty = every monitor. "
                                    "A monitor that is not connected falls back to every monitor",
                                    Hyprlang::STRING{""}));
    // Spring loading and the finger-aware swipe (user, 2026-10-07). SpringLoad.cpp,
    // FingerSwipe.cpp.
    addKey(makeShared<CBoolValue>(KEY("drag:spring_dock"),
                                  "a file drag held over a running app's dock icon or preview for spring_delay_ms flashes it twice and switches to it", true));
    addKey(makeShared<CBoolValue>(KEY("drag:spring_edges"),
                                  "a file/folder/window drag held at the left/right screen edge switches desktop that way every spring_delay_ms (a new one past the last)",
                                  true));
    addKey(makeShared<CIntValue>(KEY("drag:spring_delay_ms"), "how long a drag is held over a dock app or a screen edge before it springs", 1500,
                                 SIntValueOptions{.min = 200, .max = 10000}));
    // Hold to swap and the hot corner (user, 2026-10-13, asked for on r/omarchy).
    // Overview.cpp (updateSwapHold), HotCorner.cpp.
    addKey(makeShared<CBoolValue>(KEY("drag:hold_to_swap"),
                                  "in the overview, a window dragged and held over another window (in the grid or the desktop strip) for hold_to_swap_ms swaps "
                                  "places with it when let go there",
                                  true));
    addKey(makeShared<CIntValue>(KEY("drag:hold_to_swap_ms"), "how long a dragged window is held over another before letting go swaps them", 1500,
                                 SIntValueOptions{.min = 300, .max = 5000}));
    // PR #2 (@johwconst, 2026-10-19), opt-in. OverviewInternal.hpp (pickKeepsFocus).
    addKey(makeShared<CBoolValue>(KEY("focus_follows_pick"),
                                  "with several monitors, closing the overview leaves focus and the pointer where you picked, instead of on another "
                                  "monitor's selected window",
                                  false));
    addKey(makeShared<CBoolValue>(KEY("hot_corner:enabled"), "resting the pointer in a screen corner opens the overview, and closes it again, like GNOME's Activities corner", false));
    addKey(makeShared<CStringValue>(KEY("hot_corner:corner"),
                                    "the hot corner: top-left, top-right, bottom-left or bottom-right (an outer corner only; between two monitors the pointer crosses)",
                                    Hyprlang::STRING{"top-left"}));
    addKey(makeShared<CIntValue>(KEY("hot_corner:delay_ms"), "how long the pointer rests in the hot corner before the overview opens", 150,
                                 SIntValueOptions{.min = 0, .max = 2000}));
    addKey(makeShared<CBoolValue>(KEY("hot_corner:ripple"), "the hot corner sends a ripple across the screen from the corner, bending what is under it", true));
    addKey(makeShared<CBoolValue>(KEY("gestures:keep_swipe"),
                                  "a 3/4-finger swipe keeps going after fingers lift, as long as one finger that started it is down (reads the touchpad's own touches)",
                                  true));
    addKey(makeShared<CBoolValue>(KEY("gestures:swipe_while_dragging"),
                                  "with one finger holding a drag (button down), 3/4 other fingers swipe as a normal gesture instead of flinging the drag", true));
    addKey(makeShared<CBoolValue>(KEY("dock:workspace_agnostic_urgent"),
                                  "hyprgnomedock: an urgent window stays listed even when isolate_workspaces hides its workspace "
                                  "(D2D workspace-agnostic-urgent-windows)",
                                  true));

    // X34. The accelerators are NOT keys -- Hyprland's own `bind` lines are the
    // store GNOME uses gsettings for. These four are the behaviour around them.
    addKey(makeShared<CBoolValue>(KEY("dock:hot_keys"), "hyprgnomedock: Super+number activates dock apps (D2D hot-keys)", false));
    addKey(makeShared<CBoolValue>(KEY("dock:hotkeys_overlay"),
                                  "hyprgnomedock: a hotkey press draws the slot numbers on the icons (D2D hotkeys-overlay)", true));
    addKey(makeShared<CBoolValue>(KEY("dock:hotkeys_show_dock"),
                                  "hyprgnomedock: a hotkey press reveals a hidden dock while the overlay is up (D2D hotkeys-show-dock)", true));
    addKey(makeShared<CFloatValue>(KEY("dock:shortcut_timeout"),
                                   "hyprgnomedock: seconds the number overlay stays up (D2D shortcut-timeout)", 2.F,
                                   SFloatValueOptions{.min = 0.F, .max = 10.F}));

    // X35. The dock's non-app items. `show_mounts` is the MASTER switch in the
    // reference -- `_ensureLocations` never constructs `Removables` while it is
    // off, so the two keys under it are read by nothing -- and it is false in the
    // target config. The trash is therefore the only location the rice draws.
    addKey(makeShared<CBoolValue>(KEY("dock:folders_anywhere"),
                                  "dock stacks: folders can be dragged anywhere among the apps (off: they sit together after the apps; drives, the Trash and Show Overview stay last either way)",
                                  false));
    addKey(makeShared<CBoolValue>(KEY("dock:show_trash"), "hyprgnomedock: a Trash item at the end of the dock (D2D show-trash)", true));
    addKey(makeShared<CBoolValue>(KEY("dock:show_mounts"), "hyprgnomedock: mounted volumes as dock items (D2D show-mounts)", false));
    addKey(makeShared<CBoolValue>(KEY("dock:show_mounts_network"),
                                  "hyprgnomedock: include network shares among the mounts (D2D show-mounts-network)", true));
    addKey(makeShared<CBoolValue>(KEY("dock:show_mounts_only_mounted"),
                                  "hyprgnomedock: only volumes that are mounted, not every one present (D2D show-mounts-only-mounted)", true));
    addKey(makeShared<CStringValue>(KEY("dock:mount_sort"),
                                    "dock: the mounted drives' order -- mounted (as they were mounted), name (alphabetical, numbers as numbers), smart (name, reading "
                                    "spelled-out and Roman numbers too: KleOne, KleTwo, KleThree) or fstab (their order in /etc/fstab; unlisted drives after)",
                                    Hyprlang::STRING{"mounted"}));
    addKey(makeShared<CBoolValue>(KEY("dock:isolate_locations"),
                                  "hyprgnomedock: a location's running state is its own, not the file manager's (D2D isolate-locations)", true));
    addKey(makeShared<CIntValue>(KEY("dock:stack_motion_blur"),
                                 "dock stacks: motion blur samples while the fan moves (0 = off); each frame averages this many instants", 8,
                                 SIntValueOptions{.min = 0, .max = 32}));
    addKey(makeShared<CIntValue>(KEY("layout_motion_blur"),
                                 "motion blur samples while the layout moves or resizes a window -- a dwindle retile, the scrolling tape, a window opening (0 = off); never an interactive drag", 16,
                                 SIntValueOptions{.min = 0, .max = 32}));
    addKey(makeShared<CBoolValue>(KEY("dock:windows_make_room"),
                                  "dock: with autohide off, tiled windows that would sit under the dock are shortened to clear it (only those in the way)", true));
    addKey(makeShared<CIntValue>(KEY("dock:minimize_motion_blur"),
                                 "dock: motion blur samples while a window flies into / out of its icon (0 = off), desktop and overview alike", 16,
                                 SIntValueOptions{.min = 0, .max = 32}));
    // Motion.hpp. The dock stacks keep dock:stack_motion.
    addKey(makeShared<CStringValue>(KEY("animation:curve"),
                                    "the curve family for everything hyprdrive animates except the dock stacks: hyprdrive (the house curves), serpantinum (its window curve), serpantinum_glide, serpantinum_soft, serpantinum_intro, serpantinum_pop, swift, springy, linear",
                                    Hyprlang::STRING{"hyprdrive"}));
    // THE INFINITE CANVAS'S MOTION (Mosaic.cpp canvasEase, user 2026-10-26):
    // focus panning a window into view, the Super zoom.
    addKey(makeShared<CStringValue>(KEY("animation:canvas_curve"),
                                    "the infinite canvas's pans and zoom: a curve family as animation:curve names them (its travel curve); hyprdrive (the house curve, no overshoot) by default",
                                    Hyprlang::STRING{"hyprdrive"}));
    addKey(makeShared<CIntValue>(KEY("animation:canvas_pan_ms"), "the infinite canvas's pans and zoom, in ms (scaled by anim_scale; 0 = instant)", 420,
                                 SIntValueOptions{.min = 0, .max = 2000}));
    // DESKTOP CHANGES, BY CAUSE (DesktopSwitch.hpp, user 2026-10-15). They
    // gate Hyprland's own workspaces animation, per switch: off, a desktop
    // change of that kind lands in one frame and the other kind still slides.
    // Omarchy ships the workspaces animation disabled; then both are moot.
    addKey(makeShared<CBoolValue>(KEY("animation:desktop_keyboard"),
                                  "animate desktop changes a key or scroll bind makes (Super+1..0, Super+Tab, Super+scroll, move window to a desktop); false = instant. Gates Hyprland's workspaces animation, so nothing to do while that is off",
                                  true));
    addKey(makeShared<CBoolValue>(KEY("animation:desktop_focus"),
                                  "animate desktop changes made by focus moving to a window on another desktop (app activation, a dock or notification click, focuswindow); false = instant",
                                  true));
    // SUPER+SCROLL (DesktopSwitch.hpp). The bind itself is the fragments'
    // (30-keybinds.lua, or 40-panel.lua once the panel owns it); these say
    // whether it exists and on which modifiers, and the wheel filter that makes
    // it one desktop per notch follows them.
    addKey(makeShared<CBoolValue>(KEY("binds:scroll_desktops"),
                                  "<mods> + wheel moves through desktops by number, empty ones included, no wrap -- the row of desktops slides with the wheel; replaces Omarchy's Super+scroll",
                                  true));
    addKey(makeShared<CStringValue>(KEY("binds:scroll_desktops_mods"), "modifiers for binds:scroll_desktops, as a bind spells them (SUPER, SUPER + SHIFT, ...)",
                                    Hyprlang::STRING{"SUPER"}));
    // THE DESKTOP STRIP (DesktopStrip.hpp, DesktopRowModel.hpp; user,
    // 2026-11-02): how the wheel moves the row and which desktops are in it.
    addKey(makeShared<CBoolValue>(KEY("binds:scroll_desktops_smooth"),
                                  "the row of desktops follows the wheel continuously (two notches a desktop) and settles on the nearest once the wheel stops; false = a desktop per binds:scroll_desktops_ticks notches",
                                  false));
    addKey(makeShared<CIntValue>(KEY("binds:scroll_desktops_ticks"),
                                 "wheel notches per desktop while binds:scroll_desktops_smooth is off; a notch short shows as a peek toward the next desktop", 1,
                                 SIntValueOptions{.min = 1, .max = 10}));
    addKey(makeShared<CBoolValue>(KEY("binds:scroll_desktops_sticky"),
                                  "after the first desktop of a quick scroll, each further one takes more push (two notches more, or a short dead zone when smooth); let go after 0.4 s still",
                                  false));
    addKey(makeShared<CBoolValue>(KEY("binds:scroll_desktops_stop_at_empty"),
                                  "the wheel stops at the first empty desktop past the last one with windows, as the overview does; false = it goes on to 10 or the highest desktop",
                                  true));
    addKey(makeShared<CBoolValue>(KEY("binds:scroll_desktops_populated_only"),
                                  "the wheel visits only desktops with windows, the first empty one after them and the one shown; false = every desktop number between", false));
    addKey(makeShared<CBoolValue>(KEY("animation:desktop_row_bounce"),
                                  "the row of desktops stretches a little past its first and last desktop and springs back", true));
    addKey(makeShared<CBoolValue>(KEY("animation:desktop_jump_through"),
                                  "a key or dock jump of several desktops (Super+5 from desktop 1) slides through the desktops between", false));
    // MOTION BLUR, PER ANIMATION (user, 2026-09-30). Each site smears a share
    // of its frame-to-frame motion (strength, 0..1; 0 = that site off) shaped
    // over the animation by `fade`: early (full at the start, gone by the
    // halfway point -- the house default), full (fades over the whole
    // animation), late (holds, then fades over the second half), linear, or
    // constant (no fade; the continuous motions -- swipes, keyboard pans --
    // have no progress to fade over). The defaults are the values these sites
    // shipped with. Sample counts stay in layout_motion_blur,
    // dock:minimize_motion_blur and dock:stack_motion_blur.
    addKey(makeShared<CBoolValue>(KEY("motion_blur:enabled"), "motion blur on hyprdrive's animations (false = every site off; samples keys untouched)", true));
    for (const auto& SITE : FlightBlur::SITES) {
        addKey(makeShared<CFloatValue>(KEY(std::format("motion_blur:{}:strength", SITE.name)), stableText(std::format("motion blur: {} -- share of the motion smeared (0 = off)", SITE.label)),
                                       SITE.strength, SFloatValueOptions{.min = 0.F, .max = 1.F}));
        addKey(makeShared<CStringValue>(KEY(std::format("motion_blur:{}:fade", SITE.name)), stableText(std::format("motion blur: {} -- early, full, late, linear or constant", SITE.label)),
                                        Hyprlang::STRING{SITE.fade}));
    }
    addKey(makeShared<CStringValue>(KEY("dock:stack_motion"),
                                    "dock stacks: the fan's motion -- serpantinum (OutQuint travel, OutBack pop, 40 ms stagger) or extension (dock-stacks' 140 ms ease-out-quad)",
                                    Hyprlang::STRING{"serpantinum"}));
    addKey(makeShared<CBoolValue>(KEY("dock:builtin_icons"),
                                  "dock stacks: hyprdrive's own empty-stack artwork (false = the folder icon)", true));
    addKey(makeShared<CStringValue>(KEY("dock:stack_icon_theme"),
                                    "dock stacks: icon theme for stacks and their files -- omarchy (the current Omarchy theme's icons) or gtk",
                                    Hyprlang::STRING{"omarchy"}));
    addKey(makeShared<CStringValue>(KEY("dock:trash_icon"), "dock: the Trash's icon -- hyprdrive (its own artwork), omarchy or gtk", Hyprlang::STRING{"hyprdrive"}));
    addKey(makeShared<CStringValue>(KEY("dock:trash_style"), "dock: hyprdrive Trash glass -- auto (follows the theme), light, dark or clear",
                                    Hyprlang::STRING{"auto"}));
    addKey(makeShared<CStringValue>(KEY("dock:trash_papers"), "dock: what fills hyprdrive's full Trash -- white or color", Hyprlang::STRING{"white"}));

    addKey(makeShared<CColorValue>(KEY("dock:background_color"),
                                   "hyprgnomedock: pill hue (alpha 0 = Omarchy terminal background, else misc:background_color; opacity is background_opacity)",
                                   (Hyprlang::INT)0));
    // -1 follows unfocused default-opacity (0.85), the same glass as idle windows.
    // A pin of 0.19 was Dash-to-Dock glass; without window blur that left
    // the pill almost invisible.
    addKey(makeShared<CFloatValue>(KEY("dock:background_opacity"),
                                   "hyprgnomedock: pill alpha (-1 = Omarchy default-opacity)", -1.F,
                                   SFloatValueOptions{.min = -1.F, .max = 1.F}));
    // X29. The dock's frost is blur-my-shell's `[dash-to-dock]`, not the desktop's
    // window blur: sigma 30, brightness 0.60, live (static-blur false), and no
    // grain. Hyprland's dual-kawase has no sigma at all, so size/passes are not
    // converted, they are MEASURED: 7/3 fits a gaussian of sigma 29.5 against the
    // real backdrop (share/x29/x29_gate.py). 15/2 fits 30.0 just as closely and
    // was passed over because more passes is the better gaussian approximation,
    // which is what the reference actually applies.
    //
    // (user, 2026-08-06): those measurements are KEPT as documentation but are no
    // longer the DEFAULT. "All of those blur configs for those different elements
    // need to be just inherited from hyprland settings" -- the dock was the only
    // surface in the plugin carrying its own blur, which is why it read as a
    // different material from the strip and the cards beside it. Every key below
    // now defaults to -1 = follow decoration:blur:*, and setting any of them back
    // to the numbers above restores the blur-my-shell match for anyone who wants
    // GNOME's dock rather than their own theme's.
    addKey(makeShared<CIntValue>(KEY("dock:blur_size"), "hyprgnomedock: blur size (-1 = follow decoration:blur:size)", -1,
                                 SIntValueOptions{.min = -1, .max = 64}));
    addKey(makeShared<CIntValue>(KEY("dock:blur_passes"), "hyprgnomedock: blur passes (-1 = follow decoration:blur:passes)", -1,
                                 SIntValueOptions{.min = -1, .max = 10}));
    addKey(makeShared<CFloatValue>(KEY("dock:blur_brightness"),
                                   "hyprgnomedock: blur brightness (blur-my-shell brightness; -1 = follow decoration:blur:brightness)", -1.F,
                                   SFloatValueOptions{.min = -1.F, .max = 2.F}));
    addKey(makeShared<CFloatValue>(KEY("dock:blur_noise"),
                                   "hyprgnomedock: blur grain; the reference has none (-1 = follow decoration:blur:noise)", -1.F,
                                   SFloatValueOptions{.min = -1.F, .max = 1.F}));
    // blur-my-shell applies a gaussian and a brightness and nothing else. Leaving
    // Hyprland's contrast at 0.89 and vibrancy at 0.5 in place put an affine term
    // and a saturation boost on top of that, which is a different look -- and it
    // made the measured brightness unrecoverable (it fitted 1.26 with a -51 offset).
    addKey(makeShared<CFloatValue>(KEY("dock:blur_contrast"),
                                   "hyprgnomedock: blur contrast; the reference applies none (-1 = follow decoration:blur:contrast)", -1.F,
                                   SFloatValueOptions{.min = -1.F, .max = 2.F}));
    addKey(makeShared<CFloatValue>(KEY("dock:blur_vibrancy"),
                                   "hyprgnomedock: blur vibrancy; the reference applies none (-1 = follow decoration:blur:vibrancy)", -1.F,
                                   SFloatValueOptions{.min = -1.F, .max = 1.F}));
    addKey(makeShared<CBoolValue>(KEY("live_layer_frost"),
                                  "evaluate dock/bar frost against the overview backdrop instead of the stale optimized blur FB", true));
    addKey(makeShared<CBoolValue>(KEY("smooth_frames"),
                                  "render ahead when the GPU runs late: holds render:new_render_scheduling on for the session and restores yours when off; a late frame then costs one refresh, not half the frame rate", false));
    addKey(makeShared<CFloatValue>(KEY("prefilter_below"),
                                   "mipmap-prefilter a tile's texture when it is minified below this fraction (0 = never)", 0.55F,
                                   SFloatValueOptions{.min = 0.F, .max = 1.F}));
    addKey(makeShared<CBoolValue>(KEY("mission_control"), "lay the focused workspace out as a mission-control grid", true));
    addKey(makeShared<CIntValue>(KEY("mission_control_gap"), "gap between mission-control grid tiles", 40, SIntValueOptions{.min = 0}));
    addKey(makeShared<CIntValue>(KEY("mission_control_reflow_ms"), "ms for a grid tile to travel to a new slot", 300, SIntValueOptions{.min = 0}));
    // The default: Config.hpp OVERVIEW_TILE_ORDER_DEFAULT, spelled here as the
    // literal docs/CONFIG.md and the settings read (config_defaults_test).
    addKey(makeShared<CStringValue>(KEY("overview_tile_order"),
                                    "the order of the overview grid's tiles, top left to bottom right: recent (the most recently used window first), created (the "
                                    "order the windows were opened) or position (as the windows sit on the desktop); taken as the overview opens",
                                    Hyprlang::STRING{"recent"}));
    addKey(makeShared<CBoolValue>(KEY("strip"), "workspace thumbnail strip across the top of the overview", true));
    addKey(makeShared<CIntValue>(KEY("strip_height"), "logical px the strip band reserves at the top of the workspace", 260,
                                 SIntValueOptions{.min = 0}));
    addKey(makeShared<CIntValue>(KEY("strip_max_screen_pct"), "ceiling on the strip band as a percent of screen height, so a strip cell never dwarfs a window preview", 30,
                                 SIntValueOptions{.min = 1, .max = 100}));
    addKey(makeShared<CIntValue>(KEY("strip_gap"), "gap between strip cells", 5, SIntValueOptions{.min = 0}));
    addKey(makeShared<CBoolValue>(KEY("strip_cleanup_button"),
                                  "hold Super over the overview's desktop strip: a Clean Up Empty Desktops button under it removes that monitor's empty desktops between the others", true));
    addKey(makeShared<CBoolValue>(KEY("tap_super"), "tapping SUPER alone toggles the overview", true));
    addKey(makeShared<CIntValue>(KEY("tap_super_timeout_ms"), "longest press-to-release still counted as a SUPER tap (0 = no limit)",
                                 250, SIntValueOptions{.min = 0}));
    addKey(makeShared<CBoolValue>(KEY("shadow:enabled"), "draw a shadow around each workspace card", false));
    addKey(makeShared<CIntValue>(KEY("shadow:range"), "workspace card shadow range", -1));
    addKey(makeShared<CIntValue>(KEY("shadow:render_power"), "workspace card shadow render power", -1));
    addKey(makeShared<CGradientValue>(KEY("shadow:color"), "workspace card shadow color", -1));
}

// Both passes lay the SAME keys down in the SAME order, so entry i of one list
// is the alias of entry i of the other and the pairing needs no name matching.
static void registerConfigValues() {
    g_legacyPass = false;
    registerConfigValuesFor();

    g_legacyPass = true;          // ONE RELEASE
    registerConfigValuesFor();
    g_legacyPass = false;

    for (size_t i = 0; i < g_modernValues.size() && i < g_legacyValues.size(); ++i)
        Hyprdrive::Compat::pairKey(g_modernValues[i]->name(), g_legacyValues[i]->name(), g_modernValues[i]->underlying());
}

void registerConfig() {
    registerLuaFunctions();
    registerConfigValues();
    g_pluginKeysRegistered = true;
    HyprlandAPI::reloadConfig();
}

int getGestureDistance() {
    static auto& VAL = valueRef<Hyprlang::INT>("plugin:hyprdrive:gesture_distance");
    return std::max<int>(1, sc<int>(*VAL));
}

float getGestureDetent() {
    // Clamped again here rather than trusted from the key: |K| >= 1 makes the
    // warp non-monotonic, and a non-monotonic warp means the cards run backwards
    // mid-drag. Cheap insurance against a hand-edited hyprland.conf.
    static auto& VAL = valueRef<Hyprlang::FLOAT>("plugin:hyprdrive:gesture_detent");
    return std::clamp(sc<float>(*VAL), 0.F, 0.9F);
}

bool getGestureDetentLabel() {
    static auto& VAL = valueRef<Hyprlang::INT>("plugin:hyprdrive:gesture_detent_label");
    return *VAL != 0;
}

std::string getGestureDetentSound() {
    auto path = getValue<std::string>("plugin:hyprdrive:gesture_detent_sound");

    // Expanded here rather than at the call site: CProcess exec's the path
    // directly, so there is no shell downstream to expand a ~ later, and the
    // default has to be able to name a file in the user's home without the
    // plugin carrying an absolute path for one machine.
    if (path.starts_with("~/")) {
        if (const auto* HOME = std::getenv("HOME"); HOME && *HOME)
            path = std::string{HOME} + path.substr(1);
        else
            return {};
    }

    return path;
}

namespace ConfigPart {
// Bumped by invalidateThemeCache(); every cache below re-checks when the number
// it last saw is stale. A counter rather than a flag because there is more than
// one cache and each has to see the invalidation exactly once.
uint64_t g_themeGeneration = 1;

} // namespace ConfigPart

float getScale() {
    static auto& VAL = valueRef<Hyprlang::FLOAT>("plugin:hyprdrive:scale");
    return std::clamp(sc<float>(*VAL), 0.1F, 0.9F);
}

float getOpenSpeed() {
    static auto& VAL = valueRef<Hyprlang::FLOAT>("plugin:hyprdrive:open_speed");
    return std::max(0.1F, sc<float>(*VAL));
}

float getCloseSpeed() {
    static auto& VAL = valueRef<Hyprlang::FLOAT>("plugin:hyprdrive:close_speed");
    return std::max(0.1F, sc<float>(*VAL));
}

int getWorkspaceGap() {
    static auto& VAL = valueRef<Hyprlang::INT>("plugin:hyprdrive:workspace_gap");
    return std::max<int>(0, sc<int>(*VAL));
}

bool getSearchDictionary() {
    static auto& VAL = valueRef<Hyprlang::INT>("plugin:hyprdrive:search:dictionary");
    return *VAL != 0;
}

bool getSearchBrowseOnClick() {
    static auto& VAL = valueRef<Hyprlang::INT>("plugin:hyprdrive:search:browse_on_click");
    return *VAL != 0;
}

bool getSearchTranslate() {
    static auto& VAL = valueRef<Hyprlang::INT>("plugin:hyprdrive:search:translate");
    return *VAL != 0;
}

std::string getSearchTranslateEndpoint() {
    return getValue<std::string>("plugin:hyprdrive:search:translate_endpoint");
}

std::string getSearchTranslateTarget() {
    return getValue<std::string>("plugin:hyprdrive:search:translate_target");
}

std::string getTerminal() {
    return getValue<std::string>("plugin:hyprdrive:terminal");
}

ELayout getLayout() {
    static auto& VAL    = valueRef<std::string>("plugin:hyprdrive:layout");
    const auto   LAYOUT = *VAL;
    return LAYOUT == "horizontal" ? ELayout::HORIZONTAL : ELayout::VERTICAL;
}

bool getLeftHanded() {
    static auto& VAL         = valueRef<Hyprlang::INT>("plugin:hyprdrive:input:left_handed");
    const auto   LEFT_HANDED = sc<int>(*VAL);
    if (LEFT_HANDED <= 1)
        return LEFT_HANDED != 0;

    static auto& HOST = valueRef<Hyprlang::INT>("input:left_handed");
    return *HOST != 0;
}

int getDragMode() {
    static auto& VAL = valueRef<Hyprlang::INT>("plugin:hyprdrive:input:drag_mode");
    return std::clamp<int>(sc<int>(*VAL), 0, 1);
}

int getDragThreshold() {
    static auto& VAL = valueRef<Hyprlang::INT>("plugin:hyprdrive:input:drag_threshold");
    return std::max<int>(0, sc<int>(*VAL));
}

// Forwarders to the one animation clock, motion/MotionClock.hpp.
float animScale() {
    return Motion::Clock::scale();
}

bool motionEnabled() {
    return Motion::Clock::enabled();
}

float realElapsedMs(const Time::steady_tp& since) {
    return Motion::Clock::realMs(since);
}

float animElapsedMs(const Time::steady_tp& since) {
    return Motion::Clock::animMs(since);
}

float getTouchpadScrollFactor() {
    static constexpr float OVERVIEWTOUCHPADSCROLLFACTOR = 1.5F;

    static auto& HOST = valueRef<Hyprlang::FLOAT>("input:touchpad:scroll_factor");
    static auto& OURS = valueRef<Hyprlang::FLOAT>("plugin:hyprdrive:input:touchpad_scroll_factor");

    return OVERVIEWTOUCHPADSCROLLFACTOR * std::max<float>(0.F, sc<float>(*HOST)) * std::max<float>(0.F, sc<float>(*OURS));
}

static EScrollAction defaultVerticalScrollAction(ELayout layout) {
    return layout == ELayout::HORIZONTAL ? EScrollAction::COLUMN : EScrollAction::WORKSPACE;
}

EScrollAction getVerticalScrollAction(ELayout layout) {
    static auto& VAL  = valueRef<Hyprlang::INT>("plugin:hyprdrive:input:scrolling_mode");
    const auto   MODE = std::clamp<int>(sc<int>(*VAL), 0, 3);

    switch (MODE) {
        case 1: return defaultVerticalScrollAction(layout) == EScrollAction::WORKSPACE ? EScrollAction::COLUMN : EScrollAction::WORKSPACE;
        case 2: return EScrollAction::WORKSPACE;
        case 3: return EScrollAction::COLUMN;
        case 0:
        // ponytail: mission control draws a grid, not scrolling columns, so COLUMN
        // has nothing to select there - the vertical wheel is only useful as a
        // desktop switch. Explicit scrolling_mode above still overrides this.
        default: return getMissionControl() ? EScrollAction::WORKSPACE : defaultVerticalScrollAction(layout);
    }
}

EScrollAction getHorizontalScrollAction(ELayout layout) {
    return getVerticalScrollAction(layout) == EScrollAction::WORKSPACE ? EScrollAction::COLUMN : EScrollAction::WORKSPACE;
}

int getScrollEventDelay() {
    static auto& VAL = valueRef<Hyprlang::INT>("plugin:hyprdrive:input:scroll_event_delay");
    return std::max<int>(0, sc<int>(*VAL));
}

int getWallpaperMode() {
    static auto& VAL = valueRef<Hyprlang::INT>("plugin:hyprdrive:wallpaper");
    return std::clamp<int>(sc<int>(*VAL), 0, 2);
}

EBackdrop getBackdrop() {
    static auto& VAL = valueRef<std::string>("plugin:hyprdrive:backdrop");
    const auto&  V   = *VAL;
    if (V == "off")
        return EBackdrop::OFF;
    if (V == "wallpaper")
        return EBackdrop::WALLPAPER;
    if (V == "live")
        return EBackdrop::LIVE;
    if (V == "auto")
        return EBackdrop::AUTO;
    return EBackdrop::SNAPSHOT;
}

bool getMissionControl() {
    static auto& VAL = valueRef<Hyprlang::INT>("plugin:hyprdrive:mission_control");
    return *VAL != 0;
}

int getMissionControlGap() {
    static auto& VAL = valueRef<Hyprlang::INT>("plugin:hyprdrive:mission_control_gap");
    return std::max<int>(0, sc<int>(*VAL));
}

int getMissionControlReflowMs() {
    static auto& VAL = valueRef<Hyprlang::INT>("plugin:hyprdrive:mission_control_reflow_ms");
    return std::max<int>(0, sc<int>(*VAL));
}

ETileOrder getOverviewTileOrder() {
    static auto& VAL = valueRef<std::string>("plugin:hyprdrive:overview_tile_order");
    const auto&  V   = *VAL;
    if (V == "recent")
        return ETileOrder::RECENT;
    if (V == "created")
        return ETileOrder::CREATED;
    if (V == "position")
        return ETileOrder::POSITION;
    return OVERVIEW_TILE_ORDER_DEFAULT;
}

bool getStrip() {
    static auto& VAL = valueRef<Hyprlang::INT>("plugin:hyprdrive:strip");
    return *VAL != 0;
}

int getStripHeight() {
    static auto& VAL = valueRef<Hyprlang::INT>("plugin:hyprdrive:strip_height");
    return std::max<int>(0, sc<int>(*VAL));
}

int getStripMaxScreenPct() {
    static auto& VAL = valueRef<Hyprlang::INT>("plugin:hyprdrive:strip_max_screen_pct");
    return std::clamp<int>(sc<int>(*VAL), 1, 100);
}

int getStripGap() {
    static auto& VAL = valueRef<Hyprlang::INT>("plugin:hyprdrive:strip_gap");
    return std::max<int>(0, sc<int>(*VAL));
}

float getDragMiniScale() {
    static auto& VAL = valueRef<Hyprlang::FLOAT>("plugin:hyprdrive:drag_mini_scale");
    return std::clamp(sc<float>(*VAL), 0.05F, 1.F);
}

int getDragMiniMs() {
    static auto& VAL = valueRef<Hyprlang::INT>("plugin:hyprdrive:drag_mini_ms");
    return std::max<int>(0, sc<int>(*VAL));
}

int getDragRecentreMs() {
    static auto& VAL = valueRef<Hyprlang::INT>("plugin:hyprdrive:drag_recentre_ms");
    return std::max<int>(0, sc<int>(*VAL));
}

bool getTapSuper() {
    static auto& VAL = valueRef<Hyprlang::INT>("plugin:hyprdrive:tap_super");
    return *VAL != 0;
}

int getTapSuperTimeoutMs() {
    static auto& VAL = valueRef<Hyprlang::INT>("plugin:hyprdrive:tap_super_timeout_ms");
    return std::max<int>(0, sc<int>(*VAL));
}

// See the note in Config.hpp. Values are read through the registration objects
// rather than through valueRef, so this needs no second spelling of any key --
// and therefore cannot drift from what is actually registered.
uint64_t dockKeyFingerprint() {
    static uint64_t unknownCounter = 0;

    uint64_t   h      = 14695981039346656037ULL;   // FNV-1a
    const auto mix    = [&h](const uint64_t v) { h = (h ^ v) * 1099511628211ULL; };
    const auto mixStr = [&mix](const std::string_view s) { mix(std::hash<std::string_view>{}(s)); };

    const std::string PREFIX = std::string{Compat::KEY_PREFIX} + "dock:";

    for (const auto& VALUE : g_modernValues) {
        if (!VALUE)
            continue;

        const std::string_view NAME{VALUE->name()};
        if (!NAME.starts_with(PREFIX))
            continue;

        mixStr(NAME);

        if (const auto* V = dynamic_cast<::Config::Values::CBoolValue*>(VALUE.get()))
            mix(V->value() ? 1U : 0U);
        else if (const auto* V = dynamic_cast<::Config::Values::CIntValue*>(VALUE.get()))
            mix(sc<uint64_t>(V->value()));
        else if (const auto* V = dynamic_cast<::Config::Values::CColorValue*>(VALUE.get()))
            mix(sc<uint64_t>(V->value()));
        else if (const auto* V = dynamic_cast<::Config::Values::CFloatValue*>(VALUE.get())) {
            const float F    = V->value();
            uint32_t    bits = 0;
            std::memcpy(&bits, &F, sizeof(bits));
            mix(bits);
        } else if (const auto* V = dynamic_cast<::Config::Values::CStringValue*>(VALUE.get()))
            mixStr(V->value());
        else
            mix(++unknownCounter);
    }

    return h;
}

::Config::CCssGapData getCssGapData(const std::string& name) {
    auto& VALUE = valueRef<::Config::IComplexConfigValue>(name);
    if (!VALUE.good())
        return {};

    auto* const GAPS = dc<::Config::CCssGapData*>(VALUE.ptr());
    if (!GAPS)
        return {};

    return *GAPS;
}

int getShadowEnabled() {
    static auto& VAL = valueRef<Hyprlang::INT>("plugin:hyprdrive:shadow:enabled");
    return *VAL != 0 ? 1 : 0;
}

int getShadowRange() {
    static auto& VAL = valueRef<Hyprlang::INT>("plugin:hyprdrive:shadow:range");
    return sc<int>(*VAL);
}

int getShadowRenderPower() {
    static auto& VAL = valueRef<Hyprlang::INT>("plugin:hyprdrive:shadow:render_power");
    return sc<int>(*VAL);
}

// By pointer, not by optional-of-copy: the card shadow asks for this once per
// card and once per strip cell, and a CGradientValueData copy is a heap
// allocation. nullptr means "not pinned" exactly as std::nullopt did.
const ::Config::CGradientValueData* getShadowColor() {
    constexpr auto NAME = "plugin:hyprdrive:shadow:color";

    if (!::Config::mgr()->getConfigValue(NAME).setByUser)
        return nullptr;

    static auto& VAL = valueRef<::Config::IComplexConfigValue>(NAME);
    return gradientPtr(VAL);
}

}
