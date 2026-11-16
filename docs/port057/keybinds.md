# Port 0.56.2 -> 0.57.2: keybinds and input

Scope: `g_pKeybindManager` / `SKeybind` / dispatchers-by-name / modifier masks / trackpad gestures.
Headers: `<SDK>/include/hyprland/src`. Hyprland's own use: `<hyprland-0.57.2 source>/src`.
`HyprCompat.hpp` already has `using enum Input::eKeyboardModifiers;`, so bare `HL_MODIFIER_META` etc. still resolve
(but are now enum values, not `uint32_t`).

## 1. Manager, registry, submap

| # | OLD (0.56) | NEW (0.57) |
|---|---|---|
| 1 | `#include <hyprland/src/managers/KeybindManager.hpp>` / `keybinds/Manager.hpp` | `#include <hyprland/src/keybinds/Manager.hpp>` (pulls `Bind.hpp`, `Registry.hpp`, `InputState.hpp`, `Key.hpp`). For `modMaskFromString` also `keybinds/Resolver.hpp`. |
| 2 | `g_pKeybindManager` (raw `UP<CKeybindManager>` global) | `Keybinds::mgr()` -> `UP<CKeybindManager>&` (`UP<CKeybindManager>& mgr();`). Null-check as before: `if (!Keybinds::mgr()) ...` |
| 3 | `g_pKeybindManager->m_keybinds` (`std::vector<SP<SKeybind>>`) | `Keybinds::mgr()->registry().binds()` -> `std::span<const PBind> binds() const;` (`using PBind = SP<CBind>;`). Read-only span; add/remove only through `addBind`/`removeBind`. `std::ranges::any_of/find_if` work unchanged on the span. |
| 4 | `SP<SKeybind>` / `WP<SKeybind>` | `Keybinds::PBind` / `WP<Keybinds::CBind>`. `CBind` is move-only, all data private behind getters (table 2). |
| 5 | `g_pKeybindManager->getCurrentSubmap().name` (`SSubmap`) | `Keybinds::mgr()->currentSubmap()` -> `std::string_view currentSubmap() const;` (it returns `Config::Actions::state()->m_currentSubmap`). Copy into `std::string` before storing (`previousSubmapName = std::string{...}`). Default submap is `""`. |
| 6 | `g_pKeybindManager->m_groupsLocked` | `#include <hyprland/src/desktop/state/WindowState.hpp>` -> `Desktop::windowState()->groupsLocked()` (`bool groupsLocked() const;`). Same line Hyprland uses in `CHyprGroupBarDecoration.cpp:168`. |
| 7 | `g_pKeybindManager->stringToModMask(std::string)` -> `uint32_t` | `Keybinds::modMaskFromString(std::string)` -> `Input::ModifierMask` (`Input::ModifierMask modMaskFromString(std::string modifiers);`, `keybinds/Resolver.hpp`). Free function, substring match on SHIFT/CAPS/CTRL/CONTROL/ALT/MOD1-5/SUPER/WIN/LOGO/META, case-insensitive. Single name: `std::optional<Input::eKeyboardModifiers> modifierFromString(std::string_view);` |

## 2. SKeybind fields -> CBind getters

`#include <hyprland/src/keybinds/Bind.hpp>`; flags: `bool hasFlag(eBindFlags flag) const;`

| OLD field | NEW | Note |
|---|---|---|
| `k->enabled` | `k->enabled()` | `void setEnabled(bool)` exists |
| `k->key` (`std::string`) | `k->keyNames()` -> `std::span<const std::string>` | non-modifier keys only, spelled as the user wrote them (`"W"`, `"mouse:272"`, `"mouse_down"`, `"catchall"`). A bind may be a chord (size > 1). Old `key` == `keyNames().size()==1 ? keyNames()[0] : ""`. Compare case-insensitively (or via `xkb_keysym_from_name(..., XKB_KEYSYM_CASE_INSENSITIVE)` as now). |
| `k->displayKey` | `k->metadata().displayKey` | full string e.g. `"SUPER + W"` as given to `hl.bind` |
| `k->modmask` (`uint32_t`) | `k->modifierMask()` -> `Input::ModifierMask` | compare with a `ModifierMask`, not an int (section 3) |
| `k->mouse` | `k->hasFlag(Keybinds::BIND_FLAG_MOUSE)` | `bindm`/`{mouse=true}` |
| `k->release` | `k->hasFlag(Keybinds::BIND_FLAG_RELEASE)` | also set by `click`/`drag` |
| `k->nonConsuming` (read) | `k->hasFlag(Keybinds::BIND_FLAG_NON_CONSUMING)` | write: see No direct replacement |
| `k->submapUniversal` (read) | `k->hasFlag(Keybinds::BIND_FLAG_SUBMAP_UNIVERSAL)` | write: see No direct replacement |
| `k->catchAll` | `k->hasFlag(Keybinds::BIND_FLAG_CATCH_ALL)` | |
| `k->ignoreMods` | `k->hasFlag(Keybinds::BIND_FLAG_IGNORE_MODS)` | |
| `k->submap.name` | `k->metadata().submap` (`std::string`) | `""` = global |
| `k->description` | `k->metadata().description.value_or("")` | now `std::optional<std::string>` |
| `k->handler` | `k->metadata().handler` | **content changed**: `luaL_tolstring` of arg 2 of `hl.bind`: `"HL.Dispatcher(close)"`, `"HL.Dispatcher(exec_cmd)"` (leaf name only) or `"function: 0x..."`. Never `"__lua"`, `"killactive"`, `"exec"`. |
| `k->arg` | `k->metadata().argument` | always the Lua registry ref of the callback (`std::to_string(CLuaRef::id())`). Still numeric, so `Config::boundAction(argument)` keeps working. The exec command line is NOT in it. |
| `k->shadowed` | `Keybinds::mgr()->m_shadowed.contains(WP<CBind>{k})` | `std::unordered_set<WP<CBind>> m_shadowed` is private: needs the file's `#define private public` around `keybinds/Manager.hpp` |

Close-bind detection in `OverviewInput.cpp:869`: old `handler == "killactive"/"closewindow"` becomes
`handler == "HL.Dispatcher(close)" || handler == "HL.Dispatcher(kill)"`; keep the description fallback for `function:` binds.

### Iterating binds (shape for `bindFor`, `fireMatchingKeybind`, `OverviewInput.cpp:864`)
```cpp
#include <hyprland/src/keybinds/Manager.hpp>
using Keybinds::BIND_FLAG_MOUSE; using Keybinds::BIND_FLAG_RELEASE;

Keybinds::PBind bindFor(Input::ModifierMask mods, xkb_keysym_t sym) {
    if (!Keybinds::mgr()) return nullptr;
    for (const auto& K : Keybinds::mgr()->registry().binds()) {
        if (!K || !K->enabled() || K->hasFlag(BIND_FLAG_MOUSE) || K->hasFlag(BIND_FLAG_RELEASE) ||
            !K->metadata().submap.empty() || K->modifierMask() != mods || K->keyNames().size() != 1)
            continue;
        if (xkb_keysym_from_name(K->keyNames()[0].c_str(), XKB_KEYSYM_CASE_INSENSITIVE) == sym)
            return K;
    }
    return nullptr;
}
```

### Running a bind (replaces `m_dispatchers.find(k->handler)` + `IT->second(k->arg)`, and `callLuaFn(index)`)
| OLD | NEW |
|---|---|
| `g_pKeybindManager->m_dispatchers.find(k->handler)->second(k->arg)` | `Keybinds::SBindResult r = k->invoke();` (`SBindResult invoke() const;`). Runs the bind's own Lua callback. `r.success`, `r.error`, `r.passEvent`. |
| set `m_currentKeybind`, zero `m_passPressed`, `MGR->callLuaFn(*INDEX)` (`OverviewLifecycle.cpp:1190-1200`) | Preferred: `Keybinds::mgr()->invokeBind(KEYBIND, /*pressed*/ true);` (private `SBindResult invokeBind(const PBind& bind, bool pressed, SPressedInput* pressedInput = nullptr);`, needs `#define private public`). It sets `m_passPressed`, `m_bindInvocationDepth`, applies `submapReset`, exactly as a real key press. Public fallback: save/set `Config::Actions::state()->m_passPressed` yourself and call `KEYBIND->invoke()`. `callLuaFn` no longer exists. |
| `if (!k->nonConsuming) break;` | `if (!k->hasFlag(Keybinds::BIND_FLAG_NON_CONSUMING)) break;` |

## 3. Modifier masks

`#include <hyprland/src/input/Keys.hpp>` (namespace `Input`). `enum class eKeyboardModifiers : uint8_t` + `EXPOSE_ENUM_AS_MASK(eKeyboardModifiers, ModifierMask)` (`macros/Enums.hpp`).
Enum-only `|`, `&`, `~` also work on bare `HL_MODIFIER_*` values (`HL_MODIFIER_CAPS | HL_MODIFIER_MOD2`, KeyRouter.cpp:100 compiles unchanged once `getModsFromAllKBs()` returns a mask). `ModifierMask` provides: implicit ctor from the enum, `explicit` ctor from any integer, `explicit operator bool`, `explicit operator T()` for integers, `value()`, `==`, `&`, `|`, `~`, `&=`, `|=`. No `!=`/`==` against int, no implicit conversion to `uint32_t`.

| OLD | NEW |
|---|---|
| `uint32_t MODS = g_pInputManager->getModsFromAllKBs();` | `Input::ModifierMask MODS = g_pInputManager->getModsFromAllKBs();` (`Input::ModifierMask getModsFromAllKBs();`). Same for `IKeyboard::getModifiers()`, `xkbModsToHyprland(...)`. |
| `if (MODS & HL_MODIFIER_META)` | unchanged (contextual bool). `bool b = x && (MODS & META)` also fine. Plain `bool b = MODS & META;` needs `static_cast<bool>(...)`. |
| `(MODS & X) != 0` / `MODS != 0` / `modMask == 0` (GestureBinds.cpp:54) / `WANT != 0` (DesktopSwitch.cpp:365) | `static_cast<bool>(MODS & X)` / `MODS != Input::HL_MODIFIER_NONE` (C++20 synthesises `!=` from `==`); `== 0` becomes `== Input::HL_MODIFIER_NONE`. A bare int never converts implicitly. |
| `mods == HL_MODIFIER_META` with `uint32_t mods` | make `mods` a `ModifierMask`, or `mods == static_cast<uint32_t>(Input::ModifierMask{HL_MODIFIER_META})` |
| `cond ? HL_MODIFIER_META : 0` (DebugGesture.cpp:110, DockPointer.cpp:392, OverviewGesture.cpp:692) | `cond ? Input::ModifierMask{HL_MODIFIER_META} : Input::ModifierMask{HL_MODIFIER_NONE}` |
| `uint32_t` stored / logged / passed to `%u` / `std::pair<uint32_t,const char*>` tables (SettingsApp.cpp:97) | store `Input::ModifierMask`; when an int is needed: `static_cast<uint32_t>(mask)`; table: `std::pair<Input::eKeyboardModifiers, const char*>` and test with `if (mask & entry.first)` |
| `uint32_t` from config/int -> mask (also when passing a `uint32_t` variable to `addGesture`/`removeGesture`/`pass`, GestureBinds.cpp:50/59) | `Input::ModifierMask{u32}` (explicit) |
| `static_assert(MOD_SHIFT == HL_MODIFIER_SHIFT ...)` (OverviewLifecycle.cpp:1094) | `MOD_SHIFT == static_cast<uint32_t>(Input::ModifierMask{HL_MODIFIER_SHIFT})` (all `constexpr`) |
| `Config::Actions::pass(uint32_t mods, uint32_t key, std::nullopt)` (OverviewGesture.cpp:865) | `ActionResult pass(Input::ModifierMask modMask, uint32_t key, std::optional<PHLWINDOW> window = std::nullopt);` (`config/shared/actions/ConfigActions.hpp`) - pass a `ModifierMask`. |

Bit values are unchanged (SHIFT=1, CAPS=2, CTRL=4, ALT=8, MOD2=16, MOD3=32, META=64, MOD5=128).

## 4. Trackpad gestures

| OLD | NEW |
|---|---|
| `#include <hyprland/src/managers/input/trackpad/gestures/DispatcherGesture.hpp>` (PluginParts.hpp:47) | Delete the include. Hyprdrive never instantiates `CDispatcherTrackpadGesture`. Config-defined gestures are now `CLuaFunctionGesture` (`gestures/LuaFunctionGesture.hpp`: `explicit CLuaFunctionGesture(Config::Lua::CLuaRef&& fn);` and a 3-ref start/update/end ctor), made by `hl.gesture` in `config/lua/bindings/LuaBindingsConfigRules.cpp:901-903` (cursor zoom gesture: line 927-928). Hyprdrive's own gestures keep subclassing `ITrackpadGesture`. |
| `ITrackpadGesture` (`begin/update/end`, `STrackpadGestureBegin/Update/End{swipe,pinch,direction,scale}`, `distance`, `isDirectionSensitive`, `m_lastPinchScale`, `m_scale`) | **Unchanged** (`gestures/ITrackpadGesture.hpp`). No edit needed in `COverviewGesture` etc. |
| `g_pTrackpadGestures->addGesture(UP<ITrackpadGesture>&&, size_t, eTrackpadGestureDirection, uint32_t modMask, float, bool)` | `std::expected<void, std::string> addGesture(UP<ITrackpadGesture>&& gesture, size_t fingerCount, eTrackpadGestureDirection direction, Input::ModifierMask modMask, float deltaScale, bool disableInhibit);` |
| `removeGesture(size_t, dir, uint32_t, float, bool)` | `std::expected<void, std::string> removeGesture(size_t fingerCount, eTrackpadGestureDirection direction, Input::ModifierMask modMask, float deltaScale, bool disableInhibit);` |
| `clearGestures()`, `gestureBegin/Update/End(SSwipe*/SPinch*)`, `dirForString`, `stringForDir`, `g_pTrackpadGestures` | unchanged |
| `m_activeGesture`, `m_gestureFindFailed`, `SGestureData::{gesture,currentDirection,deltaScale,...}` | still **private**, same names; `SGestureData::modMask` is now `Input::ModifierMask`. Existing `#define private public` around `TrackpadGestures.hpp` keeps working. Any `modMask` compare inside GestureDrive must use section 3 rules. |
| `CCursorZoomTrackpadGesture` (if used for `cursorzoom`) | `gestures/CursorZoomGesture.hpp`: `CCursorZoomTrackpadGesture(const std::string& zoomLevel, const std::string& mode);` |

## 5. Calling a dispatcher by name

`HyprlandAPI::addDispatcherV2` / `removeDispatcher` still compile but are stubs in 0.57.2 (`plugins/PluginAPI.cpp:202`: `return false;`): there is no name -> function table. `hyprdrive:*` names registered through it are never findable. Replace each call site:

| OLD (`dispatchNamed` / `m_dispatchers.find(name)`) | NEW (`config/shared/actions/ConfigActions.hpp`, namespace `Config::Actions`, all return `ActionResult = std::expected<SActionResult, SActionError>`) |
|---|---|
| `"hyprdrive:overview", "toggle all"` (HotCorner.cpp:136, OverviewGesture.cpp:642) | call Hyprdrive's own handler directly (`onOverviewDispatcher("toggle all")` / the `TDispatcher` behind `registerDispatcher`), inside `HoverActions::SOwnCall own;` as the V2 wrapper did |
| `"killactive", ""` | `Config::Actions::closeWindow();` (`killWindow()` = force kill) |
| `"fullscreen", "0"` / `"1"` | `Config::Actions::fullscreenWindow(Fullscreen::FSMODE_FULLSCREEN, /*layoutAware*/ false);` / `FSMODE_MAXIMIZED`. Toggles (unsets if already in that mode). Enum in `managers/fullscreen/FullscreenTypes.hpp`. |
| `"togglefloating", ""` | `Config::Actions::floatWindow(Config::Actions::TOGGLE_ACTION_TOGGLE);` |
| `"workspace", "e+1"` | `Config::Actions::changeWorkspace(std::string{"e+1"});` (`ActionResult changeWorkspace(const std::string& ws);`) |
| `"togglespecialworkspace", name` | `Config::Actions::toggleSpecial(PHLWORKSPACE)`; resolve/create the workspace first exactly as `dsp_toggleSpecial` in `config/lua/bindings/LuaBindingsDispatchers.cpp:1182` does (workspace resolver is the workspace-area doc's subject) |
| arbitrary user `name, data` (OverviewGesture.cpp:680) | Lua: `#include <hyprland/src/config/lua/ConfigManager.hpp>`; `Config::Lua::mgr()` returns `WP<CConfigManager>` (check it is non-null/lockable); `Config::Lua::mgr()->eval(std::format("return hl.dispatch({})", luaExpr))` (`std::optional<std::string> eval(const std::string& code, bool repl = false);` returns the Lua result/error text, nullopt only when empty). This is what `hyprctl dispatch` does (`dispatchRequest`, `ipc/s1/Commands.cpp:~1332`, which uses `"return hl.dispatch({})"` and `Config::mgr()->type() == Config::CONFIG_LUA`). User strings must now be Lua dispatcher expressions (`hl.dsp.window.close()`); the old `name arg` syntax has no parser. |

Session-lock guard (`hyprdriveSessionLocked()`) stays in front of every path above.

## No direct replacement

0. **`"cursorzoom"` dispatcher** (OverviewGesture.cpp:686): no `hl.dsp` entry or `Config::Actions` function for zoom exists in 0.57.2 (grep of `LuaBindingsDispatchers.cpp`, `ConfigActions.hpp`). Zoom is only the config value `cursor:zoom_factor` (`config/values/ConfigValues.cpp:713`, 1-10) and `CCursorZoomTrackpadGesture`. Closest: set `cursor:zoom_factor` through the config API, or `eval("hl.config({cursor={zoom_factor=N}})")` (unverified call shape).

1. **`m_currentKeybind`** (OverviewLifecycle.cpp:72/266, DesktopSwitch.cpp:160/168, OverviewHit.cpp:755, HoverActions.cpp:106, ConfigDispatch.cpp:271).
   Hyprland keeps no "current bind" pointer. Internally `CKeybindManager::invokeBind` (private, Manager.cpp:611) only bumps
   `Config::Actions::state()->m_bindInvocationDepth`, sets `m_passPressed` (restored on exit) and resets `m_requestBindRelease`. `m_lastCode`/`m_lastMouseCode`/`m_timeLastMs` are set by the key/mouse event handlers (Manager.cpp:220, 352), not by `invokeBind`. All of `state()` needs `#include <hyprland/src/config/shared/actions/ConfigActions.hpp>`.
   - "Is a bind running?" -> `Config::Actions::state()->m_bindInvocationDepth > 0` (public; `UP<CActionState>& state();`).
   - ConfigDispatch.cpp:271 `handler == "__lua"` test -> the same depth test (every bind is Lua now).
   - "Was it a mouse button bind?" (`key.starts_with("mouse:")`) -> `state()->m_lastMouseCode != 0 && state()->m_lastCode == 0` (set in `onMouseEvent`, Manager.cpp:352).
   - "Was it a wheel bind?" (`mouse_up/down/left/right`): `onAxisEvent` (Manager.cpp:312) sets none of those fields. Closest:
     hook `Keybinds::CKeybindManager::invokeBind` with `HyprlandAPI::findFunctionsByName(handle, "invokeBind")` +
     `createFunctionHook`, store the `PBind` in a Hyprdrive-owned `currentBind()` for the call's duration, and read
     `keyNames()` from it. This one hook restores every `m_currentKeybind` use above (also the save/set/restore at
     OverviewLifecycle.cpp:1190, which then becomes unnecessary when `invokeBind` is called directly).
2. **Writing `keybind->nonConsuming = true`** (OverviewLifecycle.cpp:100) and **`keybind->submapUniversal = true/false`**
   (borrow/return passthrough binds, OverviewLifecycle.cpp:1098-1116). `CBind` flags are private and immutable
   (`BindFlags m_flags`, no setter); Hyprland reads them live (`hasFlag` in Manager.cpp:535/589, `Bind.cpp:226` for
   SUBMAP_UNIVERSAL), so flipping the bit takes effect immediately. Closest: `#define private public` around
   `keybinds/Bind.hpp` and `K->m_flags |= Keybinds::BIND_FLAG_SUBMAP_UNIVERSAL;` / `&= ~...`; remember the bits Hyprdrive
   set in `g_passthroughBinds` (now `std::vector<WP<Keybinds::CBind>>`) and clear only those. Rebuilding the bind is not
   possible: `CBind::make` needs the original `BindCallback`, which is private (`m_callback`).
3. **Exec command of a bind** (`isScreenshotBind` reads `k->arg`, MosaicGestures.cpp:934). `argument` is now the Lua ref,
   the command lives inside the Lua closure. Closest: match on `metadata().description` and `metadata().displayKey`
   only; optionally push the ref (`lua_rawgeti(L, LUA_REGISTRYINDEX, ref)`) and inspect upvalues of the
   `HL.Dispatcher(exec_cmd)` userdata, which Hyprland does not expose.
4. **Name -> dispatcher table** (`m_dispatchers`, and `addDispatcherV2` being a no-op): see section 5; internal state is
   Lua tables under `hl.dsp.*` (`registerDispatcherBindings`, LuaBindingsDispatchers.cpp:1320). Hyprdrive's own
   dispatchers must be reached by direct C++ call or via `hl.plugin.<ns>.<name>` (already registered with `addLuaFunction`).
5. **`keybind->shadowed`** (OverviewLifecycle.cpp:1177): only reachable through private `CKeybindManager::m_shadowed`
   (see table 2); no public query.
