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
        bool                     eof = false;
        std::vector<std::string> paths;
        size_t                   files = 0, dirs = 0;

        ECell       cell = ECell::None;
        std::string stack;     // the hovered stack's path
        int         gap  = -1; // the stack index the gap is at
    };
    SState           g;
    wl_event_source* g_readSource = nullptr;
    int              g_readFd     = -1;
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

    void closeRead() {
        if (g_readSource)
            wl_event_source_remove(g_readSource);
        g_readSource = nullptr;
        if (g_readFd >= 0)
            ::close(g_readFd);
        g_readFd = -1;
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
        g.paths = DockDropModel::parseUriList(g.data);
        g.files = g.dirs = 0;
        for (const auto& P : g.paths) {
            struct stat st{};
            if (::stat(P.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
                ++g.dirs;
            else
                ++g.files;
        }
    }

    void perform(const Dock::SDropHit& hit, EAction act, const std::vector<std::string>& paths) {
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
        perform(HIT, DockDropModel::resolve(HIT.cell, g.files, g.dirs), g.paths);
        g = SState{};
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
                g.eof = true;
                closeRead();
                finishList();
                finishPending();
                damageAll();
            }
            break;
        }
        return 0;
    }

    void requestList(const SP<IDataSource>& src) {
        g_pending.reset();
        closeRead();
        g.source = src;
        g.data.clear();
        g.eof = false;
        g.paths.clear();
        g.files = g.dirs = 0;
        bool hasUris = false;
        for (const auto& M : src->mimes())
            hasUris |= M == "text/uri-list";
        if (!hasUris)
            return; // not a drag of files: the dock ignores it
        int fds[2];
        if (pipe2(fds, O_CLOEXEC | O_NONBLOCK) != 0)
            return;
        g_readFd     = fds[0];
        g_readSource = wl_event_loop_add_fd(g_pCompositor->m_wlEventLoop, fds[0], WL_EVENT_READABLE, onReadable, nullptr);
        src->send("text/uri-list", Hyprutils::OS::CFileDescriptor{fds[1]}); // the source owns the write end now
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
        if (!g.eof && g_readFd >= 0) {
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
        PROTO::data->abortDndIfPresent();
        reset();
        perform(HIT, ACT, PATHS);
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
