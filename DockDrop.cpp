// See DockDrop.hpp.
//
// FIRST, before anything includes it: the drag's source is the private
// CWLDataDeviceProtocol::m_dnd.currentSource (member layout unchanged, access
// only). Its dependencies go in UNwidened, so the define touches this header only.
#include <any>
#include <cstdint>
#include <optional>
#include <sstream>
#include <vector>
#include <hyprland/src/protocols/WaylandProtocol.hpp>
#include <wayland-server-protocol.h>
#include <hyprland/src/helpers/signal/Signal.hpp>
#include <hyprland/src/helpers/math/Math.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprland/src/protocols/types/DataDevice.hpp>
#include <hyprutils/os/FileDescriptor.hpp>
#define private public
#include <hyprland/src/protocols/core/DataDevice.hpp>
#undef private

#include "DockDrop.hpp"

#include "Dock.hpp"
#include "DockDropModel.hpp"
#include "DockStacks.hpp"
#include "IOverview.hpp"
#include "StackReader.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/view/WLSurface.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/pointer/cursor/CursorShapeOverrideController.hpp>
#include <hyprland/src/render/pass/TexPassElement.hpp>
#include "OverviewRender.hpp"

#include <glib.h>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/state/MonitorState.hpp>

#include <cerrno>
#include <sys/resource.h>
#include <unordered_map>
#include <thread>
#include <mutex>
#include <fstream>
#include <filesystem>
#include <deque>
#include <condition_variable>
#include <chrono>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
    using namespace ScrollOverview;
    using DockDropModel::EAction;
    using DockDropModel::ECell;

    struct SState {
        WP<IDataSource>          source; // the drag we asked
        std::string              data;   // text/uri-list so far
        bool                     eof = false; // everything asked for has arrived
        bool                     uriDone = true, imgDone = true;
        std::vector<std::string> paths;
        size_t                   files = 0, dirs = 0;

        // FROM A BROWSER (2026-09-24): no local file, but a web address in the
        // uri-list and/or the image itself in an image/* format.
        std::vector<std::string> webUrls;
        std::string              imgMime, imgData;
        bool                     web = false;

        ECell       cell = ECell::None;
        std::string stack;     // the hovered stack's path
        int         gap  = -1; // the stack index the gap is at
    };
    SState           g;
    wl_event_source* g_readSource = nullptr;
    int              g_readFd     = -1;
    wl_event_source* g_imgSource  = nullptr;
    int              g_imgFd      = -1;
    uint64_t         g_placeholderGen = 1;

    // A drop released before the source finished answering: acted on when the
    // list completes (or dropped, if that takes longer than PENDING_MS).
    struct SPending {
        Dock::SDropHit asFiles, asFolder;
        std::chrono::steady_clock::time_point at;
    };
    std::optional<SPending> g_pending;
    constexpr int           PENDING_MS = 3000;
    std::string      g_placeholderPath;
    int              g_placeholderIndex = -1;

    void closeUri() {
        if (g_readSource)
            wl_event_source_remove(g_readSource);
        g_readSource = nullptr;
        if (g_readFd >= 0)
            ::close(g_readFd);
        g_readFd = -1;
    }
    void closeImg() {
        if (g_imgSource)
            wl_event_source_remove(g_imgSource);
        g_imgSource = nullptr;
        if (g_imgFd >= 0)
            ::close(g_imgFd);
        g_imgFd = -1;
    }
    void closeRead() {
        closeUri();
        closeImg();
    }

    // http(s) lines of a uri-list (a browser's drag of a picture or a link).
    std::vector<std::string> webUrlsOf(const std::string& data) {
        std::vector<std::string> out;
        std::istringstream       in(data);
        std::string              line;
        while (std::getline(in, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                line.pop_back();
            if (line.starts_with("http://") || line.starts_with("https://"))
                out.push_back(line);
        }
        return out;
    }

    void damageAll() {
        for (const auto& M : State::monitorState()->monitors())
            if (M)
                g_pHyprRenderer->damageMonitor(M);
    }

    void setPlaceholder(int index, const std::string& path) {
        if (index == g_placeholderIndex && path == g_placeholderPath)
            return;
        g_placeholderIndex = index;
        g_placeholderPath  = path;
        ++g_placeholderGen;
        damageAll();
    }

    void reset() {
        closeRead();
        const bool HAD = g.cell != ECell::None || g_placeholderIndex >= 0;
        g              = SState{};
        setPlaceholder(-1, "");
        if (HAD)
            damageAll();
    }

    // The list arrived: decode it and count files vs folders (a few local
    // stats, once per drag).
    void finishList() {
        g.paths   = DockDropModel::parseUriList(g.data);
        g.webUrls = webUrlsOf(g.data);
        g.files = g.dirs = 0;
        // Nothing local, but a picture or a web address: one thing to save.
        g.web = g.paths.empty() && (!g.imgData.empty() || !g.webUrls.empty());
        if (g.web)
            g.files = 1;
        for (const auto& P : g.paths) {
            struct stat st{};
            if (::stat(P.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
                ++g.dirs;
            else
                ++g.files;
        }
    }

    struct SWeb {
        bool                     web = false;
        std::vector<std::string> urls;
        std::string              mime, data;
    };
    void saveWeb(const std::string& stackPath, SWeb w); // below

    void perform(const Dock::SDropHit& hit, EAction act, const std::vector<std::string>& paths, SWeb web = {}) {
        if (web.web) {
            // Only a stack takes something from the web: it is saved into the
            // folder. Onto Trash or a gap it means nothing.
            if (act == EAction::IntoStack)
                saveWeb(hit.stackPath, std::move(web));
            return;
        }
        switch (act) {
            case EAction::IntoStack: Stacks::moveFrom(hit.stackPath, paths); break;
            case EAction::ToTrash: Stacks::trashFrom("", paths); break;
            case EAction::PinAt: Stacks::pinAt(paths.front(), sc<size_t>(std::max(0, hit.gapIndex))); break;
            case EAction::None: break;
        }
    }

    void finishPending() {
        if (!g_pending)
            return;
        const auto P = *g_pending;
        g_pending.reset();
        if (std::chrono::steady_clock::now() - P.at > std::chrono::milliseconds(PENDING_MS))
            return;
        const bool FOLDER = g.dirs == 1 && g.files == 0;
        const auto& HIT   = FOLDER ? P.asFolder : P.asFiles;
        perform(HIT, DockDropModel::resolve(HIT.cell, g.files, g.dirs), g.paths, SWeb{g.web, g.webUrls, g.imgMime, g.imgData});
        g = SState{};
    }

    void maybeComplete() {
        if (!g.uriDone || !g.imgDone)
            return;
        g.eof = true;
        finishList();
        finishPending();
        damageAll();
    }

    int onReadable(int fd, uint32_t, void*) {
        char buf[4096];
        for (;;) {
            const ssize_t N = ::read(fd, buf, sizeof(buf));
            if (N > 0) {
                g.data.append(buf, sc<size_t>(N));
                if (g.data.size() > (1u << 20)) // a megabyte of URIs is not a drag of files
                    g.data.clear();
                continue;
            }
            if (N == 0 || errno != EAGAIN) {
                g.uriDone = true;
                closeUri();
                maybeComplete();
            }
            break;
        }
        return 0;
    }

    int onImgReadable(int fd, uint32_t, void*) {
        char buf[65536];
        for (;;) {
            const ssize_t N = ::read(fd, buf, sizeof(buf));
            if (N > 0) {
                if (g.imgData.size() < (64u << 20)) // a 64 MB picture is the limit
                    g.imgData.append(buf, sc<size_t>(N));
                continue;
            }
            if (N == 0 || errno != EAGAIN) {
                g.imgDone = true;
                closeImg();
                maybeComplete();
            }
            break;
        }
        return 0;
    }

    void requestList(const SP<IDataSource>& src) {
        g_pending.reset();
        closeRead();
        g        = SState{};
        g.source = src;
        const auto MIMES = src->mimes();
        bool       hasUris = false;
        std::string img;
        std::string all;
        for (const auto& M : MIMES) {
            hasUris |= M == "text/uri-list";
            all += M + " ";
        }
        for (const char* WANT : {"image/png", "image/jpeg", "image/webp", "image/gif", "image/avif", "image/bmp"})
            if (img.empty() && std::ranges::find(MIMES, std::string{WANT}) != MIMES.end())
                img = WANT;
        Stacks::traceEvent("drag offers: " + all);
        if (!hasUris && img.empty())
            return; // neither files nor a picture: the dock ignores it

        const auto ASK = [&](const char* mime, int& fd, wl_event_source*& source, wl_event_loop_fd_func_t cb) {
            int fds[2];
            if (pipe2(fds, O_CLOEXEC | O_NONBLOCK) != 0)
                return false;
            fd     = fds[0];
            source = wl_event_loop_add_fd(g_pCompositor->m_wlEventLoop, fds[0], WL_EVENT_READABLE, cb, nullptr);
            src->send(mime, Hyprutils::OS::CFileDescriptor{fds[1]}); // the source owns the write end now
            return true;
        };
        if (hasUris)
            g.uriDone = !ASK("text/uri-list", g_readFd, g_readSource, onReadable);
        if (!img.empty()) {
            g.imgMime = img;
            g.imgDone = !ASK(img.c_str(), g_imgFd, g_imgSource, onImgReadable);
        }
        if (g.uriDone && g.imgDone)
            maybeComplete();
    }
}

namespace {
    // ---- DRAGGING OUT (2026-09-24) ------------------------------------------
    // A file dragged out of a fan is a REAL Wayland drag, the same one Nautilus
    // starts: the plugin is the data source (IDataSource is an interface; the
    // drag state and every offer only hold it weakly), so any client --
    // Nautilus, a browser's upload field, a chat window -- receives it as it
    // would a file from the file manager, and the dock's own cells take it
    // through the drop takeover above. COPY and MOVE are offered, as Nautilus
    // does: for file URIs the TARGET performs a move, the source never deletes.
    class CStackDragSource : public IDataSource {
      public:
        explicit CStackDragSource(std::vector<std::string> paths) : m_paths(std::move(paths)) {}

        std::vector<std::string> mimes() override {
            return {"text/uri-list", "text/plain;charset=utf-8", "UTF8_STRING"};
        }

        void send(const std::string& mime, Hyprutils::OS::CFileDescriptor fd) override {
            std::string data;
            for (const auto& P : m_paths) {
                if (mime == "text/uri-list") {
                    gchar* uri = g_filename_to_uri(P.c_str(), nullptr, nullptr);
                    if (!uri)
                        continue;
                    data += std::string(uri) + "\r\n";
                    g_free(uri);
                } else
                    data += (data.empty() ? "" : "\n") + P;
            }
            // A pipe holds far more than a handful of paths: one blocking-free
            // pass, whatever does not fit is the reader's loss, never a stall
            // of the compositor.
            const int FD    = fd.get();
            const int FLAGS = fcntl(FD, F_GETFL);
            fcntl(FD, F_SETFL, FLAGS | O_NONBLOCK);
            size_t at = 0;
            while (at < data.size()) {
                const ssize_t N = ::write(FD, data.data() + at, data.size() - at);
                if (N <= 0)
                    break;
                at += sc<size_t>(N);
            }
        }

        void accepted(const std::string&) override {}
        void cancelled() override {
            m_over = true;
        }
        void error(uint32_t, const std::string&) override {
            m_over = true;
        }
        void sendDndFinished() override {
            m_over = true;
        }
        uint32_t actions() override {
            return WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY | WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE;
        }

        bool                     m_over = false;
        std::vector<std::string> m_paths;
    };

    SP<CStackDragSource> g_out;       // the drag we started, while it lasts
    std::string          g_outIcon;   // what the ghost under the pointer shows

    bool outActive() {
        return g_out && !g_out->m_over && PROTO::data && PROTO::data->m_dnd.currentSource.lock() == g_out;
    }
}

namespace {
    // ---- SAVING WHAT A BROWSER DROPPED (2026-09-24) -------------------------
    // Off the compositor: a worker writes the picture's bytes -- or downloads
    // the address with curl -- into a staging folder beside the user's cache
    // (same filesystem as home, so the final step is a rename), then hands the
    // file to StackReader's move job like any other drop, whose result bounces
    // the stack (or shakes it with the error).
    struct SFetch {
        std::string dir, name, url, data;
    };
    std::mutex              g_fetchMutex;
    std::condition_variable g_fetchCv;
    std::deque<SFetch>      g_fetchQueue;
    std::thread             g_fetchWorker;
    bool                    g_fetchStop = false;

    std::string percentDecode(const std::string& in) {
        gchar* out = g_uri_unescape_string(in.c_str(), nullptr);
        std::string r = out ? out : in;
        g_free(out);
        return r;
    }

    // A file name for what came from `url`: its last path segment, decoded,
    // without query or fragment, made safe; `ext` appended when it has none
    // that matches.
    std::string nameFor(const std::string& url, const std::string& ext) {
        std::string path = url;
        if (const auto Q = path.find_first_of("?#"); Q != std::string::npos)
            path.resize(Q);
        std::string base = path.substr(path.find_last_of('/') + 1);
        base             = percentDecode(base);
        std::erase_if(base, [](char c) { return c == '/' || c == '\0' || c == '\\'; });
        while (!base.empty() && base.front() == '.')
            base.erase(base.begin());
        if (base.size() > 180)
            base.resize(180);
        if (base.empty())
            base = ext.empty() ? "download" : "image";
        if (!ext.empty()) {
            const auto DOT = base.find_last_of('.');
            const auto CUR = DOT == std::string::npos ? std::string{} : base.substr(DOT);
            const bool OK  = CUR == ext || (ext == ".jpg" && CUR == ".jpeg");
            if (!OK)
                base += ext;
        }
        return base;
    }

    void fetchMain() {
        ::setpriority(PRIO_PROCESS, 0, 5);
        std::unique_lock lock(g_fetchMutex);
        while (!g_fetchStop) {
            g_fetchCv.wait(lock, [] { return g_fetchStop || !g_fetchQueue.empty(); });
            if (g_fetchStop)
                break;
            auto job = std::move(g_fetchQueue.front());
            g_fetchQueue.pop_front();
            lock.unlock();

            std::error_code ec;
            const auto      STAGE = std::string(g_get_user_cache_dir()) + "/hyprdrive/drops/" + std::to_string(::getpid()) + "-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
            std::filesystem::create_directories(STAGE, ec);
            const auto FILE = STAGE + "/" + job.name;
            bool       ok   = false;
            if (!job.data.empty()) {
                std::ofstream out(FILE, std::ios::binary);
                out.write(job.data.data(), sc<std::streamsize>(job.data.size()));
                ok = out.good();
            } else if (!job.url.empty()) {
                std::vector<std::string> args{"curl", "-fsSL", "--max-time", "20", "-o", FILE, job.url};
                std::vector<char*>       argv;
                for (auto& a : args)
                    argv.push_back(a.data());
                argv.push_back(nullptr);
                gint status = -1;
                ok = g_spawn_sync(nullptr, argv.data(), nullptr, static_cast<GSpawnFlags>(G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL),
                                  nullptr, nullptr, nullptr, nullptr, &status, nullptr) &&
                    g_spawn_check_wait_status(status, nullptr) && std::filesystem::exists(FILE, ec);
            }
            if (ok)
                StackReader::moveOrCopyInto(job.dir, {FILE}); // its result bounces (or shakes) the stack
            // The staging folder of an earlier drop is empty by now; tidy them.
            for (const auto& E : std::filesystem::directory_iterator(std::string(g_get_user_cache_dir()) + "/hyprdrive/drops", ec))
                if (E.is_directory(ec) && std::filesystem::is_empty(E.path(), ec))
                    std::filesystem::remove(E.path(), ec);
            lock.lock();
        }
    }

    void saveWeb(const std::string& stackPath, SWeb w) {
        static const std::unordered_map<std::string, std::string> EXT{{"image/png", ".png"},  {"image/jpeg", ".jpg"}, {"image/webp", ".webp"},
                                                                       {"image/gif", ".gif"},  {"image/avif", ".avif"}, {"image/bmp", ".bmp"}};
        SFetch job;
        job.dir = stackPath;
        const std::string URL = w.urls.empty() ? std::string{} : w.urls.front();
        if (!w.data.empty()) {
            const auto IT = EXT.find(w.mime);
            job.name      = nameFor(URL, IT == EXT.end() ? std::string{} : IT->second);
            job.data      = std::move(w.data);
        } else if (!URL.empty()) {
            job.name = nameFor(URL, "");
            job.url  = URL;
        } else
            return;
        Stacks::expectJobFor(stackPath); // the result bounces this stack
        std::scoped_lock lock(g_fetchMutex);
        if (g_fetchStop)
            return;
        g_fetchQueue.push_back(std::move(job));
        if (!g_fetchWorker.joinable())
            g_fetchWorker = std::thread(fetchMain);
        g_fetchCv.notify_one();
    }
}

namespace ScrollOverview::DockDrop {

    // Hyprland's initiateDrag, for a source that is not a client's: the same
    // state, the same listeners, the same "unfocus the pointer" step. Its drag
    // functions are private, reached through the widened header above.
    bool startFileDrag(const std::vector<std::string>& paths, const std::string& icon) {
        if (paths.empty() || !PROTO::data || PROTO::data->dndActive())
            return false;

        g_out     = makeShared<CStackDragSource>(paths);
        g_outIcon = icon;
        g_out->markUsed();

        auto& D = PROTO::data->m_dnd;
        Pointer::Cursor::overrideController->setOverride("grabbing", Pointer::Cursor::CURSOR_OVERRIDE_DND);
        D.overriddenCursor = true;
        D.currentSource    = g_out;
        D.originSurface.reset();
        D.dndSurface.reset();

        D.mouseButton = Event::bus()->m_events.input.mouse.button.listen([](IPointer::SButtonEvent e, Event::SCallbackInfo&) {
            if (e.state == WL_POINTER_BUTTON_STATE_RELEASED && PROTO::data)
                PROTO::data->dropDrag();
        });
        D.mouseMove = Event::bus()->m_events.input.mouse.move.listen([](Vector2D pos, Event::SCallbackInfo&) {
            if (!PROTO::data)
                return;
            auto& DD = PROTO::data->m_dnd;
            if (!DD.focusedDevice || !g_pSeatManager->m_state.dndPointerFocus)
                return;
            const auto SURF = Desktop::View::CWLSurface::fromResource(g_pSeatManager->m_state.dndPointerFocus.lock());
            if (!SURF)
                return;
            const auto BOX = SURF->getSurfaceBoxGlobal();
            if (!BOX)
                return;
            DD.focusedDevice->sendMotion(Time::millis(Time::steadyNow()), pos - BOX->pos());
        });

        const auto LASTDNDFOCUS = g_pSeatManager->m_state.dndPointerFocus;
        g_pSeatManager->setPointerFocus(nullptr, {});
        g_pSeatManager->m_state.dndPointerFocus = LASTDNDFOCUS;

        PROTO::data->updateDrag();
        damageAll();
        return true;
    }

    // The drag icon: Hyprland draws a client's drag surface; ours has none, so
    // the file's icon follows the pointer from here.
    void renderGhost(PHLMONITOR monitor) {
        if (!monitor || !outActive())
            return;
        const auto  MOUSE = g_pInputManager->getMouseCoordsInternal();
        const float S     = monitor->m_scale;
        const auto  LOCAL = (MOUSE - monitor->m_position) * S;
        const float SZ    = 48.F * S;
        if (LOCAL.x < -SZ || LOCAL.y < -SZ || LOCAL.x > monitor->m_size.x * S + SZ || LOCAL.y > monitor->m_size.y * S + SZ)
            return;
        const auto TEX = OverviewRender::appIcon(g_outIcon);
        if (!TEX || !TEX->ok())
            return;
        CTexPassElement::SRenderData d;
        d.tex = TEX;
        d.box = CBox{LOCAL.x - SZ / 2.0, LOCAL.y - SZ / 2.0, SZ, SZ}.round();
        d.a   = 0.85F;
        g_pHyprRenderer->m_renderPass.add(makeUnique<CTexPassElement>(d));
    }

    bool draggingOut() {
        return outActive();
    }

    void onPointerMove(PHLMONITOR monitor, const Vector2D& local) {
        if (outActive())
            damageAll(); // the ghost follows the pointer
        else if (g_out) {
            g_out.reset(); // that drag is over: let the source go
            damageAll();
        }
        if (!PROTO::data || !PROTO::data->dndActive()) {
            if (!g_pending && (g.source.lock() || g.cell != ECell::None || g_placeholderIndex >= 0))
                reset();
            return;
        }
        const auto SRC = PROTO::data->m_dnd.currentSource.lock();
        if (!SRC)
            return;
        // Desktop only: inside the overview the overview owns drags.
        const bool DESKTOP = monitor && !scrollOverviewForMonitor(monitor);
        // Ask for the list as soon as the drag moves on the desktop, not when it
        // reaches the dock: a quick fling onto a stack must not arrive before
        // the source has answered.
        if (DESKTOP && g.source.lock() != SRC)
            requestList(SRC);
        if (!DESKTOP || !Dock::pillContains(monitor, local)) {
            if (g.cell != ECell::None) {
                g.cell  = ECell::None;
                g.stack = "";
                setPlaceholder(-1, "");
                damageAll();
            }
            return;
        }
        const bool FOLDER = g.eof && g.dirs == 1 && g.files == 0;
        const auto HIT    = Dock::dropCellAt(monitor, local, FOLDER);
        const bool CHANGED = HIT.cell != g.cell || HIT.stackPath != g.stack || HIT.gapIndex != g.gap;
        g.cell  = HIT.cell;
        g.stack = HIT.stackPath;
        g.gap   = HIT.gapIndex;

        // The Moses gap: only for ONE folder, only over a gap.
        const bool PIN = g.eof && DockDropModel::resolve(g.cell, g.files, g.dirs) == EAction::PinAt;
        setPlaceholder(PIN ? g.gap : -1, PIN ? g.paths.front() : "");
        if (CHANGED) {
            damageAll();
        }
    }

    bool onButton(PHLMONITOR monitor, const Vector2D& local, bool pressed) {
        if (pressed || !PROTO::data || !PROTO::data->dndActive() || !monitor || scrollOverviewForMonitor(monitor) || !Dock::pillContains(monitor, local))
            return false;

        // Aimed at the dock: the dock takes it. The drag is CANCELLED whatever
        // happens next, so the window under the dock never receives it and the
        // source never thinks its files were moved.
        if (!g.eof && (g_readFd >= 0 || g_imgFd >= 0)) {
            // The source has not finished answering: take the drop now, act
            // when the list lands.
            g_pending = SPending{Dock::dropCellAt(monitor, local, false), Dock::dropCellAt(monitor, local, true), std::chrono::steady_clock::now()};
            PROTO::data->abortDndIfPresent();
            g.cell  = ECell::None;
            g.stack = "";
            setPlaceholder(-1, "");
            damageAll();
            return true;
        }
        const auto HIT   = Dock::dropCellAt(monitor, local, g.eof && g.dirs == 1 && g.files == 0);
        const auto ACT   = g.eof ? DockDropModel::resolve(HIT.cell, g.files, g.dirs) : EAction::None;
        const auto PATHS = g.paths;
        SWeb       WEB{g.web, g.webUrls, g.imgMime, g.imgData};
        PROTO::data->abortDndIfPresent();
        reset();
        perform(HIT, ACT, PATHS, std::move(WEB));
        return true;
    }

    int placeholderIndex() {
        return g_placeholderIndex;
    }
    const std::string& placeholderPath() {
        return g_placeholderPath;
    }
    uint64_t placeholderGeneration() {
        return g_placeholderGen;
    }
    const std::string& hoveredStack() {
        static const std::string EMPTY;
        return g.cell == ECell::Stack ? g.stack : EMPTY;
    }
    bool hoveringTrash() {
        return g.cell == ECell::Trash;
    }

    void shutdown() {
        {
            std::scoped_lock lock(g_fetchMutex);
            g_fetchStop = true;
            g_fetchQueue.clear();
        }
        g_fetchCv.notify_all();
        if (g_fetchWorker.joinable())
            g_fetchWorker.join(); // at most one curl's --max-time
        // Our source's code is about to be unloaded: a drag of ours must not
        // outlive it (the drag state and the offers hold it only weakly).
        if (outActive())
            PROTO::data->abortDrag();
        g_out.reset();
        closeRead();
        g_pending.reset();
        g = SState{};
        g_placeholderIndex = -1;
        g_placeholderPath.clear();
    }
}
