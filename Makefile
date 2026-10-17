CC     = gcc
CFLAGS = -O0 -Wall -Wextra -m64 -g -MMD -MP -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -fno-stack-protector -mno-sse -mno-sse2 -mno-avx -mno-mmx -Igraphics/d3d11 -Igraphics/d3d12 -Iaudio -Imedia -Iplatform -Irenderer/vulkan
RENDER_CFLAGS = $(filter-out -O0,$(CFLAGS)) -O3 -march=native
LDFLAGS = -lpthread -ldl
TARGET = loader
.DEFAULT_GOAL := $(TARGET)

AUDIO_SRCS = audio/audio_backend.c audio/wasapi_compat.c
MEDIA_SRCS = media/media_foundation.c
PLATFORM_SRCS = platform/xwayland_backend.c
VULKAN_SRCS = renderer/vulkan/vulkan_context.c \
               renderer/vulkan/vulkan_presenter.c \
               renderer/vulkan/vulkan_renderer.c \
               renderer/vulkan/vulkan_compute.c \
               renderer/vulkan/vulkan_graphics.c \
               renderer/vulkan/vulkan_indexed_renderer.c
GRAPHICS_SRCS = graphics/d3d11/d3d11_impl.c \
                graphics/d3d12/d3d12_compat.c \
                graphics/d3d12/d3d12_graphics.c \
                graphics/d3d12/d3d12_vulkan_compute.c \
                graphics/d3d12/d3d12_vulkan_graphics.c \
                graphics/d3d12/d3d12_shader_translator.c
SRCS = loader.c $(AUDIO_SRCS) $(MEDIA_SRCS) $(PLATFORM_SRCS) $(VULKAN_SRCS) $(GRAPHICS_SRCS)
OBJS = $(SRCS:.c=.o)
DEPS = $(OBJS:.o=.d)

-include $(DEPS)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJS) $(LDFLAGS)

graphics/d3d11/d3d11_impl.o: graphics/d3d11/d3d11_impl.c
	$(CC) $(RENDER_CFLAGS) -c $< -o $@

# D3D12's Win64 ABI passes floating-point parameters in XMM registers (for
# example ClearDepthStencilView's depth value). The loader core deliberately
# avoids SIMD while running guest code, but this isolated COM boundary must let
# GCC generate the ABI-mandated XMM argument handling.
D3D12_ABI_CFLAGS = $(filter-out -O0 -mno-sse -mno-sse2 -mno-avx -mno-mmx,$(CFLAGS)) -O2 -msse2

graphics/d3d12/d3d12_compat.o: graphics/d3d12/d3d12_compat.c graphics/d3d12/d3d12_compat.h
	$(CC) $(D3D12_ABI_CFLAGS) -c $< -o $@

graphics/d3d12/d3d12_compute.o: graphics/d3d12/d3d12_compute.c graphics/d3d12/d3d12_compute.h
	$(CC) $(D3D12_ABI_CFLAGS) -c $< -o $@

# These files run entirely on the host side after guest ABI state has already
# been decoded. Building them with the loader's -O0 default wastes substantial
# CPU time in descriptor resolution and draw/dispatch preparation. Keep the
# ABI-facing compatibility objects above on their specialized flags, but
# optimize the backend-neutral translation bridge just like the Vulkan backend.
graphics/d3d12/d3d12_graphics.o: graphics/d3d12/d3d12_graphics.c graphics/d3d12/d3d12_graphics.h
	$(CC) $(RENDER_CFLAGS) -c $< -o $@

graphics/d3d12/d3d12_vulkan_compute.o: graphics/d3d12/d3d12_vulkan_compute.c graphics/d3d12/d3d12_vulkan_compute.h
	$(CC) $(RENDER_CFLAGS) -c $< -o $@

graphics/d3d12/d3d12_vulkan_graphics.o: graphics/d3d12/d3d12_vulkan_graphics.c graphics/d3d12/d3d12_vulkan_graphics.h
	$(CC) $(RENDER_CFLAGS) -c $< -o $@

graphics/d3d12/d3d12_shader_translator.o: graphics/d3d12/d3d12_shader_translator.c graphics/d3d12/d3d12_shader_translator.h
	$(CC) $(RENDER_CFLAGS) -c $< -o $@

platform/xwayland_backend.o: platform/xwayland_backend.c
	$(CC) $(RENDER_CFLAGS) -c $< -o $@

VULKAN_HEADERS ?= ../Android/toolchain/android-ndk-r30/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/include
VULKAN_GLSLC ?= ../Android/toolchain/android-ndk-r30/shader-tools/linux-x86_64/glslc
VULKAN_SHADER_DIR = renderer/vulkan/shaders
VULKAN_COMPOSITOR_SPV = $(VULKAN_SHADER_DIR)/fullscreen_composite.comp.spv
VULKAN_INDEXED_SPV = $(VULKAN_SHADER_DIR)/indexed_scaleform.comp.spv
VULKAN_SPV = $(VULKAN_COMPOSITOR_SPV) $(VULKAN_INDEXED_SPV)

$(VULKAN_COMPOSITOR_SPV): $(VULKAN_SHADER_DIR)/fullscreen_composite.comp
	$(VULKAN_GLSLC) -O $< -o $@

$(VULKAN_INDEXED_SPV): $(VULKAN_SHADER_DIR)/indexed_scaleform.comp
	$(VULKAN_GLSLC) -O $< -o $@

renderer/vulkan/vulkan_context.o: renderer/vulkan/vulkan_context.c renderer/vulkan/vulkan_context.h
	$(CC) $(RENDER_CFLAGS) -idirafter $(VULKAN_HEADERS) -c $< -o $@

renderer/vulkan/vulkan_compute.o: renderer/vulkan/vulkan_compute.c renderer/vulkan/vulkan_compute.h renderer/vulkan/vulkan_context.h
	$(CC) $(RENDER_CFLAGS) -idirafter $(VULKAN_HEADERS) -c $< -o $@

renderer/vulkan/vulkan_graphics.o: renderer/vulkan/vulkan_graphics.c renderer/vulkan/vulkan_graphics.h renderer/vulkan/vulkan_context.h
	$(CC) $(RENDER_CFLAGS) -idirafter $(VULKAN_HEADERS) -c $< -o $@

renderer/vulkan/vulkan_presenter.o: renderer/vulkan/vulkan_presenter.c renderer/vulkan/vulkan_presenter.h renderer/vulkan/vulkan_context.h
	$(CC) $(RENDER_CFLAGS) -idirafter $(VULKAN_HEADERS) -c $< -o $@

renderer/vulkan/vulkan_renderer.o: renderer/vulkan/vulkan_renderer.c renderer/vulkan/vulkan_renderer.h renderer/vulkan/vulkan_context.h $(VULKAN_COMPOSITOR_SPV)
	$(CC) $(RENDER_CFLAGS) -idirafter $(VULKAN_HEADERS) -c $< -o $@

renderer/vulkan/vulkan_indexed_renderer.o: renderer/vulkan/vulkan_indexed_renderer.c renderer/vulkan/vulkan_indexed_renderer.h renderer/vulkan/vulkan_context.h $(VULKAN_INDEXED_SPV)
	$(CC) $(RENDER_CFLAGS) -idirafter $(VULKAN_HEADERS) -c $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

.PHONY: clean smoke-cli test-audio test-d3d11 test-d3d12 test-media test-renderer test-input

test-d3d11: $(filter-out loader.o,$(OBJS))
	$(CC) -O2 -Wall -Wextra -pthread -Igraphics/d3d11 \
		graphics/d3d11/d3d11_lifetime_test.c $(filter-out loader.o,$(OBJS)) \
		$(LDFLAGS) -o /tmp/beer-d3d11-lifetime-test
	/tmp/beer-d3d11-lifetime-test
	rm -f /tmp/beer-d3d11-lifetime-test

test-input:
	$(CC) -O2 -Wall -Wextra -Iplatform platform/directinput_format_test.c -o /tmp/beer-directinput-format-test
	/tmp/beer-directinput-format-test
	$(CC) -O2 -Wall -Wextra -Iplatform platform/window_style_policy_test.c -o /tmp/beer-window-style-policy-test
	/tmp/beer-window-style-policy-test
	$(CC) -O2 -Wall -Wextra -Iplatform platform/window_icon_policy_test.c -o /tmp/beer-window-icon-policy-test
	/tmp/beer-window-icon-policy-test
	$(CC) -O2 -Wall -Wextra -Iplatform platform/guid_compat_test.c -o /tmp/beer-guid-compat-test
	/tmp/beer-guid-compat-test
	$(CC) -O2 -Wall -Wextra -Iplatform platform/win32_filesystem_policy_test.c -o /tmp/beer-win32-filesystem-policy-test
	/tmp/beer-win32-filesystem-policy-test
	$(CC) -O2 -Wall -Wextra -Iplatform platform/steam_offline_policy_test.c -o /tmp/beer-steam-offline-policy-test
	/tmp/beer-steam-offline-policy-test
	rm -f /tmp/beer-directinput-format-test /tmp/beer-window-style-policy-test /tmp/beer-window-icon-policy-test /tmp/beer-guid-compat-test /tmp/beer-win32-filesystem-policy-test /tmp/beer-steam-offline-policy-test

test-renderer:
	$(CC) -O2 -Wall -Wextra -Irenderer/vulkan renderer/vulkan/target_mirror_test.c -o /tmp/beer-target-mirror-test
	/tmp/beer-target-mirror-test
	rm -f /tmp/beer-target-mirror-test
	$(VULKAN_GLSLC) renderer/vulkan/tests/buffer_scale.comp -o /tmp/beer-buffer-scale.spv
	$(VULKAN_GLSLC) renderer/vulkan/tests/storage_image.comp -o /tmp/beer-storage-image.spv
	$(VULKAN_GLSLC) renderer/vulkan/tests/storage_array.comp -o /tmp/beer-storage-array.spv
	$(VULKAN_GLSLC) renderer/vulkan/tests/storage_volume.comp -o /tmp/beer-storage-volume.spv
	$(VULKAN_GLSLC) renderer/vulkan/tests/storage_mip.comp -o /tmp/beer-storage-mip.spv
	$(VULKAN_GLSLC) renderer/vulkan/tests/storage_cube.comp -o /tmp/beer-storage-cube.spv
	$(VULKAN_GLSLC) renderer/vulkan/tests/storage_1d.comp -o /tmp/beer-storage-1d.spv
	$(VULKAN_GLSLC) renderer/vulkan/tests/watchdog_storage_volume.comp -o /tmp/beer-watchdog-storage-volume.spv
	$(VULKAN_GLSLC) renderer/vulkan/tests/noop.comp -o /tmp/beer-noop.spv
	$(CC) -O2 -Wall -Wextra -pthread -Irenderer/vulkan -idirafter $(VULKAN_HEADERS) renderer/vulkan/vulkan_compute_test.c renderer/vulkan/vulkan_compute.c renderer/vulkan/vulkan_graphics.c renderer/vulkan/vulkan_context.c -ldl -o /tmp/beer-vulkan-compute-test
	/tmp/beer-vulkan-compute-test /tmp/beer-buffer-scale.spv /tmp/beer-storage-image.spv /tmp/beer-storage-array.spv /tmp/beer-storage-volume.spv /tmp/beer-storage-mip.spv /tmp/beer-storage-cube.spv /tmp/beer-storage-1d.spv /tmp/beer-watchdog-storage-volume.spv /tmp/beer-noop.spv
	rm -f /tmp/beer-vulkan-compute-test /tmp/beer-buffer-scale.spv /tmp/beer-storage-image.spv /tmp/beer-storage-array.spv /tmp/beer-storage-volume.spv /tmp/beer-storage-mip.spv /tmp/beer-storage-cube.spv /tmp/beer-storage-1d.spv /tmp/beer-watchdog-storage-volume.spv /tmp/beer-noop.spv
	$(VULKAN_GLSLC) renderer/vulkan/tests/fullscreen.vert -o /tmp/beer-fullscreen.vert.spv
	$(VULKAN_GLSLC) renderer/vulkan/tests/fullscreen.frag -o /tmp/beer-fullscreen.frag.spv
	$(VULKAN_GLSLC) renderer/vulkan/tests/fullscreen_cb.frag -o /tmp/beer-fullscreen-cb.frag.spv
	$(VULKAN_GLSLC) renderer/vulkan/tests/fullscreen_depth.frag -o /tmp/beer-fullscreen-depth.frag.spv
	$(VULKAN_GLSLC) renderer/vulkan/tests/fullscreen_buffer.frag -o /tmp/beer-fullscreen-buffer.frag.spv
	$(VULKAN_GLSLC) renderer/vulkan/tests/indexed.vert -o /tmp/beer-indexed.vert.spv
	$(VULKAN_GLSLC) renderer/vulkan/tests/indexed.frag -o /tmp/beer-indexed.frag.spv
	$(VULKAN_GLSLC) renderer/vulkan/tests/fullscreen_array.frag -o /tmp/beer-fullscreen-array.frag.spv
	$(VULKAN_GLSLC) renderer/vulkan/tests/fullscreen_volume.frag -o /tmp/beer-fullscreen-volume.frag.spv
	$(VULKAN_GLSLC) renderer/vulkan/tests/fullscreen_1d_array.frag -o /tmp/beer-fullscreen-1d-array.frag.spv
	$(VULKAN_GLSLC) renderer/vulkan/tests/fullscreen_mrt.frag -o /tmp/beer-fullscreen-mrt.frag.spv
	$(VULKAN_GLSLC) renderer/vulkan/tests/fullscreen_texel.vert -o /tmp/beer-fullscreen-texel.vert.spv
	$(VULKAN_GLSLC) renderer/vulkan/tests/fullscreen_texel.frag -o /tmp/beer-fullscreen-texel.frag.spv
	$(VULKAN_GLSLC) renderer/vulkan/tests/fullscreen_mip.frag -o /tmp/beer-fullscreen-mip.frag.spv
	$(VULKAN_GLSLC) renderer/vulkan/tests/fullscreen_sample.frag -o /tmp/beer-fullscreen-sample.frag.spv
	$(VULKAN_GLSLC) renderer/vulkan/tests/fullscreen_cube.frag -o /tmp/beer-fullscreen-cube.frag.spv
	$(CC) -O2 -Wall -Wextra -pthread -Irenderer/vulkan -idirafter $(VULKAN_HEADERS) renderer/vulkan/vulkan_graphics_test.c renderer/vulkan/vulkan_graphics.c renderer/vulkan/vulkan_compute.c renderer/vulkan/vulkan_context.c -ldl -o /tmp/beer-vulkan-graphics-test
	/tmp/beer-vulkan-graphics-test /tmp/beer-fullscreen.vert.spv /tmp/beer-fullscreen.frag.spv /tmp/beer-fullscreen-cb.frag.spv /tmp/beer-fullscreen-depth.frag.spv /tmp/beer-fullscreen-buffer.frag.spv /tmp/beer-indexed.vert.spv /tmp/beer-indexed.frag.spv /tmp/beer-fullscreen-array.frag.spv /tmp/beer-fullscreen-volume.frag.spv /tmp/beer-fullscreen-1d-array.frag.spv /tmp/beer-fullscreen-mrt.frag.spv /tmp/beer-fullscreen-texel.vert.spv /tmp/beer-fullscreen-texel.frag.spv /tmp/beer-fullscreen-mip.frag.spv /tmp/beer-fullscreen-sample.frag.spv /tmp/beer-fullscreen-cube.frag.spv
	rm -f /tmp/beer-vulkan-graphics-test /tmp/beer-fullscreen.vert.spv /tmp/beer-fullscreen.frag.spv /tmp/beer-fullscreen-cb.frag.spv /tmp/beer-fullscreen-depth.frag.spv /tmp/beer-fullscreen-buffer.frag.spv /tmp/beer-indexed.vert.spv /tmp/beer-indexed.frag.spv /tmp/beer-fullscreen-array.frag.spv /tmp/beer-fullscreen-volume.frag.spv /tmp/beer-fullscreen-1d-array.frag.spv /tmp/beer-fullscreen-mrt.frag.spv /tmp/beer-fullscreen-texel.vert.spv /tmp/beer-fullscreen-texel.frag.spv /tmp/beer-fullscreen-mip.frag.spv /tmp/beer-fullscreen-sample.frag.spv /tmp/beer-fullscreen-cube.frag.spv

test-d3d12:
	$(CC) -O2 -Wall -Wextra -msse2 -Igraphics/d3d12 graphics/d3d12/d3d12_compat_test.c graphics/d3d12/d3d12_compat.c -o /tmp/beer-d3d12-test
	/tmp/beer-d3d12-test
	$(CC) -O2 -Wall -Wextra -Igraphics/d3d12 graphics/d3d12/d3d12_graphics_test.c graphics/d3d12/d3d12_graphics.c -o /tmp/beer-d3d12-graphics-test
	/tmp/beer-d3d12-graphics-test
	$(CC) -O2 -Wall -Wextra -pthread -Igraphics/d3d12 graphics/d3d12/d3d12_shader_translator_test.c graphics/d3d12/d3d12_shader_translator.c -ldl -o /tmp/beer-d3d12-shader-translator-test
	@if test -f /tmp/beer-d3d12-31b662b443572be7.dxbc; then \
		/tmp/beer-d3d12-shader-translator-test /tmp/beer-d3d12-31b662b443572be7.dxbc; \
	else \
		echo "D3D12 translation fixture absent; runtime translation test skipped"; \
	fi
	rm -f /tmp/beer-d3d12-test /tmp/beer-d3d12-graphics-test /tmp/beer-d3d12-shader-translator-test

test-audio:
	$(CC) -O2 -Wall -Wextra -pthread -Iaudio audio/audio_backend_test.c audio/audio_backend.c -ldl -o /tmp/beer-audio-test
	timeout 10s /tmp/beer-audio-test
	$(CC) -O2 -Wall -Wextra -pthread -Iaudio audio/wasapi_compat_test.c audio/wasapi_compat.c audio/audio_backend.c -ldl -o /tmp/beer-wasapi-test
	timeout 10s /tmp/beer-wasapi-test
	rm -f /tmp/beer-audio-test /tmp/beer-wasapi-test

test-media:
	$(CC) -O2 -Wall -Wextra -Imedia media/media_foundation_test.c media/media_foundation.c -o /tmp/beer-media-test
	/tmp/beer-media-test
	rm -f /tmp/beer-media-test

smoke-cli: $(TARGET)
	@set -e; \
	./$(TARGET) --renderer=vulkan --presenter=vulkan /tmp/beer-cli-smoke-missing.exe 2>&1 | grep -F "[RENDERER] selected Vulkan-only rendering" >/dev/null; \
	./$(TARGET) --renderer=cpu /tmp/beer-cli-smoke-missing.exe 2>&1 | grep -F "only vulkan is supported" >/dev/null; \
	./$(TARGET) --presenter=auto /tmp/beer-cli-smoke-missing.exe 2>&1 | grep -F "[PRESENTER] selected auto" >/dev/null; \
	./$(TARGET) --presenter=x11 /tmp/beer-cli-smoke-missing.exe 2>&1 | grep -F "[PRESENTER] selected x11" >/dev/null; \
	./$(TARGET) --audio=unknown /tmp/beer-cli-smoke-missing.exe 2>&1 | grep -F "Unknown audio backend" >/dev/null; \
	./$(TARGET) /tmp/beer-cli-smoke-missing.exe 2>&1 | grep -F "[AUDIO] selected bundled FMOD backend" >/dev/null; \
	./$(TARGET) --audio=compat /tmp/beer-cli-smoke-missing.exe 2>&1 | grep -F "[AUDIO] selected silent compatibility backend" >/dev/null; \
	rm -rf /tmp/beer-cli-prefix; \
	./$(TARGET) --prefix=/tmp/beer-cli-prefix /tmp/beer-cli-smoke-missing.exe 2>&1 | grep -F "[PREFIX] /tmp/beer-cli-prefix" >/dev/null; \
	test -d "/tmp/beer-cli-prefix/drive_c/Windows/System32"; \
	test -d "/tmp/beer-cli-prefix/drive_c/Users/Player/AppData/Roaming"; \
	test -L "/tmp/beer-cli-prefix/dosdevices/c:"; \
	rm -rf /tmp/beer-cli-prefix; \
	echo "Strict Vulkan CLI and prefix smoke tests passed"

clean:
	rm -f $(TARGET) $(OBJS) $(DEPS) $(VULKAN_SPV)
