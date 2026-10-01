# Hyprdrive for Omarchy

The bar button and settings panel for the Hyprdrive compositor plugin.

- **Overview**: Super tap, Mission Control grid, desktop strip, size, spacing, wallpaper blur, search extras
- **Dock**: visibility, contents, appearance and frost
- **Animation**: one curve preset (Hyprdrive, Serpantinum, Smooth, Swift, Springy, Linear) and one speed for everything hyprdrive animates except the dock stacks, plus per-feature timing
- **Motion blur**: strength (in eighths) and fade shape for every animation hyprdrive smears, and sample counts
- **Gestures**: the full trackpad map (fingers, direction, action, modifier keys, distance, inhibit)
- **Performance**: smooth frames, live frost, preview smoothing, and AC / battery profiles

Every section has **Restore defaults**. Type anywhere to search settings; Alt+1..6 jumps between sections.

`Panel.qml` is only the bar button: the panel itself (`HyprdrivePanel.qml`, laid out by `Schema.js`) is loaded when it opens and destroyed when it closes, so it holds no memory while closed.

The panel talks to `bin/hdpanel`, which applies values live and persists them into `~/.config/hypr/hyprdrive/40-panel.lua` and `35-gestures.lua`.
