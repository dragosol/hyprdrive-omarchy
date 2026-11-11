#pragma once
#include <hyprland/src/SharedDefs.hpp>
#include <string>
#include "globals.hpp"
// SHOW DESKTOP (user, 2026-11-10: "a super + d keybind that moves all windows
// in the active workspace completely out of view with an animation, reversable
// at any point of course, and leave it on by default").
//
// hyprdrive:show_desktop [toggle|on|off] -- in Lua hl.plugin.hyprdrive.
// show_desktop() -- bound to Super+D by the Keybinds switch (hdpanel's
// BIND_SWITCHES "show_desktop", on by default). Every window of the focused
// monitor's desktop -- tiled, floating or fullscreen; not a pinned one, which
// is pinned to stay -- slides straight off its nearest screen edge
// (ShowDesktopMath::outOf) on a critically damped spring, so a second press at
// any point turns them round where they are.
//
// Only what is DRAWN moves: m_realPosition's value, never its goal (Z19,
// FlightController.cpp), so no client is configured and no layout is touched.
// Out of view they are HIDDEN (CWindow::setHidden): not drawn, no input, their
// clients suspended, while every layout -- dwindle, scrolling, Mosaic -- keeps
// their tiles, so nothing reflows and they come back to the places they left.
//
// They come back on the key again, when a new window opens on that desktop or
// one of theirs takes the focus (the dock, a notification), and at once when
// the desktop leaves the screen, before the overview opens (it shows the real
// windows), and when the plugin unloads.
namespace Hyprdrive::ShowDesktop {
    void init();
    void shutdown();

    SDispatchResult dispatch(std::string arg);

    // render.pre, after every other module's: the slide's frame, written last
    // so it is what gets drawn.
    void preRender(PHLMONITOR monitor);

    // Every window back in its place at once (the overview opening).
    void restoreNow();

    // A desktop is shown, or its windows are on their way out or back.
    bool active();
}
