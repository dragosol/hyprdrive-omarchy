#include "TouchpadRaw.hpp"

#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/input.h>
#include <memory>
#include <string>
#include <sys/ioctl.h>
#include <unistd.h>
#include <vector>

#include <libinput.h>
#include <libudev.h>

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <aquamarine/input/Input.hpp>

namespace ScrollOverview::TouchpadRaw {

    namespace {
        constexpr int MAX_SLOTS = 16;

        struct SSlot {
            int id = -1;
            int x = 0, y = 0;
        };

        struct SDevice {
            std::string           node;
            int                   fd     = -1;
            wl_event_source*      source = nullptr;
            WP<IPointer>          pointer;
            std::array<SSlot, MAX_SLOTS> slots;
            int                   slot    = 0;
            int                   slotMax = 0;
            double                resX = 1, resY = 1; // units per mm
            bool                  button  = false;
            bool                  dropped = false; // SYN_DROPPED: skip to the next report, then resync
        };

        std::vector<std::unique_ptr<SDevice>> g_devices;
        FrameFn                               g_onFrame;
        LostFn                                g_onLost;

        void closeDevice(SDevice& d) {
            if (d.source)
                wl_event_source_remove(d.source);
            d.source = nullptr;
            if (d.fd >= 0)
                close(d.fd);
            d.fd = -1;
        }

        // The state as the kernel has it now -- on OUR fd, so these ioctls
        // flush nothing libinput needs.
        void resync(SDevice& d) {
            const int N = std::min(d.slotMax + 1, MAX_SLOTS);
            struct {
                uint32_t code;
                int32_t  values[MAX_SLOTS];
            } req{};
            const auto query = [&](uint32_t code, auto&& put) {
                req.code = code;
                if (ioctl(d.fd, EVIOCGMTSLOTS(sizeof(req)), &req) < 0)
                    return;
                for (int i = 0; i < N; ++i)
                    put(d.slots[i], req.values[i]);
            };
            query(ABS_MT_TRACKING_ID, [](SSlot& s, int v) { s.id = v; });
            query(ABS_MT_POSITION_X, [](SSlot& s, int v) { s.x = v; });
            query(ABS_MT_POSITION_Y, [](SSlot& s, int v) { s.y = v; });
            input_absinfo slot{};
            if (ioctl(d.fd, EVIOCGABS(ABS_MT_SLOT), &slot) >= 0)
                d.slot = slot.value;
            unsigned long keys[(KEY_MAX + 1) / (8 * sizeof(unsigned long)) + 1] = {};
            if (ioctl(d.fd, EVIOCGKEY(sizeof(keys)), keys) >= 0)
                d.button = (keys[BTN_LEFT / (8 * sizeof(unsigned long))] >> (BTN_LEFT % (8 * sizeof(unsigned long)))) & 1;
        }

        void emitFrame(const SDevice& d) {
            if (!g_onFrame)
                return;
            FingerSwipeModel::SFrame f;
            f.button = d.button;
            for (int i = 0; i < MAX_SLOTS && i <= d.slotMax; ++i) {
                const auto& S = d.slots[i];
                if (S.id >= 0)
                    f.touches.push_back({S.id, S.x / d.resX, S.y / d.resY});
            }
            g_onFrame(f);
        }

        int onReadable(int fd, uint32_t mask, void* data) {
            auto* d = sc<SDevice*>(data);
            if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
                closeDevice(*d);
                if (g_onLost)
                    g_onLost();
                return 0;
            }
            input_event ev[64];
            while (true) {
                const ssize_t N = read(fd, ev, sizeof(ev));
                if (N < 0) {
                    if (errno == EAGAIN || errno == EINTR)
                        break;
                    // ENODEV: unplugged / re-created (i2c-hid on resume).
                    closeDevice(*d);
                    if (g_onLost)
                        g_onLost();
                    return 0;
                }
                if (N == 0)
                    break;
                for (size_t i = 0; i < sc<size_t>(N) / sizeof(input_event); ++i) {
                    const auto& E = ev[i];
                    if (E.type == EV_SYN && E.code == SYN_DROPPED) {
                        d->dropped = true;
                        continue;
                    }
                    if (d->dropped) {
                        if (E.type == EV_SYN && E.code == SYN_REPORT) {
                            d->dropped = false;
                            resync(*d);
                            emitFrame(*d);
                        }
                        continue;
                    }
                    if (E.type == EV_ABS) {
                        if (E.code == ABS_MT_SLOT)
                            d->slot = E.value;
                        else if (d->slot >= 0 && d->slot < MAX_SLOTS) {
                            if (E.code == ABS_MT_TRACKING_ID)
                                d->slots[d->slot].id = E.value;
                            else if (E.code == ABS_MT_POSITION_X)
                                d->slots[d->slot].x = E.value;
                            else if (E.code == ABS_MT_POSITION_Y)
                                d->slots[d->slot].y = E.value;
                        }
                    } else if (E.type == EV_KEY && E.code == BTN_LEFT)
                        d->button = E.value != 0;
                    else if (E.type == EV_SYN && E.code == SYN_REPORT)
                        emitFrame(*d);
                }
            }
            return 0;
        }

        std::string devnodeOf(const SP<IPointer>& pointer) {
            const auto AQ = pointer ? pointer->aq() : nullptr;
            auto*      LI = AQ ? AQ->getLibinputHandle() : nullptr;
            if (!LI)
                return {};
            auto* UD = libinput_device_get_udev_device(LI);
            if (!UD)
                return {};
            const char* NODE = udev_device_get_devnode(UD);
            std::string out  = NODE ? NODE : "";
            udev_device_unref(UD);
            return out;
        }

        void openDevice(const SP<IPointer>& pointer, const std::string& node) {
            const int FD = open(node.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (FD < 0)
                return; // no ACL (inactive seat) or gone: libinput's behaviour stands
            auto d     = std::make_unique<SDevice>();
            d->node    = node;
            d->fd      = FD;
            d->pointer = pointer;

            input_absinfo info{};
            if (ioctl(FD, EVIOCGABS(ABS_MT_SLOT), &info) >= 0)
                d->slotMax = std::clamp(info.maximum, 0, MAX_SLOTS - 1);
            if (ioctl(FD, EVIOCGABS(ABS_MT_POSITION_X), &info) >= 0)
                d->resX = info.resolution > 0 ? info.resolution : std::max(1.0, (info.maximum - info.minimum) / 100.0);
            if (ioctl(FD, EVIOCGABS(ABS_MT_POSITION_Y), &info) >= 0)
                d->resY = info.resolution > 0 ? info.resolution : std::max(1.0, (info.maximum - info.minimum) / 70.0);
            resync(*d);

            d->source = wl_event_loop_add_fd(g_pCompositor->m_wlEventLoop, FD, WL_EVENT_READABLE, onReadable, d.get());
            if (!d->source) {
                close(FD);
                return;
            }
            g_devices.push_back(std::move(d));
        }
    }

    void init(FrameFn onFrame, LostFn onLost) {
        g_onFrame = std::move(onFrame);
        g_onLost  = std::move(onLost);
        rescan();
    }

    void rescan() {
        if (!g_pInputManager || !g_pCompositor)
            return;
        // Drop what died; a re-created node (resume) is opened again below.
        std::erase_if(g_devices, [](const auto& d) {
            if (d->fd >= 0 && d->pointer)
                return false;
            closeDevice(*d);
            return true;
        });
        for (const auto& P : g_pInputManager->m_pointers) {
            if (!P || !P->m_isTouchpad)
                continue;
            const auto NODE = devnodeOf(P);
            if (NODE.empty())
                continue;
            const bool OPEN = std::ranges::any_of(g_devices, [&](const auto& d) { return d->node == NODE; });
            if (!OPEN)
                openDevice(P, NODE);
        }
    }

    bool reads(const SP<IPointer>& pointer) {
        return pointer && std::ranges::any_of(g_devices, [&](const auto& d) { return d->fd >= 0 && d->pointer.lock() == pointer; });
    }

    bool active() {
        return std::ranges::any_of(g_devices, [](const auto& d) { return d->fd >= 0; });
    }

    void shutdown() {
        for (auto& d : g_devices)
            closeDevice(*d);
        g_devices.clear();
        g_onFrame = nullptr;
        g_onLost  = nullptr;
    }
}
