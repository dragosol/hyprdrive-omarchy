// Dock stacks: the store -- the stacks file read and written, and the lookups on it.
// Split out of DockStacks.cpp; the state is still there (StackParts.hpp).
#include "StackParts.hpp"

using namespace StacksDetail;

namespace StacksDetail {

    // ---------------------------------------------------------------- store --
    //
    // VERSIONED, and the favourites file is not -- so this introduces the
    // convention rather than following one. The header is a `#` comment so a
    // reader that predates a future v2 sees a file of comments and blank
    // records rather than garbage, and an unknown version is IGNORED rather
    // than rewritten: a newer session's stacks are not something an older one
    // should silently truncate.
    //
    // One record per line, tab separated, path LAST so a path containing a tab
    // (legal on Linux) still round-trips:
    //
    //     # hyprdrive stacks v1
    //     fan<TAB>name<TAB>/home/dragos/Downloads
    // The FORMAT lives in StackModel (host-tested: v1 upgrade, v2 round trip,
    // a newer build's file read-only); this file only does the I/O.

    std::filesystem::path storePath() {
        if (const char* STATE = std::getenv("XDG_STATE_HOME"); STATE && *STATE)
            return std::filesystem::path{STATE} / Hyprdrive::Compat::NAME / "stacks";
        if (const char* HOME = std::getenv("HOME"); HOME && *HOME)
            return std::filesystem::path{HOME} / ".local" / "state" / Hyprdrive::Compat::NAME / "stacks";
        return {};
    }

    SStore& store() {
        static SStore s;
        return s;
    }

    std::vector<std::string> split(const std::string& line, char sep, int maxParts) {
        std::vector<std::string> out;
        size_t                   at = 0;
        while (sc<int>(out.size()) < maxParts - 1) {
            const auto NEXT = line.find(sep, at);
            if (NEXT == std::string::npos)
                break;
            out.emplace_back(line.substr(at, NEXT - at));
            at = NEXT + 1;
        }
        out.emplace_back(line.substr(at));
        return out;
    }

    // Trailing slashes are stripped on the way IN so `/home/x/Downloads` and
    // `/home/x/Downloads/` are one stack and not two -- otherwise unpin would
    // silently miss the cell the user is looking at.
    std::string canonical(const std::string& path) {
        std::string out = path;
        if (out.starts_with("file://"))
            out = out.substr(7);
        while (out.size() > 1 && out.back() == '/')
            out.pop_back();
        return out;
    }

    void reload(SStore& s) {
        std::error_code ec;
        const auto      PATH   = storePath();
        const bool      EXISTS = !PATH.empty() && std::filesystem::exists(PATH, ec);

        if (!EXISTS) {
            s.loaded = true;
            if (StackModel::shouldSeed(false, StackModel::SStore{.seeded = g_storeState.seeded})) {
                // FIRST RUN: Downloads, as macOS does (dock stacks A). `seeded` is
                // written with it, so removing Downloads later keeps it removed.
                g_storeState.seeded = true;
                s.items.clear();
                if (const char* DL = g_get_user_special_dir(G_USER_DIRECTORY_DOWNLOAD); DL && std::filesystem::is_directory(DL, ec)) {
                    s.items.emplace_back(Stacks::SStack{.path = canonical(DL)});
                    decorate(s.items.back());
                }
                ++g_storeGeneration;
                writeStore();
            } else if (!s.items.empty()) {
                s.items.clear();
                ++g_storeGeneration;
            }
            return;
        }

        const auto WRITTEN = std::filesystem::last_write_time(PATH, ec);
        if (ec || (s.loaded && WRITTEN == s.stamp))
            return;

        s.loaded = true;
        s.stamp  = WRITTEN;
        ++g_storeGeneration;

        std::ifstream     file(PATH);
        std::stringstream buf;
        buf << file.rdbuf();
        const auto PARSED = StackModel::parseStore(buf.str());

        g_storeState.readOnly = PARSED.readOnly;
        // A v1 file, or any file with stacks in it, means the user has been here:
        // never seed on top of it.
        g_storeState.seeded = true;
        s.items.clear();
        for (const auto& R : PARSED.records) {
            s.items.emplace_back(Stacks::SStack{.path = canonical(R.path), .sort = R.sort, .display = R.display, .view = R.view, .customLabel = R.label});
            decorate(s.items.back());
        }
    }

    void writeStore() {
        const auto PATH = storePath();
        if (PATH.empty() || g_storeState.readOnly)
            return;

        StackModel::SStore out;
        out.seeded = g_storeState.seeded;
        for (const auto& S : store().items)
            out.records.push_back({.path = S.path, .sort = S.sort, .display = S.display, .view = S.view, .label = S.customLabel});

        std::error_code ec;
        std::filesystem::create_directories(PATH.parent_path(), ec);
        // Whole or not at all (StoreFile.hpp): on a full disk the old write
        // renamed an empty temp file over this one, and every pinned stack was
        // gone at the next login. A failed save leaves the file as it was; the
        // edit still shows, for this session only.
        if (const int ERR = StoreFile::replace(PATH.string(), StackModel::formatStore(out)); ERR != 0) {
            std::fprintf(stderr, "[hyprdrive] stacks: could not save %s (%s); the file on disk is unchanged\n", PATH.c_str(), std::strerror(ERR));
        } else {
            // Adopt our own mtime so the next poll does not read our output
            // back -- which is why the generation moves HERE too.
            store().stamp = std::filesystem::last_write_time(PATH, ec);
        }
        ++g_storeGeneration;
    }

    Stacks::SStack* find(const std::string& path) {
        const auto CANON = canonical(path);
        for (auto& S : store().items) {
            if (S.path == CANON)
                return &S;
        }
        return nullptr;
    }

    // Declared above reload(); defined here, where folderIcon/iconNameForFile
    // and canonical() are all in scope.
    void decorate(Stacks::SStack& stack) {
        std::error_code ec;
        stack.isDir = std::filesystem::is_directory(stack.path, ec) && !ec;
        stack.icon  = stack.isDir ? folderIcon(stack.path) : iconNameForFile(stack.path, false);

        const auto SLASH = stack.path.rfind('/');
        const auto BASE  = SLASH == std::string::npos ? stack.path : stack.path.substr(SLASH + 1);
        stack.label      = stack.customLabel.empty() ? BASE : stack.customLabel;
    }

}

