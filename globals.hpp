#pragma once

#include <hyprland/src/plugins/PluginAPI.hpp>

#include <chrono>
#include <cstdint>
#include <unistd.h>
#include <cstdlib>
#include <string>

inline HANDLE SCROLLOVERVIEW_HANDLE = nullptr;

// The search query, the keyboard selection, the result drag and the layout the
// search step publishes all live in SearchSession.hpp -- one grouped object with
// one reset, instead of the six loose globals that used to sit here.

// STANDING UX RULE (RICE_PROGRESS.md): the rice has exactly TWO motion curves,
// mirrored in hyprland/general.lua as `appleDecel`/`appleAccel` and in the
// quickshell Appearance.qml. Registered once with Hyprland's animation manager
// in CScrollOverview's constructor, so anything hand-rolled can evaluate the
// SAME curve object instead of approximating it with a polynomial.
inline constexpr const char* HOUSE_DECEL_BEZIER = "hyprdriveOpen";
inline constexpr const char* HOUSE_ACCEL_BEZIER = "hyprdriveClose";

// The house curve at t in [0,1]. Entrances decelerate, exits accelerate.
// Falls back to a cubic of the right direction if called before the curves are
// registered -- a wrong-but-sane shape beats linear.
float houseEase(bool decel, float t);

// OVERVIEW_CPP_SPEC §13.12 X52. Keystroke-to-paint profiler for the search
// path. Gated on $HYPRDRIVE_PROFILE so it is one predictable branch when
// off ($SCROLLOVERVIEW_PROFILE also works, for one release); the standing rule is that a performance claim is closed by a measurement
// of the moment complained about, and this is the instrument for that moment.
namespace Prof {
    inline bool enabled() {
        // The env vars are read from the COMPOSITOR's environment, so turning
        // profiling on used to mean a full logout -- which is a poor trade when
        // the thing to measure is a moment you can reproduce in ten seconds.
        // The sentinel file is read once per plugin LOAD, so `touch` it and
        // hot-reload the plugin and the same instrument is live.
        static const bool ON = std::getenv("HYPRDRIVE_PROFILE") != nullptr || std::getenv("SCROLLOVERVIEW_PROFILE") != nullptr ||
                               ::access("/tmp/hyprdrive-profile.on", F_OK) == 0;
        return ON;
    }

    using clock = std::chrono::steady_clock;

    inline uint64_t    matchNs = 0, matchCalls = 0;   // overviewSearchMatches
    inline uint64_t    appNs   = 0, appCalls   = 0;   // AppSearch::search
    inline uint64_t    rowsNs  = 0, rowsCalls  = 0;   // overviewSearchRows
    inline uint64_t    scanNs  = 0, scanCalls  = 0;   // AppSearch corpus scan (.desktop walk)
    inline uint64_t    srchNs  = 0, srchCalls  = 0;   // renderSearch (layout + text + icons)
    inline uint64_t    txtNs   = 0, txtCold    = 0;   // cold renderText (pango layout + raster + upload)
    inline uint64_t    icoResolveNs = 0, icoDecodeNs = 0, icoUploadNs = 0, icoCold = 0;
    inline uint64_t    frameNs = 0, frameCalls = 0;   // the whole overview render pass
    inline uint64_t    bgNs    = 0, bgCalls    = 0;   // renderWorkspaceBackground walk
    inline uint64_t    liveNs  = 0, liveCalls  = 0;   // renderWorkspaceLive walk (the tiles)
    inline uint64_t    stripNs = 0, stripCalls = 0;   // renderStrip
    inline float       pillScale = 0.F, fieldWL = 0.F; // diagnostic: drawSearch's layout inputs
    inline int         retargets = 0;
    inline clock::time_point keyAt{};
    inline bool        pending = false;
    inline uint64_t    frames  = 0;                   // frames since the keystroke

    inline void reset() { retargets = 0; txtNs = txtCold = scanNs = scanCalls = matchNs = matchCalls = appNs = appCalls = rowsNs = rowsCalls = srchNs = srchCalls = frameNs = frameCalls = frames = 0; icoResolveNs = icoDecodeNs = icoUploadNs = icoCold = 0; bgNs = bgCalls = liveNs = liveCalls = stripNs = stripCalls = 0; }

    // Accumulates into a counter pair. Reads the clock only while profiling.
    struct Timer {
        uint64_t*        ns    = nullptr;
        uint64_t*        calls = nullptr;
        clock::time_point t0{};

        Timer(uint64_t& nsSink, uint64_t& callSink) {
            if (!enabled())
                return;
            ns = &nsSink; calls = &callSink; t0 = clock::now();
        }
        ~Timer() {
            if (!ns)
                return;
            *ns += std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - t0).count();
            ++*calls;
        }
    };
}

// P6i1: the membership snapshot -- a generation counter, a cache keyed on it,
// and the SHADOW that proved the bump set complete before any reader was
// switched over. Membership.cpp owns the cache and carries the evidence;
// OverviewInternal.hpp has the cost split and the one call site.
namespace Membership {
    // THE SHADOW IS A DEBUG INSTRUMENT AND DOES NOT SHIP HOT.
    //
    // The gate it verifies is consulted ~192k times per suite from inside the
    // render pass, the verification recomputes the answer it was meant to save,
    // and a divergence writes a line to a file SYNCHRONOUSLY -- an fopen /
    // fprintf / fclose on the compositor's own thread. That is a fine price for
    // an evidence run and an indefensible one for every frame the user ever
    // sees. So it is off unless asked for, exactly like Prof:: above: the
    // environment is read once and the CALL SITE branches on it, which is what
    // makes "off" mean a predictable branch rather than a cheaper function.
    inline bool shadowEnabled() {
        static const bool ON = std::getenv("HYPRDRIVE_MEMBERSHIP_SHADOW") != nullptr;
        return ON;
    }

    // THE RENDER-LIST SHADOW, and it measures a DIFFERENT thing from the one
    // above. shadowEnabled() asks "does the membership CACHE agree with the
    // live gate"; this one asks "does the list the render actually WALKS agree
    // with the gate at all". `images[i]->windows` is a copy rebuilt only by
    // redrawAll(), so a window that spawns, closes or changes workspace between
    // rebuilds leaves the copy describing a desktop that no longer exists --
    // the Z20 class. Same discipline as above: off unless asked for, and it
    // writes to a file from inside the frame, so it is never a shipping path.
    inline bool renderShadowEnabled() {
        static const bool ON = std::getenv("HYPRDRIVE_RENDERLIST_SHADOW") != nullptr;
        return ON;
    }

    // THE DOCK-PREVIEW SHADOW. A third thing, and the only one aimed at a defect
    // that has never been reproduced in the rig: "dock previews sometimes don't
    // show, randomly, hard to reproduce" (user, 2026-08-17). Fifty laps across
    // the four states y11/z1/x47 do not walk -- overview closed, overview open,
    // a re-hover inside the 250ms TTL, and a hover straight after a flight --
    // came back clean (scripts/dock_preview_soak.sh), so the instrument goes
    // where the rig cannot: the user's own session.
    //
    // It measures the one thing the suspects have in common. A live preview
    // keeps itself alive by being DRAWN: renderLivePreview stamps g_previewed,
    // and the overview's frame hook consults it to decide whether the client is
    // allowed another frame. Miss the 250ms window -- because the dock stopped
    // asking for frames, or because a hover re-armed the panel just as the sweep
    // ran -- and the client stops drawing, which is an invisible preview and no
    // other symptom at all. So: how long has it been since this window's last
    // preview draw, and had its entry already lapsed by the time we got here.
    inline bool previewShadowEnabled() {
        static const bool ON = std::getenv("HYPRDRIVE_PREVIEW_SHADOW") != nullptr;
        return ON;
    }

    void bump(const char* why);
    // The counter bump() moves. Read by anything that caches a per-window
    // answer -- the dock's item list, the search result rows, the open-window
    // match count (user, 2026-09-14): every mutator that changes which windows
    // exist already bumps here, so those caches inherit this bump set instead
    // of growing a second one nobody would remember to feed.
    uint64_t generation();
    bool lookup(const void* key, bool& out);
    void store(const void* key, bool member);
    void divergence(const std::string& cls, const std::string& title, bool live);

    // RENDER-LIST SHADOW ONLY. `kind` is STALE (the copy holds a window the
    // gate no longer puts on this card), MISSING (the gate does and the copy
    // does not), DUP (the copy holds it twice) or CROSS (two cards hold it).
    void renderDivergence(const char* kind, long long cardWs, long long liveWs, const std::string& cls, const std::string& title);
    void renderPass();

    // DOCK-PREVIEW SHADOW ONLY. `gapMs` is real milliseconds since this window
    // was last drawn as a preview (-1 the first time), `lapsed` is whether its
    // liveness entry had already expired when this draw began. Lives here
    // because this file owns the shadow log; nothing else about it is
    // membership.
    void previewGap(const std::string& cls, float gapMs, bool lapsed, bool suspended);
}

bool ensureScrollOverviewHooks();
void disableScrollOverviewHooks();

// THE overview membership gate, reachable from a TU that cannot include the
// internals header. Defined in scrollOverview.cpp as a one-line forwarder to
// `shouldShowOverviewWindow` in OverviewInternal.hpp, which is the single
// authority ~40 call sites already consult.
//
// It exists because Window.cpp had a COPY of that gate -- the ancestor version,
// missing the Z9r5 hidden-for-a-flight check, the Y9 minimized-only-under-a-
// query rule and the search filter -- and the copy decided whether a window's
// blur takes the precomputed path. Two functions of the same name answering
// differently about the same window is the exact shape of the Z20/Y9 bug class,
// and it was one `#include` away from being unfixable: OverviewInternal.hpp
// pulls in scrollOverview.hpp AND Dock.hpp, so Window.cpp cannot have it.
bool overviewShowsWindow(const PHLWINDOW& window);
