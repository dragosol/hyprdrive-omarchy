#include "PluginParts.hpp"
using namespace PluginDetail;

typedef void (*origRenderWorkspace)(void*, PHLMONITOR, PHLWORKSPACE, const Time::steady_tp&, const CBox&);
typedef void (*origAddDamageA)(void*, const CBox&);
typedef void (*origAddDamageB)(void*, const pixman_region32_t*);
typedef void (*origDamageSurface)(void*, SP<CWLSurfaceResource>, double, double, double);
typedef void (*origScheduleFrame)(void*, Aquamarine::IOutput::scheduleFrameReason);
typedef void (*origSendFrameEventsToWorkspace)(void*, PHLMONITOR, PHLWORKSPACE, const Time::steady_tp&);
typedef void (*origSurfaceFrame)(void*, const Time::steady_tp&);
typedef void (*origOnMouseMoved)(void*, IPointer::SMotionEvent);

bool ensureScrollOverviewHooks() {
    if (g_scrollOverviewHooksActive)
        return true;

    bool success = g_pScrollRenderWorkspaceHook->hook();
    success      = success && g_pScrollScheduleFrameHook->hook();
    success      = success && g_pScrollDamageSurfaceHook->hook();
    success      = success && g_pScrollSendFrameEventsHook->hook();
    success      = success && g_pScrollSurfaceFrameHook->hook();
    success      = success && g_pScrollAddDamageHookA->hook();
    success      = success && g_pScrollAddDamageHookB->hook();

    if (!success) {
        disableScrollOverviewHooks();
        failNotif("Failed enabling overview hooks (is other overview plugin enabled?)");
        return false;
    }

    g_scrollOverviewHooksActive = true;
    return true;
}

void disableScrollOverviewHooks() {
    if (g_pScrollAddDamageHookB)
        g_pScrollAddDamageHookB->unhook();
    if (g_pScrollAddDamageHookA)
        g_pScrollAddDamageHookA->unhook();
    if (g_pScrollSurfaceFrameHook)
        g_pScrollSurfaceFrameHook->unhook();
    if (g_pScrollSendFrameEventsHook)
        g_pScrollSendFrameEventsHook->unhook();
    if (g_pScrollDamageSurfaceHook)
        g_pScrollDamageSurfaceHook->unhook();
    if (g_pScrollScheduleFrameHook)
        g_pScrollScheduleFrameHook->unhook();
    if (g_pScrollRenderWorkspaceHook)
        g_pScrollRenderWorkspaceHook->unhook();

    g_scrollOverviewHooksActive = false;
}

namespace PluginDetail {
void setDockCursor(bool on) {
    if (on == g_dockCursor || !Pointer::Cursor::overrideController)
        return;
    g_dockCursor = on;
    if (on)
        Pointer::Cursor::overrideController->setOverride("left_ptr", Pointer::Cursor::CURSOR_OVERRIDE_UNKNOWN);
    else
        Pointer::Cursor::overrideController->unsetOverride(Pointer::Cursor::CURSOR_OVERRIDE_UNKNOWN);
}

void hkSendWindowSize(void* thisptr, bool force) {
    if (ScrollOverview::ConfigureHold::intercept(sc<Desktop::View::CWindow*>(thisptr), force))
        return;
    rc<origSendWindowSize>(g_pSendWindowSizeHook->m_original)(thisptr, force);
}

bool           hkShouldRenderWindow(void* thisptr, PHLWINDOW window, PHLMONITOR monitor) {
    // A window travelling to or from its fullscreen desktop is drawn all the
    // way -- before the canvas's rule that hides windows off its view while
    // their desktop slides (a fullscreen app on a canvas usually starts there).
    if (ScrollOverview::FullscreenDesktop::travelling(window, monitor))
        return true;
    if (ScrollOverview::Mosaic::hideDuringSlide(window, monitor))
        return false;
    return rc<bool (*)(void*, PHLWINDOW, PHLMONITOR)>(g_pShouldRenderWindowHook->m_original)(thisptr, window, monitor);
}

void           hkOnAck(void* thisptr, uint32_t serial) {
    ScrollOverview::SizeAck::onAck(sc<Desktop::View::CWindow*>(thisptr), serial);
}

void hkTargetPlace(void* thisptr, const Layout::STargetBox& box, uint8_t flags) {
    ScrollOverview::DockRoom::place(thisptr, box, flags, [](void* t, const Layout::STargetBox& b, uint8_t f) {
        ScrollOverview::OwnSizeTile::place(t, b, f, [](void* t2, const Layout::STargetBox& b2, uint8_t f2) {
            rc<void (*)(void*, const Layout::STargetBox&, uint8_t)>(g_pTargetPlaceHook->m_original)(t2, b2, f2);
        });
    });
}

void hkOnMouseMoved(void* thisptr, IPointer::SMotionEvent e) {
    // A swipe the plugin owns (a continued one, or one under a held drag):
    // libinput's leftover touchpad motion would move the pointer -- and fling
    // the held drag with the sum of every finger.
    if (ScrollOverview::FingerSwipe::swallowMotion(e))
        return;
    const Vector2D BEFORE = g_pInputManager->getMouseCoordsInternal();
    rc<origOnMouseMoved>(g_pScrollMouseMovedHook->m_original)(thisptr, e);
    ScrollOverview::Mosaic::onPointerDelta(e.delta, g_pInputManager->getMouseCoordsInternal() - BEFORE);
    ScrollOverview::Dock::feedPointerMotion(e.delta);
    ScrollOverview::XGeometry::check();
    ScrollOverview::FullscreenGuard::resumePointerLock();
}

void hkScheduleFrame(void* thisptr, Aquamarine::IOutput::scheduleFrameReason reason) {
    const auto OVERVIEW = scrollOverviewForMonitor(sc<Monitor::CMonitor*>(thisptr)->m_self.lock());
    if (OVERVIEW) {
        using enum Aquamarine::IOutput::scheduleFrameReason;

        const bool THROTTLEDREASON =
            reason == AQ_SCHEDULE_UNKNOWN || reason == AQ_SCHEDULE_CLIENT_UNKNOWN || reason == AQ_SCHEDULE_NEEDS_FRAME || reason == AQ_SCHEDULE_RENDER_MONITOR ||
            reason == AQ_SCHEDULE_DAMAGE;

        if (THROTTLEDREASON && !OVERVIEW->blockDamageReporting && !OVERVIEW->shouldAllowRealtimePreviewSchedule())
            return;
    }

    rc<origScheduleFrame>(g_pScrollScheduleFrameHook->m_original)(thisptr, reason);
}

//
void hkRenderWorkspace(void* thisptr, PHLMONITOR pMonitor, PHLWORKSPACE pWorkspace, const Time::steady_tp& now, const CBox& geometry) {
    const auto OVERVIEW = scrollOverviewForMonitor(pMonitor);
    if (!OVERVIEW || renderingOverview)
        rc<origRenderWorkspace>(g_pScrollRenderWorkspaceHook->m_original)(thisptr, pMonitor, pWorkspace, now, geometry);
    else {
        const bool PREVRENDERINGOVERVIEW = renderingOverview;
        const auto PREVRENDERINGMONITOR  = renderingOverviewMonitor;
        renderingOverview                = true;
        renderingOverviewMonitor         = pMonitor;
        OVERVIEW->render();
        renderingOverviewMonitor = PREVRENDERINGMONITOR;
        renderingOverview = PREVRENDERINGOVERVIEW;
    }
}

void hkDamageSurface(void* thisptr, SP<CWLSurfaceResource> surface, double x, double y, double scale) {
    // Y17 (GPU, 2026-09-14). The overview's U2 prefilter keeps a mipmapped copy
    // of each surface's texture and no longer rebuilds it per draw; this is the
    // "that copy is stale" signal. Unconditional, and BEFORE the gate below:
    // whether a given overview wants the damage reported is a different
    // question from whether the client's pixels changed.
    OverviewWindow::notifySurfaceCommit(surface);

    const bool HANDLED = scrollOverviews().empty() ||
        std::ranges::any_of(scrollOverviews(), [](const auto& overview) { return overview && overview->blockDamageReporting; }) ||
        std::ranges::all_of(scrollOverviews(), [&surface](const auto& overview) { return !overview || overview->shouldHandleSurfaceDamage(surface); });
    if (HANDLED) {
        const bool PREVDAMAGEFROMSURFACE = damageFromSurface;
        damageFromSurface                = !scrollOverviews().empty();
        rc<origDamageSurface>(g_pScrollDamageSurfaceHook->m_original)(thisptr, surface, x, y, scale);
        damageFromSurface = PREVDAMAGEFROMSURFACE;
    }
}

void hkSendFrameEventsToWorkspace(void* thisptr, PHLMONITOR monitor, PHLWORKSPACE workspace, const Time::steady_tp& now) {
    if (scrollOverviewForMonitor(monitor))
        return;

    rc<origSendFrameEventsToWorkspace>(g_pScrollSendFrameEventsHook->m_original)(thisptr, monitor, workspace, now);
}

void hkSurfaceFrame(void* thisptr, const Time::steady_tp& now) {
    const auto SURFACE = sc<CWLSurfaceResource*>(thisptr)->m_self.lock();

    if (std::ranges::any_of(scrollOverviews(), [&SURFACE, &now](const auto& overview) { return overview && !overview->shouldAllowSurfaceFrame(SURFACE, now); }))
        return;

    rc<origSurfaceFrame>(g_pScrollSurfaceFrameHook->m_original)(thisptr, now);
}
} // namespace PluginDetail

// CMonitor::addDamage has two overloads (a CBox, a pixman region); we hook both
// and the suppress decision is identical for either payload. It is the ONE thing
// the two hooks share, so it lives here and the hooks are one line each. Only the
// overview's own render is ever suppressed; everything else passes straight to the
// original -- the old code's `!OVERVIEW || blockDamageReporting || damageFromSurface`
// branch and the fall-through both called the original with the same args, so it
// was dead weight and is gone.
static bool suppressAddDamage(void* thisptr) {
    const auto PMONITOR = sc<Monitor::CMonitor*>(thisptr);
    const auto OVERVIEW = scrollOverviewForMonitor(PMONITOR->m_self.lock());
    return OVERVIEW && renderingOverviewMonitor.lock() == PMONITOR->m_self.lock() && !damageFromSurface && OVERVIEW->shouldSuppressRenderDamage();
}

namespace PluginDetail {
void hkAddDamageA(void* thisptr, const CBox& box) {
    if (suppressAddDamage(thisptr))
        return;
    rc<origAddDamageA>(g_pScrollAddDamageHookA->m_original)(thisptr, box);
}

void hkAddDamageB(void* thisptr, const pixman_region32_t* rg) {
    if (suppressAddDamage(thisptr))
        return;
    rc<origAddDamageB>(g_pScrollAddDamageHookB->m_original)(thisptr, rg);
}

// The desktop dock's pointer hooks all begin the same way: take the cursor, find
// the monitor it is over, SKIP any monitor whose overview is up (that one has its
// own input path -- letting the desktop hook fire there too is a double-handle),
// and hand back the pill-local point. Five hooks pasted this preamble; the one
// that forgot the overview-skip would be the drift this phase exists to kill. Now
// there is one copy. Returns nullopt when the cursor is over no plain-desktop
// monitor (an empty result means "not my click", same as the old loop falling
// through without a match).
std::optional<std::pair<PHLMONITOR, Vector2D>> monitorUnderCursor() {
    const auto POINT = g_pInputManager->getMouseCoordsInternal();
    for (const auto& MONITOR : State::monitorState()->monitors()) {
        if (!MONITOR || scrollOverviewForMonitor(MONITOR) || !MONITOR->logicalBox().containsPoint(POINT))
            continue;
        return std::make_pair(MONITOR, (POINT - MONITOR->m_position) * MONITOR->m_scale);
    }
    return std::nullopt;
}

void failNotif(const std::string& reason) {
    HyprlandAPI::addNotification(SCROLLOVERVIEW_HANDLE, "[hyprdrive] Failure in initialization: " + reason, CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
}

// Helper function to find a function by name and ensure it contains one of the requested substrings in its demangled name (to disambiguate overloads).
void* findFnOrThrow(const std::string& name, std::initializer_list<std::string_view> demangledNeedles) {
    auto fns = HyprlandAPI::findFunctionsByName(SCROLLOVERVIEW_HANDLE, name);
    if (fns.empty()) {
        failNotif(std::format("no fns for hook {}", name));
        throw std::runtime_error(std::format("[hyprdrive] No fns for hook {}", name));
    }

    if (demangledNeedles.size() == 0 || (demangledNeedles.size() == 1 && demangledNeedles.begin()->empty()))
        return fns[0].address;

    std::vector<SFunctionMatch> matches;
    matches.reserve(fns.size());
    for (const auto& fn : fns) {
        for (const auto& needle : demangledNeedles) {
            if (needle.empty() || fn.demangled.find(needle) != std::string::npos) {
                matches.push_back(fn);
                break;
            }
        }
    }

    if (matches.empty()) {
        failNotif(std::format("no matching overload for hook {}", name));
        throw std::runtime_error(std::format("[hyprdrive] No matching overload for hook {}", name));
    }

    if (matches.size() > 1) {
        failNotif(std::format("ambiguous overload for hook {} ({} matches)", name, matches.size()));
        throw std::runtime_error(std::format("[hyprdrive] Ambiguous overload for hook {}", name));
    }

    return matches[0].address;
}
} // namespace PluginDetail
