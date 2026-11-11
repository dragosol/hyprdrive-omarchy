// Every namespace-scope variable of the dock, in the order the original
// Dock.cpp defined them: one TU, so their construction order is unchanged.
#include "DockParts.hpp"

namespace Hyprdrive::Dock {
using namespace DockDetail;
namespace DockDetail {

// The reference keeps one timeout id per dock and restarts it on every press
// (`_showOverlay`: "Restart the counting if the shortcut is pressed again"). A
// DEADLINE is the same thing with no timer to leak: restarting is one
// assignment and asking whether the overlay is up is one comparison. ONE
// deadline rather than one per monitor because there is one keyboard --
// `_showOverlay` loops over `allDocks` and hands every dock the same timeout.
Time::steady_tp g_overlayUntil = Time::steadyNow();

SDockFlight g_flight;

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

std::unordered_map<MONITORID, SDockVisualState> g_visualStates;

std::unordered_map<MONITORID, SReveal> g_reveal;

// Between hops nothing on the desktop asks for frames, so the next hop is woken
// by a timer rather than by pumping 60 frames a second for an icon that is
// still for two seconds in three -- an app can stay urgent for hours. Removed
// from PLUGIN_EXIT (stopUrgentHopTimer).
wl_event_source* g_hopTimer = nullptr;

std::unordered_map<MONITORID, SBaseLayoutCache> g_baseLayouts;

std::unordered_map<MONITORID, SArrange> g_arrange;
STooltip g_tip;

// Seat is held while the pointer is on dock chrome so axis events cannot
// reach the window Hyprland hit-tests underneath (the pill is not a surface).
bool g_dockSeatHeld = false;
    PHLWINDOWREF                          g_focusNow, g_focusSettled;
    std::chrono::steady_clock::time_point g_focusSince{};
    bool                                  g_focusMeant = false; // g_focusNow came by a key, a click, the dock: no pass-over

std::optional<SCycle> g_cycle;

std::optional<SMenu> g_menu;
std::optional<SMenuGhost> g_menuGhost;

SDockDragState g_dockDrag;
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

std::optional<SPress> g_press;
namespace DockDetail {

// While Get Info waits on the package lookup, ask for a frame now and then so
// the answer appears without the pointer having to move. A main-thread timer,
// not a frame request from inside the render pass.
    wl_event_source* g_infoPoll = nullptr;
}

std::unordered_map<MONITORID, SPumpRequest> g_pumpPending;
std::unordered_map<MONITORID, uint64_t>     g_pumpSeqs;

SPumpReaper g_pumpReaper;
namespace DockDetail {
std::vector<std::pair<PHLWINDOWREF, Time::steady_tp>>      g_previewed;

SDockHoverPanel g_hover;
SOrganicPlate g_organicPlate;

uint64_t g_dockSeatDoLaterSeq = 0;
}
std::vector<std::pair<PHLWINDOWREF, Time::steady_tp>> g_retiled;
namespace DockDetail {
    SSpring g_spring;
}
}
