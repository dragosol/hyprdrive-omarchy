-- hyprdrive fragment 10-theme -- appearance is Hyprland's, so this file pins
-- nothing.
--
-- The plugin reads live compositor values (see Config.hpp):
--   accent     general:col.active_border          -> highlightColor()
--   cards      decoration:rounding                -> themeRounding()
--   search     same plate as the dock, rounding   -> themePlate() / drawSearch()
--   results    from decoration:rounding (0 stays
--   strip      squared). Dock rounding stays 24.
--   blur       decoration:blur:*                  -> dock/search frost; off when
--              decoration:blur:enabled is false
--   canvas     Omarchy lockscreen MultiEffect     -> dimmed wallpaper behind cards
--              (blurMax 128), not window blur
--   shadows    decoration:shadow:*                -> plugin shadow at -1
--   dock pill  Omarchy colors.toml background     -> dockBackgroundColor()
--              at Omarchy default-opacity, else misc:background_color
--   type       Omarchy font (fontconfig monospace) -> themeFontFamily()
--   dock dots  the same accent                    -> dockIndicatorColor()
--   dock radius dock:corner_radius (24)           -> dockCornerRadius()
--               the pill and its highlights keep the original Dash-to-Dock
--               rounding; they do not follow decoration:rounding.
--
-- Omarchy writes those Hyprland keys from the current theme and then
-- `hyprctl reload`. Do not set col_highlight or dock colours here: a pin would
-- freeze the last value and stop following `omarchy theme set`.
-- To pin deliberately, put the key in the raw-snippet block below.

if hyprdrive_loaded() then
    hl.config({
        plugin = {
            hyprdrive = {
                wallpaper = 0,
            },
        },
    })
end
