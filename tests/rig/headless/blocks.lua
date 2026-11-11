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
-- Without HD_BLOCKS_LUA (or HD_OMARCHY_MW, below) it is the vanilla test config
-- plus the fragment (blocks off: the implicit row). HD_LAYOUT=mosaic puts the
-- layout in the config FILE, as a user's config does (an `eval` does not
-- survive the config reload every plugin unload/load makes).
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

-- HD_OMARCHY_MW=/abs/monitor-workspaces.lua: Omarchy's REAL script (a copy of
-- /usr/share/omarchy/default/hypr/monitor-workspaces.lua; Omarchy's file, never
-- in this repository) instead of the emulation. It needs only what omarchy.lua
-- gives it: require("default.hypr.paths") (state_home) and the global `o`
-- (o.workspaces = M; o.bind_expressions for its Super+N actions; o.shell_quote
-- when the state dir is missing). Loaded as require() would, under its module
-- name, so the publisher in 00-plugin.lua finds it. The rig's monitors have no
-- EDID: their identity is the connector, as in the emulation's seeds.
local real = os.getenv("HD_OMARCHY_MW")
local emu = os.getenv("HD_BLOCKS_LUA")
if real and real ~= "" then
    local home = os.getenv("HOME") or ""
    local state = os.getenv("XDG_STATE_HOME")
    if not state or state == "" then
        state = home .. "/.local/state"
    end
    package.loaded["default.hypr.paths"] = { home = home, config_home = home .. "/.config", state_home = state, omarchy_path = "/usr/share/omarchy" }
    o = o or {}
    o.bind_expressions = o.bind_expressions or {}
    o.shell_quote = o.shell_quote or function(s)
        return "'" .. (tostring(s):gsub("'", "'\\''")) .. "'"
    end
    -- HD_LUA_ERRLOG=/path: every error raised in the script's hl.on / hl.timer
    -- callbacks is appended there, then raised again unchanged. Hyprland only
    -- pops a notification for a runtime error, and any `hyprctl eval` clears
    -- configerrors, so a gate could not see one otherwise.
    local errlog = os.getenv("HD_LUA_ERRLOG")
    local on, timer = hl.on, hl.timer
    if errlog and errlog ~= "" then
        local function guarded(what, fn)
            return function(...)
                local ok, err = xpcall(fn, function(e)
                    return debug and debug.traceback and debug.traceback(tostring(e), 2) or tostring(e)
                end, ...)
                if not ok then
                    local f = io.open(errlog, "a")
                    if f then
                        f:write(what .. ": " .. tostring(err) .. "\n")
                        f:close()
                    end
                    error(err, 0)
                end
            end
        end
        hl.on = function(name, fn)
            return on(name, guarded("hl.on(" .. tostring(name) .. ")", fn))
        end
        hl.timer = function(fn, opts)
            return timer(guarded("hl.timer", fn), opts)
        end
    end
    package.loaded["default.hypr.monitor-workspaces"] = dofile(real)
    hl.on = on -- the timer wrapper stays: the script arms its timers later
    -- proof for the gate that THIS ran (the real script sets o.workspaces)
    local mark = io.open(state .. "/omarchy-real.loaded", "w")
    if mark then
        mark:write(real .. (o.workspaces == package.loaded["default.hypr.monitor-workspaces"] and " o.workspaces\n" or " ?\n"))
        mark:close()
    end
elseif emu and emu ~= "" then
    dofile(emu)
end
local frag = os.getenv("HD_PLUGIN_LUA")
if frag and frag ~= "" then
    dofile(frag)
end
local layout = os.getenv("HD_LAYOUT")
if layout and layout ~= "" then
    hl.config({ general = { layout = layout } })
end
