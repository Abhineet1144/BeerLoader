#!/bin/sh
# Build the native-ARM64 Beer loader with the FEXCore backend (run in ~/beer-src on the phone, after
# android/spike has produced ~/beer-spike/FEX/build). Output: obj/beer_arm
cd "$(dirname "$0")/.." || exit 1
F=$HOME/beer-spike/FEX
INC="-Iplatform -Iandroid -I$F/FEXCore/include -I$F/build/include -I$F/Source -I$F/FEXHeaderUtils -I$F/External/fmt/include -I$F/External/range-v3/include -I$F/External/unordered_dense/include -I$F/CodeEmitter"
DEFS="-DARCHITECTURE_arm64=1 -DFEX_DISABLE_TELEMETRY=1 -D_LARGEFILE64_SOURCE -DFEXCORE_HAS_PRESERVE_ALL_ATTR=1 -DFEX_HAS_PRESERVE_ALL_ATTR=1 -DHAS_SYSCALL_GETTID=1 -DHAS_SYSCALL_TGKILL=1 -DJIT_ARM64"
mkdir -p obj
sh android/build_arm.sh >/dev/null 2>&1   # C objects (link there is expected to fail: backend missing)
for src in android/guestcpu_fex.cpp $F/Source/Common/HostFeatures.cpp $F/Source/Common/CPUInfo.cpp $F/Source/Common/Linux/LinuxVersion.cpp; do
  o=obj/cpp_$(basename $src .cpp).o
  if [ ! -f "$o" ] || [ "$src" -nt "$o" ]; then
    clang++ --target=aarch64-linux-android30 -std=gnu++20 -O1 -g0 -w $DEFS '-DFEXCORE_PRESERVE_ALL_ATTR=__attribute__((preserve_all))' '-DFEX_PRESERVE_ALL_ATTR=__attribute__((preserve_all))' $INC -c "$src" -o "$o" 2>obj/$(basename $src).err || { echo "COMPILE FAIL: $src"; grep -E "error" obj/$(basename $src).err | head -20; exit 1; }
  fi
done
clang++ --target=aarch64-linux-android30 -o obj/beer_arm $(ls obj/*.o) \
  $F/build/FEXCore/Source/libFEXCore.a $F/build/FEXCore/Source/libFEXCore_Base.a $F/build/FEXCore/Source/libJemallocDummy.a \
  $F/build/External/fmt/libfmt.a $F/build/External/xxhash/cmake_unofficial/libxxhash.a \
  $F/build/External/SoftFloat-3e/libsoftfloat_3e.a $F/build/External/cephes/libcephes_128bit.a \
  -ldl -lm -lz -llog 2>obj/link.err
echo "link exit: $?"
grep -oE "undefined symbol: [A-Za-z0-9_:]+" obj/link.err | sort | uniq -c | sort -rn | head -20
grep -E "error" obj/link.err | grep -v 'undefined symbol' | head -5
