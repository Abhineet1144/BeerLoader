#!/bin/sh
# Build a Beer spike against the FEXCore built in ~/beer-spike/FEX/build.  usage: build_spike.sh out src.cpp [more.cpp...]
F=$HOME/beer-spike/FEX
OUT=$1; shift
INC="-I$F/FEXCore/include -I$F/build/include -I$F/Source -I$F/FEXHeaderUtils -I$F/External/fmt/include -I$F/External/range-v3/include -I$F/External/unordered_dense/include -I$F/CodeEmitter"
exec clang++ --target=aarch64-linux-android30 -std=gnu++20 -O1 -g \
  -DARCHITECTURE_arm64=1 -DFEX_DISABLE_TELEMETRY=1 -D_LARGEFILE64_SOURCE \
  -DFEXCORE_HAS_PRESERVE_ALL_ATTR=1 '-DFEXCORE_PRESERVE_ALL_ATTR=__attribute__((preserve_all))' \
  -DFEX_HAS_PRESERVE_ALL_ATTR=1 '-DFEX_PRESERVE_ALL_ATTR=__attribute__((preserve_all))' \
  -DHAS_SYSCALL_GETTID=1 -DHAS_SYSCALL_TGKILL=1 -DJIT_ARM64 \
  $INC "$@" \
  $F/Source/Common/HostFeatures.cpp $F/Source/Common/CPUInfo.cpp $F/Source/Common/Linux/LinuxVersion.cpp \
  $F/build/FEXCore/Source/libFEXCore.a $F/build/FEXCore/Source/libFEXCore_Base.a $F/build/FEXCore/Source/libJemallocDummy.a \
  $F/build/External/fmt/libfmt.a $F/build/External/xxhash/cmake_unofficial/libxxhash.a \
  $F/build/External/SoftFloat-3e/libsoftfloat_3e.a $F/build/External/cephes/libcephes_128bit.a -o "$OUT"
