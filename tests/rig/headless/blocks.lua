-- blocks.lua -- a headless-rig config with Omarchy's per-monitor workspace
-- blocks EMULATED (omarchy-blocks.lua) and Hyprdrive loaded through its real
-- 00-plugin.lua fragment, so the block publisher runs as on Omarchy:
--
--   hl.sh start --config tests/rig/headless/blocks.lua \
--       --env HD_BLOCKS_LUA=tests/rig/headless/omarchy-blocks.lua \
--       --env HD_PLUGIN_LUA=config/hyprdrive/00-plugin.lua \
--       --env HYPRDRIVE_SO=/abs/hyprdrive.so --env XDG_STATE_HOME=/tmp/hl-<name>/state
--
-- (hl.sh copies each file hyprtest cannot read and hands Hyprland the copy.)
-- XDG_STATE_HOME must be a directory hyprtest owns: the emulation writes
-- omarchy/monitor-workspaces(-merged) there and the plugin watches it. A test
-- may pre-seed monitor-workspaces to choose each output's block.
-- Without HD_BLOCKS_LUA it is the vanilla test config plus the fragment
-- (blocks off: the implicit row).
hl.monitor({ output = "HEADLESS-1", mode = "1920x1080@60", position = "0x0", scale = 1 })
hl.monitor({ output = "HEADLESS-2", mode = "1920x1080@60", position = "1920x0", scale = 1 })
hl.monitor({ output = "WL-1", mode = "1920x1080@60", position = "-1920x0", scale = 1 })

hl.config({
    general = { gaps_in = 5, gaps_out = 10, border_size = 2, layout = "dwindle" },
    decoration = { rounding = 8 },
    animations = { enabled = true },
    misc = { disable_hyprland_logo = true, disable_splash_rendering = true, force_default_wallpaper = 0, background_color = 0x202830 },
    xwayland = { enabled = true },
    debug = { disable_logs = false, enable_stdout_logs = false },
})
hl.env("XCURSOR_SIZE", "24")

local emu = os.getenv("HD_BLOCKS_LUA")
if emu and emu ~= "" then
    dofile(emu)
end
local frag = os.getenv("HD_PLUGIN_LUA")
if frag and frag ~= "" then
    dofile(frag)
end
