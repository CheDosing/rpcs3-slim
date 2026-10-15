#!/bin/sh -ex
# RPCS3 Lite packaging: no compatibility/config databases, no translations (offline + smaller footprint).

ARTIFACT_DIR="$BUILD_ARTIFACTSTAGINGDIRECTORY"

rm -f ./bin/rpcs3.exp ./bin/rpcs3.lib ./bin/rpcs3.pdb ./bin/vc_redist.x64.exe

# SDL controller mappings (needed for SDL-handled Xbox / generic pads)
mkdir -p ./bin/config/input_configs
curl -fsSL 'https://raw.githubusercontent.com/gabomdq/SDL_GameControllerDB/master/gamecontrollerdb.txt' 1> ./bin/config/input_configs/gamecontrollerdb.txt || true

BUILD_LITE="rpcs3-lite-$(git rev-parse --short HEAD)_win64.7z"
7z a -m0=LZMA2 -mx9 "$BUILD_LITE" ./bin/*
sha256sum "$BUILD_LITE" | awk '{ print $1 }' | tee "$BUILD_LITE.sha256"

cp -- "$BUILD_LITE" "$ARTIFACT_DIR"
cp -- "$BUILD_LITE.sha256" "$ARTIFACT_DIR"
