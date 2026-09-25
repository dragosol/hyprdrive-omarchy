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

namespace ScrollOverview::DockDrop {

    void onPointerMove(PHLMONITOR monitor, const Vector2D& local) {
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
        closeRead();
        g_pending.reset();
        g = SState{};
        g_placeholderIndex = -1;
        g_placeholderPath.clear();
    }
}
