-- Load Hyprdrive fragments from ~/.config/hypr/hyprdrive/ in filename order.
-- Sourced from hyprland.lua after Omarchy defaults, so plugin keys and binds
-- land on top of the desktop without rewriting Omarchy's files.

local dest = (os.getenv("XDG_CONFIG_HOME") or ((os.getenv("HOME") or "") .. "/.config")) .. "/hypr/hyprdrive"
local handle = io.popen("find '" .. dest:gsub("'", "'\\''") .. "' -maxdepth 1 -type f -name '*.lua' -printf '%f\\n' 2>/dev/null | sort")
if not handle then
  return
end

for filename in handle:lines() do
  dofile(dest .. "/" .. filename)
end
handle:close()
