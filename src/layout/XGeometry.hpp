#pragma once

#include <string>

// A FULLSCREEN GAME WHOSE X SIDE DISAGREES WITH HYPRLAND (user, 2026-10-21:
// "the mouse coords don't match the cursor" in Overwatch under Proton;
// Alt+Enter out of fullscreen and back fixes it, minimize and restore
// sometimes does not).
//
// Two ways it happens, both checked on pointer motion for the focused
// fullscreen X11 window:
//
// 1. The window's box. X was left with the tiled box (3828x2148 at 6,6) while
//    Hyprland drew the window fullscreen at 0,0 3840x2160. sendWindowSize skips
//    a send whose box matches what Hyprland believes the client has, and that
//    belief was not what X had. Fixed by forcing one send when the box X
//    reports (ConfigureNotify keeps CXWaylandSurface::m_geometry) differs.
//
// 2. Wine's cursor clip. A fullscreen Wine game confines the pointer with an
//    X grab on an InputOnly, override-redirect window sized to the rect the
//    game clipped to. The game clips when it is activated, and when Hyprland
//    activates it while it is briefly tiled (a window picked under it, then
//    the game again) or before its emulated display mode settles, that rect is
//    the tile box or a 1920x1080 quarter. The game then goes fullscreen and
//    never clips again: X's pointer stays pinned to the old rect's edge while
//    Hyprland's cursor moves on, so the game acts somewhere else than the
//    cursor is. Alt+Enter cured it because the game clips again on a mode
//    change. The signature is unambiguous -- X's pointer inside a mapped
//    InputOnly override-redirect window that Hyprland's cursor has left -- and
//    the fix is the clip the game would have asked for: that window is moved
//    to the fullscreen window's box.
//
// Neither runs while a ConfigureHold is up or the window is animating: those
// boxes are in-between on purpose.

namespace Hyprdrive::XGeometry {
    // main.cpp, after each pointer motion.
    void check();

    // PLUGIN_EXIT: drop the X connection the clip check opened.
    void shutdown();

    // A STALE STATE REQUEST TOOK GAMES OUT OF FULLSCREEN (user, 2026-11-01:
    // "takes my overwatch window out of fullscreen half the time i swap back
    // to it"). Recorded on fedora: Overwatch, unfocused on its desktop, sent
    // one _NET_WM_STATE message -- ADD DEMANDS_ATTENTION, nothing about
    // fullscreen -- and in the same millisecond Hyprland emptied its
    // _NET_WM_STATE and tiled it. Hyprland 0.56 keeps an X window's requested
    // fullscreen/maximize/minimize in std::optionals that nothing ever resets
    // (CXWM::handleClientMessage sets only the atoms a message names), and
    // every state message re-runs CWindow::onUpdateState on all three: an
    // earlier "fullscreen off" request was applied again. Before Hyprland
    // handles a _NET_WM_STATE / WM_CHANGE_STATE message for a mapped window,
    // the three are cleared, so a message changes only what it names.
    //
    // AN X11 WINDOW ASKING FOR ATTENTION WAS NEVER HEARD (user, 2026-11-01:
    // "when overwatch tries to grab my attention ... dock item doesn't bounce
    // ... and it doesn't auto switch to that window"). X11's way is
    // _NET_WM_STATE ADD _NET_WM_STATE_DEMANDS_ATTENTION, which Hyprland 0.56's
    // XWM ignores (it knows FULLSCREEN, HIDDEN and MAXIMIZED only), so the
    // window never became urgent. It is "notice me", not "focus me" (that is
    // _NET_ACTIVE_WINDOW, which Hyprland handles): the window is marked
    // urgent -- the `urgent` event, and the dock's own answer (its icon
    // jumps the hidden dock into view, wiggles on a shown one) -- and focus
    // stays where it is (user: "attention should make the dock icon jump,
    // when it is hidden, and wiggle when it is shown"). REMOVE takes the
    // urgency back.
    //
    // A GAME FLIPPING MAXIMIZED (user, 2026-11-11: World of Warcraft under
    // Wine went between maximized and windowed ~15 times a second, and before
    // that between fullscreen and windowed). Hyprland 0.56 reads every
    // _NET_WM_STATE message naming MAXIMIZED_VERT/HORZ as a TOGGLE
    // (CWindow::onUpdateState flips on any value), and never tells X a window
    // is maximized (CXWaylandSurface::m_maximized is never written, so
    // sendState leaves MAXIMIZED out). Wine reads _NET_WM_STATE back, finds no
    // MAXIMIZED and adds it again: the add un-maximizes, the state is written
    // again without it, and round it goes. Three answers, in the same hook:
    //  - a maximize half that asks for what the window already is (ADD on a
    //    maximized window, REMOVE on one that is not) is taken out of the
    //    message before Hyprland reads it; TOGGLE stays a toggle, FULLSCREEN
    //    (Hyprland already does add/remove right) goes through untouched
    //    (XStateRequest.hpp);
    //  - m_maximized follows the window's client mode, and the state is sent
    //    again when it changes -- after a client message, and on every
    //    window.fullscreen event (keybinds, the dock, a pill, a gesture);
    //  - a window whose own messages change its mode more than 6 times in a
    //    second has its maximize/fullscreen requests ignored for 4 s (logged
    //    once). What the user does never goes through a client message, so it
    //    is never blocked.
    // PLUGIN_INIT; Hyprland unhooks at unload.
    void installStateRequestFix();

    // `hyprdrive:debug xgeom`: what check() would see right now, without
    // acting -- which condition lets it through or stops it.
    std::string status();
}
