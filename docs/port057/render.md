# Renderer port reference: Hyprland 0.56.2 -> 0.57.2

Scope: pass elements, render context, GL draw calls, framebuffers, render-stage events, window transformers.
Only symbols Hyprdrive actually uses are listed. All includes are `<hyprland/src/...>`.

## The one model change everything else follows from

- **All per-frame state moved off the renderer into one `Render::CRenderContext`.** `IHyprRenderer::m_renderData`
  and `m_renderPass` are gone. There is one live context: `g_pHyprRenderer->context()` (`render/Context.hpp`).
  Inside a pass element, a transformer or a render-stage event, use the `ctx` you are handed, not `context()`.
- **Every draw/blur/scissor/projection call now takes `CRenderContext& ctx` as its first argument.**
- **No monitor-transform toggle any more.** The whole pass renders into a monitor-oriented (unrotated) buffer of
  `pMonitor->m_transformedSize` with `targetProjection = identity` (`RPT_MONITOR`) and `ctx.m_data.transformDamage = false`
  (OpenGL.cpp ~712 and ~756). The output rotation is applied once, at the final off->out copy (`setProjectionType(ctx, RPT_OUTPUT)`).
  `renderTexture` uses only `invertTransform(tex->m_transform)` (OpenGL.cpp:1648). So every
  `push/popMonitorTransformEnabled` and every "compose the monitor transform if monitorTransformEnabled()" branch is deleted.

Get the context:

```cpp
#include <hyprland/src/render/Context.hpp>
auto& ctx = g_pHyprRenderer->context();   // only outside a pass element/transformer/stage callback
if (!ctx.active()) return;                // no frame in flight (e.g. RENDER_PRE / RENDER_POST)
```

## A. Render data and passes (`render/Context.hpp`, `render/Renderer.hpp`, `render/pass/Pass.hpp`)

| # | OLD (0.56) | NEW (0.57) | Notes |
|---|---|---|---|
| 1 | `g_pHyprRenderer->m_renderData.X` (pMonitor, currentFB, damage, finalDamage, clipBox, renderModif, currentWindow, transformDamage, primarySurfaceUV*) | `ctx.m_data.X` (`Render::SRenderData`, `render/types.hpp`) | Same field names and types; all used fields still exist. |
| 2 | `g_pHyprRenderer->m_renderPass.add(UP)` | `ctx.m_pass.add(std::move(up))` | `m_renderPass.add` bypassed redirection; `ctx.m_pass` keeps that. Use #3 when it should follow a redirect. |
| 3 | `g_pHyprRenderer->addPassElement(UP)` | `IHyprRenderer::addPassElement(ctx, std::move(up))` | Now `static void addPassElement(CRenderContext& ctx, UP<IPassElement>&& element);` adds to `currentPass(ctx)`. |
| 4 | `g_pHyprRenderer->currentPass()` | `IHyprRenderer::currentPass(ctx)` | `static CRenderPass& currentPass(CRenderContext& ctx);` = `ctx.m_currentPass ? *ctx.m_currentPass : ctx.m_pass`. |
| 5 | `g_pHyprRenderer->redirectPass(pass)` | `IHyprRenderer::redirectPass(ctx, pass)` | `static UP<CScopeGuard> redirectPass(CRenderContext& ctx, CRenderPass* pass);` same restore-on-destroy. |
| 6 | `m_renderPass.clear() / empty() / removeAllOfType(s)` | `ctx.m_pass.clear() / empty() / removeAllOfType(s)` | Unchanged signatures. |
| 7 | `pass.render(damage)` (incl. `m_renderPass.render`, `data.pass->render`) | `pass.render(ctx, damage)` | `CRegion render(CRenderContext& ctx, const CRegion& damage_);` |
| 8 | `currentPass().m_passElements` / `m_renderPass.m_passElements` | `IHyprRenderer::currentPass(ctx).m_passElements` / `ctx.m_pass.m_passElements` | Still **private**, still `std::vector<SPassElementData{elementDamage, element, discard}>`; keep the `#define private public` include hack. |
| 9 | `g_pHyprRenderer->m_bBlockSurfaceFeedback` | `ctx.m_blockSurfaceFeedback` | Public member of the context. |
| 10 | `g_pHyprRenderer->m_bRenderingSnapshot` | `ctx.m_renderingSnapshot` | Also `ctx.m_renderingCapture` exists (screencopy). |
| 11 | hand-saved `clipBox/renderModif/damage/finalDamage/m_blurFBShouldRender` + `CScopeGuard` (OverviewPassElement.cpp ~407) | `auto saved = ctx.saveDrawState();` | `[[nodiscard]] CRenderDataScope saveDrawState();` restores the whole `m_data`, backdrop captures and the blur-queued flag (scene resources or `m_blurFBShouldRender`) on scope exit. Must not outlive `ctx`. |
| 12 | `g_pHyprRenderer->draw(SClearData/STexData/SRectData/SBorderData/..., damage)` | `g_pHyprRenderer->draw(ctx, data, damage)` | `void draw(CRenderContext& ctx, const CTexPassElement::SRenderData& data, const CRegion& damage = {});` (same overload set). |

## B. IPassElement (`render/pass/PassElement.hpp`)

Every virtual except `passName()`/`type()` gained `Render::CRenderContext& ctx`. A missing param is the
"marked override, but does not override" error.

| # | OLD | NEW (exact) |
|---|---|---|
| 13 | `std::vector<UP<IPassElement>> draw()` | `virtual std::vector<UP<IPassElement>> draw(Render::CRenderContext& ctx);` |
| 14 | `bool needsLiveBlur()` | `virtual bool needsLiveBlur(Render::CRenderContext& ctx) = 0;` |
| 15 | `bool needsPrecomputeBlur()` | `virtual bool needsPrecomputeBlur(Render::CRenderContext& ctx) = 0;` |
| 16 | `std::optional<CBox> boundingBox()` | `virtual std::optional<CBox> boundingBox(Render::CRenderContext& ctx);` (still monitor-local logical) |
| 17 | `CRegion opaqueRegion()` | `virtual CRegion opaqueRegion(Render::CRenderContext& ctx);` (still monitor-local logical) |
| 18 | `bool undiscardable()` | `virtual bool undiscardable(Render::CRenderContext& ctx);` |
| 19 | `bool disableSimplification()` | `virtual bool disableSimplification(Render::CRenderContext& ctx);` |
| 20 | `void discard()` | `virtual void discard(Render::CRenderContext& ctx);` |
| -- | `passName()`, `type()` | unchanged. New optional `virtual bool requiresFullDamage(Render::CRenderContext& ctx);` and enum value `EK_BACKDROP_SCOPE`. |

Custom elements (`EK_CUSTOM`) are drawn by `IElementRenderer::drawCustom`: it calls `element->draw(ctx)` and then draws
each returned child with the same damage.

Minimal 0.57 element (shape of Ripple / DockTilt / TripFrame / StackBlur / OpenProf marks):

```cpp
#include <hyprland/src/render/pass/PassElement.hpp>
#include <hyprland/src/render/Context.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/OpenGL.hpp>

class CMyElement : public IPassElement {
  public:
    std::vector<UP<IPassElement>> draw(Render::CRenderContext& ctx) override {
        const auto MON = ctx.m_data.pMonitor.lock();
        if (!MON || !ctx.m_data.currentFB) return {};
        CRegion dmg = ctx.m_data.damage.copy().intersect(m_box);
        Render::GL::g_pHyprOpenGL->renderTexture(ctx, m_tex, m_box, {.damage = &dmg, .a = m_alpha});
        return {};
    }
    bool                needsLiveBlur(Render::CRenderContext&) override       { return false; }
    bool                needsPrecomputeBlur(Render::CRenderContext&) override { return false; }
    std::optional<CBox> boundingBox(Render::CRenderContext& ctx) override {
        const auto MON = ctx.m_data.pMonitor.lock();
        return MON ? std::optional{m_box.copy().scale(1.0 / MON->m_scale).round()} : std::nullopt;
    }
    CRegion          opaqueRegion(Render::CRenderContext&) override { return {}; }
    const char*      passName() override { return "CMyElement"; }
    ePassElementType type() override { return EK_CUSTOM; }
    // optional: bool undiscardable(Render::CRenderContext&) override { return true; }
    SP<Render::ITexture> m_tex; CBox m_box; float m_alpha = 1.F;
};
// queue it:  ctx.m_pass.add(makeUnique<CMyElement>(...));  or  IHyprRenderer::addPassElement(ctx, ...)
```

## C. GL draw calls (`render/OpenGL.hpp`, class `Render::GL::CHyprOpenGLImpl`)

| # | OLD | NEW (exact) | Notes |
|---|---|---|---|
| 21 | `renderTexture(tex, box, data)` | `void renderTexture(CRenderContext& ctx, SP<ITexture>, const CBox&, STextureRenderData data);` | `STextureRenderData` gained `blurProportional` between `roundingPower` and `discardActive`; designated initialisers in that order still compile. |
| 22 | `renderBorder(box, grad, SBorderRenderData)` | `void renderBorder(CRenderContext& ctx, const CBox&, const Config::CGradientValueData&, SBorderRenderData data);` | 2-gradient overload likewise gains `ctx`. |
| 23 | `renderRoundedShadow(box, round, power, range, color, a)` | `void renderRoundedShadow(CRenderContext& ctx, const CBox&, int round, float roundingPower, int range, const Config::CGradientValueData& color, float a, const Render::SWindowRenderPresentation& presentation);` | `a` no longer defaulted; pass `Render::SWindowRenderPresentation{}` (`render/WindowRenderPresentation.hpp`) for "no workspace offset/alpha". |
| 24 | `scissor(const CBox&, bool transform = true)` | `void scissor(CRenderContext& ctx, const CBox&, bool transform = true);` | Also `scissor(ctx, const pixman_box32*, bool)` and `(ctx, x, y, w, h, bool)`. `transform=true` applies the inverse **output** transform; inside the pass pass `ctx.m_data.transformDamage` (false), as Hyprland does. |
| 25 | `scissor(nullptr)` | `Render::GL::g_pHyprOpenGL->disableScissor();` | `scissor(ctx, (const pixman_box32*)nullptr)` still disables too; `disableScissor()` avoids the overload ambiguity. |
| 26 | `blend(bool)`, `useShader(prog)`, `makeEGLCurrent()`, `m_cmSupported` | `void blend(bool)`, `WP<CShader> useShader(WP<CShader>)`, `void makeEGLCurrent()` unchanged (public). **`m_cmSupported` is now PRIVATE** (`bool m_cmSupported = true;` in `CHyprOpenGLImpl`'s private block, as is `m_blend`) | Needs the `#define private public` hack. `IHyprRenderer::blend(bool)` / `disableScissor()` also exist as backend-neutral pure virtuals. |
| -- | (new, useful for Genie) | `void renderTextureMesh(CRenderContext& ctx, SP<ITexture>, const CBox&, const std::vector<SMeshRenderVertex>& vertices, STextureRenderData data);` | What CWobbleTransformer uses to draw a deformed window. |

## D. Renderer helpers (`render/Renderer.hpp`, `Render::IHyprRenderer`)

| # | OLD | NEW (exact) | Notes |
|---|---|---|---|
| 27 | `pushMonitorTransformEnabled(true)` / `popMonitorTransformEnabled()` | **delete both** | See the model section; FB contents are already monitor-oriented. |
| 28 | `if (monitorTransformEnabled()) transform = compose(invert(mon->m_transform), transform)` (Ripple, DockTilt) | `auto transform = tex->m_transform;` | Treat as always false. Ripple's `RES = m_pixelSize` should become `m_transformedSize` (the pass buffer's size; differs on rotated outputs). |
| 29 | `projectBoxToTarget(box, tr)` | `Mat3x3 projectBoxToTarget(CRenderContext& ctx, const CBox& box, std::optional<eTransform> transform = std::nullopt);` | |
| 30 | `setProjectionType(RPT_*)` / `setProjectionType(fbSize)` | `void setProjectionType(CRenderContext& ctx, eRenderProjectionType);` / `(CRenderContext& ctx, const Vector2D& fbSize)` | Save/restore with `ctx.m_data.projectionType` (and `fbSize`) or `saveDrawState()`. `setViewport(x,y,w,h)` unchanged (no ctx). |
| 31 | `auto g = bindTempFB(fb)` (returned `UP<CScopeGuard>`) | `[[nodiscard]] CTempFramebufferScope bindTempFB(CRenderContext& ctx, SP<IFramebuffer> fb);` | RAII value, non-copyable: `auto g = g_pHyprRenderer->bindTempFB(ctx, fb);` restores `ctx.m_data.currentFB` and GL bindings. |
| 32 | `blurMainFramebuffer(float a, CRegion* dmg)` -> `SP<ITexture>` | `SP<IFramebuffer> blurMainFramebuffer(CRenderContext& ctx, float strength, const CRegion& originalDamage, const Render::SBlurContext& context = {});` | Returns a **framebuffer**: use `->getTexture()`. Damage by const-ref, not pointer. Blurs the innermost backdrop capture if one is open, else `currentFB`. `SBlurContext` in `render/blur/Provider.hpp`. |
| 33 | `blurFramebuffer(fb, a, CRegion*)` | `virtual SP<IFramebuffer> blurFramebuffer(CRenderContext& ctx, SP<IFramebuffer> source, float strength, const CRegion& originalDamage, const Render::SBlurContext& context = {}) = 0;` | Same return change as #32. |
| 34 | `RES->m_blurFB->getTexture()` (precomputed new_optimizations blur) | `g_pHyprRenderer->getBlurTexture(ctx)` | `SP<ITexture> getBlurTexture(CRenderContext& ctx);` prefers the context's scene resources; `m_blurFB` alone can be stale/unused for scene renders. |
| 35 | `shouldUseNewBlurOptimizations(ls, w)` | `virtual bool shouldUseNewBlurOptimizations(CRenderContext& ctx, PHLLS pLayer, PHLWINDOW pWindow);` | |
| 36 | `g_pHyprRenderer->shouldBlur(PHLWINDOW)` (was protected) | `window->shouldBlur()` | `desktop/view/window/Window.hpp`, public: `bool shouldBlur() const;` and `bool shouldBlur(const Render::SWindowRenderPresentation&) const;`. Layers/popups: `ls->shouldBlur()`, `popup->shouldBlur()`. |
| 37 | `renderWindow(w, mon, now, decorate, mode, ignorePos, standalone)` | `void renderWindow(CRenderContext& ctx, PHLWINDOW, PHLMONITOR, const SWindowRenderPresentation&, const Time::steady_tp&, bool, eRenderPassMode, bool ignorePosition = false, bool standalone = false);` | Still protected (keep the access hack). Hyprland passes `w->presentation().renderPresentation()` (`desktop/view/window/WindowPresentation.hpp`); its `alpha`/`workspaceAlpha` replace reading live animvars. |
| 38 | `renderLayer(ls, mon, now, popups=false, lockscreen=false)` | `void renderLayer(CRenderContext& ctx, PHLLS, PHLMONITOR, const Time::steady_tp&, bool popups = false, bool lockscreen = false, bool workspaceScene = false);` | Protected. |
| 39 | `renderBackground(mon)` | `void renderBackground(CRenderContext& ctx, PHLMONITOR pMonitor);` | Protected. |
| 40 | `MONITOR->resources()->getUnusedWorkBuffer()` (inside a transformer) | `g_pHyprRenderer->getWorkBuffer(ctx, size)` | `SP<IFramebuffer> getWorkBuffer(CRenderContext& ctx, std::optional<Vector2D> size = std::nullopt);` honours the scene's buffer format/CM. The resources overloads still exist. |
| 41 | `MONITOR->workBufferImageDescription()` | `g_pHyprRenderer->workBufferImageDescription(ctx)` | Monitor version still exists; the renderer one accounts for scene resources. |
| -- | `damageMonitor/damageBox/damageWindow/createFB/createTexture/renderText/makeSnapshotFB/glBackend/explicitSyncSupported` | unchanged | `makeSnapshotFB` gained `(PHLWINDOW, eSceneMode)` overloads. |

## E. Stock pass-element data (`render/pass/*.hpp`)

| # | OLD | NEW | Notes |
|---|---|---|---|
| 42 | `CTexPassElement::SRenderData{.flipEndFrame = true}` | drop the field | Removed (with `useMirrorProjection`). It toggled the monitor transform for an FB; obsolete under the new model. New fields to know: `useProvidedDamage`, `blurPatternBox`, `liveBlurOverride`, `blurOwner`, `workspacePresentation`. |
| 43 | `CTransformedWindowPassElement::SData{pass, window, currentBox, blurBox, blur, blurA, ..., motionBlur}` | same + `blurUsesLive, transformedBox, standalone, renderingSnapshot, workspacePresentation` | Designated-initialiser order: `pass, window, currentBox, blurBox, blur, blurUsesLive, blurA, blurRound, blurRoundingPower, transformedBox, motionBlur, standalone, renderingSnapshot, workspacePresentation`. Set `transformedBox` to the drawn box. |

## F. Render-stage events (`event/EventBus.hpp`)

| # | OLD | NEW |
|---|---|---|
| 44 | `Event::bus()->m_events.render.stage.listen([](eRenderStage stage){...})` | `...listen([](Event::SRenderStageEvent e){ ... e.stage ... })` |

```cpp
struct SRenderStageEvent {
    eRenderStage stage = RENDER_PRE;
    PHLMONITOR   monitor;
    // Borrowed only for this synchronous emission; PRE and POST have no live context.
    std::optional<std::reference_wrapper<Render::CRenderContext>> context;
};
```
- Take it **by value** (as hyprtester's plugin does). `e.monitor` replaces reading `m_renderData.pMonitor`.
- `e.context` is set for `RENDER_BEGIN, POST_WALLPAPER, PRE_WINDOWS, PRE_WINDOW, POST_WINDOW, POST_WINDOWS, LAST_MOMENT, POST_MIRROR`;
  `nullopt` for `RENDER_PRE` and `RENDER_POST`. Use `auto& ctx = e.context->get();` and never store it.
- Adding elements in `RENDER_LAST_MOMENT` (Ripple) -> `e.context->get().m_pass.add(...)`.

## G. Window transformers (`render/transformer/Transformer.hpp`, `TransformerList.hpp`, `desktop/view/window/WindowEffectsController.hpp`)

| # | OLD | NEW (exact) | Notes |
|---|---|---|---|
| 45 | `SP<IFramebuffer> transform(SP<IFramebuffer> in)` | `virtual SWindowTransformBuffer transform(CRenderContext& ctx, const SWindowTransformBuffer& in, const SWindowTransformContext& context) = 0;` | `in.framebuffer` holds only `in.box` (monitor-local **pixels**), not a full-monitor FB. `context.currentBox/inputBox/outputBox` are monitor-local **logical**. Return `in` for pass-through; on failure return `{.framebuffer = in.framebuffer, .box = in.box, .success = false}`. |
| 46 | `void amendTransformedRenderData(const CBox& currentBox, SMotionBlurData*)` | `virtual void amendTransformedRenderData(CRenderContext& ctx, const CBox& currentBox, SMotionBlurData* pMotionBlurData, const Render::SWindowRenderPresentation& presentation);` | |
| 47 | `void preWindowRender(CSurfacePassElement::SRenderData*)` | `virtual void preWindowRender(CRenderContext& ctx, CSurfacePassElement::SRenderData* pRenderData);` | Not overridden by Hyprdrive today. |
| -- | (new virtuals, all defaulted) | `int priority() const` (0; wobble 10, motion blur 100, sorted ascending), `bool active() const` (true), `bool blocksDirectScanout() const`, `bool allocatesOutputBuffer() const` (true), `CBox transformedExtents(const CBox&) const`, `CBox sourceBoxForOutput(const CBox&, const CBox&) const`, `CBox transformBoxForDamage(const CBox&) const` | FlightBlur (returns `in`): override `allocatesOutputBuffer()` -> `false`, like `CMotionBlurTransformer`. Genie draws outside the window: override `transformedExtents()` to return window ∪ icon box (logical), or its output is clipped and damage is wrong. Inactive transformers are skipped entirely. |
| 48 | `window->m_transformers` (`std::vector<UP<IWindowTransformer>>`) | `window->effects().transformers()` -> `const UP<Render::CWindowTransformerList>&` | Pointee is mutable: `window->effects().transformers()->emplace<T>(args...)` returns `T*` and re-sorts by priority; `->get<T>()` finds by `dynamic_cast`; `->empty()`; `->removeInactive()`. The vector is private `m_transformers`. **There is no `size()`, `begin()`/`end()` or `erase`**: layout/SizeAck.cpp:80 (`W->m_transformers.size()`) must use `!empty()` (or the private-hack vector). |

Iterating / finding / removing:

```cpp
#include <hyprland/src/desktop/view/window/WindowEffectsController.hpp>
#include <hyprland/src/render/transformer/TransformerList.hpp>
auto& list = window->effects().transformers();            // const UP<CWindowTransformerList>&
auto* t    = list->get<CFlightBlurTransformer>();          // was: loop + dynamic_cast
if (!t) t  = list->emplace<CFlightBlurTransformer>();      // was: emplace_back + back().get()
// remove: make active() return false (e.g. m_dead = true), then
list->removeInactive();
```

## No direct replacement

1. **Moving a transformer out and back (`FlightBlur::CSuspend`, `std::erase_if` in FlightBlur/Genie).**
   `CWindowTransformerList` exposes only `emplace/get/empty/removeInactive`. Hyprland itself never removes a live
   transformer; it lets `active()` go false (motion blur: `state(true).has_value()`), which skips it in `plan()`,
   `transform()` and `amendTransformedRenderData()`, and later calls `removeInactive()`.
   Closest: give `CFlightBlurTransformer` a `m_suspended` flag that `active()` checks; `CSuspend` sets it and its
   destructor clears it (same observable effect, no ownership transfer, `m_held` goes away). For a true erase, the
   existing `#define private public` hack reaches `list->m_transformers` (a `std::vector<UP<IWindowTransformer>>`);
   call `list->sort()` (also private) after re-inserting.
2. **`push/popMonitorTransformEnabled`, `monitorTransformEnabled()`, `STexPassElement::flipEndFrame`.** Removed with
   no switch: the pass buffer is always monitor-oriented and only the final copy rotates (`RPT_OUTPUT`). Drawing a
   Hyprdrive FB back with `tex->m_transform` is the equivalent. If an FB was rendered under `RPT_EXPORT`/`RPT_FB`
   with a rotated projection, draw it with that same projection (`setProjectionType`) rather than a transform flip.
3. **Transformer input as a full-monitor framebuffer (Genie used `m_transformedSize` and `getUnusedWorkBuffer()`).**
   0.57 hands a cropped buffer (`in.box`) sized to the plan's input box. Hyprland's pattern (CWobbleTransformer):
   `OUTPUTCANVAS = pixelBoxForLogical(context.outputBox, scale)`, `OUT = getWorkBuffer(ctx, OUTPUTCANVAS.size())`,
   `auto s = ctx.saveDrawState(); bindFB(ctx, OUT); ctx.m_data.damage = {0,0,w,h}; ctx.m_data.fbSize = OUTPUTCANVAS.size();
   setProjectionType(ctx, RPT_EXPORT);` clear, draw with coordinates translated by `-OUTPUTCANVAS.pos()` and texture
   UVs offset by `in.box.pos()`, return `{.framebuffer = OUT, .box = OUTPUTCANVAS}`. Genie's strip mesh maps onto
   `renderTextureMesh`.
