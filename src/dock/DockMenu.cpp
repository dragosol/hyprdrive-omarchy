// The context menu: its rows (ERow, buildRows, info rows) and drawing.
#include "IdentityLive.hpp"
#include "DockParts.hpp"
#include "MotionCurves.hpp"

namespace Hyprdrive::Dock {
using namespace DockDetail;
namespace DockDetail {

// Hot reads (Config.hpp): asked for every row of every menu frame.
float menuAnimMs() {
    if (!Config::g_pluginKeysRegistered)
        return 0.F;
    static auto& VAL = Config::valueRef<Hyprlang::INT>("plugin:hyprdrive:dock:menu_anim_ms");
    return std::max(0.F, sc<float>(sc<int>(*VAL)));
}
bool menuAnimOn() {
    if (!Config::g_pluginKeysRegistered)
        return false;
    static auto& VAL = Config::valueRef<Hyprlang::INT>("plugin:hyprdrive:dock:menu_animation");
    return *VAL != 0 && menuAnimMs() > 0.F && Config::motionEnabled();
}
// How open the live menu is, 0..1, eased (no overshoot: user, 2026-11-04).
// Its rows cascade in behind the plate (renderMenuOne).
float menuOpenMs() {
    return menuAnimMs();
}
float menuOpenP(const SMenu& M) {
    if (!menuAnimOn() || M.openedAt.time_since_epoch().count() == 0)
        return 1.F;
    return Motion::ease(Motion::ERole::DECEL, std::clamp(elapsedMs(M.openedAt) / std::max(1.F, menuOpenMs()), 0.F, 1.F));
}
bool menuOpening(const SMenu& M) {
    return menuAnimOn() && M.openedAt.time_since_epoch().count() != 0 && elapsedMs(M.openedAt) < menuOpenMs() + 20.F;
}
// How open the closing ghost still is, 0..1, eased; <= 0 once it is gone.
float menuGhostP(const SMenuGhost& G) {
    if (!menuAnimOn())
        return 0.F;
    const float DUR = menuAnimMs() * 0.75F * std::max(0.05F, G.from); // the exit ~25% shorter, and only what is left of it
    const float T   = std::clamp(elapsedMs(G.closedAt) / std::max(1.F, DUR), 0.F, 1.F);
    return G.from * (1.F - Motion::ease(Motion::ERole::ACCEL, T));
}

// Back off to a lead byte so a truncated title cannot end mid-codepoint and
// hand the text renderer a broken string.
std::string elide(const std::string& text) {
    if (text.size() <= MENU_LABEL_MAX)
        return text;

    size_t cut = MENU_LABEL_MAX;
    while (cut > 0 && (sc<unsigned char>(text[cut]) & 0xC0) == 0x80)
        --cut;

    return text.substr(0, cut) + "…";
}

// V11 V4. What the stack's own store says about it, for the menu labels --
// the row has to name the state it will move TO, so it has to read the state
// it is in.

// Spec §13.9 X28, in the order the spec lists them: .desktop Actions, window
// list, Show Details, Add/Remove from Favorites, Quit.
// A pinned app that is not running has a desktop id and no window class, so
// every lookup keyed on the class came back empty for it.
std::string itemDesktopId(const SItem& item) {
    return !item.desktopId.empty() ? item.desktopId : AppSearch::idForClass(item.wmClass);
}

// Where a window is, as its row's hint says it -- its desktop, the scratchpad,
// or minimized, "\u00b7 current" on the desktop this monitor is showing -- and
// whether Move to This Workspace has anything to do for it.
//
// The ones on the desktop this monitor is showing say so (user, 2026-10-08)
// -- in the hint, the dim words the row already has, so the marker costs no
// new ink: "3 \u00b7 current". A minimized window counts by the desktop it
// came off and comes back to. A scratchpad's is never the current one.
static std::pair<std::string, bool> windowWhere(const PHLWINDOW& WINDOW, const PHLMONITOR& monitor) {
    const auto  ACTIVE   = monitor ? monitor->m_activeWorkspace : PHLWORKSPACE{};
    const auto  ACTIVEID = ACTIVE && !ACTIVE->m_isSpecialWorkspace ? ACTIVE->m_id : WORKSPACE_INVALID;
    std::string where;
    WORKSPACEID on = WORKSPACE_INVALID;
    if (Minimize::isMinimized(WINDOW)) {
        where = "minimized";
        on    = Minimize::originalWorkspace(WINDOW);
    } else if (const auto SCRATCH = Scratchpad::workspaceFor(WINDOW))
        where = Scratchpad::label(SCRATCH);
    else if (WINDOW->m_workspace) {
        where = WINDOW->m_workspace->m_name;
        on    = WINDOW->m_workspace->m_id;
    }
    if (ACTIVEID != WORKSPACE_INVALID && on == ACTIVEID)
        where += " \u00b7 current";
    // Onto the desktop this monitor is showing -- only for a window that is
    // somewhere else (another desktop or a scratchpad). A minimized one is
    // left to Show, which brings it back where it came from.
    return {where, ACTIVEID != WORKSPACE_INVALID && on != ACTIVEID && !Minimize::isMinimized(WINDOW)};
}

// The rows an item's WINDOWS get, shared by an app and a location (the Trash):
// each window with where it is and its own Move to Empty Workspace, then Show
// Windows and whole-item Minimize / Show.
void appendWindowRows(std::vector<SRow>& rows, const std::vector<PHLWINDOW>& WINDOWS, const SItem& item, const PHLMONITOR& monitor) {
    // Each window with WHERE it is: its desktop, the scratchpad, or minimized.
    // A list of identical app names with nothing to tell them apart was the
    // commonest reason to open the hover panel instead (windowWhere).
    size_t minimized = 0;
    for (const auto& WINDOW : WINDOWS) {
        const auto [where, HEREOK] = windowWhere(WINDOW, monitor);
        if (Minimize::isMinimized(WINDOW))
            ++minimized;
        SRow row{.kind = ERow::Window, .label = elide(WINDOW->m_title.empty() ? item.wmClass : WINDOW->m_title), .window = WINDOW, .glyph = MenuGlyph::WINDOW, .hint = where};
        // Its moves ride ITS row, as buttons, so which window they move is
        // never a question (user, 2026-10-08) and a window costs one row, not
        // four (user, 2026-11-03). Always four slots, so the buttons line up
        // down the list; a move that does nothing is left out of its slot.
        const bool STASHOK = !Scratchpad::workspaceFor(WINDOW);
        row.inlines.push_back({.kind = HEREOK ? ERow::MoveHere : ERow::Separator, .glyph = MenuGlyph::HERE, .label = "Move to This Workspace"});
        row.inlines.push_back({.kind = ERow::MoveEmptyWorkspace, .glyph = MenuGlyph::NEW, .label = "Move to Empty Workspace"});
        row.inlines.push_back({.kind = STASHOK ? ERow::MoveScratchpad : ERow::Separator, .glyph = MenuGlyph::STASH, .label = "Move to Scratchpad"});
        // And closing it, last (user, 2026-11-11: "add a close glyph next to
        // the 'move to scratchpad' button"): the client is asked, not killed.
        row.inlines.push_back({.kind = ERow::CloseWindow, .glyph = MenuGlyph::CLOSE, .label = "Close Window"});
        rows.emplace_back(std::move(row));
    }

    // Every window of the app at once, where they can be compared: the overview
    // filtered on the app's name.
    if (!WINDOWS.empty())
        rows.emplace_back(SRow{.kind = ERow::ShowWindows, .label = "Show Windows", .glyph = MenuGlyph::GRID});

    // Whole-app minimize / restore, offered in whichever direction does
    // something. The flight into (and out of) the icon is the dock's own.
    if (!WINDOWS.empty()) {
        if (minimized < WINDOWS.size())
            rows.emplace_back(SRow{.kind = ERow::MinimizeAll, .label = WINDOWS.size() > 1 ? "Minimize All" : "Minimize", .glyph = MenuGlyph::MINIMIZE});
        if (minimized > 0)
            rows.emplace_back(SRow{.kind = ERow::RestoreAll, .label = minimized > 1 ? "Show All" : "Show", .glyph = MenuGlyph::RESTORE});
    }
}

std::vector<SRow> buildRows(const SItem& item, const PHLMONITOR& monitor) {
    std::vector<SRow> rows;

    // X35. A location's menu is its `list_actions()` and nothing else -- no
    // window list, no favourite, no Quit: none of those has an answer for a
    // thing that is not an app. An empty trash lists no action at all, so its
    // menu is a bare plate, which is what the reference shows too.
    // V11 V4. A stack's settings ARE this menu -- the spec is explicit that
    // there is no external prefs panel, which is the reference's one real
    // design mistake (its stack icon has no context menu at all, and a folder
    // can only be unpinned from a GTK preferences window two clicks away).
    if (item.stack) {
        // Dock stacks A: the stack's menu, in eight groups.
        const Stacks::SStack* S = nullptr;
        for (const auto& X : Stacks::all())
            if (X.path == item.stackPath)
                S = &X;
        if (item.stackDir && S) {
            // 1. Recent items: the 5 newest files, with how long ago they arrived.
            auto L = StackReader::listing(S->path);
            std::erase_if(L.entries, [](const StackModel::SEntry& e) { return e.dir; });
            StackModel::sortEntries(L.entries, StackModel::ESort::Added);
            const int64_t NOW = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            for (size_t i = 0; i < std::min<size_t>(5, L.entries.size()); ++i)
                rows.emplace_back(SRow{.kind  = ERow::StackRecent,
                                       .label = elide(L.entries[i].name),
                                       .exec  = L.entries[i].path,
                                       .glyph = MenuGlyph::FILE,
                                       .hint  = StackModel::relativeAge(NOW, L.entries[i].btime)});
            if (!L.entries.empty())
                rows.emplace_back(SRow{.kind = ERow::Separator});

            // 2-4. Sort by / Display as / View as, the current one checked.
            const auto CHECKED = [](bool on) { return on ? std::string{MenuGlyph::CHECK} : std::string{}; };
            static const char* SORTNAMES[] = {"Name", "Date Added", "Date Modified", "Kind"};
            for (const auto K : {StackModel::ESort::Name, StackModel::ESort::Added, StackModel::ESort::Modified, StackModel::ESort::Kind})
                rows.emplace_back(SRow{.kind  = ERow::StackSortKey,
                                       .label = std::string("Sort by ") + SORTNAMES[sc<int>(K)],
                                       .exec  = StackModel::sortName(K),
                                       .glyph = CHECKED(S->sort == K)});
            rows.emplace_back(SRow{.kind = ERow::Separator});
            rows.emplace_back(SRow{.kind = ERow::StackDisplayKey, .label = "Display as Stack", .exec = "stack", .glyph = CHECKED(S->display == StackModel::EDisplay::Stack)});
            rows.emplace_back(SRow{.kind = ERow::StackDisplayKey, .label = "Display as Folder", .exec = "folder", .glyph = CHECKED(S->display == StackModel::EDisplay::Folder)});
            rows.emplace_back(SRow{.kind = ERow::StackViewKey, .label = "View as Automatic", .exec = "auto", .glyph = CHECKED(S->view == StackModel::EView::Auto)});
            rows.emplace_back(SRow{.kind = ERow::StackViewKey, .label = "View as Fan", .exec = "fan", .glyph = CHECKED(S->view == StackModel::EView::Fan)});
            rows.emplace_back(SRow{.kind = ERow::StackViewKey, .label = "View as Grid", .exec = "grid", .glyph = CHECKED(S->view == StackModel::EView::Grid)});
            // VIEW IN FILES (user, 2026-11-05): a click opens the file manager
            // and Super+click the fan; the other views swap the two.
            rows.emplace_back(SRow{.kind = ERow::StackViewKey, .label = "View in Files", .exec = "files", .glyph = CHECKED(S->view == StackModel::EView::Files)});
            rows.emplace_back(SRow{.kind = ERow::Separator});
        }
        // 5-6.
        rows.emplace_back(SRow{.kind = ERow::StackOpen, .label = "Open in Files", .glyph = MenuGlyph::FOLDER});
        rows.emplace_back(SRow{.kind = ERow::StackShowParent, .label = "Show in Files", .glyph = MenuGlyph::FOLDER});
        rows.emplace_back(SRow{.kind = ERow::StackRename, .label = "Rename in Dock\u2026", .glyph = MenuGlyph::PENCIL});
        // 7. Both trash, never delete; in the warning colour.
        if (item.stackDir) {
            rows.emplace_back(SRow{.kind = ERow::Separator});
            rows.emplace_back(SRow{.kind = ERow::StackTrashNewest, .label = "Move Newest to Trash", .glyph = MenuGlyph::TRASH, .danger = true});
            rows.emplace_back(SRow{.kind = ERow::StackEmptyAsk, .label = "Empty Folder\u2026", .glyph = MenuGlyph::TRASH, .danger = true});
        }
        // 8. Get Info (its icon is changed there), and removing a stack never
        // touches the folder.
        rows.emplace_back(SRow{.kind = ERow::Separator});
        if (item.stackDir)
            rows.emplace_back(SRow{.kind = ERow::Details, .label = "Get Info", .glyph = MenuGlyph::DETAILS});
        rows.emplace_back(SRow{.kind = ERow::StackUnpin, .label = "Remove from Dock", .glyph = MenuGlyph::UNPIN});
        return rows;
    }

    if (!item.uri.empty()) {
        if (item.mount) {
            // An fstab point that is not up yet offers the opposite action --
            // and no Eject, because there is nothing mounted to eject.
            if (!item.mounted)
                rows.emplace_back(SRow{.kind = ERow::Mount, .label = "Mount", .glyph = MenuGlyph::DRIVE});
            else {
                rows.emplace_back(SRow{.kind = ERow::Unmount, .label = "Unmount", .glyph = MenuGlyph::DRIVE});
                if (item.canEject)
                    rows.emplace_back(SRow{.kind = ERow::Eject, .label = "Eject", .glyph = MenuGlyph::EJECT});
            }
        } else if (item.uri == "trash:///") {
            // The Trash's menu (user, 2026-10-13: "needs same options as other
            // app icons AND empty trash, greyed out when empty"): what an app's
            // menu offers that means something for a folder -- open it, Get
            // Info -- and Empty Trash, always there, dimmed while there is
            // nothing to empty rather than vanishing, so the menu keeps its shape.
            // Its windows are the file manager's windows showing it, and get
            // the rows an app's windows get. No Force Quit: every Files window
            // is one process, so it would kill the user's other folders too.
            const auto WINDOWS = locationWindows(item);
            rows.emplace_back(SRow{.kind = ERow::StackOpen, .label = WINDOWS.empty() ? "Open" : "New Window", .glyph = WINDOWS.empty() ? MenuGlyph::FOLDER : MenuGlyph::NEW});
            rows.emplace_back(SRow{.kind = ERow::OpenEmptyWorkspace, .label = "Open in Empty Workspace", .glyph = MenuGlyph::NEW});
            rows.emplace_back(SRow{.kind = ERow::OpenScratchpad, .label = "Open in Scratchpad", .glyph = MenuGlyph::STASH});
            if (!WINDOWS.empty())
                rows.emplace_back(SRow{.kind = ERow::Separator});
            appendWindowRows(rows, WINDOWS, item, monitor);
            rows.emplace_back(SRow{.kind = ERow::Separator});
            rows.emplace_back(SRow{.kind = ERow::EmptyTrash, .label = "Empty Trash", .glyph = MenuGlyph::TRASH, .hint = item.trashEmpty ? "empty" : "",
                                   .danger = !item.trashEmpty, .disabled = item.trashEmpty});
            rows.emplace_back(SRow{.kind = ERow::Separator});
            rows.emplace_back(SRow{.kind = ERow::Details, .label = "Get Info", .glyph = MenuGlyph::DETAILS});
            if (!WINDOWS.empty()) {
                rows.emplace_back(SRow{.kind = ERow::Separator});
                rows.emplace_back(SRow{.kind = ERow::Quit, .label = WINDOWS.size() > 1 ? "Close " + std::to_string(WINDOWS.size()) + " Windows" : "Close Window", .glyph = MenuGlyph::QUIT});
            }
        }

        return rows;
    }

    const auto& wmClass = item.wmClass;
    const auto  WINDOWS = itemWindows(item);
    const auto  ID      = itemDesktopId(item);

    // An app that declares its OWN new-window action (Zen, Ghostty, Nautilus,
    // Chromium, Evince, Showtime here: `[Desktop Action new-window]`, Name=New
    // Window) listed it a second time under the dock's New Window (user,
    // 2026-10-08: "why do I have two 'new window' context entries?"). The app's
    // one wins: it is the app's own word for a new window (`zen --new-window`,
    // where the dock's would re-run the main Exec), and it is there whether or
    // not the app is running. It takes the dock's row's place at the top.
    auto       actions   = AppSearch::actionsForId(ID);
    const auto NEWWINDOW = std::ranges::find_if(actions, [](const AppSearch::SAction& a) {
        const auto LOWER = [](std::string s) {
            std::ranges::transform(s, s.begin(), [](unsigned char c) { return sc<char>(std::tolower(c)); });
            return s;
        };
        return LOWER(a.id) == "new-window" || LOWER(a.name) == "new window";
    });
    if (NEWWINDOW != actions.end()) {
        rows.emplace_back(SRow{.kind = ERow::Action, .label = NEWWINDOW->name, .exec = NEWWINDOW->exec, .glyph = MenuGlyph::NEW});
        actions.erase(NEWWINDOW);
    }
    // New Window first when there is something to launch: the one thing a
    // running app's icon cannot do on a click, which focuses instead.
    else if (!ID.empty() && !WINDOWS.empty())
        rows.emplace_back(SRow{.kind = ERow::NewWindow, .label = "New Window", .glyph = MenuGlyph::NEW});
    // Running or not: a new window of it, alone on a desktop of its own.
    if (!ID.empty()) {
        rows.emplace_back(SRow{.kind = ERow::OpenEmptyWorkspace, .label = "Open in Empty Workspace", .glyph = MenuGlyph::NEW});
        // ALWAYS OPEN IN, as two chips on one row (user, 2026-11-11: "space
        // saving"): each its own toggle, lit while it is on; both may be.
        SRow ALWAYS{.kind = ERow::AlwaysOpenIn, .label = "Always Open in", .segmented = true};
        ALWAYS.inlines.push_back({.kind = ERow::AlwaysEmptyWorkspace, .label = "Empty Workspace", .on = opensInEmptyDesktop(ID)});
        ALWAYS.inlines.push_back({.kind = ERow::AlwaysFullscreen, .label = "Fullscreen", .on = opensInFullscreen(ID)});
        rows.emplace_back(std::move(ALWAYS));
        rows.emplace_back(SRow{.kind = ERow::OpenScratchpad, .label = "Open in Scratchpad", .glyph = MenuGlyph::STASH});
    }

    for (const auto& ACTION : actions)
        rows.emplace_back(SRow{.kind = ERow::Action, .label = ACTION.name, .exec = ACTION.exec, .glyph = MenuGlyph::ACTION});

    if (!rows.empty() && !WINDOWS.empty())
        rows.emplace_back(SRow{.kind = ERow::Separator});

    appendWindowRows(rows, WINDOWS, item, monitor);

    // MAXIMIZE | FULLSCREEN, one split row (user, 2026-11-11), for the window a
    // click on the icon would bring up: the app's most recently focused one
    // that is out on a desktop. Each half names what it will do -- a window
    // already maximized (fullscreen) lights that half and offers to undo it.
    {
        std::vector<PHLWINDOW> out;
        for (const auto& W : WINDOWS)
            if (W && !Minimize::isMinimized(W) && !Scratchpad::workspaceFor(W))
                out.push_back(W);
        if (const auto TARGET = out.empty() ? PHLWINDOW{} : recentWindow(out)) {
            const auto& FS  = ::Fullscreen::controller();
            const bool  MAX = FS && FS->isFullscreen(TARGET, ::Fullscreen::FSMODE_MAXIMIZED);
            const bool  FUL = FS && FS->isFullscreen(TARGET, ::Fullscreen::FSMODE_FULLSCREEN);
            SRow        SIZE{.kind = ERow::WindowSize, .window = TARGET, .segmented = true};
            SIZE.inlines.push_back({.kind = ERow::Maximize, .glyph = MAX ? MenuGlyph::RESTORE : MenuGlyph::WINDOW, .label = MAX ? "Restore" : "Maximize", .on = MAX});
            SIZE.inlines.push_back({.kind = ERow::Fullscreen, .glyph = FUL ? MenuGlyph::COMPRESS : MenuGlyph::EXPAND, .label = FUL ? "Exit Fullscreen" : "Fullscreen", .on = FUL});
            rows.emplace_back(std::move(SIZE));
        }
    }

    // A class no desktop entry claims has nothing to show the details of and
    // nothing to favourite by id, so both rows are left out rather than drawn
    // dead. GNOME does the same with an unmatched window.
    if (!ID.empty()) {
        if (!rows.empty())
            rows.emplace_back(SRow{.kind = ERow::Separator});

        const bool FAV = std::ranges::any_of(favorites(), [&](const std::string& F) { return Hyprdrive::AppIdentity::sameApp(F, ID); });
        rows.emplace_back(SRow{.kind = ERow::Favorite, .label = FAV ? "Remove from Favorites" : "Add to Favorites", .glyph = FAV ? MenuGlyph::FAV_DEL : MenuGlyph::FAV_ADD});
        rows.emplace_back(SRow{.kind = ERow::Details, .label = "Get Info", .glyph = MenuGlyph::DETAILS});
    }

    if (!WINDOWS.empty()) {
        if (!rows.empty())
            rows.emplace_back(SRow{.kind = ERow::Separator});
        rows.emplace_back(SRow{.kind = ERow::Quit, .label = WINDOWS.size() > 1 ? "Quit " + std::to_string(WINDOWS.size()) + " Windows" : "Quit", .glyph = MenuGlyph::QUIT});
        // The app asked nicely and did not go (a hung client, a modal it is
        // stuck behind): SIGKILL every process behind its windows. Last, and in
        // the warning colour, because it loses unsaved work.
        rows.emplace_back(SRow{.kind = ERow::ForceQuit, .label = "Force Quit", .glyph = MenuGlyph::KILL, .danger = true});
    }

    return rows;
}

// THE WINDOW'S OWN MENU (user, 2026-11-11: "a context menu for window previews
// too, similar to the app context menu but with functions limited to that
// window including closing it"). The app menu's rows for one window, in the
// order MenuModel::windowMenuRows gives -- every row the same ERow the app
// menu's row or button is, so each acts through activateRow exactly as there.
// Nothing app-wide: no New Window, Always Open in, favourites or Quit.
std::vector<SRow> buildWindowRows(const SItem& item, const PHLWINDOW& window, const PHLMONITOR& monitor) {
    std::vector<SRow> rows;
    if (!window)
        return rows;
    const auto [where, HEREOK] = windowWhere(window, monitor);
    const bool MINIMIZED       = Minimize::isMinimized(window);
    const bool SCRATCH         = Scratchpad::workspaceFor(window) != nullptr;
    const auto& FS             = ::Fullscreen::controller();
    for (const auto K : MenuModel::windowMenuRows({.elsewhere = HEREOK, .minimized = MINIMIZED, .scratchpad = SCRATCH})) {
        using W = MenuModel::EWinRow;
        switch (K) {
            case W::Header:
                rows.emplace_back(SRow{.kind = ERow::Window, .label = elide(window->m_title.empty() ? item.wmClass : window->m_title), .window = window, .glyph = MenuGlyph::WINDOW,
                                       .hint = where});
                break;
            case W::Separator: rows.emplace_back(SRow{.kind = ERow::Separator}); break;
            case W::MoveHere: rows.emplace_back(SRow{.kind = ERow::MoveHere, .label = "Move to This Workspace", .window = window, .glyph = MenuGlyph::HERE}); break;
            case W::MoveEmptyWorkspace:
                rows.emplace_back(SRow{.kind = ERow::MoveEmptyWorkspace, .label = "Move to Empty Workspace", .window = window, .glyph = MenuGlyph::NEW});
                break;
            case W::MoveScratchpad: rows.emplace_back(SRow{.kind = ERow::MoveScratchpad, .label = "Move to Scratchpad", .window = window, .glyph = MenuGlyph::STASH}); break;
            case W::Size: {
                const bool MAX = FS && FS->isFullscreen(window, ::Fullscreen::FSMODE_MAXIMIZED);
                const bool FUL = FS && FS->isFullscreen(window, ::Fullscreen::FSMODE_FULLSCREEN);
                SRow       SIZE{.kind = ERow::WindowSize, .window = window, .segmented = true};
                SIZE.inlines.push_back({.kind = ERow::Maximize, .glyph = MAX ? MenuGlyph::RESTORE : MenuGlyph::WINDOW, .label = MAX ? "Restore" : "Maximize", .on = MAX});
                SIZE.inlines.push_back({.kind = ERow::Fullscreen, .glyph = FUL ? MenuGlyph::COMPRESS : MenuGlyph::EXPAND, .label = FUL ? "Exit Fullscreen" : "Fullscreen", .on = FUL});
                rows.emplace_back(std::move(SIZE));
                break;
            }
            case W::Minimize:
                rows.emplace_back(SRow{.kind = ERow::MinimizeAll, .label = "Minimize", .window = window, .glyph = MenuGlyph::MINIMIZE, .oneWindow = true});
                break;
            case W::Show: rows.emplace_back(SRow{.kind = ERow::RestoreAll, .label = "Show", .window = window, .glyph = MenuGlyph::RESTORE, .oneWindow = true}); break;
            case W::Close: rows.emplace_back(SRow{.kind = ERow::CloseWindow, .label = "Close Window", .window = window, .glyph = MenuGlyph::CLOSE}); break;
        }
    }
    return rows;
}

// GET INFO's rows: the .desktop entry's own answers straight away, the package
// manager's as soon as AppInfo has them. Each value row carries the WHOLE value
// in `exec` (the hint is elided to fit) so a click copies all of it.
// The Trash's Get Info: what is in it and where it lives. Counted from the top
// level of Trash/files -- one entry per thing trashed, however deep a trashed
// folder is -- so opening the view never walks a large trash.
static std::vector<SRow> trashInfoRows() {
    std::vector<SRow> rows;
    std::string       data;
    if (const char* X = std::getenv("XDG_DATA_HOME"); X && *X)
        data = X;
    else if (const char* HOME = std::getenv("HOME"); HOME && *HOME)
        data = std::string(HOME) + "/.local/share";
    const std::string DIR = data + "/Trash";
    const size_t      n   = Locations::trashCount(); // every disk's trash, not only this one
    std::string shown = DIR;
    if (const char* HOME = std::getenv("HOME"); HOME && *HOME && shown.starts_with(HOME))
        shown = "~" + shown.substr(std::strlen(HOME));
    rows.emplace_back(SRow{.kind = ERow::InfoLine, .label = "Name", .exec = "Trash", .glyph = MenuGlyph::TAG, .hint = "Trash"});
    const std::string COUNT = n == 0 ? "Empty" : std::to_string(n) + (n == 1 ? " item" : " items");
    rows.emplace_back(SRow{.kind = ERow::InfoLine, .label = "Contents", .exec = COUNT, .glyph = MenuGlyph::TRASH, .hint = COUNT});
    rows.emplace_back(SRow{.kind = ERow::InfoLine, .label = "Location", .exec = DIR, .glyph = MenuGlyph::FOLDER, .hint = shown});
    rows.emplace_back(SRow{.kind = ERow::Separator});
    rows.emplace_back(SRow{.kind = ERow::InfoReveal, .label = "Show in Files", .exec = DIR + "/files", .glyph = MenuGlyph::FOLDER});
    rows.emplace_back(SRow{.kind = ERow::InfoCopyAll, .label = "Copy Info", .glyph = MenuGlyph::COPY});
    rows.emplace_back(SRow{.kind = ERow::Separator});
    rows.emplace_back(SRow{.kind = ERow::InfoBack, .label = "Back", .glyph = MenuGlyph::BACK});
    return rows;
}

// A dock folder's Get Info: what it is, where, how full -- counted from its
// cached listing, never walked -- and its icon, changed in the settings'
// Icons -> Folders (user, 2026-11-05).
static std::vector<SRow> folderInfoRows(const SItem& item) {
    std::vector<SRow> rows;
    const auto&       PATH  = item.stackPath;
    const std::string NAME  = item.locationName.empty() ? std::filesystem::path(PATH).filename().string() : item.locationName;
    std::string       shown = PATH;
    if (const char* HOME = std::getenv("HOME"); HOME && *HOME && shown.starts_with(HOME))
        shown = "~" + shown.substr(std::strlen(HOME));
    if (shown.size() > 46)
        shown = "\u2026" + shown.substr(shown.size() - 45);
    const auto        L     = StackReader::listing(PATH);
    const size_t      N     = L.entries.size();
    const std::string COUNT = N == 0 ? "Empty" : std::to_string(N) + (N == 1 ? " item" : " items");
    rows.emplace_back(SRow{.kind = ERow::InfoLine, .label = "Name", .exec = NAME, .glyph = MenuGlyph::TAG, .hint = elide(NAME)});
    rows.emplace_back(SRow{.kind = ERow::InfoLine, .label = "Contents", .exec = COUNT, .glyph = MenuGlyph::FILE, .hint = COUNT});
    rows.emplace_back(SRow{.kind = ERow::InfoLine, .label = "Location", .exec = PATH, .glyph = MenuGlyph::FOLDER, .hint = shown});
    rows.emplace_back(SRow{.kind = ERow::Separator});
    const auto ID     = IconOverrides::folderId(PATH);
    const bool CUSTOM = IconOverrides::forId(ID) != nullptr;
    rows.emplace_back(SRow{.kind = ERow::InfoIcon, .label = "Folder Icon", .exec = ID, .glyph = MenuGlyph::IMAGE, .hint = std::string(CUSTOM ? "Custom  " : "Change  ") + "\uf054"});
    rows.emplace_back(SRow{.kind = ERow::InfoReveal, .label = "Show in Files", .exec = PATH, .glyph = MenuGlyph::FOLDER});
    rows.emplace_back(SRow{.kind = ERow::InfoCopyAll, .label = "Copy Info", .glyph = MenuGlyph::COPY});
    rows.emplace_back(SRow{.kind = ERow::Separator});
    rows.emplace_back(SRow{.kind = ERow::InfoBack, .label = "Back", .glyph = MenuGlyph::BACK});
    return rows;
}

std::vector<SRow> infoRows(const SItem& item) {
    if (item.uri == "trash:///")
        return trashInfoRows();
    if (item.stack && item.stackDir)
        return folderInfoRows(item);
    std::vector<SRow> rows;
    const auto*       ENTRY = AppSearch::entryForId(itemDesktopId(item));

    const auto line = [&](const char* glyph, const std::string& key, std::string value) {
        if (value.empty())
            return;
        // Paths read from home, the way a person would say them.
        if (const char* HOME = std::getenv("HOME"); HOME && *HOME && value.starts_with(HOME))
            value = "~" + value.substr(std::strlen(HOME));
        std::string shown = value;
        if (shown.size() > 46) {
            size_t cut = 45;
            while (cut > 0 && (sc<unsigned char>(shown[cut]) & 0xC0) == 0x80)
                --cut;
            shown = shown.substr(0, cut) + "…";
        }
        rows.emplace_back(SRow{.kind = ERow::InfoLine, .label = key, .exec = value, .glyph = glyph, .hint = shown});
    };

    if (ENTRY) {
        line(MenuGlyph::TAG, "Name", ENTRY->name);
        line(MenuGlyph::TEXT, "About", ENTRY->comment.empty() ? ENTRY->generic : ENTRY->comment);
    } else if (const auto* ID = Hyprdrive::AppIdentity::forKey(item.wmClass); ID && item.wmClass.starts_with("hd:"))
        line(MenuGlyph::TAG, "Name", ID->displayName);
    else
        line(MenuGlyph::TAG, "Class", item.wmClass);
    // A Wine, Proton, Steam or Lutris app (AppIdentity): what it is and where.
    if (const auto* ID = item.wmClass.starts_with("hd:") ? Hyprdrive::AppIdentity::forKey(item.wmClass) : nullptr) {
        line(MenuGlyph::SOURCE, "Kind", std::string{Hyprdrive::AppIdentity::familyName(ID->family)});
        if (!ID->exePath.empty())
            line(MenuGlyph::DISK, "Program", ID->exePath);
        if (!ID->prefix.empty())
            line(MenuGlyph::DISK, "Prefix", ID->prefix);
    }

    rows.emplace_back(SRow{.kind = ERow::Separator});

    const auto INFO = ENTRY ? AppInfo::lookup(ENTRY->path, ENTRY->exec) : AppInfo::SInfo{.ready = true, .fields = {}};
    if (!INFO.ready)
        rows.emplace_back(SRow{.kind = ERow::InfoLine, .label = "Looking up the package…", .glyph = MenuGlyph::PACKAGE});
    for (const auto& [KEY, VALUE] : INFO.fields) {
        const char* G = KEY == "Version" ? MenuGlyph::TAG :
            KEY == "Source"              ? MenuGlyph::SOURCE :
            KEY == "Package"             ? MenuGlyph::PACKAGE :
            KEY == "Size"                ? MenuGlyph::DISK :
            KEY == "Installed"           ? MenuGlyph::CLOCK :
                                           MenuGlyph::LINK;
        line(G, KEY, VALUE);
    }

    if (ENTRY) {
        // Exec= without its field codes (%U, %f, ...), which are placeholders,
        // not part of the command anyone would type.
        std::string cmd;
        for (size_t i = 0; i < ENTRY->exec.size(); ++i) {
            if (ENTRY->exec[i] == '%' && i + 1 < ENTRY->exec.size()) {
                ++i;
                continue;
            }
            cmd += ENTRY->exec[i];
        }
        while (!cmd.empty() && cmd.back() == ' ')
            cmd.pop_back();
        line(MenuGlyph::TERMINAL, "Command", cmd);
        line(MenuGlyph::FILE, "Desktop file", ENTRY->path);
    }

    // Clickable, unlike the lines around it: it opens the windows it counts.
    if (const auto N = itemWindows(item).size(); N > 0)
        rows.emplace_back(SRow{.kind = ERow::ShowWindows, .label = "Open windows", .glyph = MenuGlyph::WINDOW, .hint = std::to_string(N) + "  \uf054"});

    rows.emplace_back(SRow{.kind = ERow::Separator});
    // App Icon (user, 2026-10-13): change it right here rather than hunting
    // for the app in the settings panel. Only an app with a desktop id can
    // have an override -- that is what the store is keyed on.
    if (ENTRY && !itemDesktopId(item).empty()) {
        const bool CUSTOM = IconOverrides::forId(itemDesktopId(item)) != nullptr;
        rows.emplace_back(SRow{.kind = ERow::InfoIcon, .label = "App Icon", .exec = itemDesktopId(item), .glyph = MenuGlyph::IMAGE,
                               .hint = std::string(CUSTOM ? "Custom  " : "Change  ") + "\uf054"});
    }
    if (ENTRY)
        rows.emplace_back(SRow{.kind = ERow::InfoReveal, .label = "Show in Files", .exec = ENTRY->path, .glyph = MenuGlyph::FOLDER});
    rows.emplace_back(SRow{.kind = ERow::InfoCopyAll, .label = "Copy Info", .glyph = MenuGlyph::COPY});
    rows.emplace_back(SRow{.kind = ERow::Separator});
    rows.emplace_back(SRow{.kind = ERow::InfoBack, .label = "Back", .glyph = MenuGlyph::BACK});
    return rows;
}

// Lays the rows out AND draws them: a row's height is its text's height, and
// the only way to learn that is to build the texture. Doing both here is what
// keeps the hit-test boxes and the pixels the user sees the same numbers.
void renderMenuOne(PHLMONITOR monitor, const SLayout& layout, SMenu& M, float OPENP) {
    if (!monitor || M.monitor != monitor->m_id)
        return;

    // The package lookup landed: the info rows grow their answers.
    if (M.infoView && AppInfo::generation() != M.infoGen) {
        M.infoGen = AppInfo::generation();
        M.rows    = infoRows(M.item);
    }

    const bool LIVE = g_menu && &*g_menu == &M; // not the closing ghost

    // A window of a row closed (onWindowClose): the rows again, without it.
    // The boxes carry over by index, so the hover is tested this frame against
    // where the rows were, and the row that moves up under the pointer lights
    // rather than the whole menu blinking unlit for a frame.
    if (M.stale && LIVE && !M.infoView && !M.editing) {
        M.stale   = false;
        auto rows = buildRows(M.item, monitor);
        for (size_t i = 0; i < rows.size() && i < M.rows.size(); ++i) {
            rows[i].box = M.rows[i].box;
            if (rows[i].inlines.size() == M.rows[i].inlines.size())
                for (size_t b = 0; b < rows[i].inlines.size(); ++b)
                    rows[i].inlines[b].box = M.rows[i].inlines[b].box;
        }
        M.rows = std::move(rows);
        if (std::ranges::none_of(M.rows, [](const SRow& r) { return r.kind == ERow::Window; })) {
            // The app's last window went with the menu still up: as after Quit.
            closeMenu();
            return;
        }
    }

    const float SCALE  = std::max(monitor->m_scale, 0.01F);
    const int   FONTPX = std::max(1, sc<int>(std::round(MENU_FONT * SCALE)));

    // The warning colour for Force Quit: a desaturated red that still reads as
    // part of a dark Omarchy plate rather than as an alert box.
    // The THEME's text colour, not white: on a light Omarchy theme the plate is
    // light and white labels vanished (user, 2026-09-29). Dim = the same ink,
    // thinner; the danger red darkens on a light plate to stay readable.
    const bool       LIGHT = Hyprdrive::Config::isLightTheme();
    const auto       INK   = Hyprdrive::Config::themeForeground();
    const CHyprColor LABEL_COLOR{INK.r, INK.g, INK.b, 0.95F};
    const CHyprColor DANGER_COLOR = LIGHT ? CHyprColor{0.78F, 0.16F, 0.14F, 1.F} : CHyprColor{1.F, 0.50F, 0.47F, 0.97F};
    const CHyprColor DIM_COLOR{INK.r, INK.g, INK.b, LIGHT ? 0.60F : 0.45F};

    struct SRowTex {
        SP<Render::ITexture> label, glyph, hint;
        std::vector<SP<Render::ITexture>> inlineGlyphs;
        // A segmented row: each segment's label and glyph, and the line height
        // the row is laid out on (its label's, or its first segment's).
        std::vector<SP<Render::ITexture>> segLabels, segGlyphs;
        float                             lineH = 0.F;
    };
    std::vector<SRowTex> textures;
    textures.reserve(M.rows.size());

    // One glyph column for the whole plate, so every label starts at the same x
    // -- present only when some row has a glyph.
    const bool  GLYPHS    = std::ranges::any_of(M.rows, [](const SRow& r) { return !r.glyph.empty(); });
    const float GLYPHCOLW = GLYPHS ? std::round(sc<float>(FONTPX) * 1.9F) : 0.F;
    const float HINTGAP   = std::round(18.F * SCALE);

    // SEGMENTED ROWS (Maximize | Fullscreen, Always Open in's chips): a
    // segment's plate sits SEG_EDGE in from the plate's side, so its radius is
    // the plate's less that -- concentric where it meets the corner -- and its
    // content SEG_PAD in from its own sides.
    const float SEG_EDGE   = std::round(4.F * SCALE);
    const float SEG_GAP    = std::round(4.F * SCALE);
    const float SEG_PAD    = std::round(10.F * SCALE);
    const float SEG_GLYPHG = std::round(6.F * SCALE);
    const float SEG_INSETY = std::round(3.F * SCALE);
    const int   SEG_RADIUS = std::max(0, sc<int>(std::round(MENU_RADIUS * SCALE - SEG_EDGE)));
    const auto  segContentW = [&](const auto& T, size_t b) {
        const float GW = b < T.segGlyphs.size() && T.segGlyphs[b] && T.segGlyphs[b]->ok() ? sc<float>(T.segGlyphs[b]->m_size.x) + SEG_GLYPHG : 0.F;
        const float LW = b < T.segLabels.size() && T.segLabels[b] && T.segLabels[b]->ok() ? sc<float>(T.segLabels[b]->m_size.x) : 0.F;
        return GW + LW;
    };

    float width  = MENU_MIN_W * SCALE;
    float height = 0.F;
    for (const auto& ROW : M.rows) {
        if (ROW.kind == ERow::Separator) {
            textures.emplace_back();
            height += MENU_SEP_H * SCALE;
            continue;
        }

        if (ROW.segmented) {
            SRowTex t;
            if (!ROW.label.empty())
                t.label = OverviewRender::text(ROW.label, DIM_COLOR, FONTPX, false, "", 0, 500);
            for (const auto& B : ROW.inlines) {
                const bool CHIP = !ROW.label.empty();
                t.segLabels.push_back(OverviewRender::text(B.label, CHIP && !B.on ? DIM_COLOR : LABEL_COLOR, FONTPX, false, "", 0, 500));
                t.segGlyphs.push_back(B.glyph.empty() ? SP<Render::ITexture>{} :
                                                        OverviewRender::text(B.glyph, B.on ? LABEL_COLOR : DIM_COLOR.modifyA(0.8F), FONTPX, false, "JetBrainsMono Nerd Font", 0, 400));
            }
            const auto FIRST = t.label && t.label->ok() ? t.label : !t.segLabels.empty() ? t.segLabels.front() : SP<Render::ITexture>{};
            if (!FIRST || !FIRST->ok()) {
                textures.emplace_back();
                height += MENU_SEP_H * SCALE;
                continue;
            }
            t.lineH         = sc<float>(FIRST->m_size.y);
            float segs      = 0.F, widest = 0.F;
            const size_t NS = ROW.inlines.size();
            for (size_t b = 0; b < NS; ++b) {
                segs += segContentW(t, b) + 2.F * SEG_PAD;
                widest = std::max(widest, segContentW(t, b) + 2.F * SEG_PAD);
            }
            segs += SEG_GAP * sc<float>(NS > 0 ? NS - 1 : 0);
            if (t.label && t.label->ok()) // label, then the chips at the right end
                width = std::max(width, MENU_PAD_X * SCALE + GLYPHCOLW + sc<float>(t.label->m_size.x) + HINTGAP + segs + SEG_EDGE);
            else // equal halves across the row
                width = std::max(width, 2.F * SEG_EDGE + widest * sc<float>(NS) + SEG_GAP * sc<float>(NS > 0 ? NS - 1 : 0));
            height += t.lineH + 2.F * MENU_ROW_PAD_Y * SCALE;
            textures.emplace_back(std::move(t));
            continue;
        }

        SRowTex t;
        t.label = OverviewRender::text(ROW.label, ROW.disabled ? DIM_COLOR.modifyA(0.6F) : ROW.danger ? DANGER_COLOR : LABEL_COLOR, FONTPX, false, "", 0, 500);
        if (!t.label || !t.label->ok()) {
            textures.emplace_back();
            height += MENU_SEP_H * SCALE;
            continue;
        }
        if (!ROW.glyph.empty())
            t.glyph = OverviewRender::text(ROW.glyph, ROW.disabled ? DIM_COLOR.modifyA(0.45F) : ROW.danger ? DANGER_COLOR : DIM_COLOR.modifyA(0.8F), FONTPX, false, "JetBrainsMono Nerd Font", 0, 400);
        if (!ROW.hint.empty())
            t.hint = OverviewRender::text(ROW.hint, DIM_COLOR, FONTPX, false, "", 0, 400);

        float HINTW = t.hint && t.hint->ok() ? HINTGAP + sc<float>(t.hint->m_size.x) : 0.F;
        float BTNSW = 0.F;
        if (!ROW.inlines.empty()) {
            const float BTN = sc<float>(t.label->m_size.y) + 2.F * MENU_ROW_PAD_Y * SCALE;
            BTNSW           = BTN * sc<float>(ROW.inlines.size()) + HINTGAP * 0.5F;
            // Room for the longest button name, which takes the hint's place
            // while hovered: the plate must not change width under the pointer.
            for (const auto& B : ROW.inlines) {
                t.inlineGlyphs.push_back(B.kind == ERow::Separator ? SP<Render::ITexture>{} :
                                                                     OverviewRender::text(B.glyph, DIM_COLOR.modifyA(0.85F), FONTPX, false, "JetBrainsMono Nerd Font", 0, 400));
                if (const auto L = OverviewRender::text(B.label, DIM_COLOR, FONTPX, false, "", 0, 400); L && L->ok())
                    HINTW = std::max(HINTW, HINTGAP + sc<float>(L->m_size.x));
            }
        }
        const float SUBW  = ROW.sub ? GLYPHCOLW : 0.F;
        width             = std::max(width, 2.F * MENU_PAD_X * SCALE + SUBW + GLYPHCOLW + sc<float>(t.label->m_size.x) + HINTW + BTNSW);
        height += sc<float>(t.label->m_size.y) + 2.F * MENU_ROW_PAD_Y * SCALE;
        textures.emplace_back(std::move(t));
    }

    if (height < 1.F)
        return;

    // NEVER OFF THE SCREEN (user, 2026-11-03). Taller than the room above the
    // dock, the window list scrolls inside the plate, a row at a time, between
    // two chevron strips; the rows above and below it stay put.
    const double EDGEPX  = std::round(MENU_EDGE_MARGIN * SCALE);
    // Where the plate's bottom edge sits: MENU_GAP over the pill, or -- a
    // preview's menu -- over the pane it came from, following the pane while
    // it reflows (a window of it closing). The ghost keeps where it was.
    if (M.windowScope && LIVE && panelOn(monitor) && !g_hover.panel->out && g_hover.drawn.height >= 1)
        M.bottom = sc<double>(g_hover.drawn.y) - MENU_GAP * SCALE;
    if (M.windowScope && M.bottom < 0.0)
        M.bottom = sc<double>(M.anchor.y) - MENU_GAP * SCALE;
    const float  BOTTOM  = M.windowScope ? sc<float>(M.bottom) : sc<float>(layout.pill.y) - MENU_GAP * SCALE;
    const float  ROOM    = BOTTOM - sc<float>(EDGEPX);
    const float  CHEV    = std::round(MENU_SEP_H * SCALE * 1.6F);
    int          segFirst = -1, segLast = -1;
    for (size_t i = 0; i < M.rows.size(); ++i) {
        if (M.rows[i].kind == ERow::Window && textures[i].label) {
            if (segFirst < 0)
                segFirst = sc<int>(i);
            segLast = sc<int>(i);
        }
    }
    int  visibleRows = 0;
    bool scrolling   = false;
    if (height > ROOM && segFirst >= 0) {
        const float WINH  = sc<float>(textures[segFirst].label->m_size.y) + 2.F * MENU_ROW_PAD_Y * SCALE;
        const int   N     = segLast - segFirst + 1;
        const float OTHER = height - WINH * sc<float>(N);
        visibleRows       = std::clamp(sc<int>(std::floor((ROOM - OTHER - 2.F * CHEV) / WINH)), 1, N);
        if (visibleRows < N) {
            scrolling           = true;
            M.scrollTop   = std::clamp(M.scrollTop, 0, N - visibleRows);
            height              = OTHER + 2.F * CHEV + WINH * sc<float>(visibleRows);
        }
    }
    if (!scrolling)
        M.scrollTop = 0;

    const float CX = sc<float>(M.anchor.x) + sc<float>(M.anchor.width) / 2.F;
    CBox        plate{std::round(CX - width / 2.F), std::round(BOTTOM - height), std::round(width), std::round(height)};
    // Wholly on screen, border and all, with room to breathe: clamped flush to
    // 0 the plate's left border sat ON the screen edge and half of it was cut
    // away (user, 2026-09-29, an app far left on the dock).
    const double EDGE = std::round(MENU_EDGE_MARGIN * SCALE);
    plate.x           = std::clamp(plate.x, EDGE, std::max(EDGE, monitor->m_size.x * SCALE - plate.width - EDGE));
    plate.y           = std::max(EDGE, plate.y);

    M.box = plate;

    // The part of the plate open so far (OPENP: 0 = only the icon, 1 = all of
    // it), and everything drawn below is clipped to it and fades with it.
    const float  OP      = std::clamp(OPENP, 0.F, 1.F);
    const double STARTW  = std::min(plate.width, std::max(sc<double>(M.anchor.width) * 1.2, 40.0 * SCALE));
    const double VISW    = STARTW + (plate.width - STARTW) * OP;
    const double VISH    = std::max(1.0, plate.height * OP);
    const double ACX     = std::clamp(sc<double>(CX), plate.x + VISW / 2.0, plate.x + plate.width - VISW / 2.0);
    const double VISCX   = ACX + (plate.x + plate.width / 2.0 - ACX) * OP;
    const CBox   VIS     = CBox{VISCX - VISW / 2.0, plate.y + plate.height - VISH, VISW, VISH}.round();
    const float  CONTENT = std::clamp((OP - 0.2F) / 0.8F, 0.F, 1.F); // the rows come in once there is room for them
    // THE ROWS CASCADE: each row comes in on its own stretch of CONTENT, the
    // one nearest the dock first, rising a few px into its place as it fades
    // in. Closing runs the same curve backwards, so the top row leaves first.
    // ROWA / ROWDY are what the row being drawn has (set in the loop below).
    float      ROWA = CONTENT, ROWDY = 0.F;
    const auto rowIn = [&](size_t i) {
        const size_t N  = std::max<size_t>(1, M.rows.size());
        const float  ST = N > 1 ? std::min(0.07F, 0.55F / sc<float>(N - 1)) : 0.F; // stagger, in CONTENT
        const float  K  = sc<float>(N - 1 - std::min(i, N - 1));                     // 0 = the row nearest the dock
        const float  P  = std::clamp(CONTENT * (1.F + ST * sc<float>(N - 1)) - ST * K, 0.F, 1.F);
        const float  E  = Hyprdrive::Motion::outCubic(P);
        ROWA            = Hyprdrive::Motion::smoothstep(P);
        ROWDY           = (1.F - E) * 9.F * SCALE;
    };
    const auto   clipTo  = [&](CBox c) { return c.empty() ? VIS : c.intersection(VIS); };
    const auto   ADDTEX  = [&](CTexPassElement::SRenderData d) {
        if (ROWDY != 0.F) {
            d.box.y += ROWDY;
            if (!d.clipBox.empty())
                d.clipBox.y += ROWDY;
        }
        d.clipBox = clipTo(d.clipBox);
        d.a *= ROWA;
        if (!d.clipBox.empty() && d.a > 0.001F)
            g_pHyprRenderer->m_renderPass.add(makeUnique<CTexPassElement>(d));
    };
    const auto ADDRECT = [&](CRectPassElement::SRectData d) {
        if (ROWDY != 0.F) {
            d.box.y += ROWDY;
            if (!d.clipBox.empty())
                d.clipBox.y += ROWDY;
        }
        d.clipBox = clipTo(d.clipBox);
        d.color   = d.color.modifyA(d.color.a * ROWA);
        if (!d.clipBox.empty() && d.color.a > 0.001F)
            g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(d));
    };

    // X13/X29/X86: menu frost -- shared chrome sample in overview, live on desktop.
    frostedPlate(monitor, VIS, sc<int>(std::round(std::min<double>(MENU_RADIUS * SCALE, VISH / 2.0))), 1.F);

    const auto MOUSE = (g_pInputManager->getMouseCoordsInternal() - monitor->m_position) * monitor->m_scale;

    // What the pointer is on, by last frame's boxes (the ones it was drawn at).
    // Just after a row's close, no button is: the one that slid under the
    // resting pointer must not look armed (menuButtonsArmed).
    const bool ARMED = menuButtonsArmed(M, MOUSE, SCALE);
    {
        int hr = -1, hi = -1;
        for (size_t i = 0; i < M.rows.size(); ++i) {
            const auto& R = M.rows[i];
            if (R.kind == ERow::Separator || R.disabled || R.box.empty() || !R.box.containsPoint(MOUSE))
                continue;
            hr = sc<int>(i);
            for (size_t b = 0; b < R.inlines.size(); ++b)
                if (ARMED && R.inlines[b].kind != ERow::Separator && !R.inlines[b].box.empty() && R.inlines[b].box.containsPoint(MOUSE))
                    hi = sc<int>(b);
            break;
        }
        if (hr != M.hoverRow || hi != M.hoverInl) {
            M.hoverRow     = hr;
            M.hoverInl     = hi;
            const auto NOW = Time::steadyNow();
            M.lit.moveTo(SMenu::litKey(hr, hi), NOW, menuHoverEased);
            M.lift.moveTo(SMenu::litKey(hr, -1), NOW, menuHoverEased);
        }
    }
    // THE ROW'S PREVIEW: the window of the row the pointer is on -- its
    // buttons count as the row -- and nothing for any other row, a scoped
    // (preview's own) menu, the closing ghost or a menu still unfolding.
    {
        PHLWINDOW want;
        if (LIVE && !M.windowScope && !menuOpening(M) && M.hoverRow >= 0 && M.hoverRow < sc<int>(M.rows.size())) {
            const auto& R = M.rows[M.hoverRow];
            if (const auto W = R.kind == ERow::Window ? R.window.lock() : PHLWINDOW{}; W && W->m_isMapped)
                want = W;
        }
        M.peekWant = want;
    }

    // The row's highlight (the row itself hovered, not one of its buttons).
    const auto rowLit = [&](int i) {
        return M.lit.level(SMenu::litKey(i, -1), menuHoverEased);
    };
    // The row's lift: it stays lifted while the pointer is on its buttons.
    const auto rowLift = [&](int i) {
        return M.lift.level(SMenu::litKey(i, -1), menuHoverEased);
    };
    const auto buttonLit = [&](int i, int b) {
        return M.lit.level(SMenu::litKey(i, b), menuHoverEased);
    };

    // Text that may run past `right` fades out over its last few px there
    // instead of being cut: drawn whole up to the fade, then in thin slices
    // that thin out, each clipped to its own strip.
    const auto fadedText = [&](CTexPassElement::SRenderData d, float right) {
        if (!d.tex || !Stacks::drawableBox(d.box))
            return;
        if (d.box.x + d.box.width <= right + 0.5) {
            ADDTEX(d);
            return;
        }
        const float FADE   = std::round(12.F * SCALE);
        const float SOLIDR = right - FADE;
        if (SOLIDR > d.box.x) {
            auto solid    = d;
            solid.clipBox = CBox{d.box.x, d.box.y - 2, SOLIDR - d.box.x, d.box.height + 4};
            ADDTEX(solid);
        }
        constexpr int STEPS = 6;
        for (int k = 0; k < STEPS; ++k) {
            const float X0 = SOLIDR + FADE * sc<float>(k) / STEPS, X1 = SOLIDR + FADE * sc<float>(k + 1) / STEPS;
            if (X1 <= d.box.x)
                continue;
            auto slice    = d;
            slice.clipBox = CBox{std::max<double>(X0, d.box.x), d.box.y - 2, X1 - std::max<double>(X0, d.box.x), d.box.height + 4};
            slice.a       = d.a * (1.F - (sc<float>(k) + 0.5F) / STEPS);
            if (slice.clipBox.width > 0)
                ADDTEX(slice);
        }
    };

    const auto chevron = [&](float atY, bool up, bool more) {
        if (!more)
            return;
        const auto T = OverviewRender::text(up ? MenuGlyph::UP : MenuGlyph::DOWN, DIM_COLOR, std::max(1, sc<int>(std::round(FONTPX * 0.8F))), false, "JetBrainsMono Nerd Font", 0, 400);
        if (!T || !T->ok())
            return;
        CTexPassElement::SRenderData c;
        c.tex = T;
        c.box = CBox{std::round(plate.x + (plate.width - sc<float>(T->m_size.x)) / 2.F), std::round(atY + (CHEV - sc<float>(T->m_size.y)) / 2.F), sc<float>(T->m_size.x),
                     sc<float>(T->m_size.y)};
        c.a   = 1.F;
        ADDTEX(c);
    };

    float y = sc<float>(plate.y);
    for (size_t i = 0; i < M.rows.size(); ++i) {
        auto&       row = M.rows[i];
        const auto& TEX = textures[i];
        rowIn(i);

        if (scrolling && sc<int>(i) == segFirst) {
            chevron(y, true, M.scrollTop > 0);
            y += CHEV;
        }
        if (scrolling && sc<int>(i) >= segFirst && sc<int>(i) <= segLast) {
            const int K = sc<int>(i) - segFirst;
            if (K < M.scrollTop || K >= M.scrollTop + visibleRows) {
                row.box = {};
                for (auto& B : row.inlines)
                    B.box = {};
                if (sc<int>(i) == segLast) {
                    chevron(y, false, M.scrollTop + visibleRows < segLast - segFirst + 1);
                    y += CHEV;
                }
                continue;
            }
        }

        if (row.segmented && TEX.lineH > 0.F) {
            const float ROWH = TEX.lineH + 2.F * MENU_ROW_PAD_Y * SCALE;
            row.box          = CBox{plate.x, std::round(y), plate.width, std::round(ROWH)};
            const size_t NS  = row.inlines.size();
            const bool   CHIPS = TEX.label && TEX.label->ok();

            // Where each segment goes, guarded (MenuModel: finite, >= 2 px).
            std::vector<MenuModel::SSpan> spans;
            if (CHIPS) {
                std::vector<double> widths;
                for (size_t b = 0; b < NS; ++b)
                    widths.push_back(segContentW(TEX, b) + 2.F * SEG_PAD);
                spans = MenuModel::chipsEndingAt(plate.x + plate.width - SEG_EDGE, widths, SEG_GAP);
            } else
                spans = MenuModel::splitSpan(plate.x + SEG_EDGE, plate.width - 2.F * SEG_EDGE, sc<int>(NS), SEG_GAP);

            // The chips' caption, where every other row's label starts.
            float captionRight = sc<float>(plate.x + plate.width);
            if (CHIPS) {
                const float TEXTX = std::round(plate.x + MENU_PAD_X * SCALE + GLYPHCOLW);
                const float LH    = sc<float>(TEX.label->m_size.y);
                captionRight      = !spans.empty() && spans.front().w > 0 ? sc<float>(spans.front().x) - SEG_GAP : captionRight;
                CTexPassElement::SRenderData cap;
                cap.tex     = TEX.label;
                cap.box     = CBox{TEXTX, std::round(y + (ROWH - LH) / 2.F), sc<float>(TEX.label->m_size.x), LH};
                cap.a       = 1.F;
                fadedText(cap, captionRight);
            }

            for (size_t b = 0; b < NS; ++b) {
                auto&      B    = row.inlines[b];
                const auto SPAN = b < spans.size() ? spans[b] : MenuModel::SSpan{};
                const auto HALF = CBox{SPAN.x, std::round(y), SPAN.w, std::round(ROWH)};
                if (SPAN.w <= 0 || !Stacks::drawableBox(HALF)) {
                    B.box = {};
                    continue;
                }
                B.box = HALF; // the whole row's height is the hit area

                // Its plate: always there, faint, so the halves read as
                // buttons; lit in the accent while ON, and the hover's chip
                // eases in over it from a little smaller, as a row's buttons do.
                const float BL    = buttonLit(sc<int>(i), sc<int>(b));
                const CBox  PLATE = CBox{HALF.x, HALF.y + SEG_INSETY, HALF.width, HALF.height - 2.0 * SEG_INSETY};
                if (Stacks::drawableBox(PLATE)) {
                    const int R = std::min(SEG_RADIUS, sc<int>(PLATE.height / 2.0));
                    CRectPassElement::SRectData rest;
                    rest.box           = PLATE;
                    rest.color         = B.on ? Hyprdrive::Config::highlightColor().modifyA(0.48F) : CHyprColor{INK.r, INK.g, INK.b, LIGHT ? 0.08F : 0.07F};
                    rest.round         = R;
                    rest.roundingPower = 2.F;
                    ADDRECT(rest);
                    if (BL > 0.01F) {
                        const double INSET = (1.F - BL) * 3.F * SCALE;
                        CRectPassElement::SRectData hov;
                        hov.box           = CBox{PLATE.x + INSET, PLATE.y + INSET, PLATE.width - 2 * INSET, PLATE.height - 2 * INSET};
                        hov.color         = Hyprdrive::Config::highlightColor().modifyA((B.on ? 0.12F : 0.26F) * BL);
                        hov.round         = std::max(0, R - sc<int>(std::round(INSET)));
                        hov.roundingPower = 2.F;
                        if (Stacks::drawableBox(hov.box))
                            ADDRECT(hov);
                    }
                }

                // Glyph and label, centred together; the hover grows them a
                // touch, as it lifts a row's. A label too long for its half
                // fades out at the half's inner edge rather than being cut.
                const float GROW = 1.F + 0.035F * BL;
                const auto  G    = b < TEX.segGlyphs.size() ? TEX.segGlyphs[b] : SP<Render::ITexture>{};
                const auto  L    = b < TEX.segLabels.size() ? TEX.segLabels[b] : SP<Render::ITexture>{};
                const float GW   = G && G->ok() ? sc<float>(G->m_size.x) * GROW : 0.F;
                const float LW   = L && L->ok() ? sc<float>(L->m_size.x) * GROW : 0.F;
                const float CW   = GW + (GW > 0.F && LW > 0.F ? SEG_GLYPHG : 0.F) + LW;
                // Centred on the segment, so the hover's growth spends a px
                // of padding each side; the fade starts only past the padding
                // less a hair -- i.e. only for a label that does not fit.
                const float INL  = sc<float>(HALF.x) + SEG_INSETY, INR = sc<float>(HALF.x + HALF.width) - SEG_INSETY;
                float       cx   = std::max(INL, std::round(sc<float>(HALF.x) + (sc<float>(HALF.width) - CW) / 2.F));
                if (GW > 0.F) {
                    CTexPassElement::SRenderData g;
                    g.tex = G;
                    const float GH = sc<float>(G->m_size.y) * GROW;
                    g.box          = CBox{cx, std::round(y + (ROWH - GH) / 2.F), GW, GH};
                    g.a            = 1.F;
                    fadedText(g, INR);
                    cx += GW + SEG_GLYPHG;
                }
                if (LW > 0.F) {
                    CTexPassElement::SRenderData t;
                    t.tex          = L;
                    const float LH = sc<float>(L->m_size.y) * GROW;
                    t.box          = CBox{cx, y + (ROWH - LH) / 2.F, LW, LH};
                    t.a            = 1.F;
                    fadedText(t, INR);
                }
            }
            y += ROWH;
            continue;
        }

        if (!TEX.label) {
            row.box = CBox{plate.x, std::round(y), plate.width, std::round(MENU_SEP_H * SCALE)};

            CRectPassElement::SRectData sep;
            sep.box   = CBox{std::round(plate.x + MENU_PAD_X * SCALE), std::round(y + MENU_SEP_H * SCALE / 2.F),
                             std::round(plate.width - 2.F * MENU_PAD_X * SCALE), sc<double>(std::max(1.F, std::round(SCALE)))};
            sep.color = CHyprColor{INK.r, INK.g, INK.b, 0.15F};
            ADDRECT(sep);

            y += MENU_SEP_H * SCALE;
            continue;
        }

        const float ROWH = sc<float>(TEX.label->m_size.y) + 2.F * MENU_ROW_PAD_Y * SCALE;
        row.box          = CBox{plate.x, std::round(y), plate.width, std::round(ROWH)};

        // The row's buttons, right to left from the plate's inner edge.
        const SRow::SInline* hoveredInline = nullptr;
        const float          BTN           = std::round(ROWH);
        const auto           SPANS         = MenuModel::rowButtons(plate.x + plate.width - MENU_PAD_X * SCALE * 0.5F, BTN, row.inlines.size());
        for (size_t b = 0; b < row.inlines.size(); ++b) {
            auto& B = row.inlines[b];
            B.box   = SPANS[b].w > 0 ? CBox{SPANS[b].x, std::round(y), SPANS[b].w, std::round(ROWH)} : CBox{};
            if (B.kind == ERow::Separator || B.box.empty())
                continue;
            if (ARMED && B.box.containsPoint(MOUSE))
                hoveredInline = &B;
        }

        if (const float LIT = row.disabled ? 0.F : rowLit(sc<int>(i)); LIT > 0.01F) {
            // X56: the context menu's row highlight follows the theme too --
            // except on the destructive row, which lights in its own colour so
            // the pointer landing on it is noticed.
            const auto ROWACCENT = row.danger ? DANGER_COLOR : Hyprdrive::Config::highlightColor();

            // The first and last rows sit in the plate's rounded corners, and a
            // square highlight poked out of them (user, 2026-09-29, Force Quit).
            // A rect rounds all four corners or none, so the fill is drawn
            // rounded, stretched past the row on its INNER side and clipped
            // back to it: rounded where it meets the plate's edge, square
            // where it meets the next row.
            const int  RADIUS = sc<int>(std::round(MENU_RADIUS * SCALE));
            const bool FIRST  = i == 0;
            const bool LAST   = i + 1 == M.rows.size();
            CBox       shape  = row.box;
            if (FIRST && !LAST)
                shape.height += RADIUS; // stretch down: only the top is rounded
            else if (LAST && !FIRST) {
                shape.y -= RADIUS; // stretch up: only the bottom is rounded
                shape.height += RADIUS;
            }

            CRectPassElement::SRectData fill;
            fill.box           = shape;
            fill.clipBox       = row.box;
            fill.color         = ROWACCENT.modifyA((row.danger ? 0.18F : 0.26F) * LIT);
            fill.round         = FIRST || LAST ? RADIUS : 0;
            fill.roundingPower = 2.F;
            ADDRECT(fill);
        }

        // A sub-row starts where its parent's label does. A hovered row's glyph
        // and label lift: a few px to the right, and a touch larger.
        const float LIFT  = rowLift(sc<int>(i));
        const float GROW  = 1.F + 0.035F * LIFT;
        const float TEXTX = std::round(plate.x + MENU_PAD_X * SCALE + (row.sub ? GLYPHCOLW : 0.F) + 4.F * SCALE * LIFT);
        const float TEXTY = std::round(y + MENU_ROW_PAD_Y * SCALE);
        const float LH    = sc<float>(TEX.label->m_size.y);

        if (TEX.glyph && TEX.glyph->ok()) {
            CTexPassElement::SRenderData g;
            g.tex = TEX.glyph;
            // Centred in the column and on the label's line.
            const float GW = sc<float>(TEX.glyph->m_size.x) * GROW, GH = sc<float>(TEX.glyph->m_size.y) * GROW;
            g.box = CBox{std::round(TEXTX + (GLYPHCOLW - GW) / 2.F - GLYPHCOLW * 0.15F), std::round(TEXTY + (LH - GH) / 2.F), GW, GH};
            g.a   = 1.F;
            ADDTEX(g);
        }

        CTexPassElement::SRenderData text;
        text.tex = TEX.label;
        text.box = CBox{TEXTX + GLYPHCOLW, TEXTY - (LH * GROW - LH) / 2.F, sc<float>(TEX.label->m_size.x) * GROW, LH * GROW};
        text.a   = 1.F;
        ADDTEX(text);

        for (size_t b = 0; b < row.inlines.size(); ++b) {
            const auto& B = row.inlines[b];
            const auto  G = b < TEX.inlineGlyphs.size() ? TEX.inlineGlyphs[b] : SP<Render::ITexture>{};
            if (B.kind == ERow::Separator || B.box.empty() || !G || !G->ok())
                continue;
            // A hovered button's chip pops in from smaller, and its glyph grows.
            const float BL = buttonLit(sc<int>(i), sc<int>(b));
            if (BL > 0.01F) {
                CRectPassElement::SRectData fill;
                const double INSET = 3.F * SCALE + (1.F - BL) * 5.F * SCALE;
                fill.box           = CBox{B.box.x + INSET, B.box.y + INSET, B.box.width - 2 * INSET, B.box.height - 2 * INSET};
                fill.color         = Hyprdrive::Config::highlightColor().modifyA(0.30F * BL);
                fill.round         = sc<int>(std::round(6.F * SCALE));
                fill.roundingPower = 2.F;
                ADDRECT(fill);
            }
            CTexPassElement::SRenderData g;
            g.tex = G;
            const float BG = 1.F + 0.15F * BL, GW = sc<float>(G->m_size.x) * BG, GH = sc<float>(G->m_size.y) * BG;
            g.box = CBox{std::round(B.box.x + (B.box.width - GW) / 2.F), std::round(B.box.y + (B.box.height - GH) / 2.F), GW, GH};
            g.a   = 1.F;
            ADDTEX(g);
        }
        // A hovered button names itself where the hint was.
        const auto HINTTEX = hoveredInline ? OverviewRender::text(hoveredInline->label, LABEL_COLOR, FONTPX, false, "", 0, 400) : TEX.hint;
        const float HINTRIGHT = row.inlines.empty() ? sc<float>(plate.x + plate.width - MENU_PAD_X * SCALE) :
                                                      sc<float>(row.inlines.front().box.x) - HINTGAP * 0.5F;
        if (HINTTEX && HINTTEX->ok()) {
            CTexPassElement::SRenderData h;
            h.tex = HINTTEX;
            h.box = CBox{std::round(HINTRIGHT - sc<float>(HINTTEX->m_size.x)), std::round(TEXTY + (LH - sc<float>(HINTTEX->m_size.y)) / 2.F),
                         sc<float>(HINTTEX->m_size.x), sc<float>(HINTTEX->m_size.y)};
            h.a   = 1.F;
            ADDTEX(h);
        }

        y += ROWH;
        if (scrolling && sc<int>(i) == segLast) {
            chevron(y, false, M.scrollTop + visibleRows < segLast - segFirst + 1);
            y += CHEV;
        }
    }

    // The pill's outline on the menu too (user, 2026-09-29): theme accent at
    // Hyprland's border weight, floored at 1. Drawn after the rows so a
    // full-width highlight never paints over it.
    {
        const int                       BORDERSIZE = std::max(1, Hyprdrive::Config::getValue<int>("general:border_size"));
        CBorderPassElement::SBorderData border;
        border.box = VIS;
        border.grad1.reset(Hyprdrive::Config::highlightColor().modifyA(1.F));
        border.a             = 1.F;
        border.borderSize    = BORDERSIZE;
        border.round         = sc<int>(std::round(std::min<double>(MENU_RADIUS * SCALE, VISH / 2.0)));
        border.roundingPower = 2.F;
        if (Stacks::drawableBox(border.box))
            g_pHyprRenderer->m_renderPass.add(makeUnique<CBorderPassElement>(border));
    }

    // The row preview, beside the plate as it is now: one plate travelling
    // from row to row as the hover pane does between apps (renderRowPeek).
    renderRowPeek(monitor, M, plate, LIVE, Time::steadyNow());
}

void renderMenu(PHLMONITOR monitor, const SLayout& layout) {
    if (g_menuGhost) {
        const float P = menuGhostP(*g_menuGhost);
        if (P <= 0.001F)
            g_menuGhost.reset();
        else
            renderMenuOne(monitor, layout, g_menuGhost->menu, P);
    }
    if (g_menu)
        renderMenuOne(monitor, layout, *g_menu, menuOpenP(*g_menu));
}
}
}
