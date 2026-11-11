// Dock stacks: the popup -- open and close, the folder listing, the Sushi previewer, the
// partial-download timer, and its input (pointer, click, keys, drag). Split out of
// DockStacks.cpp; the state is still there (StackParts.hpp).
#include "StackParts.hpp"

using namespace StacksDetail;

namespace StacksDetail {

    int64_t unixNow() {
        return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }

    size_t livePartials(const std::vector<int64_t>& mtimes) {
        const int64_t NOW = unixNow();
        return sc<size_t>(std::ranges::count_if(mtimes, [&](int64_t m) { return StackModel::partialLive(m, NOW); }));
    }

    // List the folders holding a partial file again; onReaderReady() sees the
    // fresh mtimes when the reads land, redraws, and re-arms this. While a
    // download grows, only the folders it grows in: a folder being re-read is
    // unready, and a listing landing meanwhile for another one blanks the
    // re-read folder's pile until its own read lands (newestFour, StackIcon).
    int onPartialTimer(void*) {
        const bool ANYLIVE = std::ranges::any_of(g_partialMtimes, [](const auto& kv) { return livePartials(kv.second) > 0; });
        for (const auto& [PATH, COUNT] : g_downloads) {
            const auto IT = g_partialMtimes.find(PATH);
            if (ANYLIVE && (IT == g_partialMtimes.end() || livePartials(IT->second) == 0))
                continue;
            StackReader::invalidate(PATH);
            StackReader::status(PATH); // queues the read
        }
        return 0;
    }

    void armPartialTimer() {
        if (g_downloads.empty()) {
            if (g_partialTimer)
                wl_event_source_timer_update(g_partialTimer, 0);
            return;
        }
        if (!g_partialTimer && g_pCompositor && g_pCompositor->m_wlEventLoop)
            g_partialTimer = wl_event_loop_add_timer(g_pCompositor->m_wlEventLoop, onPartialTimer, nullptr);
        if (!g_partialTimer)
            return;
        // 2 s while one grows, so a bar whose download stalls goes a few
        // seconds after it stops counting; a minute for one written long ago.
        int64_t newest = 0;
        for (const auto& [PATH, MTIMES] : g_partialMtimes)
            for (const int64_t M : MTIMES)
                newest = std::max(newest, M);
        wl_event_source_timer_update(g_partialTimer, StackModel::partialRecheckMs(newest, unixNow()));
    }

    // A file opens in the app the desktop has for it -- GIO's choice, made by
    // `gio open` in a process of its own, started through Launch::wrap like
    // every other launch (review 2026-11-09). g_app_info_launch_default_for_uri
    // in here spawned a handler that is not D-Bus activated as a child of
    // Hyprland, inside its unit under uwsm, and never told Intent a launch was
    // coming, so with quiet_new_windows a slow app opened without the focus.
    // `gio open` keeps the same handler choice, terminal apps and D-Bus
    // activation included.
    void launchPath(const std::string& path) {
        if (path.empty())
            return;
        char* uri = g_filename_to_uri(path.c_str(), nullptr, nullptr);
        if (!uri)
            return;
        std::string              binary = "gio";
        std::vector<std::string> args   = {"open", uri};
        g_free(uri);
        Launch::wrap(binary, args);
        Hyprutils::OS::CProcess(binary, args).runAsync();
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

    // The search: which entries the query keeps, in display order -- the
    // pane's field, or the fan's (2026-10-08: type while on the fan).
    void refilter(SPopup& p) {
        // The preview ring indexes `visible`, which is about to change under it.
        if (p.previewing >= 0) {
            showPreviewClose();
            p.previewing = -1;
        }
        p.visible = StackModel::filter(p.entries, p.query);
        if (p.hover >= sc<int>(p.visible.size()))
            p.hover = p.visible.empty() ? -1 : 0;
        p.scrollGoal = std::clamp(p.scrollGoal, 0.F, fanMaxScroll(p));
    }

    // Everything a new listing changes: the entries, the visible order, the
    // fan-or-pane decision, and a selection that fell off the end.
    void applyListing(SPopup& p, StackReader::SListing&& L, PHLMONITOR monitor) {
        p.entries   = std::move(L.entries);
        p.loading   = !L.ready;
        p.truncated = L.truncated;
        p.readerGen = StackReader::generation();
        p.grid      = chooseGrid(p, monitor);
        p.slots     = 0;
        if (!p.grid && monitor) {
            // Rows that fit with their tilt. A folder that fits shows all of
            // it; one that does not scrolls, and is measured as if every slot
            // held its longest name -- whichever rows scroll into the top slots,
            // their labels must still clear the bar.
            const auto   F     = fanFit(p, monitor);
            const float  ROOM  = F.pillTopL - F.gapL - F.topLimitL;
            const size_t EXACT = rotatedFanCapacity(p, fanNames(p, FAN_MAX_ITEMS), ROOM);
            std::string  longest;
            for (const auto& E : p.entries)
                if (E.name.size() > longest.size())
                    longest = E.name;
            if (p.entries.size() <= EXACT)
                p.slots = std::max<size_t>(1, EXACT);
            else // one slot less: the "N more in Files" row takes the top one
                p.slots = std::max<size_t>(1, rotatedFanCapacity(p, std::vector<std::string>(FAN_MAX_ITEMS + 1, longest), ROOM)) - 1;
            p.slots = std::max<size_t>(1, p.slots);
            p.slots     = std::min(p.slots, FAN_MAX_ITEMS);
            p.maxLabelW = std::min(sc<float>(longest.size()) * FAN_FONT * 0.6F, sc<float>(FAN_LABEL_MAX)) * std::max(monitor->m_scale, 0.01F);
        }
        refilter(p);
        if (p.hover >= sc<int>(p.visible.size()))
            p.hover = p.visible.empty() ? -1 : 0;
        if (p.previewing >= sc<int>(p.visible.size()))
            p.previewing = -1;
    }

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

}

namespace Hyprdrive::Stacks {

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
                    auto& partial = g_partialMtimes[S.path];
                    partial.clear();
                    for (const auto& E : L.entries) {
                        now.insert(E.name);
                        if (StackModel::isPartialDownload(E.name))
                            partial.push_back(E.mtime);
                    }
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
        // A folder unpinned mid-download is not the dock's to watch any more.
        std::erase_if(g_downloads, [](const auto& kv) { return kv.second == 0 || !find(kv.first); });
        std::erase_if(g_partialMtimes, [](const auto& kv) { return !g_downloads.contains(kv.first); });
        armPartialTimer();

        ++g_storeGeneration; // the dock's item cache (and the pile) key on this
        for (const auto& M : State::monitorState()->monitors())
            if (M)
                g_pHyprRenderer->damageMonitor(M);
    }

    void traceEvent(const std::string& what) {
        // Ours only, in the private runtime directory (DebugPaths.hpp).
        if (!Hyprdrive::DebugPaths::flagOn("hyprdrive-stacklog.on"))
            return;
        if (FILE* f = Hyprdrive::DebugPaths::openLog("hyprdrive-stack.log")) {
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
        if (!g_popup->grid && g_popup->filterPill.containsPoint(point))
            return true; // the filter field is part of the fan: a press on it is not a miss

        for (const auto& R : g_popup->rows) {
            if (rowHit(R, point))
                return true;
        }
        return false;
    }

    // THE FAN'S GAPS ARE THE FAN'S (user, 2026-11-03, on a canvas desktop:
    // hovering the Downloads fan, "the window steals my focus and moves my
    // cursor away from the fan stack"). contains() is the rows alone, so the
    // space between two papers, or between the fan and its icon, was the
    // window underneath's: focus followed the mouse onto it, and Mosaic panned
    // the canvas to the window -- the pointer riding along, off the fan. For
    // hover, the fan's drawn area and its icon cell are one piece.
    bool ownsHover(PHLMONITOR monitor, const Vector2D& point) {
        if (contains(monitor, point))
            return true;
        if (!g_popup || g_popup->leaving || !monitor || g_popup->monitor != monitor->m_id)
            return false;
        CBox area = g_popup->grid ? g_popup->plate : g_popup->fanBounds;
        if (area.empty())
            return false;
        if (!g_popup->anchor.empty()) {
            const double X0 = std::min(area.x, g_popup->anchor.x), Y0 = std::min(area.y, g_popup->anchor.y);
            const double X1 = std::max(area.x + area.width, g_popup->anchor.x + g_popup->anchor.width);
            const double Y1 = std::max(area.y + area.height, g_popup->anchor.y + g_popup->anchor.height);
            area = CBox{X0, Y0, X1 - X0, Y1 - Y0};
        }
        return area.containsPoint(point);
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
    // THE FAN TAKES TYPING only while the pointer is on it (user, 2026-10-08:
    // "if the stack is open and I'm focused on it (mousing-over the stack)"):
    // anywhere in its area, on its cell, or on the filter field itself. The
    // AREA, not the rows: a filter removes rows from under a still pointer,
    // and the next letter must not fall through to the window behind.
    static bool fanFocused() {
        if (!g_popup || g_popup->leaving || g_popup->grid)
            return false;
        const auto MONITOR = popupMonitorPtr();
        if (!MONITOR)
            return false;
        const auto POINTER = g_pInputManager->getMouseCoordsInternal();
        if (!MONITOR->logicalBox().containsPoint(POINTER))
            return false;
        const Vector2D P = (POINTER - MONITOR->m_position) * MONITOR->m_scale;
        // A FILTER MUST NOT TAKE THE KEYBOARD AWAY (user, 2026-10-08): typing
        // with the pointer on a high row shrinks the fan below the pointer, and
        // the next letter left for the window behind. While a query is up, the
        // area the fan had when it started still counts.
        if (!g_popup->query.empty() && g_popup->typingBounds.containsPoint(P))
            return true;
        return g_popup->fanBounds.containsPoint(P) || g_popup->filterPill.containsPoint(P) || Dock::cellHitBox(g_popup->anchor, MONITOR->m_scale).containsPoint(P);
    }

    // A query changed: refilter, start the list from its top, and keep a
    // settled fan settled (its timeline's span depends on its row count, and a
    // span that grew under it would replay the entrance).
    static void queryChanged(SPopup& P) {
        // The first letter freezes the area the fan covers right now (it is
        // still laid out unfiltered); clearing the query lets it go.
        if (P.query.empty())
            P.typingBounds = {};
        else if (P.typingBounds.empty())
            P.typingBounds = P.fanBounds;
        const bool SETTLED = !P.leaving && P.tl >= timelineSpan(P) - 0.5F;
        refilter(P);
        P.scroll     = 0.F;
        P.scrollGoal = 0.F;
        P.scrollPos  = 0.F;
        P.scrollVel  = 0.F;
        P.hover      = P.visible.empty() ? -1 : 0;
        if (SETTLED)
            P.tl = timelineSpan(P);
        P.typing = !P.query.empty();
        damagePopupMonitor();
    }

    // Keep the fan's keyboard selection on screen.
    static void revealHover(SPopup& P) {
        if (P.grid || P.hover < 0 || P.slots == 0)
            return;
        const float H = sc<float>(P.hover), S = sc<float>(P.slots);
        if (H < P.scrollGoal)
            P.scrollGoal = H;
        else if (H > P.scrollGoal + S - 1.F)
            P.scrollGoal = H - S + 1.F;
        P.scrollGoal   = std::clamp(std::round(P.scrollGoal), 0.F, fanMaxScroll(P));
        P.lastScrollAt = Time::steadyNow();
    }

    bool keyboardActive() {
        if (!g_popup || g_popup->leaving)
            return false;
        const auto MONITOR = popupMonitorPtr();
        if (!MONITOR)
            return false;
        if (Hyprdrive::Config::realElapsedMs(g_popup->lastDrawn) < 500.F)
            return true;
        if (!g_popup->grid && fanFocused())
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

        // The pane always types; the fan while the pointer is on it.
        if (P.grid || fanFocused()) {
            if (keycode == 14) { // BACKSPACE
                if (!P.query.empty()) {
                    TextInput::popCodepoint(P.query);
                    queryChanged(P);
                    return true;
                }
                if (P.grid)
                    return true;
            } else if (keycode == 1 && !P.query.empty() && P.previewing < 0) { // ESC clears first
                P.query.clear();
                queryChanged(P);
                return true;
            } else if (keycode == 57 && P.typing && !P.query.empty()) { // SPACE mid-query is a space
                P.query += ' ';
                queryChanged(P);
                return true;
            } else if (keycode != 57 && keycode != 28) { // SPACE previews, ENTER opens
                if (const auto K = TextInput::keyFor(overviewActiveXkbState(), keycode); K.consume) {
                    P.query += K.text;
                    queryChanged(P);
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
        P.typing   = false;
        revealHover(P);
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

    bool setQuery(const std::string& query) {
        if (!g_popup || g_popup->leaving)
            return false;
        g_popup->query = query;
        queryChanged(*g_popup);
        return true;
    }

}
