# Port 0.56.2 -> 0.57.2

Mapping references: `render.md`, `keybinds.md`, `window.md`, `workspace.md`, `events.md`, `layout.md`.

## Shared helpers (HyprCompat.hpp)

`src/core/HyprCompat.hpp` is force-included first in every TU. It includes all of `<bits/stdc++.h>`, then
opens (`#define private public` / `#define protected public`) every Hyprland header it pulls in: animated
variables (hyprutils + `helpers/AnimatedVariable.hpp`), `HLWorkspace`, `Window` and its parts, `X11Backend`,
`WaylandBackend`, `XSurface`, the decorations + `AnimatedDecorationGradient`, `TransformerList`, `render/pass/Pass`,
`render/OpenGL`, `Shader`, `types`, `keybinds/Bind` + `Manager`, `TrackpadGestures`, `Monitor`, `EventBus`, `S2`,
`WorkspaceState`, both `ConfigManager`s, `ConfigActions`. Private members of these are reachable directly from any
file (no per-file `#define` needed; a per-file one still works for headers HyprCompat does not reach, e.g. `XWM.hpp`,
`MonitorFrameScheduler.hpp`, `DataDevice.hpp`).

### `Hyprdrive::Compat` (existing)
- `WORKSPACEID id(ws)` (`CHLWorkspace*`, `PHLWORKSPACE`, `PHLWORKSPACEREF`): Hyprdrive's integer workspace key (number; special = name hash < -100).
- `bool isSpecial(ws)`: special workspace test (template now constrained to pointer-likes).
- `bool pinned(const PHLWINDOW&)`, `bool urgent(const PHLWINDOW&)`: 0.56 `m_pinned` / `m_isUrgent` reads.

### `Hyprdrive::Compat` (new)
- `PHLWORKSPACE workspaceByID(WORKSPACEID id)`: live workspace with that `Compat::id` (numbered or special), or nullptr.
- `void postEvent(std::string event, std::string data)`: replaces `g_pEventManager->postEvent(SHyprIPCEvent{event, data})`, no-op without socket2.
- `void setBeingAnimated(Hyprutils::Animation::CBaseAnimatedVariable* var, bool animated)`: replaces `av->m_bIsBeingAnimated = b` (`setBeingAnimated(w->positionAnimation().get(), true)`).
- `using TOwnDispatcher = std::function<SDispatchResult(std::string)>;`
- `std::unordered_map<std::string, TOwnDispatcher>& ownDispatchers()`: Hyprdrive's name -> dispatcher table. **`Config::registerDispatcher` (ConfigDispatch.cpp) must insert both full names** (`hyprdrive:<name>` and the legacy spelling, same wrappers it passes to `addDispatcherV2`).
- `std::optional<SDispatchResult> dispatchOwn(const std::string& name, std::string arg)`: replaces `g_pKeybindManager->m_dispatchers.find(name)` for Hyprdrive names (`dispatchNamed`, HotCorner, MosaicGestures); nullopt = not ours.
- `std::optional<std::string> dispatchLua(const std::string& luaExpr)`: fallback for Hyprland built-ins: `dispatchLua("hl.dsp.window.close()")` runs `return hl.dispatch(<expr>)`; nullopt off a Lua config.
- `bool hookInvokeBind()`: hooks `CKeybindManager::invokeBind` (via FunctionHooks). **Must be called once from plugin init (main.cpp)**, otherwise `currentBind()` is always null.
- `Keybinds::PBind currentBind()`: the bind running now (0.56 `g_pKeybindManager->m_currentKeybind`); `currentBind()->keyNames()` for the key, nullptr outside a bind.

### `Hyprdrive::Priv` (private members, no 0.57 accessor)
- `SP<CXWaylandSurface> xsurface(const PHLWINDOW&)`: 0.56 `w->m_xwaylandSurface`; nullptr for Wayland windows.
- `std::optional<Vector2D>& pendingReportedSize(CWindow&)`: 0.56 `m_pendingReportedSize`, either backend (`x>0` tests become `has_value()`).
- `bool& hidden(CWindow&)`: raw `m_hidden` write without `setHidden()` side effects.
- `bool& suspended(CWindow&)`: raw `m_suspended`.
- `PHLANIMVAR<float>& dimPercent(CWindow&)`: 0.56 `w->m_dimPercent` (`isBeingAnimated()`, `warp()`).
- `CAnimatedDecorationGradient* borderGradient(const PHLWINDOW&)`: border colors/fade/angle; `->renderState()` = `{current, previous, progress, transitioning}`, `->m_fadeProgress`, `->m_angleProgress`; nullptr without a border.
- `CAnimatedDecorationGradient* shadowGradient(const PHLWINDOW&)`: same for the drop shadow (`m_realShadowColor`, `m_shadowFadeAnimationProgress`).
- `std::vector<UP<Render::IWindowTransformer>>& transformers(CWindow&)`: 0.56 `w->m_transformers` (erase/move out, `.size()`).
- `void sortTransformers(CWindow&)`: re-sort by priority after re-inserting into `transformers(w)`.
- `void setBindFlag(Keybinds::CBind&, Keybinds::eBindFlags, bool on)`: 0.56 `k->nonConsuming = true` / `k->submapUniversal = b` (`BIND_FLAG_NON_CONSUMING`, `BIND_FLAG_SUBMAP_UNIVERSAL`).

Others reachable directly thanks to the access block (single-file, no helper): `CX11Backend::m_reportedPosition`,
`CWaylandBackend::m_configureAcks`, `CKeybindManager::m_shadowed` / `invokeBind`, `m_passElements`, `m_cmSupported`,
`CTrackpadGestures::m_activeGesture`.

`src/core/PluginParts.hpp`: `DispatcherGesture.hpp` (removed in 0.57) is replaced by `gestures/LuaFunctionGesture.hpp`
(`CLuaFunctionGesture`); `CDispatcherTrackpadGesture` users (GestureBinds.cpp:140) need the events.md N3 rewrite.
