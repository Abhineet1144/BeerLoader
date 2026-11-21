#!/usr/bin/env bash
# Build the external Valve runtime with Beer's narrow authentication extension; never vendor it.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
source=${GNS_SOURCE_DIR:?Set GNS_SOURCE_DIR to an external GameNetworkingSockets checkout}
build=${GNS_BUILD_DIR:-${XDG_CACHE_HOME:-$HOME/.cache}/beer/steam-networking-build}
dest=${GNS_INSTALL_DIR:-$HOME/.local/share/beer/native/steam-networking}
cmake -S "$root/tools/steam_networking" -B "$build" -DGNS_SOURCE_DIR="$source" -DCMAKE_BUILD_TYPE=Release "$@"
cmake --build "$build" --parallel "${BEER_BUILD_JOBS:-8}"
library="$build/bin/libGameNetworkingSockets.so"
BEER_STEAM_NETWORKING_LIBRARY="$library" python3 "$root/tools/steam_networking/bridge_test.py"
mkdir -p "$dest"
if [[ -f "$dest/libGameNetworkingSockets.so" ]]; then
    cp -p -- "$dest/libGameNetworkingSockets.so" "$dest/libGameNetworkingSockets.so.backup-$(date +%Y%m%d-%H%M%S)-$$"
fi
# Rename a fresh inode. Never overwrite an mmap'd runtime used by a running game.
temporary=$(mktemp "$dest/.libGameNetworkingSockets.XXXXXX")
trap 'rm -f -- "$temporary"' EXIT
install -m 755 "$library" "$temporary"
mv -f -- "$temporary" "$dest/libGameNetworkingSockets.so"
install -m 644 "$source/LICENSE" "$dest/GameNetworkingSockets-LICENSE"
git -C "$source" rev-parse HEAD
printf 'Installed Steam-authentication runtime in %s\n' "$dest"
