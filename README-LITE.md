# RPCS3 Lite

A slimmed-down, offline-only build of [RPCS3](https://github.com/RPCS3/rpcs3) for testing PS3 homebrew and disc images (`.iso` / `.dec.iso`) on low-end PCs before running them on real hardware.

## What's different from stock RPCS3

| Area | Lite behavior |
|---|---|
| Online | RPCN, Clans, PSN-over-RPCN, internet and UPnP are forced **off** at every boot (even if a config file says otherwise). Network settings tab, RPCN toolbar button and online menus are hidden. |
| Updater | Auto-updater and compatibility/config/integrity database downloads are disabled. |
| Upscaling | FSR / RCAS removed. Resolution Scale is capped at **100%** (can still go *down* to 25% for speed). |
| CRT filter | `Configuration → GPU → Output Scaling → CRT (Scanlines)`: scanlines, slot mask, soft vignette. One cheap fragment pass, works on Vulkan and OpenGL. |
| Controllers | `Configuration → Pads → Auto-Detect`: picks a connected DualSense first, then an Xbox pad (XInput), then any SDL pad. |
| Precompile | Toolbar **Precompile** button builds PPU + SPU LLVM caches for everything in the game list (tick the box to include the firmware UI). `File → Firmware → Precompile Firmware Modules (PPU)` builds firmware caches alone (also runs automatically after installing firmware). |
| Game updates | `Utilities → Game Update Downloader...` (or right-click a game → **Check for Game Updates...**) runs the bundled [rusty-psn](https://github.com/RainbowCookie32/rusty-psn) CLI in a console window: pick the updates, it downloads and verifies them, then RPCS3 Lite installs every downloaded PKG automatically. Files are kept in `cache/game_updates/`. |
| Defaults | Shader Mode = Async (recompiler only, lighter on weak GPUs), hover GIFs off, update checks off. |

## Quick start

1. Download `PS3UPDAT.PUP` from Sony's official PS3 system software page.
2. `File → Install Firmware` → pick the PUP. Firmware module precompile starts automatically.
3. `File → Add Games` (or drag & drop) your `.iso` / `.dec.iso` files, or `File → Boot ISO`.
   - Decrypted ISOs (`.dec.iso`, 3k3y) work directly.
   - Encrypted Redump ISOs need a matching `.dkey` next to the `.iso`.
4. Click **Precompile** once. Later boots skip the long "Compiling PPU modules" step.
5. `Configuration → Pads → Auto-Detect` with your controller plugged in.

## About shader caching

PS3 shaders only exist once a game submits them to the GPU, so they can't be built ahead of the first run. RPCS3 saves every shader to an on-disk cache during that first run and loads the whole cache on every later boot, so stutter goes away after a title has been played once. Async mode keeps the first run playable while shaders compile in the background.

## Low-end tips

- Renderer: Vulkan if your GPU supports it, otherwise OpenGL.
- GPU → Resolution Scale: 75% or 50% if the GPU is the bottleneck.
- CPU → Preferred SPU Threads: try 1–2 on dual/quad-core CPUs.
- Leave "Max LLVM Compile Threads" on Auto. Precompiling once avoids paying that cost at boot.

## Building

Every push to `master` builds a Windows package through GitHub Actions (`.github/workflows/rpcs3-lite.yml`). Download it from the run's **Artifacts** section.
