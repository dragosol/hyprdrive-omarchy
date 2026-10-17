#pragma once

#include "DockDropModel.hpp"

#include "globals.hpp"

#include <hyprland/src/helpers/time/Time.hpp>  // Time::steady_tp: the frame clock a flight draw is stamped with

#include <string>
#include <vector>

// OVERVIEW_CPP_SPEC §13.9 X27. hyprgnomedock's SURFACE: a floating pill at the
// bottom of the monitor, proportioned from the reference Dash to Dock config.
// X25 (live previews) and X26 (drag-to-dock) both need something to draw into
// and drop onto, which is why the surface lands before them.
namespace ScrollOverview::Dock {

struct SItem {
    std::string wmClass;
    std::string icon;    // desktop-entry Icon=, "" when no entry claims the class
    CBox        iconBox; // monitor-local, RENDER px

    // X32. The desktop id, which is what a FAVOURITE is stored as -- and the
    // only handle a pinned-but-not-running app has, since it owns no window and
    // therefore no class. `running` is the split the running dot draws and the
    // click reads: a favourite with no window launches instead of minimizing.
    std::string desktopId;
    bool        favorite = false;
    bool        running  = false;

    // X35. A LOCATION -- the trash, or a mount -- rather than an app. Dash to
    // Dock wraps these as fake ShellApps so its dash can hold them; here they
    // keep their own identity, because every app lookup in Dock.cpp keys on a
    // WINDOW CLASS and a location has none. A non-empty `uri` IS the
    // discriminator, and it is also what activating the item opens.
    std::string uri;
    std::string locationName;
    bool        mount      = false; // so its menu unmounts rather than empties
    bool        canEject   = false;
    bool        mounted    = true;  // false: an fstab point offering Mount instead

    bool        trashEmpty = true;  // no `Empty Trash` row while it is

    // V11 V4. A STACK -- a file or folder the user pinned. `uri` is set to its
    // file:// form as well, deliberately: every `!uri.empty()` branch in
    // Dock.cpp already means "not an app, so no badge, no wiggle, no window
    // list, name from locationName", and all of that is true of a stack. The
    // handful of places a stack differs from a location test `stack` FIRST.
    // That is one added field on nine existing branches rather than a third
    // arm on each of them.
    bool        stack    = false;
    std::string stackPath;          // absolute, no trailing slash
    bool        stackDir = false;   // a folder fans; a file just opens
    // Dock stacks B: the faint cell a folder being dragged over the dock will
    // become (DockDrop's Moses gap). Not a stack yet: nothing reads its path.
    bool        placeholder = false;

    // THE SCRATCHPAD (Scratchpad.hpp). Not an app and not a location: it owns
    // no window class and no desktop id, so `scratchpadName` is its handle the
    // way `uri` is a location's, and its click TOGGLES that special workspace
    // rather than minimizing anything. Nothing is ever stashed from here -- a
    // window minimized off a scratchpad would come back to a normal desktop and
    // quietly empty the shelf.
    bool        scratchpad = false;
    std::string scratchpadName;

    // WHAT THE ITEM IS CALLED BY EVERYTHING THAT REMEMBERS IT: the tooltip's
    // fade, the press squish and DockTilt's hover spring are all keyed on this
    // one string (Dock.cpp, tooltipSubject). Filled in by dockItems() when the
    // item is built, because the alternative is rebuilding it -- a prefix and a
    // concatenation per icon -- inside the render loop of every monitor, every
    // frame (user, 2026-09-14). Never set it by hand: an item built anywhere
    // else answers with an empty subject, which is the "no icon" key.
    std::string subject;

    // 0..1 while the item is arriving (Dock.cpp, "the arrangement"): the icon
    // grows out of its slot and fades in with it. 1 for every settled item.
    float       appear = 1.F;
};

struct SLayout {
    CBox               pill; // monitor-local, RENDER px; empty when there is nothing to draw
    int                rounding = 0;
    std::vector<SItem> items;

    // X32 Show Applications. `show-apps-at-top` false and
    // `show-apps-always-in-the-edge` true, so on a bottom dock it is the LAST
    // slot, hard against the right-hand edge. Empty when `show-apps-button` is
    // off.
    CBox showApps;
};

// Y7. Whether an overview WINDOW DRAG is in flight. Told to the dock rather
// than asked of the overview, and the DISTANCE is then computed here -- the
// dock is the only place that knows where the pill actually is this frame,
// which is also true while it is still sliding out.
void setWindowDragActive(bool active);

// A native (desktop) window drag that the DOCK started, by tearing a live
// preview out. It needs its own flag because the dock swallowed the press that
// began it: Hyprland therefore never entered its own button state machine for
// that drag, so nothing on its side will end the drag when the button comes up.
// The dock has to close what it opened.
bool dockOwnsNativeDrag();
void clearDockNativeDrag();

// Spring loading (Dock.cpp, end): a client's file drag held over a running
// app's icon or one of its previews for delayMs flashes the icon twice and
// brings the app forward. True while it is waiting or flashing (keep ticking).
bool    springUpdate(PHLMONITOR monitor, const Vector2D& local, bool folderDrag, int delayMs);
void    springReset();
float   springFlash(const SItem& item);
bool    springArmed(const SItem& item);
bool    springAnimating();
// The theme's accent (Hyprland's active border), for chrome drawn elsewhere.
CHyprColor accentColor();

// The dock is on and dock:monitor puts it on this output.
bool    onMonitor(PHLMONITOR monitor);

// Pure geometry, so a test rig can assert proportions without a frame.
SLayout layout(PHLMONITOR monitor);
// The dock's icons and the device-px size each monitor's resting cell draws
// them at -- for OverviewRender's icon warmer (IconSize.hpp).
std::vector<std::pair<std::string, double>> iconWarmList();
// The device-px size of a RESTING dock cell on `monitor` (0 with no dock
// there): the size dock icon textures are asked for, whatever box animates.
double restingIconPx(PHLMONITOR monitor);
void    render(PHLMONITOR monitor);

// X26. Is a monitor-local RENDER px point inside the pill? The overview's drop
// handling asks this before it asks any workspace: the pill floats over the main
// area, so the card underneath would otherwise claim the drop.
bool    pillContains(PHLMONITOR monitor, const Vector2D& point);
// The box a click on a cell is tested against (the icon plus the hover pad) --
// anything that must agree with the dock about "was that my cell" uses this.
CBox    cellHitBox(const CBox& iconBox, float scale);

// Dock stacks B. What a drag of files is over. `folderOffer`: the drag is ONE
// folder, so everything on the pill except a stack's centre and Trash is a gap
// it can be pinned into (at `gapIndex`, a stack-slot index).
struct SDropHit {
    DockDropModel::ECell cell = DockDropModel::ECell::None;
    std::string          stackPath;
    int                  gapIndex = -1;
};
SDropHit dropCellAt(PHLMONITOR monitor, const Vector2D& point, bool folderOffer);
// Pill, hover-preview plate, context menu or a stack's fan/pane: the overview's
// tile hover asks this, so nothing lights up BEHIND dock chrome.
bool    uiContains(PHLMONITOR monitor, const Vector2D& point);

// Pill, hover-preview plate, or context menu: the pointer is on dock chrome,
// not on the window painted underneath.
bool    occupiesPointer(PHLMONITOR monitor, const Vector2D& point);

// While the pointer is on the dock, seat focus leaves the window underneath
// so wheel/touchpad scroll cycles that icon's windows instead of scrolling
// the client. Restored when the pointer leaves (or overview/plugin takes over).
void    syncPointerFocus(PHLMONITOR monitor, const Vector2D& point);
void    releasePointerFocus();

// X24. A click inside the pill, in the same monitor-local RENDER px as
// pillContains. An icon min/maxes the app's window on THIS desktop: restore
// if it is hidden, minimize if it is visible. A window only on another
// desktop is focused there -- never launched over, never stashed from here.
// Launch only when the app has no windows at all. A preview still restores
// that one window.
// Returns whether an item claimed the click; the caller swallows the event
// either way, since the pill is not a window and must not leak clicks through.
//
// X33. `middle` runs `middle-click-action` / `shift-middle-click-action`
// instead, which reach ICONS only -- the reference hangs middle-click off the
// AppIcon, so previews and Show Applications are not targets for it.
// Scroll (not click) cycles windows. `ctrl` is the leftover click-cycle.
bool    click(PHLMONITOR monitor, const Vector2D& point, bool shift, bool middle = false, bool ctrl = false);

// Z2. The Y1 hover panel's own button. It sits ABOVE the pill, so `pillContains`
// is false over it and this has to be asked FIRST in both button hooks or the
// click reaches the window underneath instead of the preview it landed on.
// Returns whether the panel claimed the event; `release` false claims without
// acting, matching the dock's press-remembers / release-decides gesture.
bool    panelClick(PHLMONITOR monitor, const Vector2D& point, bool release);

// Y11. Is the dock drawing a LIVE preview of this window right now? The overview
// suppresses frame callbacks for windows whose tile is off screen, which is
// correct for a tile nobody can see and wrong for the same window shown in a
// dock preview -- the client stops committing and the preview freezes. The
// overview asks this before refusing.
bool    isPreviewing(const PHLWINDOW& window);

// A window retiled onto a workspace NOBODY IS LOOKING AT. It has a new size,
// but its client has not drawn at that size yet and the overview refuses its
// frame callbacks (its tile is off screen), so the strip preview shows the old
// buffer letterboxed inside the new frame. noteRetiled opens a bounded window of
// real frames so the client can commit once at the new size and then go quiet
// again -- deliberately NOT "always render", which would keep every off-screen
// desktop live for nothing.
void    noteRetiled(const PHLWINDOW& window);
bool    isRetiling(const PHLWINDOW& window);

// ...and ASK for the frame. Permitting the callback is only half of it: on a
// desktop nobody is looking at, nothing requests a frame in the first place, so
// the client sits on the buffer it already has and the preview stays a still at
// the old size. This is the same presentFeedback the dock previews drive for
// exactly that reason.
void    pumpRetiled(const PHLWINDOW& window, PHLMONITOR monitor, const Time::steady_tp& now);

// X33. A wheel tick over the pill, `up` for one notch away from the user.
// `scroll_action` decides: `cycle-windows` steps through the windows of the icon
// under the pointer, `switch-workspace` moves to the neighbouring workspace from
// anywhere on the pill, `do-nothing` does nothing. All three SWALLOW the tick,
// which is what the return value says -- true whenever the point is on the pill,
// because the pill is not a window and a tick must not leak to the tile under it.
// The reference's two behaviours are ONE setting and are mutually exclusive; see
// the note beside the key in Config.cpp for why `scroll-switch-workspace` is not
// a key of its own.
bool    scroll(PHLMONITOR monitor, const Vector2D& point, bool up);

// X32 reordering and G12 share ONE drop handler, per G12's "the two drag
// sources land in the same dock drop handler and must not fight". Both end in
// pinAt(): a favourite being moved and a search result being pinned are the
// same edit to the same stored list, one at an index it already holds and one
// at an index it does not.
//
// The insertion index a monitor-local RENDER px point names, counted in
// FAVOURITE SLOTS as they are drawn -- 0 before the first icon, size() past the
// last. -1 when the point is not over the pill at all.
int  dropIndexAt(PHLMONITOR monitor, const Vector2D& point);

// X32/G12. Is the point over the Show Applications button? G12 drops there to
// UNPIN, so the caller has to tell the two targets apart before it commits.
bool showAppsContains(PHLMONITOR monitor, const Vector2D& point);

// Move `desktopId` to `index` among the favourites, inserting it if it is not
// one yet. Writes the store. `index` counts the favourites this machine
// actually provides; a stored id nothing provides is never drawn and keeps its
// place.
void pinAt(const std::string& desktopId, int index);
void unpin(const std::string& desktopId);

// X32. The dock's own drag, which is why the pill's press is no longer its
// click: press remembers, motion past the threshold commits to a drag, and only
// the release decides which of the two gestures happened.
//
// V3 (spec V11 V3) gave the release somewhere to go OFF the pill: the active
// desktop, or a workspace card in the overview strip, and the app launches
// there. `dragRelease` is the old dragEnd with that ladder in front of it --
// one entry point, because the decision has to be the same one wherever the
// button happened to come up. It is Drop::resolveIcon, and it is a pure
// function for the same reason the window drop's is (DropDecision.hpp).
//
// Returns whether the gesture was a DROP. False is "this was a click after all"
// (X16's recovery: a release near its own press), and the caller runs click().
// PRESS FEEDBACK. A held left button on an icon eases it down a little, as if
// pressed into the pill, and the release springs it back. Tracked apart from the
// drag state: dragBegin only follows APPS, and the trash, a stack and the
// scratchpad squish too. Every left release ends it (DockInput::handlePill),
// as do a drag going live and dragCancel, so an icon can never stay shrunk.
void pressBegin(PHLMONITOR monitor, const Vector2D& point);
void pressEnd();

bool dragBegin(PHLMONITOR monitor, const Vector2D& point);
bool dragMove(PHLMONITOR monitor, const Vector2D& point);
bool dragRelease(PHLMONITOR monitor, const Vector2D& point);
// ABANDON a drag that will never get a release: the overview closing under a
// held button (Escape, a keybind, a window taking focus). Deliberately NOT
// dragRelease with a miss -- there was no drop, so nothing launches and nothing
// flies home; the gesture simply stops existing, which is what it did before V3
// gave the release its meanings.
void dragCancel();
bool dragPending();
bool dragging();

// G12. A foreign drag (a search result) over the pill lights the same insertion
// caret the dock's own drag draws. -1 clears it.
void setForeignInsert(int index);

// X28. The GNOME app-icon menu, opened by a right-click over an icon. It floats
// above the pill and outlives the press, so while it is up it takes every
// pointer press and a press that misses it dismisses it.
//
// The row boxes are laid out during render() -- measuring a label means asking
// the renderer for its texture, which is only legal inside the pass -- so
// menuContains/menuClick answer against the LAST frame's geometry. That is the
// frame the user is looking at when they click, which is the one that matters.
bool openMenu(PHLMONITOR monitor, const Vector2D& point);
bool menuOpen();
void closeMenu();
// Dock stacks A: while Rename in Dock's field is up the menu owns the keyboard.
bool menuWantsKeyboard();
bool menuHandleKey(uint32_t keycode); // a PRESS; true = claimed
// Get Info's lookup poll: armed when the info view opens; removed on unload.
void armInfoPoll();
void stopInfoPoll();
// The urgent hop's wake-up timer; removed on unload.
void stopUrgentHopTimer();
bool menuContains(PHLMONITOR monitor, const Vector2D& point);
bool menuClick(PHLMONITOR monitor, const Vector2D& point);

// X34. A hotkey press. `number` is the reference's 1..10 -- the SLOT, so the
// tenth app is 10 even though its key is <Super>0 -- and not a keycap, so a
// config that rebinds the digits still names the same app. `shift` and `ctrl`
// are the modifier the binding carries and they pick the same three behaviours
// a click does: plain runs `click-action`, shift runs `shift-click-action`,
// ctrl opens a new window. Out of range is not an error -- the reference
// activates nothing and still shows the overlay, which is what tells you the
// slot is empty.
void hotkey(int number, bool shift, bool ctrl);

// X34. `<Super>q`: show the number overlay for `shortcut_timeout`, and reveal
// the dock while it is up if `hotkeys_show_dock`. Also runs after every hotkey.
void showOverlay();

// X34. Is the overlay up right now? For the acceptance rig, which has to know
// what the frame it is about to capture is supposed to contain.
// Damage the monitor when the pill's shape changes -- an app opening or closing,
// the dock being switched off, the hovered icon moving -- and every frame while a
// live preview is in it. MUST be called before the frame is rendered: damage
// asked for during the pass is cleared with the rest of the frame's ring.
//
// Returns whether the pill's appearance actually changed. The overview throttles
// scheduleFrame while it is open, so a plain damageMonitor from here is swallowed
// -- it has to answer with requestInputFrame() instead, and this is how it knows.
bool damageIfChanged(PHLMONITOR monitor);

// Drop every per-monitor row the dock holds for an output that has gone away.
// Called from the monitor.removed handler beside Flight::abortAll. The frame-
// pump mark is the one that matters: it is cleared by a doLater, and a doLater
// queued against a removed output never runs, which would leave that monitor's
// dock latched out of ever requesting another frame.
void forgetMonitor(MONITORID id);

// WHY THIS DOCK IS ASKING FOR FRAMES, or nullptr when it is not: the name of
// the needsFrames term that answered. For `hyprdrive:debug frames`, because a
// monitor repainting with nothing moving is this plugin's most expensive bug
// class and every previous hunt for one cost a custom build (2026-09-15).
const char* frameReason(PHLMONITOR monitor);

// X30. Feed the autohide pressure barrier with RELATIVE pointer motion. The
// bus's mouse.move carries the absolute position, and at the bottom edge the
// cursor is clamped -- so its position stops changing while the user is still
// pushing into the barrier, and pressure derived from it would read every push
// as zero.
void feedPointerMotion(const Vector2D& delta);

// OVERVIEW_CPP_SPEC 13.12 X54. Minimize / restore WITH the shrink-into-the-icon
// flight. Everything that minimizes must come through these, not through
// Minimize:: directly -- the dispatcher did exactly that and so a keybind
// minimize never animated at all.
// Y8. `bounceOnLand` hops the icon when the window arrives in it, which a DROP
// on the pill asks for and a click does not -- the click was its own
// acknowledgement.
void minimizeWithFlight(PHLMONITOR monitor, PHLWINDOW window, bool bounceOnLand = false, std::optional<CBox> startBox = std::nullopt);
// The test rig's dock click: activate the item whose class is `wmClass`.
bool debugActivate(PHLMONITOR monitor, const std::string& wmClass);
// DockRoom.hpp: the pill tiled windows make room for, in global logical px --
// its RESTING box (no hover magnification), and empty whenever they should not
// (autohide on, dock off, dock:windows_make_room off).
CBox roomPillGlobal(PHLMONITOR monitor);
void restoreWithFlight(PHLMONITOR monitor, PHLWINDOW window);

// P0's settleFlights lived here. It is Flight::abortAll(reason) now --
// FlightController.hpp -- because "land every flight where it stands" and "land
// this flight because it arrived" want the same landing, and having two of them
// is how the monitor-removed path ended up missing a driven restore's geometry
// write-back.

// ---- what the FLIGHT module calls back into the dock ----------------------
// The dock owns the pill, the render context and the hover panel; the flight
// owns the animation. These four are the seam, and they are declared here rather
// than left in the anonymous namespace because FlightController.cpp is a
// different translation unit now.

// X54. Bring the pill out NOW, skipping show_delay: a flight is shorter than the
// delay, so "ask nicely and wait" meant the animation was over before the dock
// arrived -- and render() returns early on an empty pill, so it was never drawn.
void revealNow(PHLMONITOR monitor);

// THE DOCK'S GLASS, and the one way to put a frosted plate on this desktop.
// FIVE surfaces wear it -- the pill, the context menu, the hover panel, V4's
// stack popup and V2's window pill -- and it is one call rather than five copies
// of the same six lines, so a re-theme moves all of them together instead of
// four of them. The blur is the live dual-kawase, inheriting decoration:blur:*
// through the -1 sentinels (X71) and skipped entirely when
// decoration:blur:enabled is off, matching Omarchy windows; the tint on top is
// the dock's background colour, and `alpha` is multiplied into both.
CHyprColor accentColor(); // Hyprland's active border colour (the theme accent)
// roundingPower: Hyprland's decoration:rounding_power for a plate that must be
// concentric with window corners (the hover panel); 2 = circular. currentPass:
// queue into the pass the window previews go to (OverviewWindow uses
// currentPass()), so the plate never comes apart from them under a nested pass.
void frostedPlate(PHLMONITOR monitor, const CBox& box, int rounding, float alpha, const CBox& clip = {}, float roundingPower = 2.F, bool currentPass = false);

// A dragged icon is drawn as a translucent CLONE -- the original is still in its
// cell, and the copy under the pointer says so. ONE token because there are two
// such gestures on one dock: an icon pulled off the pill (Dock.cpp) and a file
// pulled out of an open stack (DockStacks.cpp). Two ghosts at two opacities
// would be a difference nobody could explain.
inline constexpr float DRAG_GHOST_ALPHA = 0.85F;

// Y8/Z10. Hop the icon that is receiving something, once, on acknowledgement.
void armBounce(const std::string& wmClass);

// Y8's hop as ONE motion, evaluated. Two ledgers arm it -- an app's keyed on
// window class, a stack's keyed on path (DockStacks) -- because they key on
// different things; the DURATION and the CURVE live here so the two cannot drift
// into being two different hops. `t` runs 0..1 across HOP_MS, and the result is
// how far the icon is lifted out of the pill, in `iconHeight`'s own units.
inline constexpr float HOP_MS = 420.F;
// A stack's bounce seen from a HIDDEN dock: its pile peeks up from the edge --
// longer than the hop, or it is gone before anyone sees it.
inline constexpr float STACK_PEEK_MS = 900.F;
float                  hopLift(float t, float iconHeight);

// Z15. A flight click kills the Y1 hover panel INSTANTLY -- leaving it alive
// re-eased it from a stale parked box, which read as "a panel dragged from the
// bottom of the screen" under the restore.
void dismissHoverPanelNow();

// Y5. Draw one flight frame: the REAL window, live, through the same path the
// overview draws a tile with. `now` and `ownsPass` are this frame's render
// context, which only the dock knows.
void drawFlightSurface(PHLMONITOR monitor, const PHLWINDOW& window, const CBox& box, float alpha, const Time::steady_tp& now, bool ownsPass);

}
