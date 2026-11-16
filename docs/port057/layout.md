# Port 0.56.2 -> 0.57.2: layout, fullscreen, animated variables

Scope: `Layout::*`, `g_layoutManager`, drag controller, `Fullscreen::*`, Hyprutils animated variables,
`Animation::mgr()->createAnimation`, `Config::animationTree()`, window/layer animated members.
Headers: `NEW = <SDK>/include/hyprland/src`. Include as `<hyprland/src/...>`.

## 0. Why the m_realPosition / m_Value / m_bIsBeingAnimated / m_target errors appear (read first)

The headers for these did NOT change: `CGeometricMovableAnimated::m_realPosition/m_realSize` were already
`protected` in 0.56.2, `CGenericAnimatedVariable::m_Value` was already `private`, and the vendored
Hyprutils `AnimatedVariable.hpp` is byte-identical to `/usr/include/hyprutils` 0.14.1.
In 0.56 the TUs got through with `#define private public` / `#define protected public` before their
Hyprland includes. `src/core/HyprCompat.hpp` is now force-included (`-include` in `Makefile:53`) and pulls in
`desktop/view/window/Window.hpp` (and so `GeometricMovableAnimated.hpp`, `helpers/AnimatedVariable.hpp`,
`hyprutils/animation/AnimatedVariable.hpp`) **before** each TU's `#define`, so `#pragma once` keeps the real
access specifiers. Every per-TU `#define private public` is now dead for anything HyprCompat reaches.
Fix below = use the public accessors (they exist for everything except `m_bIsBeingAnimated`).

## 1. Animated variables (Hyprutils `CGenericAnimatedVariable`)

Header: `<hyprutils/animation/AnimatedVariable.hpp>` (unchanged). Public API, copied:

```cpp
const VarType& value() const;          VarType& value();      // mutable ref: writes m_Value, no callbacks, no begun/goal change
const VarType& goal() const;           const VarType& begun() const;
void setValue(const VarType& v);       // m_Value = v; m_Begun = v; (goal untouched)
void setValueAndWarp(const VarType& v);// goal = begun = v, then warp() (fires end callback)
virtual void warp(bool endCallback = true, bool forceDisconnect = true); // no-op unless m_bIsBeingAnimated
bool isBeingAnimated() const;
void setCallbackOnEnd(CallbackFun func, bool remove = true);
void setCallbackOnBegin(CallbackFun func, bool remove = true);
void setUpdateCallback(CallbackFun func);
void resetAllCallbacks();
// operator=(v) on *pav starts an animation to v (goal=v, begun=value)
```

| OLD (0.56 via #define hack) | NEW 0.57 | Note |
|---|---|---|
| `av->m_Value = X;` | `av->value() = X;` | identical semantics (raw write, nothing else touched). Already used in `MosaicDrive.cpp:28`, `overview/Window.cpp:130`. |
| `av->m_Value` (read) | `av->value()` | |
| `av->m_Goal` / `av->m_Begun` | `av->goal()` / `av->begun()` | read-only; to change goal use `*av = X` or `setValueAndWarp`. |
| `av->m_bIsBeingAnimated` (read) | `av->isBeingAnimated()` | |
| `av->m_bIsBeingAnimated = b;` (write) | no setter -> see "No direct replacement" #1 | sites: `MosaicDrive.cpp:35,36,56,57`, `FlightController.cpp:726,727,1077,1078` |

Callbacks, `createAnimation`, `setConfig/getConfig`: unchanged.

## 2. Window / layer geometry animations

Header: `<hyprland/src/desktop/view/types/GeometricMovableAnimated.hpp>` (included by `Window.hpp`, `LayerSurface.hpp`).

```cpp
PHLANIMVAR<Vector2D>& positionAnimation();
PHLANIMVAR<Vector2D>& sizeAnimation();
virtual Vector2D position(eGeometricValueType) const override;   // GEOMETRIC_CURRENT -> value(), GEOMETRIC_GOAL -> goal()
virtual Vector2D size(eGeometricValueType) const override;
virtual CBox     geometricBox(eGeometricValueType) const override;
virtual void     move(const Vector2D& x) override;   // *m_realPosition = x (animates)
virtual void     resize(const Vector2D& x) override; // *m_realSize = x (animates)
virtual void     setBox(const CBox& x) override;     // both, animated
virtual void     finishAnimation();                  // warp() both
```
`eGeometricValueType` is `Desktop::View::IGeometric::GEOMETRIC_CURRENT | GEOMETRIC_GOAL` (`desktop/view/types/Geometric.hpp`).

| OLD | NEW |
|---|---|
| `w->m_realPosition` / `w->m_realSize` (the PHLANIMVAR, any use) | `w->positionAnimation()` / `w->sizeAnimation()` (returns `PHLANIMVAR&`, so `->`, `*x = v`, `.get()` all work) |
| `w->m_realPosition->m_Value = P` | `w->positionAnimation()->value() = P` |
| `w->m_realPosition->goal()` | `w->position(Desktop::View::IGeometric::GEOMETRIC_GOAL)` or `w->positionAnimation()->goal()` |
| `!w->m_realPosition` null checks | `!w->positionAnimation()` |
| same for `CLayerSurface` (`ls->m_realPosition/m_realSize`) | `ls->positionAnimation()` / `ls->sizeAnimation()` |

Mechanical: `sed -E 's/->m_realPosition\b/->positionAnimation()/g; s/->m_realSize\b/->sizeAnimation()/g'`
then `->m_Value` -> `->value()`. ~70 sites (FlightController 30, MosaicDrive 15, ShowDesktop 6, dock/*, overview/*).

## 3. Window presentation animated members (moved off CWindow)

Header: `<hyprland/src/desktop/view/window/WindowPresentation.hpp>`; reach via `w->presentation()`.

| OLD | NEW (exact signature) | Note |
|---|---|---|
| `w->alpha(WINDOW_ALPHA_X)` / `w->m_alpha` | `PHLANIMVAR<float>& alpha(eWindowAlpha type);` -> `w->presentation().alpha(Desktop::View::WINDOW_ALPHA_X)` | `CWindow::alpha()` (no-arg) still exists and returns the `CMultiAVarContainer<float,uint8_t>&`; the indexed overload is only on presentation. |
| `w->m_alpha.isBeingAnimated()` | `w->presentation().alpha().isBeingAnimated()` (or `w->alpha().isBeingAnimated()`) | |
| read value | `float alphaValue(eWindowAlpha) const; float alphaGoal(eWindowAlpha) const; float alphaTotal() const;` | |
| `*w->m_dimPercent = D` | `void setDimPercent(float percent);` (animated, `*m_dimPercent = percent`) | `MosaicParts.hpp:3055` |
| `w->m_dimPercent->warp()` | `void warpDimPercent(float percent);` (= `setValueAndWarp`) -> `p.warpDimPercent(p.dimPercent())` | `FlightController.cpp:607` |
| `w->m_dimPercent->value()` | `float dimPercent() const;` | |
| `w->updateDecorationValues()` | `void refreshValues();` -> `w->presentation().refreshValues()` | recomputes active/inactive/fullscreen alpha + dim targets |
| `w->m_monitorMovedFrom = -1` | `void resetMonitorMovedFrom();` (`setMonitorMovedFrom(int)`, `movingFromMonitor()`) | `FullscreenDesktop.cpp:243` |
| `w->m_floatingOffset` | `const Vector2D& floatingOffset() const;` / `setFloatingOffset(const Vector2D&)` / `clearFloatingOffset()` | `TripFrame.cpp:376`, `overview/Window.cpp:759,1412`, `FlightBlur.cpp:349` |
| animating-in bit | `bool animatingIn() const; void setAnimatingIn(bool);` | already used |

Workspace `ws->m_alpha` / `ws->m_renderOffset`: **unchanged**, now public members of base
`Workspace::CWorkspacePresentable` (`workspace/presentation/WorkspacePresentable.hpp`), inherited by `CHLWorkspace`.

Border / shadow animation vars (`m_borderFadeAnimationProgress`, `m_borderAngleAnimationProgress`,
`m_realBorderColor[Previous]`, `m_shadowFadeAnimationProgress`, `m_realShadowColor[Previous]`): gone from
CWindow -> see "No direct replacement" #2.

## 4. Animation manager / config tree (unchanged API)

Header `<hyprland/src/animation/AnimationManager.hpp>`: `UP<CHyprAnimationManager>& Animation::mgr();`
```cpp
template <Animable VarType>
void createAnimation(const VarType& v, PHLANIMVAR<VarType>& pav, SP<SAnimationPropertyConfig> pConfig, eAVarDamagePolicy policy);
// + overloads taking PHLWINDOW / PHLWORKSPACE / PHLLS before policy, and NEW: (…, PHLWINDOW, SP<IHyprWindowDecoration>, policy)
void requestTick();   // NEW: schedule a tick without a variable change
```
All Hyprdrive calls use `AVARDAMAGE_NONE` -> no change. Enum change (only matters if used):
`AVARDAMAGE_BORDER / AVARDAMAGE_SHADOW / AVARDAMAGE_GLOW` -> single `AVARDAMAGE_DECORATION` (+ `SAnimationContext::pDecoration`).
`Config::animationTree()->getAnimationPropertyConfig(name) / getAnimationConfig() / setConfigForNode(name, enabled, speed, bezier, style="")`
in `<hyprland/src/config/shared/animation/AnimationTree.hpp>`: unchanged.

## 5. Layout targets

Header `<hyprland/src/desktop/view/window/Window.hpp>` (forward-declares `Layout::CWindowTarget` only):
```cpp
SP<Layout::CWindowTarget>  windowTarget();       // the window's own target (old m_target)
SP<Layout::ITarget>        layoutTarget();       // group target if grouped, else windowTarget()
bool                       isFloating() const;   // = m_target->floating()
```

| OLD | NEW | Note |
|---|---|---|
| `w->m_target` (now private, type `SP<Layout::CWindowTarget>`) | `w->windowTarget()` | Mosaic.cpp:462-566. Hyprland itself passes `window->windowTarget()` to `IFullscreenHandler::setTargetFullscreenMode*` (FullscreenController.cpp:518). |
| passing it where `SP<Layout::ITarget>` is expected (Mosaic.cpp:465-466 `setTargetFullscreenModeInternal(const SP<Layout::ITarget>, mode)`; Mosaic.cpp includes only `layout/target/Target.hpp`, so it needs the WindowTarget.hpp include below) -> "cannot convert CSharedPointer<CWindowTarget>" | add `#include <hyprland/src/layout/target/WindowTarget.hpp>` (upcast needs the complete type), or use `w->layoutTarget()` | Window.hpp no longer includes WindowTarget.hpp. For a grouped window `layoutTarget()` is the *group* target; keep `windowTarget()` where the 0.56 code meant the window itself. |
| `w->m_isFloating = b;` (pre-map, WindowPlacement.cpp:198,240) | `w->windowTarget()->setFloatingInitial(b);` | `void setFloatingInitial(bool x);` = raw store, what Hyprland's mapWindow uses. `setFloating(bool)` instead also unpins, schedules state update and re-applies rules: use it only for a live toggle. |
| `CWindowTarget::create(w)` returned `SP<ITarget>` | `static SP<CWindowTarget> create(PHLWINDOW w);` | not called by Hyprdrive |
| `ITarget`, `warpPositionSize()`, `setPositionGlobal`, `position()`, `floating()` | unchanged (`layout/target/Target.hpp`, only include path inside changed) | |
| `Layout::ITiledAlgorithm`, `IModeAlgorithm::getFSHandler`, `CAlgorithm`, `CSpace`, `SColumnData`, `CScrollingAlgorithm`, `eRectCorner`, `RECALCULATE_REASON_*` | unchanged headers -> Mosaic's ITiledAlgorithm overrides compile as-is | `ScrollTapeController::getOffset()` now in the public section (was already reachable). |

New public on `CWindowTarget` (optional): `cantLockCursor()/setCantLockCursor(bool)`, `clampWindowSize(min,max)`,
`calculateExpression(const Math::SExpressionVec2&)`, `sendWindowSize(bool force=false)`.

## 6. Layout manager / drag controller

Headers `<hyprland/src/layout/LayoutManager.hpp>`, `<hyprland/src/layout/supplementary/DragController.hpp>`.

| OLD | NEW | Note |
|---|---|---|
| `void CLayoutManager::endDragTarget();` | `bool endDragTarget();` | returns `dragEnd()`: false = no drag target was held (nothing happened). Call sites (`Mosaic.cpp:286`, `MosaicGestures.cpp:520`, `SpringLoad.cpp:478`) compile unchanged; Hyprland's keybind manager uses the bool as "drag was active". |
| `void CDragStateController::dragEnd();` | `bool dragEnd();` | same meaning; `main.cpp:548` unchanged. |
| polling `dragController()->target()` for motion/end | NEW `m_events.motion` / `m_events.ended` (`CSignalT<>`) on `CDragStateController` | `ended` fires via scope guard on every dragEnd that had a target (also the unmapped-window path). Optional; existing polling still works. |
| `dragBegin(...)`, `target()`, `wasDraggingWindow()`, `beginDragTarget`, `switchTargets`, `changeFloatingMode`, `resizeTarget`, `moveInDirection`, `recalculateMonitor` | unchanged | |

## 7. Fullscreen

Headers `<hyprland/src/managers/fullscreen/FullscreenController.hpp>` (now includes the NEW
`managers/fullscreen/FullscreenTypes.hpp`, where `eFullscreenMode`, `FSMODE_*`, `SFullscreenMode`,
`eFullscreenHandler`, `eFullscreenRequestResult` moved, same namespace `Fullscreen::`; no source change).

| OLD | NEW | Note |
|---|---|---|
| `controller()->setFullscreenMode(w, mode, …, std::optional<bool> layoutAware)` | `…, std::optional<bool> layoutAware = std::nullopt, eFullscreenMutationContext context = FULLSCREEN_MUTATION_NORMAL)` | defaulted; `FULLSCREEN_MUTATION_TRANSFER` = move compositor-internal state without touching client state. |
| `IFullscreenHandler::setNoMembersAboveFullscreen()` | `virtual void setNoMembersAboveFullscreen(const std::optional<SP<Layout::ITarget>> coveringFsTarget = std::nullopt);` | only matters if Hyprdrive overrides it (it does not). |
| `isFullscreen/hasFullscreen/getFullscreenWindow/getFullscreenModes`, handler `setTargetFullscreenModeInternal/Client(SP<ITarget>, eFullscreenMode)` | unchanged | |
| `w->m_allowedOverFullscreen = true` | `w->fullscreenPolicy().setAllowedOverFullscreen(true);` (`desktop/view/window/WindowFullscreenPolicy.hpp`) | read: `w->isAllowedOverFullscreen()`. Hyprland's focus/state code also writes it (FocusState.cpp:43, WindowState.cpp:55), as before. |
| `w->m_suppressedEvents` + `SUPPRESS_FULLSCREEN` / `SUPPRESS_MAXIMIZE` bitmask | `const SFullscreenRequestSuppression& requestSuppression() const; void setRequestSuppression(const SFullscreenRequestSuppression&);` struct `{bool fullscreen, maximize, fullscreenOutput;}` | Rewritten from window rules only in `CWindow::mapWindow()`; Hyprdrive's hold survives until a remap (same as 0.56). Activate/x11 suppressions moved to a separate private struct (not used here). Snippet below. |
| `w->m_noInitialFocus = true` | `w->m_state \|= Desktop::View::WINDOW_STATE_NO_INITIAL_FOCUS;` | `m_state` is public `WindowState` mask; Hyprland's map reads it (Window.cpp:1303). |
| `ls->m_aboveFullscreen` (bool) | `ls->m_flags & Desktop::View::LAYER_FLAG_ABOVE_FULLSCREEN` (set: `ls->m_flags \|= …`, clear: `ls->m_flags &= ~…`) | `LayerFlags m_flags` public in `desktop/view/LayerSurface.hpp`. Overview.cpp:167-252 stores/restores the bool: store `bool(ls->m_flags & LAYER_FLAG_ABOVE_FULLSCREEN)`. |

`holdOut`/`releaseHeld` (FullscreenDesktop.cpp:1004-1028) in 0.57 shape:
```cpp
#include <hyprland/src/desktop/view/window/WindowFullscreenPolicy.hpp>
enum class EHold : uint8_t { Fullscreen, Maximize };
static bool& bit(Desktop::View::SFullscreenRequestSuppression& s, EHold h) { return h == EHold::Maximize ? s.maximize : s.fullscreen; }
void holdOut(const PHLWINDOW& w, EHold h = EHold::Fullscreen) {
    if (!w) return;
    auto s = w->fullscreenPolicy().requestSuppression();      // copy
    if (bit(s, h)) return;                                    // the user's own rule, or ours already
    bit(s, h) = true;
    w->fullscreenPolicy().setRequestSuppression(s);
    g_heldOut.push_back({w, w->m_workspace, h});              // SHeldOut::flag becomes EHold
}
// release: auto s = W->fullscreenPolicy().requestSuppression(); bit(s, h.flag) = false; W->fullscreenPolicy().setRequestSuppression(s);
```

## No direct replacement

1. **Writing `m_bIsBeingAnimated`** (MosaicDrive.cpp, FlightController.cpp; purpose: make Hyprland treat a
   hand-driven size as animating so `CSurfacePassElement` scales subsurfaces). Hyprland only sets it inside
   `operator=`/`setValueAndWarp` (true) and `warp()`/`update()` (false); `CAnimationManager` is the only friend.
   Closest option, already in-tree: `COverviewAnimatedVariableAccess::setBeingAnimated(var.get(), bool)`
   (`overview/Window.cpp:87`, derives from `CBaseAnimatedVariable` so `protected` is reachable, no `#define`).
   Move that struct to a shared header (e.g. `src/core/HyprCompat.hpp` or a `motion/` header) and use
   `setBeingAnimated(w->positionAnimation().get(), true)` at all 8 sites. Do not replace with `*av = X`:
   that registers the var with the animation manager and it would start ticking toward X.
2. **Border / shadow gradient + fade progress** (`overview/Window.cpp:829-926`, `FlightController.cpp:605,1127,1155`,
   `OverviewInternal.hpp:1353`). 0.57 moved them into `CAnimatedDecorationGradient m_gradient` (private) inside
   `CHyprBorderDecoration` / `CHyprDropShadowDecoration` (`render/decorations/AnimatedDecorationGradient.hpp`),
   which exposes `SRenderState renderState() const` = `{current, previous, progress, transitioning}` but the member is private
   and the decorations have no getter. Closest: get the decoration with
   `w->presentation().decoration(DECORATION_BORDER)` / `decoration(DECORATION_SHADOW)` (`SP<IHyprWindowDecoration> decoration(eDecorationType) const`, ONE type per call, not a bitmask; enum in `render/decorations/IHyprWindowDecoration.hpp`), then read `m_gradient.renderState()` (private in both `CHyprBorderDecoration` and `CHyprDropShadowDecoration`; a derived-access shim like #1 is impossible, private not protected), so either (a) put `#define private public`
   in front of the decoration headers *inside HyprCompat.hpp* before anything else includes them, or (b) recompute
   from config (`general:col.active_border` / `inactive`, `decoration:shadow:color`) and the window's focus state,
   losing the fade. "Is any border animation running" (OverviewInternal.hpp:1353): no public query; drop to
   `presentation().alpha().isBeingAnimated()` + position/size, or use (a). Owner of the decoration area should decide (a) vs (b).
3. **`#define private public` generally**: all per-TU uses are defeated by the force-include (section 0). If the team
   wants to keep any of them for other private members, the `#define`s must move into HyprCompat.hpp above its
   includes (one place, applies everywhere) or HyprCompat must stop including Hyprland headers.
