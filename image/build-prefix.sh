#!/bin/bash
# One-time: create the wineprefix and install DXVK into it.
# DXVK 2.x ships no setup script: copy the DLLs and set registry overrides.
set -euxo pipefail

export WINEPREFIX="${WINEPREFIX:-/home/wine/.wine}"
export WINEARCH="${WINEARCH:-win64}"
export WINEDEBUG=-all
export HOME="${HOME:-/home/wine}"
mkdir -p "$WINEPREFIX" "${DXVK_STATE_CACHE_PATH:-/home/wine/.cache}"

# Initialize the prefix (headless).
WINEDLLOVERRIDES="mscoree,mshtml=" wineboot -i
wineserver -w

# Install DXVK: 32-bit DLLs -> syswow64, 64-bit -> system32 (win64 prefix).
DXVK=/opt/dxvk-2.4.1
cp -v "$DXVK"/x32/*.dll "$WINEPREFIX/drive_c/windows/syswow64/"
cp -v "$DXVK"/x64/*.dll "$WINEPREFIX/drive_c/windows/system32/"

# Overrides = native (use the DXVK DLLs).
for dll in d3d9 dxgi d3d10core d3d11 d3d8; do
  wine reg add 'HKCU\Software\Wine\DllOverrides' /v "$dll" /d native /f
done
wineserver -w

ls -l "$WINEPREFIX/drive_c/windows/syswow64/d3d9.dll"
echo "PREFIX-OK"
