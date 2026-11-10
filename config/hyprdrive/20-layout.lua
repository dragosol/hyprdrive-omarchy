-- hyprdrive fragment 20-layout -- geometry of the overview itself.
--
-- MACHINE-OWNED above the raw-snippet marker; USER-OWNED below it. Everything
-- above BEGIN is rewritten wholesale when this fragment is regenerated;
-- everything between BEGIN and END is carried across verbatim.

if hyprdrive_loaded() then
    -- A window that fixes its own size tiles at that size (Super+T,
    -- OwnSizeTile.hpp), and a tile smaller than it must not squeeze it: its
    -- program fights a smaller size with a configure storm. This clamps a
    -- tiled window to its min/max size rules -- only windows that have one,
    -- which are those (and any min_size/max_size rule of your own).
    hl.config({ misc = { size_limits_tiled = true } })
    hl.config({
        plugin = {
            hyprdrive = {
                scale         = 0.82,
                layout        = "horizontal",
                workspace_gap = 40,
                dock = {
                    -- Super+1..0 stay Omarchy workspace focus. The plugin still
                    -- has the hotkey dispatcher; this fork does not steal the keys.
                    hot_keys = false,
                },
            },
        },
    })
end
