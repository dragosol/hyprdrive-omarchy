-- hyprdrive fragment 10-theme -- appearance is Hyprland's, so this file pins
-- nothing.
--
-- The plugin reads live compositor values (see Config.hpp):
--   accent     general:col.active_border          -> highlightColor()
--   corners    decoration:rounding                -> themeRounding()
--   blur       decoration:blur:*                  -> dock frost at -1
--   shadows    decoration:shadow:*                -> plugin shadow at -1
--   dock pill  misc:background_color              -> dockBackgroundColor()
--   dock dots  the same accent                    -> dockIndicatorColor()
--
-- Omarchy writes those Hyprland keys from the current theme and then
-- `hyprctl reload`. Do not set col_highlight, dock colours, or rounding here:
-- a pin would freeze the last value and stop following `omarchy theme set`.
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
