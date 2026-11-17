#pragma once

// The window gate's query half: does a window match the typed query (+ its
// per-frame memo).
// Cut from OverviewInternal.hpp by pure code motion. Not standalone: that header
// includes this one at the point the block stood, after everything it reads.

namespace OvInternal {

// OVERVIEW_CPP_SPEC §5 F2, and §13.7 X18's open-window half.
//
// Open windows ARE the window search result: they are already the grid's tiles,
// already promoted from other desktops by B2 and already animated in by F3-b,
// so a second result ROW for a window the user can see would be the same window
// twice in one view. What X18 buys is PARITY OF MATCHING with
// windows-search-provider@G-dH, and the host's own settings were read rather
// than its defaults: `search-method` is 0 = Strict, so the fuzzy app scorer is
// the WRONG algorithm here. Strict is every space-separated term appearing
// somewhere, in any order, case- and accent-insensitively.
//
// Haystack is the extension's own, `${wsIndex + 1}: ${title} ${appName}`, plus
// the window class -- dropping the class would regress the F2 behaviour that
// shipped long before X18.
inline bool overviewSearchMatches(const PHLWINDOW& window) {
    auto& S = searchSession();
    if (S.query.text.empty())
        return true;

    Prof::Timer PROF{Prof::matchNs, Prof::matchCalls};

    // X52 secondary. ~40 call sites consult this gate, so one frame asks the
    // same question about the same window dozens of times -- measured 617-1041
    // calls per frame over a 16-window fixture on the broad query `x`, each one
    // rebuilding the haystack from scratch. The answer cannot change within a
    // frame, so cache it for the frame.
    //
    // The key carries the QUERY as well as the frame counter, not a generation
    // number someone has to remember to bump: a keystroke lands between frames,
    // and the query is 1-3 characters, so comparing it is cheaper than the
    // allocation this skips. Liveness (validMapped) stays OUTSIDE the memo, in
    // shouldShowOverviewWindow, so an unmapped window is never served from here.
    //
    // The three cache fields were function-local statics of this inline
    // function. They are session state -- they are keyed on the query -- so they
    // live with it now; the single-instance property is unchanged, and a header
    // that grows a second definition of this function can no longer grow a
    // second cache with it.
    auto& CACHE = S.query;

    if (CACHE.memoFrame != g_overviewFrame || CACHE.memoQuery != CACHE.text) {
        CACHE.memo.clear();
        CACHE.memoFrame = g_overviewFrame;
        CACHE.memoQuery = CACHE.text;
    } else if (const auto IT = CACHE.memo.find(window.get()); IT != CACHE.memo.end())
        return IT->second;

    // Hyprland numbers regular workspaces from 1, which is what the extension
    // renders as `wsIndex + 1`, so typing "2:" finds desktop 2 on both.
    std::string haystack = window->m_workspace ? std::format("{}: ", window->m_workspace->m_id) : "";
    haystack += window->m_title + " " + window->m_class;

    if (const auto APPNAME = AppSearch::appNameForClass(Hyprdrive::AppIdentity::groupKey(window)); !APPNAME.empty())
        haystack += " " + APPNAME;

    const bool MATCHED       = AppSearch::strictMatch(CACHE.text, haystack);
    CACHE.memo[window.get()] = MATCHED;
    return MATCHED;
}

} // namespace OvInternal
