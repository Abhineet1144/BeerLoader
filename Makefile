CC     = gcc
CFLAGS = -O0 -Wall -Wextra -m64 -g -MMD -MP -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -fno-stack-protector -mno-sse -mno-sse2 -mno-avx -mno-mmx -Igraphics/d3d11 -Igraphics/d3d12 -Iaudio -Imedia -Iplatform -Irenderer/vulkan
RENDER_CFLAGS = $(filter-out -O0,$(CFLAGS)) -O3 -march=native
LDFLAGS = -lpthread -ldl
TARGET = loader
.DEFAULT_GOAL := $(TARGET)

AUDIO_SRCS = audio/audio_backend.c
MEDIA_SRCS = media/media_foundation.c
PLATFORM_SRCS = platform/xwayland_backend.c
VULKAN_SRCS = renderer/vulkan/vulkan_context.c \
               renderer/vulkan/vulkan_presenter.c \
               renderer/vulkan/vulkan_renderer.c \
               renderer/vulkan/vulkan_indexed_renderer.c
GRAPHICS_SRCS = graphics/d3d11/d3d11_impl.c \
                graphics/d3d12/d3d12_compat.c \
                graphics/d3d12/d3d12_compute.c
SRCS = loader.c $(AUDIO_SRCS) $(MEDIA_SRCS) $(PLATFORM_SRCS) $(VULKAN_SRCS) $(GRAPHICS_SRCS)
OBJS = $(SRCS:.c=.o)
DEPS = $(OBJS:.o=.d)

-include $(DEPS)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJS) $(LDFLAGS)

graphics/d3d11/d3d11_impl.o: graphics/d3d11/d3d11_impl.c
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

renderer/vulkan/vulkan_presenter.o: renderer/vulkan/vulkan_presenter.c renderer/vulkan/vulkan_presenter.h renderer/vulkan/vulkan_context.h
	$(CC) $(RENDER_CFLAGS) -idirafter $(VULKAN_HEADERS) -c $< -o $@

renderer/vulkan/vulkan_renderer.o: renderer/vulkan/vulkan_renderer.c renderer/vulkan/vulkan_renderer.h renderer/vulkan/vulkan_context.h $(VULKAN_COMPOSITOR_SPV)
	$(CC) $(RENDER_CFLAGS) -idirafter $(VULKAN_HEADERS) -c $< -o $@

renderer/vulkan/vulkan_indexed_renderer.o: renderer/vulkan/vulkan_indexed_renderer.c renderer/vulkan/vulkan_indexed_renderer.h renderer/vulkan/vulkan_context.h $(VULKAN_INDEXED_SPV)
	$(CC) $(RENDER_CFLAGS) -idirafter $(VULKAN_HEADERS) -c $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

.PHONY: clean smoke-cli test-audio test-d3d12 test-media test-renderer test-input

test-input:
	$(CC) -O2 -Wall -Wextra -Iplatform platform/directinput_format_test.c -o /tmp/beer-directinput-format-test
	/tmp/beer-directinput-format-test
	rm -f /tmp/beer-directinput-format-test

test-renderer:
	$(CC) -O2 -Wall -Wextra -Irenderer/vulkan renderer/vulkan/target_mirror_test.c -o /tmp/beer-target-mirror-test
	/tmp/beer-target-mirror-test
	rm -f /tmp/beer-target-mirror-test

test-d3d12:
	$(CC) -O2 -Wall -Wextra -Igraphics/d3d12 graphics/d3d12/d3d12_compat_test.c graphics/d3d12/d3d12_compat.c -o /tmp/beer-d3d12-test
	/tmp/beer-d3d12-test
	$(CC) -O2 -Wall -Wextra -Igraphics/d3d12 graphics/d3d12/d3d12_compute_test.c graphics/d3d12/d3d12_compute.c -o /tmp/beer-d3d12-compute-test
	/tmp/beer-d3d12-compute-test
	rm -f /tmp/beer-d3d12-test /tmp/beer-d3d12-compute-test

test-audio:
	$(CC) -O2 -Wall -Wextra -pthread -Iaudio audio/audio_backend_test.c audio/audio_backend.c -ldl -o /tmp/beer-audio-test
	timeout 10s /tmp/beer-audio-test
	rm -f /tmp/beer-audio-test

test-media:
	$(CC) -O2 -Wall -Wextra -Imedia media/media_foundation_test.c media/media_foundation.c -o /tmp/beer-media-test
	/tmp/beer-media-test
	rm -f /tmp/beer-media-test

smoke-cli: $(TARGET)
	@set -e; \
	./$(TARGET) --renderer=vulkan --presenter=vulkan /tmp/beer-cli-smoke-missing.exe 2>&1 | grep -F "[RENDERER] selected Vulkan-only rendering" >/dev/null; \
	./$(TARGET) --renderer=cpu /tmp/beer-cli-smoke-missing.exe 2>&1 | grep -F "only vulkan is supported" >/dev/null; \
	./$(TARGET) --presenter=x11 /tmp/beer-cli-smoke-missing.exe 2>&1 | grep -F "only vulkan is supported" >/dev/null; \
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
