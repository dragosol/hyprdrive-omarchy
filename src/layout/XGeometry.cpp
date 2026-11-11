// See XGeometry.hpp.
#include "XGeometry.hpp"
#include "FunctionHooks.hpp"
#include "ConfigureHold.hpp"
#include "XStateRequest.hpp"

#include <chrono>
#include <format>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

#include <xcb/xcb.h>

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/managers/EventManager.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/debug/log/Logger.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/managers/XWaylandManager.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/protocols/PointerConstraints.hpp>
#include <hyprland/src/desktop/view/WLSurface.hpp>
// XWM::activateSurface and its focused surface are private (the refocus).
#define private public
#include <hyprland/src/xwayland/XSurface.hpp>
#include <hyprland/src/xwayland/XWayland.hpp>
#include <hyprland/src/xwayland/XWM.hpp>
#undef private

namespace Hyprdrive::XGeometry {
    namespace {
        using Clock = std::chrono::steady_clock;

        // A client that keeps asking for its own box would otherwise be told
        // ours on every motion event.
        constexpr auto    RESEND = std::chrono::milliseconds(500);
        // The clip check costs a few X round trips; motion comes at 1 kHz.
        constexpr auto    CLIPCHECK = std::chrono::milliseconds(200);
        Clock::time_point g_lastSend;
        // `hyprdrive:debug xgeom`: how far each check got.
        struct {
            uint64_t calls = 0, hold = 0, window = 0, anim = 0, xsurface = 0, clipRate = 0, clipOutside = 0, clipConn = 0, clipPtr = 0, clipAgree = 0,
                     clipTree = 0, clipNone = 0, clipMoved = 0, refocus = 0, refocusBack = 0;
        } g_n;
        Clock::time_point g_lastClipCheck;

        // THE REFOCUS (2026-10-29, fedora, Proton-GE): newer Wine puts its clip
        // window back within milliseconds of being moved (watched: 3840x2160,
        // then 1582x889 again 4 ms later), so moving it no longer frees the
        // pointer. What does: X focus leaving the game for ANOTHER client's
        // window and Hyprland's XWM activating it again -- Wine drops the clip
        // on the way out, and the game clips to what it is now (the whole
        // screen) on the way back. Focus to the root is ignored by Wine, so the
        // decoy is a window of ours: input-only, 1x1 and off screen, never drawn
        // and never a Hyprland window. The game is not deactivated long enough
        // to notice (it kept drawing). Only after a move did not stick, at most
        // once every REFOCUS_EVERY.
        constexpr auto    MOVE_STUCK    = std::chrono::milliseconds(1500);
        constexpr auto    REFOCUS_EVERY = std::chrono::milliseconds(2000);
        constexpr int     REFOCUS_BACK  = 250; // ms with X focus on the decoy
        Clock::time_point g_lastMove, g_lastRefocus;
        xcb_window_t      g_decoy        = XCB_NONE;
        wl_event_source*  g_refocusTimer = nullptr;
        PHLWINDOWREF      g_refocusWindow;

        // Our own X client: Hyprland's WM connection is private, and an
        // override-redirect window takes a configure from any client.
        xcb_connection_t* g_conn = nullptr;

        bool              differs(double a, double b) {
            return std::abs(std::round(a) - std::round(b)) > 1.0;
        }

        xcb_connection_t* connection() {
            if (g_conn && !xcb_connection_has_error(g_conn))
                return g_conn;
            shutdown();
            // Hyprland exports XWayland's display once it is up.
            const char* DISPLAY = getenv("DISPLAY");
            if (!g_pXWayland || !DISPLAY || !*DISPLAY)
                return nullptr;
            g_conn = xcb_connect(DISPLAY, nullptr);
            if (xcb_connection_has_error(g_conn)) {
                shutdown();
                return nullptr;
            }
            return g_conn;
        }

        bool inside(int px, int py, int x, int y, int w, int h) {
            return px >= x && py >= y && px < x + w && py < y + h;
        }

        void checkBox(const PHLWINDOW& window, const SP<CXWaylandSurface>& xsurface) {
            const auto HAS  = xsurface->m_geometry;
            const auto POS  = window->realToReportPosition();
            const auto SIZE = window->realToReportSize();
            if (!differs(HAS.x, POS.x) && !differs(HAS.y, POS.y) && !differs(HAS.width, SIZE.x) && !differs(HAS.height, SIZE.y))
                return;

            const auto NOW = Clock::now();
            if (NOW - g_lastSend < RESEND)
                return;
            g_lastSend = NOW;

            Log::logger->log(Log::DEBUG, "[hyprdrive] X has {} {}x{} at {},{}, Hyprland draws it {}x{} at {},{}: telling X again", window->m_class, HAS.width,
                             HAS.height, HAS.x, HAS.y, SIZE.x, SIZE.y, POS.x, POS.y);
            window->sendWindowSize(true);
        }

        int refocusBack(void*) {
            if (const auto CONN = g_conn; CONN && g_decoy != XCB_NONE) {
                xcb_unmap_window(CONN, g_decoy);
                xcb_flush(CONN);
            }
            const auto W  = g_refocusWindow.lock();
            const auto XS = W ? W->m_xwaylandSurface.lock() : nullptr;
            g_refocusWindow.reset();
            // Only if it is still the window Hyprland has focused: anything else
            // that took the focus meanwhile already told X.
            if (!XS || !g_pXWayland || !g_pXWayland->m_wm || Desktop::focusState()->window() != W)
                return 0;
            ++g_n.refocusBack;
            g_pXWayland->m_wm->m_focusedSurface.reset(); // or activateSurface takes it as done
            g_pXWayland->m_wm->activateSurface(XS, true);
            return 0;
        }

        void refocus(const PHLWINDOW& window, const SP<CXWaylandSurface>& xsurface, xcb_connection_t* conn, xcb_window_t root) {
            const auto NOW = Clock::now();
            if (NOW - g_lastRefocus < REFOCUS_EVERY || !g_pCompositor || !g_pCompositor->m_wlEventLoop)
                return;
            g_lastRefocus = NOW;
            if (g_decoy == XCB_NONE) {
                g_decoy               = xcb_generate_id(conn);
                const uint32_t VALUES[] = {1}; // override-redirect: no WM ever manages it
                xcb_create_window(conn, XCB_COPY_FROM_PARENT, g_decoy, root, -16, -16, 1, 1, 0, XCB_WINDOW_CLASS_INPUT_ONLY, XCB_COPY_FROM_PARENT,
                                  XCB_CW_OVERRIDE_REDIRECT, VALUES);
            }
            xcb_map_window(conn, g_decoy);
            xcb_set_input_focus(conn, XCB_INPUT_FOCUS_POINTER_ROOT, g_decoy, XCB_CURRENT_TIME);
            xcb_flush(conn);
            // What Wine reads as "another window is active": the root's
            // _NET_ACTIVE_WINDOW and the game's _NET_WM_STATE losing FOCUSED --
            // bare X focus alone it ignored. As XWM::activateSurface does for a
            // real window.
            if (g_pXWayland && g_pXWayland->m_wm) {
                g_pXWayland->m_wm->setActiveWindow(g_decoy);
                g_pXWayland->m_wm->m_focusedSurface.reset();
                g_pXWayland->m_wm->sendState(xsurface);
                xcb_flush(g_pXWayland->m_wm->getConnection());
            }
            ++g_n.refocus;
            Log::logger->log(Log::DEBUG, "[hyprdrive] {}: its clip went back after a move, focus away and back", window->m_class);
            g_refocusWindow = window;
            if (!g_refocusTimer)
                g_refocusTimer = wl_event_loop_add_timer(g_pCompositor->m_wlEventLoop, refocusBack, nullptr);
            wl_event_source_timer_update(g_refocusTimer, REFOCUS_BACK);
        }

        void checkClip(const PHLWINDOW& window, const SP<CXWaylandSurface>& xsurface) {
            const auto NOW = Clock::now();
            if (NOW - g_lastClipCheck < CLIPCHECK) {
                ++g_n.clipRate;
                return;
            }
            g_lastClipCheck = NOW;

            const auto BOX    = xsurface->m_geometry;
            const auto CURSOR = g_pXWaylandManager->waylandToXWaylandCoords(g_pInputManager->getMouseCoordsInternal());
            const int  CX = std::lround(CURSOR.x), CY = std::lround(CURSOR.y);
            if (!inside(CX, CY, BOX.x, BOX.y, BOX.width, BOX.height)) {
                ++g_n.clipOutside;
                return;
            }

            const auto CONN = connection();
            if (!CONN) {
                ++g_n.clipConn;
                return;
            }
            const auto ROOT = xcb_setup_roots_iterator(xcb_get_setup(CONN)).data->root;

            auto*      ptr = xcb_query_pointer_reply(CONN, xcb_query_pointer(CONN, ROOT), nullptr);
            if (!ptr) {
                ++g_n.clipPtr;
                return;
            }
            const int PX = ptr->root_x, PY = ptr->root_y;
            free(ptr);
            // XWayland is a motion event behind at most; a pinned pointer is
            // far off.
            if (std::abs(PX - CX) <= 2 && std::abs(PY - CY) <= 2) {
                ++g_n.clipAgree;
                return;
            }

            auto* tree = xcb_query_tree_reply(CONN, xcb_query_tree(CONN, ROOT), nullptr);
            if (!tree) {
                ++g_n.clipTree;
                return;
            }
            const auto*                                  KIDS = xcb_query_tree_children(tree);
            const int                                    N    = xcb_query_tree_children_length(tree);
            std::vector<xcb_get_window_attributes_cookie_t> attrs;
            std::vector<xcb_get_geometry_cookie_t>          geoms;
            for (int i = 0; i < N; ++i) {
                attrs.push_back(xcb_get_window_attributes(CONN, KIDS[i]));
                geoms.push_back(xcb_get_geometry(CONN, KIDS[i]));
            }

            const bool STUCK = Clock::now() - g_lastMove < MOVE_STUCK;
            bool       moved = false;
            for (int i = 0; i < N; ++i) {
                auto* a = xcb_get_window_attributes_reply(CONN, attrs[i], nullptr);
                auto* g = xcb_get_geometry_reply(CONN, geoms[i], nullptr);
                // The grab window: input-only, unmanaged, shown, holding X's
                // pointer where Hyprland's cursor is not.
                if (a && g && a->_class == XCB_WINDOW_CLASS_INPUT_ONLY && a->override_redirect && a->map_state == XCB_MAP_STATE_VIEWABLE &&
                    inside(PX, PY, g->x, g->y, g->width, g->height) && !inside(CX, CY, g->x, g->y, g->width, g->height)) {
                    Log::logger->log(Log::DEBUG, "[hyprdrive] {} clips the pointer to {}x{} at {},{} (X pointer {},{}, cursor {},{}): clipping to {}x{} at {},{}",
                                     window->m_class, g->width, g->height, g->x, g->y, PX, PY, CX, CY, BOX.width, BOX.height, BOX.x, BOX.y);
                    if (STUCK) {
                        // Moved a moment ago and back again: Wine holds it there.
                        free(a);
                        free(g);
                        free(tree);
                        refocus(window, xsurface, CONN, ROOT);
                        return;
                    }
                    const uint32_t VALUES[] = {sc<uint32_t>(sc<int32_t>(BOX.x)), sc<uint32_t>(sc<int32_t>(BOX.y)), sc<uint32_t>(BOX.width), sc<uint32_t>(BOX.height)};
                    xcb_configure_window(CONN, KIDS[i], XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, VALUES);
                    moved = true;
                }
                free(a);
                free(g);
            }
            free(tree);
            ++(moved ? g_n.clipMoved : g_n.clipNone);
            if (moved) {
                g_lastMove = Clock::now();
                xcb_flush(CONN);
            }
        }
    }

    void check() {
        ++g_n.calls;
        if (ConfigureHold::held()) {
            ++g_n.hold;
            return;
        }

        const auto WINDOW = Desktop::focusState()->window();
        if (!WINDOW || !WINDOW->m_isX11 || !WINDOW->m_isMapped || !Fullscreen::controller()->isFullscreen(WINDOW)) {
            ++g_n.window;
            return;
        }
        if (WINDOW->positionAnimation()->isBeingAnimated() || WINDOW->sizeAnimation()->isBeingAnimated()) {
            ++g_n.anim;
            return;
        }

        const auto XSURFACE = WINDOW->m_xwaylandSurface.lock();
        if (!XSURFACE || !XSURFACE->m_mapped) {
            ++g_n.xsurface;
            return;
        }

        checkBox(WINDOW, XSURFACE);
        checkClip(WINDOW, XSURFACE);
    }

    namespace {
        CFunctionHook* g_clientMessageHook = nullptr;
        using origHandleClientMessage      = void (*)(CXWM*, xcb_client_message_event_t*);

        // The atoms, from our own X connection (atom ids are the server's):
        // Hyprland's HYPRATOMS is an inline header map the plugin may hold an
        // unfilled copy of.
        xcb_atom_t g_atomNetWmState = XCB_ATOM_NONE, g_atomWmChangeState = XCB_ATOM_NONE, g_atomAttention = XCB_ATOM_NONE, g_atomFullscreen = XCB_ATOM_NONE,
                   g_atomMaxVert = XCB_ATOM_NONE, g_atomMaxHorz = XCB_ATOM_NONE;
        bool stateAtoms() {
            if (g_atomNetWmState != XCB_ATOM_NONE)
                return true;
            const auto CONN = connection();
            if (!CONN)
                return false;
            const char* const NAMES[] = {"_NET_WM_STATE",           "WM_CHANGE_STATE",
                                         "_NET_WM_STATE_DEMANDS_ATTENTION", "_NET_WM_STATE_FULLSCREEN",
                                         "_NET_WM_STATE_MAXIMIZED_VERT",    "_NET_WM_STATE_MAXIMIZED_HORZ"};
            xcb_atom_t* const OUT[]   = {&g_atomNetWmState, &g_atomWmChangeState, &g_atomAttention, &g_atomFullscreen, &g_atomMaxVert, &g_atomMaxHorz};
            xcb_intern_atom_cookie_t cookies[6];
            for (size_t i = 0; i < 6; ++i)
                cookies[i] = xcb_intern_atom(CONN, 0, std::strlen(NAMES[i]), NAMES[i]);
            for (size_t i = 0; i < 6; ++i)
                if (auto* r = xcb_intern_atom_reply(CONN, cookies[i], nullptr)) {
                    *OUT[i] = r->atom;
                    free(r);
                }
            return g_atomNetWmState != XCB_ATOM_NONE;
        }

        // A GAME FLIPPING MAXIMIZED (XGeometry.hpp): the storm guard per X
        // window, and what the hook did, for `hyprdrive:debug xgeom`.
        std::unordered_map<xcb_window_t, XStateRequest::CStormGuard> g_storms;
        struct {
            uint64_t noop = 0, blocked = 0, storms = 0, flips = 0, told = 0;
        } g_xs;

        int64_t nowMs() {
            return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
        }

        PHLWINDOW windowOf(const SP<CXWaylandSurface>& xs) {
            if (!xs)
                return nullptr;
            for (const auto& W : Desktop::windowState()->windows())
                if (W && W->m_isX11 && W->m_xwaylandSurface.lock() == xs)
                    return W;
            return nullptr;
        }

        XStateRequest::eMode clientMode(const PHLWINDOW& w) {
            if (!w || !w->m_isMapped || !w->m_workspace || !Fullscreen::controller())
                return XStateRequest::eMode::NONE;
            const auto C = Fullscreen::controller()->getFullscreenModes(w).client;
            return (C & Fullscreen::FSMODE_FULLSCREEN) ? XStateRequest::eMode::FULLSCREEN :
                (C & Fullscreen::FSMODE_MAXIMIZED)     ? XStateRequest::eMode::MAXIMIZED :
                                                         XStateRequest::eMode::NONE;
        }

        // Tell X the truth: Hyprland 0.56 never sets CXWaylandSurface::m_maximized,
        // so _NET_WM_STATE never says MAXIMIZED and a client that reads it back
        // (Wine) asks again, forever. Kept in step with the window's client mode;
        // the state is sent again when it changed.
        void tellMaximized(const PHLWINDOW& w) {
            if (!w || !w->m_isX11 || !g_pXWayland || !g_pXWayland->m_wm)
                return;
            const auto XS = w->m_xwaylandSurface.lock();
            if (!XS || XS->m_overrideRedirect)
                return;
            const bool MAX = clientMode(w) == XStateRequest::eMode::MAXIMIZED;
            if (XS->m_maximized == MAX)
                return;
            XS->m_maximized = MAX;
            ++g_xs.told;
            g_pXWayland->m_wm->sendState(XS);
        }

        CHyprSignalListener g_fsTellHook;

        void hkHandleClientMessage(CXWM* wm, xcb_client_message_event_t* e) {
            if (!wm || !e || e->format != 32 || !stateAtoms() || !g_clientMessageHook) {
                if (g_clientMessageHook)
                    rc<origHandleClientMessage>(g_clientMessageHook->m_original)(wm, e);
                return;
            }
            const bool NETSTATE = e->type == g_atomNetWmState;
            SP<CXWaylandSurface> xs;
            if (NETSTATE || (g_atomWmChangeState != XCB_ATOM_NONE && e->type == g_atomWmChangeState)) {
                if (xs = wm->windowForXID(e->window); xs && xs->m_mapped) {
                    xs->m_state.requestsFullscreen.reset();
                    xs->m_state.requestsMaximize.reset();
                    xs->m_state.requestsMinimize.reset();
                }
            }

            // The maximize / fullscreen half of a _NET_WM_STATE message, judged
            // (XStateRequest.hpp) on a copy: Hyprland reads the atoms it names.
            xcb_client_message_event_t msg    = *e;
            PHLWINDOW                  window = NETSTATE && xs && xs->m_mapped ? windowOf(xs) : nullptr;
            auto                       before = XStateRequest::eMode::NONE;
            bool                       judged = false;
            if (window && window->m_isMapped) {
                auto       names = [&](xcb_atom_t a) { return a != XCB_ATOM_NONE && (msg.data.data32[1] == a || msg.data.data32[2] == a); };
                const bool MAXI  = names(g_atomMaxVert) || names(g_atomMaxHorz);
                const bool FULL  = names(g_atomFullscreen);
                if (MAXI || FULL) {
                    judged          = true;
                    before          = clientMode(window);
                    const auto  NOW = nowMs();
                    const auto& G   = g_storms[e->window];
                    const auto  V   = XStateRequest::decide({.action = msg.data.data32[0], .maximize = MAXI, .fullscreen = FULL}, before, G, NOW);
                    for (size_t i = 1; i <= 2; ++i) {
                        const xcb_atom_t A = msg.data.data32[i];
                        if ((V.stripMaximize && A != XCB_ATOM_NONE && (A == g_atomMaxVert || A == g_atomMaxHorz)) ||
                            (V.stripFullscreen && A != XCB_ATOM_NONE && A == g_atomFullscreen))
                            msg.data.data32[i] = XCB_ATOM_NONE;
                    }
                    if (V.blocked)
                        ++g_xs.blocked;
                    else if (V.stripMaximize)
                        ++g_xs.noop;
                }
            }

            rc<origHandleClientMessage>(g_clientMessageHook->m_original)(wm, &msg);

            if (judged) {
                // The window may have gone in Hyprland's handling of it.
                if (window && window->m_isMapped) {
                    const auto AFTER = clientMode(window);
                    if (AFTER != before) {
                        ++g_xs.flips;
                        const auto NOW = nowMs();
                        if (g_storms[e->window].noteFlip(NOW)) {
                            ++g_xs.storms;
                            Log::logger->log(Log::WARN, "[hyprdrive] {} flipped its own maximize/fullscreen more than {} times in {} ms: its requests are ignored for {} ms",
                                             window->m_class, XStateRequest::CStormGuard::FLIPS, XStateRequest::CStormGuard::WINDOW,
                                             XStateRequest::CStormGuard::BLOCK);
                        }
                    }
                    tellMaximized(window);
                }
                if (g_storms.size() > 32) {
                    const auto NOW = nowMs();
                    std::erase_if(g_storms, [&](const auto& kv) { return kv.second.idle(NOW); });
                }
            }

            // DEMANDS_ATTENTION, after Hyprland has had the message (it ignores it).
            if (!NETSTATE || g_atomAttention == XCB_ATOM_NONE)
                return;
            if (e->data.data32[1] != g_atomAttention && e->data.data32[2] != g_atomAttention)
                return;
            const auto XS = wm->windowForXID(e->window);
            if (!XS || !XS->m_mapped)
                return;
            for (const auto& W : Desktop::windowState()->windows()) {
                if (!W || !W->m_isMapped || W->m_xwaylandSurface.lock() != XS)
                    continue;
                const uint32_t ACTION = e->data.data32[0]; // 0 remove, 1 add, 2 toggle
                const bool     WANT   = ACTION == 1 || (ACTION == 2 && !W->m_isUrgent);
                if (!WANT) {
                    W->m_isUrgent = false;
                    break;
                }
                if (W->m_isUrgent || Desktop::focusState()->window() == W)
                    break; // already asking, or already in front of you
                W->m_isUrgent = true;
                g_pEventManager->postEvent(SHyprIPCEvent{.event = "urgent", .data = std::format("{:x}", rc<uintptr_t>(W.get()))});
                Event::bus()->m_events.window.urgent.emit(W);
                break;
            }
        }
    }

    void installStateRequestFix() {
        g_clientMessageHook = FunctionHooks::firstMatch("XGeometry", "_ZN4CXWM19handleClientMessageEP26xcb_client_message_event_t", rc<void*>(hkHandleClientMessage));
        if (!g_clientMessageHook)
            return; // not found: Hyprland's own handling stands
        if (!g_clientMessageHook->hook()) {
            g_clientMessageHook = nullptr;
            return;
        }
        // Every mode change a keybind, the dock, a pill or a gesture makes
        // reaches X too (a client message's own is told in the hook).
        g_fsTellHook = Event::bus()->m_events.window.fullscreen.listen([](PHLWINDOW w) { tellMaximized(w); });
    }

    uint32_t atom(const char* name) {
        static std::unordered_map<std::string, uint32_t> s_atoms; // the server's ids: stable for its life
        if (!name)
            return XCB_ATOM_NONE;
        if (const auto IT = s_atoms.find(name); IT != s_atoms.end())
            return IT->second;
        const auto CONN = connection();
        if (!CONN)
            return XCB_ATOM_NONE;
        uint32_t out = XCB_ATOM_NONE;
        if (auto* r = xcb_intern_atom_reply(CONN, xcb_intern_atom(CONN, 1, std::strlen(name), name), nullptr)) {
            out = r->atom;
            free(r);
        }
        if (out != XCB_ATOM_NONE)
            s_atoms.emplace(name, out);
        return out;
    }

    std::string status() {
        std::string s = std::format("calls={} hold={} window={} anim={} xsurface={} clip: rate={} outside={} conn={} ptr={} agree={} tree={} none={} moved={} refocus={}/{} | ",
                                    g_n.calls, g_n.hold, g_n.window, g_n.anim, g_n.xsurface, g_n.clipRate, g_n.clipOutside, g_n.clipConn, g_n.clipPtr,
                                    g_n.clipAgree, g_n.clipTree, g_n.clipNone, g_n.clipMoved, g_n.refocus, g_n.refocusBack);
        s += std::format("xstate: noop={} blocked={} storms={} flips={} told={} | ", g_xs.noop, g_xs.blocked, g_xs.storms, g_xs.flips, g_xs.told);
        const auto W = Desktop::focusState()->window();
        if (!W)
            return s + "no focused window";
        s += std::format("focused={} x11={} mapped={} fs={} hold={} posAnim={} sizeAnim={}", W->m_class, W->m_isX11, W->m_isMapped,
                         Fullscreen::controller()->isFullscreen(W), ConfigureHold::held(), W->positionAnimation()->isBeingAnimated(),
                         W->sizeAnimation()->isBeingAnimated());
        // The pointer constraints Hyprland holds for this window's surface: a
        // lock XWayland asked for (warp emulation) that is not active leaves X
        // following relative motion from wherever it was.
        for (const auto& c : g_pInputManager->m_constraints)
            if (const auto C = c.lock(); C && C->owner() && W->wlSurface() && C->owner() == W->wlSurface())
                s += std::format(" constraint={} active={} pointerFocusHere={}", C->isLocked() ? "lock" : "confine", C->isActive(),
                                 g_pSeatManager->m_state.pointerFocus == W->wlSurface()->resource());
        const auto XS = W->m_xwaylandSurface.lock();
        if (!XS)
            return s + " no xsurface";
        const auto B = XS->m_geometry;
        const auto C = g_pXWaylandManager->waylandToXWaylandCoords(g_pInputManager->getMouseCoordsInternal());
        s += std::format(" xmapped={} xbox={}x{}+{}+{} cursorX={},{} DISPLAY={} conn={}", XS->m_mapped, B.width, B.height, B.x, B.y, std::lround(C.x),
                         std::lround(C.y), getenv("DISPLAY") ? getenv("DISPLAY") : "(unset)", connection() ? "ok" : "none");
        if (const auto CONN = connection()) {
            const auto ROOT = xcb_setup_roots_iterator(xcb_get_setup(CONN)).data->root;
            if (auto* ptr = xcb_query_pointer_reply(CONN, xcb_query_pointer(CONN, ROOT), nullptr)) {
                s += std::format(" xpointer={},{}", ptr->root_x, ptr->root_y);
                free(ptr);
            }
        }
        return s;
    }

    void shutdown() {
        g_fsTellHook.reset();
        g_storms.clear();
        if (g_refocusTimer)
            wl_event_source_remove(g_refocusTimer);
        g_refocusTimer = nullptr;
        g_refocusWindow.reset();
        if (g_conn && g_decoy != XCB_NONE)
            xcb_destroy_window(g_conn, g_decoy);
        g_decoy = XCB_NONE;
        if (g_conn)
            xcb_disconnect(g_conn);
        g_conn = nullptr;
    }
}
