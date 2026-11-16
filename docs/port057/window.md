# CWindow leftovers: 0.56.2 -> 0.57.2

Scope: the `Desktop::View::CWindow` members Hyprdrive touches that moved, went private or were split into
sub-objects in 0.57 (alpha/dim, decoration fades, transformers, X11/XDG surfaces, reported size, suppression,
target, suspend/hidden, floating/pinned/urgent writes, fullscreen policy, real pos/size, layer above-FS, rules).
Header root: `<hyprland/src/...>` (SDK `<SDK>/include/hyprland/src`). `HyprCompat.hpp` already pulls in
Window, WindowMetadata, WindowPresentation, WindowBackend, WindowEffectsController, WindowFullscreenPolicy.

0.57 splits CWindow into sub-objects, reached through accessors on `CWindow`:
`backend()` (IWindowBackend&), `presentation()` (CWindowPresentation&), `effects()` (CWindowEffectsController&),
`metadata()` (const CWindowMetadata&), `fullscreenPolicy()` (CWindowFullscreenPolicy&), `windowTarget()` / `layoutTarget()`.

## 1. Geometry animation (`m_realPosition`, `m_realSize`): now protected

| OLD | NEW | Notes |
|---|---|---|
| `w->m_realPosition` | `w->positionAnimation()` | `desktop/view/types/GeometricMovableAnimated.hpp`: `PHLANIMVAR<Vector2D>& positionAnimation();` |
| `w->m_realSize` | `w->sizeAnimation()` | `PHLANIMVAR<Vector2D>& sizeAnimation();` Same object as before, so `->goal()`, `->value()`, `->warp()`, `->setValueAndWarp()`, `->isBeingAnimated()`, `->getConfig()`, `= x` all work unchanged. |
| `w->m_realPosition->m_Value = P` / `->m_bIsBeingAnimated = b` | `w->positionAnimation()->m_Value = P` | Only the receiver changes. The TU still needs the Z19 `#define private public` block around `<hyprutils/animation/AnimatedVariable.hpp>` *first*. DockPointer.cpp:523-524 fails because that TU lacks the block (or includes it too late). |
| read-only box | `w->position(IGeometric::GEOMETRIC_GOAL / GEOMETRIC_CURRENT)`, `w->size(...)`, `w->geometricBox(...)` | Hyprland itself uses these for the configure box (`CWindowTarget::sendWindowSize`). |
| `*w->m_realPosition = x` | `w->move(x)` / `w->resize(x)` / `w->setBox(box)` | These animate to the value (they are `*m_realPosition = x`). `finishAnimation()` warps both. |

## 2. Alpha containers

| OLD | NEW | Notes |
|---|---|---|
| `w->m_alpha.isBeingAnimated()` (the window's multi-var) | `w->alpha().isBeingAnimated()` | `Types::CMultiAVarContainer<float, uint8_t>& alpha() override;` (Window.hpp). It forwards to `presentation().alpha()`. Container also has `value()`, `goal()`, `warp()`, `forEach`, `operator[](key)`. |
| `w->alpha(WINDOW_ALPHA_X)` (single var) | `w->presentation().alpha(Desktop::View::WINDOW_ALPHA_X)` | `PHLANIMVAR<float>& alpha(eWindowAlpha type);` (WindowPresentation.hpp). `CWindow::alpha` now takes no argument. `w->alpha()[WINDOW_ALPHA_X]` is equivalent and already compiles. |
| `w->alphaValue(WINDOW_ALPHA_LAYOUT)` | `w->presentation().alphaValue(WINDOW_ALPHA_LAYOUT)` | `float alphaValue(eWindowAlpha type) const;` also `alphaGoal`, `alphaTotal()`, `alphaTotalGoal()`. |
| `ls->m_alpha.isBeingAnimated()` (CLayerSurface) | `ls->alpha().isBeingAnimated()` | `m_alpha` is private in LayerSurface.hpp. `ls->alpha()[LS_ALPHA_FADE]` is what Hyprdrive already uses elsewhere. |

## 3. Dim, decoration values, border size

| OLD | NEW | Notes |
|---|---|---|
| `*w->m_dimPercent = DIM` | `w->presentation().setDimPercent(DIM)` | `void setDimPercent(float percent);` animates to it (`*m_dimPercent = percent`). |
| `w->m_dimPercent->setValueAndWarp(D)` | `w->presentation().warpDimPercent(D)` | `void warpDimPercent(float percent);` |
| read dim | `w->presentation().dimPercent()` | `float dimPercent() const;` returns the current value, not the goal. |
| `w->updateDecorationValues()` | `w->presentation().refreshValues()` | `void refreshValues();` Recomputes WINDOW_ALPHA_ACTIVE from rules/focus/fullscreen, **calls setDimPercent(goal)** (so it overwrites a Hyprdrive dim), then runs every decoration's `updateState()` and `updateDecorations()`. Hyprland calls it from Group.cpp and Window.cpp:1593. |
| `w->m_borderSizeCacheDirty = true` | `w->presentation().invalidateBorderSize()` | The cache now lives in `CHyprBorderDecoration` (private `m_cachedBorderSize`, `m_borderSizeCacheDirty`). |
| read border size | `w->presentation().borderSize()` | `int borderSize() const;` |
| save/restore `m_cachedBorderSize` + dirty flag | `invalidateBorderSize()` at restore | No setter for the cached value. Invalidating costs one recompute on the next read and gives the same result. |
| `w->m_windowDecorations` | `w->presentation().decorations()` | `std::span<const SP<IHyprWindowDecoration>> decorations() const;` Read-only. Use `addDecoration` / `removeDecoration(IHyprWindowDecoration*)` to change it. `decoration(eDecorationType)` returns one. |
| `w->m_floatingOffset` | `w->presentation().floatingOffset()` | `const Vector2D& floatingOffset() const;` Setters: `setFloatingOffset(v)`, `clearFloatingOffset()`. |
| `w->m_monitorMovedFrom = -1` | `w->presentation().resetMonitorMovedFrom()` | `void resetMonitorMovedFrom();` reads via `movingFromMonitor()`. |

## 4. Window rules (`m_ruleApplicator`)

Still a public member: `UP<Desktop::Rule::CWindowRuleApplicator> m_ruleApplicator;` (`desktop/rule/windowRule/WindowRuleApplicator.hpp`).
Each property `P` gives `Types::COverridableVar<T>& P()` and `void P##Override(const Types::COverridableVar<T>&)`.

| OLD | NEW |
|---|---|
| `m_ruleApplicator->presentation().rounding()` | `m_ruleApplicator->rounding()`. There is no `presentation()` sub-struct. `roundingOverride(...)`, `roundingPower()` keep their names. |
| `borderSize()`, `borderSizeOverride()`, `alpha()`, `alphaInactive()`, `alphaFullscreen()`, `decorate()`, `noBlur()`, `noShadow()`, `noMaxSize()`, `minSize()`, `xray()`, `noDim()`, `static_`, `m_tagKeeper` | Unchanged. They compile as they are. |

## 5. Transformers (`m_transformers`)

`#include <hyprland/src/render/transformer/TransformerList.hpp>`. `w->effects().transformers()` returns `const UP<Render::CWindowTransformerList>&`
(the UP is const, the list is not). The vector is private. The `IWindowTransformer` virtuals changed too (`transform(ctx, in, context)`), which another port doc covers.

| OLD | NEW | Notes |
|---|---|---|
| `w->m_transformers.emplace_back(makeUnique<T>()); ... back().get()` | `T* t = w->effects().transformers()->emplace<T>(args...);` | Returns the raw pointer. Sorts by `priority()` after insert, so `back()` is no longer "yours". |
| loop + `dynamic_cast<T*>` to find ours | `w->effects().transformers()->get<T>()` | Returns the first `T` or `nullptr`. |
| `std::erase_if(..., isOurs)` | make `T::active()` return false, then `w->effects().transformers()->removeInactive()` | `void removeInactive();` erases every transformer whose `active()` is false. Hyprland calls it itself after `resetMotionBlur`, `resetWobble` and `tickWobble`, so **an inactive transformer is destroyed by Hyprland at any of those points**. |
| `.empty()` / `.size()` | `->empty()` / `w->effects().hasActiveTransformers()` | `bool empty() const;` means "no **active** transformer", not "no elements". There is no `size()`. |

## 6. Backend: X11 / XDG surfaces, reported size, parent, traits

`#include <hyprland/src/desktop/view/window/WindowBackend.hpp>` (already in HyprCompat). `IWindowBackend& backend();`

| OLD | NEW | Notes |
|---|---|---|
| `w->isX11OverrideRedirect()` | `w->backend().traits().overrideRedirect` | `virtual SBackendTraits traits() const = 0;` bitfield struct: `overrideRedirect, modal, hasModalChild, transient, wantsFocus, suggestsFloat, suggestsNoInitialFocus, preventsFocus, suggestsNoBorder, fullscreen`. Always false on Wayland. |
| `w->isModal()` | `w->backend().traits().modal` | X11: `_NET_WM_STATE_MODAL`. Wayland: from the xdg dialog protocol. |
| `w->m_X11DoesntWantBorders` | `w->backend().traits().suggestsNoBorder` | Now also true for override-redirect windows (X11Backend.cpp traitsFrom). |
| `w->parent()` | `w->backend().parent()` | `virtual PHLWINDOW parent() const = 0;` **Returns nullptr unless the parent is `validMapped`.** |
| `w->x11Parent()` | `w->backend().parent()` (when `w->backend().isX11()`; `CWindow` has no `isX11()` in 0.57, it is `IWindowBackend::isX11()`) | X11 implementation: looks up the parent's `m_xID` against `backend().clientID()`. Same mapped-only rule. |
| `XS->m_xID` | `w->backend().clientID().id` (when `w->backend().isX11()`; `CWindow` has no `isX11()` in 0.57, it is `IWindowBackend::isX11()`) | `struct SBackendClientID { eBackendType type; uint64_t id; }`. Hyprland's own xsurface->window lookup: `W->backend().clientID() == SBackendClientID{.type = eBackendType::WINDOW_BACKEND_X11, .id = xs->m_xID}`. Use this for `windowOf(xs)`. |
| `XS->m_mapped` | `w->backend().isMapped()` | Set by the backend's map/unmap listeners on the xsurface. |
| `XS->m_overrideRedirect` | `w->backend().traits().overrideRedirect` | |
| `XS->m_sizeHints` + `w->xwaylandSizeToReal(min)` | `w->backend().geometryHints(eBackendState::BACKEND_STATE_CURRENT)` | `struct SGeometryHints { std::optional<Vector2D> minSize, maxSize; }`. Already in logical units (divided by the xwayland scale). min is clamped to >=1 and max<5 becomes `DBL_MAX`, so "fixed size" means `min == max`. |
| `XS->m_geometry` (X coords) | `w->backend().geometry().box` = logical. Raw X box: `w->backend().logicalToClient(w->backend().geometry().box, nullptr)` | `SClientGeometry { CBox box; bool positionAuthoritative; }`. X11 `geometry()` returns `xwaylandToWaylandCoords(pos)` and `size / scale`, so the round trip can be off by rounding. |
| `w->realToReportPosition()` / `realToReportSize()` | `w->backend().logicalToClient(CBox{w->position(GEOMETRIC_GOAL), w->size(GEOMETRIC_GOAL)}, w->m_monitor.lock())` `.pos()` / `.size()` | `virtual CBox logicalToClient(const CBox& box, PHLMONITOR preferredMonitor) const = 0;` Hyprland configures with this same goal box. Size is clamped to >=1, scaled and rounded. |
| `w->getReportedSize()` / `w->m_reportedSize` | `w->backend().reportedSize()` | `virtual Vector2D reportedSize() const = 0;` Client units. **Wayland: the surface's `m_current.ackedSize` (the acked size, not the last size sent).** X11: the last acked configure. |
| `w->fetchClass()` | `w->metadata().appID()` (cached) or `w->backend().metadata().appID` (live from the client) | `const std::string& appID() const;` |
| `w->m_stableID` | `w->metadata().stableID()` | `uint64_t stableID() const;` |
| `W->m_xdgSurface && W->m_xdgSurface->m_initialCommit` | `!W->backend().isX11() && W->backend().initialCommit()` | `virtual bool initialCommit() const = 0;` (X11 is always false). |
| `XDG->m_current.geometry` | `w->backend().geometry().box` | Wayland: exactly `resource->m_current.geometry`. |
| `w->m_xdgSurface->m_toplevel->m_current.minSize/maxSize` (raw, no offset) | through the surface role, see the snippet below | `geometryHints()` **adds the xdg geometry offset** to min/max (WaylandBackend.cpp:52). OwnSizeTile deliberately avoids that offset, so it must read the raw toplevel. |

Raw XDG resource (public path, same object 0.56 exposed):
```cpp
#include <hyprland/src/protocols/XDGShell.hpp>          // CXDGSurfaceRole, CXDGSurfaceResource, CXDGToplevelResource
#include <hyprland/src/protocols/core/Compositor.hpp>   // CWLSurfaceResource::m_role
#include <hyprland/src/protocols/types/SurfaceRole.hpp> // SURFACE_ROLE_XDG_SHELL
inline SP<CXDGSurfaceResource> xdgOf(const PHLWINDOW& w) {
    if (!w || w->backend().isX11()) return nullptr;
    const auto S = w->backend().surface();
    if (!S || !S->m_role || S->m_role->role() != SURFACE_ROLE_XDG_SHELL) return nullptr;
    return sc<CXDGSurfaceRole*>(S->m_role.get())->m_xdgSurface.lock();
}
// xdgOf(w)->m_toplevel->m_current.minSize / maxSize, ->m_current.geometry, ->m_initialCommit
```

## 7. State bits, floating, target, suspend/hidden

| OLD | NEW | Notes |
|---|---|---|
| `w->m_pinned = b` | `b ? w->m_state |= WINDOW_STATE_PINNED : w->m_state &= ~WINDOW_STATE_PINNED` | `WindowState m_state;` mask type with `| |= & &= ~`. Reads: `Compat::pinned(w)`. `CWindowTarget::setFloating()` **clears PINNED** (WindowTarget.cpp:293), and map re-applies the `pin` rule as `rule.value_or(current bit)`. |
| `w->m_isUrgent = true/false` | `w->m_hints |= WINDOW_HINT_URGENT` / `&= ~WINDOW_HINT_URGENT` | `WindowHints m_hints;` A raw write sends no IPC/event. `w->markUrgent()` sets the bit **and** posts socket2 `urgent` + `Event::bus()->m_events.window.urgent`, and does nothing if the window is focused. There is no "clear" call. |
| `w->m_noInitialFocus = true` | `w->m_state |= WINDOW_STATE_NO_INITIAL_FOCUS` | Map keeps the bit unless a `noInitialFocus` rule says otherwise (Window.cpp:1303). |
| `w->m_isFloating = b` (before map) | `w->windowTarget()->setFloatingInitial(b)` | `layout/target/WindowTarget.hpp`: `void setFloatingInitial(bool x);` sets the flag only. `CWindow::isFloating()` is `m_target->floating()`. mapWindow runs `setFloatingInitial(rule.floating.value_or(target->floating()))` and then emits `window.openEarly` (Window.cpp:1444) before `g_layoutManager->newTarget` (1459), so a value set there or earlier holds. |
| `w->m_isFloating = b` (mapped) | `w->windowTarget()->setFloating(b)` | `virtual void setFloating(bool x);` also clears PINNED, schedules a state update and fires the floating rule change. |
| `w->m_target` (private) | `w->windowTarget()` -> `SP<Layout::CWindowTarget>`; `w->layoutTarget()` -> `SP<Layout::ITarget>` | Use `layoutTarget()` wherever an `SP<ITarget>` is wanted (Mosaic.cpp:465-466 `setTargetFullscreenModeInternal/Client`). |
| `if (w->m_suspended) w->setSuspended(false);` | `w->setSuspended(false);` | `void setSuspended(bool suspend);` already returns early when unchanged (Window.cpp:731). The flag only flips if `backend().setSuspended()` succeeds. There is no getter; see No direct replacement. |
| `w->m_hidden` read | `w->isHidden()` | `bool isHidden() const;` |
| `w->m_hidden = v` | (private) | `setHidden(bool)` also emits `m_events.hide` and drops focus when hiding, and always calls `setSuspended(hidden)`. See No direct replacement. |

## 8. Fullscreen policy and request suppression

`#include <hyprland/src/desktop/view/window/WindowFullscreenPolicy.hpp>`. `CWindowFullscreenPolicy& fullscreenPolicy();`

| OLD | NEW | Notes |
|---|---|---|
| `w->m_allowedOverFullscreen = b` | `w->fullscreenPolicy().setAllowedOverFullscreen(b)` | Read: `allowedOverFullscreen()`. `w->isAllowedOverFullscreen()` is the effective value. |
| `w->m_suppressedEvents` with `SUPPRESS_FULLSCREEN` / `SUPPRESS_MAXIMIZE` / `SUPPRESS_FULLSCREEN_OUTPUT` | `struct SFullscreenRequestSuppression { bool fullscreen, maximize, fullscreenOutput; }` via `const SFullscreenRequestSuppression& requestSuppression() const;` / `void setRequestSuppression(const SFullscreenRequestSuppression&);` | The bit enum is gone. activate / activatefocus / x11configurerequest moved to the private `CWindow::m_requestSuppression`. **mapWindow overwrites the whole struct from `suppress_event` rules (Window.cpp:1336)**, so set it after map, as Hyprdrive already does. Checked in `requestClientFullscreen` (Window.cpp:863). |

`holdOut` / restore pattern:
```cpp
auto S = w->fullscreenPolicy().requestSuppression();          // copy
bool& bit = (flag == HOLD_MAXIMIZE) ? S.maximize : S.fullscreen;
if (bit) return; bit = true;  w->fullscreenPolicy().setRequestSuppression(S);   // restore: bit = false, set again
```
Keep Hyprdrive's own enum for `flag`, since `Desktop::View::SUPPRESS_*` no longer exists.

## 9. Layer surface above fullscreen

| OLD | NEW | Notes |
|---|---|---|
| `ls->m_aboveFullscreen` read | `bool(ls->m_flags & Desktop::View::LAYER_FLAG_ABOVE_FULLSCREEN)` | `desktop/view/LayerSurface.hpp`: `LayerFlags m_flags;` mask `eLayerFlags`. |
| `= true` / `= false` | `ls->m_flags |= LAYER_FLAG_ABOVE_FULLSCREEN` / `ls->m_flags &= ~LAYER_FLAG_ABOVE_FULLSCREEN` | Hyprland sets or clears it from the layer rule (LayerSurface.cpp:201-203) and Monitor.cpp clears it when a fullscreen window appears (1410/1468/1527/1546). `isHiddenByFullscreen(mon)` = TOP layer && !flag && `Fullscreen::controller()->hasFullscreen(mon, true)`. |

## 10. SizeAck hook target

`CWindow::onAck`, `m_pendingSizeAck` and `m_pendingSizeAcks` are gone. Acks now go to the private `CWaylandBackend::onAck` -> `CWindowConfigureAckTracker::acknowledge(serial)`.
Upstream already copies the acked serial and size before `erase_if` (WindowBackend.cpp:19-28), which is the bug `SizeAck::onAck` existed to fix.
The hook has no 0.57 target, and the behaviour it provided is now Hyprland's default. Keep `SizeAck::report()` diagnostics; they need the private-access helper (below) for the pending counts.

## No direct replacement

All of these are reachable with one private-access TU. Recommended: `src/core/HyprPrivate.cpp` + `.hpp`, using the standard explicit-instantiation idiom
(access checks do not apply to explicit instantiation arguments; keep it in **one** .cpp so the instantiation exists once).
`CX11Backend` and `CWaylandBackend` are `final` and derive from `IWindowBackend` non-virtually, so after checking `backend().type()`, `static_cast` is valid.
```cpp
// HyprPrivate.cpp
#include <hyprland/src/desktop/view/window/X11Backend.hpp>
#include <hyprland/src/desktop/view/window/WaylandBackend.hpp>
#include <hyprland/src/xwayland/XSurface.hpp>
template <typename Tag, typename Tag::type M> struct SRob { friend typename Tag::type get(Tag) { return M; } };
#define HD_ROB(NAME, CLASS, TYPE, MEMBER) \
    struct NAME { using type = TYPE CLASS::*; friend type get(NAME); }; template struct SRob<NAME, &CLASS::MEMBER>;
HD_ROB(TX11Surf, Desktop::View::CX11Backend, WP<CXWaylandSurface>, m_xwaylandSurface)
HD_ROB(TX11Pend, Desktop::View::CX11Backend, std::optional<Vector2D>, m_pendingReportedSize)
HD_ROB(TX11Pos,  Desktop::View::CX11Backend, Vector2D, m_reportedPosition)
HD_ROB(TWlPend,  Desktop::View::CWaylandBackend, std::optional<Vector2D>, m_pendingReportedSize)
HD_ROB(TSusp,    Desktop::View::CWindow, bool, m_suspended)
HD_ROB(THidden,  Desktop::View::CWindow, bool, m_hidden)
SP<CXWaylandSurface> Hyprdrive::Priv::xsurface(const PHLWINDOW& w) {
    if (!w || w->backend().type() != Desktop::View::eBackendType::WINDOW_BACKEND_X11) return nullptr;
    return (sc<Desktop::View::CX11Backend&>(w->backend()).*get(TX11Surf{})).lock();
}
bool& Hyprdrive::Priv::hidden(Desktop::View::CWindow& w) { return w.*get(THidden{}); }
// ... one small accessor per tag, declared in HyprPrivate.hpp
```

| Symbol | What 0.57 does internally | Closest option |
|---|---|---|
| `w->m_xwaylandSurface` (`m_atoms`, `m_maximized` write, `m_state.requests*`, raw `m_geometry`, `m_sizeHints`) | private `WP<CXWaylandSurface> CX11Backend::m_xwaylandSurface`. `CXWM::m_surfaces` and `windowForXID` are private too, so there is no public window->xsurface path | `Priv::xsurface(w)` (above) returns the same `SP<CXWaylandSurface>` 0.56 gave, with every field Hyprdrive reads still public on `CXWaylandSurface` (`xwayland/XSurface.hpp`). Use the section 6 rows wherever they are enough. |
| `w->m_pendingReportedSize` (TripFrame `sentSize`) | private `std::optional<Vector2D> m_pendingReportedSize` in **both** backends (client units). Set in `configure()` and reset on ack | `TWlPend` / `TX11Pend`, falling back to `backend().reportedSize()` when nullopt. `x>0` tests become `has_value()`. |
| `w->m_reportedPosition` | X11 only: private `CX11Backend::m_reportedPosition` (X coords, last configure sent). Wayland has none | `TX11Pos`. Wayland callers already gate on `backend().isX11()` at TripFrame:264. Lines 368/384 need the same gate or `{}`. |
| `w->m_suspended` read (diagnostics, prof counter) | private bool, flipped only by a successful `backend().setSuspended()` | Logic: call `setSuspended(false)` unconditionally (section 7). Counters/log: `TSusp`. |
| `w->m_hidden = v` without side effects (Overview save/force/restore, DockItems:509) | `setHidden` emits `hide`, drops focus, and calls `setSuspended` | `Priv::hidden(*w) = v` keeps 0.56 behaviour. The read side is `isHidden()`. |
| `w->m_dimPercent->isBeingAnimated()` / `->warp()` | private `PHLANIMVAR<float> CWindowPresentation::m_dimPercent`. Only value, set and warp-to-value are public | `HD_ROB(TDim, Desktop::View::CWindowPresentation, PHLANIMVAR<float>, m_dimPercent)`. Without it: after `refreshValues()`, the goal is `noDim||focused||!dim_inactive ? 0 : dim_strength` (+ modal half-step), which can be passed to `warpDimPercent(goal)`. |
| `m_borderFadeAnimationProgress` (`isBeingAnimated`, `value`, `warp`), `m_realBorderColor`, `m_realBorderColorPrevious` | moved into each decoration's private `CAnimatedDecorationGradient m_gradient` (CHyprBorderDecoration.hpp:47) holding `m_fadeProgress`, `m_angleProgress`, `m_current`, `m_previous` | Rob `CHyprBorderDecoration::m_gradient` (type `CAnimatedDecorationGradient`). Then the public `renderState()` returns `{current, previous, progress, transitioning}` = old {color, previous, fade value, fade isBeingAnimated}. Decoration: `sc<CHyprBorderDecoration*>(w->presentation().decoration(DECORATION_BORDER).get())`. Warp: rob `CAnimatedDecorationGradient::m_fadeProgress` and call `->warp()`. |
| `m_borderAngleAnimationProgress->isBeingAnimated()` | `CAnimatedDecorationGradient::m_angleProgress` (private). `renderState()` folds the angle into `current.m_angle` but does not say whether it is animating | Rob `m_angleProgress`. |
| `m_shadowFadeAnimationProgress->isBeingAnimated()`, `m_realShadowColor` | `CHyprDropShadowDecoration::m_gradient` (private, CHyprDropShadowDecoration.hpp:55) | Same as border: rob `m_gradient` and use `renderState().transitioning` / `.current`. |
| transformer erase + move out and back in (`FlightBlur CSuspend`) | `CWindowTransformerList::m_transformers` is a private vector with no extract/erase, only `removeInactive()` | Keep the transformer in the list and give it a `bypass` flag: `transform()` returns `in` unchanged and `allocatesOutputBuffer()`/`blocksDirectScanout()` return false while bypassed. `active()` **must stay true**, or Hyprland's next `removeInactive()` destroys it. Moving the UP out needs `HD_ROB(TTr, Render::CWindowTransformerList, std::vector<UP<Render::IWindowTransformer>>, m_transformers)`. |
| `m_transformers.size()` (SizeAck diag) | no count API | `TTr` above, or print `hasActiveTransformers()`. |
| `m_pendingSizeAcks.size()` (SizeAck diag) | private `CWindowConfigureAckTracker CWaylandBackend::m_configureAcks` with a private vector, `empty()` public | Rob `m_configureAcks` and print `empty()`. A count needs a second rob of `CWindowConfigureAckTracker::m_pending`. |

Unresolved without the private-access TU: `m_xwaylandSurface` (atoms, maximized, state requests), `m_pendingReportedSize`, `m_reportedPosition`,
`m_suspended` (diag only), raw `m_hidden` write, `m_dimPercent` animating/warp, border/shadow fade progress + border angle progress + real border/shadow colors,
transformer removal-without-destroy and count, ack-queue count.
