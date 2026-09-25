// First: it widens private members of the Hyprland headers it includes, and
// that only works if nothing has included them before it.
#include "OverviewInternal.hpp"

#include "DockStacks.hpp"

#include "AppSearch.hpp"
#include "Compat.hpp"
#include "Config.hpp"
#include "LauncherEntry.hpp"
#include "OverviewRender.hpp"
#include "OverviewPassElement.hpp"
#include "StackReader.hpp"
#include "TextInput.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <utility>
#include <string>
#include <unordered_map>
#include <vector>

#include <gio/gio.h>

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/pass/TexPassElement.hpp>
#include <hyprland/src/render/pass/BorderPassElement.hpp>
#include <hyprland/src/render/pass/RectPassElement.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
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
    // The FORMAT lives in StackModel (host-tested: v1 upgrade, v2 round trip,
    // a newer build's file read-only); this file only does the I/O.

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
    void writeStore();

    struct SStoreState {
        bool seeded   = false;
        bool readOnly = false; // written by a newer build: never write it back
    };
    SStoreState g_storeState;

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
        const auto TMP = PATH.string() + ".tmp";
        {
            std::ofstream f(TMP, std::ios::trunc);
            f << StackModel::formatStore(out);
            if (!f)
                return;
        }
        std::filesystem::rename(TMP, PATH, ec); // atomic on one filesystem: a crash leaves the old file or the new one
        // Adopt our own mtime so the next poll does not read our output back --
        // which is why the generation moves HERE too.
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
    using SEntry = StackModel::SEntry;

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

    // The popup's entries: the reader's listing (StackReader, off the compositor
    // thread), sorted by the stack's key. Empty and `ready=false` until the
    // first read lands; onReaderReady() fills the popup when it does.
    // The reader hands every candidate icon name ('|'-joined, most specific
    // first); the first one the user's theme has wins. Memoized per list.
    std::string pickIconImpl(const std::string& names) {
        static std::unordered_map<std::string, std::string> memo;
        if (const auto IT = memo.find(names); IT != memo.end())
            return IT->second;
        std::string pick = "text-x-generic";
        size_t      at   = 0;
        while (at <= names.size()) {
            const auto BAR  = names.find('|', at);
            const auto NAME = names.substr(at, BAR == std::string::npos ? std::string::npos : BAR - at);
            if (AppSearch::iconExists(NAME)) {
                pick = NAME;
                break;
            }
            if (BAR == std::string::npos)
                break;
            at = BAR + 1;
        }
        return memo[names] = pick;
    }

    StackReader::SListing sortedListing(const std::string& dir, StackModel::ESort sort) {
        auto L = StackReader::listing(dir);
        for (auto& E : L.entries)
            E.icon = E.dir ? folderIcon(canonical(E.path)) : pickIconImpl(E.icon);
        StackModel::sortEntries(L.entries, sort);
        return L;
    }

    wl_event_source* g_inotifySource = nullptr;

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

    // Dock stacks A: there is NO item cap. The fan shows every item while it
    // fits the screen, and a stack that does not fit opens as the searchable
    // pane instead -- so nothing is ever silently dropped.
    constexpr size_t STAGGER_ROWS = 24; // the stagger stops growing here, however tall the fan

    // ----------------------------------------------------------- popup state --
    struct SRow {
        CBox        box;     // monitor-local RENDER px, as DRAWN last frame
        std::string path;    // empty: the "N more..." / action row
        std::string name;
        std::string icon;
        bool        action = false;
        int         vis    = -1; // index into SPopup::visible; -1 for the action row
    };

    struct SPopup {
        std::string         path;
        StackModel::ESort   sort = StackModel::ESort::Name;
        StackModel::EView   view = StackModel::EView::Auto;
        bool                grid = false; // decided at open: view==Grid, or the fan does not fit
        MONITORID           monitor = MONITOR_INVALID;
        CBox                anchor;  // the cell it grew out of
        std::vector<SEntry> entries;
        bool                loading   = true;  // no listing yet
        bool                truncated = false; // past StackReader::MAX_ENTRIES
        uint64_t            readerGen = 0;
        // Dock stacks A: no item cap. `visible` indexes `entries` in display
        // order (everything, until the pane's search filters it -- Task 7);
        // `hover` / `previewing` index `visible`.
        std::vector<size_t> visible;
        int                 hover      = -1;
        int                 previewing = -1;    // the row Sushi is showing; drawn enlarged + ringed
        bool                keyboard   = false; // the keyboard moved last: `hover` is its selection
        int                 cols       = 1;     // the pane's column count (Task 7); 1 for the fan
        Vector2D            lastMouse;
        std::string         query;                        // the pane's search field
        CBox                fieldBox, headerBox, sortBox; // the pane's chrome, as DRAWN
        std::vector<SRow>   rows;        // published for the pointer
        CBox                plate;       // grid only: the whole panel, as drawn
        float               scroll = 0.F;
        Time::steady_tp     since{};
        bool                leaving = false;
    };

    std::string     g_trashStack;   // whose trash job is in flight (Stacks::trashFrom)
    uint64_t        g_trashSeenSeq = 0;
    std::string     g_trashError;   // shown in that stack's tooltip for 3 s
    Time::steady_tp g_trashErrorAt{};

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

        // One row per item (no cap: a stack that does not fit opens as the pane).
        // An empty folder -- or one whose first listing has not landed -- still
        // shows ONE row saying so, so a click always shows something.
        const size_t ROWS     = std::max<size_t>(1, p.visible.size());
        const bool   EMPTYROW = p.visible.empty(); // "Loading…" / "Empty folder"

        p.rows.clear();
        p.rows.reserve(ROWS);
        for (size_t i = 0; i < ROWS; ++i) {
            const bool  ACTION = EMPTYROW;
            const float FI     = sc<float>(i);
            const float SHIFT  = FAN_SHIFT_K * std::pow(FI, FAN_SHIFT_P) * SCALE;

            // The stagger: row i has not started until i * STAGGER_MS has gone
            // by, and then runs its own OPEN_MS. Read off ONE clock so the fan
            // cannot tear.
            const float T0 = FI * STAGGER_MS / std::max(1.F, OPEN_MS + STAGGER_MS * sc<float>(std::min(p.visible.size(), STAGGER_ROWS)));
            const float TI = std::clamp((progress - T0) / std::max(0.001F, 1.F - T0), 0.F, 1.F);
            const float E  = p.leaving ? 1.F - houseEase(false, TI) : houseEase(true, TI);

            const float ROWH = FAN_ROW_H * SCALE;
            const float RESTY = BASE - FAN_STEP * SCALE * (FI + 1.F);
            // Grows OUT of the icon: at E=0 the row sits collapsed on the cell,
            // which is the hover panel's entrance and the reference's both.
            const float Y = sc<float>(p.anchor.y) + (RESTY - sc<float>(p.anchor.y)) * E;

            std::string LABEL, ICON;
            if (!ACTION) {
                LABEL = p.entries[p.visible[i]].name;
                ICON  = p.entries[p.visible[i]].icon;
            } else {
                LABEL = p.loading ? std::string{"Loading\xe2\x80\xa6"} : std::string{"Empty folder"};
                ICON  = "folder-open";
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
                .path   = ACTION ? std::string{} : p.entries[p.visible[i]].path,
                .name   = LABEL,
                .icon   = ICON,
                .action = ACTION,
                .vis    = ACTION ? -1 : sc<int>(i),
            });
        }
    }

    // THE PANE (dock stacks A): a searchable grid of the VISIBLE entries,
    // scrolled, cells outside the viewport neither drawn nor hit.
    void layoutPane(SPopup& p, PHLMONITOR monitor, const CBox& pill, float progress) {
        // Sized by the whole FOLDER, not the matches: a pane that shrank with
        // every letter typed would jump about under the pointer.
        const auto L  = Stacks::Pane::layout(monitor, p.anchor, pill, p.entries.size(), progress, p.leaving);
        p.plate       = L.plate;
        p.cols        = L.cols;
        p.fieldBox    = L.field;
        p.headerBox   = L.header;
        const float S = std::max(monitor->m_scale, 0.01F);
        const float SP = Stacks::Pane::SPACING * S;
        const int   ROWS = std::max(1, sc<int>((p.visible.size() + L.cols - 1) / L.cols));
        const float MAXSCROLL = std::max(0.F, ROWS * (L.cellH + SP) - SP - sc<float>(L.grid.height));
        // Keep the keyboard's selection in view.
        if (p.keyboard && p.hover >= 0) {
            const float ROWY = sc<float>(p.hover / L.cols) * (L.cellH + SP);
            if (ROWY < p.scroll)
                p.scroll = ROWY;
            else if (ROWY + L.cellH > p.scroll + L.grid.height)
                p.scroll = ROWY + L.cellH - sc<float>(L.grid.height);
        }
        p.scroll = std::clamp(p.scroll, 0.F, MAXSCROLL);

        p.rows.clear();
        for (size_t k = 0; k < p.visible.size(); ++k) {
            const auto& E = p.entries[p.visible[k]];
            CBox box{std::round(L.grid.x + (k % L.cols) * (L.cellW + SP)), std::round(L.grid.y + (k / L.cols) * (L.cellH + SP) - p.scroll), std::round(L.cellW),
                     std::round(L.cellH)};
            if (box.y + box.height < L.grid.y || box.y > L.grid.y + L.grid.height)
                continue;
            p.rows.emplace_back(SRow{.box = box, .path = E.path, .name = E.name, .icon = E.icon, .vis = sc<int>(k)});
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
        const float SPAN = p.leaving ? CLOSE_MS : (OPEN_MS + STAGGER_MS * sc<float>(std::min(p.visible.size(), STAGGER_ROWS)));
        return std::clamp(MS / std::max(1.F, SPAN), 0.F, 1.F);
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

    PHLMONITOR popupMonitorPtr() {
        if (!g_popup)
            return nullptr;
        for (const auto& M : State::monitorState()->monitors())
            if (M && M->m_id == g_popup->monitor)
                return M;
        return nullptr;
    }

    // THE FIT (dock stacks A: "no fixed cap"). The fan is shown while every row
    // stays one step below the top bar; otherwise the pane. A stack set to
    // Always Grid always takes the pane.
    bool chooseGrid(const SPopup& p, PHLMONITOR monitor) {
        if (p.view == StackModel::EView::Grid)
            return true;
        if (!monitor)
            return false;
        const float S = std::max(monitor->m_scale, 0.01F);
        const StackModel::SFanFit FIT{
            .pillTopL  = sc<float>(p.anchor.y) / S,
            .topLimitL = OvInternal::overviewTopChromeBottomL(monitor) - sc<float>(monitor->m_position.y),
            .stepL     = FAN_STEP,
            .gapL      = FAN_GAP,
        };
        return !StackModel::fanFits(std::max<size_t>(1, p.entries.size()), FIT);
    }

    // The pane's search: which entries the query keeps, in display order.
    void refilter(SPopup& p) {
        p.visible = StackModel::filter(p.entries, p.grid ? p.query : std::string{});
        if (p.hover >= sc<int>(p.visible.size()))
            p.hover = p.visible.empty() ? -1 : 0;
    }

    // Everything a new listing changes: the entries, the visible order, the
    // fan-or-pane decision, and a selection that fell off the end.
    void applyListing(SPopup& p, StackReader::SListing&& L, PHLMONITOR monitor) {
        p.entries   = std::move(L.entries);
        p.loading   = !L.ready;
        p.truncated = L.truncated;
        p.readerGen = StackReader::generation();
        p.grid = chooseGrid(p, monitor);
        refilter(p);
        if (p.hover >= sc<int>(p.visible.size()))
            p.hover = p.visible.empty() ? -1 : 0;
        if (p.previewing >= sc<int>(p.visible.size()))
            p.previewing = -1;
    }

    // SUSHI BESIDE THE POPUP (dock stacks A). The previewer is a normal client;
    // window rules float and centre it, and when it maps (`openLate`, after
    // those rules) it is moved beside the fan or pane, on the side with more
    // room. No deferred callback: one still queued at unload would call into
    // unloaded code. If the move is ignored Sushi stays centred -- acceptable.
    CHyprSignalListener g_sushiOpen;

    void placeSushi(const PHLWINDOW& W) {
        if (!W || !g_popup || g_popup->previewing < 0)
            return;
        const auto MON = popupMonitorPtr();
        if (!MON)
            return;
        const float S    = std::max(MON->m_scale, 0.01F);
        const auto  SIZE = W->m_realSize->goal();
        double left = 1e9, right = -1e9, top = 1e9;
        if (g_popup->grid && !g_popup->plate.empty()) {
            left  = g_popup->plate.x / S;
            right = (g_popup->plate.x + g_popup->plate.width) / S;
            top   = g_popup->plate.y / S;
        } else {
            for (const auto& R : g_popup->rows) {
                left  = std::min(left, R.box.x / S);
                right = std::max(right, (R.box.x + R.box.width) / S);
                top   = std::min(top, R.box.y / S);
            }
        }
        if (right < left)
            return;
        const double MONW       = MON->m_size.x;
        const bool   RIGHT_SIDE = (MONW - right) >= left;
        double       x          = RIGHT_SIDE ? right + 16.0 : left - 16.0 - SIZE.x;
        x                       = std::clamp(x, 8.0, std::max(8.0, MONW - SIZE.x - 8.0));
        const double Y          = std::clamp(top, 8.0, std::max(8.0, MON->m_size.y - SIZE.y - 8.0));
        W->move(Vector2D{MON->m_position.x + x, MON->m_position.y + Y});
    }

    void armSushiPlacement() {
        if (g_sushiOpen)
            return;
        g_sushiOpen = Event::bus()->m_events.window.openLate.listen([](PHLWINDOW w) {
            if (w && w->m_class == "org.gnome.NautilusPreviewer")
                placeSushi(w);
        });
    }

    // Every setter: re-read, change one field, write, and re-show an open popup
    // that belongs to this stack.
    template <typename F>
    bool editStack(const std::string& path, F&& change) {
        Stacks::all();
        auto* S = find(path);
        if (!S || !change(*S))
            return false;
        decorate(*S);
        writeStore();
        if (g_popup && g_popup->path == canonical(path)) {
            g_popup->sort  = S->sort;
            g_popup->view  = S->view;
            applyListing(*g_popup, sortedListing(g_popup->path, g_popup->sort), popupMonitorPtr());
            g_popup->since = Time::steadyNow();
            damagePopupMonitor();
        }
        return true;
    }

    // Close the previewer over the same bus ShowFile used (no reply awaited).
    void showPreviewClose() {
        sd_bus* bus = LauncherEntry::sessionBus();
        if (!bus)
            return;
        sd_bus_message* m = nullptr;
        if (sd_bus_message_new_method_call(bus, &m, "org.gnome.NautilusPreviewer", "/org/gnome/NautilusPreviewer", "org.gnome.NautilusPreviewer2", "Close") < 0)
            return;
        sd_bus_message_set_expect_reply(m, 0);
        if (sd_bus_send(bus, m, nullptr) >= 0)
            sd_bus_flush(bus);
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

        // Watch exactly the pinned folders, so a file landing in one refreshes
        // its cell and an open popup (StackReader, inotify on the event loop).
        std::vector<std::string> dirs;
        for (const auto& ST : S.items)
            if (ST.isDir)
                dirs.push_back(ST.path);
        StackReader::watch(dirs);
        if (!g_inotifySource && StackReader::inotifyFd() >= 0 && g_pCompositor && g_pCompositor->m_wlEventLoop)
            g_inotifySource = wl_event_loop_add_fd(g_pCompositor->m_wlEventLoop, StackReader::inotifyFd(), WL_EVENT_READABLE,
                                                   [](int, uint32_t, void*) -> int {
                                                       StackReader::drainInotify();
                                                       Stacks::onReaderReady();
                                                       return 0;
                                                   },
                                                   nullptr);
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

    bool setSort(const std::string& path, StackModel::ESort sort) {
        return editStack(path, [&](SStack& s) { return std::exchange(s.sort, sort) != sort; });
    }
    bool setDisplay(const std::string& path, StackModel::EDisplay display) {
        return editStack(path, [&](SStack& s) { return std::exchange(s.display, display) != display; });
    }
    bool setViewMode(const std::string& path, StackModel::EView view) {
        return editStack(path, [&](SStack& s) { return std::exchange(s.view, view) != view; });
    }
    bool setLabel(const std::string& path, const std::string& label) {
        return editStack(path, [&](SStack& s) { return std::exchange(s.customLabel, label) != label; });
    }

    bool moveTo(const std::string& path, size_t index) {
        all();
        auto&      ITEMS = store().items;
        const auto CANON = canonical(path);
        const auto IT    = std::ranges::find_if(ITEMS, [&](const SStack& s) { return s.path == CANON; });
        if (IT == ITEMS.end())
            return false;
        const size_t FROM = sc<size_t>(IT - ITEMS.begin());
        index             = std::min(index, ITEMS.size() - 1);
        if (FROM == index)
            return false;
        auto moved = std::move(*IT);
        ITEMS.erase(IT);
        ITEMS.insert(ITEMS.begin() + sc<long>(index), std::move(moved));
        writeStore();
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
        if (!std::filesystem::exists(CANON, ec)) {
            // GONE (deleted, renamed, an unmounted drive): refuse visibly. The
            // cell stays, dimmed, until the user removes it.
            armShake(CANON);
            return;
        }
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
        p.sort       = S ? S->sort : StackModel::ESort::Name;
        p.view       = S ? S->view : StackModel::EView::Auto;
        p.monitor    = monitor->m_id;
        p.anchor     = anchor;
        applyListing(p, sortedListing(CANON, p.sort), monitor);
        p.since      = Time::steadyNow();

        // An EMPTY folder still opens -- with one row saying so. The reference
        // returns early and the click does nothing at all, which is
        // indistinguishable from a broken dock.
        g_popup = std::move(p);
        armSushiPlacement();
    }

    void onReaderReady() {
        // The wake fd is shared with the overview's file search and the
        // dictionary: only a job of OURS is a reason to rebuild and repaint.
        static uint64_t seen = 0;
        if (StackReader::generation() == seen)
            return;
        seen = StackReader::generation();

        if (g_popup && !g_popup->leaving && g_popup->readerGen != StackReader::generation()) {
            applyListing(*g_popup, sortedListing(g_popup->path, g_popup->sort), popupMonitorPtr());
            damagePopupMonitor();
        }
        // A trash job landed: bounce the stack on success; on a failure shake it
        // and put the error (and how many made it) in its tooltip.
        if (const auto R = StackReader::lastTrashResult(); R.seq != g_trashSeenSeq) {
            g_trashSeenSeq = R.seq;
            StackReader::invalidate(g_trashStack);
            if (!R.error.empty()) {
                g_trashError   = R.error + (R.moved ? " (" + std::to_string(R.moved) + " moved)" : "");
                g_trashErrorAt = Time::steadyNow();
                armShake(g_trashStack);
            } else
                armBounce(g_trashStack);
        }
        ++g_storeGeneration; // the dock's item cache (and the pile) key on this
        for (const auto& M : State::monitorState()->monitors())
            if (M)
                g_pHyprRenderer->damageMonitor(M);
    }

    void shutdown() {
        if (g_inotifySource) {
            wl_event_source_remove(g_inotifySource);
            g_inotifySource = nullptr;
        }
        g_sushiOpen.reset();
        g_popup.reset();
        g_itemDrag.reset();
    }

    void close() {
        if (!g_popup || g_popup->leaving)
            return;
        if (g_popup->previewing >= 0) {
            showPreviewClose();
            g_popup->previewing = -1;
        }
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

    // THE ITEM SPACE MEANS (dock stacks A): the keyboard's selection when the
    // keyboard moved last, else the row under the pointer. An index into
    // `visible`, or -1.
    static int targetIndex() {
        if (!g_popup || g_popup->leaving)
            return -1;
        if (g_popup->keyboard && g_popup->hover >= 0 && g_popup->hover < sc<int>(g_popup->visible.size()))
            return g_popup->hover;
        const auto PATH = hoveredItemPath();
        if (PATH.empty())
            return -1;
        for (size_t v = 0; v < g_popup->visible.size(); ++v)
            if (g_popup->entries[g_popup->visible[v]].path == PATH)
                return sc<int>(v);
        return -1;
    }

    // Spec: "No Sushi installed: Space does nothing" (and the pane says so).
    bool sushiAvailable() {
        static const bool AVAILABLE = [] {
            std::vector<std::string> dirs = {std::string(g_get_user_data_dir())};
            for (const char* const* D = g_get_system_data_dirs(); D && *D; ++D)
                dirs.emplace_back(*D);
            for (const auto& D : dirs)
                if (g_file_test((D + "/dbus-1/services/org.gnome.NautilusPreviewer.service").c_str(), G_FILE_TEST_EXISTS))
                    return true;
            return false;
        }();
        return AVAILABLE;
    }

    bool previewHovered() {
        if (!sushiAvailable())
            return false;
        const int I = targetIndex();
        if (I < 0)
            return false;

        // A path that will not become a uri is one sushi cannot open, so NOTHING
        // is claimed and SPACE goes where it was going.
        const auto& PATH = g_popup->entries[g_popup->visible[sc<size_t>(I)]].path;
        GError*     err  = nullptr;
        char*       uri  = g_filename_to_uri(PATH.c_str(), nullptr, &err);
        if (!uri) {
            if (err)
                g_error_free(err);
            return false;
        }
        showPreview(uri); // sushi's own toggle: the same uri again closes it
        g_free(uri);
        g_popup->previewing = g_popup->previewing == I ? -1 : I;
        damagePopupMonitor();
        return true;
    }

    std::string pickIcon(const std::string& names) {
        return pickIconImpl(names);
    }

    bool popupShowsStack(const std::string& stackPath) {
        return g_popup && g_popup->path == canonical(stackPath) && !g_popup->grid;
    }

    void trashFrom(const std::string& stackPath, std::vector<std::string> paths) {
        if (paths.empty())
            return;
        g_trashStack = canonical(stackPath);
        StackReader::trash(std::move(paths));
    }

    std::string tooltipSuffix(const std::string& stackPath) {
        const auto CANON = canonical(stackPath);
        if (const auto L = StackReader::listing(CANON); L.ready && L.missing)
            return "Folder not found";
        if (CANON == g_trashStack && !g_trashError.empty() && elapsed(g_trashErrorAt) < 3000.F)
            return g_trashError;
        return {};
    }

    bool keyboardActive() {
        return g_popup && !g_popup->leaving;
    }

    // Arrows move the selection, Enter opens, Esc closes the preview first and
    // then the popup, Space previews. evdev keycodes.
    bool handleKey(uint32_t keycode) {
        if (!keyboardActive())
            return false;
        auto&     P    = *g_popup;
        const int N    = sc<int>(P.visible.size());
        const int STEP = P.grid ? std::max(1, P.cols) : 1;
        // The keyboard starts from what the user is looking at: its own
        // selection, else the item being previewed, else the row under the pointer.
        const int FROM = P.keyboard ? P.hover : (P.previewing >= 0 ? P.previewing : targetIndex());

        if (P.grid) {
            if (keycode == 14) { // BACKSPACE
                if (!P.query.empty()) {
                    TextInput::popCodepoint(P.query);
                    refilter(P);
                    P.scroll = 0.F;
                    damagePopupMonitor();
                }
                return true;
            }
            if (keycode == 1 && !P.query.empty() && P.previewing < 0) { // ESC clears first
                P.query.clear();
                refilter(P);
                damagePopupMonitor();
                return true;
            }
            if (keycode != 57 && keycode != 28) { // SPACE previews, ENTER opens
                if (const auto K = TextInput::keyFor(overviewActiveXkbState(), keycode); K.consume) {
                    P.query += K.text;
                    refilter(P);
                    P.scroll = 0.F;
                    P.hover  = P.visible.empty() ? -1 : 0;
                    damagePopupMonitor();
                    return true;
                }
            }
        }

        switch (keycode) {
            case 103: // UP: the fan grows upward, so up is the next row; in the pane, the row above
                P.hover = N == 0 ? -1 : std::clamp(FROM < 0 ? 0 : (P.grid ? FROM - STEP : FROM + 1), 0, N - 1);
                break;
            case 108: // DOWN
                P.hover = N == 0 ? -1 : std::clamp(FROM < 0 ? 0 : (P.grid ? FROM + STEP : FROM - 1), 0, N - 1);
                break;
            case 105: // LEFT
                if (!P.grid)
                    return false;
                P.hover = N == 0 ? -1 : std::max(0, FROM - 1);
                break;
            case 106: // RIGHT
                if (!P.grid)
                    return false;
                P.hover = N == 0 ? -1 : std::min(N - 1, FROM + 1);
                break;
            case 28: // ENTER
                if (P.hover >= 0 && P.hover < N) {
                    launchPath(P.entries[P.visible[sc<size_t>(P.hover)]].path);
                    close();
                }
                return true;
            case 1: // ESC
                if (P.previewing >= 0) {
                    showPreviewClose();
                    P.previewing = -1;
                    damagePopupMonitor();
                } else
                    close();
                return true;
            case 57: // SPACE
                if (!P.keyboard && P.hover < 0 && hoveredItemPath().empty())
                    return false;
                return previewHovered();
            default:
                // The pane owns the keyboard while it is up (the overview's
                // rule): nothing typed at it reaches the window underneath.
                // Modifiers pass, so chords still work.
                switch (keycode) {
                    case 29: case 42: case 54: case 56: case 97: case 100: case 125: case 126: return false;
                    default: return P.grid;
                }
        }
        P.keyboard = true;
        damagePopupMonitor();
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
                    // Both folders changed; the reader re-reads them and
                    // onReaderReady() refreshes the popup when it lands.
                    StackReader::invalidate(DEST);
                    if (g_popup)
                        StackReader::invalidate(g_popup->path);
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

        // The pane's sort, in its header: the next of the four keys.
        if (g_popup->grid && !release && g_popup->sortBox.containsPoint(point)) {
            setSort(g_popup->path, sc<StackModel::ESort>((sc<int>(g_popup->sort) + 1) % 4));
            return true;
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
        g_popup->scroll += (up ? -1.F : 1.F) * (Pane::CELL_H + Pane::SPACING) * SCALE * 0.5F;
        g_popup->keyboard = false;
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
            // The hovered row repaints when the pointer moves onto another one.
            out += "|h" + hoveredItemPath() + "|p" + std::to_string(g_popup->previewing) + "|k" + std::to_string(g_popup->keyboard ? g_popup->hover : -1);
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
            layoutPane(*g_popup, monitor, layout.pill, PROGRESS);
        else
            layoutFan(*g_popup, monitor, layout.pill, PROGRESS);

        // FROST. User law: every new surface, by default -- and through the
        // dock's own plate call, so the popup re-themes with the rest of the
        // dock rather than beside it. The 2 px floor is the popup's own: a
        // plate that thin is the entrance's first frame, not a surface.
        // READABLE WITHOUT FROST (dock stacks A). With Hyprland's blur off the
        // plate is only the theme tint at the window opacity (~0.72), and a pane
        // of file names over a terminal of the same colour reads as two layers
        // of text. The dock pill holds icons and is fine; these hold words. So
        // a second pass of the SAME theme tint brings them to ~0.95 -- still the
        // theme's colour, and with blur on the frost does this job instead.
        const bool BLUR       = Config::getValue<bool>("decoration:blur:enabled");
        const auto drawPlate = [&](const CBox& box, float radius, float alpha) {
            if (box.width < 2.0 || box.height < 2.0)
                return;
            Dock::frostedPlate(monitor, box, sc<int>(std::round(radius * SCALE)), alpha);
            if (!BLUR) {
                CRectPassElement::SRectData solid;
                solid.box           = box;
                solid.color         = Config::dockBackgroundColor().modifyA(0.8F * alpha);
                solid.round         = sc<int>(std::round(radius * SCALE));
                solid.roundingPower = 2.F;
                if (drawableBox(solid.box))
                    g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(solid));
            }
        };

        const auto drawTex = [&](const SP<Render::ITexture>& tex, const CBox& box, float alpha, float rot = 0.F) {
            if (!tex || !tex->ok() || !drawableBox(box))
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
        // The pointer moving takes the selection back from the keyboard.
        if (MOUSE.distance(g_popup->lastMouse) > 2.0)
            g_popup->keyboard = false;
        g_popup->lastMouse = MOUSE;
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
            auto&      P     = *g_popup;
            const auto FG    = Config::themeForeground();
            const auto MUTED = Config::themeMutedText();
            const auto HEAD  = Config::themeHeading();
            drawPlate(P.plate, GRID_RADIUS, ALPHA);

            // HEADER: the stack's label, "12 of 340", and the sort on the right
            // (click it to change it).
            std::string label;
            for (const auto& ST : all())
                if (ST.path == P.path)
                    label = ST.label;
            static const char* SORTS[] = {"Name", "Date Added", "Date Modified", "Kind"};
            const std::string  COUNT   = std::to_string(P.visible.size()) + " of " + std::to_string(P.entries.size());
            const std::string  SORTL   = std::string(SORTS[sc<int>(P.sort)]) + "  ";
            float              hx      = sc<float>(P.headerBox.x);
            if (const auto T = OverviewRender::text(label, FG, sc<int>(std::round(13.F * SCALE)), false, "", sc<int>(P.headerBox.width * 0.5), 600); T && T->ok()) {
                drawTex(T, CBox{hx, P.headerBox.y + (P.headerBox.height - T->m_size.y) / 2.0, T->m_size.x, T->m_size.y}.round(), ALPHA);
                hx += sc<float>(T->m_size.x) + 10.F * SCALE;
            }
            if (const auto T = OverviewRender::text(COUNT, MUTED, sc<int>(std::round(11.F * SCALE)), false, "", 0, 500); T && T->ok())
                drawTex(T, CBox{hx, P.headerBox.y + (P.headerBox.height - T->m_size.y) / 2.0, T->m_size.x, T->m_size.y}.round(), ALPHA);
            if (const auto T = OverviewRender::text(SORTL, HEAD, sc<int>(std::round(11.F * SCALE)), false, "", 0, 600); T && T->ok()) {
                P.sortBox = CBox{P.headerBox.x + P.headerBox.width - T->m_size.x, P.headerBox.y + (P.headerBox.height - T->m_size.y) / 2.0, T->m_size.x, T->m_size.y}.round();
                drawTex(T, P.sortBox, ALPHA);
            }

            // THE FIELD: an inset plate, the query or its placeholder, a caret.
            Dock::frostedPlate(monitor, P.fieldBox, sc<int>(std::round(8.F * SCALE)), ALPHA * 0.5F);
            const bool        EMPTYQ = P.query.empty();
            const std::string SHOWN  = EMPTYQ ? (sushiAvailable() ? std::string{"Search"} : std::string{"Search · install sushi for previews"}) : P.query;
            float             caretX = sc<float>(P.fieldBox.x) + 10.F * SCALE;
            if (const auto T = OverviewRender::text(SHOWN, EMPTYQ ? MUTED : FG, sc<int>(std::round(13.F * SCALE)), false, "", sc<int>(P.fieldBox.width - 20.F * SCALE), 500);
                T && T->ok()) {
                drawTex(T, CBox{caretX, P.fieldBox.y + (P.fieldBox.height - T->m_size.y) / 2.0, T->m_size.x, T->m_size.y}.round(), ALPHA);
                if (!EMPTYQ)
                    caretX += sc<float>(T->m_size.x) + 1.F;
            }
            CRectPassElement::SRectData caret;
            caret.box   = CBox{caretX, P.fieldBox.y + P.fieldBox.height * 0.25, std::max(1.0, 1.5 * SCALE), P.fieldBox.height * 0.5}.round();
            caret.color = Dock::accentColor().modifyA(ALPHA);
            if (drawableBox(caret.box))
                    g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(caret));

            // THE CELLS, clipped to the grid: icon, two-line name, and the
            // previewed / selected / hovered highlight.
            const double GTOP = P.fieldBox.y + P.fieldBox.height;
            const CBox   CLIP{P.plate.x, GTOP, P.plate.width, std::max(0.0, P.plate.y + P.plate.height - GTOP)};
            for (const auto& R : P.rows) {
                const bool PREV  = R.vis >= 0 && R.vis == P.previewing;
                const bool SEL   = R.vis >= 0 && P.keyboard && R.vis == P.hover;
                const bool HOVER = !P.keyboard && R.box.containsPoint(MOUSE);
                CBox       cell  = R.box;
                if (PREV) {
                    cell.x -= cell.width * 0.075;
                    cell.y -= cell.height * 0.075;
                    cell.width *= 1.15;
                    cell.height *= 1.15;
                }
                if (PREV || SEL || HOVER) {
                    CRectPassElement::SRectData hl;
                    hl.box     = cell;
                    hl.color   = Dock::accentColor().modifyA(ALPHA * (PREV ? 0.28F : SEL ? 0.2F : 0.12F));
                    hl.round   = sc<int>(std::round(10.F * SCALE));
                    hl.clipBox = CLIP;
                    if (drawableBox(hl.box))
                    g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(hl));
                }
                const float ICON = GRID_ICON * SCALE * (PREV ? 1.15F : 1.F);
                CBox        ib{std::round(cell.x + (cell.width - ICON) / 2.0), std::round(cell.y + 8.F * SCALE), std::round(ICON), std::round(ICON)};
                const auto TEX = R.vis >= 0 && R.vis < sc<int>(P.visible.size()) ? iconTexture(P.entries[P.visible[sc<size_t>(R.vis)]], sc<int>(std::round(ICON))) :
                                                                                  OverviewRender::appIcon(R.icon);
                if (TEX && TEX->ok() && drawableBox(ib)) {
                    CTexPassElement::SRenderData d;
                    d.tex     = TEX;
                    d.box     = ib;
                    d.a       = ALPHA;
                    d.clipBox = CLIP;
                    g_pHyprRenderer->m_renderPass.add(makeUnique<CTexPassElement>(d));
                }
                if (const auto T = OverviewRender::textWrapped(R.name, FG, sc<int>(std::round(11.F * SCALE)), sc<int>(cell.width - 8.F * SCALE), sc<int>(std::round(30.F * SCALE)),
                                                               false, "", 500);
                    T && T->ok() && drawableBox(cell)) {
                    CTexPassElement::SRenderData l;
                    l.tex     = T;
                    l.box     = CBox{cell.x + (cell.width - T->m_size.x) / 2.0, ib.y + ICON + 6.F * SCALE, T->m_size.x, T->m_size.y}.round();
                    l.a       = ALPHA;
                    l.clipBox = CLIP;
                    g_pHyprRenderer->m_renderPass.add(makeUnique<CTexPassElement>(l));
                }
            }

            // STATES: loading, no matches, and the read cap.
            const char* STATE = P.loading && P.entries.empty() ? "Loading…" : (P.visible.empty() ? (P.query.empty() ? "Empty folder" : "No matches") : nullptr);
            if (STATE)
                if (const auto T = OverviewRender::text(STATE, MUTED, sc<int>(std::round(13.F * SCALE)), false, "", 0, 500); T && T->ok())
                    drawTex(T, CBox{CLIP.x + (CLIP.width - T->m_size.x) / 2.0, CLIP.y + (CLIP.height - T->m_size.y) / 2.0, T->m_size.x, T->m_size.y}.round(), ALPHA);
            if (P.truncated)
                if (const auto T = OverviewRender::text("showing first 5 000", MUTED, sc<int>(std::round(10.F * SCALE)), false, "", 0, 500); T && T->ok())
                    drawTex(T, CBox{P.plate.x + (P.plate.width - T->m_size.x) / 2.0, P.plate.y + P.plate.height - T->m_size.y - 4.F * SCALE, T->m_size.x, T->m_size.y}.round(),
                            ALPHA);

            drawGhost();
            return;
        }

        // THE FAN. One frosted plate per row: the rows are not a rectangle, so
        // a single enclosing plate would be a panel with a fan painted on it,
        // which is a different object. One blur region per row, and only while a
        // fan is actually up.
        // PILE -> FAN (dock stacks A): the items the cell's pile shows fly from
        // their pile slot (box and angle) to their row, on that row's own eased
        // progress -- and back onto the pile on close. The cell draws only its
        // outline meanwhile (popupShowsStack), so they visibly left it.
        const auto PILE = pileLayout(g_popup->path, g_popup->anchor);

        for (size_t i = 0; i < g_popup->rows.size(); ++i) {
            const auto& R   = g_popup->rows[i];
            const float ROT = FAN_TILT_DEG * sc<float>(i) * 3.14159265F / 180.F;
            const SPileItem* FROMPILE = nullptr;
            for (const auto& PI : PILE)
                if (!R.action && PI.entry.path == R.path)
                    FROMPILE = &PI;

            // Per-row alpha: a row that has not started its stagger is not
            // drawn at all rather than drawn at zero, which keeps its frost off
            // the queue too.
            const float T0 = sc<float>(i) * STAGGER_MS / std::max(1.F, OPEN_MS + STAGGER_MS * sc<float>(std::min(g_popup->visible.size(), STAGGER_ROWS)));
            const float TI = std::clamp((PROGRESS - T0) / std::max(0.001F, 1.F - T0), 0.F, 1.F);
            const float A  = (g_popup->leaving ? 1.F - houseEase(false, PROGRESS) : houseEase(true, TI));
            const float E  = A; // the row's own eased progress: 0 on the pile, 1 in place
            if (A <= 0.02F && !FROMPILE)
                continue;

            // The frost cannot be rotated -- queueFrost takes a box, and the
            // blur pass has no transform. So the PLATE is axis-aligned and only
            // the artwork on it tilts. Measured against the alternative (no
            // tilt at all) that reads as the reference's fan; measured against
            // a rotated plate, which is not available, it is the honest
            // approximation and it is recorded here as one.
            // Dock stacks A: the previewed row is enlarged 1.15x with an accent
            // ring; the keyboard's selection and the pointer's row get the same
            // ring, lighter. Nothing opens on hover.
            const bool PREV  = !R.action && sc<int>(i) == g_popup->previewing;
            const bool SEL   = !R.action && g_popup->keyboard && sc<int>(i) == g_popup->hover;
            const bool HOVER = !R.action && !g_popup->keyboard && R.box.containsPoint(MOUSE);
            const float K    = PREV ? 1.15F : 1.F;
            CBox        box  = R.box;
            if (PREV) {
                box.x -= box.width * 0.075;
                box.y -= box.height * 0.075;
                box.width *= 1.15;
                box.height *= 1.15;
            }
            drawPlate(box, FAN_RADIUS, A);
            if (PREV || SEL || HOVER) {
                CBorderPassElement::SBorderData ring;
                ring.box        = box;
                ring.grad1      = ::Config::CGradientValueData(Dock::accentColor().modifyA(A * (PREV ? 1.F : SEL ? 0.6F : 0.35F)));
                ring.round      = sc<int>(std::round(FAN_RADIUS * SCALE));
                ring.borderSize = std::max(1, sc<int>(std::round(2.F * SCALE)));
                if (drawableBox(ring.box))
                    g_pHyprRenderer->m_renderPass.add(makeUnique<CBorderPassElement>(ring));
            }

            const float ICON = FAN_ICON * SCALE * K;
            CBox        ib{std::round(sc<float>(box.x) + sc<float>(box.width) - FAN_PAD_X * SCALE * K - ICON),
                    std::round(sc<float>(box.y) + (sc<float>(box.height) - ICON) / 2.F), std::round(ICON), std::round(ICON)};
            if (FROMPILE) {
                // Lerp box and angle from the pile slot to the row; drawn opaque
                // throughout, because it never stops being on screen.
                const CBox& P0 = FROMPILE->box;
                CBox        fly{P0.x + (ib.x - P0.x) * E, P0.y + (ib.y - P0.y) * E, P0.width + (ib.width - P0.width) * E, P0.height + (ib.height - P0.height) * E};
                drawTex(iconTexture(FROMPILE->entry, sc<int>(std::round(ib.width))), fly.round(), 1.F, FROMPILE->rot + (ROT - FROMPILE->rot) * E);
            } else
                drawTex(OverviewRender::appIcon(R.icon), ib, A, ROT);

            if (const auto TEX = OverviewRender::text(R.name, Config::themeForeground(), sc<int>(std::round(FAN_FONT * SCALE * K)), false, "",
                                                      sc<int>(std::round(FAN_LABEL_MAX * SCALE * K)), 500);
                TEX && TEX->ok()) {
                CBox lb{std::round(sc<float>(box.x) + FAN_PAD_X * SCALE * K), std::round(sc<float>(box.y) + (sc<float>(box.height) - sc<float>(TEX->m_size.y)) / 2.F),
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
