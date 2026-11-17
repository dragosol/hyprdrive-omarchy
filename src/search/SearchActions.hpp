#pragma once

// What a result does: the pane-up test and activation.
// Cut from OverviewInternal.hpp by pure code motion. Not standalone: that header
// includes this one at the point the block stood, after everything it reads.

namespace OvInternal {

// Activating a result runs the corresponding app, Omarchy action, agent prompt,
// calculator copy, or file open. Submenu navigation returns false so the overview
// stays open while drilling down into the hierarchy. For an already-open app,
// Enter focuses that window unless newInstance is set (double-tap Shift).
// THE RESULTS PANE IS UP: something is typed, a submenu is open, or -- with
// search:browse_on_click (off by default, user 2026-11-04) -- the pill was
// clicked with nothing typed, which BROWSES: the menu's root, the folder
// shortcuts, the agent help rows. Every "are there results to walk into"
// question asks this one, so the pane and the keys cannot disagree.
inline bool overviewSearchPaneUp() {
    return !g_search.query.text.empty() || (g_search.query.active && Hyprdrive::Config::getSearchBrowseOnClick()) || SearchBackend::canNavigateMenuBack();
}

void overviewQueryEdited(); // main.cpp

inline bool activateSearchResult(int index, bool newInstance = false) {
    const auto RESULTS = overviewSearchRows();
    if (index < 0 || index >= sc<int>(RESULTS.size()))
        return false;

    const auto& ROW = RESULTS[index];

    // A tool row that rewrites the query (AI & Tools "Define" / "Translate"):
    // the panel turns into the tool, and the overview stays up to show it.
    if (ROW.omarchy && ROW.omarchy->kind == SearchBackend::EItemKind::Action && ROW.omarchy->actionCommand.starts_with(OVERVIEW_QUERY_REWRITE)) {
        g_search.query.text             = ROW.omarchy->actionCommand.substr(OVERVIEW_QUERY_REWRITE.size());
        g_search.query.active           = true;
        g_search.nav.zone               = EOverviewNavZone::Search;
        g_search.nav.selection          = -1;
        g_search.nav.explicitSelection  = false;
        g_search.nav.selectFirstPending = false;
        g_search.nav.intent             = ESearchIntent::Typing;
        g_search.nav.hoverIndex         = -1;
        // The rewritten query owes the same trailing work as a typed one --
        // without it no provider (and no lookup) ever saw `define <word>`.
        overviewQueryEdited();
        return false;
    }

    // A status row ("Looking up...") is not a result.
    if (ROW.toolStatus)
        return false;

    if (ROW.omarchy) {
        if (ROW.omarchy->kind == SearchBackend::EItemKind::Menu || ROW.omarchy->kind == SearchBackend::EItemKind::Back) {
            const bool DRILL = ROW.omarchy->kind == SearchBackend::EItemKind::Menu;
            SearchBackend::activate(*ROW.omarchy);
            if (DRILL) {
                g_search.query.text.clear();
                g_search.query.active          = false;
                g_search.nav.zone              = EOverviewNavZone::Result;
                g_search.nav.explicitSelection = true;
                g_search.nav.selectFirstPending = false;
                g_search.nav.intent            = ESearchIntent::Browsing;
                g_search.nav.hoverIndex        = -1;
            }
            // Skip the ‹ Back row so a second Enter launches the first child.
            const auto NEXT = overviewSearchRows();
            int        sel  = 0;
            for (int i = 0; i < sc<int>(NEXT.size()); ++i) {
                if (NEXT[sc<size_t>(i)].column != 0)
                    continue;
                if (NEXT[sc<size_t>(i)].omarchy && NEXT[sc<size_t>(i)].omarchy->kind == SearchBackend::EItemKind::Back)
                    continue;
                sel = i;
                break;
            }
            g_search.nav.selection = sel;
            return false;
        }
        return SearchBackend::activate(*ROW.omarchy);
    } else if (ROW.app) {
        if (!newInstance) {
            if (const auto W = overviewWindowForApp(*ROW.app)) {
                g_search.nav.pendingFocus = W;
                return true;
            }
        }
        AppSearch::launch(*ROW.app, Hyprdrive::Config::getTerminal());
        return true;
    } else {
        SearchProviders::activate(ROW.provider, g_search.query.text);
        return true;
    }
}

} // namespace OvInternal
