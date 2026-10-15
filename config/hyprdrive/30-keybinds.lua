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
-- dock slots (the panel's Keybinds section can, with dock:hot_keys).

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
    -- down closes it. Reversing mid-gesture undoes the motion. Super tap
    -- still toggles. When the settings panel has written 35-gestures.lua it
    -- owns the whole map (including workspace / close / zoom), so skip these
    -- factory defaults or they would double-register on reload.
    local dest = (os.getenv("XDG_CONFIG_HOME") or ((os.getenv("HOME") or "") .. "/.config")) .. "/hypr/hyprdrive"
    local owned = io.open(dest .. "/35-gestures.lua", "r")
    if owned then
        owned:close()
    else
        hl.plugin.hyprdrive.gesture({ fingers = 3, direction = "up" })
        hl.plugin.hyprdrive.gesture({ fingers = 3, direction = "down" })
    end

    -- Super+scroll walks the desktops by NUMBER, the empty ones included
    -- (1 .. max(10, highest), no wrap), one desktop per wheel notch -- in place
    -- of Omarchy's Super+scroll, which skips the empty ones (e+1 / e-1). A
    -- second bind of a chord does not replace the first (both would fire), so
    -- Omarchy's goes first, as Mosaic does with Super+L. The actions are built
    -- BEFORE anything is unbound: a plugin that cannot make them leaves
    -- Omarchy's binds alone. Off: plugin:hyprdrive:binds:scroll_desktops =
    -- false through the settings panel, whose 40-panel.lua then owns this bind
    -- (marked "hyprdrive:binds owned") and this default steps aside.
    local panel = io.open(dest .. "/40-panel.lua", "r")
    local panel_owns_binds = false
    if panel then
        panel_owns_binds = (panel:read("a") or ""):find("hyprdrive:binds owned", 1, true) ~= nil
        panel:close()
    end
    if not panel_owns_binds then
        local ok_next, desktop_next = pcall(hl.plugin.hyprdrive.desktop, "next")
        local ok_prev, desktop_prev = pcall(hl.plugin.hyprdrive.desktop, "prev")
        if ok_next and ok_prev then
            hl.unbind("SUPER + mouse_down")
            hl.unbind("SUPER + mouse_up")
            hl.bind("SUPER + mouse_down", desktop_next, { description = "Next desktop (empty ones too)" })
            hl.bind("SUPER + mouse_up", desktop_prev, { description = "Previous desktop (empty ones too)" })
        end
    end
end
