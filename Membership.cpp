#include "globals.hpp"

#include <cstdarg>
#include <filesystem>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>

// P6i1: THE MEMBERSHIP SNAPSHOT. Readers are cut over; this file owns the cache.
//
// `shouldShowOverviewWindow` is consulted from about sixty places -- render,
// hit-test, drag, the grid, the strip, the frame pacer -- and every one of them
// used to recompute the whole answer. One snapshot, invalidated by a generation
// that every mutator bumps, is both cheaper and the only way those callers
// cannot disagree inside a frame.
//
// The danger was always the bump set. A mutator nobody thought of means a stale
// answer: a window that renders when it should not, or worse, one that stops
// rendering and cannot be got back -- the exact shape of Z20 and of the Y9
// minimized-window bug, both of which cost rounds. So the cutover was not taken
// on reasoning. A SHADOW ran first: every call computed the live answer AND what
// a generation-keyed cache would have returned, and wrote down any disagreement.
//
// What the two rounds of that shadow found, and why the cache looks like this:
//
//   ROUND ONE shadowed the WHOLE gate: 15 divergences over a full suite, every
//   one of them `validMapped` going false between `window.close` and
//   `window.destroy` -- a gap no event announces. Not a missing bump. A property
//   that cannot be cached at all.
//   ROUND TWO split the gate BY COST (see OverviewInternal.hpp). The three
//   uncacheable properties are also the cheapest -- four bool reads -- so they
//   are re-read on every call and never come here. What arrives is the expensive
//   half: the search match and the minimize-stash scan, both of which have
//   mutators that already bump. Divergence 0 over ~14M checks across two full
//   suites, hit rate 99.8%. That is the evidence the cutover was waiting for;
//   the log is in zrig/final/.
//
// THE SHADOW IS STILL HERE, and still the way to re-prove this. It is off unless
// HYPRDRIVE_MEMBERSHIP_SHADOW is set (globals.hpp) -- with it on, every call
// recomputes the live answer and compares it against what the cache holds, which
// is a verification mode and not a shipping one: it also writes a line to a file
// from inside the render pass. With it off, none of this code runs.

namespace Membership {
    namespace {
        uint64_t g_generation = 1;

        struct SEntry {
            uint64_t generation = 0;
            bool     member     = false;
        };

        std::unordered_map<const void*, SEntry> g_cache;

        uint64_t g_checks = 0;    // calls to the gate
        uint64_t g_hits   = 0;    // ...answered by an entry of the current generation
        uint64_t g_fills  = 0;    // ...that had to fill or refresh one
        uint64_t g_bumps  = 0;
        uint64_t g_diverged = 0;

        // The last few reasons, so a divergence can be read against what had
        // just happened rather than against the whole log.
        std::string g_lastBump = "(none)";

        const char* logPath() {
            static std::string PATH = [] {
                const char* HOME = std::getenv("HOME");
                std::string dir  = std::string(HOME ? HOME : "/tmp") + "/.local/state/hyprdrive";
                // NOT system("mkdir -p"): forking a shell out of a running
                // compositor to make one directory is the sort of thing that
                // works until the day it does not.
                std::error_code ec;
                std::filesystem::create_directories(dir, ec);
                return dir + "/membership-shadow.log";
            }();
            return PATH.c_str();
        }

        void say(const char* fmt, ...) {
            FILE* f = std::fopen(logPath(), "a");
            if (!f)
                return;
            va_list ap;
            va_start(ap, fmt);
            std::vfprintf(f, fmt, ap);
            va_end(ap);
            std::fclose(f);
        }

        void report(const char* when) {
            say("REPORT %-18s generation %llu  checks %llu  hits %llu (%.1f%%)  fills %llu  bumps %llu  DIVERGED %llu  cache %zu  last bump %s\n", when,
                (unsigned long long)g_generation, (unsigned long long)g_checks, (unsigned long long)g_hits,
                g_checks ? 100.0 * (double)g_hits / (double)g_checks : 0.0, (unsigned long long)g_fills, (unsigned long long)g_bumps,
                (unsigned long long)g_diverged, g_cache.size(), g_lastBump.c_str());
        }
    }

    // WHAT THE SNAPSHOT IS KEYED ON, read out. The dock memoizes its item list
    // on this plus a cheap window-list digest (Dock.cpp, dockItems): every
    // mutator that can change which windows exist already bumps here, so the
    // dock gets that whole bump set for free rather than growing a second one
    // that could drift out of step with this one (user, 2026-09-14).
    uint64_t generation() {
        return g_generation;
    }

    // The generation ALWAYS moves -- the cache is keyed on it, so gating this
    // would be gating correctness. Only the bookkeeping that exists to explain a
    // divergence is behind the shadow: `why` is a literal, but assigning it to a
    // std::string is an allocation on a path a mutator can reach.
    void bump(const char* why) {
        ++g_generation;

        if (!shadowEnabled())
            return;

        ++g_bumps;
        g_lastBump = why ? why : "?";
    }

    // THE CACHE READ. True when this generation already has an answer for
    // `key`, written to `out`; false means the caller must compute it and
    // store() the result.
    //
    // Keyed on the raw window pointer, and an address CAN be reused by a later
    // window. That is safe here and not by luck: every open, close, kill and
    // destroy bumps the generation, so a reused address is always consulted in
    // a generation the old entry does not carry, and misses.
    bool lookup(const void* key, bool& out) {
        ++g_checks;

        if (shadowEnabled()) {
            if (g_checks == 1)
                report("first check");
            else if (g_checks % 20000 == 0)
                report("periodic");
        }

        // Bounded the same way the grid maps are. Dropping the table costs a
        // generation of refills and nothing else -- every entry is derivable.
        if (g_cache.size() > 512)
            g_cache.clear();

        const auto IT = g_cache.find(key);
        if (IT == g_cache.end() || IT->second.generation != g_generation) {
            ++g_fills;
            return false;
        }

        ++g_hits;
        out = IT->second.member;
        return true;
    }

    void store(const void* key, bool member) {
        g_cache[key] = SEntry{g_generation, member};
    }

    // RENDER-LIST SHADOW ONLY. Counted per FRAME: one stale entry reported every
    // frame for a second would drown the log, so each line carries the pass
    // number and the running total, and the periodic report carries both.
    namespace {
        uint64_t g_renderPasses   = 0;
        uint64_t g_renderDiverged = 0;
    }

    void renderDivergence(const char* kind, long long cardWs, long long liveWs, const std::string& cls, const std::string& title) {
        ++g_renderDiverged;
        say("RENDER-%-7s card=ws%lld  lives-on=ws%lld  class=%s  title=%s  pass %llu  total %llu\n", kind, cardWs, liveWs, cls.c_str(), title.c_str(),
            (unsigned long long)g_renderPasses, (unsigned long long)g_renderDiverged);
    }

    // DOCK-PREVIEW SHADOW ONLY. Quiet by design: a preview redrawn every frame
    // is the healthy case and would write sixty lines a second, so only a gap
    // WIDER than the liveness window is written down -- that is the moment the
    // client is about to be told to stop drawing. The first draw of a hover has
    // no previous one and is reported once, as the baseline.
    void previewGap(const std::string& cls, float gapMs, bool lapsed, bool suspended) {
        if (gapMs >= 0.F && gapMs < 250.F && !lapsed && !suspended)
            return;

        say("PREVIEW-GAP  class=%-24s gap=%8.1fms  lapsed=%d  suspended=%d\n", cls.c_str(), gapMs, (int)lapsed, (int)suspended);
    }

    void renderPass() {
        ++g_renderPasses;
        if (g_renderPasses % 600 == 0)
            say("RENDER-REPORT  passes %llu  divergences %llu\n", (unsigned long long)g_renderPasses, (unsigned long long)g_renderDiverged);
    }

    // SHADOW ONLY. The cache said one thing and the freshly computed answer said
    // another, which means the bump set has a hole. Records it once per entry
    // per generation -- see the file header for the two rounds of this that
    // shaped the split.
    void divergence(const std::string& cls, const std::string& title, bool live) {
        ++g_diverged;
        say("DIVERGE generation %llu  live=%s  cached=%s  class=%s  title=%s  last bump=%s\n", (unsigned long long)g_generation, live ? "in" : "out",
            live ? "out" : "in", cls.c_str(), title.c_str(), g_lastBump.c_str());
        if (g_diverged <= 3 || g_diverged % 100 == 0)
            report("at divergence");
    }
}
