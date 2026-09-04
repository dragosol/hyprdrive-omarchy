-- hyprdrive fragment 30-keybinds -- the overview submap.
--
-- MACHINE-OWNED above the raw-snippet marker; USER-OWNED below it. Everything
-- above BEGIN is rewritten wholesale when this fragment is regenerated;
-- everything between BEGIN and END is carried across verbatim.
--
-- Tapping SUPER alone toggles the overview. There is deliberately NO bind here:
-- the plugin detects the tap itself (main.cpp registerTapSuper), because on
-- 0.56 every bind form on SUPER_L fires the same whether SUPER was tapped or
-- held as a modifier, so a bind fires on every SUPER+<key> shortcut too.
-- Disable with: plugin:hyprdrive:tap_super = false
--
-- SUPER+SPACE stays the Omarchy menu. A tap (press and release, no other key)
-- is not a chord, so the two do not fight.
--
-- SUPER+1..0 stay Omarchy workspace focus. This fork does not rebind them to
-- dock slots.

-- While the overview is open the plugin enters this submap (it does so only
-- because these binds exist). Inside it the global SUPER+mouse:272 window.drag
-- bind cannot match, so a click with SUPER still held reaches the overview
-- instead of being eaten by the drag bind.
if hyprdrive_loaded() then
    hl.define_submap("hyprdrive", function()
        hl.bind("mouse:272",         hl.plugin.hyprdrive.window([[click]]), { mouse = true, description = "Overview: select" })
        hl.bind("SUPER + mouse:272", hl.plugin.hyprdrive.window([[click]]), { mouse = true, description = "Overview: select (SUPER held)" })
    end)

    -- 3-finger swipe up opens the overview and follows the fingers; swipe
    -- down closes it. Reversing mid-gesture undoes the motion. 3-finger
    -- horizontal stays Omarchy workspace switching. Super tap still toggles.
    hl.plugin.hyprdrive.gesture({ fingers = 3, direction = "up" })
    hl.plugin.hyprdrive.gesture({ fingers = 3, direction = "down" })
end
