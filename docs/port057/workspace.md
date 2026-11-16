# Port 0.56.2 -> 0.57.2: workspaces and monitors

Scope: workspace lookup, creation, rename, renumber, visibility, special workspaces, workspace
animations, monitor workspace state, `g_pCompositor` members. Only symbols Hyprdrive uses.
Headers are relative to `<hyprland/src/...>`. Error lines are from the port's first build log (not kept in the tree).

Background: `CWorkspace` (0.56, `desktop/Workspace.hpp`) is now `Workspace::CHLWorkspace`
(`workspace/HLWorkspace.hpp`), with subclasses `CRegularWorkspace` / `CSpecialWorkspace`.
Its id is `Workspace::WorkspaceID = std::variant<SWorkspaceNumberedID, SWorkspaceSpecialID>`
(`workspace/AbstractWorkspace.hpp`); numbered ids are `uint32_t` (`WorkspaceIDContainer`),
special workspaces have NO number, only an address (`"special:minimized"`).
`Hyprdrive::Compat::id(ws)` (src/core/HyprCompat.hpp) gives Hyprdrive's integer key; use it
everywhere Hyprdrive read `ws->m_id`.

## 1. Workspace identity and fields

| OLD (0.56) | NEW (0.57) | Notes |
|---|---|---|
| `ws->m_id` (read) | `Hyprdrive::Compat::id(ws)` | `m_id` is private. Raw API: `WorkspaceID id() const override;` and `std::optional<WorkspaceIDContainer> numberedID() const;` (nullopt for special). Sites: DesktopOrder.cpp:212,216,219,225,227,232; Rows.cpp:223,255; SpringLoad.cpp:177; FullscreenDesktop.cpp:360,501,1385; OverviewLifecycle.cpp:778. |
| `ws->m_id = TO` / `Compat::id(WS) = TO` (DesktopOrder.cpp:120, lvalue error) | `void changeID(SWorkspaceNumberedID id);` | See 1a. Numbered workspaces only; refuses `id 0` and special workspaces silently. |
| `ws->m_name = X` (DesktopOrder.cpp:123) | `void rename(const std::string& name = "");` | `m_name` private. `rename` is a no-op on special workspaces, sets the private `m_wasRenamed`, schedules a config refresh, posts IPC `renameworkspace` and emits `m_events.renamed` + `Event::bus()->m_events.workspace.renamed`. Read the name with `const std::string& displayName() const`. |
| `ws->m_name` (read) | `ws->displayName()` | already used by most of src/. `addressableName()` is the lookup key ("3", "special:minimized", a named ws's name). |
| `ws->m_visible = b` (DockPreview.cpp:125,151; OverviewCards.cpp:26,30,168,172) | `void setVisible(bool visible);` / `bool visible() const;` | Plain setter, no side effects (same as the 0.56 field write). Save with `visible()`, restore with `setVisible(saved)`. |
| `ws->isPersistent()` (OverviewLifecycle.cpp:767) | `workspace/RegularWorkspace.hpp`: `bool isPersistent() const;` on `Workspace::CRegularWorkspace` | Not on `CHLWorkspace` any more. `const auto R = dynamicPointerCast<Workspace::CRegularWorkspace>(ws); const bool PERSISTENT = R && R->isPersistent();` (Hyprland does the same cast in ipc/s1/Commands.cpp:603 and state/workspace/PlacementController.cpp:117; `State::clear()` casts only to call `setPersistent`). Special workspaces are never persistent. |
| `ws->m_monitor`, `ws->getWindowCount(...)`, `ws->m_events.*` | unchanged on `CHLWorkspace` | `PHLMONITORREF m_monitor` is still public. New signals: `renamed`, `idChanged`, `monitorChanged`, `activeChanged`. |
| `ws->m_renderOffset`, `ws->m_alpha` | unchanged, now inherited from `workspace/presentation/WorkspacePresentable.hpp` | `PHLANIMVAR<Vector2D> m_renderOffset; PHLANIMVAR<float> m_alpha;` public. Overview/Window.cpp and OverviewGesture.cpp compile as is. |
| special check | `Hyprdrive::Compat::isSpecial(ws)` (`ws->type() == Workspace::eWorkspaceType::SPECIAL`) | already in src/. |

### 1a. Renumbering in place (DesktopOrder.cpp `renumber`, Rows::renumbering)

`changeID` (workspace/HLWorkspace.cpp:299) does, in order: set `m_id`; set
`m_addressableName = to_string(id)`; if the workspace was never `rename()`d set
`m_name = to_string(id)`; schedule `REFRESH_ALL`; tell the lifecycle policy and the workspace
history tracker (so `workspace previous` and remembered per-monitor workspaces follow the
renumber, which 0.56 could not do); post IPC `changeworkspaceid` with `"OLD,NEW"`; emit
`m_events.idChanged`. It does NOT check for a duplicate number, so applying the whole plan
synchronously (as `renumber` does) is fine; nothing queries in between.

```cpp
#include <hyprland/src/workspace/HLWorkspace.hpp>
for (const auto& [WS, TO] : moves) {
    const WORKSPACEID FROM = Hyprdrive::Compat::id(WS);
    if (FROM == TO || TO <= 0 || !WS->numberedID())
        continue;
    const bool NUMERIC = WS->displayName() == std::to_string(FROM);
    WS->changeID(Workspace::SWorkspaceNumberedID{sc<Workspace::WorkspaceIDContainer>(TO)});
    // changeID already renames never-renamed workspaces; this only covers one the user renamed to its own number.
    if (NUMERIC && WS->displayName() != std::to_string(TO))
        WS->rename(std::to_string(TO));
}
```
Behaviour delta: Hyprland now emits `changeworkspaceid` itself per workspace. Hyprdrive's own
destroy/create/workspace IPC burst after the renumber (DesktopOrder.cpp:130-144) may now be
redundant; that is the IPC area's call (`g_pEventManager` is gone, see the IPC doc).

## 2. Lookup and iteration

| OLD | NEW | Notes |
|---|---|---|
| `State::workspaceState()->workspaces()` | unchanged: `state/WorkspaceState.hpp` -> `State::workspaceState()` returns `UP<State::Workspace::CState>&`; `auto workspaces() const` (filtered view of `PHLWORKSPACEREF`, `.lock()` each) | `std::vector<PHLWORKSPACE> workspacesCopy() const;` if you mutate while iterating. Hyprdrive's hand loops (Minimize.cpp `workspaceByID`, FullscreenDesktop.cpp `byId`, OverviewWorkspaces.cpp `workspaceWithID`, DockActivate.cpp:522) keep working via `Compat::id`. |
| lookup by number (0.56 `query()` / hand loop) | `state/workspace/StateTracker.hpp`: `CQuery query() const;` + `state/workspace/Query.hpp`: `CQuery&& numbered(SWorkspaceNumberedID id) &&; CQuery&& address(std::string_view address) &&; CQuery&& input(std::string_view input) &&; PHLWORKSPACE run() &&;` | `State::workspaceState()->query().numbered(Workspace::SWorkspaceNumberedID{sc<uint32_t>(id)}).run()` (Hyprland: state/workspace/Resolver.cpp:174). By name/special: `.address("special:minimized").run()`. Optional; the loops are fine. |
| lookup by STarget | `PHLWORKSPACE find(const STarget& target) const;` (state/workspace/State.hpp) | returns null when `!target.valid()`. |
| `getWorkspaceIDNameFromString(str)` -> `SWorkspaceIDName{id,name}` (DockActivate.cpp:403) | `state/workspace/Resolver.hpp`: `STarget getWorkspaceTargetFromString(const std::string&, std::optional<PHLMONITOR> = std::nullopt);` on `State::Workspace::resolver()` | Returns `State::Workspace::STarget` (`state/workspace/Target.hpp`): `std::optional<WorkspaceID> id; std::string address; std::string displayName; eWorkspaceType type;` + `valid()`. Pass the monitor explicitly (default is the focused monitor). See snippet. |
| `g_pCompositor->getWorkspaceByID/ByName`, `m_workspaces` | not used by Hyprdrive any more (src/ already on `State::workspaceState()`) | |

```cpp
#include <hyprland/src/state/workspace/Resolver.hpp>
const auto  T   = State::Workspace::resolver()->getWorkspaceTargetFromString("emptym", monitor);
const auto* NUM = T.valid() ? std::get_if<Workspace::SWorkspaceNumberedID>(&*T.id) : nullptr;
const WORKSPACEID TARGETID = NUM ? sc<WORKSPACEID>(NUM->value) : WORKSPACE_INVALID;
// then use TARGETID where the code read TARGET.id (DockActivate.cpp:403-423)
```
`emptym` semantics are unchanged: lowest number with no windows that no workspace rule binds to
another monitor.

## 3. Creating workspaces

0.56: `PHLWORKSPACE create(const WORKSPACEID& id, const MONITORID& monid, const std::string& name = "", bool isEmpty = true);`
0.57 (`state/workspace/State.hpp`, all `[[nodiscard]]`, all take a `PHLMONITOR`, not a MONITORID):

```cpp
PHLWORKSPACE create(const STarget& target, PHLMONITOR monitor, bool isEmpty = true);
PHLWORKSPACE createNumbered(::Workspace::SWorkspaceNumberedID id, PHLMONITOR monitor, std::string displayName = {}, bool isEmpty = true);
PHLWORKSPACE createNamed(std::string address, PHLMONITOR monitor, std::string displayName = {}, bool isEmpty = true);
PHLWORKSPACE createSpecial(std::string address, PHLMONITOR monitor, bool isEmpty = true);
```

| Site | NEW call |
|---|---|
| DockActivate.cpp:528 `create(ID, MONITOR->m_id, to_string(ID))` | `createNumbered(Workspace::SWorkspaceNumberedID{sc<uint32_t>(ID)}, MONITOR)` (default name is already the number) |
| OverviewWorkspaces.cpp:180 | same, with `monitor` |
| FullscreenDesktop.cpp:446,571,776 `create(at, MON->m_id)` | `createNumbered(Workspace::SWorkspaceNumberedID{sc<uint32_t>(at)}, MON)` |
| Minimize.cpp:379,454 `create(IT->workspace, IT->monitor, IT->workspaceName)` | numbered entry: `createNumbered({sc<uint32_t>(id)}, mon, name)`; special entry: `createSpecial(name, mon)`. `IT->monitor` is a MONITORID: resolve it with `State::monitorState()->query().id(IT->monitor).run()` (already used in FlightController.cpp:978). Branch on `IT->workspace < 0` or on `name.starts_with("special:")`. |

Behaviour deltas (state/workspace/State.cpp:84-160):
- Duplicates are now REFUSED (returns null, logs `Refusing duplicate numbered workspace`). 0.56 made a
  second workspace with the same id, which is what OverviewWorkspaces.cpp `createWorkspaceIfFree`
  guards against; the guard stays harmless.
- A workspace rule's bound monitor still overrides the passed monitor (unchanged; keep the
  `CREATED->m_monitor.lock() != monitor` check).
- New workspaces start with `m_alpha` warped to 0 (State.cpp:114,136,159); fade them in as before.
- Null monitor, id 0 or `uint32_t` overflow -> null. Negative Hyprdrive ids must never reach `createNumbered`.

## 4. Special workspaces

| OLD | NEW | Notes |
|---|---|---|
| `workspaceState()->newSpecialID()` + `create(thatID, monId, "special:minimized")` (Minimize.cpp:61, Scratchpad.cpp:213) | `State::workspaceState()->createSpecial("special:minimized", monitor)` / `createSpecial("special:scratchpad", monitor)` | No special ids exist; the address IS the identity. `createSpecial` prefixes `special:` if missing, maps bare `"special"` to `"special:special"`, returns null if one with that address already exists (so look it up first: `query().address(STASH_NAME).run()` or the existing displayName loop). Internally `Workspace::CSpecialWorkspace::create(PHLMONITOR, std::string address, bool isEmpty)` (`workspace/SpecialWorkspace.hpp`). |
| `MONITOR->setSpecialWorkspace(ws)` / `(PHLWORKSPACE{})` (Scratchpad.cpp:223,231) | `void setSpecialWorkspace(const PHLWORKSPACE& pWorkspace, bool noFocus = false);` (`output/Monitor.hpp:275`) | Same behaviour; new optional `noFocus`. The `WORKSPACEID` overload is gone (Hyprdrive does not use it). |
| special `Compat::id` (negative) | hash of `addressableName()`, stable per name, NOT the 0.56 creation-order -99,-98... | Anything persisted to disk with a special id (Minimize entries) should store/compare the address instead. |

## 5. Switching and moving

| OLD | NEW | Notes |
|---|---|---|
| `mon->changeWorkspace(ws, internal, noMouseMove, noFocus)` | unchanged: `void changeWorkspace(const PHLWORKSPACE& pWorkspace, bool internal = false, bool noMouseMove = false, bool noFocus = false);` (`output/Monitor.hpp:274`) | The `WORKSPACEID` overload is gone; Hyprdrive only passes PHLWORKSPACE. |
| `::Config::Actions::changeWorkspace(std::to_string(id))` | unchanged (`config/shared/actions/ConfigActions.hpp:75-76`, overloads `(PHLWORKSPACE)` and `(const std::string&)`), string selector | |
| move a workspace to a monitor | `state/WorkspacePlacementController.hpp` (accessor `State::workspacePlacementController()`; class `State::Workspace::CPlacementController` is declared in `state/workspace/PlacementController.hpp`): `State::workspacePlacementController()->moveWorkspaceToMonitor(PHLWORKSPACE, PHLMONITOR, bool noWarpCursor = false, bool carryFocus = true, bool replace = true) const;` also `swapActiveWorkspaces(PHLMONITOR, PHLMONITOR) const;` | Hyprland: config/supplementary/propRefresher/PropRefresher.cpp:135. Hyprdrive currently does not call it (no hits), listed because the area asked. |
| `Config::workspaceRuleMgr()->getBoundMonitorForWS(std::string)` | unchanged string overload returns `PHLMONITOR`; new `std::optional<PHLMONITOR> getBoundMonitorForWS(const WorkspaceID&, eWorkspaceType, std::string_view address);` | OverviewWorkspaces.cpp:158: the code reads `Hyprdrive::Compat::id(BOUNDTO)` on a monitor (botched rewrite). It must be `BOUNDTO->m_id`. |

## 6. Workspace animation hook (silent runtime break, not a compile error)

`animation/WorkspaceAnimationController.hpp` (unchanged shape):
```cpp
namespace Animation::Workspace {
    enum eAnimationType : uint8_t { ANIMATION_TYPE_IN = 0, ANIMATION_TYPE_OUT };
    void startAnimation(PHLWORKSPACE ws, eAnimationType type, bool left = true, bool instant = false, std::optional<std::string> style = std::nullopt);
}
```
`PHLWORKSPACE` is now `CSharedPointer<Workspace::CHLWorkspace>`, so the mangled name in
DesktopSwitch.cpp:387 no longer exists in the binary. Verified with `nm` on
`<SDK>/bin/Hyprland`:
```
_ZN9Animation9Workspace14startAnimationEN9Hyprutils6Memory14CSharedPointerIN9Workspace12CHLWorkspaceEEENS0_14eAnimationTypeEbbSt8optionalINSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEE
```
Same for HoverActions.cpp's `Config::Actions::moveToWorkspace(PHLWORKSPACE, bool, optional<PHLWINDOW>)`:
```
_ZN6Config7Actions15moveToWorkspaceEN9Hyprutils6Memory14CSharedPointerIN9Workspace12CHLWorkspaceEEEbSt8optionalINS3_IN7Desktop4View7CWindowEEEE
```
Rule: every hook string containing `I10CWorkspaceE` becomes `IN9Workspace12CHLWorkspaceEE`.
Better: resolve with `HyprlandAPI::findFunctionsByName(handle, "startAnimation")` and match the
demangled signature, so the next type rename does not silently disable the hook.
(Also outside this area: `src/layout/OwnSizeTile.cpp` hooks two symbols missing from the 0.57
binary, `Config::Actions::floatWindow(...)` and `CWindow::onX11ConfigureRequest(CBox)`; the window doc owns them.)

## 7. Monitor state

| OLD | NEW | Notes |
|---|---|---|
| `mon->m_activeWorkspace`, `mon->m_activeSpecialWorkspace` | unchanged: `PHLWORKSPACE m_activeWorkspace = nullptr; PHLWORKSPACE m_activeSpecialWorkspace = nullptr;` (`output/Monitor.hpp:76-77`) | Read only; switch through `changeWorkspace` / `setSpecialWorkspace`. `activeWorkspaceID()` / `activeSpecialWorkspaceID()` are gone (unused). |
| `mon->m_id`, `mon->m_enabled`, `mon->m_size` | unchanged | `MONITORID m_id`. |
| `mon->displayName()` (OpenProf.cpp:220, Rows.cpp:218,426, DockLayout.cpp:567, GridLayout.cpp:496-509,887, OverviewFrames.cpp:70, OverviewKeys.cpp:220,253, OverviewLifecycle.cpp:288, OverviewStrip.cpp:388, OverviewStripCleanup.cpp:402) | `mon->m_name` (`std::string`, `output/Monitor.hpp:81`) or `virtual std::string_view name() const override;` | `CMonitor` never had `displayName()`; these are an over-eager `m_name` -> `displayName()` rewrite meant for workspaces. Use `m_name` where a `std::string` key is needed (Rows.cpp `g_blocks.find`). `Window.cpp:530` is `CPlugin`, not a monitor (plugin doc). |
| `State::monitorState()->monitors()` / `->query().id(x)/.vec(p)/.relativeTo(m).inDirection(d).run()` | unchanged (`state/MonitorState.hpp`, `state/MonitorQuery.hpp`) | compiles today. |
| `window->m_monitorMovedFrom = -1` (FullscreenDesktop.cpp:243) | `window->presentation().resetMonitorMovedFrom();` (`desktop/view/window/WindowPresentation.hpp:85`; setter `setMonitorMovedFrom(int)`) | field is now private in `CWindowPresentation`. |

## 8. `g_pCompositor` members Hyprdrive uses

`rg 'g_pCompositor->'` over src/ finds exactly two members; both still exist in `Compositor.hpp`:

| OLD | NEW |
|---|---|
| `g_pCompositor->m_wlEventLoop` (42 uses) | unchanged: `wl_event_loop* m_wlEventLoop = nullptr;` (Compositor.hpp:36) |
| `g_pCompositor->m_isShuttingDown` (12 uses) | unchanged: `bool m_isShuttingDown = false;` (Compositor.hpp:72) |

## 9. Mechanical fixes from the earlier rewrite (not API changes)

- `X->Hyprdrive::Compat::id(member)` -> `Hyprdrive::Compat::id(X->member)`: OverviewHit.cpp:84,111;
  OverviewLifecycle.cpp:468,526,1269; OverviewStrip.cpp:1635 (`images[i]->pWorkspace`);
  GridLayout.cpp:661 (`WINDOWS[i]->m_workspace`).
- HyprCompat.hpp:41: the unconstrained `template <typename T> bool isSpecial(const T&)` wins
  over the `const CHLWorkspace*` overload for a non-const `CHLWorkspace*`, then calls `.get()` on a
  pointer. Constrain it: `template <typename T> requires requires(const T& t) { t.get(); }`.
- `std::format("{}", ws->m_id)`, `std::pair<long,long>{ws->m_id, ...}`, `<`, `-`, `>` on ids
  (DesktopOrder.cpp:212-232, FullscreenDesktop.cpp:360,501, SpringLoad.cpp:177) all go away once
  `m_id` becomes `Compat::id(ws)`. `WorkspaceID` has no formatter or ordering.

## No direct replacement

- **Special workspace integer ids** (`newSpecialID`, negative `WORKSPACEID`): 0.57 identifies
  special workspaces only by address (`SWorkspaceSpecialID{}` + `"special:<name>"`). Closest:
  `createSpecial(address, monitor)` + `query().address(address).run()`; Hyprdrive keeps negative
  keys via `Compat::id`'s name hash, but any stored special id must round-trip through the address.
- **Setting the display name without marking it renamed**: `m_name` and `m_wasRenamed` are
  private. `changeID` updates the name only for never-renamed workspaces; `rename()` always sets
  `m_wasRenamed` (so later `changeID`s stop renaming) and posts `renameworkspace`. Closest: the 1a
  snippet; the residual case (user renamed a desktop to its own number) ends up flagged renamed.
- **Renaming a special workspace**: `rename()` returns early for SPECIAL. Hyprdrive does not need
  it today; a different name means a new `createSpecial` address.
- **Numbering a named/special workspace**: `changeID` only works on numbered workspaces. No
  conversion exists; create a numbered one and move windows.
