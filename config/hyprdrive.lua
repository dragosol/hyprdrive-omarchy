-- Load Hyprdrive fragments from ~/.config/hypr/hyprdrive/ in filename order.
-- Sourced from hyprland.lua after Omarchy defaults, so plugin keys and binds
-- land on top of the desktop without rewriting Omarchy's files.
--
-- ONE BAD FRAGMENT NEVER TAKES THE OTHERS WITH IT. Each runs under pcall. On
-- 0.56 a runtime error inside a require()d file is caught by Hyprland's safe
-- require and listed in `hyprctl configerrors`, and the rest of hyprland.lua
-- still runs -- but this loop used to dofile() the fragments bare, so the
-- first error also skipped every fragment after it (measured headless: an
-- error in 25-rules.lua dropped 30-keybinds.lua, the overview submap and the
-- 3-finger gestures). Now every fragment runs, and the failures are raised
-- together at the end, as ONE error naming each file, so they still reach
-- configerrors (and the red banner) instead of disappearing. The native build
-- has done this for a while; Omarchy's copy was left behind (review, 2026-11-09).

local dest = (os.getenv("XDG_CONFIG_HOME") or ((os.getenv("HOME") or "") .. "/.config")) .. "/hypr/hyprdrive"
local handle = io.popen("find '" .. dest:gsub("'", "'\\''") .. "' -maxdepth 1 -type f -name '*.lua' -printf '%f\\n' 2>/dev/null | sort")
if not handle then
  return
end

local failed = {}
for filename in handle:lines() do
  local ok, err = pcall(dofile, dest .. "/" .. filename)
  if not ok then
    failed[#failed + 1] = filename .. ": " .. tostring(err)
  end
end
handle:close()

if #failed > 0 then
  error("hyprdrive: " .. #failed .. " fragment(s) in " .. dest .. " failed (the others loaded):\n  " .. table.concat(failed, "\n  "), 0)
end
