#pragma once

// THUMBNAILS FOR STACK ITEMS (2026-09-24: "real media file previews in the
// icons, images and videos in particular").
//
// First the freedesktop cache every file manager fills -- including GNOME's
// x-large / xx-large directories, which Nautilus uses now and the stacks were
// not reading. For a file nobody has thumbnailed yet, the system's own
// thumbnailers (/usr/share/thumbnailers/*.thumbnailer: ffmpegthumbnailer for
// video, glycin for images, evince for PDFs...) are run on a worker thread of
// this module -- never on the compositor, and never on the stack reader, whose
// folder listings must not wait behind a slow video -- into
// ~/.cache/hyprdrive/thumbnails.

#include <cstdint>
#include <functional>
#include <string>

namespace ScrollOverview::Thumbs {

    // The thumbnail PNG for `path` if one exists anywhere; "" if not (and
    // then, for a kind a thumbnailer handles, one is queued). Cheap: stats only.
    std::string lookup(const std::string& path, const std::string& kind);

    // Moves every time a queued thumbnail lands (or fails for good).
    uint64_t generation();

    // Called on the worker when one lands; main.cpp points it at the wake fd.
    void setReadyCallback(std::function<void()> cb);

    // Plugin unload: stop the worker (a running thumbnailer is let finish).
    void shutdown();
}
