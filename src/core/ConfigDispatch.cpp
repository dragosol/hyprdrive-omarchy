#include "ConfigParts.hpp"
using namespace ConfigDetail;

namespace {

int dispatcherFactoryLua(lua_State* L, std::string_view name);

using TArgValidator = bool (*)(std::string_view);

struct SDispatcher {
    std::string_view                    name;
    std::regex                          argPattern;
    std::string_view                    defaultArg;
    std::string_view                    typeArgError;
    std::string_view                    invalidArgError;
    TArgValidator                       argValidator = nullptr;
    ScrollOverview::Config::TDispatcher dispatcher   = nullptr;
    lua_CFunction                       luaFunction  = nullptr;
    // Same factory, distinct function: a lua_CFunction cannot capture, so
    // 'this call came in under the old namespace' has to be baked into the
    // pointer the old namespace is registered with. ONE RELEASE.
    lua_CFunction                       luaLegacy    = nullptr;

    bool isArgValid(const std::string_view arg) const {
        if (argValidator)
            return argValidator(arg);

        return std::regex_match(arg.begin(), arg.end(), argPattern);
    }
};

SDispatcher* findDispatcher(const std::string_view name) {
    static SDispatcher registrations[] = {
        {
            .name            = "overview",
            .argPattern      = std::regex{R"(^(select|tab|(toggle|on|open|enable|off|close|disable)([ \t]+(all|[^ \t"\\]+))?)$)"},
            .defaultArg      = "toggle",
            .typeArgError    = "expected an optional string argument; did you forget quotes around it?",
            .invalidArgError = "expected select, tab or toggle/open/close [monitor|all] (aliases: on, enable, off, disable)",
            .luaFunction     = [](lua_State* L) { return dispatcherFactoryLua(L, "overview"); },
            .luaLegacy       = [](lua_State* L) {
                ScrollOverview::Compat::noteLegacyUse(ScrollOverview::Compat::ELegacy::LUA, "hl.plugin.scrolloverview.overview");
                return dispatcherFactoryLua(L, "overview");
            },
        },
        {
            .name            = "navigate",
            .argPattern      = std::regex{"^(left|right|up|down)$"},
            .typeArgError    = "expected a string argument",
            .invalidArgError = "expected one of: left, right, up, down",
            .luaFunction     = [](lua_State* L) { return dispatcherFactoryLua(L, "navigate"); },
            .luaLegacy       = [](lua_State* L) {
                ScrollOverview::Compat::noteLegacyUse(ScrollOverview::Compat::ELegacy::LUA, "hl.plugin.scrolloverview.navigate");
                return dispatcherFactoryLua(L, "navigate");
            },
        },
        {
            .name            = "minimize",
            .argPattern      = std::regex{"^(window|restore)$"},
            .defaultArg      = "window",
            .typeArgError    = "expected an optional string argument; did you forget quotes around it?",
            .invalidArgError = "expected one of: window, restore",
            .luaFunction     = [](lua_State* L) { return dispatcherFactoryLua(L, "minimize"); },
            .luaLegacy       = [](lua_State* L) {
                ScrollOverview::Compat::noteLegacyUse(ScrollOverview::Compat::ELegacy::LUA, "hl.plugin.scrolloverview.minimize");
                return dispatcherFactoryLua(L, "minimize");
            },
        },
        {
            // X34. `1`..`10` are the reference's SLOT numbers, not the digits on
            // the keys -- `<Super>0` is the tenth app and so binds `10`. One
            // dispatcher for all thirty accelerators, because the reference has
            // one callback for all thirty and the modifier is an argument to it.
            .name            = "hotkey",
            .argPattern      = std::regex{"^(overlay|((shift|ctrl) )?([1-9]|10))$"},
            .typeArgError    = "expected a string argument",
            .invalidArgError = "expected overlay, or 1-10 optionally prefixed with 'shift ' or 'ctrl '",
            .luaFunction     = [](lua_State* L) { return dispatcherFactoryLua(L, "hotkey"); },
            .luaLegacy       = [](lua_State* L) {
                ScrollOverview::Compat::noteLegacyUse(ScrollOverview::Compat::ELegacy::LUA, "hl.plugin.scrolloverview.hotkey");
                return dispatcherFactoryLua(L, "hotkey");
            },
        },
        {
            // V11 V4. The path is the tail of the argument and is deliberately
            // NOT constrained: a stack is any path the user has, and a pattern
            // strict enough to be worth writing would reject the ones with
            // spaces, brackets or non-ASCII in them. The VERB is checked here;
            // the path is checked by the filesystem, which is the only thing
            // that can actually answer.
            .name            = "stack",
            .argPattern      = std::regex{"^(clear|(pin|unpin|toggle|open) .+|view (fan|grid|auto) .+|sort (name|mtime|added|modified|kind) .+|scroll -?[0-9]+|filter( .*)?)$"},
            .typeArgError    = "expected a string argument",
            .invalidArgError = "expected: pin <path> | unpin <path> | toggle <path> | open <path> | view auto|fan|grid <path> | sort name|added|modified|kind <path> | scroll <rows> | filter [text] | clear",
            .luaFunction     = [](lua_State* L) { return dispatcherFactoryLua(L, "stack"); },
            .luaLegacy       = [](lua_State* L) {
                ScrollOverview::Compat::noteLegacyUse(ScrollOverview::Compat::ELegacy::LUA, "hl.plugin.scrolloverview.stack");
                return dispatcherFactoryLua(L, "stack");
            },
        },
        {
            // A diagnostic, and in the table for the same reason the rest are:
            // a Lua config reaches dispatchers ONLY through these bindings, so
            // one that is not here cannot be called at all (user, 2026-09-19:
            // "attempt to call a nil value (field 'debug')").
            .name            = "debug",
            .argPattern      = std::regex{"^(frames|mem|motion|windows|glass|scratchpad|lockstate|desktop|type|type \\S+|activate \\S+|tile \\S+|xgeom|press [0-9]+|ripple|ripple -?[0-9.]+ -?[0-9.]+)$"},
            .defaultArg      = "frames",
            .typeArgError    = "expected a string argument",
            .invalidArgError = "expected: frames | mem | motion | scratchpad | lockstate | desktop | type [text] | activate <class> | tile <class> | xgeom | press <keycode> | ripple [x y]",
            .luaFunction     = [](lua_State* L) { return dispatcherFactoryLua(L, "debug"); },
            .luaLegacy       = [](lua_State* L) {
                ScrollOverview::Compat::noteLegacyUse(ScrollOverview::Compat::ELegacy::LUA, "hl.plugin.scrolloverview.debug");
                return dispatcherFactoryLua(L, "debug");
            },
        },
        {
            // Mosaic.cpp: focus by direction, mosaic-aware (Super+arrows).
            .name            = "focus",
            .argPattern      = std::regex{"^(l|r|u|d|left|right|up|down)$"},
            .typeArgError    = "expected a string argument",
            .invalidArgError = "expected one of: l, r, u, d",
            .luaFunction     = [](lua_State* L) { return dispatcherFactoryLua(L, "focus"); },
            .luaLegacy       = [](lua_State* L) { return dispatcherFactoryLua(L, "focus"); },
        },
        {
            // Mosaic.cpp: swap by direction, mosaic-aware (Super+Shift+arrows).
            .name            = "swap",
            .argPattern      = std::regex{"^(l|r|u|d|left|right|up|down)$"},
            .typeArgError    = "expected a string argument",
            .invalidArgError = "expected one of: l, r, u, d",
            .luaFunction     = [](lua_State* L) { return dispatcherFactoryLua(L, "swap"); },
            .luaLegacy       = [](lua_State* L) { return dispatcherFactoryLua(L, "swap"); },
        },
        {
            // DesktopSwitch.cpp: one desktop by number, empty ones included
            // (Super+scroll).
            .name            = "desktop",
            .argPattern      = std::regex{"^(next|prev)$"},
            .typeArgError    = "expected a string argument",
            .invalidArgError = "expected one of: next, prev",
            .luaFunction     = [](lua_State* L) { return dispatcherFactoryLua(L, "desktop"); },
            .luaLegacy       = [](lua_State* L) { return dispatcherFactoryLua(L, "desktop"); },
        },
        {
            // ShowDesktop.hpp: Super+D's slide of the desktop's windows out of
            // view and back (the Keybinds switch binds it, on by default):
            // hl.plugin.hyprdrive.show_desktop().
            .name            = "show_desktop",
            .argPattern      = std::regex{"^(toggle|on|off)$"},
            .defaultArg      = "toggle",
            .typeArgError    = "expected an optional string argument; did you forget quotes around it?",
            .invalidArgError = "expected toggle, on or off",
            .luaFunction     = [](lua_State* L) { return dispatcherFactoryLua(L, "show_desktop"); },
            .luaLegacy       = [](lua_State* L) { return dispatcherFactoryLua(L, "show_desktop"); },
        },
        {
            .name            = "window",
            .argPattern      = std::regex{"^(select|close|click)$"},
            .typeArgError    = "expected a string argument",
            .invalidArgError = "expected one of: select, close, click",
            .luaFunction     = [](lua_State* L) { return dispatcherFactoryLua(L, "window"); },
            .luaLegacy       = [](lua_State* L) {
                ScrollOverview::Compat::noteLegacyUse(ScrollOverview::Compat::ELegacy::LUA, "hl.plugin.scrolloverview.window");
                return dispatcherFactoryLua(L, "window");
            },
        },
    };

    const auto MATCH = std::ranges::find_if(registrations, [name](const auto& registration) { return registration.name == name; });
    return MATCH == std::end(registrations) ? nullptr : &*MATCH;
}

int runDispatcherNow(lua_State* L, const SDispatcher& dispatcher, const char* arg) {
    if (!dispatcher.dispatcher)
        return luaL_error(L, "%s: dispatcher is not registered", dispatcher.name.data());

    const auto result = dispatcher.dispatcher(arg);
    if (!result.success)
        return luaL_error(L, "%s: %s", dispatcher.name.data(), result.error.c_str());

    return 0;
}

void pushDispatcherBindAction(lua_State* L, const char* name, const char* arg) {
    const std::string CODE = "return function() return hl.plugin." + std::string{ScrollOverview::Compat::NAME} + "._dispatch(\"" + std::string{name} + "\", \"" + std::string{arg} + "\") end";

    if (luaL_loadstring(L, CODE.c_str()) != LUA_OK)
        lua_error(L);

    if (lua_pcall(L, 0, 1, 0) != LUA_OK)
        lua_error(L);
}

int runDispatcherActionLua(lua_State* L) {
    if (lua_gettop(L) < 2 || lua_isnoneornil(L, 1) || lua_isnoneornil(L, 2))
        return luaL_error(L, "_dispatch: expected dispatcher name and argument");

    if (!lua_isstring(L, 1) || !lua_isstring(L, 2))
        return luaL_error(L, "_dispatch: expected string arguments");

    const char* name = lua_tostring(L, 1);
    const char* arg  = lua_tostring(L, 2);
    const auto  DISPATCHER = findDispatcher(name);

    if (!DISPATCHER)
        return luaL_error(L, "_dispatch: unknown dispatcher '%s'", name);
    if (!DISPATCHER->isArgValid(arg))
        return luaL_error(L, "%s: invalid argument '%s', %s", name, arg, DISPATCHER->invalidArgError.data());

    return runDispatcherNow(L, *DISPATCHER, arg);
}

int runDispatcherActionLegacyLua(lua_State* L) {
    ScrollOverview::Compat::noteLegacyUse(ScrollOverview::Compat::ELegacy::LUA, "hl.plugin.scrolloverview._dispatch");
    return runDispatcherActionLua(L);
}

int dispatcherFactoryLua(lua_State* L, std::string_view name) {
    const auto DISPATCHER = findDispatcher(name);
    if (!DISPATCHER)
        return luaL_error(L, "%s: dispatcher metadata is not registered", name.data());

    const char* arg = DISPATCHER->defaultArg.empty() ? nullptr : DISPATCHER->defaultArg.data();

    if (!arg && (lua_gettop(L) < 1 || lua_isnoneornil(L, 1)))
        return luaL_error(L, "%s: %s", DISPATCHER->name.data(), DISPATCHER->typeArgError.data());

    if (lua_gettop(L) >= 1) {
        if (lua_isnoneornil(L, 1))
            return luaL_error(L, "%s: %s", DISPATCHER->name.data(), DISPATCHER->typeArgError.data());

        if (!lua_isstring(L, 1))
            return luaL_error(L, "%s: %s", DISPATCHER->name.data(), DISPATCHER->typeArgError.data());

        arg = lua_tostring(L, 1);
    }

    if (!DISPATCHER->isArgValid(arg))
        return luaL_error(L, "%s: invalid argument '%s', %s", DISPATCHER->name.data(), arg, DISPATCHER->invalidArgError.data());

    if (g_pKeybindManager && g_pKeybindManager->m_currentKeybind && g_pKeybindManager->m_currentKeybind->handler == "__lua") {
        return runDispatcherNow(L, *DISPATCHER, arg);
    }

    pushDispatcherBindAction(L, DISPATCHER->name.data(), arg);

    return 1;
}

int configureLua(lua_State* L) {
    if (!lua_istable(L, 1))
        return luaL_error(L, "configure: expected a table");

    const int CONFIG = lua_absindex(L, 1);

    lua_getglobal(L, "hl");
    if (!lua_istable(L, -1))
        return luaL_error(L, "configure: global hl table is not available");

    lua_getfield(L, -1, "config");
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        return luaL_error(L, "configure: hl.config is not available");
    }

    lua_newtable(L);
    lua_newtable(L);
    lua_pushvalue(L, CONFIG);
    lua_setfield(L, -2, ScrollOverview::Compat::NAME);
    lua_setfield(L, -2, "plugin");

    if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        lua_pop(L, 2);
        return luaL_error(L, "configure: %s", err ? err : "hl.config failed");
    }

    lua_pop(L, 1);

    return 0;
}

int configureLegacyLua(lua_State* L) {
    ScrollOverview::Compat::noteLegacyUse(ScrollOverview::Compat::ELegacy::LUA, "hl.plugin.scrolloverview.configure");
    return configureLua(L);
}


int gestureLua(lua_State* L) {
    if (!g_gestureRegistrar)
        return luaL_error(L, "gesture: registrar is not registered");

    if (!lua_istable(L, 1))
        return luaL_error(L, "gesture: expected a table, e.g. { fingers = 3, direction = \"up\" }");

    lua_getfield(L, 1, "fingers");
    if (!lua_isinteger(L, -1))
        return luaL_error(L, "gesture: 'fingers' (integer) is required");
    const size_t FINGERS = sc<size_t>(lua_tointeger(L, -1));
    lua_pop(L, 1);

    lua_getfield(L, 1, "direction");
    if (!lua_isstring(L, -1))
        return luaL_error(L, "gesture: 'direction' (string) is required");
    const std::string DIRECTION = lua_tostring(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, 1, "action");
    const std::string ACTION = lua_isstring(L, -1) ? lua_tostring(L, -1) : "overview";
    lua_pop(L, 1);

    // accept either "mods" or "mod"
    lua_getfield(L, 1, "mods");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_getfield(L, 1, "mod");
    }
    const std::string MODS = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
    lua_pop(L, 1);

    lua_getfield(L, 1, "scale");
    const float SCALE = lua_isnumber(L, -1) ? sc<float>(lua_tonumber(L, -1)) : 1.F;
    lua_pop(L, 1);

    lua_getfield(L, 1, "disable_inhibit");
    const bool DISABLE_INHIBIT = lua_toboolean(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, 1, "arg");
    std::string ARG = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
    lua_pop(L, 1);
    if (ARG.empty()) {
        lua_getfield(L, 1, "dispatcher");
        const std::string DISPATCHER = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
        lua_pop(L, 1);
        lua_getfield(L, 1, "data");
        const std::string DATA = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
        lua_pop(L, 1);
        if (!DISPATCHER.empty())
            ARG = DATA.empty() ? DISPATCHER : DISPATCHER + "," + DATA;
    }

    const auto result = g_gestureRegistrar(FINGERS, DIRECTION, ACTION, MODS, SCALE, DISABLE_INHIBIT, ARG);
    if (!result.success)
        return luaL_error(L, "gesture: %s", result.error.c_str());

    return 0;
}

int gestureLegacyLua(lua_State* L) {
    ScrollOverview::Compat::noteLegacyUse(ScrollOverview::Compat::ELegacy::LUA, "hl.plugin.scrolloverview.gesture");
    return gestureLua(L);
}

}

namespace ScrollOverview::Config {

using namespace ConfigPart;

void registerDispatcher(const std::string& name, TDispatcher dispatcher) {
    HyprlandAPI::addDispatcherV2(SCROLLOVERVIEW_HANDLE, std::string{Compat::NAME} + ":" + name, dispatcher);

    // ONE RELEASE. The old spelling dispatches into the very same handler; the
    // wrapper exists only to notice that it was used.
    const std::string LEGACY = std::string{Compat::LEGACY_NAME} + ":" + name;
    HyprlandAPI::addDispatcherV2(SCROLLOVERVIEW_HANDLE, LEGACY, [dispatcher, LEGACY](std::string arg) {
        Compat::noteLegacyUse(Compat::ELegacy::DISPATCHER, LEGACY);
        return dispatcher(std::move(arg));
    });

    if (::Config::mgr()->type() != ::Config::CONFIG_LUA)
        return;

    const auto DISPATCHER = findDispatcher(name);
    if (!DISPATCHER)
        return;

    DISPATCHER->dispatcher = dispatcher;
    HyprlandAPI::addLuaFunction(SCROLLOVERVIEW_HANDLE, Compat::NAME, std::string{DISPATCHER->name}, DISPATCHER->luaFunction);
    HyprlandAPI::addLuaFunction(SCROLLOVERVIEW_HANDLE, Compat::LEGACY_NAME, std::string{DISPATCHER->name}, DISPATCHER->luaLegacy);
}

void registerGesture(TGestureRegistrar gestureRegistrar, TGestureKeyword gestureKeyword) {
    HyprlandAPI::addConfigKeyword(SCROLLOVERVIEW_HANDLE, std::string{Compat::NAME} + "-gesture", gestureKeyword, {});
    HyprlandAPI::addConfigKeyword(SCROLLOVERVIEW_HANDLE, std::string{Compat::LEGACY_NAME} + "-gesture", gestureKeyword, {});   // ONE RELEASE

    if (::Config::mgr()->type() != ::Config::CONFIG_LUA)
        return;

    g_gestureRegistrar = gestureRegistrar;
    HyprlandAPI::addLuaFunction(SCROLLOVERVIEW_HANDLE, Compat::NAME, "gesture", ::gestureLua);
    HyprlandAPI::addLuaFunction(SCROLLOVERVIEW_HANDLE, Compat::LEGACY_NAME, "gesture", ::gestureLegacyLua);
}

namespace ConfigPart {
void registerLuaFunctions() {
    if (::Config::mgr()->type() != ::Config::CONFIG_LUA)
        return;

    HyprlandAPI::addLuaFunction(SCROLLOVERVIEW_HANDLE, Compat::NAME, "_dispatch", ::runDispatcherActionLua);
    HyprlandAPI::addLuaFunction(SCROLLOVERVIEW_HANDLE, Compat::NAME, "configure", ::configureLua);

    // ONE RELEASE. `configure` under the old namespace still writes the NEW
    // table -- an alias that wrote the deprecated keys would keep re-arming the
    // deprecation it exists to retire.
    HyprlandAPI::addLuaFunction(SCROLLOVERVIEW_HANDLE, Compat::LEGACY_NAME, "_dispatch", ::runDispatcherActionLegacyLua);
    HyprlandAPI::addLuaFunction(SCROLLOVERVIEW_HANDLE, Compat::LEGACY_NAME, "configure", ::configureLegacyLua);
}
} // namespace ConfigPart

}
