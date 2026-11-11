// Declarations the DockStacks.cpp parts share: the popup's types and constants, and the
// file-scope state of DockStacks.cpp and the helpers each part calls, declared extern.
// Internal to src/stacks/DockStacks.cpp and its Stack*.cpp parts: never include it anywhere
// else. The include order is DockStacks.cpp's (OverviewInternal.hpp must come first).
#pragma once

#include "Motion.hpp"
#include "DebugPaths.hpp"
#include "MotionCurves.hpp"
#include "OverviewInternal.hpp"
#include "DockStacks.hpp"
#include "Locations.hpp"
#include <unordered_set>
#include "Thumbs.hpp"
#include "BuiltinIcons.hpp"
#include <sys/stat.h>
#include "StackBlur.hpp"
#include "FlightBlur.hpp"
#include "DockDrop.hpp"
#include "AppSearch.hpp"
#include "Compat.hpp"
#include "Config.hpp"
#include "Launch.hpp"
#include "LauncherEntry.hpp"
#include "OverviewRender.hpp"
#include "OverviewPassElement.hpp"
#include "StackReader.hpp"
#include "StoreFile.hpp"
#include "TextInput.hpp"
#include "SearchLetters.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <utility>
#include <string>
#include <unordered_map>
#include <vector>
#include <gio/gio.h>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/pass/TexPassElement.hpp>
#include <hyprland/src/render/gl/GLTexture.hpp>
#include <drm_fourcc.h>
#include <cairo/cairo.h>
#include <format>
#include <hyprland/src/render/pass/BorderPassElement.hpp>
#include <hyprland/src/render/pass/RectPassElement.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprutils/os/Process.hpp>

using namespace Hyprdrive;

namespace StacksDetail {

    struct SStore {
        std::vector<Stacks::SStack>     items;
        std::filesystem::file_time_type stamp{};
        Time::steady_tp                 checkedAt{};
        bool                            loaded = false;
    };

    // The three answers a stack's CELL needs, filled in once per row. Every one
    // of them touches the filesystem, so this is deliberately on the store's
    // read path and not on the dock's build path -- see SStack in the header.
    void decorate(Stacks::SStack& stack);
    void writeStore();
    void showPreviewClose();

    struct SStoreState {
        bool seeded   = false;
        bool readOnly = false; // written by a newer build: never write it back
    };
    // ------------------------------------------------------------- contents --
    using SEntry = StackModel::SEntry;

    // ------------------------------------------------------------- geometry --
    //
    // Sized off the reference's proportions, scaled to this dock's 54 px icon
    // rather than the Dash's 48, and with one deliberate departure: the
    // TIMINGS. The reference eases a row in over 140 ms with an 11 ms stagger.
    // The house law is a 200-400 ms band with entrances decelerating and exits
    // 20-30% faster, and 140 ms is under the floor of it -- so the row duration
    // is 220 ms and the stagger 14, which puts the whole 13-row fan at 388 ms
    // and every individual row inside the band.
    constexpr float FAN_ICON     = 40.F;  // px, unscaled
    constexpr float FAN_ROW_H    = 52.F;
    constexpr float FAN_STEP     = 56.F;  // row height + 4 px gap, the reference's 64/68 ratio
    constexpr float FAN_SHIFT_K  = 2.5F;  // xShift = K * i^POW, straight from the reference
    constexpr float FAN_SHIFT_P  = 1.8F;
    constexpr float FAN_TILT_DEG = 2.5F;  // per row, clockwise
    constexpr float FAN_PAD_X    = 10.F;
    constexpr float FAN_RADIUS   = 10.F;
    constexpr float FAN_GAP      = 10.F;  // between the dock pill and the first row
    constexpr int   FAN_FONT     = 13;
    constexpr int   FAN_LABEL_MAX = 260;  // px, unscaled: ellipsize past this

    constexpr float GRID_COLS    = 4.F;
    constexpr float GRID_CELL_W  = 92.F;
    constexpr float GRID_CELL_H  = 104.F;
    constexpr float GRID_ICON    = 48.F;
    constexpr float GRID_SPACING = 12.F;
    constexpr float GRID_PAD     = 14.F;
    constexpr float GRID_MAX_ROWS = 3.F;  // past this the panel scrolls
    constexpr float GRID_RADIUS  = 14.F;
    constexpr int   GRID_FONT    = 11;

    // THE MOTION, from the user's own GNOME extension (dragosol/dock-stacks,
    // FanPopup/GridPopup), asked for by name on 2026-09-24 -- so these are its
    // numbers, not the house band: each fan row flies out of the cell over
    // 140 ms ease-out-quad, 11 ms after the one below it, growing from 0.1x
    // and fading in, at its fixed tilt; the fan folds back in 84 ms. The pane
    // grows over 200 ms and leaves in 150.
    //
    // ONE REVERSIBLE TIMELINE (user, same day: "spam click the stack and have
    // the animation reverse itself"). The popup's state is a position on its
    // opening timeline; opening runs it forward, closing runs it back (faster,
    // so the fold takes FAN_CLOSE_MS from fully open), and every visual is a
    // pure function of that position -- so turning round mid-flight starts
    // from exactly where everything is, with no jump.
    constexpr size_t FAN_MAX_ITEMS = 12; // the extension's fan-threshold default
    // TWO MOTIONS (user, 2026-09-24: "the fanning out doesn't seem natural
    // enough ... try Serpantinum's curves and timing"), picked by
    // dock:stack_motion so they can be compared on the real desktop:
    //   extension   -- dragosol/dock-stacks' FanPopup: every channel one
    //                  140 ms ease-out-quad, 11 ms stagger, 84 ms fold.
    //   serpantinum -- ilyamiro/serpantinum's dock popups and lists: position
    //                  280 ms OutQuint (a long soft landing), size 280 ms
    //                  OutBack overshoot 1.1 (a slight pop past full size),
    //                  opacity 260 ms OutCubic, rows 40 ms apart (its
    //                  Dropdown), a 150 ms fold (its InQuint exit), hover
    //                  220 ms OutCubic (its dock buttons). A long fan's stagger
    //                  is squeezed so the whole entrance stays under 300 ms of
    //                  stagger -- 13 rows x 40 ms would be three quarters of a
    //                  second before the last row even starts.
    struct SFanMotion {
        float rowMs, alphaMs, staggerMs, staggerCapMs, closeMs, hoverMs;
        bool  serp;
    };
    constexpr float PANE_OPEN_MS   = 200.F;
    constexpr float PANE_CLOSE_MS  = 150.F;

    // Dock stacks A: there is NO item cap. The fan shows every item while it
    // fits the screen, and a stack that does not fit opens as the searchable
    // pane instead -- so nothing is ever silently dropped.
    constexpr size_t STAGGER_ROWS = 24; // the stagger stops growing here, however tall the fan

    // ----------------------------------------------------------- popup state --
    struct SRow {
        CBox        box;     // monitor-local RENDER px, as DRAWN last frame
        std::string path;    // empty: the "N more..." / action row
        std::string name;
        std::string icon;
        bool        action = false;
        int         vis    = -1; // index into SPopup::visible; -1 for the action row
        float       k      = 1.F; // fan: the row's scale on its way in or out
        float       e      = 1.F; // fan: the row's eased progress (0 in the cell)
        float       a      = 1.F; // fan: the row's opacity
        float       rot    = 0.F; // fan: the row's turn right now (radians)
        float       drop   = 0.F; // fan: 0..1 of its way down into the cell's pile (scrolled off the bottom)
        float       top    = 0.F; // fan: 0..1 of its way out past the top slot
    };

    struct SPopup {
        std::string         path;
        StackModel::ESort   sort = StackModel::ESort::Added;
        StackModel::EView   view = StackModel::EView::Auto;
        bool                grid = false; // decided at open: view==Grid, or the fan does not fit
        CBox                lastPlate;    // the pane's plate last frame, for its entrance blur
        // THE FAN SCROLLS (user, 2026-10-08: "scrollable so I can go through
        // more than just what's displayed"). It replaces the "N more in Files"
        // row: `slots` rows show at once, a window `scrollPos` rows into
        // `visible`. A row scrolled off the bottom drops into the cell's pile,
        // and comes back out of it the other way.
        size_t              slots      = 0;   // rows the screen holds (0: not a fan)
        float               scrollGoal = 0.F; // rows; whole once settled
        float               scrollPos  = 0.F; // what is drawn, chasing the goal
        float               scrollVel  = 0.F;
        bool                fingerScroll = false; // a touchpad moved the goal: it snaps to a row once the fingers rest
        Time::steady_tp     lastScrollAt{};       // the scrollbar shows from here, holds, then fades
        float               barAlpha   = 0.F;     // the scrollbar's own fade
        float               maxLabelW  = 0.F;     // the widest label the fan can show, render px (an estimate)
        float               edgeShift  = 0.F;     // how far the left-edge clamp moved the settled fan
        CBox                fanBounds;            // the fan's area as drawn: the wheel and typing answer anywhere in it
        CBox                typingBounds;         // the fan's area when the query started: it keeps the keyboard while the filter shrinks the fan
        Vector2D            moreIcon;             // the "N more in Files" row's icon centre, as laid out
        float               moreIconK = 0.F, moreIconRot = 0.F; // its scale (0: no such row) and turn
        // THE FILTER FIELD beside the fan (2026-10-08): the overview's search
        // pill without its glyph, grown from the side it hugs.
        float               filterW = 0.F, filterV = 0.F;           // the pill's width, logical px (a spring)
        float               filterScroll = 0.F, filterScrollV = 0.F; // text scrolled out of a capped pill, logical px
        float               filterEdge = 0.F, filterEdgeV = 0.F;     // the edge it grows from, logical px
        float               filterContentL = 0.F;                    // the query as last drawn, logical px
        float               filterMaxL = 0.F;                        // the room on its side, logical px
        bool                filterLeft = false, filterPlaced = false, filterMoving = false;
        CBox                filterPill;                              // as DRAWN
        SearchLetters::SAnimator letters;
        MONITORID           monitor = MONITOR_INVALID;
        CBox                anchor;  // the cell it grew out of
        std::vector<SEntry> entries;
        bool                loading   = true;  // no listing yet
        bool                truncated = false; // past StackReader::MAX_ENTRIES
        uint64_t            readerGen = 0;
        // Dock stacks A: no item cap. `visible` indexes `entries` in display
        // order (everything, until the pane's search filters it -- Task 7);
        // `hover` / `previewing` index `visible`.
        std::vector<size_t> visible;
        int                 hover      = -1;
        int                 previewing = -1;    // the row Sushi is showing; drawn enlarged + ringed
        bool                keyboard   = false; // the keyboard moved last: `hover` is its selection
        // The overview's SPACE rule (SearchIntent.hpp): while the user is
        // TYPING a query, Space is a space; once they point at an item or move
        // the selection, it previews (user, 2026-10-08: "I can't add spaces").
        bool                typing     = false;
        int                 cols       = 1;     // the pane's column count (Task 7); 1 for the fan
        Vector2D            lastMouse;
        Time::steady_tp     lastDrawn{};                  // keyboardActive: only a popup actually on screen takes keys
        std::string         query;                        // the pane's search field
        CBox                fieldBox, headerBox, sortBox; // the pane's chrome, as DRAWN
        std::vector<SRow>   rows;        // published for the pointer
        CBox                plate;       // grid only: the whole panel, as drawn
        CBox                gridBox;     // grid only: the scrolled viewport the cells show through
        float               scroll = 0.F;
        Time::steady_tp     since{};
        bool                leaving = false;
        float               tl      = 0.F;   // position on the opening timeline, ms
        std::unordered_map<std::string, float> hoverAmt; // fan: row key -> 0..1 of its hover blow-up
        bool                hoverMoving = false;         // a blow-up still easing (asks for frames)
        std::string         hoverKey;                    // the row the pointer is on this frame
        float               lastProgress = 0.F;          // last frame's, for the motion blur's shutter
        Time::steady_tp     lastTick{};      // when tick() last advanced it
        float               lastDT  = 0.F;   // the ms that tick advanced it by (animation-scaled)
        bool                ticked  = false;
        bool                scrolled = false; // the fan has scrolled since it opened: its pile hands back to the cell's
    };


    // The fan scrollbar's texture (drawArcBar), kept while its shape holds.
    struct SArcBar {
        std::string          key;
        SP<Render::ITexture> tex;
        CBox                 box;
    };
    // THE MOVE GESTURE. Dragging a file out of a fan/grid and onto another
    // stack's cell -- which is the ONE drag that can reach this dock at all
    // (see DockStacks.hpp for why an XDG drag from Nautilus cannot).
    //
    // There is no motion hook here on purpose. render() already runs every
    // frame and already reads g_pInputManager->getMouseCoordsInternal() to
    // decide what is hovered; the ghost rides that same read, so the gesture
    // costs one press handler, one release handler and no new input path.
    struct SItemDrag {
        std::string srcPath;
        std::string icon;
        Vector2D    start;
        bool        live = false;
    };
    constexpr float DRAG_THRESHOLD = 12.F; // px, the reference's, and euclidean here
    constexpr float GHOST_SIZE     = 48.F;

    // ------------------------------------------------------------ animations --
    // Y8's hop, keyed on a PATH: armBounce keys on wmClass and a stack has none,
    // so the LEDGER is a second one. The motion is not -- the duration and the
    // curve are Dock::HOP_MS and Dock::hopLift, so a stack cell and an app icon
    // hop identically and cannot be re-timed apart.

    // The ERROR SHAKE V4 asks for, which this dock did not have -- the nearest
    // thing was X31's urgent wiggle, and that is a 3000 ms attention loop, not
    // an acknowledgement. On the record as an exemption for the same reason the
    // wiggle is: a shake that eased would not read as a refusal.
    constexpr float SHAKE_MS     = 300.F;
    constexpr float SHAKE_CYCLES = 3.F;
    constexpr float SHAKE_AMP    = 0.16F; // of one icon width

    struct SHop {
        std::string     path;
        Time::steady_tp startedAt;
    };

    // THE CLOCK IS THE FRAMES, not the wall (2026-09-24: "it lags, then loads
    // too fast"). A frame that stalls -- a folder's icons and thumbnails being
    // decoded for the first time -- used to be followed by a timeline that had
    // run on without it and jumped to catch up. Each drawn frame advances it
    // by at most one 30 fps frame instead, so a stall pauses the animation
    // rather than eating it. And an opening waits (up to OPEN_WAIT_MS) for the
    // folder's listing, so the rows fly out with their contents, not over a
    // "Loading" row.
    constexpr float MAX_TICK_MS  = 34.F;
    // The fan row under the pointer blows up (user, 2026-09-24), on the
    // extension's hover curve: 120 ms ease-out-quad, here to 1.1x.
    constexpr float HOVER_SCALE  = 1.10F;
    constexpr float OPEN_WAIT_MS = 400.F;

    // THE FAN'S SCROLL MOTION. A critically damped spring: a wheel notch moves
    // the goal a whole row and the fan glides there in ~250 ms, notches in a
    // row stack up without a restart, and nothing overshoots -- an overshoot
    // would bounce a paper back out of the pile it just landed on.
    constexpr float SCROLL_OMEGA   = 18.F;  // 1/s
    // THE SENSITIVITY IS NAUTILUS'S (user, 2026-10-08: "should be same scroll
    // sensitivity as my nautilus app"). GTK moves a list by the axis delta 1:1
    // in logical px, so a finger moves the fan one row per FAN_STEP of travel;
    // a wheel notch is delta 15 and moves one row, and a high-resolution wheel
    // that splits a notch into several events adds up to the same row (each of
    // its events was a whole row at first: four rows a notch).
    constexpr float WHEEL_NOTCH    = 15.F;  // Hyprland's axis delta for one wheel notch
    constexpr float FINGER_REST_MS = 140.F; // no motion this long: snap to the nearest row
    // The scrollbar is there only while the fan moves (user, 2026-10-08:
    // "show up only during a scroll ... and fade away nicely").
    constexpr float BAR_IN_MS   = 120.F;
    constexpr float BAR_HOLD_MS = 650.F;
    constexpr float BAR_OUT_MS  = 420.F;

    void damagePopupMonitor();

        // ---- defined in the parts (and DockStacks.cpp's state)
        extern SStore& store();
        extern uint64_t g_storeGeneration;
        extern std::string canonical(const std::string& path);
        extern SStoreState g_storeState;
        extern void reload(SStore& s);
        extern Stacks::SStack* find(const std::string& path);
        extern std::string iconNameForFile(const std::string& path, bool isDir);
        extern std::string folderIcon(const std::string& path);
        extern std::string pickIconImpl(const std::string& names);
        extern StackReader::SListing sortedListing(const std::string& dir, StackModel::ESort sort);
        extern const SFanMotion& fanMotion();
        extern std::string g_trashStack;
        extern uint64_t g_trashSeenSeq;
        extern std::string g_trashError;
        extern Time::steady_tp g_trashErrorAt;
        extern std::optional<SPopup> g_popup;
        extern SArcBar g_arcBar;
        extern std::string popupState();
        extern bool rowHit(const SRow& R, const Vector2D& point);
        extern bool g_swallowToggleRelease;
        extern std::optional<SItemDrag> g_itemDrag;
        extern std::vector<SHop> g_bounces;
        extern std::unordered_map<std::string, size_t> g_downloads;
        extern std::unordered_map<std::string, std::vector<int64_t>> g_partialMtimes;
        extern wl_event_source* g_partialTimer;
        extern size_t livePartials(const std::vector<int64_t>& mtimes);
        extern std::unordered_map<std::string, std::unordered_set<std::string>> g_seenNames;
        extern std::vector<SHop> g_shakes;
        extern void retireHops();
        extern size_t fanRows(const SPopup& p);
        extern float fanMaxScroll(const SPopup& p);
        extern float timelineSpan(const SPopup& p);
        extern bool scrollMoving(const SPopup& p);
        extern void tick(SPopup& p);
        extern float popupProgress(const SPopup& p);
        extern void setDirection(SPopup& p, bool leaving);
        extern void layoutFan(SPopup& p, PHLMONITOR monitor, const CBox& pill, float progress);
        extern void layoutPane(SPopup& p, PHLMONITOR monitor, const CBox& pill, float progress);
        extern PHLMONITOR popupMonitorPtr();
        extern StackModel::SFanFit fanFit(const SPopup& p, PHLMONITOR monitor);
        extern size_t rotatedFanCapacity(const SPopup& p, const std::vector<std::string>& names, float roomL);
        extern std::vector<std::string> fanNames(const SPopup& p, size_t rows);
        extern bool chooseGrid(const SPopup& p, PHLMONITOR monitor);
        extern void applyListing(SPopup& p, StackReader::SListing&& L, PHLMONITOR monitor);
        extern CHyprSignalListener g_sushiOpen;
        extern CHyprSignalListener g_sushiResize;
        extern bool g_placingSushi;
}

using namespace StacksDetail;
