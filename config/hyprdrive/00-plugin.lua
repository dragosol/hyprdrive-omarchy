-- hyprdrive fragment 00-plugin -- load the plugin, and the guard every later
-- fragment uses.
--
-- MACHINE-OWNED above the raw-snippet marker; USER-OWNED below it. Everything
-- above BEGIN is rewritten wholesale when this fragment is regenerated;
-- everything between BEGIN and END is carried across verbatim. Put hand edits
-- THERE, not here.

local function file_exists(path)
    local f = io.open(path, "r")
    if f then
        f:close()
        return true
    end
    return false
end

local home = os.getenv("HOME") or ""
local so = os.getenv("HYPRDRIVE_SO")
if not (so and file_exists(so)) then
    for _, path in ipairs({
        home .. "/.local/lib/hyprdrive/hyprdrive.so",
        home .. "/Projects/hyprdrive-omarchy/hyprdrive.so",
    }) do
        if file_exists(path) then
            so = path
            break
        end
    end
end

-- hl.plugin.load is PARSE-TIME only: this line is what makes the plugin survive
-- a reboot, and it is a no-op on `hyprctl reload`. Manual equivalent:
--   hyprctl plugin load ~/.local/lib/hyprdrive/hyprdrive.so
if so then
    hl.plugin.load(so)
end

-- hl.config runs at parse time and the plugin:hyprdrive:* keys do not exist
-- until the .so has registered them, so an unguarded config block throws
-- "unknown config key" for every property on boot and on reload.
-- pairs() not ipairs(): omarchy-menu-keybindings scans this file with a stub
-- `hl` whose __index always returns a value, and ipairs never terminates on
-- that table. Super+K (the keybindings menu) hung in this loop.
function hyprdrive_loaded()
    local ok, plugins = pcall(function()
        return hl.get_loaded_plugins()
    end)
    if not ok or type(plugins) ~= "table" then
        return false
    end
    for _, v in pairs(plugins) do
        if (type(v) == "table" and v.name or v) == "hyprdrive" then
            return true
        end
    end
    return false
end

-- OMARCHY'S PER-MONITOR WORKSPACE BLOCKS (4.5+: monitor-workspaces.lua gives
-- each monitor ten desktop numbers of its own). The plugin numbers each
-- monitor's desktops inside its block when Omarchy's module is loaded, and on
-- one global row otherwise. Said at parse and again on hyprland.start /
-- config.reloaded, so it does not matter whether this fragment is sourced
-- before or after omarchy.lua; the plugin forgets it before every parse.
local function hd_blocks()
    local mw = package.loaded["default.hypr.monitor-workspaces"]
    local slots = type(mw) == "table" and (tonumber(mw.slots) or 10) or 0
    pcall(function()
        hl.plugin.hyprdrive.workspace_blocks(slots)
    end)
end
hd_blocks()
pcall(function()
    hl.on("hyprland.start", hd_blocks)
    hl.on("config.reloaded", hd_blocks)
end)
