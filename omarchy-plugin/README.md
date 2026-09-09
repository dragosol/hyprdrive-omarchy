# Hyprdrive for Omarchy

Bar button and a large control panel for the Hyprdrive compositor plugin.

- Overview: compositor animations, Hyprdrive motion scale, Super-tap
- Dock: square / rounded pill, autohide, icon size, minimize flight
- Gestures: a full trackpad map (fingers, direction, action, modifier keys, scale, inhibit) — meant to replace the old trackpad-gestures plugin
- Textures: mipmap prefilter, live frost, blur follow
- Power: save and auto-apply separate AC / battery profiles

The panel talks to `bin/hdpanel`, which previews with `hyprctl keyword` and persists into `~/.config/hypr/hyprdrive/40-panel.lua` and `35-gestures.lua`.
