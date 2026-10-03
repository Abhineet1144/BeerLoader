#!/usr/bin/env bash
set -euo pipefail

cd -- "$(dirname -- "$0")"

if [[ $# -lt 1 ]]; then
    echo "Usage: $0 [--renderer=vulkan] [--presenter=vulkan] <windows-executable>" >&2
    exit 2
fi

# This repository currently tracks some build artifacts. Force a clean build so
# stale objects can never hide command-line or ABI changes.
make clean
make
exec ./loader "$@"
