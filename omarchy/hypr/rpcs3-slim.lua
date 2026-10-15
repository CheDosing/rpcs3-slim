-- RPCS3 Slim window rules for Omarchy.
-- Loaded from ~/.config/hypr/hyprland.lua by `rpcs3-slim-omarchy setup`.

-- Main window and game window: fully opaque, keep the screen awake while a game runs fullscreen
o.window("rpcs3", { tag = "-default-opacity", opacity = "1 1", idle_inhibit = "fullscreen" })

-- Dialogs (Game Update Downloader, package install, settings): center them on the screen instead of over the
-- tiled main window, where they can run off the edge
o.window({ class = "rpcs3", float = true }, { center = true })
