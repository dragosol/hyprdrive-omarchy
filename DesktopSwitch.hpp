#pragma once

// DESKTOP SWITCHING BY WHEEL, AND ITS ANIMATION BY CAUSE (user, 2026-10-15).
//
//   * `hyprdrive:desktop next|prev` (Lua: hl.plugin.hyprdrive.desktop("next"))
//     steps the focused monitor one desktop by NUMBER -- empty desktops
//     included, 1 .. max(10, highest there), no wrap. The fragments bind it to
//     <binds:scroll_desktops_mods> + mouse_down / mouse_up in place of
//     Omarchy's `e+1` / `e-1`. With the overview open it moves the overview's
//     focused desktop card instead, the way the overview's own wheel does.
//   * A wheel listener makes that one step per NOTCH: on the configured chord a
//     high-resolution wheel's fractions are swallowed until they add up to a
//     whole notch (DesktopStepModel.hpp), so they neither step nor restart
//     Hyprland's binds:scroll_event_delay clock.
//   * animation:desktop_keyboard / animation:desktop_focus: a desktop change a
//     BIND caused, or one FOCUS caused (activation, a dock or notification
//     click, focuswindow), lands in one frame when its switch is off -- the
//     same warp Hyprland's own `instant` path does. Only Hyprland's workspaces
//     animation is affected (Omarchy ships it disabled, and then this has
//     nothing to cut short); never with the overview open, never a swipe.
namespace ScrollOverview::DesktopSwitch {
    // PLUGIN_INIT, after registerConfig(): the dispatcher, the listeners and
    // the focus hook. Throws (like main.cpp's hooks) if the focus function
    // cannot be found.
    void init();
    // PLUGIN_EXIT: listeners and the hook gone while this code is mapped.
    void shutdown();

    // Held around a focus change the plugin itself makes (the dock activating
    // a window on another desktop): what Hyprland's own focus path marks via
    // the hook.
    class CFocusScope {
      public:
        CFocusScope();
        ~CFocusScope();
        CFocusScope(const CFocusScope&)            = delete;
        CFocusScope& operator=(const CFocusScope&) = delete;
    };
}
