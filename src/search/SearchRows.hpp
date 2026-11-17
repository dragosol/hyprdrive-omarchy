#pragma once

// The search's rows: the result caps, SSearchRow, the ONE row memo
// (overviewSearchRows), the tool rows, the sections, and the app -> window match.
// Cut from OverviewInternal.hpp by pure code motion. Not standalone: that header
// includes this one at the point the block stood, after everything it reads.

namespace OvInternal {

// OVERVIEW_CPP_SPEC §13.4 G5. TWO LAYOUTS, TWO CAPS, and the caps are PER
// PROVIDER. Apps are a grid and cap at 6; every other provider is a list and
// caps at 5 ROWS EACH, not 5 shared. A shared cap is what made X49's section
// cards invisible: a query matching 5 apps starved every provider, so there was
// only ever one card to categorise.
// The tool pane's metrics (logical px): a header with the headword and its
// source, then one entry per sense -- two lines, the text and an example.
inline constexpr float OVERVIEW_TOOL_PADL = 16.F, OVERVIEW_TOOL_HEADL = 52.F, OVERVIEW_TOOL_ROWHL = 48.F, OVERVIEW_TOOL_GAPL = 4.F;

inline constexpr size_t OVERVIEW_RESULT_ROWS_CROWDED = 6, OVERVIEW_RESULT_ROWS_NORMAL = 8, OVERVIEW_RESULT_ROWS_ALONE = 20;
inline constexpr size_t OVERVIEW_RESULT_TILES_CROWDED = 4;
inline constexpr float  OVERVIEW_RESULT_ROWHL_FULL = 36.F, OVERVIEW_RESULT_CARD_CHROMEL = 56.F;

inline constexpr size_t OVERVIEW_APP_RESULT_LIMIT      = 6;

// X19: the panel shows ONE ordered list. Local app entries first -- they are an
// in-memory scan and always instant -- then whatever the SearchProvider2
// providers have answered with so far. Providers arrive asynchronously, so this
// is deliberately a snapshot: the list grows across frames as replies land, and
// a provider that never answers simply never appears.
// An Action whose actionCommand starts with this is not run: the rest becomes
// the query (the AI & Tools "Define" / "Translate" rows).
inline constexpr std::string_view OVERVIEW_QUERY_REWRITE = "hyprdrive:query:";

struct SSearchRow {
    std::string                  name;
    std::string                  description;
    std::string                  icon;
    std::string                  iconFont;
    const AppSearch::SEntry*     app = nullptr;
    SearchProviders::SResult     provider;
    std::optional<SearchBackend::SSearchItem> omarchy;
    int                          column = 0; // 0 = Left (Apps, Actions, Calc, Agents), 1 = Right (Files, Settings)

    // THE TOOL PANE (ToolSearch.hpp). Non-zero on every row of a `define` /
    // `tr` query, which then owns the whole results container: 1 = dictionary,
    // 2 = translator. name = the sense or translation, description = its tag.
    uint8_t                      tool = 0;
    std::string                  toolExample;  // a usage example, dimmer, under the text
    std::string                  toolTitle;    // row 0 only: the headword / source text
    std::string                  toolSubtitle; // row 0 only: "Wiktionary" / "en → es · MyMemory"
    bool                         toolStatus = false; // a "Looking up…" / "Nothing found" row, not a result
};

inline PHLWINDOW overviewWindowForApp(const AppSearch::SEntry& entry);

inline std::vector<SSearchRow> overviewSearchRowsLive();

// MEMOISED GLOBALLY, not per overview: both outputs show the SAME query, so a
// two-monitor session was building this twice per frame -- and once is already
// a scored sort of the whole app inventory, a walk of every window per app hit,
// two stat()s for the menu, a stat() plus two fs::exists per agent, and a copy
// of the file results out from under a mutex.
//
// Nothing about the SELECTION is in here. The rows are what the query and the
// system are; which one is highlighted is navigation state and lives in
// g_search.nav, which is exactly why it can be left out of the key.
//
// The empty-query browse (the pill focused with nothing typed, with
// search:browse_on_click) is on the per-frame path, and it is the only caller
// of recentFiles(), which stats the user's folder shortcuts and has no
// generation of its own to key on. So that one answer is memoised too, but for
// a second at most: a folder made or removed while the browse is up shows
// within that, and the stats are paid once a second instead of once per frame.
inline std::vector<SSearchRow> overviewSearchRows() {
    auto& S = searchSession();
    if (S.query.windowsOnly)
        return {};

    const std::string& Q           = S.query.text;
    const bool         MENU_BROWSE = SearchBackend::canNavigateMenuBack();
    const bool         ROOT_BROWSE = Q.empty() && !MENU_BROWSE;

    struct SRowMemo {
        std::string query;
        std::string route;
        bool        browse  = false;
        uint64_t    inputs  = 0;
        uint64_t    members = 0;
        uint64_t    apps    = 0;
        bool        filled  = false;

        std::chrono::steady_clock::time_point builtAt{};
        std::vector<SSearchRow>               rows;
    };

    static SRowMemo MEMO;

    const auto NOW   = std::chrono::steady_clock::now();
    const bool FRESH = !ROOT_BROWSE || NOW - MEMO.builtAt < std::chrono::seconds(1);

    const auto     ROUTE   = SearchBackend::menuCurrentRoute();
    const uint64_t INPUTS  = SearchBackend::searchInputsGeneration();
    const uint64_t MEMBERS = Membership::generation();
    const uint64_t APPS    = AppSearch::inventoryGeneration();

    if (MEMO.filled && FRESH && MEMO.query == Q && MEMO.browse == MENU_BROWSE && MEMO.route == ROUTE && MEMO.inputs == INPUTS && MEMO.members == MEMBERS && MEMO.apps == APPS)
        return MEMO.rows;

    MEMO.query   = Q;
    MEMO.route   = ROUTE;
    MEMO.browse  = MENU_BROWSE;
    MEMO.inputs  = INPUTS;
    MEMO.members = MEMBERS;
    MEMO.apps    = APPS;
    MEMO.rows    = overviewSearchRowsLive();
    MEMO.builtAt = NOW;
    MEMO.filled  = true;

    return MEMO.rows;
}

// A `define` / `tr` query's rows: the senses (or the translation and its
// alternatives), each copyable with Enter -- or one status row while the lookup
// is in flight, empty or unreachable.
inline std::vector<SSearchRow> overviewToolRows(const ToolSearch::SQuery& TQ) {
    std::vector<SSearchRow> rows;
    const uint8_t           KIND = TQ.kind == ToolSearch::EKind::Define ? 1 : 2;

    const auto statusRow = [&](const std::string& text, const std::string& title, const std::string& subtitle) {
        SSearchRow r;
        r.name         = text;
        r.column       = 0;
        r.tool         = KIND;
        r.toolStatus   = true;
        r.toolTitle    = title;
        r.toolSubtitle = subtitle;
        rows.push_back(std::move(r));
    };

    if (TQ.term.empty()) {
        statusRow(KIND == 1 ? "Type a word to look up" : "Type something to translate  ·  tr es hello  ·  tr en:fr good morning", KIND == 1 ? "Dictionary" : "Translate", "");
        return rows;
    }

    const auto RES = ToolSearch::lookup(TQ);
    using EState   = ToolSearch::SResult::EState;
    switch (RES.state) {
        case EState::Loading: statusRow(KIND == 1 ? "Looking up…" : "Translating…", RES.title, RES.subtitle); return rows;
        case EState::Empty: statusRow(KIND == 1 ? "No definitions found" : "No translation came back", RES.title, RES.subtitle); return rows;
        case EState::Error:
            statusRow(!RES.message.empty() ? RES.message : (KIND == 1 ? "Could not reach Wiktionary -- check the connection" : "Could not reach the translation service"),
                      RES.title, RES.subtitle);
            return rows;
        case EState::Ready: break;
    }

    for (size_t i = 0; i < RES.senses.size(); ++i) {
        const auto& S = RES.senses[i];
        SSearchRow  r;
        r.name        = S.text;
        r.description = S.tag;
        r.column      = 0;
        r.tool        = KIND;
        r.toolExample = S.example;
        if (i == 0) {
            r.toolTitle    = RES.title;
            r.toolSubtitle = RES.subtitle;
        }

        // Enter copies it: the Calculator row's activation, which is wl-copy of
        // actionCommand with no shell in between.
        SearchBackend::SSearchItem item;
        item.kind          = SearchBackend::EItemKind::Calculator;
        item.id            = "tool:" + std::to_string(i);
        item.name          = S.text;
        item.description   = S.tag;
        item.actionCommand = S.text;
        item.category      = KIND == 1 ? "Dictionary" : "Translate";
        r.omarchy          = item;
        rows.push_back(std::move(r));
    }
    return rows;
}

inline std::vector<SSearchRow> overviewSearchRowsLive() {
    auto& S = searchSession();
    std::vector<SSearchRow> rows;
    Prof::Timer PROF{Prof::rowsNs, Prof::rowsCalls};

    const std::string& Q           = S.query.text;
    const bool         MENU_BROWSE = SearchBackend::canNavigateMenuBack();

    // A `define` / `tr` query owns the whole panel (user, 2026-09-22: "turns the
    // results box into dictionary definitions").
    if (!MENU_BROWSE) {
        if (const auto TQ = ToolSearch::parse(Q); TQ.kind != ToolSearch::EKind::None && ToolSearch::enabled(TQ.kind))
            return overviewToolRows(TQ);
    }

    // ------------------------------------------------------------------------
    // COLUMN 0: OMARCHY MENU & APPS (Super+Space launcher)
    // APPS ALWAYS LEAD. Running ones first, so Enter switches to an open window
    // even when an Omarchy menu row also matches; then apps that are not open
    // yet; then the menu.
    //
    // User: typing "zen" with Zen closed used to put "Zen setup -> Defaults ->
    // Browser" on top, because menu rows were spliced BETWEEN running and
    // unopened apps. Searching an app's name is a request for the app, not for
    // a setting that mentions it -- the setting is still there, one row down.
    // ------------------------------------------------------------------------
    std::vector<SSearchRow> runningApps, menuRows, otherApps;

    if (!Q.empty() && !MENU_BROWSE) {
        for (const auto* E : AppSearch::search(Q, 6)) {
            SSearchRow row{E->name, "", E->icon, "", E, {}, std::nullopt, 0};
            if (overviewWindowForApp(*E))
                runningApps.push_back(std::move(row));
            else
                otherApps.push_back(std::move(row));
        }
    }

    // The BUILDER's cap is deliberately generous: overviewTrimSectionsToFit is
    // the single place that decides how many rows survive, and it sizes that
    // against the room the panel actually has. A second, smaller cap here just
    // made the trim's ceiling unreachable -- the flat 8 below was why a panel
    // with space for fourteen rows still never showed more than eight
    // (user, 2026-09-23).
    const size_t MENU_LIMIT = (Q.empty() || MENU_BROWSE) ? 24 : OVERVIEW_RESULT_ROWS_ALONE;
    for (const auto& item : SearchBackend::searchMenu(Q, MENU_LIMIT))
        menuRows.push_back({item.name, item.description, item.icon, item.iconFont, nullptr, {}, item, 0});

    rows.insert(rows.end(), std::make_move_iterator(runningApps.begin()), std::make_move_iterator(runningApps.end()));
    rows.insert(rows.end(), std::make_move_iterator(otherApps.begin()), std::make_move_iterator(otherApps.end()));
    rows.insert(rows.end(), std::make_move_iterator(menuRows.begin()), std::make_move_iterator(menuRows.end()));

    // ------------------------------------------------------------------------
    // COLUMN 1: FILES & FOLDERS (Omarchy Find Index)
    // ------------------------------------------------------------------------
    if (!MENU_BROWSE) {
        if (Q.empty()) {
            for (const auto& f : SearchBackend::recentFiles()) {
                rows.push_back({f.name, f.description, f.icon, "", nullptr, {}, f, 1});
            }
        } else {
            for (const auto& f : SearchBackend::fileResults()) {
                rows.push_back({f.name, f.description, f.icon, "", nullptr, {}, f, 1});
            }
        }
    }

    // ------------------------------------------------------------------------
    // COLUMN 2: AI AGENTS & TOOLS
    // ------------------------------------------------------------------------
    if (!Q.empty()) {
        if (const auto calc = SearchBackend::evaluateCalculator(Q); calc) {
            rows.push_back({calc->name, calc->description, calc->icon, "", nullptr, {}, *calc, 2});
        }
    }

    if (SearchBackend::isAgentQuery(Q)) {
        for (const auto& item : SearchBackend::searchAgents(Q))
            rows.push_back({item.name, item.description, item.icon, item.iconFont, nullptr, {}, item, 2});
    } else if (!Q.empty() && !MENU_BROWSE) {
        for (const auto& item : SearchBackend::promptAllAgents(Q))
            rows.push_back({item.name, item.description, item.icon, item.iconFont, nullptr, {}, item, 2});

        // The tools, offered for whatever is typed. Activating one rewrites the
        // query into the tool's own syntax, so the panel turns into it.
        const auto toolRow = [&](const std::string& id, const std::string& name, const std::string& icon, const std::string& rewrite) {
            SearchBackend::SSearchItem t;
            t.kind          = SearchBackend::EItemKind::Action;
            t.id            = id;
            t.name          = name;
            t.description   = Q;
            t.icon          = icon;
            t.category      = "Tools";
            t.actionCommand = std::string(OVERVIEW_QUERY_REWRITE) + rewrite;
            rows.push_back({t.name, t.description, t.icon, "", nullptr, {}, t, 2});
        };
        if (ToolSearch::enabled(ToolSearch::EKind::Define) && std::ranges::count(Q, ' ') <= 3)
            toolRow("tool:define", "Define", "accessories-dictionary", "define " + Q);
        if (ToolSearch::enabled(ToolSearch::EKind::Translate))
            toolRow("tool:translate", "Translate", "preferences-desktop-locale", "translate " + Q);

        SearchBackend::SSearchItem webItem;
        webItem.kind          = SearchBackend::EItemKind::Action;
        webItem.id            = "web:google";
        webItem.name          = "Search Google";
        webItem.description   = Q;
        webItem.icon          = "web-browser";
        webItem.category      = "Web";
        std::string qenc;
        for (unsigned char c : Q) {
            if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
                qenc += static_cast<char>(c);
            else if (c == ' ')
                qenc += '+';
            else {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "%%%02X", c);
                qenc += buf;
            }
        }
        webItem.actionCommand = "xdg-open 'https://www.google.com/search?q=" + qenc + "'";
        rows.push_back({webItem.name, webItem.description, webItem.icon, "", nullptr, {}, webItem, 2});
    } else if (!MENU_BROWSE) {
        for (const auto& item : SearchBackend::agentHelpItems())
            rows.push_back({item.name, item.description, item.icon, item.iconFont, nullptr, {}, item, 2});
    }

    return rows;
}

// X49. One card per column: Omarchy Menu (0), Files & Folders (1), AI & Tools (2).
struct SSearchSection {
    std::string         title;
    std::string         icon;
    std::vector<size_t> rows;  // indices into the overviewSearchRows() vector
    size_t              total = 0;
    bool                grid = false;
    int                 column = 0; // 0, 1, or 2
};

inline std::vector<SSearchSection> overviewSearchSections(const std::vector<SSearchRow>& rows) {
    std::vector<SSearchSection> out;
    std::string menuTitle = SearchBackend::menuCurrentRouteTitle();
    out.push_back({menuTitle.empty() ? "Omarchy Menu" : menuTitle, "system-run", {}, 0, false, 0});
    out.push_back({"Files & Folders", "folder", {}, 0, false, 1});
    out.push_back({"AI & Tools", "utilities-terminal", {}, 0, false, 2});

    for (size_t i = 0; i < rows.size(); ++i) {
        int col = std::clamp(rows[i].column, 0, 2);
        out[sc<size_t>(col)].rows.push_back(i);
    }

    for (auto& sec : out) {
        sec.total = sec.rows.size();
    }

    return out;
}

// Mapped window of this desktop entry, if any. Prefer urgent, then a
// non-minimized instance, so Enter switches instead of spawning a duplicate.
inline PHLWINDOW overviewWindowForApp(const AppSearch::SEntry& entry) {
    PHLWINDOW best;
    for (const auto& WINDOW : Desktop::windowState()->windows()) {
        if (!WINDOW || !WINDOW->mapped())
            continue;
        if (WINDOW->isHidden())
            continue;
        if (SearchBackend::isOverviewSushiWindow(WINDOW->metadata().appID(), WINDOW->backend().pid()))
            continue;

        const auto CLASS = Hyprdrive::AppIdentity::groupKey(WINDOW);

        bool match = false;
        if (!entry.wmClass.empty() && CLASS == entry.wmClass)
            match = true;
        else if (AppSearch::idForClass(CLASS) == entry.id)
            match = true;
        else if (CLASS == entry.id)
            match = true;
        if (!match)
            continue;

        if (Hyprdrive::Compat::urgent(WINDOW))
            return WINDOW;
        if (!best)
            best = WINDOW;
        else if (Hyprdrive::Minimize::isMinimized(best) && !Hyprdrive::Minimize::isMinimized(WINDOW))
            best = WINDOW;
    }
    return best;
}
inline int overviewFirstAppIndex(const std::vector<SSearchRow>& rows) {
    int firstCol0 = -1;
    for (size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].column != 0)
            continue;
        if (firstCol0 < 0)
            firstCol0 = sc<int>(i);
        if (rows[i].app && overviewWindowForApp(*rows[i].app))
            return sc<int>(i);
    }
    if (firstCol0 >= 0)
        return firstCol0;
    return rows.empty() ? -1 : 0;
}

} // namespace OvInternal
