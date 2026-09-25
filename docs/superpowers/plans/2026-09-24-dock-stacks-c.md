# Dock Stacks C — our own icons — Implementation Plan

> Executed natively in the session that wrote it.

**Spec:** `docs/superpowers/specs/2026-09-24-dock-stacks-c-design.md`

1. **Artwork:** write `assets/icons/{trash-empty,trash-full,stack-empty}.svg`.
   Render them with `rsvg-convert` on a dark background, look at the result,
   and iterate.
2. **BuiltinIcons.hpp/.cpp:** use `#embed` for the three icons, and add
   `svg(name, accentHex)`. Add the Makefile dependency on `assets/icons/*.svg`.
   Add the host test `scripts/builtin_icons_test.cpp`, rendering through
   librsvg.
3. **OverviewRender:** add `builtinIcon(name, px)`. It goes through
   `CImage(span, IMAGE_FORMAT_SVG, size)`, is keyed on name + px + accent, and
   is cleared in `clearTextCache`. `appIcon` routes `hyprdrive-` names to it.
4. **Config key** `dock:builtin_icons` (default true), then regenerate CONFIG.md.
5. **Dock:** the Trash item's icon becomes `hyprdrive-trash-{empty,full}`, and
   the key goes into `itemsKey().config`. **StackIcon:** the empty branch draws
   `hyprdrive-stack-empty`, with the folder's icon on the front.
6. `make all`, then check in nested (on, and off at runtime), then commit.
