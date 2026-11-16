#include "HyprCompat.hpp"
#include "FunctionHooks.hpp"

#include "globals.hpp"

#include <hyprland/src/plugins/HookSystem.hpp>
#include <hyprland/src/config/supplementary/executor/Executor.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/state/ViewState.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/state/workspace/Resolver.hpp>
#include <hyprland/src/workspace/WorkspaceUtils.hpp>
#include <hyprland/src/helpers/math/Direction.hpp>
#include <hyprutils/string/VarList2.hpp>

namespace Hyprdrive::Compat {
    namespace {
        CFunctionHook*  g_invokeBindHook = nullptr;
        Keybinds::PBind g_currentBind;

        using TInvokeBind = Keybinds::SBindResult (*)(Keybinds::CKeybindManager*, const Keybinds::PBind&, bool, Keybinds::SPressedInput*);

        Keybinds::SBindResult hkInvokeBind(Keybinds::CKeybindManager* self, const Keybinds::PBind& bind, bool pressed, Keybinds::SPressedInput* pressedInput) {
            // Binds nest (a bind's action can invoke another): restore the outer one.
            auto outer  = std::exchange(g_currentBind, bind);
            auto result = rc<TInvokeBind>(g_invokeBindHook->m_original)(self, bind, pressed, pressedInput);
            g_currentBind = std::move(outer);
            return result;
        }
    }

    std::unordered_map<std::string, TOwnDispatcher>& ownDispatchers() {
        static std::unordered_map<std::string, TOwnDispatcher> table;
        return table;
    }

    std::optional<SDispatchResult> dispatchOwn(const std::string& name, std::string arg) {
        const auto IT = ownDispatchers().find(name);
        if (IT == ownDispatchers().end())
            return std::nullopt;
        return IT->second(std::move(arg));
    }

    std::optional<std::string> dispatchLua(const std::string& luaExpr) {
        if (!::Config::mgr() || ::Config::mgr()->type() != ::Config::CONFIG_LUA)
            return std::nullopt;
        const auto MGR = ::Config::Lua::mgr().lock();
        if (!MGR)
            return std::nullopt;
        return MGR->eval(std::format("return hl.dispatch({})", luaExpr));
    }

    namespace {
        namespace CA = ::Config::Actions;
        using Hyprutils::String::CVarList2;

        SDispatchResult wrap(CA::ActionResult res) {
            if (!res)
                return {.success = false, .error = res.error().message};
            return {.passEvent = res->passEvent};
        }

        // 0.56 windowFromArg: empty or "active" is the focused window.
        std::optional<PHLWINDOW> windowFromArg(const std::string& arg) {
            if (arg.empty() || arg == "active")
                return std::nullopt;
            return Desktop::viewState()->query().selector(arg).runWindow();
        }

        SDispatchResult fullscreen(const std::string& args) {
            CVarList2  ARGS(std::string{args}, 2, ' ');
            const auto MODE = ARGS.size() > 0 && ARGS[0] == "1" ? Fullscreen::FSMODE_MAXIMIZED : Fullscreen::FSMODE_FULLSCREEN;
            if (ARGS.size() <= 1 || ARGS[1] == "toggle")
                return wrap(CA::fullscreenWindow(MODE, true));
            const auto PWINDOW = Desktop::focusState()->window();
            if (!PWINDOW)
                return {.success = false, .error = "Window not found"};
            if ((ARGS[1] == "set" && !Fullscreen::controller()->isFullscreen(PWINDOW, MODE)) || (ARGS[1] == "unset" && Fullscreen::controller()->isFullscreen(PWINDOW, MODE)))
                return wrap(CA::fullscreenWindow(MODE, true));
            return {};
        }

        SDispatchResult fullscreenstate(const std::string& args) {
            CVarList2  ARGS(std::string{args}, 3, ' ');
            const auto PWINDOW = Desktop::focusState()->window();
            if (!PWINDOW)
                return {.success = false, .error = "Window not found"};
            const auto mode = [&](size_t i, Fullscreen::eFullscreenMode current) {
                try {
                    const int M = std::stoi(std::string(ARGS[i]));
                    return M != -1 ? sc<Fullscreen::eFullscreenMode>(M) : current;
                } catch (...) { return current; }
            };
            const auto CURRENT = Fullscreen::controller()->getFullscreenModes(PWINDOW);
            return wrap(CA::fullscreenWindow(mode(0, CURRENT.internal), mode(1, CURRENT.client), true));
        }

        // movetoworkspace[silent] WS[,window]
        SDispatchResult moveToWorkspace(const std::string& args, bool silent) {
            std::optional<PHLWINDOW> window;
            std::string              ws = args;
            if (args.contains(',')) {
                window = Desktop::viewState()->query().selector(args.substr(args.find_last_of(',') + 1)).runWindow();
                ws     = args.substr(0, args.find_last_of(','));
            }
            return wrap(CA::moveToWorkspace(ws, silent, window));
        }

        // As 0.57's dsp_toggleSpecial (0.56: "special:" + name, created on the focused monitor).
        SDispatchResult toggleSpecial(const std::string& name) {
            const auto TARGET = State::Workspace::resolver()->getWorkspaceTargetFromString(Workspace::specialWorkspaceAddressFromName(name));
            if (!TARGET.valid() || TARGET.type != Workspace::eWorkspaceType::SPECIAL)
                return {.success = false, .error = "Invalid special workspace"};
            auto ws = State::Workspace::state()->find(TARGET);
            if (!ws) {
                if (const auto PMONITOR = Desktop::focusState()->monitor())
                    ws = State::Workspace::state()->create(TARGET, PMONITOR);
            }
            if (!ws)
                return {.success = false, .error = "Could not resolve special workspace"};
            auto res = wrap(CA::toggleSpecial(ws));
            ws->ready();
            return res;
        }

        SDispatchResult cycleNext(const std::string& arg) {
            CVarList2           args(std::string{arg}, 0, 's', true);
            const bool          PREV = args.contains("prev") || args.contains("p") || args.contains("last") || args.contains("l");
            const bool          NEXT = args.contains("next") || args.contains("n");
            std::optional<bool> onlyTiled, onlyFloating;
            if (args.contains("tile") || args.contains("tiled"))
                onlyTiled = true;
            if (args.contains("float") || args.contains("floating"))
                onlyFloating = true;
            return wrap(CA::cycleNext(NEXT || !PREV, onlyTiled, onlyFloating));
        }

        // "FACTOR [toggle|mult]" on every monitor, as hl.gesture's cursor zoom does on a gesture's begin.
        // toggle: FACTOR, or back to cursor:zoom_factor when already zoomed.
        SDispatchResult cursorZoom(const std::string& arg) {
            CVarList2   ARGS(std::string{arg}, 2, ' ');
            float       factor = 2.F;
            try {
                if (ARGS.size() > 0 && !ARGS[0].empty())
                    factor = std::stof(std::string(ARGS[0]));
            } catch (...) { return {.success = false, .error = std::format("cursorzoom: bad factor {}", arg)}; }
            const bool  MULT  = ARGS.size() > 1 && ARGS[1] == "mult";
            static auto PBASE = CConfigValue<::Config::FLOAT>("cursor:zoom_factor");
            const float BASE  = std::clamp(sc<float>(*PBASE), 1.F, 100.F);
            bool        zoomed = false;
            for (const auto& m : State::monitorState()->monitors())
                zoomed = zoomed || m->m_cursorZoom->goal() != BASE;
            for (const auto& m : State::monitorState()->monitors())
                *m->m_cursorZoom = std::clamp(MULT ? m->m_cursorZoom->goal() * factor : (zoomed ? BASE : factor), 1.F, 100.F);
            return {};
        }

        using TLegacy = SDispatchResult (*)(const std::string&);
        // Hyprland 0.56's legacy dispatchers (config/legacy/DispatcherTranslator.cpp), same argument syntax.
        const std::unordered_map<std::string_view, TLegacy>& legacyDispatchers() {
            static const std::unordered_map<std::string_view, TLegacy> table = {
                {"exec",
                 [](const std::string& a) -> SDispatchResult {
                     const auto PROC = ::Config::Supplementary::executor()->spawn(a);
                     if (!PROC.has_value())
                         return {.success = false, .error = std::format("Failed to start process. No closing bracket in exec rule. {}", a)};
                     return {.success = PROC.value() > 0, .error = std::format("Failed to start process {}", a)};
                 }},
                {"execr",
                 [](const std::string& a) -> SDispatchResult {
                     const auto PROC = ::Config::Supplementary::executor()->spawnRaw(a);
                     return {.success = PROC && *PROC > 0, .error = std::format("Failed to start process {}", a)};
                 }},
                {"killactive", [](const std::string&) { return wrap(CA::closeWindow()); }},
                {"forcekillactive", [](const std::string&) { return wrap(CA::killWindow()); }},
                {"closewindow", [](const std::string& a) { return wrap(CA::closeWindow(Desktop::viewState()->query().selector(a).runWindow())); }},
                {"killwindow", [](const std::string& a) { return wrap(CA::killWindow(Desktop::viewState()->query().selector(a).runWindow())); }},
                {"togglefloating", [](const std::string& a) { return wrap(CA::floatWindow(CA::TOGGLE_ACTION_TOGGLE, windowFromArg(a))); }},
                {"setfloating", [](const std::string& a) { return wrap(CA::floatWindow(CA::TOGGLE_ACTION_ENABLE, windowFromArg(a))); }},
                {"settiled", [](const std::string& a) { return wrap(CA::floatWindow(CA::TOGGLE_ACTION_DISABLE, windowFromArg(a))); }},
                {"pseudo", [](const std::string& a) { return wrap(CA::pseudoWindow(CA::TOGGLE_ACTION_TOGGLE, windowFromArg(a))); }},
                {"pin", [](const std::string& a) { return wrap(CA::pinWindow(CA::TOGGLE_ACTION_TOGGLE, windowFromArg(a))); }},
                {"centerwindow", [](const std::string&) { return wrap(CA::center()); }},
                {"fullscreen", fullscreen},
                {"fullscreenstate", fullscreenstate},
                {"workspace", [](const std::string& a) { return wrap(CA::changeWorkspace(a)); }},
                {"focusworkspaceoncurrentmonitor", [](const std::string& a) { return wrap(CA::changeWorkspaceOnCurrentMonitor(a)); }},
                {"movetoworkspace", [](const std::string& a) { return moveToWorkspace(a, false); }},
                {"movetoworkspacesilent", [](const std::string& a) { return moveToWorkspace(a, true); }},
                {"togglespecialworkspace", toggleSpecial},
                {"movefocus",
                 [](const std::string& a) -> SDispatchResult {
                     const auto DIR = Math::fromChar(a.empty() ? '\0' : a[0]);
                     if (DIR == Math::DIRECTION_DEFAULT)
                         return {.success = false, .error = std::format("Unsupported direction: {}", a)};
                     return wrap(CA::moveFocus(DIR));
                 }},
                {"cyclenext", cycleNext},
                {"layoutmsg", [](const std::string& a) { return wrap(CA::layoutMessage(a)); }},
                {"cursorzoom", cursorZoom},
            };
            return table;
        }
    }

    bool isLuaDispatch(std::string_view s) {
        return s.starts_with("hl.") || s.contains("hl.dsp.");
    }

    SDispatchResult dispatchNamed(const std::string& name, const std::string& arg) {
        if (auto own = dispatchOwn(name, arg))
            return *own;
        if (const auto IT = legacyDispatchers().find(name); IT != legacyDispatchers().end())
            return IT->second(arg);
        if (isLuaDispatch(name)) {
            if (!::Config::mgr() || ::Config::mgr()->type() != ::Config::CONFIG_LUA)
                return {.success = false, .error = "a Lua dispatcher needs a Lua config"};
            const auto RET = dispatchLua(arg.empty() ? name : name + " " + arg);
            if (!RET || RET->starts_with("ok"))
                return {};
            return {.success = false, .error = *RET};
        }
        const auto  ERR = std::format("dispatcher \"{}\" does not exist on Hyprland 0.57; use a Lua expression such as hl.dsp.exec_cmd(\"...\")", name);
        static std::unordered_set<std::string> told;
        if (told.insert(name).second) {
            Log::logger->log(Log::WARN, "hyprdrive", "{}", ERR);
            HyprlandAPI::addNotification(HYPRDRIVE_HANDLE, "[hyprdrive] " + ERR, CHyprColor{1.0, 0.6, 0.2, 1.0}, 8000);
        }
        return {.success = false, .error = ERR};
    }

    bool hookInvokeBind() {
        if (g_invokeBindHook)
            return true;
        g_invokeBindHook = FunctionHooks::firstMatch("HyprCompat", "invokeBind", rc<void*>(hkInvokeBind), "CKeybindManager::invokeBind(");
        return g_invokeBindHook && g_invokeBindHook->hook();
    }

    Keybinds::PBind currentBind() {
        return g_currentBind;
    }
}
