-- Nested test compositor: the DEVELOPMENT build, straight from the repo.
hl.plugin.load("/home/dragos/Projects/hyprdrive-omarchy/hyprdrive.so")
hl.config({ plugin = { hyprdrive = { dock = { enabled = true, autohide = false } } } })
hl.monitor({ output = "", mode = "1280x800@60", position = "0x0", scale = 1 })
-- Vista/Aero-style glass (user, 2026-09-29): soft blur, a touch of brightness
-- and vibrancy, LIVE (new_optimizations off). hyprdrive follows these.
hl.config({ decoration = { blur = { enabled = true, size = 6, passes = 3, noise = 0.02, contrast = 1.0, brightness = 1.15, vibrancy = 0.25, vibrancy_darkness = 0.0, new_optimizations = false, xray = false, popups = true } } })
