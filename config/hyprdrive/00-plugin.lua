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
function hyprdrive_loaded()
    for _, v in ipairs(hl.get_loaded_plugins() or {}) do
        if (type(v) == "table" and v.name or v) == "hyprdrive" then
            return true
        end
    end
    return false
end
