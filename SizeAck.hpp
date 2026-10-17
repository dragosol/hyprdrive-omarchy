#pragma once

// THE SIZE A WINDOW ACKED (user, 2026-10-16: GTK windows -- ghostty, Files --
// drawn as a small picture in the top-left of their slot, the rest blurred, in
// the desktop, the overview tiles and the strip alike, on mosaic desktops; "as
// soon as I super+drag them they refresh and render properly").
//
// The cause is a bug in Hyprland 0.56's CWindow::onAck. Every size sent is
// queued as (configure serial, size); several sizes sent in one pass share a
// serial, the configure carrying the last. On an ack Hyprland finds the entry
// for that serial, then drops every entry up to it with
//     std::erase_if(acks, [&](el) { return el.first <= SERIAL->first; });
// where SERIAL is an iterator INTO the vector being compacted: as kept entries
// slide down over it, SERIAL->first grows mid-erase and the newer configure's
// entries are dropped too. The next ack then finds only that configure's FIRST
// size, not its last, and Hyprland takes it as the window's acked size.
//
// A window bigger in that stale size than its surface counts as "small"
// (CWLSurface::small), and the renderer draws the surface at
// surface * size / acked: 798x598 acked as 1903x2094 is drawn 335x171. Nothing
// the client does fixes it; only a new size (a drag) replaces the acked one.
//
// It takes two configures in flight with several sizes in the newer one --
// which a plugin reload gives every hidden mosaic window: the unload re-tiles it
// through dwindle one window at a time, the load re-adds it to mosaic one
// window at a time and restores its size, all before the client has answered.
//
// So hyprdrive replaces CWindow::onAck (hooked in main.cpp) with the same code
// minus the bug: the serial is copied before the erase. Drop this when Hyprland
// fixes onAck.

#include <cstdint>
#include <string>

namespace Desktop::View {
    class CWindow;
}

namespace ScrollOverview::SizeAck {
    // CWindow::onAck, fixed. Replaces the original entirely.
    void onAck(Desktop::View::CWindow* window, uint32_t serial);

    // `hyprdrive:debug windows`: every mapped window's rendered box against its
    // goal, its buffer, texture and committed geometry, the size it acked,
    // suspension.
    std::string report();
}
