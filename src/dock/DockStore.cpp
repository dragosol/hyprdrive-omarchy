// The dock's persistence: the favourites store (polled) and the
// empty-desktop app list.
#include "DockParts.hpp"

namespace ScrollOverview::Dock {
using namespace DockDetail;
namespace DockDetail {

// -------------------------------------------------------------- X32 pins --

// Favourites are a LIST OF DESKTOP IDS, the same unit GNOME's `favorite-apps`
// key uses. A plain file, because a plugin cannot write a Hyprland config key
// back out and this is user data rather than a tunable.
std::filesystem::path favoritesPath() {
    if (const char* STATE = std::getenv("XDG_STATE_HOME"); STATE && *STATE)
        return std::filesystem::path{STATE} / ScrollOverview::Compat::NAME / "favorites";

    if (const char* HOME = std::getenv("HOME"); HOME && *HOME)
        return std::filesystem::path{HOME} / ".local" / "state" / ScrollOverview::Compat::NAME / "favorites";

    return {};
}

SFavoritesStore& favoritesStore() {
    static SFavoritesStore store;
    return store;
}

// The store is the source of truth and the plugin is NOT its only writer: a
// profile revert (hdprof), an editor, or a second session all change the file
// under a running dock. Reading it ONCE at first use made every such write
// silently invisible until the next compositor start -- which is how X34's
// slot 1 stopped being the app the store's first line names, with the hotkey
// itself working perfectly (measured 2026-08-13, zrig/danglingC/x34-*).
std::vector<std::string>& favorites() {
    auto& STORE = favoritesStore();

    const auto NOW = Time::steadyNow();
    if (STORE.loaded && std::chrono::duration<float, std::milli>(NOW - STORE.checkedAt).count() < FAVORITES_POLL_MS)
        return STORE.ids;

    STORE.checkedAt = NOW;

    // A MISSING file seeds; an EMPTY one does not. Unpinning the last
    // favourite is a thing the user did on purpose, and re-seeding it on the
    // next start would undo them silently. A store that is DELETED while the
    // dock runs keeps what it had rather than re-seeding underneath the user.
    std::error_code ec;
    const auto      PATH = favoritesPath();
    if (PATH.empty() || !std::filesystem::exists(PATH, ec)) {
        if (!STORE.loaded) {
            STORE.loaded = true;
            STORE.ids.assign(DEFAULT_FAVORITES.begin(), DEFAULT_FAVORITES.end());
            ++STORE.generation;
        }
        return STORE.ids;
    }

    // A file that has not moved since the last read is not re-read: the vector
    // this returns is MUTATED in place by pinAt/unpin/toggleFavorite before
    // writeFavorites() puts it back, and a reload underneath one of those would
    // drop the edit on the floor.
    const auto WRITTEN = std::filesystem::last_write_time(PATH, ec);
    if (ec || (STORE.loaded && WRITTEN == STORE.stamp))
        return STORE.ids;

    STORE.loaded = true;
    STORE.stamp  = WRITTEN;
    STORE.ids.clear();
    ++STORE.generation;

    std::ifstream file(PATH);
    std::string   line;
    while (std::getline(file, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
            line.pop_back();
        if (!line.empty())
            STORE.ids.emplace_back(line);
    }

    return STORE.ids;
}

void writeFavorites() {
    // FIRST, and before the early return: the caller has already edited the
    // vector in place, so the list this process serves has changed whether or
    // not the file can be written. dockItems() is memoized on this number.
    ++favoritesStore().generation;

    const auto PATH = favoritesPath();
    if (PATH.empty())
        return;

    std::error_code ec;
    std::filesystem::create_directories(PATH.parent_path(), ec);

    {
        std::ofstream out(PATH, std::ios::trunc);
        for (const auto& F : favorites())
            out << F << "\n";
    }

    // Adopt the mtime of what we just wrote, or the next poll reads our own
    // output back in -- harmless in content, but it would clear and refill the
    // cache on every pin.
    auto& STORE = favoritesStore();
    STORE.stamp = std::filesystem::last_write_time(PATH, ec);
}

// ALWAYS OPEN IN AN EMPTY WORKSPACE (user, 2026-10-29: "Add an option in dock
// context menu to always spawn certain apps in an empty desktop"). The apps, by
// desktop id, one per line, beside the favourites; read again only when the
// file moved. A missing file is an empty list.
std::filesystem::path emptyDesktopAppsPath() {
    const auto FAV = favoritesPath();
    return FAV.empty() ? FAV : FAV.parent_path() / "empty-desktop-apps";
}

SEmptyDesktopStore& emptyDesktopStore() {
    static SEmptyDesktopStore store;
    return store;
}

const std::vector<std::string>& emptyDesktopApps() {
    auto&           STORE = emptyDesktopStore();
    std::error_code ec;
    const auto      PATH = emptyDesktopAppsPath();
    if (PATH.empty() || !std::filesystem::exists(PATH, ec)) {
        STORE.ids.clear();
        STORE.present = false;
        return STORE.ids;
    }
    const auto WRITTEN = std::filesystem::last_write_time(PATH, ec);
    if (ec || (STORE.present && WRITTEN == STORE.stamp))
        return STORE.ids;
    STORE.present = true;
    STORE.stamp   = WRITTEN;
    STORE.ids.clear();
    std::ifstream file(PATH);
    std::string   line;
    while (std::getline(file, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
            line.pop_back();
        if (!line.empty())
            STORE.ids.emplace_back(line);
    }
    return STORE.ids;
}

bool opensInEmptyDesktop(const std::string& id) {
    return !id.empty() && std::ranges::contains(emptyDesktopApps(), id);
}

void toggleEmptyDesktop(const std::string& id) {
    if (id.empty())
        return;
    auto IDS = emptyDesktopApps();
    if (const auto IT = std::ranges::find(IDS, id); IT != IDS.end())
        IDS.erase(IT);
    else
        IDS.emplace_back(id);
    const auto PATH = emptyDesktopAppsPath();
    if (PATH.empty())
        return;
    std::error_code ec;
    std::filesystem::create_directories(PATH.parent_path(), ec);
    {
        std::ofstream out(PATH, std::ios::trunc);
        for (const auto& I : IDS)
            out << I << "\n";
    }
    auto& STORE   = emptyDesktopStore();
    STORE.ids     = IDS;
    STORE.present = true;
    STORE.stamp   = std::filesystem::last_write_time(PATH, ec);
}

void toggleFavorite(const std::string& id) {
    if (id.empty())
        return;

    auto& FAV = favorites();
    if (const auto IT = std::ranges::find(FAV, id); IT != FAV.end())
        FAV.erase(IT);
    else
        FAV.emplace_back(id);

    writeFavorites();
}
}
}
