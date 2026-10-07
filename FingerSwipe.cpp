#include "FingerSwipe.hpp"

#include "Config.hpp"
#include "FingerSwipeModel.hpp"
#include "GestureDrive.hpp"
#include "TouchpadRaw.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprland/src/protocols/PointerGestures.hpp>

namespace ScrollOverview::FingerSwipe {

    namespace {
        constexpr const char* KEY_KEEP = "plugin:hyprdrive:gestures:keep_swipe";
        constexpr const char* KEY_DRAG = "plugin:hyprdrive:gestures:swipe_while_dragging";
        // A swipe of ours with no finger frame for this long is finished: the
        // raw stream stalled (a VT switch, a dropped device) and nothing else
        // would ever end it.
        constexpr int WATCHDOG_MS = 1500;

        FingerSwipeModel::CModel g_model;
        CHyprSignalListener      g_swipeBegin, g_swipeUpdate, g_swipeEnd;
        CHyprSignalListener      g_pinchBegin, g_pinchUpdate, g_pinchEnd;
        CHyprSignalListener      g_axis;
        wl_event_source*         g_watchdog = nullptr;
        bool                     g_touching = false; // the last frame had a finger down

        uint32_t nowMs() {
            return sc<uint32_t>(Time::millis(Time::steadyNow()));
        }

        bool ourMode() {
            using enum FingerSwipeModel::EMode;
            const auto M = g_model.mode();
            return M == Continue || M == DragArm || M == DragSwipe;
        }

        void syncConfig() {
            g_model.cfg.keepSwipe = Config::getValue<bool>(KEY_KEEP);
            g_model.cfg.dragSwipe = Config::getValue<bool>(KEY_DRAG);
        }

        void armWatchdog() {
            if (!g_watchdog)
                return;
            wl_event_source_timer_update(g_watchdog, ourMode() ? WATCHDOG_MS : 0);
        }

        void apply(const FingerSwipeModel::SOut& out) {
            const bool DRAG = g_model.mode() == FingerSwipeModel::EMode::DragSwipe || (out.begin && !out.end);
            if (out.begin)
                GestureDrive::begin(nowMs(), out.fingers);
            for (const auto& U : out.updates)
                GestureDrive::update(nowMs(), out.fingers, {U.x, U.y}, DRAG);
            if (out.end)
                GestureDrive::end(nowMs(), false);
            armWatchdog();
        }

        void onFrame(const FingerSwipeModel::SFrame& f) {
            g_touching = !f.touches.empty();
            syncConfig();
            apply(g_model.onFrame(f));
        }

        void onLost() {
            apply(g_model.abort());
        }

        // A finger resting still sends no frames, so silence alone proves
        // nothing: only silence with nothing down (a lost lift) ends it.
        int onWatchdog(void*) {
            if (!ourMode())
                return 0;
            if (g_touching && TouchpadRaw::active())
                armWatchdog();
            else
                apply(g_model.abort());
            return 0;
        }
    }

    void init() {
        syncConfig();
        TouchpadRaw::init(onFrame, onLost);
        if (!g_watchdog)
            g_watchdog = wl_event_loop_add_timer(g_pCompositor->m_wlEventLoop, onWatchdog, nullptr);

        // Listeners only CANCEL. A swipe passing through untouched is handed on
        // to Hyprland by its own onSwipe*, exactly as without the plugin.
        g_swipeBegin = Event::bus()->m_events.gesture.swipe.begin.listen([](IPointer::SSwipeBeginEvent e, Event::SCallbackInfo& info) {
            syncConfig();
            // A touchpad that appeared since (hotplug, resume re-creating the
            // node) is opened at the first swipe it makes.
            if (!TouchpadRaw::active())
                TouchpadRaw::rescan();
            if (g_model.onSwipeBegin(e.fingers).cancel)
                info.cancelled = true;
        });
        g_swipeUpdate = Event::bus()->m_events.gesture.swipe.update.listen([](IPointer::SSwipeUpdateEvent e, Event::SCallbackInfo& info) {
            if (g_model.onSwipeUpdate(e.fingers, {e.delta.x, e.delta.y}).cancel)
                info.cancelled = true;
        });
        g_swipeEnd = Event::bus()->m_events.gesture.swipe.end.listen([](IPointer::SSwipeEndEvent e, Event::SCallbackInfo& info) {
            const auto OUT = g_model.onSwipeEnd(e.cancelled);
            if (OUT.cancel)
                info.cancelled = true;
            // Hyprland's gesture lives on (it is ours now); the client that
            // was following libinput's swipe hears it end, as cancelled.
            if (OUT.endClients && PROTO::pointerGestures)
                PROTO::pointerGestures->swipeEnd(e.timeMs, true);
            armWatchdog();
        });
        g_pinchBegin = Event::bus()->m_events.gesture.pinch.begin.listen([](IPointer::SPinchBeginEvent, Event::SCallbackInfo& info) {
            if (g_model.onPinch(false).cancel)
                info.cancelled = true;
        });
        g_pinchUpdate = Event::bus()->m_events.gesture.pinch.update.listen([](IPointer::SPinchUpdateEvent, Event::SCallbackInfo& info) {
            if (g_model.onPinch(false).cancel)
                info.cancelled = true;
        });
        g_pinchEnd = Event::bus()->m_events.gesture.pinch.end.listen([](IPointer::SPinchEndEvent, Event::SCallbackInfo& info) {
            if (g_model.onPinch(true).cancel)
                info.cancelled = true;
        });
        // A 3->2 lift becomes libinput's two-finger scroll.
        g_axis = Event::bus()->m_events.input.mouse.axis.listen([](IPointer::SAxisEvent e, Event::SCallbackInfo& info) {
            if (e.source == WL_POINTER_AXIS_SOURCE_FINGER && g_model.swallowPointer())
                info.cancelled = true;
        });
    }

    void rescan() {
        TouchpadRaw::rescan();
    }

    bool swallowMotion(const IPointer::SMotionEvent& e) {
        return g_model.swallowPointer() && e.device && e.device->m_isTouchpad;
    }

    void shutdown() {
        // A swipe of ours mid-flight: finish it while its code is still here.
        if (ourMode())
            apply(g_model.abort());
        g_swipeBegin.reset();
        g_swipeUpdate.reset();
        g_swipeEnd.reset();
        g_pinchBegin.reset();
        g_pinchUpdate.reset();
        g_pinchEnd.reset();
        g_axis.reset();
        if (g_watchdog)
            wl_event_source_remove(g_watchdog);
        g_watchdog = nullptr;
        TouchpadRaw::shutdown();
    }
}
