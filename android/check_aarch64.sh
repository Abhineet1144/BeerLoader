#!/bin/sh
# Syntax-check every Beer source for aarch64 bionic (Android API 30). Run in ~/beer-src on the phone.
# usage: sh android/check_aarch64.sh [file.c ...]   (default: the Makefile's source list)
cd "$(dirname "$0")/.." || exit 1
FILES="$@"
if [ -z "$FILES" ]; then
  FILES="loader.c audio/audio_backend.c audio/wasapi_compat.c media/media_foundation.c \
    platform/xwayland_backend.c platform/win32_environment.c platform/win32_shared_data.c platform/win32_bcrypt_portable.c \
    platform/beer_hash.c platform/win32_version.c platform/beer_time.c platform/beer_control.c platform/beer_memory.c \
    platform/beer_overlay.c platform/beer_post.c platform/beer_gpu_stats.c platform/win32_nt_section.c \
    platform/win32_virtual_memory.c platform/x64_fault_emulation.c platform/win32_vectored_exception.c \
    platform/win32_unwind.c platform/win32_seh.c platform/win32_apiset.c platform/win32_gdi.c platform/win32_resource.c \
    platform/win32_onexit.c platform/win32_crt_init.c platform/win32_syscall.c platform/beer_gamepad.c \
    renderer/vulkan/vulkan_context.c renderer/vulkan/vulkan_presenter.c renderer/vulkan/vulkan_post.c \
    renderer/vulkan/vulkan_renderer.c renderer/vulkan/vulkan_graphics.c renderer/vulkan/vulkan_indexed_renderer.c \
    graphics/d3d11/d3d11_impl.c graphics/d3d12/d3d12_api.c graphics/d3d12/d3d12_shader_translator.c \
    graphics/d3d12v/bd12_device.c graphics/d3d12v/bd12_cmd.c graphics/d3d12v/bd12_descriptors.c graphics/d3d12v/bd12_bind.c \
    graphics/d3d12v/bd12_image.c graphics/d3d12v/bd12_graphics.c graphics/d3d12v/bd12_extra.c graphics/d3d12v/bd12_formats.c \
    graphics/d3d12v/bd12_pso.c graphics/d3d12v/bd12_translate.c graphics/d3d12v/bd12_texel_offset.c graphics/d3d12v/bd12_com.c graphics/d3d12v/bd12_com_list.c \
    graphics/d3d12v/bd12_raytracing.c graphics/d3d12v/bd12_mirror.c graphics/d3d12v/bd12_rtpipe.c graphics/d3d12v/bd12_ckpt.c \
    graphics/d3d12v/bd12_live.c graphics/d3d12v/bd12_capture.c graphics/d3d12v/bd12_cbtweak.c \
    graphics/d3d11v/bd11_obj.c graphics/d3d11v/bd11_dev.c graphics/d3d11v/bd11_ctx.c graphics/d3d11v/bd11_swap.c"
fi
INC="-Igraphics/d3d11 -Igraphics/d3d12 -Iaudio -Imedia -Iplatform -Irenderer/vulkan -Igraphics/d3d12v -Igraphics/d3d11v"
mkdir -p .chk
total=0; bad=0
for f in $FILES; do
  out=$(clang --target=aarch64-linux-android30 -std=gnu11 -fsyntax-only -ferror-limit=0 -w $INC "$f" 2>&1)
  n=$(printf '%s\n' "$out" | grep -c ' error: ')
  printf '%s\n' "$out" > ".chk/$(echo "$f" | tr / _).log"
  if [ "$n" -gt 0 ]; then bad=$((bad+1)); total=$((total+n)); fi
  printf '%5d  %s\n' "$n" "$f"
done
echo "files with errors: $bad, total errors: $total"
