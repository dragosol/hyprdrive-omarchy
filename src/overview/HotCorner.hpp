#pragma once

// HOT CORNER (user, 2026-10-13). Push the pointer into the chosen screen corner
// and rest it there for hot_corner:delay_ms, and the overview toggles on every
// monitor, exactly as a Super tap does: it opens, and with the overview up the
// corner closes it again -- the mouse-only way in and out, after GNOME's
// Activities corner. Off by default (hot_corner:enabled): on Omarchy the top-left
// corner is also the bar's menu button.
//
// It fires once per visit: the pointer has to leave the corner before it can
// fire again, and a click in the corner cancels it (that click was meant for the
// bar). Never while a button is held, something is being dragged, a window is
// fullscreen on that monitor, a game holds the pointer, a swipe is running, the
// session is locked, or an overview is closing. Only an OUTER corner
// counts: where another monitor continues past it, the pointer just crosses.
namespace ScrollOverview::HotCorner {
    // After Config::registerConfig() (the keys must exist before anything reads
    // them), and shutdown() at PLUGIN_EXIT: the timer must not outlive the .so.
    void init();
    void shutdown();
}
