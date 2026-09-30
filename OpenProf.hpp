#pragma once

// OPEN PROFILE (user, 2026-09-29: "periodic lag as I'm opening the overview").
// `touch /tmp/hyprdrive-openprof.on` and every overview open writes one block to
// /tmp/hyprdrive-openprof.log: how long each step of the open took, then every
// monitor frame of the next OPENPROF_WINDOW_MS -- the gap since that monitor's
// previous frame (a dropped frame shows as a long gap), the whole frame's CPU
// time from render.pre to RENDER_POST, and the overview's own share of it.
// Buffered in memory and written once the window is over, so the file I/O is
// not inside the frames it measures.

#include <cstdint>

namespace OpenProf {
    void init();
    void shutdown();

    // The open starts (a dispatcher, the dock, a gesture): opens the window.
    void begin(const char* what);
    // A step of the open finished; logged with the time since begin().
    void mark(const char* what);
    // CScrollOverview::render()'s own time, added to the current frame.
    void addOverviewRender(uint64_t ns);
    bool active();
    // Queue a GPU timestamp at this point of the render pass: the log then
    // splits the frame's GPU time between consecutive marks.
    void cpuMark(const char* name);
    void gpuMark(const char* name);
}
