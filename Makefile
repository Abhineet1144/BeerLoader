CC     = gcc
CFLAGS = -O0 -Wall -Wextra -m64 -g -MMD -MP -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -fno-stack-protector -mno-sse -mno-sse2 -mno-avx -mno-mmx -Igraphics
RENDER_CFLAGS = $(filter-out -O0,$(CFLAGS)) -O3 -march=native
LDFLAGS = -lpthread -ldl
TARGET = loader
.DEFAULT_GOAL := $(TARGET)
SRCS = loader.c xwayland_backend.c vulkan_presenter.c vulkan_renderer.c vulkan_indexed_renderer.c graphics/d3d11_impl.c
OBJS = $(SRCS:.c=.o)
DEPS = $(OBJS:.o=.d)

-include $(DEPS)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJS) $(LDFLAGS)

graphics/d3d11_impl.o: graphics/d3d11_impl.c
	$(CC) $(RENDER_CFLAGS) -c $< -o $@

xwayland_backend.o: xwayland_backend.c
	$(CC) $(RENDER_CFLAGS) -c $< -o $@

VULKAN_HEADERS ?= ../Android/toolchain/android-ndk-r30/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/include

vulkan_presenter.o: vulkan_presenter.c vulkan_presenter.h
	$(CC) $(RENDER_CFLAGS) -idirafter $(VULKAN_HEADERS) -c $< -o $@

VULKAN_GLSLC ?= ../Android/toolchain/android-ndk-r30/shader-tools/linux-x86_64/glslc
VULKAN_COMPOSITOR_SPV = shaders/fullscreen_composite.comp.spv
VULKAN_INDEXED_SPV = shaders/indexed_scaleform.comp.spv
VULKAN_SPV = $(VULKAN_COMPOSITOR_SPV) $(VULKAN_INDEXED_SPV)

$(VULKAN_COMPOSITOR_SPV): shaders/fullscreen_composite.comp
	$(VULKAN_GLSLC) -O $< -o $@

$(VULKAN_INDEXED_SPV): shaders/indexed_scaleform.comp
	$(VULKAN_GLSLC) -O $< -o $@

vulkan_renderer.o: vulkan_renderer.c vulkan_renderer.h $(VULKAN_COMPOSITOR_SPV)
	$(CC) $(RENDER_CFLAGS) -idirafter $(VULKAN_HEADERS) -c $< -o $@

vulkan_indexed_renderer.o: vulkan_indexed_renderer.c vulkan_indexed_renderer.h $(VULKAN_INDEXED_SPV)
	$(CC) $(RENDER_CFLAGS) -idirafter $(VULKAN_HEADERS) -c $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

.PHONY: clean smoke-cli

smoke-cli: $(TARGET)
	@set -e; \
	./$(TARGET) --renderer=vulkan --presenter=vulkan /tmp/beer-cli-smoke-missing.exe 2>&1 | grep -F "[RENDERER] selected Vulkan-only rendering" >/dev/null; \
	./$(TARGET) --renderer=cpu /tmp/beer-cli-smoke-missing.exe 2>&1 | grep -F "only vulkan is supported" >/dev/null; \
	./$(TARGET) --presenter=x11 /tmp/beer-cli-smoke-missing.exe 2>&1 | grep -F "only vulkan is supported" >/dev/null; \
	echo "Strict Vulkan CLI smoke tests passed"

clean:
	rm -f $(TARGET) $(OBJS) $(DEPS) $(VULKAN_SPV)
