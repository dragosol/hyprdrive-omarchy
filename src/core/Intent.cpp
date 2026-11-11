#include "Intent.hpp"
#include "FunctionHooks.hpp"
#include "Launch.hpp"
#include "Mosaic.hpp"
#include "Config.hpp"
#include "DebugPaths.hpp"

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/desktop/view/LayerSurface.hpp>
#include <hyprland/src/desktop/view/WLSurface.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/pointer/PointerController.hpp>
#include <hyprland/src/protocols/LayerShell.hpp>
#include <hyprland/src/protocols/core/Compositor.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/Compositor.hpp>

#include <xkbcommon/xkbcommon-keysyms.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <fstream>
#include <sstream>
#include <unistd.h>
#include <optional>
#include <string>
#include <vector>

namespace Hyprdrive::Intent {
    namespace {
        using clock = std::chrono::steady_clock;

        constexpr double GRANT_S   = 15.0; // a launch's grant, until a window takes it
        constexpr double APP_S     = 60.0; // ...then that app's next windows (a long loading screen)
        constexpr double SAMEAPP_S = 3.0;  // a click / Enter in the focused app, then its new window
        constexpr double INPUT_S   = 1.5;  // a click / Enter anywhere, then any window
        constexpr double IDLE_S    = 0.35; // the pointer counts as resting after this
        constexpr double RIDE_S    = 1.5;  // longest a pointer ride lasts

        struct SGrant {
            clock::time_point   at;
            std::string         cls;   // the app that took it ("" = not taken yet)
            std::vector<pid_t>  procs; // its window's process and parents (sameApp)
            clock::time_point   taken{};
        };
        std::vector<SGrant>      g_grants;
        clock::time_point        g_lastPress{};   // a click, or Enter, anywhere
        clock::time_point        g_lastUserMove{}; // the hand moved the pointer
        bool                     g_warping = false;
        std::vector<PHLWINDOWREF> g_quiet;

        struct SRide {
            PHLWINDOWREF      win;
            Vector2D          rel; // the pointer, from the window's top left
            clock::time_point from;
        };
        std::optional<SRide> g_ride;

        CHyprSignalListener g_openEarly, g_key, g_button, g_move, g_close;
        CFunctionHook*      g_spawnHook = nullptr;

        double since(clock::time_point t) {
            return t.time_since_epoch().count() == 0 ? 1e9 : std::chrono::duration<double>(clock::now() - t).count();
        }

        bool on() {
            return Config::getValue<bool>("plugin:hyprdrive:mosaic:quiet_new_windows");
        }

        void trace(const std::string& s) {
            if (!DebugPaths::flagOn("hyprdrive-mosaic.on"))
                return;
            if (FILE* const F = DebugPaths::openLog("hyprdrive-mosaic.log")) {
                std::fprintf(F, "%lld intent %s\n", (long long)std::chrono::duration_cast<std::chrono::milliseconds>(clock::now().time_since_epoch()).count(), s.c_str());
                std::fclose(F);
            }
        }

        // The layer surface (a launcher, the bar) the keyboard or the pointer is on.
        SP<Desktop::View::CLayerSurface> layerOf(const SP<CWLSurfaceResource>& surf) {
            const auto HLS  = surf ? Desktop::View::CWLSurface::fromResource(surf) : SP<Desktop::View::CWLSurface>{};
            const auto VIEW = HLS ? HLS->view() : SP<Desktop::View::IView>{};
            if (!VIEW || VIEW->type() != Desktop::View::VIEW_TYPE_LAYER_SURFACE)
                return nullptr;
            return Desktop::View::CLayerSurface::fromView(VIEW);
        }

        // CExecutor::spawnRawProc: every exec Hyprland makes.
        typedef std::optional<uint64_t> (*origSpawn)(void*, const std::string&, PHLWORKSPACE, const std::string&);
        std::optional<uint64_t> hkSpawn(void* self, const std::string& args, PHLWORKSPACE ws, const std::string& token) {
            granted("exec");
            return rc<origSpawn>(g_spawnHook->m_original)(self, args, std::move(ws), token);
        }

        // A window's process and its parents, up to (not including) the session
        // and the compositor: every app shares those.
        std::vector<pid_t> lineage(pid_t pid) {
            std::vector<pid_t> out;
            static const pid_t SELF = getpid();
            for (int depth = 0; pid > 1 && pid != SELF && depth < 6; ++depth) {
                std::ifstream f(std::format("/proc/{}/stat", pid));
                std::string   stat;
                std::getline(f, stat);
                const auto CLOSE = stat.rfind(')');
                const auto OPEN  = stat.find('(');
                if (CLOSE == std::string::npos || OPEN == std::string::npos)
                    break;
                const std::string COMM = stat.substr(OPEN + 1, CLOSE - OPEN - 1);
                if (COMM == "systemd" || COMM.starts_with("uwsm") || COMM == "Hyprland" || COMM == "Hyprland-wrapped")
                    break;
                out.push_back(pid);
                std::istringstream rest(stat.substr(CLOSE + 2));
                char               state = 0;
                pid_t              ppid  = 0;
                rest >> state >> ppid;
                pid = ppid;
            }
            return out;
        }
        bool sameApp(const SGrant& g, const PHLWINDOW& w, const std::vector<pid_t>& procs) {
            if (!g.cls.empty() && g.cls == w->m_class && !w->m_class.empty())
                return true;
            for (const pid_t P : procs)
                if (std::ranges::find(g.procs, P) != g.procs.end())
                    return true;
            return false;
        }

        // Did the user ask for `w`? See Intent.hpp.
        const char* askedWhy(const PHLWINDOW& w) {
            std::erase_if(g_grants, [](const SGrant& g) { return g.cls.empty() ? since(g.at) > GRANT_S : since(g.taken) > APP_S; });
            const auto PROCS = lineage(w->getPID());
            // A launch not yet answered: this window is its answer. Taken first,
            // so each launch is used up by a window of its own.
            // ...unless it is a further window of an app a launch already went
            // to (its loading screen took that launch; this is the app itself):
            // it is that launch's, and leaves the next launch for its own window.
            // (No newer launch waiting: a launch made after that app's first
            // window -- kitty, then kitty again -- is this window's own.)
            for (auto& g : g_grants)
                if (!g.cls.empty() && sameApp(g, w, PROCS) &&
                    std::ranges::none_of(g_grants, [&](const SGrant& o) { return o.cls.empty() && o.at > g.taken; })) {
                    g.taken = clock::now();
                    return "the launched app's next window";
                }
            for (auto& g : g_grants)
                if (g.cls.empty()) {
                    g.cls   = w->m_class.empty() ? std::string{"?"} : w->m_class;
                    g.procs = PROCS;
                    g.taken = clock::now();
                    return "a launch";
                }
            const auto F = Desktop::focusState()->window();
            if (!F || F == w || !F->m_isMapped)
                return "nothing focused"; // nothing to keep focused
            if (since(g_lastPress) < INPUT_S)
                return "a click or Enter just before";
            if (w->parent() && w->parent() == F)
                return "a dialog of the focused window";
            const bool SAMEAPP = (w->getPID() > 0 && w->getPID() == F->getPID()) || (!w->m_class.empty() && w->m_class == F->m_class);
            return SAMEAPP && since(g_lastPress) < SAMEAPP_S ? "the focused app, after a click or Enter" : nullptr;
        }

        void onOpenEarly(const PHLWINDOW& w) {
            std::erase_if(g_quiet, [](const PHLWINDOWREF& r) { return !r || !r->m_isMapped; });
            if (!w || !on() || !Mosaic::isMosaicWorkspace(w->m_workspace))
                return;
            if (const char* WHY = askedWhy(w)) {
                trace(std::format("{} opened: asked for ({})", w->m_class, WHY));
                return;
            }
            w->m_noInitialFocus = true;
            g_quiet.emplace_back(w);
            trace(std::format("{} opened UNASKED: no focus, the pointer stays", w->m_class));
            // The pointer rides the window it is on, if the hand is not moving it.
            const auto     F = Desktop::focusState()->window();
            const Vector2D P = g_pInputManager->getMouseCoordsInternal();
            if (F && since(g_lastUserMove) > IDLE_S && !g_pInputManager->isConstrained()) {
                const Vector2D POS = F->positionAnimation()->value(), SIZE = F->sizeAnimation()->value();
                if (P.x >= POS.x && P.y >= POS.y && P.x < POS.x + SIZE.x && P.y < POS.y + SIZE.y && SIZE.x > 0 && SIZE.y > 0)
                    g_ride = SRide{.win = F, .rel = (P - POS) / SIZE, .from = clock::now()};
            }
        }
    }

    void granted(const char* why) {
        g_grants.push_back({clock::now(), ""});
        if (g_grants.size() > 16)
            g_grants.erase(g_grants.begin());
        trace(std::format("grant ({})", why ? why : "?"));
    }

    bool quiet(const PHLWINDOW& w) {
        return w && std::ranges::any_of(g_quiet, [&](const PHLWINDOWREF& r) { return r.lock() == w; });
    }

    void preRender(PHLMONITOR monitor) {
        if (!g_ride)
            return;
        const auto W = g_ride->win.lock();
        // The hand took over, the window went, or it has had its time.
        if (!W || !W->m_isMapped || since(g_ride->from) > RIDE_S || g_lastUserMove > g_ride->from || g_pInputManager->isConstrained()) {
            g_ride.reset();
            return;
        }
        if (!monitor || W->m_monitor.lock() != monitor)
            return;
        const Vector2D POS = W->positionAnimation()->value(), SIZE = W->sizeAnimation()->value();
        const Vector2D WANT = POS + g_ride->rel * SIZE;
        const Vector2D P    = g_pInputManager->getMouseCoordsInternal();
        if (std::abs(WANT.x - P.x) < 0.5 && std::abs(WANT.y - P.y) < 0.5) {
            // Settled where its window settled.
            if (!W->positionAnimation()->isBeingAnimated() && !W->sizeAnimation()->isBeingAnimated() && since(g_ride->from) > 0.3)
                g_ride.reset();
            return;
        }
        g_warping = true;
        Pointer::pointerController()->warpTo(WANT, true);
        g_warping = false;
    }

    void init() {
        Launch::onLaunch = [] { granted("hyprdrive"); };
        g_openEarly = Event::bus()->m_events.window.openEarly.listen([](PHLWINDOW w) { onOpenEarly(w); });
        g_close     = Event::bus()->m_events.window.close.listen([](PHLWINDOW w) {
            std::erase_if(g_quiet, [&](const PHLWINDOWREF& r) { return !r || r.lock() == w; });
        });
        g_key = Event::bus()->m_events.input.keyboard.key.listen([](IKeyboard::SKeyEvent e, Event::SCallbackInfo&) {
            if (e.state != WL_KEYBOARD_KEY_STATE_PRESSED)
                return;
            // Enter, by its evdev code (28 / 96): the keymap does not matter here.
            const bool ENTER = e.keycode == 28 || e.keycode == 96;
            if (!ENTER)
                return;
            g_lastPress = clock::now();
            // Enter in a launcher (a layer surface with the keyboard) launched something.
            const auto KB = g_pSeatManager ? g_pSeatManager->m_state.keyboardFocus.lock() : SP<CWLSurfaceResource>{};
            if (layerOf(KB))
                granted("enter in a launcher");
        });
        g_button = Event::bus()->m_events.input.mouse.button.listen([](IPointer::SButtonEvent e, Event::SCallbackInfo&) {
            if (e.state != WL_POINTER_BUTTON_STATE_PRESSED)
                return;
            g_lastPress = clock::now();
            const auto PTR = g_pSeatManager ? g_pSeatManager->m_state.pointerFocus.lock() : SP<CWLSurfaceResource>{};
            if (layerOf(PTR))
                granted("click on a layer");
        });
        g_move = Event::bus()->m_events.input.mouse.move.listen([](Vector2D, Event::SCallbackInfo&) {
            if (!g_warping)
                g_lastUserMove = clock::now();
        });
        // Not fatal when missing: then only Hyprdrive's own launches and
        // launchers grant.
        // The first spawnRawProc that is CExecutor's.
        g_spawnHook = FunctionHooks::firstMatch("Intent", "spawnRawProc", rc<void*>(hkSpawn), "CExecutor::spawnRawProc(");
        if (g_spawnHook && !g_spawnHook->hook())
            g_spawnHook = nullptr;
    }

    void shutdown() {
        Launch::onLaunch = nullptr;
        if (g_spawnHook) {
            g_spawnHook->unhook();
            g_spawnHook = nullptr;
        }
        g_openEarly.reset();
        g_close.reset();
        g_key.reset();
        g_button.reset();
        g_move.reset();
        g_ride.reset();
        g_quiet.clear();
        g_grants.clear();
    }
}
