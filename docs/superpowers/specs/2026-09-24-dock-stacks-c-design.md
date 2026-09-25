# Dock Stacks, sub-project C: our own icons

Date: 2026-09-24
Status: user pre-approved "make and execute plans B and C"; the decisions below were made for them and can be revisited.
Builds on: `2026-09-24-dock-stacks-a-design.md`, `-b-design.md`

## What the user asked for

High-fidelity empty-stack and Trash artwork, "inspired by them but not exactly
them so we don't actually steal anything".

## Decisions

- **Original SVGs**, drawn for hyprdrive, in `assets/icons/`:
  - `trash-empty.svg`: a frosted, ribbed glass bin seen slightly from above
    (grooves with lit edges, a metallic rim, a dark opening, a soft floor
    shadow). It is not the macOS wire-mesh can.
  - `trash-full.svg`: the same bin with crumpled paper balls inside the
    opening. The rim's front lip is drawn over them.
  - `stack-empty.svg`: an open frosted tray with two dashed ghost cards where
    the pile would be. The cards take the **theme accent** (`#ACCENT` is
    replaced when the texture is built), in keeping with "adopt
    Omarchy/Hyprland theming".
- **Shipped inside the .so** with C23/C++26 `#embed`, so the plugin stays a
  single file. The icons are rasterised by hyprgraphics (librsvg) from memory
  at 192 px, and cached per accent colour.
- **The empty stack keeps its identity:** the folder's own themed icon sits
  small on the tray's front, so an empty Downloads still reads as Downloads.
- **Config:** `plugin:hyprdrive:dock:builtin_icons` (bool, default `true`).
  `false` goes back to the icon theme's Trash and folder.
- A name starting `hyprdrive-` never reaches the icon theme: `appIcon`
  answers it from `BuiltinIcons`, so every surface that draws the Trash item
  (dock, menus, previews) gets the same artwork.

## Testing

- `scripts/builtin_icons_test.cpp` (host): every icon exists and takes the
  accent, and librsvg renders each one with real coverage.
- Nested Hyprland: the full Trash and the empty stack appear in the dock, and
  turning `builtin_icons` off at runtime falls back to the theme's icons.
