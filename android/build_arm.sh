#!/bin/sh
# Compile every Beer source to an aarch64 bionic object and try to link; report undefined symbols.
# Run in ~/beer-src on the phone.  usage: sh build_arm.sh [extra link objects/flags...]
cd "$(dirname "$0")/.." || exit 1
INC="-Igraphics/d3d11 -Igraphics/d3d12 -Iaudio -Imedia -Iplatform -Irenderer/vulkan -Igraphics/d3d12v -Igraphics/d3d11v"
SRCS=$(sed -n 's/^  FILES="\(.*\)/\1/p' android/check_aarch64.sh | tr -d '\\"' | tr '\n' ' ')
# Source list = the one in android/check_aarch64.sh (default branch), minus the dxil translator which needs third_party.
SRCS=$(awk '/FILES="loader.c/{f=1} f{print} /bd11_swap.c"/{if(f)exit}' android/check_aarch64.sh | sed 's/FILES="//; s/"//; s/\\//g' | tr -s ' \n' '\n' | grep '\.c$')
mkdir -p obj
echo "$SRCS" | xargs -P 4 -I{} sh -c '
  f={}; o=obj/$(echo $f | tr / _).o
  if [ ! -f "$o" ] || [ "$f" -nt "$o" ] || [ -n "$FORCE" ]; then
    clang --target=aarch64-linux-android30 -std=gnu11 -O1 -w -fPIC -c '"$INC"' "$f" -o "$o" 2>obj/$(echo $f | tr / _).err || echo "COMPILE FAIL: $f"
  fi'
echo "objects: $(ls obj/*.o 2>/dev/null | wc -l)"
cat > obj/dxil_stub.c <<'EOF'
#include "d3d12_dxil_translator.h"
int beer_d3d12_is_dxil(const uint8_t *d, size_t n) { (void)d; (void)n; return 0; }
int beer_d3d12_dxil_translate(const uint8_t *c, size_t n, uint32_t s, BeerD3D12TranslatedShader *o) { (void)c; (void)n; (void)s; (void)o; return 0; }
int beer_d3d12_dxil_translate_library(const uint8_t *c, size_t n, BeerD3D12TranslatedShader *sh, BeerD3D12RtShader *o, uint32_t m, uint32_t *cnt) { (void)c; (void)n; (void)sh; (void)o; (void)m; if (cnt) *cnt = 0; return 0; }
EOF
# With a dxil-spirv checkout (DXIL_SPIRV_DIR, default ~/beer-spike/dxil-spirv) build the real translator
# (it dlopens libdxil-spirv-c-shared.so; see BEER_DXIL_SPIRV_LIBRARY) instead of the stub.
D=${DXIL_SPIRV_DIR:-$HOME/beer-spike/dxil-spirv}
if [ -f "$D/dxil_spirv_c.h" ]; then
  rm -f obj/dxil_stub.o obj/graphics_d3d12_d3d12_dxil_translator.c.o
  clang --target=aarch64-linux-android30 -std=gnu11 -O1 -w -fPIC -c $INC -I"$D" graphics/d3d12/d3d12_dxil_translator.c -o obj/graphics_d3d12_d3d12_dxil_translator.c.o
else
  clang --target=aarch64-linux-android30 -std=gnu11 -O1 -w -fPIC -c $INC obj/dxil_stub.c -o obj/dxil_stub.o
fi
clang --target=aarch64-linux-android30 -o obj/beer_arm obj/*.o "$@" -ldl -lm -lz -llog 2>obj/link.err
echo "link exit: $?"
grep -oE "undefined symbol: [A-Za-z0-9_]+" obj/link.err | sort | uniq -c | sort -rn
