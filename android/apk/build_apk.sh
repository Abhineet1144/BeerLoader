#!/bin/bash
# Package libbeer.so + the shader translators into a signed debug APK (run on the PC after build_native.sh).
# usage: android/apk/build_apk.sh   ->  android/apk/out/beer.apk
set -e
cd "$(dirname "$0")/../.."
ROOT=$(cd .. && pwd)
SDK=${ANDROID_SDK:-$ROOT/android-sdk}
BT=$(ls -d "$SDK"/build-tools/* | tail -1)
JAR=$SDK/platforms/android-35/android.jar
NDK=$(ls -d "$SDK"/ndk/29* | head -1)
STRIP=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip
OUT=android/apk/out
DXIL=${DXIL_SPIRV_DIR:-$ROOT/android-deps/dxil-spirv}/build-ndk/libdxil-spirv-c-shared.so
VKD3D=android/apk/prebuilt/libvkd3d-shader.so
FFMPEG=${FFMPEG_BUILD_DIR:-$ROOT/android-deps/ffmpeg-build}   # ffmpeg/ffprobe executables (see ANDROID_PORT.md)
KEY=${BEER_KEYSTORE:-$ROOT/android-deps/beer-debug.jks}

STAGE=$OUT/stage; rm -rf "$STAGE"; mkdir -p "$STAGE/lib/arm64-v8a"
cp "$OUT/lib/arm64-v8a/libbeer.so" "$STAGE/lib/arm64-v8a/"
cp "$DXIL" "$STAGE/lib/arm64-v8a/libdxil-spirv-c-shared.so"
cp "$VKD3D" "$STAGE/lib/arm64-v8a/libvkd3d-shader.so"
# Executables are shipped as lib*.so: Android only installs (and allows executing) files named like libraries.
cp "$FFMPEG/ffmpeg" "$STAGE/lib/arm64-v8a/libffmpeg.so"
cp "$FFMPEG/ffprobe" "$STAGE/lib/arm64-v8a/libffprobe.so"
"$STRIP" --strip-unneeded "$STAGE"/lib/arm64-v8a/*.so

"$BT/aapt2" link -o "$OUT/beer-unsigned.apk" --manifest android/apk/AndroidManifest.xml -I "$JAR" \
    --min-sdk-version 30 --target-sdk-version 35 --version-code 1 --version-name 0.1
(cd "$STAGE" && zip -q -r -D -X "../beer-unsigned.apk" lib)
"$BT/zipalign" -f -p 4 "$OUT/beer-unsigned.apk" "$OUT/beer-aligned.apk"
if [ ! -f "$KEY" ]; then
  keytool -genkeypair -keystore "$KEY" -storepass beerdebug -keypass beerdebug -alias beer -keyalg RSA -keysize 2048 \
      -validity 10000 -dname "CN=Beer Debug" >/dev/null 2>&1
fi
"$BT/apksigner" sign --ks "$KEY" --ks-pass pass:beerdebug --key-pass pass:beerdebug --out "$OUT/beer.apk" "$OUT/beer-aligned.apk"
ls -la "$OUT/beer.apk"
"$BT/apksigner" verify "$OUT/beer.apk" && echo "signature ok"
