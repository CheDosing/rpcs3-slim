# RPCS3 Slim for Omarchy

A native build of RPCS3 Slim for [Omarchy](https://omarchy.org/) (Arch Linux + Hyprland). It has every feature of the Windows build (see [README-LITE.md](README-LITE.md)), plus a few changes that make it fit into Omarchy.

## Install

```bash
git clone -b omarchy https://github.com/CheDosing/rpcs3-slim.git
cd rpcs3-slim/omarchy
makepkg -si
rpcs3-slim-omarchy setup   # optional: Hyprland window rules
```

This builds the `omarchy` branch from GitHub (not your local checkout) and installs the `rpcs3-slim-git` package. It replaces any other `rpcs3` package and uses the same `~/.config/rpcs3` settings folder. Launch it from the Omarchy app launcher (**RPCS3 Slim**) or with `rpcs3`.

CI builds a portable package on every push to `omarchy`. You can download it from the run's **Artifacts** and install it with `sudo pacman -U rpcs3-slim-git-*.pkg.tar.zst`.

## What's different on Omarchy

| Area | Behavior |
|---|---|
| Theme | The default **Mocha** UI, including the Game Update Downloader, uses your current Omarchy theme's colors (`~/.local/state/omarchy/current/theme/colors.toml`), including light themes. It updates live when you run `omarchy theme set`. To get plain Mocha back, pick **Mocha** in `Configuration → GUI → Stylesheets`, or launch with `RPCS3_NO_OMARCHY_THEME=1`. |
| Game updates | `Utilities → Game Update Downloader...` is a native window in the same theme as the rest of the app. rusty-psn runs hidden behind it (`/usr/lib/rpcs3-slim/rusty-psn-cli`). |
| Window rules | `rpcs3-slim-omarchy setup` adds one line to `~/.config/hypr/hyprland.lua` (a backup is made first). It keeps the emulator and game windows fully opaque, stops the screen from blanking while a game is fullscreen, and centers RPCS3 dialogs on the screen. `rpcs3-slim-omarchy remove` takes the line out again. |
| Controllers | A udev rule (`/usr/lib/udev/rules.d/70-rpcs3-slim-controllers.rules`) gives your user access to Sony controllers over USB and Bluetooth, which the DualShock/DualSense handlers and **Auto-Detect** need. Replug the controller once after installing. |
| Wayland | Runs natively on Wayland (Vulkan and OpenGL). |
| Build | Built with clang + lld against Arch's system Qt 6, LLVM, FFmpeg, SDL3 and OpenCV. Local `makepkg` builds tune for your CPU (`RPCS3_SLIM_NATIVE=OFF` builds a portable package). |

## Tips

- **Hybrid graphics (Intel + NVIDIA laptops):** pick the discrete GPU under `Configuration → GPU → Graphics Device`.
- **Window or input glitches on Wayland:** try XWayland with `QT_QPA_PLATFORM=xcb rpcs3`.
- **Firmware, games, precompile:** follow the Quick start in [README-LITE.md](README-LITE.md).
