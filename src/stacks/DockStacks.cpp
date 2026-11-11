// First: it widens private members of the Hyprland headers it includes, and
// that only works if nothing has included them before it.
#include "Motion.hpp"
#include "DebugPaths.hpp"
#include "OverviewInternal.hpp"

#include "DockStacks.hpp"
#include "Locations.hpp"
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
#include "Launch.hpp"
#include "LauncherEntry.hpp"
#include "OverviewRender.hpp"
#include "OverviewPassElement.hpp"
#include "StackReader.hpp"
#include "StoreFile.hpp"
#include "TextInput.hpp"
#include "SearchLetters.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
#include <hyprland/src/render/gl/GLTexture.hpp>
#include <drm_fourcc.h>
#include <cairo/cairo.h>
#include <format>
#include <hyprland/src/render/pass/BorderPassElement.hpp>
#include <hyprland/src/render/pass/RectPassElement.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprutils/os/Process.hpp>
#include "StackParts.hpp"

using namespace Hyprdrive;

namespace StacksDetail {

    // Same reasoning as FAVORITES_POLL_MS: all() is on the layout path, which
    // runs about ten times a frame, and the file changes about once a week.
    constexpr float POLL_MS = 500.F;

    // See Stacks::generation(). Moved by reload() when the file actually read
    // into something different, and by writeStore() for this process's own
    // edits (whose mtime reload deliberately adopts, so it would not see them).
    uint64_t g_storeGeneration = 1;

    SStoreState g_storeState;

    wl_event_source* g_inotifySource = nullptr;

    std::string     g_trashStack;   // whose trash job is in flight (Stacks::trashFrom)
    uint64_t        g_trashSeenSeq = 0;
    std::string     g_trashError;   // shown in that stack's tooltip for 3 s
    Time::steady_tp g_trashErrorAt{};

    std::optional<SPopup> g_popup;
    SArcBar g_arcBar;

    // A press on the open stack's own cell closed it; its release must not
    // reach the pill and open it again.
    bool g_swallowToggleRelease = false;

    std::optional<SItemDrag> g_itemDrag;

    std::vector<SHop> g_bounces;

    // Stack path -> browser partial files in it at the last listing. Only
    // folders with a download in flight are kept.
    std::unordered_map<std::string, size_t> g_downloads;

    // A PARTIAL FILE IS A DOWNLOAD ONLY WHILE IT GROWS (review 2026-11-09). A
    // `.part` left behind by a browser that crashed or was killed counted for
    // as long as it sat there: its bar slid on the cell, and animating() kept
    // the dock repainting at full rate, forever. Now only a partial file
    // written in the last few seconds (StackModel::partialLive) draws a bar or
    // asks for frames. Nothing reports a file growing (the inotify watch is on
    // the folder, not on writes into its files -- a download is thousands of
    // them), so a folder holding one is listed again, by StackReader off this
    // thread, on g_partialTimer: every 2 s while a download grows, backing off
    // to once a minute for a file nobody writes.
    //
    // Stack path -> the mtime (unix s) of each partial file at the last listing.
    std::unordered_map<std::string, std::vector<int64_t>> g_partialMtimes;
    wl_event_source*                                      g_partialTimer = nullptr;

    // Stack path -> the entry names its folder had at the last listing (for the
    // arrival bounce).
    std::unordered_map<std::string, std::unordered_set<std::string>> g_seenNames;
    std::vector<SHop> g_shakes;

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

} // anonymous namespace

// ============================================================================
// EVERYTHING BELOW IS OUTSIDE THE ANONYMOUS NAMESPACE.
//
// symcheck.sh:14-16 records what happens otherwise: Dock::panelClick was once
// defined inside Dock.cpp's anonymous namespace, linked clean, and killed the
// compositor on the next mouse press when the lazy bind failed. Every symbol
// declared in DockStacks.hpp must be defined here, at namespace scope.
// ============================================================================
namespace Hyprdrive::Stacks {

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

    void shutdown() {
        g_arcBar = SArcBar{};
        if (g_inotifySource) {
            wl_event_source_remove(g_inotifySource);
            g_inotifySource = nullptr;
        }
        if (g_partialTimer) {
            wl_event_source_remove(g_partialTimer);
            g_partialTimer = nullptr;
        }
        g_sushiOpen.reset();
        g_sushiResize.reset();
        g_popup.reset();
        g_itemDrag.reset();
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
    void warm(const std::string& stackPath, float scale) {
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
        // At the size the fan's rows ask for (FANPX there, a plain row).
        const int FANPX = sc<int>(std::round(FAN_ICON * std::max(scale, 0.01F) * HOVER_SCALE));
        for (int n = 0; n < 3 && !todo.empty(); ++n) {
            iconTexture(todo.back(), FANPX, true);
            OverviewRender::appIcon(themedIcon(todo.back().icon), false, true, FANPX);
            todo.pop_back();
        }
    }

    std::string themePrefix(const std::string& which) {
        if (which != "omarchy")
            return "";
        // No icons in the Omarchy theme (user, 2026-11-08: a theme without an
        // icons.theme left every dock folder in Adwaita's): the file
        // manager's theme, as the network drives already do.
        auto T = AppSearch::omarchyIconTheme();
        if (T.empty())
            T = Locations::nautilusIconTheme();
        return T.empty() ? "" : "@" + T + ":";
    }

    std::string themedIcon(const std::string& name) {
        if (name.empty() || name.starts_with('@') || name.starts_with('/') || name.starts_with(BuiltinIcons::PREFIX))
            return name;
        return themePrefix(Hyprdrive::Config::getValue<std::string>("plugin:hyprdrive:dock:stack_icon_theme")) + name;
    }

    size_t downloadsIn(const std::string& stackPath) {
        // Still being written: a partial file nobody writes any more is not a
        // download (g_partialMtimes).
        const auto IT = g_partialMtimes.find(canonical(stackPath));
        return IT == g_partialMtimes.end() ? 0 : livePartials(IT->second);
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

    // What noteAxis was told about the event scroll() is handling.
    static double g_axisDelta = 0.0;
    static bool   g_axisWheel = true;

    void noteAxis(double delta, bool wheel) {
        g_axisDelta = delta;
        g_axisWheel = wheel;
    }

    bool scroll(PHLMONITOR monitor, const Vector2D& point, bool up) {
        if (!g_popup || g_popup->leaving || !monitor)
            return false;
        // The fan answers anywhere in its area: a tick between two rows must
        // not scroll the window behind it.
        const bool FANAREA = !g_popup->grid && g_popup->monitor == monitor->m_id && (g_popup->fanBounds.containsPoint(point) || g_popup->filterPill.containsPoint(point));
        if (!FANAREA && !contains(monitor, point))
            return false;

        if (!g_popup->grid) {
            auto&       P    = *g_popup;
            const float MAXS = fanMaxScroll(P);
            if (MAXS <= 0.F)
                return true; // everything is showing: the tick is swallowed, it has nowhere to go
            // UP drops the bottom rows into the cell and brings the next ones
            // down from above; DOWN lifts them back out. By distance, as
            // Nautilus scrolls (WHEEL_NOTCH), settling on a row once it rests.
            const float PERROW = g_axisWheel ? WHEEL_NOTCH : FAN_STEP;
            P.scrollGoal       = std::clamp(P.scrollGoal + sc<float>(std::abs(g_axisDelta)) / PERROW * (up ? 1.F : -1.F), 0.F, MAXS);
            P.fingerScroll     = true;
            P.lastScrollAt = Time::steadyNow();
            P.keyboard     = false;
            damagePopupMonitor();
            return true;
        }

        const float SCALE = std::max(monitor->m_scale, 0.01F);
        g_popup->scroll += (up ? -1.F : 1.F) * (Pane::CELL_H + Pane::SPACING) * SCALE * 0.5F;
        g_popup->keyboard = false;
        damagePopupMonitor();
        return true;
    }

    bool scrollBy(float rows) {
        if (!g_popup || g_popup->leaving || g_popup->grid)
            return false;
        auto& P        = *g_popup;
        P.scrollGoal   = std::clamp(std::round(P.scrollGoal + rows), 0.F, fanMaxScroll(P));
        P.lastScrollAt = Time::steadyNow();
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
        // The fan's scroll, its scrollbar fading, and its filter field's
        // springs and letters are wall-clock motion nothing else reports.
        const auto& P = *g_popup;
        if (scrollMoving(P) || P.fingerScroll || P.barAlpha > 0.F || P.filterMoving || P.letters.busy(Time::steadyNow()))
            return true;
        return g_popup->leaving || popupProgress(*g_popup) < 1.F || g_popup->hoverMoving;
    }

    bool anyDownloading() {
        return std::ranges::any_of(g_partialMtimes, [](const auto& kv) { return livePartials(kv.second) > 0; });
    }

    bool animating() {
        retireHops();
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
            out += "|s" + std::to_string(sc<int>(std::round(g_popup->scrollPos * 100.F))) + "|a" + std::to_string(sc<int>(std::round(g_popup->barAlpha * 100.F)));
            out += "|q" + g_popup->query + "|w" + std::to_string(sc<int>(std::round(g_popup->filterW)));
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
