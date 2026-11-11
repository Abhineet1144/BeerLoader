#!/bin/sh
# Regenerates graphics/d3d12v/bd12_sampler_feedback_spv.h from graphics/d3d12v/shaders/sampler_feedback.comp.
set -e
cd "$(dirname "$0")/.."
SRC=graphics/d3d12v/shaders/sampler_feedback.comp
OUT=graphics/d3d12v/bd12_sampler_feedback_spv.h
GLSLC=${GLSLC:-glslc}
TMP=$(mktemp -d)
echo "/* Generated from $SRC by tools/gen_sampler_feedback_spv.sh (glslc --target-env=vulkan1.3). */" > $OUT
for v in DECODE_MIN_MIP DECODE_MIP_USED ENCODE_MIN_MIP ENCODE_MIP_USED; do
    $GLSLC --target-env=vulkan1.3 -O -D$v -fshader-stage=compute $SRC -o $TMP/$v.spv
    name=$(echo $v | tr 'A-Z' 'a-z')
    python3 - "$TMP/$v.spv" "$name" >> $OUT <<'PY'
import sys, struct
d = open(sys.argv[1], 'rb').read()
words = struct.unpack('<%dI' % (len(d) // 4), d)
print('static const uint32_t k_sf_%s_spv[] = {' % sys.argv[2])
for i in range(0, len(words), 8):
    print(', '.join('0x%08x' % w for w in words[i:i + 8]) + ',')
print('};')
PY
done
rm -rf $TMP
