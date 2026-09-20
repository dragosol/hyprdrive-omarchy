#include "DockStacks.hpp"

#include "AppSearch.hpp"
#include "Compat.hpp"
#include "Config.hpp"
#include "LauncherEntry.hpp"
#include "OverviewRender.hpp"
#include "OverviewPassElement.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <gio/gio.h>

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/pass/TexPassElement.hpp>
#include <hyprland/src/state/MonitorState.hpp>

using namespace ScrollOverview;

namespace {

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
    constexpr const char* STORE_HEADER  = "# hyprdrive stacks v1";
    constexpr int         STORE_VERSION = 1;

    // Same reasoning as FAVORITES_POLL_MS: all() is on the layout path, which
    // runs about ten times a frame, and the file changes about once a week.
    constexpr float POLL_MS = 500.F;

    std::filesystem::path storePath() {
        if (const char* STATE = std::getenv("XDG_STATE_HOME"); STATE && *STATE)
            return std::filesystem::path{STATE} / ScrollOverview::Compat::NAME / "stacks";
        if (const char* HOME = std::getenv("HOME"); HOME && *HOME)
            return std::filesystem::path{HOME} / ".local" / "state" / ScrollOverview::Compat::NAME / "stacks";
        return {};
    }

    struct SStore {
        std::vector<Stacks::SStack>     items;
        std::filesystem::file_time_type stamp{};
        Time::steady_tp                 checkedAt{};
        bool                            loaded = false;
    };

    SStore& store() {
        static SStore s;
        return s;
    }

    // See Stacks::generation(). Moved by reload() when the file actually read
    // into something different, and by writeStore() for this process's own
    // edits (whose mtime reload deliberately adopts, so it would not see them).
    uint64_t g_storeGeneration = 1;

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

    // The three answers a stack's CELL needs, filled in once per row. Every one
    // of them touches the filesystem, so this is deliberately on the store's
    // read path and not on the dock's build path -- see SStack in the header.
    void decorate(Stacks::SStack& stack);

    void reload(SStore& s) {
        std::error_code ec;
        const auto      PATH = storePath();
        if (PATH.empty() || !std::filesystem::exists(PATH, ec)) {
            // No file is no stacks. Unlike favourites there is nothing to seed
            // with: a stack is a path on THIS machine and there is no neutral
            // default that is not a guess about the user's home.
            s.loaded = true;
            if (!s.items.empty()) {
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
        s.items.clear();
        ++g_storeGeneration;

        std::ifstream file(PATH);
        std::string   line;
        int           version = STORE_VERSION;
        bool          first   = true;
        while (std::getline(file, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                line.pop_back();
            if (line.empty())
                continue;

            if (first && line.starts_with("# hyprdrive stacks v")) {
                first   = false;
                version = std::atoi(line.c_str() + 20);
                if (version > STORE_VERSION) {
                    // A file written by a newer build. Read nothing, and --
                    // load-bearing -- do not write either: see writeStore.
                    s.items.clear();
                    return;
                }
                continue;
            }
            first = false;
            if (line[0] == '#')
                continue;

            const auto PARTS = split(line, '\t', 3);
            if (PARTS.size() < 3 || PARTS[2].empty())
                continue;

            s.items.emplace_back(Stacks::SStack{
                .path       = canonical(PARTS[2]),
                .grid       = PARTS[0] == "grid",
                .sortByTime = PARTS[1] == "mtime",
            });
            decorate(s.items.back());
        }
    }

    void writeStore() {
        const auto PATH = storePath();
        if (PATH.empty())
            return;

        std::error_code ec;
        std::filesystem::create_directories(PATH.parent_path(), ec);
        {
            std::ofstream out(PATH, std::ios::trunc);
            out << STORE_HEADER << "\n";
            for (const auto& S : store().items)
                out << (S.grid ? "grid" : "fan") << "\t" << (S.sortByTime ? "mtime" : "name") << "\t" << S.path << "\n";
        }
        // Adopt our own mtime so the next poll does not read our output back --
        // which is exactly why the generation has to move HERE as well: reload()
        // will never notice this write, and the dock's item cache hangs off it.
        store().stamp = std::filesystem::last_write_time(PATH, ec);
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

    // ------------------------------------------------------------- contents --
    struct SEntry {
        std::string path;
        std::string name;
        std::string icon;
        bool        dir = false;
        int64_t     mtime = 0;
    };

    // GIcon -> the first themed name, which is what OverviewRender::appIcon
    // resolves. Locations.cpp has the same four lines in its own anonymous
    // namespace; copying them is cheaper than exporting a GIcon* across a
    // header that no other module should need gio for.
    std::string themedName(GIcon* icon) {
        if (!icon || !G_IS_THEMED_ICON(icon))
            return {};
        const char* const* NAMES = g_themed_icon_get_names(G_THEMED_ICON(icon));
        return NAMES && NAMES[0] ? std::string{NAMES[0]} : std::string{};
    }

    // THE MIME LOOKUP IS NAME-ONLY, ON PURPOSE. g_content_type_guess with no
    // data buffer decides from the filename alone: no open(), no read(), no
    // stat beyond what the directory walk already did. Locations.cpp records
    // what a synchronous gio query on this thread costs -- a `trash:///`
    // query_info wedged the compositor -- and a fan of 200 files would be 200
    // of them.
    std::string iconNameForFile(const std::string& path, bool isDir) {
        if (isDir)
            return "folder";

        gboolean uncertain = FALSE;
        char*    ct        = g_content_type_guess(path.c_str(), nullptr, 0, &uncertain);
        if (!ct)
            return "text-x-generic";

        GIcon*     icon = g_content_type_get_icon(ct);
        const auto NAME = themedName(icon);
        if (icon)
            g_object_unref(icon);
        g_free(ct);

        return NAME.empty() ? std::string{"text-x-generic"} : NAME;
    }

    // The reference's special-directory table, which is the one part of its
    // icon story worth copying exactly: a Downloads stack that draws a generic
    // folder is indistinguishable from every other folder on the dock.
    std::string folderIcon(const std::string& path) {
        static const std::array<std::pair<GUserDirectory, const char*>, 8> TABLE = {{
            {G_USER_DIRECTORY_DOWNLOAD, "folder-download"},
            {G_USER_DIRECTORY_DOCUMENTS, "folder-documents"},
            {G_USER_DIRECTORY_MUSIC, "folder-music"},
            {G_USER_DIRECTORY_PICTURES, "folder-pictures"},
            {G_USER_DIRECTORY_VIDEOS, "folder-videos"},
            {G_USER_DIRECTORY_DESKTOP, "user-desktop"},
            {G_USER_DIRECTORY_TEMPLATES, "folder-templates"},
            {G_USER_DIRECTORY_PUBLIC_SHARE, "folder-publicshare"},
        }};

        for (const auto& [DIR, ICON] : TABLE) {
            const char* SPECIAL = g_get_user_special_dir(DIR);
            if (SPECIAL && canonical(SPECIAL) == path)
                return ICON;
        }
        if (const char* HOME = g_get_home_dir(); HOME && canonical(HOME) == path)
            return "user-home";
        return "folder";
    }

    // Read once per OPEN, never per frame. A directory walk is a local syscall
    // and this is the input path, not the render path -- but it is still not
    // something to repeat 120 times a second, and the popup owns the answer for
    // as long as it is up.
    std::vector<SEntry> readDir(const std::string& dir, bool sortByTime) {
        std::vector<SEntry> out;

        std::error_code ec;
        auto            it = std::filesystem::directory_iterator{dir, std::filesystem::directory_options::skip_permission_denied, ec};
        if (ec)
            return out;

        // The reference caps at 500 "to prevent pathological memory bloat" and
        // that number is right for the same reason here: past it the popup is
        // not a browsing surface, it is a file manager nobody asked for.
        constexpr size_t HARD_CAP = 500;

        for (const auto& E : it) {
            if (out.size() >= HARD_CAP)
                break;

            const auto NAME = E.path().filename().string();
            if (NAME.empty() || NAME[0] == '.') // hidden, as the reference does
                continue;

            std::error_code e2;
            const bool      IS_DIR = E.is_directory(e2);

            int64_t mtime = 0;
            if (const auto T = std::filesystem::last_write_time(E.path(), e2); !e2)
                mtime = sc<int64_t>(T.time_since_epoch().count());

            out.emplace_back(SEntry{
                .path  = E.path().string(),
                .name  = NAME,
                .icon  = IS_DIR ? folderIcon(canonical(E.path().string())) : iconNameForFile(E.path().string(), false),
                .dir   = IS_DIR,
                .mtime = mtime,
            });
        }

        // DEVIATION from the reference, which sorts by mtime only and does not
        // group directories. Directories first is what every file manager on
        // this desktop does, and a stack whose folders are scattered through
        // its files reads as a bug rather than as a choice.
        std::ranges::sort(out, [sortByTime](const SEntry& a, const SEntry& b) {
            if (a.dir != b.dir)
                return a.dir;
            if (sortByTime)
                return a.mtime > b.mtime; // newest first, as the reference does
            // Case-insensitive, because "Zebra" sorting before "apple" is the
            // ASCII table talking and not the user's alphabet.
            return std::lexicographical_compare(a.name.begin(), a.name.end(), b.name.begin(), b.name.end(),
                                                [](unsigned char x, unsigned char y) { return std::tolower(x) < std::tolower(y); });
        });

        return out;
    }

    // ------------------------------------------------------------- geometry --
    //
    // Sized off the reference's proportions, scaled to this dock's 54 px icon
    // rather than the Dash's 48, and with one deliberate departure: the
    // TIMINGS. The reference eases a row in over 140 ms with an 11 ms stagger.
    // The house law is a 200-400 ms band with entrances decelerating and exits
    // 20-30% faster, and 140 ms is under the floor of it -- so the row duration
    // is 220 ms and the stagger 14, which puts the whole 13-row fan at 388 ms
    // and every individual row inside the band.
    constexpr float FAN_ICON     = 40.F;  // px, unscaled
    constexpr float FAN_ROW_H    = 52.F;
    constexpr float FAN_STEP     = 56.F;  // row height + 4 px gap, the reference's 64/68 ratio
    constexpr float FAN_SHIFT_K  = 2.5F;  // xShift = K * i^POW, straight from the reference
    constexpr float FAN_SHIFT_P  = 1.8F;
    constexpr float FAN_TILT_DEG = 2.5F;  // per row, clockwise
    constexpr float FAN_PAD_X    = 10.F;
    constexpr float FAN_RADIUS   = 10.F;
    constexpr float FAN_GAP      = 10.F;  // between the dock pill and the first row
    constexpr int   FAN_FONT     = 13;
    constexpr int   FAN_LABEL_MAX = 260;  // px, unscaled: ellipsize past this

    constexpr float GRID_COLS    = 4.F;
    constexpr float GRID_CELL_W  = 92.F;
    constexpr float GRID_CELL_H  = 104.F;
    constexpr float GRID_ICON    = 48.F;
    constexpr float GRID_SPACING = 12.F;
    constexpr float GRID_PAD     = 14.F;
    constexpr float GRID_MAX_ROWS = 3.F;  // past this the panel scrolls
    constexpr float GRID_RADIUS  = 14.F;
    constexpr int   GRID_FONT    = 11;

    constexpr float OPEN_MS  = 220.F;
    constexpr float STAGGER_MS = 14.F;
    constexpr float CLOSE_MS = 165.F; // 25% quicker than the entrance, per the law

    // DEVIATION, and an improvement the brief asked for. The reference caps the
    // fan at `fan-threshold` items and silently DROPS the rest -- `items.splice`
    // with no affordance anywhere that anything is missing. A stack of 40 files
    // showing 12 with no sign of it is a lie about the folder. So the cap keeps
    // one row for an honest "N more..." that opens the folder in Files.
    constexpr size_t FAN_MAX = 12;

    // ----------------------------------------------------------- popup state --
    struct SRow {
        CBox        box;     // monitor-local RENDER px, as DRAWN last frame
        std::string path;    // empty: the "N more..." / action row
        std::string name;
        std::string icon;
        bool        action = false;
    };

    struct SPopup {
        std::string         path;
        bool                grid = false;
        bool                sortByTime = false;
        MONITORID           monitor = MONITOR_INVALID;
        CBox                anchor;  // the cell it grew out of
        std::vector<SEntry> entries;
        size_t              shown = 0;   // rows drawn before the action row
        size_t              hidden = 0;  // how many "N more..." stands for
        std::vector<SRow>   rows;        // published for the pointer
        CBox                plate;       // grid only: the whole panel, as drawn
        float               scroll = 0.F;
        Time::steady_tp     since{};
        bool                leaving = false;
    };

    std::optional<SPopup> g_popup;

    // THE MOVE GESTURE. Dragging a file out of a fan/grid and onto another
    // stack's cell -- which is the ONE drag that can reach this dock at all
    // (see DockStacks.hpp for why an XDG drag from Nautilus cannot).
    //
    // There is no motion hook here on purpose. render() already runs every
    // frame and already reads g_pInputManager->getMouseCoordsInternal() to
    // decide what is hovered; the ghost rides that same read, so the gesture
    // costs one press handler, one release handler and no new input path.
    struct SItemDrag {
        std::string srcPath;
        std::string icon;
        Vector2D    start;
        bool        live = false;
    };
    std::optional<SItemDrag> g_itemDrag;

    constexpr float DRAG_THRESHOLD = 12.F; // px, the reference's, and euclidean here
    constexpr float GHOST_SIZE     = 48.F;

    // ------------------------------------------------------------ animations --
    // Y8's hop, keyed on a PATH: armBounce keys on wmClass and a stack has none,
    // so the LEDGER is a second one. The motion is not -- the duration and the
    // curve are Dock::HOP_MS and Dock::hopLift, so a stack cell and an app icon
    // hop identically and cannot be re-timed apart.

    // The ERROR SHAKE V4 asks for, which this dock did not have -- the nearest
    // thing was X31's urgent wiggle, and that is a 3000 ms attention loop, not
    // an acknowledgement. On the record as an exemption for the same reason the
    // wiggle is: a shake that eased would not read as a refusal.
    constexpr float SHAKE_MS     = 300.F;
    constexpr float SHAKE_CYCLES = 3.F;
    constexpr float SHAKE_AMP    = 0.16F; // of one icon width

    struct SHop {
        std::string     path;
        Time::steady_tp startedAt;
    };

    std::vector<SHop> g_bounces;
    std::vector<SHop> g_shakes;

    float elapsed(const Time::steady_tp& since) {
        return std::chrono::duration<float, std::milli>(Time::steadyNow() - since).count();
    }

    // ------------------------------------------------------------ fan layout --
    // Laid out into `rows`, in DRAWN coordinates, so the hit test and the paint
    // can never disagree: the same function fills both, exactly as renderMenu
    // does for the context plate.
    void layoutFan(SPopup& p, PHLMONITOR monitor, const CBox& pill, float progress) {
        const float SCALE = std::max(monitor->m_scale, 0.01F);
        const float CX    = sc<float>(p.anchor.x) + sc<float>(p.anchor.width) / 2.F;
        const float BASE  = sc<float>(pill.y) - FAN_GAP * SCALE;

        // shown file rows, then ONE action row: "N more..." when the cap bit,
        // "Open in Files" otherwise, "Empty folder" when there is nothing. The
        // fan always has at least one row, so a click always shows something --
        // the reference returns early on an empty folder and the click is
        // indistinguishable from a dead dock.
        const size_t ROWS = p.shown + 1;

        p.rows.clear();
        p.rows.reserve(ROWS);
        for (size_t i = 0; i < ROWS; ++i) {
            const bool  ACTION = i >= p.shown;
            const float FI     = sc<float>(i);
            const float SHIFT  = FAN_SHIFT_K * std::pow(FI, FAN_SHIFT_P) * SCALE;

            // The stagger: row i has not started until i * STAGGER_MS has gone
            // by, and then runs its own OPEN_MS. Read off ONE clock so the fan
            // cannot tear.
            const float T0 = FI * STAGGER_MS / std::max(1.F, OPEN_MS + STAGGER_MS * sc<float>(p.shown));
            const float TI = std::clamp((progress - T0) / std::max(0.001F, 1.F - T0), 0.F, 1.F);
            const float E  = p.leaving ? 1.F - houseEase(false, TI) : houseEase(true, TI);

            const float ROWH = FAN_ROW_H * SCALE;
            const float RESTY = BASE - FAN_STEP * SCALE * (FI + 1.F);
            // Grows OUT of the icon: at E=0 the row sits collapsed on the cell,
            // which is the hover panel's entrance and the reference's both.
            const float Y = sc<float>(p.anchor.y) + (RESTY - sc<float>(p.anchor.y)) * E;

            std::string LABEL, ICON;
            if (!ACTION) {
                LABEL = p.entries[i].name;
                ICON  = p.entries[i].icon;
            } else if (p.hidden > 0) {
                LABEL = std::to_string(p.hidden) + " more\xe2\x80\xa6";
                ICON  = "folder-open";
            } else if (p.entries.empty()) {
                LABEL = "Empty folder";
                ICON  = "folder-open";
            } else {
                LABEL = "Open in Files";
                ICON  = "system-file-manager";
            }

            // The row's width is its label plus its icon; measured through the
            // cached text path, which is the only legal way to ask inside a
            // pass and the only affordable way anywhere.
            float labelW = 0.F;
            if (const auto TEX = OverviewRender::text(LABEL, CHyprColor{1.F, 1.F, 1.F, 0.95F}, sc<int>(std::round(FAN_FONT * SCALE)), false, "",
                                                      sc<int>(std::round(FAN_LABEL_MAX * SCALE)), 500);
                TEX && TEX->ok())
                labelW = sc<float>(TEX->m_size.x);

            const float ICONW = FAN_ICON * SCALE;
            const float W     = FAN_PAD_X * SCALE * 2.F + labelW + (labelW > 0.F ? FAN_PAD_X * SCALE : 0.F) + ICONW;

            // Right-aligned on the anchor's centre plus the shift, so the ICON
            // column tracks the fan's arc and the labels ragged out to the left
            // -- which is the reference's shape.
            CBox box{std::round(CX + SHIFT - W + ICONW / 2.F + FAN_PAD_X * SCALE), std::round(Y), std::round(W), std::round(ROWH)};
            box.x = std::max(0.0, std::min(box.x, sc<double>(monitor->m_size.x * SCALE) - box.width));

            p.rows.emplace_back(SRow{
                .box    = box,
                .path   = ACTION ? std::string{} : p.entries[i].path,
                .name   = LABEL,
                .icon   = ICON,
                .action = ACTION,
            });
        }
    }

    void layoutGrid(SPopup& p, PHLMONITOR monitor, const CBox& pill, float progress) {
        const float SCALE = std::max(monitor->m_scale, 0.01F);
        const float CELLW = GRID_CELL_W * SCALE;
        const float CELLH = GRID_CELL_H * SCALE;
        const float SP    = GRID_SPACING * SCALE;
        const float PAD   = GRID_PAD * SCALE;

        const size_t N    = p.entries.size() + 1; // + the Open in Files action
        const size_t COLS = sc<size_t>(GRID_COLS);
        const size_t ROWS = (N + COLS - 1) / COLS;
        const float  VIEWROWS = std::min(sc<float>(ROWS), GRID_MAX_ROWS);

        const float W = PAD * 2.F + GRID_COLS * CELLW + (GRID_COLS - 1.F) * SP;
        const float H = PAD * 2.F + VIEWROWS * CELLH + std::max(0.F, VIEWROWS - 1.F) * SP;

        const float E = p.leaving ? 1.F - houseEase(false, progress) : houseEase(true, progress);

        const float CX    = sc<float>(p.anchor.x) + sc<float>(p.anchor.width) / 2.F;
        const float RESTY = sc<float>(pill.y) - FAN_GAP * SCALE - H;

        // Grows out of the cell, like the hover panel: interpolate the whole
        // plate from the anchor box to the settled one.
        CBox plate{
            sc<double>(sc<float>(p.anchor.x) + (CX - W / 2.F - sc<float>(p.anchor.x)) * E),
            sc<double>(sc<float>(p.anchor.y) + (RESTY - sc<float>(p.anchor.y)) * E),
            sc<double>(sc<float>(p.anchor.width) + (W - sc<float>(p.anchor.width)) * E),
            sc<double>(sc<float>(p.anchor.height) + (H - sc<float>(p.anchor.height)) * E),
        };
        plate.x = std::clamp(plate.x, 0.0, std::max(0.0, monitor->m_size.x * SCALE - plate.width));
        p.plate = plate;

        // Scroll clamp, in whole rows of travel.
        const float MAXSCROLL = std::max(0.F, sc<float>(ROWS) - VIEWROWS) * (CELLH + SP);
        p.scroll              = std::clamp(p.scroll, 0.F, MAXSCROLL);

        p.rows.clear();
        for (size_t i = 0; i < N; ++i) {
            const bool  ACTION = i >= p.entries.size();
            const float COL    = sc<float>(i % COLS);
            const float ROW    = sc<float>(i / COLS);

            CBox box{
                std::round(sc<float>(plate.x) + PAD + COL * (CELLW + SP)),
                std::round(sc<float>(plate.y) + PAD + ROW * (CELLH + SP) - p.scroll),
                std::round(CELLW),
                std::round(CELLH),
            };

            // Clipped OUT rather than clipped: there is no scissor on this
            // path, so a cell that does not fit inside the plate is simply not
            // a row this frame -- which also keeps it out of the hit test.
            if (box.y < plate.y + PAD * 0.5 - 1 || box.y + box.height > plate.y + plate.height - PAD * 0.5 + 1)
                continue;

            p.rows.emplace_back(SRow{
                .box    = box,
                .path   = ACTION ? std::string{} : p.entries[i].path,
                .name   = ACTION ? std::string{"Open in Files"} : p.entries[i].name,
                .icon   = ACTION ? std::string{"system-file-manager"} : p.entries[i].icon,
                .action = ACTION,
            });
        }
    }

    // The popup no longer asks for a frame just for existing (Stacks::
    // needsFrames), so every edit that changes what it looks like WITHOUT
    // moving its entrance has to ask for one itself. All of these arrive on an
    // input event, never from inside the pass, so a plain damage is legal here.
    void damagePopupMonitor() {
        if (!g_popup || !g_pHyprRenderer)
            return;
        for (const auto& MONITOR : State::monitorState()->monitors()) {
            if (MONITOR && MONITOR->m_id == g_popup->monitor)
                g_pHyprRenderer->damageMonitor(MONITOR);
        }
    }

    float popupProgress(const SPopup& p) {
        const float MS   = elapsed(p.since);
        const float SPAN = p.leaving ? CLOSE_MS : (OPEN_MS + STAGGER_MS * sc<float>(std::min<size_t>(p.shown, FAN_MAX)));
        return std::clamp(MS / std::max(1.F, SPAN), 0.F, 1.F);
    }

    // Declared above reload(); defined here, where folderIcon/iconNameForFile
    // and canonical() are all in scope.
    void decorate(Stacks::SStack& stack) {
        std::error_code ec;
        stack.isDir = std::filesystem::is_directory(stack.path, ec) && !ec;
        stack.icon  = stack.isDir ? folderIcon(stack.path) : iconNameForFile(stack.path, false);

        const auto SLASH = stack.path.rfind('/');
        stack.label      = SLASH == std::string::npos ? stack.path : stack.path.substr(SLASH + 1);
    }

    void launchPath(const std::string& path) {
        if (path.empty())
            return;
        GError* err  = nullptr;
        char*   uri  = g_filename_to_uri(path.c_str(), nullptr, &err);
        if (!uri) {
            if (err)
                g_error_free(err);
            return;
        }
        g_app_info_launch_default_for_uri(uri, nullptr, nullptr);
        g_free(uri);
    }

    // ---------------------------------------------------------- the preview --
    // V11 SPEC V5. GNOME-SUSHI, over the interface Nautilus itself calls.
    // MEASURED in the guest rather than guessed --
    //
    //   busctl --user introspect org.gnome.NautilusPreviewer \
    //                            /org/gnome/NautilusPreviewer
    //   org.gnome.NautilusPreviewer2.ShowFile(s uri, s windowHandle,
    //                                         b closeIfAlreadyShown,
    //                                         s activationToken)
    //   org.gnome.NautilusPreviewer2.Close()
    //
    // -- and note the 2: the bus NAME and the OBJECT are NautilusPreviewer,
    // the INTERFACE is NautilusPreviewer2. Guessing the obvious one is a
    // silent UnknownMethod on a call that asks for no answer.
    //
    // closeIfAlreadyShown = TRUE IS THE WHOLE TOGGLE, and it is sushi's own
    // semantics rather than an invention here: measured in the guest, the same
    // uri while the previewer is up CLOSES it, the same uri again brings it
    // back, and a DIFFERENT uri swaps the preview without closing anything.
    // That is exactly what V5 asks SPACE to do -- "space again, or hover
    // elsewhere + space" -- so this end keeps no state at all. There is
    // nothing to get out of step with a window the plugin does not own.
    //
    // windowHandle is left EMPTY. It is an xdg-foreign exported handle, which
    // Nautilus can produce because it has a surface to export and this dock
    // has none (DockStacks.hpp records why). Sushi treats an empty handle as
    // "no parent", which under Hyprland means an ordinary floating toplevel
    // -- and that is the window config/hyprdrive/25-rules.lua dresses.
    void showPreview(const char* uri) {
        sd_bus* bus = LauncherEntry::sessionBus();
        if (!bus)
            return;

        sd_bus_message* m = nullptr;
        if (sd_bus_message_new_method_call(bus, &m, "org.gnome.NautilusPreviewer", "/org/gnome/NautilusPreviewer", "org.gnome.NautilusPreviewer2", "ShowFile") < 0)
            return;

        // NO REPLY EXPECTED, which is what keeps a dbus round trip off the
        // compositor's frame. Sushi is D-BUS ACTIVATED: the first preview of a
        // session costs the bus daemon a fork and an exec, and a call that
        // waited for its answer would wait for all of it. Queued instead --
        // the daemon reads the message, starts the service and delivers it,
        // and this returns in the time it takes to write ~200 bytes to a unix
        // socket. The flush is the other half of that promise: this
        // connection's event source watches READABLE only (LauncherEntry.cpp),
        // so a message left in the write queue would have nothing to push it
        // out. Empty queue -- the normal case -- and it returns immediately.
        if (sd_bus_message_append(m, "ssbs", uri, "", 1, "") >= 0) {
            sd_bus_message_set_expect_reply(m, 0);
            if (sd_bus_send(bus, m, nullptr) >= 0)
                sd_bus_flush(bus);
        }
        sd_bus_message_unref(m);
    }

} // anonymous namespace

// ============================================================================
// EVERYTHING BELOW IS OUTSIDE THE ANONYMOUS NAMESPACE.
//
// symcheck.sh:14-16 records what happens otherwise: Dock::panelClick was once
// defined inside Dock.cpp's anonymous namespace, linked clean, and killed the
// compositor on the next mouse press when the lazy bind failed. Every symbol
// declared in DockStacks.hpp must be defined here, at namespace scope.
// ============================================================================
namespace ScrollOverview::Stacks {

    const std::vector<SStack>& all() {
        auto& S = store();

        const auto NOW = Time::steadyNow();
        if (S.loaded && std::chrono::duration<float, std::milli>(NOW - S.checkedAt).count() < POLL_MS)
            return S.items;

        S.checkedAt = NOW;
        reload(S);
        return S.items;
    }

    bool pin(const std::string& path) {
        const auto CANON = canonical(path);
        if (CANON.empty() || CANON == "/")
            return false;

        std::error_code ec;
        if (!std::filesystem::exists(CANON, ec) || ec)
            return false;

        all(); // make sure the cache is current before we mutate it
        if (find(CANON))
            return false;

        store().items.emplace_back(SStack{.path = CANON});
        decorate(store().items.back());
        writeStore();
        return true;
    }

    bool unpin(const std::string& path) {
        const auto CANON = canonical(path);
        all();

        auto& ITEMS  = store().items;
        const auto N = ITEMS.size();
        std::erase_if(ITEMS, [&](const SStack& s) { return s.path == CANON; });
        if (ITEMS.size() == N)
            return false;

        if (isOpen() && openPath() == CANON)
            close();

        writeStore();
        return true;
    }

    bool setView(const std::string& path, bool grid) {
        all();
        auto* S = find(path);
        if (!S || S->grid == grid)
            return false;
        S->grid = grid;
        writeStore();

        // A view change with the popup up re-renders it in the new view rather
        // than leaving a fan the menu says is a grid.
        if (g_popup && g_popup->path == canonical(path)) {
            g_popup->grid  = grid;
            g_popup->since = Time::steadyNow();
            g_popup->scroll = 0.F;
            damagePopupMonitor();
        }
        return true;
    }

    bool setSort(const std::string& path, bool byTime) {
        all();
        auto* S = find(path);
        if (!S || S->sortByTime == byTime)
            return false;
        S->sortByTime = byTime;
        writeStore();

        if (g_popup && g_popup->path == canonical(path)) {
            g_popup->sortByTime = byTime;
            g_popup->entries    = readDir(g_popup->path, byTime);
            g_popup->shown      = std::min<size_t>(g_popup->entries.size(), FAN_MAX);
            g_popup->hidden     = g_popup->entries.size() - g_popup->shown;
            damagePopupMonitor();
        }
        return true;
    }

    void clearAll() {
        all();
        if (store().items.empty())
            return;
        store().items.clear();
        close();
        writeStore();
    }

    uint64_t generation() {
        // all() is what polls, so answering without it would hand back a number
        // from before a reload the caller's next line is about to see.
        all();
        return g_storeGeneration;
    }

    std::string iconFor(const std::string& path) {
        const auto      CANON = canonical(path);
        std::error_code ec;
        const bool      DIR = std::filesystem::is_directory(CANON, ec);
        return DIR ? folderIcon(CANON) : iconNameForFile(CANON, false);
    }

    std::string labelFor(const std::string& path) {
        const auto CANON = canonical(path);
        const auto SLASH = CANON.rfind('/');
        return SLASH == std::string::npos ? CANON : CANON.substr(SLASH + 1);
    }

    // ---------------------------------------------------------------- popup --
    void open(PHLMONITOR monitor, const Dock::SItem& item, const CBox& anchor) {
        if (!monitor)
            return;

        const auto CANON = canonical(item.stackPath);

        // A second click on the same cell closes it, which is the reference's
        // toggle and what every dock popup on this desktop does.
        if (g_popup && g_popup->path == CANON && !g_popup->leaving) {
            close();
            return;
        }

        std::error_code ec;
        if (!std::filesystem::is_directory(CANON, ec) || ec) {
            // A pinned FILE has no contents to fan. Clicking it opens it, which
            // is the only thing it can mean.
            launchPath(CANON);
            return;
        }

        all();
        const auto* S = find(CANON);

        SPopup p;
        p.path       = CANON;
        p.grid       = S && S->grid;
        p.sortByTime = S && S->sortByTime;
        p.monitor    = monitor->m_id;
        p.anchor     = anchor;
        p.entries    = readDir(CANON, p.sortByTime);
        p.shown      = std::min<size_t>(p.entries.size(), FAN_MAX);
        p.hidden     = p.entries.size() - p.shown;
        p.since      = Time::steadyNow();

        // An EMPTY folder still opens -- with one row saying so. The reference
        // returns early and the click does nothing at all, which is
        // indistinguishable from a broken dock.
        g_popup = std::move(p);
    }

    void close() {
        if (!g_popup || g_popup->leaving)
            return;
        g_popup->leaving = true;
        g_popup->since   = Time::steadyNow();
    }

    // Only the popup's own rows can start an item drag, so a popup that goes
    // away with a button still down must take the drag with it -- otherwise the
    // next release anywhere would resolve a drop from a fan nobody can see.
    void abandonItemDrag() {
        g_itemDrag.reset();
    }

    bool isOpen() {
        return g_popup.has_value() && !g_popup->leaving;
    }

    MONITORID popupMonitor() {
        // Not isOpen(): a popup on its way OUT is still being drawn, and the
        // caller is asking what it has to repaint, not what it can click.
        return g_popup ? g_popup->monitor : MONITOR_INVALID;
    }

    const std::string& openPath() {
        static const std::string EMPTY;
        return g_popup ? g_popup->path : EMPTY;
    }

    bool contains(PHLMONITOR monitor, const Vector2D& point) {
        if (!g_popup || g_popup->leaving || !monitor || g_popup->monitor != monitor->m_id)
            return false;

        if (g_popup->grid && !g_popup->plate.empty() && g_popup->plate.containsPoint(point))
            return true;

        for (const auto& R : g_popup->rows) {
            if (R.box.containsPoint(point))
                return true;
        }
        return false;
    }

    // V11 SPEC V5. WHAT IS UNDER THE POINTER, and therefore what SPACE means.
    //
    // Deliberately NOT a second hit test: it walks the same `rows` that
    // contains() and click() walk, in the same DRAWN coordinates, so a fan
    // that has not finished growing reports the row where the row actually is
    // this frame. The action row is skipped -- "Open in Files" is not a file
    // and has nothing to preview -- which is also why the empty answer is the
    // common one and why it has to be cheap.
    std::string hoveredItemPath() {
        if (!g_popup || g_popup->leaving)
            return {};

        PHLMONITOR monitor = nullptr;
        for (const auto& M : State::monitorState()->monitors()) {
            if (M && M->m_id == g_popup->monitor) {
                monitor = M;
                break;
            }
        }
        if (!monitor)
            return {};

        const Vector2D POINT = (g_pInputManager->getMouseCoordsInternal() - monitor->m_position) * monitor->m_scale;

        for (const auto& R : g_popup->rows) {
            if (!R.action && !R.path.empty() && R.box.containsPoint(POINT))
                return R.path;
        }
        return {};
    }

    bool previewHovered() {
        const auto PATH = hoveredItemPath();
        if (PATH.empty())
            return false;

        // A path that will not become a uri is a path sushi cannot open, so
        // NOTHING is claimed and SPACE goes where it was going. The claim and
        // the action are the same decision on purpose: there is no branch here
        // that swallows the key and then does not preview.
        GError* err = nullptr;
        char*   uri = g_filename_to_uri(PATH.c_str(), nullptr, &err);
        if (!uri) {
            if (err)
                g_error_free(err);
            return false;
        }

        showPreview(uri);
        g_free(uri);
        return true;
    }

    bool holdsDock(PHLMONITOR monitor) {
        if (!g_popup || g_popup->leaving || !monitor)
            return false;
        // The dock may not slide out from under an open popup, for the same
        // reason a flight holds it out: the popup is anchored to a cell.
        return g_popup->monitor == monitor->m_id;
    }

    bool click(PHLMONITOR monitor, const Vector2D& point, bool release) {
        if (!g_popup || g_popup->leaving || !monitor || g_popup->monitor != monitor->m_id)
            return false;

        // A RELEASE always belongs to the press that started it, wherever the
        // pointer has since gone -- that is the dock's own rule (dragPending),
        // and it is what makes a drop onto ANOTHER cell reachable at all.
        if (release && g_itemDrag) {
            const auto DRAG = *g_itemDrag;
            g_itemDrag.reset();

            if (!DRAG.live) {
                // Never moved: it was a click after all.
                for (const auto& R : g_popup->rows) {
                    if (!R.box.containsPoint(point))
                        continue;
                    launchPath(R.path.empty() ? g_popup->path : R.path);
                    close();
                    return true;
                }
                return true;
            }

            // Which stack cell is under the release? Asked of a FRESH layout:
            // the dock reflows under a drag and last frame's boxes are not
            // where the user let go.
            const auto LAYOUT = Dock::layout(monitor);
            for (const auto& ITEM : LAYOUT.items) {
                if (!ITEM.stack || !ITEM.stackDir)
                    continue;
                if (!ITEM.iconBox.containsPoint(point))
                    continue;

                const auto DEST = canonical(ITEM.stackPath);
                std::string err;
                if (moveInto(DEST, DRAG.srcPath, err)) {
                    // Y8's hop, and ONLY after gio reported success. The order
                    // is the spec's: "bounce fires only AFTER the move reports
                    // success".
                    armBounce(DEST);
                    if (g_popup)
                        g_popup->entries = readDir(g_popup->path, g_popup->sortByTime);
                    if (g_popup) {
                        g_popup->shown  = std::min<size_t>(g_popup->entries.size(), FAN_MAX);
                        g_popup->hidden = g_popup->entries.size() - g_popup->shown;
                    }
                } else
                    armShake(DEST); // refused: the file is exactly where it was

                close();
                return true;
            }

            // Released over nothing that accepts it: no move, no acknowledgement.
            close();
            return true;
        }

        const bool INSIDE = contains(monitor, point);
        if (!INSIDE) {
            // A press that misses dismisses, and is NOT claimed: the click
            // belongs to whatever is under it. That is the hover panel's
            // behaviour and the menu's `menuMissLetsThrough` on the desktop.
            if (!release)
                close();
            return false;
        }

        // Press remembers, release decides -- the dock's own gesture, so a
        // press-drag off a row does not launch it.
        if (!release) {
            for (const auto& R : g_popup->rows) {
                if (!R.box.containsPoint(point) || R.path.empty())
                    continue;
                g_itemDrag = SItemDrag{.srcPath = R.path, .icon = R.icon, .start = point};
                break;
            }
            return true;
        }

        for (const auto& R : g_popup->rows) {
            if (!R.box.containsPoint(point))
                continue;

            launchPath(R.path.empty() ? g_popup->path : R.path);
            close();
            return true;
        }
        return true;
    }

    bool scroll(PHLMONITOR monitor, const Vector2D& point, bool up) {
        if (!g_popup || g_popup->leaving || !monitor || !contains(monitor, point))
            return false;

        if (!g_popup->grid)
            return true; // the fan swallows the tick; it has nowhere to go

        const float SCALE = std::max(monitor->m_scale, 0.01F);
        g_popup->scroll += (up ? -1.F : 1.F) * (GRID_CELL_H + GRID_SPACING) * SCALE * 0.5F;
        damagePopupMonitor();
        return true;
    }

    bool needsFrames(MONITORID monitor) {
        // The hop and the shake ride a stack CELL, and every monitor's dock
        // draws that cell -- so they are answered for all of them.
        if (animating())
            return true;

        // Everything below belongs to the POPUP, and render() returns at once
        // on any monitor that is not its own. Asking globally is what kept the
        // second monitor at full refresh for a fan it was not drawing (user,
        // 2026-09-14).
        if (!g_popup || g_popup->monitor != monitor)
            return false;

        // The ghost follows the pointer, and a pointer that STOPS while the
        // button is still down emits no motion -- so without this the ghost
        // would freeze wherever the hand paused, which is the same class of
        // bug the hover panel's entrance had.
        if (g_itemDrag.has_value())
            return true;

        // An OPEN, SETTLED popup is a still image: no hover highlight, no
        // per-frame content. It used to ask for a frame anyway, for as long as
        // it was up. The two things that CAN change it while it sits there --
        // a wheel tick on a grid, and the view/sort rows of its menu -- damage
        // the monitor themselves now, because nothing else would.
        return g_popup->leaving || popupProgress(*g_popup) < 1.F;
    }

    bool animating() {
        return !g_bounces.empty() || !g_shakes.empty();
    }

    std::string visualState() {
        std::string out;
        if (g_popup) {
            out += g_popup->path;
            out += g_popup->grid ? "|g" : "|f";
            out += g_popup->leaving ? "|x" : "|o";
            out += "|" + std::to_string(sc<int>(std::round(popupProgress(*g_popup) * 100.F)));
            out += "|" + std::to_string(sc<int>(std::round(g_popup->scroll)));
        }
        for (const auto& B : g_bounces)
            out += "|b" + B.path;
        for (const auto& S : g_shakes)
            out += "|s" + S.path;
        if (g_itemDrag && g_itemDrag->live) {
            const auto M = g_pInputManager->getMouseCoordsInternal();
            out += "|d" + std::to_string(sc<int>(M.x)) + "," + std::to_string(sc<int>(M.y));
        }
        return out;
    }

    // ---------------------------------------------------------------- paint --
    void render(PHLMONITOR monitor, const Dock::SLayout& layout) {
        std::erase_if(g_bounces, [](const SHop& h) { return elapsed(h.startedAt) > Dock::HOP_MS; });
        std::erase_if(g_shakes, [](const SHop& h) { return elapsed(h.startedAt) > SHAKE_MS; });

        if (!g_popup || !monitor || g_popup->monitor != monitor->m_id)
            return;

        // The popup is anchored to a CELL, and the layout reflows underneath it
        // (an app opens, the dock slides). Re-read the anchor every frame from
        // the item that still owns this path, exactly as the reference's 16 ms
        // dock-follow timer does -- and if the cell is gone, so is the popup.
        bool found = false;
        for (const auto& ITEM : layout.items) {
            if (!ITEM.stack || canonical(ITEM.stackPath) != g_popup->path)
                continue;
            g_popup->anchor = ITEM.iconBox;
            found           = true;
            break;
        }
        if (!found || layout.pill.empty()) {
            g_popup.reset();
            return;
        }

        const float PROGRESS = popupProgress(*g_popup);
        if (g_popup->leaving && PROGRESS >= 1.F) {
            g_popup.reset();
            return;
        }

        const float SCALE = std::max(monitor->m_scale, 0.01F);
        const float ALPHA = g_popup->leaving ? 1.F - houseEase(false, PROGRESS) : houseEase(true, PROGRESS);

        if (g_popup->grid)
            layoutGrid(*g_popup, monitor, layout.pill, PROGRESS);
        else
            layoutFan(*g_popup, monitor, layout.pill, PROGRESS);

        // FROST. User law: every new surface, by default -- and through the
        // dock's own plate call, so the popup re-themes with the rest of the
        // dock rather than beside it. The 2 px floor is the popup's own: a
        // plate that thin is the entrance's first frame, not a surface.
        const auto drawPlate = [&](const CBox& box, float radius, float alpha) {
            if (box.width < 2.0 || box.height < 2.0)
                return;
            Dock::frostedPlate(monitor, box, sc<int>(std::round(radius * SCALE)), alpha);
        };

        const auto drawTex = [&](const SP<Render::ITexture>& tex, const CBox& box, float alpha, float rot = 0.F) {
            if (!tex)
                return;
            CTexPassElement::SRenderData d;
            d.tex     = tex;
            d.box     = box;
            d.box.rot = rot;
            d.a       = alpha;
            g_pHyprRenderer->m_renderPass.add(makeUnique<CTexPassElement>(d));
        };

        // THE DRAG GHOST, and the threshold that promotes a press into a drag.
        // Both ride the pointer read this function already has to do; there is
        // no motion hook and no second input path -- see the note on SItemDrag.
        const Vector2D MOUSE = (g_pInputManager->getMouseCoordsInternal() - monitor->m_position) * monitor->m_scale;
        if (g_itemDrag && !g_itemDrag->live && MOUSE.distance(g_itemDrag->start) > DRAG_THRESHOLD * SCALE)
            g_itemDrag->live = true;

        const auto drawGhost = [&] {
            if (!g_itemDrag || !g_itemDrag->live)
                return;
            const float SZ = GHOST_SIZE * SCALE;
            drawTex(OverviewRender::appIcon(g_itemDrag->icon), CBox{std::round(MOUSE.x - SZ / 2.0), std::round(MOUSE.y - SZ / 2.0), std::round(SZ), std::round(SZ)},
                    Dock::DRAG_GHOST_ALPHA);
        };

        if (g_popup->grid) {
            drawPlate(g_popup->plate, GRID_RADIUS, ALPHA);

            for (const auto& R : g_popup->rows) {
                const float ICON = GRID_ICON * SCALE;
                CBox        ib{std::round(sc<float>(R.box.x) + (sc<float>(R.box.width) - ICON) / 2.F), std::round(sc<float>(R.box.y) + GRID_SPACING * SCALE * 0.5F),
                        std::round(ICON), std::round(ICON)};
                drawTex(OverviewRender::appIcon(R.icon), ib, ALPHA);

                if (const auto TEX = OverviewRender::text(R.name, CHyprColor{1.F, 1.F, 1.F, 0.95F}, sc<int>(std::round(GRID_FONT * SCALE)), false, "",
                                                          sc<int>(std::round((GRID_CELL_W - 8.F) * SCALE)), 500);
                    TEX && TEX->ok()) {
                    CBox lb{std::round(sc<float>(R.box.x) + (sc<float>(R.box.width) - sc<float>(TEX->m_size.x)) / 2.F),
                            std::round(sc<float>(ib.y) + ICON + 6.F * SCALE), sc<double>(TEX->m_size.x), sc<double>(TEX->m_size.y)};
                    drawTex(TEX, lb, ALPHA);
                }
            }
            drawGhost();
            return;
        }

        // THE FAN. One frosted plate per row: the rows are not a rectangle, so
        // a single enclosing plate would be a panel with a fan painted on it,
        // which is a different object. Capped at FAN_MAX + 1 = 13 blur regions,
        // and only while a fan is actually up.
        for (size_t i = 0; i < g_popup->rows.size(); ++i) {
            const auto& R   = g_popup->rows[i];
            const float ROT = FAN_TILT_DEG * sc<float>(i) * 3.14159265F / 180.F;

            // Per-row alpha: a row that has not started its stagger is not
            // drawn at all rather than drawn at zero, which keeps its frost off
            // the queue too.
            const float T0 = sc<float>(i) * STAGGER_MS / std::max(1.F, OPEN_MS + STAGGER_MS * sc<float>(g_popup->shown));
            const float TI = std::clamp((PROGRESS - T0) / std::max(0.001F, 1.F - T0), 0.F, 1.F);
            const float A  = (g_popup->leaving ? 1.F - houseEase(false, PROGRESS) : houseEase(true, TI));
            if (A <= 0.02F)
                continue;

            // The frost cannot be rotated -- queueFrost takes a box, and the
            // blur pass has no transform. So the PLATE is axis-aligned and only
            // the artwork on it tilts. Measured against the alternative (no
            // tilt at all) that reads as the reference's fan; measured against
            // a rotated plate, which is not available, it is the honest
            // approximation and it is recorded here as one.
            drawPlate(R.box, FAN_RADIUS, A);

            const float ICON = FAN_ICON * SCALE;
            CBox        ib{std::round(sc<float>(R.box.x) + sc<float>(R.box.width) - FAN_PAD_X * SCALE - ICON),
                    std::round(sc<float>(R.box.y) + (sc<float>(R.box.height) - ICON) / 2.F), std::round(ICON), std::round(ICON)};
            drawTex(OverviewRender::appIcon(R.icon), ib, A, ROT);

            if (const auto TEX = OverviewRender::text(R.name, CHyprColor{1.F, 1.F, 1.F, 0.95F}, sc<int>(std::round(FAN_FONT * SCALE)), false, "",
                                                      sc<int>(std::round(FAN_LABEL_MAX * SCALE)), 500);
                TEX && TEX->ok()) {
                CBox lb{std::round(sc<float>(R.box.x) + FAN_PAD_X * SCALE), std::round(sc<float>(R.box.y) + (sc<float>(R.box.height) - sc<float>(TEX->m_size.y)) / 2.F),
                        sc<double>(TEX->m_size.x), sc<double>(TEX->m_size.y)};
                drawTex(TEX, lb, A);
            }
        }

        drawGhost();
    }

    // ----------------------------------------------------------------- move --
    bool moveInto(const std::string& destDir, const std::string& srcPath, std::string& err) {
        const auto DEST = canonical(destDir);
        const auto SRC  = canonical(srcPath);

        if (SRC.empty() || DEST.empty()) {
            err = "empty path";
            return false;
        }

        const auto SLASH = SRC.rfind('/');
        const auto BASE  = SLASH == std::string::npos ? SRC : SRC.substr(SLASH + 1);
        const auto TARGET = DEST + "/" + BASE;

        if (SRC == TARGET) {
            err = "source and destination are the same file";
            return false;
        }

        // G_FILE_COPY_NONE IS THE POLICY, DELIBERATELY. gio's own default:
        // it refuses rather than overwriting, and it does the cross-filesystem
        // copy-and-delete itself, atomically enough, with the ownership and
        // xattr handling this code has no business reimplementing. The spec is
        // explicit -- "never copy+delete hand-rolled" -- and there is exactly
        // one call here that could be mistaken for one, which is this one, and
        // it is g_file_move.
        GFile*  src  = g_file_new_for_path(SRC.c_str());
        GFile*  dst  = g_file_new_for_path(TARGET.c_str());
        GError* gerr = nullptr;

        const gboolean OK = g_file_move(src, dst, G_FILE_COPY_NONE, nullptr, nullptr, nullptr, &gerr);

        if (!OK)
            err = gerr && gerr->message ? gerr->message : "move failed";
        if (gerr)
            g_error_free(gerr);
        g_object_unref(src);
        g_object_unref(dst);

        return OK == TRUE;
    }

    void armBounce(const std::string& path) {
        const auto CANON = canonical(path);
        std::erase_if(g_bounces, [&](const SHop& h) { return h.path == CANON; });
        g_bounces.emplace_back(SHop{.path = CANON, .startedAt = Time::steadyNow()});
    }

    void armShake(const std::string& path) {
        const auto CANON = canonical(path);
        std::erase_if(g_shakes, [&](const SHop& h) { return h.path == CANON; });
        g_shakes.emplace_back(SHop{.path = CANON, .startedAt = Time::steadyNow()});
    }

    float bounceLift(const std::string& path, float iconHeight) {
        const auto CANON = canonical(path);
        float      out   = 0.F;
        for (const auto& B : g_bounces) {
            if (B.path != CANON)
                continue;
            out = std::max(out, Dock::hopLift(elapsed(B.startedAt) / Dock::HOP_MS, iconHeight));
        }
        return out;
    }

    float shakeOffset(const std::string& path, float iconWidth) {
        const auto CANON = canonical(path);
        float      out   = 0.F;
        for (const auto& S : g_shakes) {
            if (S.path != CANON)
                continue;
            const float T = std::clamp(elapsed(S.startedAt) / SHAKE_MS, 0.F, 1.F);
            // Decaying sine: full amplitude at the start, nothing at the end,
            // so it stops rather than being cut off mid-swing.
            out += std::sin(T * SHAKE_CYCLES * 2.F * 3.14159265F) * SHAKE_AMP * iconWidth * (1.F - T);
        }
        return out;
    }
}
