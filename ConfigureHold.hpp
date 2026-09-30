#pragma once

// ONE RESIZE PER GESTURE (user, 2026-09-30: "the prompt line keeps moving down
// when I spam click it" -- several Ghostty windows, the dock icon clicked to
// minimize/restore them all, over and over).
//
// Every size Hyprland tells a client is a real resize for it, and a shell
// redraws its prompt on each one (SIGWINCH), one line lower every time. A
// whole-app minimize or restore handed each window a string of them: the
// windows stash one at a time, so the ones still waiting their turn are
// re-tiled after every departure; a single minimize also passes through the
// stash's tiling before it is floated back to its own box; a restore floats in
// and is then tiled. None of those in-between sizes is ever drawn -- the flight
// is -- so none needs to reach the client.
//
// While a hold is up, CWindow::sendWindowSize (hooked in main.cpp) only notes
// the window. When the outermost hold ends, each noted window gets ONE send,
// and Hyprland's own check drops it when the final size is what the client
// already has -- which, for a window that went away and came back, is usual.
// Holds nest; they are scoped to a gesture, never to a frame.

#include "globals.hpp"

#include <functional>

namespace Desktop::View {
    class CWindow;
}

namespace ScrollOverview::ConfigureHold {

    // main.cpp: how to reach the real CWindow::sendWindowSize past the hook.
    void setSender(std::function<void(Desktop::View::CWindow*, bool)> sender);

    // From the hook: true = held (noted for later), the caller must not send.
    bool intercept(Desktop::View::CWindow* window, bool force);

    class CHold {
      public:
        CHold();
        ~CHold();
        CHold(const CHold&)            = delete;
        CHold& operator=(const CHold&) = delete;
    };

    // PLUGIN_EXIT: nothing may stay held once the hook is gone.
    void flush();
}
