#!/usr/bin/env bash
set -euo pipefail

cd -- "$(dirname -- "$0")"

if [[ $# -lt 1 ]]; then
    echo "Usage: $0 [--renderer=vulkan] [--presenter=vulkan] [--prefix=PATH] [--audio=fmod|compat] <windows-executable>" >&2
    exit 2
fi

has_audio=0
for arg in "$@"; do
    [[ "$arg" == --audio=* ]] && has_audio=1
 done

# This repository currently tracks some build artifacts. Force a clean build so
# stale objects can never hide command-line or ABI changes.
make clean
make

if (( has_audio )); then
    exec ./loader "$@"
else
    exec ./loader --audio=fmod "$@"
fi
