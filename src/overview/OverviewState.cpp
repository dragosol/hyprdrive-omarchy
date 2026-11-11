// COverview: every namespace-scope variable of the original scrollOverview.cpp, in its original order.
// Cut from Overview.cpp by pure code motion.
#include "OverviewParts.hpp"

using namespace OverviewDetail;


namespace OverviewDetail {
    std::unordered_map<MONITORID, SWallpaperStash> g_wallpaperStash;
    uint64_t                                       g_wallpaperCommits = 0;
    std::unordered_map<MONITORID, SWallpaperRate> g_wallpaperRate;

    // Every commit of a monitor's wallpaper layers, open or closed.
    std::unordered_map<MONITORID, uint64_t> g_wallpaperSeq;
    std::unordered_map<MONITORID, SOpenListen> g_openListen;

// WHICH DESKTOPS THIS MONITOR'S STRIP MAY CREATE: WorkspacePlan.hpp, a pure
// function with its own host test. Everything below only gathers the facts
// for it and applies this monitor's row.
//
// The phantoms every overview holds are part of those facts (rule 6), so they
// are registered here rather than read off the instances: the registry holds
// IOverview, and a plan that could not see another monitor's phantoms would
// hand its id out as a spare.
std::unordered_map<const COverview*, std::vector<PHLWORKSPACEREF>> g_phantomsByOverview;

// seen = forceWindow ran for this window; cleared = it was suspended and we
// cleared it. seen == 0 means the window never reaches forceWindow at all,
// which is how the fullscreen path was caught.
std::unordered_map<std::string, std::pair<uint64_t, uint64_t>> g_unsuspendCount;
// asked = frameWindow was reached for this class; sent = a frame callback was
// actually handed to its surface tree. asked>0 with sent==0 means the throttle
// ate every one of them.
std::unordered_map<std::string, std::pair<uint64_t, uint64_t>> g_frameSent;

std::unordered_map<MONITORID, SFrameCadence> g_frameCadence;

// DIAGNOSTIC: which branch of shouldAllowSurfaceFrame a window last took.
// "the video is not updating in the strip" has a dozen candidate answers above
// the adaptive gate -- off-screen card, occluded by a fullscreen window, not
// shown at all -- and they are indistinguishable from outside. Written only
// while profiling is on (see Prof::enabled), read back by frameDebugLine.
std::unordered_map<std::string, const char*> g_frameGateReason;
std::unordered_map<std::string, const char*> g_damageGateReason;
    Hyprdrive::Motion::SLitSet<Time::steady_tp> g_searchLit;
    bool g_pillHovered = false;
    SPillPress g_pillPress;
    // The placeholder's own letters (user, 2026-11-03: "the text should pop in
    // and erase as if I were typing (same typing animation)"): the same
    // animator the query's letters use, fed the text the pill is showing.
    SearchLetters::SAnimator g_placeholderLetters;
}
std::chrono::steady_clock::time_point g_pillReleasedAt{};

namespace OverviewDetail {

// While the overview holds its submap Hyprland matches only that submap's binds,
// so Omarchy's PRINT screenshot and every volume/brightness/media key went dead
// with the overview open (main.cpp's raw hook was eating them before that -- see
// isGlobalPassthroughKeycode). The keys that mean something to the whole desktop
// are borrowed into the submap for exactly as long as the overview holds it: the
// binds that were live when it opened are marked submap-universal, and those,
// and only those, are unmarked on the way out. Read from the live bind table
// rather than restated here, so Omarchy's own definitions stay the source.
//
// O5/B19: the user's keyboard CHORDS too (Super/Ctrl/Alt held) -- Super+F
// (fullscreen) did nothing with the overview open here while native's worked
// (user, 2026-11-06). Which binds qualify is SubmapBorrow.hpp's rule
// (host-tested), the same file as native's.
std::vector<WP<SKeybind>> g_passthroughBinds;
}

PHLWINDOWREF OvInternal::g_focusAtOpen;

namespace OverviewDetail {
uint64_t g_warpSeq          = 0;
bool     g_handoffShutdown = false;
bool     g_warpCentred     = false; // overviewCenterCursorOnHandoff: this close centres the cursor
}
