#pragma once

#include "globals.hpp"

#include <cstdint>

// THE OVERVIEW'S EASTER EGG (user, 2026-10-08). Type a letter or two and
// backspace, five times inside four seconds, and the search pill opens into a
// little frame playing a video (search:easter_egg_video) with its sound. Keys
// are swallowed for the first moments -- the hands are still mid-spam -- and
// after that any key stops it and searching carries on. It also ends on its own
// when the video does.
//
// ffmpeg does the decoding, in real time (-re) and as raw RGBA on a pipe, read
// by a worker thread; the same process plays the audio through PulseAudio, so
// picture and sound share one clock and one kill. The render pass only uploads
// the newest frame. Nothing here runs until the egg is triggered.
//
// Symcheck rule: everything declared here is defined OUTSIDE any anonymous
// namespace in KeyboardCat.cpp.
namespace ScrollOverview::KeyboardCat {
    // The search field's two edits, from handleSearchPress. A backspace that
    // follows a typed character is one cycle; the fifth inside the window
    // starts the egg (returns true).
    void noteTyped();
    bool noteBackspace();

    // Playing or folding: the overview's keys are the egg's.
    bool active();
    // A key PRESS while active. Always claimed; past the grace period it also
    // stops the video.
    bool handleKey(uint32_t keycode);

    // Anything moving or playing: keep the overview drawing.
    bool busy();
    // From the frame tick: stop the sound if the overview stopped drawing us.
    void watchdog();

    // THE STRIP MAKES ROOM (user, 2026-10-08: "the workspace strip should move
    // down to make space for the video, same padding distance as what's keeping
    // it separated from everything else"). The band under the pill the search
    // results already push the strip down with, in logical px: the frame's
    // height past the pill's while the egg is open, 0 otherwise. The strip keeps
    // its own gap below whatever the band holds.
    float bandL();

    // Drawn over the search pill (monitor-local render px), from renderSearch.
    // `bandNow` is the band as animated this frame: the frame grows with it, so
    // the frame and the strip move as one.
    void render(PHLMONITOR monitor, const CBox& pill, float scale, float fade, int rounding, float roundingPower, float bandNow);

    void stop();
    // PLUGIN_EXIT: the process killed, the reader joined, the texture dropped.
    void shutdown();
}
