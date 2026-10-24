CC     = gcc
CFLAGS = -O0 -Wall -Wextra -m64 -g -MMD -MP -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -fno-stack-protector -mno-sse -mno-sse2 -mno-avx -mno-mmx -Igraphics/d3d11 -Igraphics/d3d12 -Iaudio -Imedia -Iplatform -Irenderer/vulkan
RENDER_CFLAGS = $(filter-out -O0,$(CFLAGS)) -O3 -march=native
LDFLAGS = -lpthread -ldl
TARGET = loader
.DEFAULT_GOAL := $(TARGET)

AUDIO_SRCS = audio/audio_backend.c audio/wasapi_compat.c
MEDIA_SRCS = media/media_foundation.c
PLATFORM_SRCS = platform/xwayland_backend.c platform/win32_environment.c platform/win32_shared_data.c \
                platform/win32_nt_section.c platform/win32_virtual_memory.c \
                platform/x64_fault_emulation.c platform/win32_vectored_exception.c platform/win32_unwind.c \
                platform/win32_seh.c
VULKAN_SRCS = renderer/vulkan/vulkan_context.c \
               renderer/vulkan/vulkan_presenter.c \
               renderer/vulkan/vulkan_renderer.c \
               renderer/vulkan/vulkan_graphics.c \
               renderer/vulkan/vulkan_indexed_renderer.c
GRAPHICS_SRCS = graphics/d3d11/d3d11_impl.c \
                graphics/d3d12/d3d12_api.c \
                graphics/d3d12/d3d12_shader_translator.c \
                graphics/d3d12v/bd12_device.c graphics/d3d12v/bd12_cmd.c \
                graphics/d3d12v/bd12_descriptors.c graphics/d3d12v/bd12_bind.c \
                graphics/d3d12v/bd12_image.c graphics/d3d12v/bd12_graphics.c \
                graphics/d3d12v/bd12_extra.c graphics/d3d12v/bd12_formats.c \
                graphics/d3d12v/bd12_pso.c graphics/d3d12v/bd12_translate.c \
                graphics/d3d12v/bd12_com.c graphics/d3d12v/bd12_com_list.c
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

# BD12 core and COM shim: Vulkan host code plus Win64 ABI entry points with
# floating-point parameters, so it needs SSE like the compat object.
graphics/d3d12v/%.o: graphics/d3d12v/%.c
	$(CC) $(D3D12_ABI_CFLAGS) -Igraphics/d3d12v -idirafter $(VULKAN_HEADERS) -c $< -o $@

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

.PHONY: clean smoke-cli test-audio test-d3d11 test-d3d12 test-d3d12v test-media test-renderer test-input

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
	$(CC) -O2 -Wall -Wextra -Iplatform platform/win32_environment_test.c platform/win32_environment.c -o /tmp/beer-win32-environment-test
	/tmp/beer-win32-environment-test
	$(CC) -O2 -Wall -Wextra -pthread -Iplatform platform/win32_shared_data_test.c platform/win32_shared_data.c -o /tmp/beer-win32-shared-data-test
	/tmp/beer-win32-shared-data-test
	$(CC) -O2 -Wall -Wextra -pthread -Iplatform platform/win32_nt_section_test.c platform/win32_nt_section.c -o /tmp/beer-win32-nt-section-test
	/tmp/beer-win32-nt-section-test
	$(CC) -O2 -Wall -Wextra -pthread -Iplatform platform/win32_virtual_memory_test.c platform/win32_virtual_memory.c -o /tmp/beer-win32-virtual-memory-test
	/tmp/beer-win32-virtual-memory-test
	$(CC) -O2 -Wall -Wextra -Iplatform platform/x64_fault_emulation_test.c platform/x64_fault_emulation.c -o /tmp/beer-x64-fault-emulation-test
	/tmp/beer-x64-fault-emulation-test
	$(CC) -O2 -Wall -Wextra -Iplatform platform/win32_vectored_exception_test.c platform/win32_vectored_exception.c -o /tmp/beer-win32-vectored-exception-test
	/tmp/beer-win32-vectored-exception-test
	$(CC) -O2 -Wall -Wextra -Iplatform platform/win32_unwind_test.c platform/win32_unwind.c -o /tmp/beer-win32-unwind-test
	/tmp/beer-win32-unwind-test
	$(CC) -O2 -Wall -Wextra -no-pie -Wl,--section-start=.beer_test_handler=0x500000 -Iplatform platform/win32_seh_test.c platform/win32_seh.c platform/win32_unwind.c -o /tmp/beer-win32-seh-test
	/tmp/beer-win32-seh-test
	rm -f /tmp/beer-directinput-format-test /tmp/beer-window-style-policy-test /tmp/beer-window-icon-policy-test /tmp/beer-guid-compat-test /tmp/beer-win32-filesystem-policy-test /tmp/beer-steam-offline-policy-test /tmp/beer-win32-environment-test /tmp/beer-win32-shared-data-test /tmp/beer-win32-nt-section-test /tmp/beer-win32-virtual-memory-test /tmp/beer-x64-fault-emulation-test /tmp/beer-win32-vectored-exception-test /tmp/beer-win32-unwind-test /tmp/beer-win32-seh-test

test-renderer:
	$(CC) -O2 -Wall -Wextra -Irenderer/vulkan renderer/vulkan/target_mirror_test.c -o /tmp/beer-target-mirror-test
	/tmp/beer-target-mirror-test
	rm -f /tmp/beer-target-mirror-test
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
	$(CC) -O2 -Wall -Wextra -pthread -Irenderer/vulkan -idirafter $(VULKAN_HEADERS) renderer/vulkan/vulkan_graphics_test.c renderer/vulkan/vulkan_graphics.c renderer/vulkan/vulkan_context.c -ldl -o /tmp/beer-vulkan-graphics-test
	/tmp/beer-vulkan-graphics-test /tmp/beer-fullscreen.vert.spv /tmp/beer-fullscreen.frag.spv /tmp/beer-fullscreen-cb.frag.spv /tmp/beer-fullscreen-depth.frag.spv /tmp/beer-fullscreen-buffer.frag.spv /tmp/beer-indexed.vert.spv /tmp/beer-indexed.frag.spv /tmp/beer-fullscreen-array.frag.spv /tmp/beer-fullscreen-volume.frag.spv /tmp/beer-fullscreen-1d-array.frag.spv /tmp/beer-fullscreen-mrt.frag.spv /tmp/beer-fullscreen-texel.vert.spv /tmp/beer-fullscreen-texel.frag.spv /tmp/beer-fullscreen-mip.frag.spv /tmp/beer-fullscreen-sample.frag.spv /tmp/beer-fullscreen-cube.frag.spv
	rm -f /tmp/beer-vulkan-graphics-test /tmp/beer-fullscreen.vert.spv /tmp/beer-fullscreen.frag.spv /tmp/beer-fullscreen-cb.frag.spv /tmp/beer-fullscreen-depth.frag.spv /tmp/beer-fullscreen-buffer.frag.spv /tmp/beer-indexed.vert.spv /tmp/beer-indexed.frag.spv /tmp/beer-fullscreen-array.frag.spv /tmp/beer-fullscreen-volume.frag.spv /tmp/beer-fullscreen-1d-array.frag.spv /tmp/beer-fullscreen-mrt.frag.spv /tmp/beer-fullscreen-texel.vert.spv /tmp/beer-fullscreen-texel.frag.spv /tmp/beer-fullscreen-mip.frag.spv /tmp/beer-fullscreen-sample.frag.spv /tmp/beer-fullscreen-cube.frag.spv

D3D12V_SRCS = graphics/d3d12v/bd12_device.c graphics/d3d12v/bd12_cmd.c \
              graphics/d3d12v/bd12_descriptors.c graphics/d3d12v/bd12_bind.c \
              graphics/d3d12v/bd12_image.c graphics/d3d12v/bd12_graphics.c \
              graphics/d3d12v/bd12_extra.c
D3D12V_CFLAGS = -O2 -g -Wall -Wextra -pthread -Igraphics/d3d12v -Irenderer/vulkan -idirafter $(VULKAN_HEADERS)

# Standalone tests for the GPU-resident D3D12-on-Vulkan core (no game needed).
test-d3d12v:
	$(VULKAN_GLSLC) --target-env=vulkan1.3 graphics/d3d12v/tests/m1_va_double.comp -o /tmp/bd12-m1.spv
	$(VULKAN_GLSLC) --target-env=vulkan1.3 graphics/d3d12v/tests/m2_bound.comp -o /tmp/bd12-m2.spv
	$(VULKAN_GLSLC) --target-env=vulkan1.3 graphics/d3d12v/tests/m3_image.comp -o /tmp/bd12-m3.spv
	$(VULKAN_GLSLC) --target-env=vulkan1.3 graphics/d3d12v/tests/m4_quad.vert -o /tmp/bd12-m4.vert.spv
	$(VULKAN_GLSLC) --target-env=vulkan1.3 graphics/d3d12v/tests/m4_quad.frag -o /tmp/bd12-m4.frag.spv
	$(CC) $(D3D12V_CFLAGS) graphics/d3d12v/bd12_m1_test.c $(D3D12V_SRCS) renderer/vulkan/vulkan_context.c $(LDFLAGS) -o /tmp/bd12-m1-test
	$(CC) $(D3D12V_CFLAGS) graphics/d3d12v/bd12_m2_test.c $(D3D12V_SRCS) renderer/vulkan/vulkan_context.c $(LDFLAGS) -o /tmp/bd12-m2-test
	$(CC) $(D3D12V_CFLAGS) graphics/d3d12v/bd12_m3_test.c $(D3D12V_SRCS) renderer/vulkan/vulkan_context.c $(LDFLAGS) -o /tmp/bd12-m3-test
	$(CC) $(D3D12V_CFLAGS) graphics/d3d12v/bd12_m4_test.c $(D3D12V_SRCS) renderer/vulkan/vulkan_context.c $(LDFLAGS) -o /tmp/bd12-m4-test
	$(CC) $(D3D12V_CFLAGS) -Igraphics/d3d12 graphics/d3d12v/bd12_com_test.c $(D3D12V_SRCS) graphics/d3d12v/bd12_formats.c graphics/d3d12v/bd12_pso.c graphics/d3d12v/bd12_translate.c graphics/d3d12v/bd12_com.c graphics/d3d12v/bd12_com_list.c graphics/d3d12/d3d12_shader_translator.c renderer/vulkan/vulkan_context.c $(LDFLAGS) -o /tmp/bd12-com-test
	/tmp/bd12-m1-test /tmp/bd12-m1.spv
	/tmp/bd12-m2-test /tmp/bd12-m2.spv
	/tmp/bd12-m3-test /tmp/bd12-m3.spv
	/tmp/bd12-m4-test /tmp/bd12-m4.vert.spv /tmp/bd12-m4.frag.spv
	/tmp/bd12-com-test
	rm -f /tmp/bd12-com-test /tmp/bd12-m1-test /tmp/bd12-m2-test /tmp/bd12-m3-test /tmp/bd12-m4-test /tmp/bd12-m4.vert.spv /tmp/bd12-m4.frag.spv /tmp/bd12-m1.spv /tmp/bd12-m2.spv /tmp/bd12-m3.spv

test-d3d12:
	$(CC) -O2 -Wall -Wextra -pthread -Igraphics/d3d12 graphics/d3d12/d3d12_shader_translator_test.c graphics/d3d12/d3d12_shader_translator.c -ldl -o /tmp/beer-d3d12-shader-translator-test
	@if test -f /tmp/beer-d3d12-31b662b443572be7.dxbc; then \
		/tmp/beer-d3d12-shader-translator-test /tmp/beer-d3d12-31b662b443572be7.dxbc; \
	else \
		echo "D3D12 translation fixture absent; runtime translation test skipped"; \
	fi
	rm -f /tmp/beer-d3d12-shader-translator-test

test-audio:
	$(CC) -O2 -Wall -Wextra -pthread -Iaudio audio/audio_backend_test.c audio/audio_backend.c -ldl -o /tmp/beer-audio-test
	timeout 10s /tmp/beer-audio-test
	$(CC) -O2 -Wall -Wextra -pthread -Iaudio audio/wasapi_compat_test.c audio/wasapi_compat.c audio/audio_backend.c -ldl -o /tmp/beer-wasapi-test
	timeout 10s /tmp/beer-wasapi-test
	rm -f /tmp/beer-audio-test /tmp/beer-wasapi-test

test-media:
	$(CC) -O2 -Wall -Wextra -Imedia -Iplatform media/media_foundation_test.c media/media_foundation.c platform/xwayland_backend.c -o /tmp/beer-media-test -lpthread -ldl
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
