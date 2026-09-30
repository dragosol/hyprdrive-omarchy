// First: it widens private members of the Hyprland headers it includes, and
// that only works if nothing has included them before it.
#include "Motion.hpp"
#include "OverviewInternal.hpp"

#include "DockStacks.hpp"
#include <unordered_set>
#include "Thumbs.hpp"
#include "BuiltinIcons.hpp"
#include <sys/stat.h>
#include "StackBlur.hpp"
#include "FlightBlur.hpp"
#include "DockDrop.hpp"

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
    void showPreviewClose();

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
            const auto HAS  = AppSearch::iconExistsNow(NAME);
            if (!HAS)
                return pick; // the index is busy (startup scan): answer generic, do NOT memoize, ask next time
            if (*HAS) {
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

    // THE MOTION, from the user's own GNOME extension (dragosol/dock-stacks,
    // FanPopup/GridPopup), asked for by name on 2026-09-24 -- so these are its
    // numbers, not the house band: each fan row flies out of the cell over
    // 140 ms ease-out-quad, 11 ms after the one below it, growing from 0.1x
    // and fading in, at its fixed tilt; the fan folds back in 84 ms. The pane
    // grows over 200 ms and leaves in 150.
    //
    // ONE REVERSIBLE TIMELINE (user, same day: "spam click the stack and have
    // the animation reverse itself"). The popup's state is a position on its
    // opening timeline; opening runs it forward, closing runs it back (faster,
    // so the fold takes FAN_CLOSE_MS from fully open), and every visual is a
    // pure function of that position -- so turning round mid-flight starts
    // from exactly where everything is, with no jump.
    constexpr size_t FAN_MAX_ITEMS = 12; // the extension's fan-threshold default
    // TWO MOTIONS (user, 2026-09-24: "the fanning out doesn't seem natural
    // enough ... try Serpantinum's curves and timing"), picked by
    // dock:stack_motion so they can be compared on the real desktop:
    //   extension   -- dragosol/dock-stacks' FanPopup: every channel one
    //                  140 ms ease-out-quad, 11 ms stagger, 84 ms fold.
    //   serpantinum -- ilyamiro/serpantinum's dock popups and lists: position
    //                  280 ms OutQuint (a long soft landing), size 280 ms
    //                  OutBack overshoot 1.1 (a slight pop past full size),
    //                  opacity 260 ms OutCubic, rows 40 ms apart (its
    //                  Dropdown), a 150 ms fold (its InQuint exit), hover
    //                  220 ms OutCubic (its dock buttons). A long fan's stagger
    //                  is squeezed so the whole entrance stays under 300 ms of
    //                  stagger -- 13 rows x 40 ms would be three quarters of a
    //                  second before the last row even starts.
    struct SFanMotion {
        float rowMs, alphaMs, staggerMs, staggerCapMs, closeMs, hoverMs;
        bool  serp;
    };
    const SFanMotion& fanMotion() {
        static const SFanMotion EXT{140.F, 140.F, 11.F, 1e9F, 84.F, 120.F, false};
        // Serpantinum's numbers x0.8 (user, 2026-09-24: "the curve is nice,
        // maybe 20% faster").
        static const SFanMotion SERP{224.F, 208.F, 32.F, 240.F, 120.F, 176.F, true};
        return ScrollOverview::Config::getValue<std::string>("plugin:hyprdrive:dock:stack_motion") == "extension" ? EXT : SERP;
    }
    float outQuad(float t) { return 1.F - (1.F - t) * (1.F - t); }
    float outCubic(float t) { return 1.F - std::pow(1.F - t, 3.F); }
    float outQuint(float t) { return 1.F - std::pow(1.F - t, 5.F); }
    float outBack(float t, float s) { // Qt's Easing.OutBack
        t -= 1.F;
        return t * t * ((s + 1.F) * t + s) + 1.F;
    }
    constexpr float PANE_OPEN_MS   = 200.F;
    constexpr float PANE_CLOSE_MS  = 150.F;

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
        float       k      = 1.F; // fan: the row's scale on its way in or out
        float       e      = 1.F; // fan: the row's eased progress (0 in the cell)
        float       a      = 1.F; // fan: the row's opacity
        float       rot    = 0.F; // fan: the row's turn right now (radians)
    };

    struct SPopup {
        std::string         path;
        StackModel::ESort   sort = StackModel::ESort::Added;
        StackModel::EView   view = StackModel::EView::Auto;
        bool                grid = false; // decided at open: view==Grid, or the fan does not fit
        CBox                lastPlate;    // the pane's plate last frame, for its entrance blur
        size_t              fanCap   = 0; // View as Fan: rows the screen holds (0 = no cap)
        size_t              overflow = 0; // entries behind the "N more in Files" row
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
        Time::steady_tp     lastDrawn{};                  // keyboardActive: only a popup actually on screen takes keys
        std::string         query;                        // the pane's search field
        CBox                fieldBox, headerBox, sortBox; // the pane's chrome, as DRAWN
        std::vector<SRow>   rows;        // published for the pointer
        CBox                plate;       // grid only: the whole panel, as drawn
        CBox                gridBox;     // grid only: the scrolled viewport the cells show through
        float               scroll = 0.F;
        Time::steady_tp     since{};
        bool                leaving = false;
        float               tl      = 0.F;   // position on the opening timeline, ms
        std::unordered_map<std::string, float> hoverAmt; // fan: row key -> 0..1 of its hover blow-up
        bool                hoverMoving = false;         // a blow-up still easing (asks for frames)
        std::string         hoverKey;                    // the row the pointer is on this frame
        float               lastProgress = 0.F;          // last frame's, for the motion blur's shutter
        Time::steady_tp     lastTick{};      // when tick() last advanced it
        bool                ticked  = false;
    };

    std::string     g_trashStack;   // whose trash job is in flight (Stacks::trashFrom)
    uint64_t        g_trashSeenSeq = 0;
    std::string     g_trashError;   // shown in that stack's tooltip for 3 s
    Time::steady_tp g_trashErrorAt{};

    std::optional<SPopup> g_popup;

    std::string popupState() {
        if (!g_popup)
            return "none";
        return std::string(g_popup->leaving ? "closing" : "opening/open") + " p=" + std::to_string(g_popup->tl);
    }



    // A row is only hit where it is SHOWN: the pane's cells scroll under a
    // viewport, and the part of a cell past it (over the dock, 2026-09-24 bug)
    // is not there to click.
    bool rowHit(const SRow& R, const Vector2D& point) {
        if (!R.box.containsPoint(point))
            return false;
        // A fan row in flight is not a target yet: only once it has (nearly)
        // landed -- a click aimed at the stack must not catch a passing paper.
        if (R.e < 0.9F)
            return false;
        return !g_popup || !g_popup->grid || g_popup->gridBox.empty() || g_popup->gridBox.containsPoint(point);
    }

    // A press on the open stack's own cell closed it; its release must not
    // reach the pill and open it again.
    bool g_swallowToggleRelease = false;

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

    // Stack path -> browser partial files in it at the last listing. Only
    // folders with a download in flight are kept.
    std::unordered_map<std::string, size_t> g_downloads;
    // Stack path -> the entry names its folder had at the last listing (for the
    // arrival bounce).
    std::unordered_map<std::string, std::unordered_set<std::string>> g_seenNames;
    std::vector<SHop> g_shakes;

    float elapsed(const Time::steady_tp& since) {
        return std::chrono::duration<float, std::milli>(Time::steadyNow() - since).count();
    }

    float fanStagger(const SPopup& p) {
        const auto&  M    = fanMotion();
        const size_t ROWS = std::min(std::max<size_t>(1, p.visible.size() + (p.overflow > 0 ? 1 : 0)), STAGGER_ROWS);
        return ROWS > 1 ? std::min(M.staggerMs, M.staggerCapMs / sc<float>(ROWS - 1)) : M.staggerMs;
    }

    // A THROWN PACK, NOT A DEALT ONE (user, 2026-09-24: "like I just threw a
    // pack of papers on the ground and they happened to land in a fan"). In
    // the serpantinum motion each paper gets its own small, repeatable
    // randomness -- seeded by the stack's path and the row, so a stack always
    // lands the same way: when it leaves (roughly together, not strictly one
    // after another), how far its path bows to one side, and how far off its
    // final tilt it starts spinning from.
    float rowJitter(const SPopup& p, size_t i, int salt) {
        const double SEED = sc<double>(std::hash<std::string>{}(p.path) % 997) * 0.37 + sc<double>(i) * 12.9898 + salt * 78.233;
        const double F    = std::sin(SEED) * 43758.5453;
        return sc<float>((F - std::floor(F)) * 2.0 - 1.0); // -1..1
    }

    float rowDelay(const SPopup& p, size_t i) {
        const float STAGGER = fanStagger(p);
        const float I       = sc<float>(std::min(i, STAGGER_ROWS));
        if (!fanMotion().serp)
            return STAGGER * I;
        // Bottom papers still tend to land first -- they have least far to go --
        // but a pack does not leave in file order.
        // Shifted so the earliest paper leaves at once: any lead-in before
        // the first movement reads as the click lagging.
        float lead = 1e9F;
        const size_t ROWS = std::min(std::max<size_t>(1, p.visible.size() + (p.overflow > 0 ? 1 : 0)), STAGGER_ROWS);
        for (size_t k = 0; k < ROWS; ++k)
            lead = std::min(lead, STAGGER * (0.55F * sc<float>(k) + 0.9F * (rowJitter(p, k, 1) + 1.F) / 2.F));
        return STAGGER * (0.55F * I + 0.9F * (rowJitter(p, i, 1) + 1.F) / 2.F) - lead;
    }

    // How long the opening timeline is: the pane's grow, or the fan's last
    // row landing.
    float timelineSpan(const SPopup& p) {
        if (p.grid)
            return PANE_OPEN_MS;
        const size_t ROWS = std::min(std::max<size_t>(1, p.visible.size() + (p.overflow > 0 ? 1 : 0)), STAGGER_ROWS);
        float        last = 0.F;
        for (size_t i = 0; i < ROWS; ++i)
            last = std::max(last, rowDelay(p, i));
        return fanMotion().rowMs + last;
    }

    // THE CLOCK IS THE FRAMES, not the wall (2026-09-24: "it lags, then loads
    // too fast"). A frame that stalls -- a folder's icons and thumbnails being
    // decoded for the first time -- used to be followed by a timeline that had
    // run on without it and jumped to catch up. Each drawn frame advances it
    // by at most one 30 fps frame instead, so a stall pauses the animation
    // rather than eating it. And an opening waits (up to OPEN_WAIT_MS) for the
    // folder's listing, so the rows fly out with their contents, not over a
    // "Loading" row.
    constexpr float MAX_TICK_MS  = 34.F;
    // The fan row under the pointer blows up (user, 2026-09-24), on the
    // extension's hover curve: 120 ms ease-out-quad, here to 1.1x.
    constexpr float HOVER_SCALE  = 1.10F;
    constexpr float OPEN_WAIT_MS = 400.F;

    void tick(SPopup& p) {
        const auto NOW = Time::steadyNow();
        float      DT  = p.ticked ? std::min(std::chrono::duration<float, std::milli>(NOW - p.lastTick).count(), MAX_TICK_MS) : 0.F;
        // anim_scale, like every other hyprdrive animation: 10 = ten times
        // slower, 0 = snap.
        if (const float AS = ScrollOverview::Config::animScale(); AS <= 0.F)
            DT = DT > 0.F ? 1e6F : 0.F;
        else
            DT /= AS;
        p.lastTick     = NOW;
        p.ticked       = true;
        // Only a FIRST read holds it: a folder re-reading after a change still
        // has its previous entries to show, and shows them.
        if (!p.leaving && p.loading && p.entries.empty() && elapsed(p.since) < OPEN_WAIT_MS)
            DT = 0.F;
        // The hover blow-ups ride the same frame clock.
        p.hoverMoving = false;
        for (auto& [KEY, amt] : p.hoverAmt) {
            const bool ON = KEY == p.hoverKey;
            amt           = std::clamp(amt + (ON ? 1.F : -1.F) * DT / fanMotion().hoverMs, 0.F, 1.F);
            p.hoverMoving |= ON ? amt < 1.F : amt > 0.F;
        }
        std::erase_if(p.hoverAmt, [&](const auto& kv) { return kv.second <= 0.F && kv.first != p.hoverKey; });

        const float SPAN = timelineSpan(p);
        if (p.leaving)
            p.tl = std::max(0.F, p.tl - DT * SPAN / (p.grid ? PANE_CLOSE_MS : fanMotion().closeMs));
        else
            p.tl = std::min(SPAN, p.tl + DT);
    }

    float timelineNow(const SPopup& p) {
        return std::clamp(p.tl, 0.F, timelineSpan(p));
    }

    // 0 closed .. 1 open, on the way in or out alike.
    float popupProgress(const SPopup& p) {
        return std::clamp(timelineNow(p) / std::max(1.F, timelineSpan(p)), 0.F, 1.F);
    }

    // Turn round: whatever the popup is doing, it now heads the other way from
    // exactly where it is.
    void damagePopupMonitor();

    void setDirection(SPopup& p, bool leaving) {
        p.leaving = leaving; // the clock simply runs the other way from here
        // The clock only runs on DRAWN frames, and a fold on the press (its
        // release swallowed) or a turn-round starts no other animation that
        // would ask for one -- so the popup sat frozen at p=0 until something
        // unrelated repainted (2026-09-24 trace). Every change of direction
        // asks for its frame.
        damagePopupMonitor();
    }

    // Row i's three channels on the fan's timeline: where it is (0 in the
    // cell, 1 in place), how big (may pass 1 -- the OutBack pop), how opaque.
    struct SRowEase {
        float pos = 0.F, scale = 0.F, alpha = 0.F, spin = 1.F; // spin: 0 thrown, 1 settled at its tilt
    };
    SRowEase fanRowEase(const SPopup& p, size_t i, float progress) {
        const auto& M  = fanMotion();
        const float T  = progress * timelineSpan(p) - rowDelay(p, i);
        const float TR = std::clamp(T / M.rowMs, 0.F, 1.F);
        const float TA = std::clamp(T / M.alphaMs, 0.F, 1.F);
        if (!M.serp)
            return {outQuad(TR), outQuad(TR), outQuad(TR), 1.F};
        // The spin settles with a small wobble past its tilt, as paper does.
        return {outQuint(TR), outBack(TR, 1.1F), outCubic(TA), outBack(TR, 1.4F)};
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
        const size_t ROWS     = std::max<size_t>(1, p.visible.size() + (p.overflow > 0 ? 1 : 0));
        const bool   EMPTYROW = p.visible.empty(); // "Loading…" / "Empty folder"

        p.rows.clear();
        p.rows.reserve(ROWS);
        double minLeft = 1e9; // the settled fan's leftmost point, labels turned
        for (size_t i = 0; i < ROWS; ++i) {
            const bool  ACTION = EMPTYROW || i >= p.visible.size(); // the empty row, or "N more
            const float FI     = sc<float>(i);
            const float SHIFT  = FAN_SHIFT_K * std::pow(FI, FAN_SHIFT_P) * SCALE;

            // The row's own eased progress on the ONE timeline (fanRowEase):
            // the stagger and the reversal both come from there.
            const auto  EZ = fanRowEase(p, i, progress);
            const float E  = EZ.pos;

            const float ROWH  = FAN_ROW_H * SCALE;
            const float RESTY = BASE - FAN_STEP * SCALE * (FI + 1.F);

            std::string LABEL, ICON;
            if (!ACTION) {
                LABEL = p.entries[p.visible[i]].name;
                ICON  = p.entries[p.visible[i]].icon;
            } else {
                LABEL = p.overflow > 0 ? std::to_string(p.overflow) + " more in Files" : p.loading ? std::string{"Loading\xe2\x80\xa6"} : std::string{"Empty folder"};
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
            CBox rest{std::round(CX + SHIFT - W + ICONW / 2.F + FAN_PAD_X * SCALE), std::round(RESTY), std::round(W), std::round(ROWH)};
            rest.x = std::max(0.0, std::min(rest.x, sc<double>(monitor->m_size.x * SCALE) - rest.width));

            // The extension's flight: the row starts on the cell's centre at
            // 0.1x and flies to its place growing to full size, scaled about its
            // ICON (the pivot the extension sets), so the icons trace the arc.
            // Starts at 0.35x in the thrown motion so the first frame already
            // shows paper leaving (0.1x read as a delay); 0.1x in the extension's.
            const double K0 = fanMotion().serp ? 0.35 : 0.1;
            const double K  = K0 + (1.0 - K0) * EZ.scale;
            const double C1X = rest.x + rest.width - FAN_PAD_X * SCALE - ICONW / 2.0, C1Y = rest.y + rest.height / 2.0;
            const double C0X = p.anchor.x + p.anchor.width / 2.0, C0Y = p.anchor.y + p.anchor.height / 2.0;
            // The path: a straight line for the extension; for the thrown pack
            // a curve that bows to one side by up to a third of the distance
            // (quadratic Bezier, control point off the midpoint, square to
            // the line), so each paper arrives on its own arc.
            double CXN = C0X + (C1X - C0X) * E, CYN = C0Y + (C1Y - C0Y) * E;
            if (fanMotion().serp) {
                const double DX = C1X - C0X, DY = C1Y - C0Y, LEN = std::max(1.0, std::hypot(DX, DY));
                const double BOW = LEN * 0.33 * rowJitter(p, i, 2);
                const double MX = (C0X + C1X) / 2.0 - DY / LEN * BOW, MY = (C0Y + C1Y) / 2.0 + DX / LEN * BOW;
                const double U = 1.0 - E;
                CXN            = U * U * C0X + 2.0 * U * E * MX + E * E * C1X;
                CYN            = U * U * C0Y + 2.0 * U * E * MY + E * E * C1Y;
            }
            // ...and it turns as it flies: from up to 40 degrees off its final
            // tilt, settling onto it.
            const float TILT  = FAN_TILT_DEG * FI * 3.14159265F / 180.F;
            const float THROW = fanMotion().serp ? rowJitter(p, i, 3) * 40.F * 3.14159265F / 180.F : 0.F;
            const float RROT  = TILT + THROW * (1.F - EZ.spin);
            // Where this row's label ends up once settled and turned: its far
            // end swings up and left about the icon by the row's tilt.
            // (The label is drawn from the ICON leftwards -- gap, then its pill
            // -- not from the row box, which is clamped.)
            const double REACH = (ICONW / 2.0 + 8.0 * SCALE + labelW + 20.0 * SCALE) * HOVER_SCALE; // room for the hover blow-up
            minLeft = std::min(minLeft, C1X - REACH * std::cos(TILT) - rest.height / 2.0 * std::sin(TILT));
            CBox         box{CXN - (C1X - rest.x) * K, CYN - (C1Y - rest.y) * K, rest.width * K, rest.height * K};

            p.rows.emplace_back(SRow{
                .box    = box,
                .path   = ACTION ? std::string{} : p.entries[p.visible[i]].path,
                .name   = LABEL,
                .icon   = ICON,
                .action = ACTION,
                .vis    = ACTION ? -1 : sc<int>(i),
                .k      = sc<float>(K),
                .e      = E,
                .a      = EZ.alpha,
                .rot    = RROT,
            });
        }

        // A fan near the screen's left edge would put its turned labels off it
        // (2026-09-24): the whole settled fan moves right by the shortfall,
        // arc intact, each row by as much of it as it has flown.
        const double MARGIN = 8.0 * SCALE;
        if (minLeft < MARGIN)
            for (auto& R : p.rows)
                R.box.x += (MARGIN - minLeft) * R.e;
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
        p.gridBox     = L.grid;
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
    StackModel::SFanFit fanFit(const SPopup& p, PHLMONITOR monitor) {
        const float S = std::max(monitor->m_scale, 0.01F);
        return StackModel::SFanFit{
            .pillTopL  = sc<float>(p.anchor.y) / S,
            .topLimitL = OvInternal::overviewTopChromeBottomL(monitor) - sc<float>(monitor->m_position.y),
            .stepL     = FAN_STEP,
            .gapL      = FAN_GAP,
        };
    }

    // THE FAN'S REAL HEIGHT (2026-09-24: the top rows' labels ran off the
    // screen). Each row turns about its icon by FAN_TILT_DEG per row, so the
    // far (left) end of its label rises by width x sin(angle) -- 30 degrees by
    // the 12th row. How many rows, in order, fit under `roomL` (logical px
    // above the fan's base) when each label is as long as its name makes it?
    float labelSpanL(const std::string& name) {
        // The fan font is monospaced at FAN_FONT px: ~0.6 em per character,
        // capped where the label ellipsizes; plus the pill's padding and the gap.
        return std::min(sc<float>(name.size()) * FAN_FONT * 0.6F, sc<float>(FAN_LABEL_MAX)) + 20.F + 8.F + FAN_ICON / 2.F;
    }
    size_t rotatedFanCapacity(const SPopup& p, const std::vector<std::string>& names, float roomL) {
        constexpr float MARGIN = 8.F;
        size_t          n      = 0;
        for (size_t i = 0; i < names.size(); ++i) {
            const float TILT = FAN_TILT_DEG * sc<float>(i) * 3.14159265F / 180.F;
            const float UP   = FAN_STEP * sc<float>(i + 1) + labelSpanL(names[i]) * std::sin(TILT) + FAN_ROW_H * 0.15F;
            if (UP + MARGIN > roomL)
                break;
            n = i + 1;
        }
        return n;
    }
    std::vector<std::string> fanNames(const SPopup& p, size_t rows) {
        std::vector<std::string> out;
        for (size_t k = 0; k < std::min(rows, p.entries.size()); ++k)
            out.push_back(p.entries[k].name);
        return out;
    }

    bool chooseGrid(const SPopup& p, PHLMONITOR monitor) {
        if (p.view == StackModel::EView::Grid)
            return true;
        if (p.view == StackModel::EView::Fan || !monitor)
            return false;
        // Automatic, as the extension: the pane past 12 items or when the fan
        // would not fit the screen.
        if (p.entries.size() > FAN_MAX_ITEMS)
            return true;
        const auto  F    = fanFit(p, monitor);
        const float ROOM = F.pillTopL - F.gapL - F.topLimitL;
        return rotatedFanCapacity(p, fanNames(p, p.entries.size()), ROOM) < std::max<size_t>(1, p.entries.size());
    }

    // The pane's search: which entries the query keeps, in display order.
    void refilter(SPopup& p) {
        // The preview ring indexes `visible`, which is about to change under it.
        if (p.previewing >= 0) {
            showPreviewClose();
            p.previewing = -1;
        }
        p.visible = StackModel::filter(p.entries, p.grid ? p.query : std::string{});
        // View as Fan on a folder taller than the screen: the first rows in
        // the stack's sort order, and the last row says how many more there
        // are and opens the folder -- the reference's fan does the same.
        p.overflow = 0;
        const size_t CAP = std::min(p.fanCap > 0 ? p.fanCap : FAN_MAX_ITEMS + 1, FAN_MAX_ITEMS + 1); // rows, the "more" row included
        if (!p.grid && p.visible.size() > CAP - 1) {
            const size_t KEEP = std::max<size_t>(1, std::min(CAP - 1, FAN_MAX_ITEMS));
            p.overflow        = p.visible.size() - KEEP;
            p.visible.resize(KEEP);
        }
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
        p.grid   = chooseGrid(p, monitor);
        p.fanCap = 0;
        if (!p.grid && monitor) {
            // Rows that fit with their tilt, the "N more" row included (it is
            // measured as the row after the last item it would follow).
            const auto  F     = fanFit(p, monitor);
            const float ROOM  = F.pillTopL - F.gapL - F.topLimitL;
            auto        names = fanNames(p, FAN_MAX_ITEMS + 1);
            if (p.entries.size() > names.size() - (names.size() > 0 ? 0 : 0))
                names.push_back("999 more in Files");
            p.fanCap = std::max<size_t>(1, rotatedFanCapacity(p, names, ROOM));
        }
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

    // Sushi picks its own size per file (and changes it when Space moves to the
    // next item), so its placement is redone on every resize of its window.
    CHyprSignalListener g_sushiResize;
    bool                g_placingSushi = false;

    // Beside the fan or pane, on the side with more room, and ABOVE the dock:
    // the preview must never sit under the fan or the pill (user, 2026-09-24:
    // it was clamped to the bottom of the screen and both covered it). Too big
    // for that area: scaled down, aspect kept.
    void placeSushi(const PHLWINDOW& W) {
        if (!W || !g_popup || g_popup->previewing < 0 || g_placingSushi)
            return;
        const auto MON = popupMonitorPtr();
        if (!MON)
            return;
        const float S = std::max(MON->m_scale, 0.01F);
        double left = 1e9, right = -1e9;
        if (g_popup->grid && !g_popup->plate.empty()) {
            left  = g_popup->plate.x / S;
            right = (g_popup->plate.x + g_popup->plate.width) / S;
        } else {
            for (const auto& R : g_popup->rows) {
                left  = std::min(left, R.box.x / S);
                right = std::max(right, (R.box.x + R.box.width) / S);
            }
        }
        if (right < left)
            return;

        constexpr double MARGIN = 16.0;
        const double     MONW   = MON->m_size.x;
        const double     TOP    = std::max(0.0, sc<double>(OvInternal::overviewTopChromeBottomL(MON) - sc<float>(MON->m_position.y))) + MARGIN;
        const auto       PILL   = Dock::layout(MON).pill;
        const double     BOTTOM = (PILL.width >= 1 ? PILL.y / S : MON->m_size.y) - MARGIN;
        const bool       RIGHT_SIDE = (MONW - right) >= left;
        const double     AREAX  = RIGHT_SIDE ? right + MARGIN : MARGIN;
        const double     AREAW  = RIGHT_SIDE ? MONW - right - 2 * MARGIN : left - 2 * MARGIN;
        const double     AREAH  = BOTTOM - TOP;
        if (AREAW < 64.0 || AREAH < 64.0)
            return; // no room beside the fan: leave it where the rules put it

        Vector2D   size = W->m_realSize->goal();
        const auto K    = std::min({1.0, AREAW / std::max(1.0, size.x), AREAH / std::max(1.0, size.y)});
        g_placingSushi  = true;
        if (K < 1.0) {
            size = Vector2D{std::floor(size.x * K), std::floor(size.y * K)};
            W->resize(size);
        }
        // Hugging the fan's side, centred in the height above the dock.
        const double X = RIGHT_SIDE ? AREAX : AREAX + AREAW - size.x;
        const double Y = TOP + (AREAH - size.y) / 2.0;
        W->move(Vector2D{MON->m_position.x + X, MON->m_position.y + Y});
        g_placingSushi = false;
    }

    void armSushiPlacement() {
        if (g_sushiOpen)
            return;
        g_sushiOpen = Event::bus()->m_events.window.openLate.listen([](PHLWINDOW w) {
            if (!w || w->m_class != "org.gnome.NautilusPreviewer")
                return;
            placeSushi(w);
            g_sushiResize = w->m_events.resize.listen([weak = PHLWINDOWREF{w}]() {
                if (const auto W = weak.lock())
                    placeSushi(W);
            });
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
            g_popup->tl    = 0.F; // replay the entrance with the new order
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
        // Read each folder now, dock drawn or not: the arrival bounce compares
        // against the last listing, and with the dock autohidden from the start
        // nothing else would read it before the first arrival (which the first
        // listing only records).
        for (const auto& D : dirs)
            StackReader::status(D);
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

    bool pinAt(const std::string& path, size_t index) {
        // Already a stack: the drop only moves it (dragging Downloads out of
        // the file manager onto the dock again reorders it rather than failing).
        if (!pin(path) && !find(canonical(path)))
            return false;
        moveTo(path, index);
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
        traceEvent("open() " + CANON + " state=" + popupState());
        if (g_popup && g_popup->path == CANON) {
            if (!g_popup->leaving)
                close();
            else {
                traceEvent("  -> reverse to OPEN");
                setDirection(*g_popup, false); // caught on its way out: turn round
            }
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
        p.sort       = S ? S->sort : StackModel::ESort::Added;
        p.view       = S ? S->view : StackModel::EView::Auto;
        p.monitor    = monitor->m_id;
        p.anchor     = anchor;
        applyListing(p, sortedListing(CANON, p.sort), monitor);
        p.since      = Time::steadyNow();

        // An EMPTY folder still opens -- with one row saying so. The reference
        // returns early and the click does nothing at all, which is
        // indistinguishable from a broken dock.
        g_popup = std::move(p);
        traceEvent("  -> NEW popup (opening)");
        damagePopupMonitor(); // its first frame: nothing else is guaranteed to ask for it
        armSushiPlacement();
    }

    void onReaderReady() {
        // The wake fd is shared with the overview's file search and the
        // dictionary: only a job of OURS is a reason to rebuild and repaint.
        static uint64_t seen = 0, seenThumbs = 0;
        if (StackReader::generation() == seen && Thumbs::generation() == seenThumbs)
            return;
        // A thumbnail landing is only a repaint: the pile and the popup pick
        // it up from Thumbs::lookup on their next frame.
        if (StackReader::generation() == seen) {
            seenThumbs = Thumbs::generation();
            for (const auto& M : State::monitorState()->monitors())
                if (M)
                    g_pHyprRenderer->damageMonitor(M);
            return;
        }
        seen       = StackReader::generation();
        seenThumbs = Thumbs::generation();

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
            } else {
                armBounce(g_trashStack);
                if (R.trashed)
                    armBounce("trash:///"); // the Trash hops only when something landed in IT
            }
        }
        // DOWNLOADS (2026-09-24, the macOS Downloads stack): a browser's partial
        // file in a stack's folder puts a progress bar on the cell; when the
        // count drops, a download finished (or was cancelled -- the folder
        // cannot tell those apart) and the stack bounces.
        for (const auto& S : all()) {
            if (!S.isDir)
                continue;
            const auto ST = StackReader::status(S.path);
            if (!ST.ready)
                continue;
            auto& was = g_downloads[S.path];
            if (ST.downloads < was)
                armBounce(S.path);

            // SOMETHING ARRIVED (user, 2026-09-25: "when something is added to
            // Downloads the folder should bounce"): a name the last listing of
            // this folder did not have. Not a browser's partial file, and not
            // the placeholder Firefox/Zen create under the final name when a
            // download STARTS (its .part is still beside it) -- that one bounces
            // when the download finishes, above. The first listing only records.
            {
                const auto L = StackReader::listing(S.path);
                if (L.ready && !L.missing) {
                    std::unordered_set<std::string> now;
                    now.reserve(L.entries.size());
                    for (const auto& E : L.entries)
                        now.insert(E.name);
                    auto [IT, FIRST] = g_seenNames.try_emplace(S.path);
                    if (!FIRST) {
                        for (const auto& N : now) {
                            if (IT->second.contains(N) || StackModel::isPartialDownload(N) || now.contains(N + ".part"))
                                continue;
                            armBounce(S.path);
                            break;
                        }
                    }
                    IT->second = std::move(now);
                }
            }
            if (ST.downloads > 0)
                LauncherEntry::start(); // Chromium's progress arrives on the session bus
            was = ST.downloads;
        }
        std::erase_if(g_downloads, [](const auto& kv) { return kv.second == 0; });

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
        g_sushiResize.reset();
        g_popup.reset();
        g_itemDrag.reset();
    }

    void traceEvent(const std::string& what) {
        struct stat st{};
        if (::stat("/tmp/hyprdrive-stacklog.on", &st) != 0)
            return;
        if (FILE* f = std::fopen("/tmp/hyprdrive-stack.log", "a")) {
            const auto MS = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count() % 100000000;
            std::fprintf(f, "%08lld %s\n", sc<long long>(MS), what.c_str());
            std::fclose(f);
        }
    }

    void close() {
        g_itemDrag.reset(); // a drag cannot outlive the popup it started in (review 2026-09-24)
        traceEvent("close() state=" + popupState());
        if (!g_popup || g_popup->leaving)
            return;
        if (g_popup->previewing >= 0) {
            showPreviewClose();
            g_popup->previewing = -1;
        }
        setDirection(*g_popup, true);
    }

    // Only the popup's own rows can start an item drag, so a popup that goes
    // away with a button still down must take the drag with it -- otherwise the
    // next release anywhere would resolve a drop from a fan nobody can see.
    // A fan/pane item pulled past the threshold becomes a REAL drag, the same
    // one a file dragged out of Nautilus is (DockDrop::startFileDrag): any app
    // can take it, and the dock's own cells take it through the drop takeover.
    // Here, on motion, and never in the pass. If another drag is somehow
    // running, the old in-dock drag carries on instead.
    void onPointerMove() {
        if (!g_itemDrag || g_itemDrag->live || !g_popup)
            return;
        const auto MON = popupMonitorPtr();
        if (!MON)
            return;
        const Vector2D MOUSE = (g_pInputManager->getMouseCoordsInternal() - MON->m_position) * MON->m_scale;
        if (MOUSE.distance(g_itemDrag->start) <= DRAG_THRESHOLD * std::max(MON->m_scale, 0.01F))
            return;
        const auto DRAG = *g_itemDrag;
        if (DockDrop::startFileDrag({DRAG.srcPath}, themedIcon(DRAG.icon))) {
            g_itemDrag.reset();
            close();
        } else
            g_itemDrag->live = true;
        damagePopupMonitor();
    }

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
            if (rowHit(R, point))
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
            if (!R.action && !R.path.empty() && rowHit(R, POINT))
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

    void expectJobFor(const std::string& stackPath) {
        g_trashStack = canonical(stackPath);
    }

    void moveFrom(const std::string& stackPath, std::vector<std::string> paths) {
        if (paths.empty())
            return;
        g_trashStack = canonical(stackPath);
        StackReader::moveOrCopyInto(g_trashStack, std::move(paths));
    }

    void trashFrom(const std::string& stackPath, std::vector<std::string> paths) {
        if (paths.empty())
            return;
        g_trashStack = canonical(stackPath);
        StackReader::trash(std::move(paths));
    }

    // WARM-UP (2026-09-24): while the pointer rests on a stack's cell, build
    // the textures its fan or pane will need -- themed icons and thumbnails,
    // a few per frame -- so the click opens onto a folder that is already
    // loaded instead of stalling its first frames on the decodes.
    void warm(const std::string& stackPath) {
        static std::string                     path;
        static uint64_t                        gen = 0;
        static std::vector<StackModel::SEntry> todo;
        const auto                             CANON = canonical(stackPath);
        if (CANON != path || gen != StackReader::generation()) {
            path = CANON;
            gen  = StackReader::generation();
            const auto* S = find(CANON);
            if (!S || !S->isDir) {
                todo.clear();
                return;
            }
            auto L = sortedListing(CANON, S->sort);
            if (!L.ready) {
                gen = 0; // not read yet: try again next frame
                todo.clear();
                return;
            }
            // Only what the popup would show first.
            if (L.entries.size() > 40)
                L.entries.resize(40);
            todo = std::move(L.entries);
            std::ranges::reverse(todo); // popped from the back, first entries first
        }
        for (int n = 0; n < 3 && !todo.empty(); ++n) {
            iconTexture(todo.back(), 64);
            OverviewRender::appIcon(themedIcon(todo.back().icon));
            todo.pop_back();
        }
    }

    std::string themePrefix(const std::string& which) {
        if (which != "omarchy")
            return "";
        const auto T = AppSearch::omarchyIconTheme();
        return T.empty() ? "" : "@" + T + ":";
    }

    std::string themedIcon(const std::string& name) {
        if (name.empty() || name.starts_with('@') || name.starts_with('/') || name.starts_with(BuiltinIcons::PREFIX))
            return name;
        return themePrefix(ScrollOverview::Config::getValue<std::string>("plugin:hyprdrive:dock:stack_icon_theme")) + name;
    }

    size_t downloadsIn(const std::string& stackPath) {
        const auto IT = g_downloads.find(canonical(stackPath));
        return IT == g_downloads.end() ? 0 : IT->second;
    }

    std::string tooltipSuffix(const std::string& stackPath) {
        const auto CANON = canonical(stackPath);
        // A pinned FILE is not a folder: never ask the reader to list it (it would
        // read as missing and every pinned file would say "Folder not found").
        const auto* S = find(CANON);
        if (S && S->isDir)
            if (const auto L = StackReader::status(CANON); L.ready && L.missing)
                return "Folder not found";
        if (CANON == g_trashStack && !g_trashError.empty() && elapsed(g_trashErrorAt) < 3000.F)
            return g_trashError;
        return {};
    }

    // Only a popup that is really in front of the user takes keys: not leaving,
    // its monitor still there, and drawn within the last half second (a popup
    // whose dock stopped drawing -- hidden, another monitor -- must not keep
    // swallowing the keyboard; review 2026-09-24).
    //
    // OR THE POINTER IS ON IT (user, 2026-09-30: "grid mode should be
    // searchable whenever I am moused over anywhere in the grid box"). A
    // settled pane is a still image and is not redrawn, so "drawn within the
    // last half second" went false half a second after the pointer stopped, and
    // typing only filtered while the mouse was moving. A pointer resting on the
    // pane says it is in front of the user just as well.
    bool keyboardActive() {
        if (!g_popup || g_popup->leaving)
            return false;
        const auto MONITOR = popupMonitorPtr();
        if (!MONITOR)
            return false;
        if (elapsed(g_popup->lastDrawn) < 500.F)
            return true;
        const auto POINTER = g_pInputManager->getMouseCoordsInternal();
        return MONITOR->logicalBox().containsPoint(POINTER) && contains(MONITOR, (POINTER - MONITOR->m_position) * MONITOR->m_scale);
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
            case 28: // ENTER: only when something is selected
                if (P.hover < 0 || P.hover >= N)
                    return false;
                launchPath(P.entries[P.visible[sc<size_t>(P.hover)]].path);
                close();
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
        traceEvent(std::string("Stacks::click ") + (release ? "UP  " : "DOWN") + " state=" + popupState() + (g_swallowToggleRelease ? " swallowPending" : ""));
        if (release && std::exchange(g_swallowToggleRelease, false)) {
            traceEvent("  -> release swallowed (toggle)");
            return true;
        }
        if (!g_popup || g_popup->leaving || !monitor)
            return false;
        if (g_popup->monitor != monitor->m_id) {
            // A press on ANOTHER monitor dismisses it too -- otherwise it stays up,
            // unseen, holding the keyboard.
            if (!release)
                close();
            return false;
        }

        // A RELEASE always belongs to the press that started it, wherever the
        // pointer has since gone -- that is the dock's own rule (dragPending),
        // and it is what makes a drop onto ANOTHER cell reachable at all.
        if (release && g_itemDrag) {
            const auto DRAG = *g_itemDrag;
            g_itemDrag.reset();

            if (!DRAG.live) {
                // Never moved: it was a click after all.
                for (const auto& R : g_popup->rows) {
                    if (!rowHit(R, point))
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

                // On the reader's worker, never here: a move to another drive is
                // a copy, and it used to freeze the compositor for its length
                // (review 2026-09-24). The bounce (or the shake and the error in
                // the tooltip) comes when the job lands, in onReaderReady.
                const auto DEST = canonical(ITEM.stackPath);
                if (g_popup)
                    StackReader::invalidate(g_popup->path);
                moveFrom(DEST, {DRAG.srcPath});

                close();
                return true;
            }

            // Released over nothing that accepts it: no move, no acknowledgement.
            close();
            return true;
        }

        // The stack's own cell is a toggle FIRST, whatever is flying over it:
        // a paper on its way out passes across the cell, and a quick second
        // click must fold the fan, not open that file (2026-09-24).
        // The DOCK's hit box, not the bare icon: a press on the pad around the
        // icon is still a click on this cell to the dock, and testing only the
        // icon here folded the fan as a dismissal while the release reopened
        // it -- open/close flicker on every click near the edge (trace,
        // 2026-09-25).
        if (!release && Dock::cellHitBox(g_popup->anchor, monitor->m_scale).containsPoint(point)) {
            traceEvent("  -> own cell press: FOLD");
            close();
            g_swallowToggleRelease = true;
            return true;
        }

        const bool INSIDE = contains(monitor, point);
        if (!INSIDE) {
            // A press that misses dismisses, and is NOT claimed: the click
            // belongs to whatever is under it. That is the hover panel's
            // behaviour and the menu's `menuMissLetsThrough` on the desktop.
            if (!release) {
                // Clicking the stack's own icon again folds it back (user,
                // 2026-09-24), the reference's toggle.
                const bool OWNCELL = g_popup->anchor.containsPoint(point);
                close();
                if (OWNCELL) {
                    g_swallowToggleRelease = true;
                    return true;
                }
            }
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
                if (!rowHit(R, point) || R.path.empty())
                    continue;
                g_itemDrag = SItemDrag{.srcPath = R.path, .icon = R.icon, .start = point};
                break;
            }
            return true;
        }

        for (const auto& R : g_popup->rows) {
            if (!rowHit(R, point))
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
        return g_popup->leaving || popupProgress(*g_popup) < 1.F || g_popup->hoverMoving;
    }

    bool anyDownloading() {
        return !g_downloads.empty();
    }

    bool animating() {
        // A download's bar moves every frame (the indeterminate slide, or the
        // browser's progress arriving over D-Bus), so it keeps the dock drawing.
        return !g_bounces.empty() || !g_shakes.empty() || anyDownloading();
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
        // Kept for the hidden dock's longer peek too (Dock::STACK_PEEK_MS); the
        // on-dock hop itself is over at HOP_MS and draws nothing after.
        std::erase_if(g_bounces, [](const SHop& h) { return elapsed(h.startedAt) > std::max(Dock::HOP_MS, Dock::STACK_PEEK_MS); });
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
            g_itemDrag.reset();
            return;
        }

        tick(*g_popup);
        const float PROGRESS = popupProgress(*g_popup);
        if (g_popup->leaving && PROGRESS <= 0.F) {
            traceEvent("fully closed (popup gone)");
            g_popup.reset();
            g_itemDrag.reset();
            return;
        }

        g_popup->lastDrawn = Time::steadyNow();
        const float SCALE = std::max(monitor->m_scale, 0.01F);
        const float ALPHA = Motion::houseFixed(true, PROGRESS); // the same curve both ways, so a reversal is seamless

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

        // Motion blur (StackBlur): while the fan's rows are sampled, what they
        // draw is collected rather than queued.
        std::vector<StackBlur::SSample>* sink    = nullptr;
        int                              sinkGroup = 0;
        CBox                             sinkBounds;
        const auto drawTex = [&](const SP<Render::ITexture>& tex, const CBox& box, float alpha, float rot = 0.F) {
            if (!tex || !tex->ok() || !drawableBox(box))
                return;
            if (sink) {
                CBox b = box;
                b.rot  = rot;
                sink->push_back(StackBlur::SSample{.tex = tex, .box = b, .alpha = alpha, .group = sinkGroup});
                // Bounds that hold the box however it is turned.
                const double R  = std::hypot(box.width, box.height) / 2.0 + 2.0;
                const double CX = box.x + box.width / 2.0, CY = box.y + box.height / 2.0;
                const CBox   B{CX - R, CY - R, 2 * R, 2 * R};
                if (sinkBounds.width < 1)
                    sinkBounds = B;
                else {
                    const double X0 = std::min(sinkBounds.x, B.x), Y0 = std::min(sinkBounds.y, B.y);
                    const double X1 = std::max(sinkBounds.x + sinkBounds.width, B.x + B.width), Y1 = std::max(sinkBounds.y + sinkBounds.height, B.y + B.height);
                    sinkBounds = CBox{X0, Y0, X1 - X0, Y1 - Y0};
                }
                return;
            }
            CTexPassElement::SRenderData d;
            d.tex     = tex;
            d.box     = box;
            d.box.rot = rot;
            d.a       = alpha;
            g_pHyprRenderer->addPassElement(makeUnique<CTexPassElement>(d));
        };

        // THE DRAG GHOST, and the threshold that promotes a press into a drag.
        // Both ride the pointer read this function already has to do; there is
        // no motion hook and no second input path -- see the note on SItemDrag.
        const Vector2D MOUSE = (g_pInputManager->getMouseCoordsInternal() - monitor->m_position) * monitor->m_scale;
        // The pointer moving takes the selection back from the keyboard.
        if (MOUSE.distance(g_popup->lastMouse) > 2.0)
            g_popup->keyboard = false;
        g_popup->lastMouse = MOUSE;

        const auto drawGhost = [&] {
            if (!g_itemDrag || !g_itemDrag->live)
                return;
            const float SZ = GHOST_SIZE * SCALE;
            drawTex(OverviewRender::appIcon(themedIcon(g_itemDrag->icon)), CBox{std::round(MOUSE.x - SZ / 2.0), std::round(MOUSE.y - SZ / 2.0), std::round(SZ), std::round(SZ)},
                    Dock::DRAG_GHOST_ALPHA);
        };

        if (g_popup->grid) {
            auto&      P     = *g_popup;
            const auto FG    = Config::themeForeground();
            const auto MUTED = Config::themeMutedText();
            const auto HEAD  = Config::themeHeading();
            drawPlate(P.plate, GRID_RADIUS, ALPHA);
            // The field's frost with the plate's: a backdrop blur has nothing to
            // smear, and inside the blurred pass it would frost an empty buffer.
            Dock::frostedPlate(monitor, P.fieldBox, sc<int>(std::round(8.F * SCALE)), ALPHA * 0.5F);

            // MOTION BLUR ON THE ENTRANCE AND EXIT (user, 2026-09-30): while
            // the pane grows out of its icon or back into it, everything on the
            // plate is drawn into a nested pass and smeared from last frame's
            // plate to this one (FlightBlur.hpp) -- the minimize flight's blur,
            // at dock:minimize_motion_blur samples. Settled, it draws directly.
            // The stack pane blur setting (FlightBlur::shape): an exit runs its progress
            // 1 -> 0, so its own clock is the complement.
            const CBox PREVPLATE = FlightBlur::shape(FlightBlur::ESite::STACK_PANE, P.lastPlate, P.plate, P.leaving ? 1.F - PROGRESS : PROGRESS);
            P.lastPlate          = P.plate;
            const bool MOVING    = (P.leaving || PROGRESS < 1.F) && PREVPLATE.width >= 2 && PREVPLATE.height >= 2;
            const auto drawBody  = [&] {
            // The dock's own outline (theme accent, Hyprland's border weight),
            // so the pane reads as part of the dock (user, 2026-09-24).
            {
                CBorderPassElement::SBorderData border;
                border.box           = P.plate.copy().round();
                border.grad1         = ::Config::CGradientValueData(Config::highlightColor().modifyA(ALPHA));
                border.borderSize    = std::max(1, Config::getValue<int>("general:border_size"));
                border.round         = sc<int>(std::round(GRID_RADIUS * SCALE));
                border.roundingPower = 2.F;
                if (drawableBox(border.box))
                    g_pHyprRenderer->addPassElement(makeUnique<CBorderPassElement>(border));
            }

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

            // THE FIELD: an inset plate (frosted above), the query or its
            // placeholder, a caret.
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
                    g_pHyprRenderer->addPassElement(makeUnique<CRectPassElement>(caret));

            // THE CELLS, clipped to the grid: icon, two-line name, and the
            // previewed / selected / hovered highlight.
            const double GTOP = P.fieldBox.y + P.fieldBox.height;
            const CBox   CLIP{P.plate.x, GTOP, P.plate.width, std::max(0.0, P.plate.y + P.plate.height - GTOP)};
            for (const auto& R : P.rows) {
                const bool PREV  = R.vis >= 0 && R.vis == P.previewing;
                const bool SEL   = R.vis >= 0 && P.keyboard && R.vis == P.hover;
                const bool HOVER = !P.keyboard && rowHit(R, MOUSE);
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
                    g_pHyprRenderer->addPassElement(makeUnique<CRectPassElement>(hl));
                }
                const float ICON = GRID_ICON * SCALE * (PREV ? 1.15F : 1.F);
                CBox        ib{std::round(cell.x + (cell.width - ICON) / 2.0), std::round(cell.y + 8.F * SCALE), std::round(ICON), std::round(ICON)};
                const auto TEX = R.vis >= 0 && R.vis < sc<int>(P.visible.size()) ? iconTexture(P.entries[P.visible[sc<size_t>(R.vis)]], sc<int>(std::round(ICON))) :
                                                                                  OverviewRender::appIcon(themedIcon(R.icon));
                if (TEX && TEX->ok() && drawableBox(ib)) {
                    CTexPassElement::SRenderData d;
                    d.tex     = TEX;
                    d.box     = ib;
                    d.a       = ALPHA;
                    d.clipBox = CLIP;
                    g_pHyprRenderer->addPassElement(makeUnique<CTexPassElement>(d));
                }
                if (const auto T = OverviewRender::textWrapped(R.name, FG, sc<int>(std::round(11.F * SCALE)), sc<int>(cell.width - 8.F * SCALE), sc<int>(std::round(30.F * SCALE)),
                                                               false, "", 500);
                    T && T->ok() && drawableBox(cell)) {
                    CTexPassElement::SRenderData l;
                    l.tex     = T;
                    l.box     = CBox{cell.x + (cell.width - T->m_size.x) / 2.0, ib.y + ICON + 6.F * SCALE, T->m_size.x, T->m_size.y}.round();
                    l.a       = ALPHA;
                    l.clipBox = CLIP;
                    g_pHyprRenderer->addPassElement(makeUnique<CTexPassElement>(l));
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
            };
            if (!MOVING || !FlightBlur::drawBlurred(monitor, nullptr, PREVPLATE, P.plate, drawBody))
                drawBody();

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

        g_popup->hoverKey.clear(); // re-found below, from the row actually under the pointer
        const auto drawFanRows = [&](const std::vector<SRow>& ROWS) {
        for (size_t i = 0; i < ROWS.size(); ++i) {
            const auto& R   = ROWS[i];
            const float ROT = R.rot; // its tilt, or where it is in its spin
            const SPileItem* FROMPILE = nullptr;
            for (const auto& PI : PILE)
                if (!R.action && PI.entry.path == R.path)
                    FROMPILE = &PI;

            // Per-row alpha: a row that has not started its stagger is not
            // drawn at all rather than drawn at zero, which keeps its frost off
            // the queue too.
            const float E = R.e; // the row's own eased progress: 0 in the cell, 1 in place
            const float A = R.a; // its own opacity channel
            if (A <= 0.02F && !FROMPILE)
                continue;

            // The previewed row is enlarged 1.15x; it, the keyboard's selection
            // and the pointer's row take the accent on their label pill.
            const bool PREV  = !R.action && sc<int>(i) == g_popup->previewing;
            const bool SEL   = !R.action && g_popup->keyboard && sc<int>(i) == g_popup->hover;
            const bool HOVER = !g_popup->keyboard && rowHit(R, MOUSE); // the "N more" row too: it opens the folder (2026-09-25)
            // The hover blow-up: this row's eased 0..1 on the frame clock.
            const std::string ROWKEY = R.path.empty() ? R.name : R.path;
            if (HOVER) {
                if (g_popup->hoverKey != ROWKEY)
                    g_popup->hoverMoving = true; // a new target: keep the frames coming
                g_popup->hoverKey = ROWKEY;
                g_popup->hoverAmt.try_emplace(ROWKEY, 0.F);
            }
            float HB = 1.F;
            if (const auto IT = g_popup->hoverAmt.find(ROWKEY); IT != g_popup->hoverAmt.end()) {
                const float U = IT->second;
                HB            = 1.F + (HOVER_SCALE - 1.F) * (fanMotion().serp ? outCubic(U) : outQuad(U));
            }

            const float KT   = PREV ? 1.15F : 1.F; // the preview's enlargement (the size text is rasterised at)
            const float KS   = KT * HB;              // ... and the hover blow-up
            const float K    = KS * R.k;             // ... times the row's flight scale

            // THE ROW TURNS AS ONE PIECE about its icon's centre -- the pivot the
            // extension sets -- so the label rides the same arc as the icon
            // (2026-09-24: each part turning about its own centre slid the labels
            // off). No plate behind the row: like the extension's, a row is its
            // icon and its label on a pill of its own, and a pill is a texture,
            // so it can turn with the row where a frost region cannot.
            const float  ICON = FAN_ICON * SCALE * K;
            const CBox&  box  = R.box;
            const double PX   = box.x + box.width - FAN_PAD_X * SCALE * R.k - FAN_ICON * SCALE * R.k / 2.0;
            const double PY   = box.y + box.height / 2.0;
            const double CS = std::cos(ROT), SN = std::sin(ROT);
            // Where a part centred at (x, y) on the upright row lands once turned.
            const auto turned = [&](double x, double y, double w, double h) {
                const double DX = x - PX, DY = y - PY;
                return CBox{PX + DX * CS - DY * SN - w / 2.0, PY + DX * SN + DY * CS - h / 2.0, w, h};
            };
            const CBox ib = turned(PX, PY, ICON, ICON).round();

            if (const auto TEX = OverviewRender::text(R.name, Config::themeForeground(), sc<int>(std::round(FAN_FONT * SCALE * KT)), false, "",
                                                      sc<int>(std::round(FAN_LABEL_MAX * SCALE * KT)), 500);
                TEX && TEX->ok()) {
                // Rendered at the row's full size (one cached texture) and drawn
                // scaled with the row -- flight and blow-up -- rather than
                // re-rasterised at every size.
                const double LW = TEX->m_size.x * R.k * HB, LH = TEX->m_size.y * R.k * HB;
                const double PADX = 10.0 * SCALE * K, PADY = 5.0 * SCALE * K;
                const double RIGHT = PX - ICON / 2.0 - 8.0 * SCALE * K; // the pill ends just left of the icon
                const double CXL = RIGHT - PADX - LW / 2.0;

                // The pill: the theme's background, the accent for the row the
                // pointer, the keyboard or the preview is on.
                const auto PILLW = sc<int>(std::round((TEX->m_size.x + 20.0 * SCALE * KT))), PILLH = sc<int>(std::round(TEX->m_size.y + 10.0 * SCALE * KT));
                // Always the dock's own dark, so the label stays readable; the
                // row being pointed at, selected or previewed says so with the
                // blow-up and a heavier outline, not an accent fill (an accent
                // behind light text washed it out, 2026-09-24).
                const CHyprColor BG = Config::dockBackgroundColor().modifyA(0.92F);
                // Bordered like the dock itself (user, 2026-09-24): the theme
                // accent at Hyprland's border weight, baked into the pill so
                // it turns with it.
                const int BORDER = std::max(1, Config::getValue<int>("general:border_size")) * ((PREV || SEL || HOVER) ? 2 : 1);
                const auto PILLTEX = OverviewRender::roundedRect(PILLW, PILLH, sc<int>(std::round(8.F * SCALE * KT)), BG, Config::highlightColor(), BORDER);
                drawTex(PILLTEX, OverviewRender::padBox(turned(CXL, PY, LW + 2 * PADX, LH + 2 * PADY), PILLTEX), A, ROT);
                drawTex(TEX, turned(CXL, PY, LW, LH).round(), A, ROT);
            }

            if (FROMPILE) {
                // Lerp box and angle from the pile slot to the row; drawn opaque
                // throughout, because it never stops being on screen.
                const CBox& P0 = FROMPILE->box;
                CBox        fly{P0.x + (ib.x - P0.x) * E, P0.y + (ib.y - P0.y) * E, P0.width + (ib.width - P0.width) * E, P0.height + (ib.height - P0.height) * E};
                const auto FLYTEX = iconTexture(FROMPILE->entry, sc<int>(std::round(FAN_ICON * SCALE * KS)), true);
                drawTex(FLYTEX, OverviewRender::padBox(fly, FLYTEX), 1.F, FROMPILE->rot + (ROT - FROMPILE->rot) * E);
            } else
            {
                // Padded, so its turned edge is filtered, not stair-stepped. A
                // file's real preview when it has one (2026-09-24).
                const bool HASENTRY = R.vis >= 0 && sc<size_t>(R.vis) < g_popup->visible.size() && g_popup->visible[sc<size_t>(R.vis)] < g_popup->entries.size();
                const auto ICONTEX  = HASENTRY ? iconTexture(g_popup->entries[g_popup->visible[sc<size_t>(R.vis)]], sc<int>(std::round(ICON)), true) :
                                                 OverviewRender::appIcon(themedIcon(R.icon), false, true);
                drawTex(ICONTEX, OverviewRender::padBox(ib, ICONTEX), A, ROT);
            }
        }
        };

        // REAL MOTION BLUR (user, 2026-09-24; StackBlur.hpp): while the fan
        // moves, lay it out at SAMPLES instants across the time this frame
        // covers and let StackBlur average them. The last sample is "now", so
        // p.rows -- what clicks are tested against -- ends as the current layout.
        const int  SAMPLES = std::clamp(Config::getValue<int>("plugin:hyprdrive:dock:stack_motion_blur"), 0, 32);
        const bool MOVING  = std::abs(PROGRESS - g_popup->lastProgress) > 1e-4F;
        if (SAMPLES > 1 && MOVING && StackBlur::usable(monitor)) {
            std::vector<StackBlur::SSample> samples;
            sink = &samples;
            // One group per instant: StackBlur composites each on its own, then
            // averages them.
            for (int k = SAMPLES - 1, j = 0; k >= 0; --k, ++j) {
                sinkGroup = j;
                const float PK = std::clamp(PROGRESS - (PROGRESS - g_popup->lastProgress) * sc<float>(k) / sc<float>(SAMPLES), 0.F, 1.F);
                layoutFan(*g_popup, monitor, layout.pill, PK);
                drawFanRows(g_popup->rows);
            }
            sink = nullptr;
            StackBlur::queue(std::move(samples), sinkBounds);
        } else
            drawFanRows(g_popup->rows);
        g_popup->lastProgress = PROGRESS;

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

    float bounceAgeMs(const std::string& path) {
        const auto CANON = canonical(path);
        float      age   = -1.F;
        for (const auto& B : g_bounces)
            if (B.path == CANON)
                age = age < 0.F ? elapsed(B.startedAt) : std::min(age, elapsed(B.startedAt));
        return age;
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
