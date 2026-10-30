#!/bin/bash
# Cross-compile the Beer loader + FEXCore backend into libbeer.so for arm64-v8a with the Android NDK (run on the PC).
# usage: android/apk/build_native.sh [out-dir]   (default: android/apk/out)
# Needs: ../android-sdk (NDK 29) and ../android-deps/FEX built in build-ndk (see ANDROID_PORT.md).
set -e
cd "$(dirname "$0")/../.."
ROOT=$(cd .. && pwd)
SDK=${ANDROID_SDK:-$ROOT/android-sdk}
NDK=$(ls -d "$SDK"/ndk/29* | head -1)
T=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
F=${FEX_DIR:-$ROOT/android-deps/FEX}
FB=$F/build-ndk
OUT=${1:-android/apk/out}
OBJ=$OUT/obj
API=30
mkdir -p "$OBJ" "$OUT/lib/arm64-v8a"
CC="$T/clang --target=aarch64-linux-android$API"
CXX="$T/clang++ --target=aarch64-linux-android$API"
INC="-Igraphics/d3d11 -Igraphics/d3d12 -Iaudio -Imedia -Iplatform -Irenderer/vulkan -Igraphics/d3d12v -Igraphics/d3d11v -Iandroid"
DXIL=${DXIL_SPIRV_DIR:-$ROOT/android-deps/dxil-spirv}
[ -f "$DXIL/dxil_spirv_c.h" ] && INC="$INC -I$DXIL"

SRCS=$(awk '/FILES="loader.c/{f=1} f{print} /bd11_swap.c"/{if(f)exit}' android/check_aarch64.sh | sed 's/FILES="//; s/"//; s/\\//g' | tr -s ' \n' '\n' | grep '\.c$')
SRCS=$(printf '%s\n%s\n%s\n' "$SRCS" graphics/d3d12/d3d12_dxil_translator.c android/android_support.c)
if [ "$BEER_X11" = 1 ] || [ ! -f android/android_backend.c ]; then :; else
  SRCS=$(echo "$SRCS" | grep -v '^platform/xwayland_backend.c$'; echo android/android_backend.c)
fi

# A header newer than the last build (a struct layout change) rebuilds everything: objects compiled against the old
# layout would otherwise be linked in silently.
STAMP=$OBJ/.headers.stamp
if [ ! -f "$STAMP" ] || [ -n "$(find platform graphics renderer audio media android \( -name '*.h' -o -name '*.inc' \) \
                                    -newer "$STAMP" -print -quit)" ]; then
  FORCE=1
fi

compile_c() {
  f=$1; o=$OBJ/$(echo "$f" | tr / _).o
  if [ ! -f "$o" ] || [ "$f" -nt "$o" ] || [ -n "$FORCE" ]; then
    extra=""
    [ "$f" = loader.c ] && extra="-Dmain=beer_loader_main"
    $CC -std=gnu11 -O2 -w -fPIC -c $INC $extra "$f" -o "$o" 2> "$o.err" || { echo "COMPILE FAIL: $f"; grep error "$o.err" | head -5; return 1; }
  fi
}
export -f compile_c; export CC INC OBJ FORCE
echo "$SRCS" | xargs -P 8 -I{} bash -c 'compile_c {}' 
touch "$STAMP"

FDEFS="-DARCHITECTURE_arm64=1 -DFEX_DISABLE_TELEMETRY=1 -D_LARGEFILE64_SOURCE -DFEXCORE_HAS_PRESERVE_ALL_ATTR=1 -DFEX_HAS_PRESERVE_ALL_ATTR=1 -DHAS_SYSCALL_GETTID=1 -DHAS_SYSCALL_TGKILL=1 -DJIT_ARM64"
FINC="-Iplatform -Iandroid -I$F/FEXCore/include -I$FB/include -I$F/Source -I$F/FEXHeaderUtils -I$F/External/fmt/include -I$F/External/range-v3/include -I$F/External/unordered_dense/include -I$F/CodeEmitter"
for src in android/guestcpu_fex.cpp $F/Source/Common/HostFeatures.cpp $F/Source/Common/CPUInfo.cpp $F/Source/Common/Linux/LinuxVersion.cpp; do
  o=$OBJ/cpp_$(basename "$src" .cpp).o
  if [ ! -f "$o" ] || [ "$src" -nt "$o" ] || [ -n "$FORCE" ]; then
    $CXX -std=gnu++20 -O2 -g0 -w -fPIC $FDEFS '-DFEXCORE_PRESERVE_ALL_ATTR=__attribute__((preserve_all))' '-DFEX_PRESERVE_ALL_ATTR=__attribute__((preserve_all))' $FINC -c "$src" -o "$o"
  fi
done

$CXX -shared -o "$OUT/lib/arm64-v8a/libbeer.so" $OBJ/*.o \
  $FB/FEXCore/Source/libFEXCore.a $FB/FEXCore/Source/libFEXCore_Base.a $FB/FEXCore/Source/libJemallocDummy.a \
  $FB/External/fmt/libfmt.a $FB/External/xxhash/cmake_unofficial/libxxhash.a \
  $FB/External/SoftFloat-3e/libsoftfloat_3e.a $FB/External/cephes/libcephes_128bit.a \
  -static-libstdc++ -landroid -llog -ldl -lm -lz -Wl,-z,max-page-size=16384 -Wl,--no-undefined 2> "$OUT/link.err" && echo "libbeer.so built" || { echo "LINK FAILED"; grep -oE "undefined symbol: [A-Za-z0-9_:]+" "$OUT/link.err" | sort | uniq -c | sort -rn | head -30; exit 1; }
