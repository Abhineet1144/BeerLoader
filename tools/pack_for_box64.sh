#!/usr/bin/env bash
# Bundle the x86-64 loader with its non-libc shared libraries so it can be
# copied to an ARM64 device and started under Box64 (see ANDROID_PORT.md).
#
# usage: tools/pack_for_box64.sh [loader-binary] [output-name]
set -euo pipefail

bin=${1:-./loader}
out=${2:-beer-box64-pack}

[ -x "$bin" ] || { echo "error: $bin not found or not executable (run make first)" >&2; exit 1; }

rm -rf "$out" "$out.tar.gz"
mkdir -p "$out/lib"
cp "$bin" "$out/loader"

# Box64 supplies its own wrapped libc/libm/libpthread/libdl/librt; everything
# else the loader links (libcrypto, libz, ...) must be shipped as x86-64 code.
ldd "$bin" | awk '/=> \//{print $3}' | while read -r lib; do
    case "$(basename "$lib")" in
        libc.so*|libm.so*|libpthread.so*|libdl.so*|librt.so*|ld-linux*) continue ;;
    esac
    cp -L "$lib" "$out/lib/"
done

# Test programs: guest exes for the loader, and the x86-64 Vulkan probe.
here=$(cd "$(dirname "$0")" && pwd)
for f in hello.exe crash.exe veh.exe com.exe vk_probe; do
    [ -f "$here/$f" ] && cp "$here/$f" "$out/$f"
done
[ -f "$out/vk_probe" ] && chmod +x "$out/vk_probe"

cat > "$out/run.sh" <<'EOF'
#!/bin/sh
# Run the x86-64 loader under Box64. Extra arguments go to the loader.
cd "$(dirname "$0")" || exit 1
export BOX64_LD_LIBRARY_PATH="$PWD/lib"
export BOX64_LOG=${BOX64_LOG:-1}
exec box64 ./loader "$@"
EOF
chmod +x "$out/run.sh"

tar czf "$out.tar.gz" "$out"
echo "wrote $out.tar.gz:"
ls "$out/lib"
