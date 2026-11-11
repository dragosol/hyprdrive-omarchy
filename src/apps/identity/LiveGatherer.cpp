// The live Gatherer (LiveGatherer.hpp): its own xcb connection to XWayland
// and /proc, on the identity worker only.
//
// THE X CONNECTION is this worker's, never XGeometry's (that one belongs to
// the compositor thread): opened on the first X11 job once $DISPLAY exists,
// dropped and opened again when it errors or $DISPLAY changes (XWayland
// restarted), and not retried more than once a second while it fails. Every
// request is answered before the next is sent, so a reply never sits in the
// connection's queue across jobs. Hyprland is never blocked by it: XWayland
// answers the worker on its own.

#include "LiveGatherer.hpp"
#include "ProcFacts.hpp"
#include "Resolvers.hpp"

#include <xcb/res.h>
#include <xcb/xcb.h>

#include <chrono>
#include <cstdlib>
#include <cstring>

namespace Hyprdrive::AppIdentity {
    namespace {
        class CLiveGatherer final : public Gatherer {
          public:
            explicit CLiveGatherer(const Sources& src) : m_src(src) {}
            ~CLiveGatherer() override {
                drop();
            }

            void window(WindowFacts& w, const std::string& display) override {
                if (!w.x11 || !w.xid || display.empty())
                    return;
                xcb_connection_t* c = conn(display);
                if (!c)
                    return;
                // The real client pid: XWayland's SO_PEERCRED view, right
                // across PID namespaces where _NET_WM_PID names another process.
                if (m_hasRes) {
                    xcb_res_client_id_spec_t spec{.client = w.xid, .mask = XCB_RES_CLIENT_ID_MASK_LOCAL_CLIENT_PID};
                    if (auto* r = xcb_res_query_client_ids_reply(c, xcb_res_query_client_ids(c, 1, &spec), nullptr)) {
                        for (auto it = xcb_res_query_client_ids_ids_iterator(r); it.rem; xcb_res_client_id_value_next(&it))
                            if ((it.data->spec.mask & XCB_RES_CLIENT_ID_MASK_LOCAL_CLIENT_PID) && xcb_res_client_id_value_value_length(it.data) >= 1) {
                                const pid_t PID = static_cast<pid_t>(*xcb_res_client_id_value_value(it.data));
                                if (PID > 0)
                                    w.pid = PID;
                                break;
                            }
                        free(r);
                    }
                }
                // WM_CLASS: "instance\0class\0".
                if (auto* r = prop(c, w.xid, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 1024)) {
                    const auto* D = static_cast<const char*>(xcb_get_property_value(r));
                    const int   N = xcb_get_property_value_length(r);
                    if (D && N > 0)
                        w.instance = std::string(D, strnlen(D, static_cast<size_t>(N)));
                    free(r);
                }
                if (m_steamGame)
                    if (auto* r = prop(c, w.xid, m_steamGame, XCB_ATOM_CARDINAL, 1)) {
                        if (r->format == 32 && xcb_get_property_value_length(r) >= 4)
                            w.steamGameProp = std::to_string(*static_cast<const uint32_t*>(xcb_get_property_value(r)));
                        free(r);
                    }
                if (m_netWmIcon && w.netWmIcon.empty())
                    if (auto* r = prop(c, w.xid, m_netWmIcon, XCB_ATOM_CARDINAL, Caps::NET_WM_ICON / 4)) {
                        if (r->format == 32)
                            w.netWmIcon = storeNetWmIcon(static_cast<const uint32_t*>(xcb_get_property_value(r)), static_cast<size_t>(xcb_get_property_value_length(r)) / 4);
                        free(r);
                    }
            }

            ProcessFacts process(pid_t pid) override {
                return ProcFacts::read(pid);
            }

            void close() override {
                drop();
            }

          private:
            const Sources&    m_src;
            xcb_connection_t* m_conn = nullptr;
            std::string       m_display;
            bool              m_hasRes    = false;
            xcb_atom_t        m_netWmIcon = 0, m_steamGame = 0;
            std::chrono::steady_clock::time_point m_retryAt{};

            void drop() {
                if (m_conn)
                    xcb_disconnect(m_conn);
                m_conn = nullptr;
            }

            xcb_atom_t atom(xcb_connection_t* c, const char* name) {
                xcb_atom_t a = 0;
                if (auto* r = xcb_intern_atom_reply(c, xcb_intern_atom(c, 0, static_cast<uint16_t>(strlen(name)), name), nullptr)) {
                    a = r->atom;
                    free(r);
                }
                return a;
            }

            xcb_connection_t* conn(const std::string& display) {
                if (m_conn && (xcb_connection_has_error(m_conn) || display != m_display))
                    drop();
                if (m_conn)
                    return m_conn;
                const auto NOW = std::chrono::steady_clock::now();
                if (NOW < m_retryAt)
                    return nullptr;
                m_conn = xcb_connect(display.c_str(), nullptr);
                if (!m_conn || xcb_connection_has_error(m_conn)) {
                    drop();
                    m_retryAt = NOW + std::chrono::seconds(1);
                    return nullptr;
                }
                m_display = display;
                const auto* EXT = xcb_get_extension_data(m_conn, &xcb_res_id);
                m_hasRes        = EXT && EXT->present;
                if (m_hasRes) {
                    // QueryClientIds is XRes 1.2.
                    auto* v  = xcb_res_query_version_reply(m_conn, xcb_res_query_version(m_conn, 1, 2), nullptr);
                    m_hasRes = v && (v->server_major > 1 || (v->server_major == 1 && v->server_minor >= 2));
                    free(v);
                }
                m_netWmIcon = atom(m_conn, "_NET_WM_ICON");
                m_steamGame = atom(m_conn, "STEAM_GAME");
                return m_conn;
            }

            xcb_get_property_reply_t* prop(xcb_connection_t* c, uint32_t xid, xcb_atom_t a, xcb_atom_t type, uint32_t words) {
                auto* r = xcb_get_property_reply(c, xcb_get_property(c, 0, xid, a, type, 0, words), nullptr);
                if (r && r->type == XCB_ATOM_NONE) {
                    free(r);
                    return nullptr;
                }
                return r;
            }

            // The largest image of the property (at most 512 px, square or
            // not), premultiplied, kept as a PNG named after its own pixels
            // (the same icon is written once, whichever window shows it).
            Icon storeNetWmIcon(const uint32_t* d, size_t n) {
                size_t best = SIZE_MAX, bestW = 0, bestH = 0;
                for (size_t at = 0; at + 2 <= n;) {
                    const size_t W = d[at], H = d[at + 1];
                    if (!W || !H || W > 4096 || H > 4096 || W * H > n - at - 2)
                        break;
                    if (W <= 512 && H <= 512 && W * H > bestW * bestH)
                        best = at, bestW = W, bestH = H;
                    at += 2 + W * H;
                }
                if (best == SIZE_MAX || bestW < 8)
                    return {};
                Ico::Picked p;
                p.size    = static_cast<int>(bestW);
                p.image.w = static_cast<int>(bestW);
                p.image.h = static_cast<int>(bestH);
                p.image.argb.resize(bestW * bestH);
                std::string_view raw{reinterpret_cast<const char*>(d + best + 2), bestW * bestH * 4};
                for (size_t i = 0; i < bestW * bestH; ++i) {
                    const uint32_t PX = d[best + 2 + i], A = PX >> 24;
                    const auto     PM = [&](uint32_t c) { return (c * A + 127) / 255; };
                    p.image.argb[i]   = (A << 24) | (PM((PX >> 16) & 0xff) << 16) | (PM((PX >> 8) & 0xff) << 8) | PM(PX & 0xff);
                }
                const std::string NAME = "netwm-" + Resolvers::hash8(raw);
                std::string       path = m_src.storedIcon(NAME);
                if (path.empty())
                    path = m_src.storeIcon(NAME, p);
                if (path.empty())
                    return {};
                Icon i  = Icon::of(path);
                i.size  = static_cast<int>(bestW);
                return i;
            }
        };
    }

    std::shared_ptr<Gatherer> liveGatherer(const Sources& src) {
        return std::make_shared<CLiveGatherer>(src);
    }
}
