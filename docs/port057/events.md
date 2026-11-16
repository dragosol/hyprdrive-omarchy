# Port 0.56.2 -> 0.57.2: events, IPC, plugin API, Lua, input/seat/focus

Scope: only symbols Hyprdrive uses. Headers: `hl057/include/hyprland/src/...` (written as `<hyprland/src/...>`).
Hyprland source refs are under `sdk/src/hyprland-0.57.2/src`.

**Caution:** the port's first build log (not kept in the tree) is incomplete for 8 TUs. They include `src/core/PluginParts.hpp`, which dies on a missing
header (`DispatcherGesture.hpp`, see N3). Those TUs are main.cpp, Hooks.cpp, Dispatchers.cpp, DebugCommands.cpp,
KeyRouter.cpp, GestureBinds.cpp, SearchInput.cpp and OverviewOpen.cpp. They were grepped by hand, and their hits are
listed below.

## 1. Event bus (`<hyprland/src/event/EventBus.hpp>`)

Most of the bus is unchanged. Every signal Hyprdrive listens to (window.*, workspace.*, monitor.*, input.*, gesture.*,
config.preReload/reloaded, render.pre/preChecks, layer.opened, lock, commit, presented, resize, setShape) keeps its
payload. Hyprland 0.57 still emits each of them. The one exception is `workspace.created`, which is gone (E2).

| # | OLD | NEW |
|---|-----|-----|
| E1 | `m_events.render.stage` was `Event<eRenderStage>`. Listen with `[](eRenderStage stage){...}` and emit with `.emit(RENDER_POST_WALLPAPER)` | `Event<SRenderStageEvent> stage;` Listen with `[](const Event::SRenderStageEvent& e){ e.stage; e.monitor; e.context; }` and emit with `.emit({RENDER_POST_WALLPAPER, MONITOR, ctx})` |
| E2 | `m_events.workspace.created` (`Event<PHLWORKSPACEREF>`) | `createdEarly` / `createdLate`, both `Event<PHLWORKSPACEREF>` |
| E3 | `window.active` carried `Desktop::eFocusReason : uint8_t` | The same signal now carries `eFocusReason : uint32_t`. Only the underlying type changed, so no code change is needed unless you forward-declare it |

Notes on E1 and E2:
- The new struct is defined in EventBus.hpp, in namespace `Event`:
  ```cpp
  struct SRenderStageEvent {
      eRenderStage stage = RENDER_PRE;
      PHLMONITOR   monitor;
      // Borrowed only for this synchronous emission; PRE and POST have no live context.
      std::optional<std::reference_wrapper<Render::CRenderContext>> context;
  };
  ```
  `Render::CRenderContext` is in `<hyprland/src/render/Context.hpp>`. Use `e.monitor` instead of reading the monitor
  from global render data.
- E1 affects 7 listeners (grep-verified): OpenProf.cpp:224, core/main.cpp:626 and :670, TripFrame.cpp:392,
  SpringLoad.cpp:491, FlightBlur.cpp:843, Ripple.cpp:287. Hooks.cpp has none. It also affects one emit, Overview.cpp:556
  (`.emit(RENDER_POST_WALLPAPER)`; use `.emit({RENDER_POST_WALLPAPER, monitor, std::nullopt})`).
- Hyprland emits like this (MonitorScene.cpp:22): `Event::bus()->m_events.render.stage.emit({RENDER_POST_MIRROR, MONITOR, ctx});`.
  Hyprdrive's emit should pass the pass's `CRenderContext&` if it has one, or `std::nullopt`. Listeners must not keep
  `context` after they return.
- E2 (OverviewLifecycle.cpp:254): `createdEarly` fires in `CHLWorkspace::init()`, right after the `createworkspace`
  and `createworkspacev2` IPC events (HLWorkspace.cpp:99-101). That is where 0.56 fired `created`, so it is the
  drop-in replacement.
- `createdLate` fires once, from `ready()`, after placement. Lua's `workspace.created` maps to `createdLate`
  (LuaEventHandler.cpp:118). Use `createdLate` if the handler needs the workspace already on its monitor.
- 0.57 also adds `workspace.renamed`, `window.minimize (PHLWINDOW,bool)`, `window.updateState (PHLWINDOW, const
  SWindowStateUpdate&)` and `window.bell` (Cancellable). Hyprdrive does not use them yet.

## 2. Socket2 events: `g_pEventManager->postEvent(SHyprIPCEvent{...})`

| # | OLD | NEW |
|---|-----|-----|
| I1 | `#include <hyprland/src/managers/EventManager.hpp>`, then `g_pEventManager->postEvent(SHyprIPCEvent{.event=..., .data=...})` | `#include <hyprland/src/ipc/s2/S2.hpp>`, then `IPC::Socket2::sock()->postEvent({.event = ..., .data = ...});` |
| I2 | `if (!g_pEventManager)` guard | `if (!IPC::Socket2::sock())` |

Header (S2.hpp):
```cpp
namespace IPC::Socket2 {
    struct SEvent { std::string event; std::string data; };
    class CSocket2 { public: void postEvent(SEvent&& event); ... };
    UP<CSocket2>& sock();
}
```
- `sock()` is a `UP&`, so guard it the same way as before (Hyprland itself does at HLWorkspace.cpp:121).
- Positional `SHyprIPCEvent{"a", b}` becomes `IPC::Socket2::SEvent{"a", b}` or a braced `{"a", b}`.
- Call sites: CursorShape.cpp:58/61, DesktopOrder.cpp:130-143, FullscreenDesktop.cpp:845, XGeometry.cpp:451,
  Overview.cpp:456, OverviewManager.cpp:32-39.

**Payload change (DesktopOrder.cpp:132-143):** Hyprland 0.57 builds the workspace v1/v2 payloads from
`addressableName()`, not the numeric id:
- `createworkspace` and `destroyworkspace` = `addressableName()`
- `createworkspacev2`, `destroyworkspacev2` and `workspacev2` = `std::format("{},{}", ws->addressableName(), ws->displayName())`
  (sources: HLWorkspace.cpp:99-123 and PlacementController.cpp:227).

To match Hyprland, replace `Compat::id(ws)` with `ws->addressableName()`, and use `addressableName()` for
`createworkspace` and `workspace`. For the destroy events, store the old addressable name in `D` (from 0.57 on, the
v2 id field is that string).

**Urgent (XGeometry.cpp:450-452, FullscreenDesktop.cpp:844-846):**
- Replace the three lines `W->m_isUrgent = true; postEvent("urgent"...); m_events.window.urgent.emit(W);` with
  `W->markUrgent();`. It is public, in `<hyprland/src/desktop/view/window/Window.hpp>`: `void markUrgent();`.
- It sets `m_hints |= WINDOW_HINT_URGENT`, posts `urgent` (`{:x}` of `this`) and emits `window.urgent`. This is the
  same order as before (Window.cpp:815).
- It returns early if the window is focused. Both Hyprdrive sites already check that.

## 3. hyprctl commands (`SHyprCtlCommand` -> `IPC::Socket1::SCommand`)

| # | OLD | NEW |
|---|-----|-----|
| C1 | `SP<SHyprCtlCommand>` | `SP<IPC::Socket1::SCommand>`. Include `<hyprland/src/ipc/s1/S1.hpp>`, which PluginAPI.hpp already includes |
| C2 | `SHyprCtlCommand{.name, .exact = true, .fn = [](eHyprCtlOutputFormat f, std::string req) -> std::string}` | `IPC::Socket1::SCommand{.name, .match = IPC::Socket1::COMMAND_MATCH_EXACT, .handler = [](const IPC::Socket1::SRequest& r) -> IPC::Socket1::SResponse}` |
| C3 | `eHyprCtlOutputFormat`, `FORMAT_JSON`, `FORMAT_NORMAL` | `IPC::Socket1::eOutputFormat`, `IPC::Socket1::FORMAT_JSON`, `IPC::Socket1::FORMAT_NORMAL` |
| C4 | `HyprlandAPI::registerHyprCtlCommand(HANDLE, SHyprCtlCommand)` | `APICALL SP<IPC::Socket1::SCommand> registerHyprCtlCommand(HANDLE handle, IPC::Socket1::SCommand cmd);` |
| C5 | `HyprlandAPI::unregisterHyprCtlCommand(HANDLE, SP<SHyprCtlCommand>)` | `APICALL bool unregisterHyprCtlCommand(HANDLE handle, SP<IPC::Socket1::SCommand> cmd);` |

S1.hpp, verbatim:
```cpp
enum eOutputFormat : uint8_t { FORMAT_NORMAL = 0, FORMAT_JSON };
enum eCommandMatch : uint8_t { COMMAND_MATCH_EXACT = 0, COMMAND_MATCH_PREFIX };
struct SRequest  { std::string command; eOutputFormat format = FORMAT_NORMAL; bool refresh, all, includeConfig, follow; pid_t pid; };
struct SResponse { SResponse(std::string response, eReplyMode mode = REPLY_MODE_CLOSE); /* also const char*, SP<CPromise<std::string>> */ };
struct SCommand  { std::string name; eCommandMatch match = COMMAND_MATCH_EXACT; std::function<SResponse(const SRequest&)> handler; };
```

How it works now:
- `r.command` is the request text after the flags are stripped, which is what the old `std::string` argument held.
- The old `exact = false` is now `COMMAND_MATCH_PREFIX`.
- Hyprland wraps its own legacy handlers like this (Commands.cpp:2075):
  `.handler = [handler](const SRequest& request) { return SResponse{handler(request.format, request.command)}; }`.
  Hyprdrive can do the same and keep `themeReply` and `bindsReply` unchanged apart from the enum type.
- Call sites: SettingsApp.cpp:28-29, 67, 111, 247-271 and Integration.cpp:34, 176-193.

Minimal example:
```cpp
g_hyprctl = HyprlandAPI::registerHyprCtlCommand(HYPRDRIVE_HANDLE, IPC::Socket1::SCommand{
    .name = "hyprdrive-theme", .match = IPC::Socket1::COMMAND_MATCH_EXACT,
    .handler = [](const IPC::Socket1::SRequest& r) { return IPC::Socket1::SResponse{themeReply(r.format)}; }});
```

## 4. HyprlandAPI (`<hyprland/src/plugins/PluginAPI.hpp>`)

- `HYPRLAND_API_VERSION` is now "0.2". main.cpp:26 returns the macro, so nothing changes there.
- Hyprdrive also uses these functions, and their signatures did not change: addConfigKeyword, addConfigValueV2,
  addLuaFunction (`PLUGIN_LUA_FN = int(*)(lua_State*)`), addNotification, addTiledAlgo, removeAlgo,
  createFunctionHook, findFunctionsByName and reloadConfig.

| # | OLD | NEW |
|---|-----|-----|
| P1 | `addDispatcherV2(HANDLE, name, std::function<SDispatchResult(std::string)>)` | The signature is unchanged, but in 0.57 the body is `return false;` (PluginAPI.cpp:202). It registers nothing, and `removeDispatcher` is a stub too. Hyprdrive's Lua functions (`addLuaFunction`, ConfigDispatch.cpp:416-451) are the only route from config. See N1 |
| P2 | (`addWindowDecoration` now takes `SP<...>&&`) | Hyprdrive does not use it |

## 5. Lua config surface (`<hyprland/src/config/lua/ConfigManager.hpp>`)

These compile unchanged and need no edits: `Config::Lua::mgr()` (`WP<CConfigManager>`), `m_eventHandler`,
`CLuaEventHandler::dispatch`, `Objects::CLuaWorkspace::push`, `Config::mgr()->getConfigValue/type()`, and the
animation/bezier accessors.

| # | OLD | NEW |
|---|-----|-----|
| L1 | `MGR->callLuaFn(int index)` (OverviewLifecycle.cpp:1200) | Removed. A Lua bind is now a `Keybinds::CBind` that owns its callback, so call `bind->invoke()`, which returns `Keybinds::SBindResult{passEvent, success, error, followUp}` (K5). The old `.success` comment now holds. For a raw Lua ref, use `Config::Lua::mgr().lock()->caller().call(const CLuaRef&, CLuaCaller::LUA_TIMEOUT_KEYBIND_CALLBACK_MS, "ctx")` (`<hyprland/src/config/lua/LuaCaller.hpp>`), which returns `std::expected<void, std::string>` |
| L2 | Bind `handler == "__lua"` with `arg` = Lua registry index (SettingsApp.cpp:116, ConfigDispatch.cpp:271) | Every `hl.bind` is a Lua bind. `metadata().argument` = `std::to_string(CLuaRef->id())`, which is still a `luaL_ref(L, LUA_REGISTRYINDEX)` index, so `Config::boundAction(argument)` keeps working. `metadata().handler` = `luaL_tolstring(arg 2)`, not `"__lua"` (LuaBindingsToplevel.cpp:75-96). Drop the `"__lua"` test and rely on `boundAction()` returning `nullopt` for binds that are not Hyprdrive's |
| L3 | Dispatch arbitrary text (`hyprctl dispatch`) | `luaMgr->eval(std::format("return hl.dispatch({})", in))`. Signature: `std::optional<std::string> eval(const std::string& code, bool repl = false);`. This is the hyprctl `dispatch` route (Commands.cpp:1336-1340). Get the manager via `Config::Lua::mgr()` |

- The ref behind `argument` is the value `Internal::pushDispatcherFunction(L, 2)` leaves on the stack, which may be a
  wrapper around the user's function or `hl.dsp.*` table. Check that `boundAction` (Hyprdrive's own, ConfigDispatch.cpp:460, not a Hyprland symbol) still recognises it. `CLuaRef::id()` is commented "for display only" in LuaRef.hpp, but it returns the `luaL_ref` value (LuaRef.cpp:10), and `hl.bind` stores exactly that string.

## 6. Keybinds (`g_pKeybindManager` removed)

Use `#include <hyprland/src/keybinds/Manager.hpp>` and `Keybinds::mgr()`. Its signature is
`UP<CKeybindManager>& mgr();`, so guard it as `if (!Keybinds::mgr())`.

| # | OLD | NEW |
|---|-----|-----|
| K1 | `g_pKeybindManager->m_keybinds` (vector `SP<SKeybind>`) | `Keybinds::mgr()->registry().binds()`. Signature: `std::span<const PBind> binds() const;`, where `PBind = SP<CBind>` (`keybinds/Registry.hpp`). Read-only. Add and remove binds on the manager, not the registry: `Keybinds::mgr()->addBind(CBind&&)` returns `PBind`, `removeBind(const PBind&)` returns bool (`registry().add/remove` also exist but skip the manager's bookkeeping) |
| K2 | `SKeybind` and `WP<SKeybind>` (OverviewParts.hpp:200, OverviewState.cpp:69) | `Keybinds::CBind` and `WP<Keybinds::CBind>` (`keybinds/Bind.hpp`) |
| K3 | `KB->key`, `displayKey`, `handler`, `arg`, `description`, `submap.name` | `KB->keyNames().back()` (`std::span<const std::string>`). For the rest use `KB->metadata()`, an `SBindMetadata{displayKey, optional<string> description, handler, argument, submap, submapReset}`, so `metadata().submap` is a plain string |
| K4 | `KB->modmask`, `mouse`, `ignoreMods`, `enabled` | `KB->modifierMask()` (`Input::ModifierMask`), `KB->hasFlag(Keybinds::BIND_FLAG_MOUSE)`, `hasFlag(BIND_FLAG_IGNORE_MODS)`, `KB->enabled()` |
| K5 | Set `m_currentKeybind`, then call `callLuaFn` | `KB->invoke()`. Signature: `SBindResult invoke() const;`. Save and restore `Config::Actions::state()->m_passPressed` as before (CActionState is unchanged, in `<hyprland/src/config/shared/actions/ConfigActions.hpp>`) |
| K6 | `m_currentKeybind != nullptr` ("running inside a bind": HoverActions.cpp:106, DesktopSwitch.cpp:160, OverviewLifecycle.cpp:72/266, OverviewHit.cpp:755, ConfigDispatch.cpp:271) | `Config::Actions::state()->m_bindInvocationDepth > 0`. `invokeBind` increments it around every bind call (keybinds/Manager.cpp:617) |
| K7 | `m_currentKeybind->key` (the trigger key) | There is no public bind pointer. Use `Config::Actions::state()->m_lastCode` (keycode, 0 if mouse) or `m_lastMouseCode` (button), both set before invoke (Manager.cpp:220/352). For a key *name* see N2 |
| K8 | `getCurrentSubmap().name` | `Keybinds::mgr()->currentSubmap()`. Signature: `std::string_view currentSubmap() const;` |
| K9 | `stringToModMask(std::string)` | `Keybinds::modMaskFromString(std::string modifiers)` returns `Input::ModifierMask` (`<hyprland/src/keybinds/Resolver.hpp>`). Assign it to `Input::ModifierMask`, or cast with `sc<uint32_t>(mask)` |
| K10 | `m_groupsLocked` (overview/Window.cpp:950/1027) | `Desktop::windowState()->groupsLocked()` (`<hyprland/src/desktop/state/WindowState.hpp>`). Hyprland uses the same call in CHyprGroupBarDecoration.cpp:168 |
| K11 | `m_dispatchers.find(name)->second(arg)` (OverviewGesture.cpp:533/627, HotCorner.cpp:136, MosaicGestures.cpp:981/999) | No map exists. See N1 |

`Input::ModifierMask` is a class defined with `EXPOSE_ENUM_AS_MASK`:
- It constructs implicitly from `eKeyboardModifiers`, and explicitly from integers. `|`, `|=`, `&` and `~` all exist (also on the raw enum pair). The enumerators live in namespace `Input` (`using enum`), so unqualified `HL_MODIFIER_*` needs `Input::` unless HyprCompat adds a using.
- Ternaries like `g_pInputManager ? getModsFromAllKBs() : 0` (DockPointer.cpp:392) do not compile: wrap in `sc<uint32_t>(...)` or use `Input::HL_MODIFIER_NONE`.
- It converts explicitly to `bool` and to integers.
- `&`, `~` and `==` work between masks. There is no `==` with `int`.
- Write `(mods & HL_MODIFIER_META)` as `bool(mods & Input::HL_MODIFIER_META)`, and `== 0` as `== Input::HL_MODIFIER_NONE`.

Iterating binds (replaces SettingsApp.cpp:115-131 and OverviewLifecycle.cpp:1177-1201):
```cpp
#include <hyprland/src/keybinds/Manager.hpp>
for (const auto& KB : Keybinds::mgr()->registry().binds()) {
    const auto& MD  = KB->metadata();
    const auto  ACTION = Config::boundAction(MD.argument);   // ours or nullopt
    const auto  NAMES  = KB->keyNames();
    const std::string KEY = NAMES.empty() ? MD.displayKey : NAMES.back();
    const auto  MODS   = sc<uint32_t>(KB->modifierMask());
    const bool  MOUSE  = KB->hasFlag(Keybinds::BIND_FLAG_MOUSE);
    // MD.submap, MD.description.value_or("")
}
// firing one bind (dispatchSubmapMouseClick):
const auto MODS = g_pInputManager->getModsFromAllKBs();        // Input::ModifierMask now
auto it = std::ranges::find_if(Keybinds::mgr()->registry().binds(), [&](const Keybinds::PBind& b) {
    return b && b->enabled() && !b->keyNames().empty() && b->keyNames().back() == KEYNAME &&
           isOverviewSubmapName(std::string{b->metadata().submap}) &&
           (b->modifierMask() == MODS || b->hasFlag(Keybinds::BIND_FLAG_IGNORE_MODS));
});
if (it == Keybinds::mgr()->registry().binds().end()) return false;
const int PREV = Config::Actions::state()->m_passPressed;
Config::Actions::state()->m_passPressed = 0;
auto restore = Hyprutils::Utils::CScopeGuard([PREV] { Config::Actions::state()->m_passPressed = PREV; });
return (*it)->invoke().success;
```
- In 0.56 the code above filtered on `shadowed`. In 0.57 the shadow set is private (`CKeybindManager::m_shadowed`).
  `enabled()` is the closest public check.
- To get the same behaviour as a real key press (submap reset, release follow-up), you would need the private
  `invokeBind`. `invoke()` skips that bookkeeping, just as `callLuaFn` did in 0.56.

## 7. Input, seat, event loop, focus

- Every member Hyprdrive uses on `Desktop::focusState()`, `g_pSeatManager`, `g_pEventLoopManager` and
  `g_pPointerManager` still exists with the same signature.
- For `g_pInputManager`, everything is unchanged except the two members below.

| # | OLD | NEW |
|---|-----|-----|
| S1 | `g_pInputManager->getModsFromAllKBs()` returned `uint32_t` | `Input::ModifierMask getModsFromAllKBs();` (`<hyprland/src/managers/input/InputManager.hpp>`). See the mask rules in §6. Call sites: OverviewStripCleanup.cpp:104, OverviewInput.cpp:898, OverviewGesture.cpp:692, OverviewStrip.cpp:1401/1570, OverviewLifecycle.cpp:1175/1337/1379, MosaicGestures.cpp:130/969 |
| S2 | `g_pInputManager->m_exclusiveLSes` (DebugCommands.cpp:222-224; hidden by the fatal include) | `std::vector<PHLLSREF> m_exclusiveKeyboardLSes;`, which is the same vector renamed. LayerSurface.cpp:214/257 and FocusState.cpp:111 use it |

## No direct replacement

- **N1 Named dispatchers (`m_dispatchers`, `addDispatcherV2`).**
  - What Hyprland 0.57 does: the dispatcher table is gone. Binds and gestures hold Lua callbacks, `hl.dispatch(...)`
    runs `hl.dsp.*` Lua functions, and C++ actions are free functions in `Config::Actions::*` (ConfigActions.hpp).
    `addDispatcherV2` is a stub that returns false.
  - Closest option: keep a Hyprdrive-owned `std::unordered_map<std::string, TDispatcher>`, filled in
    `registerDispatcher` (ConfigDispatch.cpp:390), and have `dispatchNamed`, HotCorner, OverviewGesture and
    MosaicGestures look names up there first.
  - For names that are not Hyprdrive's (Hyprland built-ins from gesture or bind strings), fall back to
    `Config::Lua::mgr()->eval(std::format("return hl.dispatch(hl.dsp.{}({}))", ...))`, or call the matching
    `Config::Actions::*` function.
  - You can still call `addDispatcherV2`, but it does nothing.
- **N2 Trigger key name of the running bind (`m_currentKeybind->key`).** 0.57 keeps only keycodes in
  `CActionState::m_lastCode` and `m_lastMouseCode`, while `invokeBind`'s bind is private. Closest options:
  - Map `m_lastMouseCode` to `"mouse:<n>"`.
  - Translate `m_lastCode` through the keyboard keymap with `xkb_state_key_get_one_sym` and `xkb_keysym_get_name`,
    using `g_pSeatManager->m_keyboard`.
  - Or set a Hyprdrive thread-local "current bind" inside Hyprdrive's own Lua functions, which run inside
    `invoke()`.
- **N3 `CDispatcherTrackpadGesture` (`managers/input/trackpad/gestures/DispatcherGesture.hpp`, removed).**
  - This is the fatal include in PluginParts.hpp:47; GestureBinds.cpp:140 uses it.
  - What 0.57 offers instead: `CLuaFunctionGesture(Config::Lua::CLuaRef&& fn)` or `(start, update, end)`, in
    `gestures/LuaFunctionGesture.hpp`. It calls the ref with `LUA_TIMEOUT_KEYBIND_CALLBACK_MS`.
  - Closest option: build a `CLuaRef(mgr->luaStateLifetime(), L, idx)` for a Hyprdrive Lua function that dispatches
    through N1, or subclass `ITrackpadGesture` with a C++ `end()` that calls the N1 registry.
- **N4 Bind "shadowed" state.** It is private to `CKeybindManager`. Use `enabled()`, and accept that a bind
  shadowed by a longer chord may still match.
