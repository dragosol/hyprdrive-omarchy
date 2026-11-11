// The item model: which icons the dock shows (buildDockItems, the cached
// dockItems), publishing them, badges, urgency and the urgent-hop timer.
#include "DockParts.hpp"
#include "IdentityLive.hpp"
#include "MotionClock.hpp"

namespace Hyprdrive::Dock {
using namespace DockDetail;
namespace DockDetail {

// isolate_workspaces everywhere, or isolate_in_overview while the focused
// monitor's overview is open. The overview moves the monitor's active
// workspace as the user walks the strip, so "current" follows it there too.
bool isolateNow() {
    if (Config::getValue<bool>(KEY_ISOLATE_WS))
        return true;
    if (!Config::getValue<bool>(KEY_ISOLATE_OV))
        return false;
    const auto MONITOR = Desktop::focusState()->monitor();
    return MONITOR && scrollOverviewForMonitor(MONITOR);
}

SInterestCfg interestCfg() {
    SInterestCfg cfg;
    cfg.isolate = isolateNow();
    if (!cfg.isolate)
        return cfg; // nothing below is read

    cfg.agnosticUrgent = Config::getValue<bool>(KEY_AGNOSTIC_URG);
    if (const auto MONITOR = Desktop::focusState()->monitor(); MONITOR)
        cfg.activeWorkspace = MONITOR->m_activeWorkspace;

    return cfg;
}

bool interesting(const PHLWINDOW& window, const SInterestCfg& cfg) {
    if (!window || !window->m_isMapped)
        return false;

    if (!cfg.isolate)
        return true;

    if (Minimize::isMinimized(window))
        return true;

    if (window->m_isUrgent && cfg.agnosticUrgent)
        return true;

    return cfg.activeWorkspace && window->m_workspace == cfg.activeWorkspace;
}

// X27 showed what is RUNNING and nothing else. X32 keeps that list and puts the
// FAVOURITES in front of it: the compositor's own window order, which is stable
// across frames -- an MRU order here would make the dock reshuffle on every
// focus.
std::vector<std::string> runningClasses() {
    std::vector<std::string>        out;
    std::unordered_set<std::string> seen;

    const auto CFG = interestCfg();
    for (const auto& windowRef : Desktop::windowState()->windows()) {
        const auto& WINDOW = windowRef;
        if (!interesting(WINDOW, CFG) || WINDOW->m_class.empty())
            continue;

        const auto CLASS = Hyprdrive::AppIdentity::groupKey(WINDOW);
        if (seen.insert(CLASS).second)
            out.emplace_back(CLASS);
    }

    return out;
}

std::string pendingLaunchKey(const SItem& item) {
    return !item.uri.empty() ? item.uri : (item.desktopId.empty() ? item.wmClass : item.desktopId);
}

// Is the dock's own background dark? The Trash's automatic glass reads this
// the same way (light glass on a dark dock).
static bool dockIsDark() {
    const auto BG = Config::dockBackgroundColor();
    return 0.2126 * BG.r + 0.7152 * BG.g + 0.0722 * BG.b < 0.5;
}

// The override id of one Trash state, as hdicons and the panel spell it.
static std::string trashStateId(bool empty, bool darkDock) {
    return std::string("trash:") + (empty ? "empty" : "full") + (darkDock ? "-dark" : "-light");
}

std::vector<SItem> buildDockItems() {
    const auto RUNNING = runningClasses();

    std::vector<std::string> runningIds;
    runningIds.reserve(RUNNING.size());
    for (const auto& CLASS : RUNNING)
        runningIds.emplace_back(AppSearch::idForClass(CLASS));

    std::vector<SItem> out;
    std::unordered_set<size_t> claimed;
    std::unordered_set<std::string> placedPaths; // folders drawn among the favourites

    if (Config::getValue<bool>(KEY_SHOW_FAVORITES)) {
        for (const auto& ID : favorites()) {
            if (const auto* S = placedFolder(ID)) {
                if (!placedPaths.insert(S->path).second)
                    continue;
                const auto* FOLDERICON = IconOverrides::forId(IconOverrides::folderId(S->path));
                out.emplace_back(SItem{
                    .icon         = FOLDERICON ? *FOLDERICON : Stacks::themedIcon(S->icon),
                    .uri          = "file://" + S->path,
                    .locationName = S->label,
                    .stack        = true,
                    .stackPath    = S->path,
                    .stackDir     = true,
                    .placed       = true,
                });
                continue;
            }
            const auto* ENTRY = AppSearch::entryForId(ID);
            // A favourite nothing on this machine provides is dropped rather
            // than drawn as a blank slot: the seed is the user's HOST list and
            // the test VM has a fraction of it installed.
            if (!ENTRY)
                continue;

            SItem item;
            item.desktopId = ID;
            item.icon      = ENTRY->icon;
            item.favorite  = true;
            // The settings panel's Icons section (IconOverrides.hpp).
            if (const auto* OVERRIDE = IconOverrides::forId(ID))
                item.icon = *OVERRIDE;

            // Every running class that resolves to this id owns the slot, so a
            // pinned app that IS running is one icon, not two -- including a
            // Chromium webapp whose live class is chrome-host_path-Default and
            // a second profile of the same app (another class, same desktop id).
            // itemWindows then finds every window of those classes.
            for (size_t i = 0; i < RUNNING.size(); ++i) {
                if (!Hyprdrive::AppIdentity::sameApp(runningIds[i], ID) || claimed.contains(i))
                    continue;

                claimed.insert(i);
                if (item.wmClass.empty())
                    item.wmClass = RUNNING[i];
                item.running = true;
            }

            out.emplace_back(std::move(item));
        }
    }

    for (size_t i = 0; i < RUNNING.size(); ++i) {
        if (claimed.contains(i))
            continue;

        // No desktop entry claims the class: ask for an icon NAMED after it
        // (plenty of apps install exactly that), which resolveIconPath answers
        // with the generic application tile when the theme has none. The slot
        // used to be left blank -- a running app with nothing to click on.
        auto icon = AppSearch::iconForClass(RUNNING[i]);
        if (icon.empty() && !RUNNING[i].empty()) {
            icon = RUNNING[i];
            std::ranges::transform(icon, icon.begin(), [](unsigned char c) { return sc<char>(std::tolower(c)); });
        }
        if (const auto* OVERRIDE = IconOverrides::forId(runningIds[i]))
            icon = *OVERRIDE;

        out.emplace_back(SItem{
            .wmClass   = RUNNING[i],
            .icon      = icon,
            .desktopId = runningIds[i],
            .running   = true,
        });
    }

    // The scratchpad sits after the apps and BEFORE the locations: it holds
    // windows, so it belongs with the app icons rather than out among the
    // shelf. One slot per populated scratchpad, and it disappears with its last
    // window -- the same "only what is running" rule the app icons follow.
    for (const auto& WORKSPACE : Scratchpad::populated()) {
        out.emplace_back(SItem{
            .icon           = Scratchpad::iconName(),
            .running        = true,
            .locationName   = Scratchpad::label(WORKSPACE),
            .scratchpad     = true,
            .scratchpadName = Scratchpad::shortName(WORKSPACE),
        });
    }

    // DOCK ORDER (dock stacks A, 2026-09-24): apps | Scratchpad | stacks |
    // removable drives | Trash. Stacks used to come after the locations, which
    // put the user's own shelf behind Trash.
    //
    // A stack carries `uri` as well as `stack`, so the `!uri.empty()` branches
    // this file already has treat it as the not-an-app it is. Its icon, label
    // and is-a-directory answer are resolved when the STORE is read
    // (DockStacks.cpp, decorate), not here.
    // Dock stacks B: a folder hovering the dock opens a gap among the stacks
    // -- a faint cell for it, which the arrangement animation parts the
    // neighbours around.
    const auto PLACEHOLDER = [&] {
        const auto& P = DockDrop::placeholderPath();
        out.emplace_back(SItem{
            // Same lookup as the real stacks below, so the gap shows the user's
            // icon pack's folder, not the fallback theme's.
            .icon         = Stacks::themedIcon("folder"),
            .uri          = "file://" + P,
            .locationName = P.substr(P.find_last_of('/') + 1),
            .stack        = true,
            .stackDir     = true,
            .placeholder  = true,
        });
    };
    int stackIndex = 0;
    for (const auto& STACK : Stacks::all()) {
        if (stackIndex++ == DockDrop::placeholderIndex())
            PLACEHOLDER();
        if (placedPaths.contains(STACK.path))
            continue; // drawn where the favourites put it
        const auto* FOLDERICON = STACK.isDir ? IconOverrides::forId(IconOverrides::folderId(STACK.path)) : nullptr;
        out.emplace_back(SItem{
            .icon         = FOLDERICON ? *FOLDERICON : Stacks::themedIcon(STACK.icon),
            .uri          = "file://" + STACK.path,
            .locationName = STACK.label,
            .stack        = true,
            .stackPath    = STACK.path,
            .stackDir     = STACK.isDir,
        });
    }
    if (DockDrop::placeholderIndex() >= stackIndex)
        PLACEHOLDER();

    // X35. Locations: mounts first, then everything else (Trash), so Trash
    // stays the last cell. `isolate_locations` TRUE makes a location's running
    // state its own -- asked of the file manager's open windows.
    const bool ISOLATE = Config::getValue<bool>(KEY_ISOLATE_LOCATIONS);
    for (const bool MOUNTS : {true, false}) {
        for (const auto& LOCATION : Locations::items()) {
            if (LOCATION.mount != MOUNTS)
                continue;
            // Dock stacks C: the Trash is hyprdrive's own artwork, or the
            // user-trash icon from the Omarchy theme's icons or GTK's
            // (dock:trash_icon, 2026-09-24).
            std::string icon = LOCATION.icon;
            if (LOCATION.uri == "trash:///") {
                const auto WHICH = Config::getValue<std::string>(KEY_TRASH_ICON);
                auto       style = Config::getValue<std::string>("plugin:hyprdrive:dock:trash_style");
                if (style != "light" && style != "dark" && style != "clear") {
                    // auto: light glass on a dark dock, dark glass on a light one.
                    const auto BG = Config::dockBackgroundColor();
                    style         = 0.2126 * BG.r + 0.7152 * BG.g + 0.0722 * BG.b < 0.5 ? "light" : "dark";
                }
                icon = WHICH == "gtk" || WHICH == "omarchy" ?
                    Stacks::themePrefix(WHICH) + LOCATION.icon :
                    BuiltinIcons::trashName(LOCATION.empty, style, Config::getValue<std::string>("plugin:hyprdrive:dock:trash_papers"));
                // The settings panel's Icons -> Trash: one picture per state,
                // empty or full, on a dark dock or a light one (user,
                // 2026-10-13). The dock's own background decides the mode.
                if (const auto* OVERRIDE = IconOverrides::forId(trashStateId(LOCATION.empty, dockIsDark())))
                    icon = *OVERRIDE;
            }
            out.emplace_back(SItem{
                .icon         = icon,
                .running      = ISOLATE && Locations::openWindows(LOCATION.uri),
                .uri          = LOCATION.uri,
                .locationName = LOCATION.name,
                .mount        = LOCATION.mount,
                .canEject     = LOCATION.canEject,
                .mounted      = LOCATION.mounted,
                .trashEmpty   = LOCATION.empty,
            });
        }
    }

    // ONE PASS, at the end, so no branch above can forget it. See SItem::subject.
    for (auto& item : out)
        item.subject = tooltipSubject(item);

    for (const auto& ITEM : out)
        if (ITEM.running && !ITEM.scratchpad)
            g_pendingLaunches.erase(pendingLaunchKey(ITEM));

    return out;
}

// FNV-1a over the four window properties the item list can see. Deliberately
// not a hash of anything allocated: this runs on the motion path.
uint64_t windowDigest() {
    uint64_t       h   = 1469598103934665603ULL;
    const auto     mix = [&h](uint64_t v) {
        h ^= v;
        h *= 1099511628211ULL;
    };

    for (const auto& WINDOW : Desktop::windowState()->windows()) {
        if (!WINDOW) {
            mix(0);
            continue;
        }

        mix(sc<uint64_t>(reinterpret_cast<uintptr_t>(WINDOW.get())));
        mix((WINDOW->m_isMapped ? 1ULL : 0ULL) | (WINDOW->m_isUrgent ? 2ULL : 0ULL));
        mix(WINDOW->m_workspace ? sc<uint64_t>(WINDOW->m_workspace->m_id) : 0ULL);
    }

    return h;
}

SItemsKey itemsKey() {
    SItemsKey key;
    key.membership = Membership::generation();
    key.windows    = windowDigest();
    key.favorites  = favoritesStore().generation;
    key.locations  = Locations::generation();
    key.stacks     = Stacks::generation();
    key.dropGap    = DockDrop::placeholderGeneration();
    // The GENERATION: a rescan that swaps one entry for another keeps the size.
    key.inventory  = AppSearch::inventoryGeneration();
    key.icons      = AppSearch::iconIndexGeneration();
    key.overrides  = IconOverrides::generation();

    const bool ISOLATE_WS = isolateNow();
    key.config            = (Config::getValue<bool>(KEY_SHOW_FAVORITES) ? 1U : 0U) |      //
        (ISOLATE_WS ? 2U : 0U) |                                                          //
        (Config::getValue<bool>(KEY_AGNOSTIC_URG) ? 4U : 0U) |                            //
        (Config::getValue<bool>(KEY_ISOLATE_LOCATIONS) ? 8U : 0U) |                       //
        (Config::getValue<bool>(KEY_BUILTIN_ICONS) ? 16U : 0U) |                          //
        (Config::getValue<bool>(KEY_FOLDERS_ANYWHERE) ? 32U : 0U);
    // The icon themes and the Trash's look are strings; a change of any of them
    // is a different item list.
    key.themes = std::hash<std::string>{}(AppSearch::omarchyIconTheme() + "|" + Locations::nautilusIconTheme() + "|" + Config::getValue<std::string>("plugin:hyprdrive:dock:stack_icon_theme") + "|" +
                                          Config::getValue<std::string>(KEY_TRASH_ICON) + "|" + Config::getValue<std::string>("plugin:hyprdrive:dock:trash_style") + "|" +
                                          Config::getValue<std::string>("plugin:hyprdrive:dock:trash_papers"));

    // Only with isolation on does WHICH desktop is focused change the list.
    // Reading it unconditionally would throw the cache away on every workspace
    // switch for a dock that is deliberately workspace-agnostic.
    if (ISOLATE_WS) {
        if (const auto MONITOR = Desktop::focusState()->monitor(); MONITOR && MONITOR->m_activeWorkspace)
            key.activeWs = MONITOR->m_activeWorkspace->m_id;
    }

    return key;
}

// The settings panel's Icons section lists the apps ON the dock, under the
// exact desktop ids the dock keys overrides on -- which only the dock knows
// (AppSearch::idForClass's rules, Chromium webapps included). So the dock says:
// ~/.local/state/hyprdrive/dock-apps, `id TAB name TAB own icon` per app, in
// dock order, rewritten only when that text changes (a rebuild of the item
// list is usually a window opening, not an app arriving).
static void publishDockApps(const std::vector<SItem>& items) {
    std::string text;
    std::unordered_set<std::string> seen;
    for (const auto& IT : items) {
        if (IT.desktopId.empty() || !IT.uri.empty() || !seen.insert(IT.desktopId).second)
            continue;
        const auto* ENTRY = AppSearch::entryForId(IT.desktopId);
        if (!ENTRY)
            continue;
        text += IT.desktopId + "\t" + ENTRY->name + "\t" + ENTRY->icon + "\n";
    }
    // The Trash's four states and the picture each gets by default, so the
    // panel can show them: `id TAB icon` in trash-states. A built-in picture is
    // written out as an SVG beside it (it exists only in the plugin), anything
    // else is an icon name the panel resolves itself.
    {
        const auto        DIR   = favoritesPath().parent_path();
        const auto        WHICH = Config::getValue<std::string>(KEY_TRASH_ICON);
        const auto        STYLE = Config::getValue<std::string>("plugin:hyprdrive:dock:trash_style");
        const auto        PAPERS = Config::getValue<std::string>("plugin:hyprdrive:dock:trash_papers");
        const auto        A     = Hyprdrive::Config::highlightColor();
        const auto        HEX   = std::format("#{:02x}{:02x}{:02x}", sc<int>(std::clamp(A.r, 0.0, 1.0) * 255.0), sc<int>(std::clamp(A.g, 0.0, 1.0) * 255.0),
                                              sc<int>(std::clamp(A.b, 0.0, 1.0) * 255.0));
        static std::string lastKey;
        const std::string  KEY = WHICH + "|" + STYLE + "|" + PAPERS + "|" + HEX;
        std::string        states;
        for (const bool DARK : {true, false}) {
            if (KEY == lastKey)
                break;
            for (const bool EMPTY : {true, false}) {
                const auto ID = trashStateId(EMPTY, DARK);
                std::string def;
                if (WHICH == "gtk" || WHICH == "omarchy")
                    def = EMPTY ? "user-trash" : "user-trash-full";
                else {
                    const std::string GLASS = STYLE == "light" || STYLE == "dark" || STYLE == "clear" ? STYLE : (DARK ? "light" : "dark");
                    def = (DIR / ("trash-default-" + ID.substr(6) + ".svg")).string();
                    const auto SVG = BuiltinIcons::svg(BuiltinIcons::trashName(EMPTY, GLASS, PAPERS), HEX);
                    std::error_code ec;
                    std::filesystem::create_directories(DIR, ec);
                    std::ifstream     in(def);
                    std::stringstream old;
                    old << in.rdbuf();
                    if (old.str() != SVG) {
                        std::ofstream out(def + ".tmp", std::ios::trunc);
                        out << SVG;
                        out.close();
                        std::filesystem::rename(def + ".tmp", def, ec);
                    }
                }
                states += ID + "\t" + def + "\n";
            }
        }
        static std::string lastStates;
        if (KEY != lastKey && states != lastStates) {
            lastStates = states;
            std::error_code ec;
            const auto      P = DIR / "trash-states";
            {
                std::ofstream out(P.string() + ".tmp", std::ios::trunc);
                out << states;
            }
            std::filesystem::rename(P.string() + ".tmp", P, ec);
        }
        lastKey = KEY;
    }

    // The dock's folders, for the panel's Icons -> Folders: `id TAB name TAB
    // own icon` in dock-folders, the id being IconOverrides::folderId.
    {
        std::string folders;
        for (const auto& S : Stacks::all())
            if (S.isDir && S.path.find('\t') == std::string::npos && S.path.find('\n') == std::string::npos) {
                std::string name = S.label.empty() ? std::filesystem::path(S.path).filename().string() : S.label;
                std::ranges::replace(name, '\t', ' ');
                folders += IconOverrides::folderId(S.path) + "\t" + name + "\t" + S.icon + "\n";
            }
        static std::string lastFolders;
        if (folders != lastFolders) {
            lastFolders = folders;
            const auto      P = favoritesPath().parent_path() / "dock-folders";
            std::error_code ec;
            std::filesystem::create_directories(P.parent_path(), ec);
            {
                std::ofstream out(P.string() + ".tmp", std::ios::trunc);
                out << folders;
            }
            std::filesystem::rename(P.string() + ".tmp", P, ec);
        }
    }

    static std::string last;
    if (text == last)
        return;
    last = text;

    const auto      PATH = favoritesPath().parent_path() / "dock-apps";
    std::error_code ec;
    std::filesystem::create_directories(PATH.parent_path(), ec);
    const auto TMP = PATH.string() + ".tmp";
    {
        std::ofstream out(TMP, std::ios::trunc);
        out << text;
    }
    std::filesystem::rename(TMP, PATH, ec);
}

const std::vector<SItem>& dockItems() {
    static std::vector<SItem> cache;
    static SItemsKey          cachedKey;
    static bool               filled = false;

    if (const auto KEY = itemsKey(); !filled || !(KEY == cachedKey)) {
        cachedKey = KEY;
        filled    = true;
        cache     = buildDockItems();
        ++g_itemsGeneration;
        publishDockApps(cache);
    }

    return cache;
}

// The item generation, REFRESHED. Reading g_itemsGeneration on its own would
// hand back the number from before the rebuild the very next line triggers.
uint64_t itemsGeneration() {
    dockItems();
    return g_itemsGeneration;
}

float previewAspect(const PHLWINDOW& window) {
    const auto SIZE = window->m_realSize->value();
    if (SIZE.x < 1.0 || SIZE.y < 1.0)
        return 1.F;

    return std::clamp(sc<float>(SIZE.x / SIZE.y), PREVIEW_MIN_ASPECT, PREVIEW_MAX_ASPECT);
}

CHyprColor background() {
    return Hyprdrive::Config::dockBackgroundColor();
}

// X25. A minimized window lives on a special workspace nobody shows, so every
// one of the compositor's "is this worth drawing?" answers is no. The overview
// solves the same problem by saving the old values and restoring them on close;
// here there is nothing to restore to -- while the window is minimized, VISIBLE
// is its state -- so this just asserts it each frame.
void forcePreviewVisible(const PHLWINDOW& window) {
    window->m_hidden = false;

    // The one that actually froze the preview: a window on a hidden workspace
    // gets the xdg_toplevel `suspended` state, and a client that honours it
    // stops drawing at all -- measured 0 frame callbacks and 0 commits over 5s
    // minimized against 52/52 visible, which no amount of acking frames on our
    // side can fix. GlobalWindowController re-asserts suspension on every
    // visibility change, so this is re-checked each frame rather than set once.
    if (window->m_suspended)
        window->setSuspended(false);
    window->alpha(Desktop::View::WINDOW_ALPHA_FULLSCREEN)->setValueAndWarp(1.F);

    if (const auto WORKSPACE = window->m_workspace; WORKSPACE && WORKSPACE->m_alpha) {
        WORKSPACE->m_alpha->setValueAndWarp(1.F);
        *WORKSPACE->m_alpha = 1.F;
    }
}

// The whole difference between a live preview and a stale one. A client draws
// the next frame when the last one is acked; nobody acks a window on a hidden
// workspace, so without this the preview freezes on whatever it happened to
// have committed when it was minimized -- which the spec calls a defect.
void presentPreview(const PHLWINDOW& window, PHLMONITOR monitor, const Time::steady_tp& now) {
    std::pair<PHLMONITOR, Time::steady_tp> data = {monitor, now};

    window->wlSurface()->resource()->breadthfirst(
        [](SP<CWLSurfaceResource> surface, const Vector2D&, void* data) {
            const auto [MONITOR, NOW] = *sc<std::pair<PHLMONITOR, Time::steady_tp>*>(data);
            surface->presentFeedback(NOW, MONITOR, false);
        },
        &data);
}

float elapsedMs(const Time::steady_tp& since) {
    // Hover panel, tooltip fade, bounce, launch zoom -- and the autohide slide
    // (owner, 2026-11-10: "speed setting everywhere", "not exempt").
    return Hyprdrive::Motion::Clock::animMs(since);
}

// ---- X31 -------------------------------------------------------------------

// `workspace-agnostic-urgent-windows` is TRUE, so this asks about the APP and
// never about a workspace: any mapped window of the class, anywhere, counts.
// The LauncherEntry half is the same question asked over D-Bus -- an app can
// demand attention without owning an urgent surface (`urgent` in the protocol),
// and the reference reads both.
bool classUrgent(const std::string& wmClass) {
    for (const auto& WINDOW : Desktop::windowState()->windows()) {
        if (WINDOW && WINDOW->m_isMapped && WINDOW->m_isUrgent && Hyprdrive::AppIdentity::groupKey(WINDOW) == wmClass)
            return true;
    }

    return LauncherEntry::state(AppSearch::idForClass(wmClass)).urgent;
}

STally buildTally() {
    STally out;

    const auto CFG = interestCfg();
    for (const auto& WINDOW : Desktop::windowState()->windows()) {
        if (!WINDOW || !WINDOW->m_isMapped)
            continue;

        const auto CLASS = Hyprdrive::AppIdentity::groupKey(WINDOW);

        if (WINDOW->m_isUrgent)
            out.urgent.insert(CLASS);

        if (!interesting(WINDOW, CFG))
            continue;

        ++out.byClass[CLASS];

        // Only when it differs: for most apps the desktop id IS the class, and
        // incrementing both would count the window twice under one key.
        if (const auto ID = AppSearch::idForClass(CLASS); !ID.empty() && ID != CLASS)
            ++out.byId[ID];
    }

    // A class that IS its own id has to answer the id question too.
    for (const auto& [CLASS, COUNT] : out.byClass) {
        if (CLASS.empty())
            continue;
        if (AppSearch::idForClass(CLASS) == CLASS)
            out.byId[CLASS] += COUNT;
    }

    return out;
}

// itemWindows(item).size(), read off the tally. The union the real function
// computes collapses to ONE of the two maps: an item that carries a desktop id
// got its class from that id, so every window of the class is already in the id
// bucket.
size_t itemWindowCount(const SItem& item, const STally& tally) {
    if (item.wmClass.empty() && item.desktopId.empty())
        return 0; // itemWindows' own early return

    const auto& MAP = item.desktopId.empty() ? tally.byClass : tally.byId;
    const auto& KEY = item.desktopId.empty() ? item.wmClass : item.desktopId;

    const auto  IT  = MAP.find(KEY);
    return IT == MAP.end() ? 0 : IT->second;
}

// classUrgent(item.wmClass), read off the tally. The D-Bus half keys on the
// item's OWN desktop id rather than resolving its class again: dockItems set
// the one from the other, so they are the same string.
bool itemUrgent(const SItem& item, const STally& tally) {
    return tally.urgent.contains(item.wmClass) || LauncherEntry::state(item.desktopId).urgent;
}

// `application-counter-overrides-notifications` is TRUE, so a LauncherEntry
// count wins outright whenever the app publishes one -- which under Hyprland is
// the whole answer, because the other half of the reference's sum is GNOME
// Shell's message tray and there is no tray here to read. Named in
// RICE_PROGRESS.md as an open decision, not silently folded to zero.
//
// Keyed on the item's OWN desktop id: dockItems set that id FROM the class, so
// resolving the class again here was a lookup for a string already in hand.
long long itemBadge(const SItem& item, bool badgesOn) {
    if (!badgesOn)
        return 0;

    const auto STATE = LauncherEntry::state(item.desktopId);
    return STATE.countVisible ? std::max<long long>(0, STATE.count) : 0;
}

// The two switches itemBadge gates on, read ONCE per pass by the caller rather
// than twice per icon (user, 2026-09-14).
bool badgesEnabled() {
    return Config::getValue<bool>(KEY_BADGE) && Config::getValue<bool>(KEY_EMBLEMS);
}

bool anyUrgent(const std::vector<SItem>& items, const STally& tally) {
    // X32: a pinned app with no window has no class, and an urgent test keyed on
    // "" would ask LauncherEntry about the empty id once per item per frame.
    // X35: and a location has no class either, nor any way to go urgent.
    return std::ranges::any_of(items, [&](const SItem& item) { return item.running && item.uri.empty() && itemUrgent(item, tally); });
}

// Radians, because CBox::rot is; the reference's degrees are converted at the
// last moment so the numbers above stay the ones that were read off it. The
// epoch is shared and set once, which is what keeps two urgent icons in phase.
// ONE clock for every urgent animation, so the wiggle (dock showing) and the
// hop (dock hidden) keep the same rhythm, and two urgent apps move together.
const Time::steady_tp& urgentEpoch() {
    static const Time::steady_tp EPOCH = Time::steadyNow();
    return EPOCH;
}

float wiggleRadians() {
    const float PROGRESS = std::fmod(elapsedMs(urgentEpoch()), WIGGLE_PERIOD_MS) / WIGGLE_PERIOD_MS;
    if (PROGRESS >= WIGGLE_FRACTION)
        return 0.F;

    const float DEGREES = WIGGLE_DEGREES * std::sin(PROGRESS * 24.F * sc<float>(M_PI));
    return DEGREES * sc<float>(M_PI) / 180.F;
}

// 0..1 of the lift now, or -1 between hops.
float urgentHopLift() {
    const float PHASE = std::fmod(elapsedMs(urgentEpoch()), WIGGLE_PERIOD_MS);
    if (PHASE >= URGENT_HOP_MS)
        return -1.F;
    const float T = PHASE / URGENT_HOP_MS;
    if (T < 0.55F)
        return std::sin(sc<float>(M_PI) * T / 0.55F);
    return 0.5F * std::sin(sc<float>(M_PI) * (T - 0.55F) / 0.45F);
}

float msToNextHop() {
    return WIGGLE_PERIOD_MS - std::fmod(elapsedMs(urgentEpoch()), WIGGLE_PERIOD_MS);
}

int hopTimerCb(void*) {
    for (const auto& MON : State::monitorState()->monitors())
        if (MON)
            g_pHyprRenderer->damageMonitor(MON);
    return 0;
}

void armHopTimer(float ms) {
    if (!g_hopTimer)
        g_hopTimer = wl_event_loop_add_timer(g_pCompositor->m_wlEventLoop, hopTimerCb, nullptr);
    if (g_hopTimer)
        wl_event_source_timer_update(g_hopTimer, std::max(1, sc<int>(std::ceil(ms))));
}
}
}
