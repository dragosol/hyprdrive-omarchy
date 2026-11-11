-- omarchy-blocks.lua -- an OPT-IN stand-in for Omarchy 4.5's per-monitor
-- workspace blocks (/usr/share/omarchy/default/hypr/monitor-workspaces.lua,
-- omarchy-settings 4.5.0-3), for the headless rig, which has no Omarchy.
-- Loaded by tests/rig/headless/blocks.lua (hl.sh start --config ...), never by
-- the default test config.
--
-- It does what the real script does to ids, from the reading in the design
-- (rigs-backup/hyprdrive-omarchy-blocks-design-2026-11-11.md, section (a)):
--   * every monitor gets a block of SLOTS ids, kept in
--     $XDG_STATE_HOME/omarchy/monitor-workspaces (block \t connector \t
--     identity; identity = the connector here, the rig has no EDIDs); a new
--     monitor takes the lowest free block. The file is read back at load (the
--     Lua state is rebuilt on every reload), so a test can pre-seed it.
--   * on hyprland.start, config.reloaded, monitor.added (and monitor.removed,
--     1 ms later) it reconciles, touching ONLY workspaces with windows:
--       place(): one in another CONNECTED monitor's block is adopted into the
--                block of the monitor it is on (same slot if free, else the
--                first free one);
--       merge(): one in a block NO connected monitor owns (a guest) gets
--                append_slot (highest used id in the block + 1, else any free
--                one, else it stays), and a record in
--                monitor-workspaces-merged (id \t original \t origin identity).
--     It never closes gaps, never touches empty workspaces, never shifts.
--   * renumbering is hl.dsp.workspace.change_id: Hyprland emits
--     changeworkspaceid>>old,new on socket2, which is what the gates count.
-- Not emulated: restore() (a guest's origin monitor coming back), own_views()
-- (switching a monitor to its home), Super+N. The rig's own host window (WL-*)
-- is not a monitor here.
--
-- It registers itself as package.loaded["default.hypr.monitor-workspaces"],
-- so Hyprdrive's real publisher in 00-plugin.lua sees blocks on, exactly as on
-- Omarchy.

local SLOTS = 10
local home = os.getenv("HOME") or ""
local state = os.getenv("XDG_STATE_HOME")
if not state or state == "" then
    state = home .. "/.local/state"
end
local DIR = state .. "/omarchy"
local FILE = DIR .. "/monitor-workspaces"
local MERGED = DIR .. "/monitor-workspaces-merged"

local M = { slots = SLOTS }
local blocks = {} -- identity -> block
local order = {}  -- identities, in file order
local merged = {} -- id -> { original, identity }

local function read_lines(path)
    local f = io.open(path, "r")
    if not f then
        return {}
    end
    local out = {}
    for line in f:lines() do
        if line ~= "" then
            out[#out + 1] = line
        end
    end
    f:close()
    return out
end

local function fields(line)
    local out = {}
    for field in (line .. "\t"):gmatch("([^\t]*)\t") do
        out[#out + 1] = field
    end
    return out
end

for _, line in ipairs(read_lines(FILE)) do
    local f = fields(line)
    local b = tonumber(f[1])
    local id = f[3] or f[2] or ""
    if b and id ~= "" and not blocks[id] then
        blocks[id] = b
        order[#order + 1] = id
    end
end
for _, line in ipairs(read_lines(MERGED)) do
    local f = fields(line)
    local id, orig = tonumber(f[1]), tonumber(f[2])
    if id and orig then
        merged[id] = { orig, f[3] or "" }
    end
end

local function write(path, text)
    local old = table.concat(read_lines(path), "\n")
    if old ~= "" then
        old = old .. "\n"
    end
    if old == text then
        return -- only when the content changes, as save() does
    end
    local f = io.open(path, "w")
    if f then
        f:write(text)
        f:close()
    end
end

local function monitors()
    local out = {}
    for _, m in ipairs(hl.get_monitors() or {}) do
        if m and m.name and not m.is_mirror and not tostring(m.name):match("^WL%-") then
            out[#out + 1] = m
        end
    end
    return out
end

local function block_of_id(id)
    return (id - 1) // SLOTS
end

local function assign()
    local connected = {}
    for _, m in ipairs(monitors()) do
        connected[m.name] = true
        if blocks[m.name] == nil then
            local taken = {}
            for _, b in pairs(blocks) do
                taken[b] = true
            end
            local b = 0
            while taken[b] do
                b = b + 1
            end
            blocks[m.name] = b
            order[#order + 1] = m.name
        end
    end
    local lines = {}
    for _, id in ipairs(order) do
        lines[#lines + 1] = blocks[id] .. "\t" .. (connected[id] and id or "") .. "\t" .. id
    end
    write(FILE, table.concat(lines, "\n") .. "\n")
    return connected
end

local function save_merged()
    local ids = {}
    for id in pairs(merged) do
        ids[#ids + 1] = id
    end
    table.sort(ids)
    local lines = {}
    for _, id in ipairs(ids) do
        lines[#lines + 1] = id .. "\t" .. merged[id][1] .. "\t" .. merged[id][2]
    end
    if #lines == 0 then
        os.remove(MERGED)
    else
        write(MERGED, table.concat(lines, "\n") .. "\n")
    end
end

local function owner(block, connected)
    for id, b in pairs(blocks) do
        if b == block and connected[id] then
            return id
        end
    end
    return nil
end

local function existing()
    local ids = {}
    for _, ws in ipairs(hl.get_workspaces() or {}) do
        if ws.id and ws.id > 0 then
            ids[ws.id] = true
        end
    end
    return ids
end

local function renumber(old, new)
    hl.dispatch(hl.dsp.workspace.change_id({ workspace = tostring(old), id = new }))
    if merged[old] then
        merged[new] = merged[old]
        merged[old] = nil
    end
end

function M.reconcile()
    local connected = assign()
    local ids = existing()
    local function free(id)
        return not ids[id]
    end
    local function take(old, new)
        renumber(old, new)
        ids[old] = nil
        ids[new] = true
    end
    for _, ws in ipairs(hl.get_workspaces() or {}) do
        local id = ws.id
        local mon = ws.monitor and ws.monitor.name
        if id and id > 0 and mon and blocks[mon] and (ws.windows or 0) > 0 then
            local mine = blocks[mon]
            local b = block_of_id(id)
            local own = owner(b, connected)
            if own and own ~= mon then
                -- place(): adopt into this monitor's block, same slot if free
                local want = mine * SLOTS + (id - 1) % SLOTS + 1
                if not free(want) then
                    want = nil
                    for s = 1, SLOTS do
                        if free(mine * SLOTS + s) then
                            want = mine * SLOTS + s
                            break
                        end
                    end
                end
                if want then
                    take(id, want)
                end
            elseif not own then
                -- merge(): a guest; append_slot in this monitor's block
                local high = nil
                for x in pairs(ids) do
                    if block_of_id(x) == mine and (not high or x > high) then
                        high = x
                    end
                end
                local want = nil
                if not high then
                    want = mine * SLOTS + 1
                elseif high + 1 <= mine * SLOTS + SLOTS and free(high + 1) then
                    want = high + 1
                else
                    for s = 1, SLOTS do
                        if free(mine * SLOTS + s) then
                            want = mine * SLOTS + s
                            break
                        end
                    end
                end
                if want then
                    local origin = ""
                    for ident, ob in pairs(blocks) do
                        if ob == b then
                            origin = ident
                        end
                    end
                    local rec = merged[id]
                    take(id, want)
                    if not rec then
                        merged[want] = { id, origin }
                    end
                end
            end
        end
    end
    save_merged()
end

for _, e in ipairs({ "hyprland.start", "config.reloaded", "monitor.added" }) do
    hl.on(e, function()
        pcall(M.reconcile)
    end)
end
hl.on("monitor.removed", function()
    hl.timer(function()
        pcall(M.reconcile)
    end, { type = "oneshot", timeout = 1 })
end)

package.loaded["default.hypr.monitor-workspaces"] = M
return M
