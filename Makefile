CC     = gcc
CFLAGS = -O0 -Wall -Wextra -m64 -gdwarf-4 -MMD -MP -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -fno-stack-protector -mno-sse -mno-sse2 -mno-avx -mno-mmx -Igraphics/d3d11 -Igraphics/d3d12 -Iaudio -Imedia -Iplatform -Irenderer/vulkan -Ithird_party/dxil-spirv -include platform/beer_env.h
RENDER_CFLAGS = $(filter-out -O0,$(CFLAGS)) -O3 -march=native
LDFLAGS = -lpthread -ldl -lcrypto -lz -lm
TARGET = loader
.DEFAULT_GOAL := $(TARGET)

AUDIO_SRCS = audio/audio_backend.c audio/wasapi_compat.c
MEDIA_SRCS = media/media_foundation.c
PLATFORM_SRCS = platform/beer_env.c platform/xwayland_backend.c platform/win32_environment.c platform/win32_shared_data.c platform/win32_apiset.c platform/win32_bcrypt.c platform/win32_version.c platform/win32_winhttp_url.c platform/win32_winhttp.c platform/win32_full_path.c platform/win32_gdi.c platform/win32_gdiplus.c platform/win32_resource.c platform/win32_address_wait.c platform/win32_registered_message.c platform/win32_tsf.c platform/beer_time.c platform/beer_control.c platform/beer_memory.c platform/beer_objects.c platform/beer_overlay.c platform/beer_post.c platform/beer_gpu_stats.c \
				platform/win32_nt_section.c platform/win32_virtual_memory.c platform/win32_entry.c platform/win32_onexit.c platform/win32_crt_init.c \
                platform/x64_fault_emulation.c platform/win32_vectored_exception.c platform/win32_unwind.c \
				platform/win32_seh.c platform/win32_syscall.c platform/beer_message_dialog.c platform/beer_steam_session.c platform/beer_steam_mms.c platform/beer_icon_png.c platform/win32_native_heap.c platform/win32_sddl.c platform/win32_shell_strings.c platform/win32_variant.c platform/win32_schannel.c
VULKAN_SRCS = renderer/vulkan/vulkan_context.c \
               renderer/vulkan/vulkan_presenter.c \
               renderer/vulkan/vulkan_post.c \
               renderer/vulkan/vulkan_bridge.c \
               renderer/vulkan/vulkan_map.c \
               renderer/vulkan/vulkan_renderer.c \
               renderer/vulkan/vulkan_graphics.c \
               renderer/vulkan/vulkan_indexed_renderer.c
GRAPHICS_SRCS = graphics/d3d11/d3d11_impl.c \
                graphics/d3d12/d3d12_api.c \
                graphics/d3d12/d3d12_shader_translator.c \
                graphics/d3d12/d3d12_dxil_translator.c \
                graphics/d3d12v/bd12_device.c graphics/d3d12v/bd12_cmd.c \
                graphics/d3d12v/bd12_descriptors.c graphics/d3d12v/bd12_bind.c \
                graphics/d3d12v/bd12_image.c graphics/d3d12v/bd12_graphics.c \
                graphics/d3d12v/bd12_extra.c graphics/d3d12v/bd12_formats.c \
                graphics/d3d12v/bd12_pso.c graphics/d3d12v/bd12_translate.c \
                graphics/d3d12v/bd12_com.c graphics/d3d12v/bd12_com_list.c graphics/d3d12v/bd12_pipelib.c graphics/d3d12v/bd12_cuda.c graphics/d3d12v/bd12_rootpush.c graphics/d3d12v/bd12_heapdb.c \
                graphics/d3d12v/bd12_raytracing.c graphics/d3d12v/bd12_mirror.c graphics/d3d12v/bd12_rtpipe.c graphics/d3d12v/bd12_ckpt.c graphics/d3d12v/bd12_gpuprof.c graphics/d3d12v/bd12_live.c \
                graphics/d3d12v/bd12_capture.c graphics/d3d12v/bd12_cbtweak.c \
                graphics/d3d11v/bd11_obj.c graphics/d3d11v/bd11_dev.c graphics/d3d11v/bd11_ctx.c \
                graphics/d3d11v/bd11_swap.c
SRCS = loader.c $(AUDIO_SRCS) $(MEDIA_SRCS) $(PLATFORM_SRCS) $(VULKAN_SRCS) $(GRAPHICS_SRCS)
OBJS = $(SRCS:.c=.o)
DEPS = $(OBJS:.o=.d)

-include $(DEPS)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJS) $(LDFLAGS)

# Steam session provider: a separate program (see docs/steam-library-broker-design.md), built with
# normal userspace flags rather than the loader's.
STEAM_PROVIDER = beer-steam
STEAM_SRCS = steam/provider.c steam/cm.c steam/auth.c steam/library.c steam/content.c steam/http.c steam/install.c \
             steam/net.c steam/pb.c steam/secret.c steam/vdf.c
STEAM_CFLAGS = -O2 -g -Wall -Wextra -Wno-format-truncation -D_GNU_SOURCE
$(STEAM_PROVIDER): $(STEAM_SRCS) $(wildcard steam/*.h)
	$(CC) $(STEAM_CFLAGS) -o $@ $(STEAM_SRCS) -lssl -lcrypto -lz -lzstd -lpthread -ldl

test-ki-dispatcher:
	$(CC) -O2 -Wall -Wextra -Iplatform platform/win32_ki_dispatcher_test.c -o /tmp/beer-ki-dispatcher-test
	/tmp/beer-ki-dispatcher-test

test-steam:
	$(CC) $(STEAM_CFLAGS) steam/pb_test.c steam/pb.c -o /tmp/beer-steam-pb-test
	/tmp/beer-steam-pb-test
	$(CC) $(STEAM_CFLAGS) steam/vdf_test.c steam/vdf.c -o /tmp/beer-steam-vdf-test
	/tmp/beer-steam-vdf-test
	$(CC) -O2 -Wall -Wextra -Iplatform platform/beer_steam_session_test.c platform/beer_steam_session.c -lpthread -o /tmp/beer-steam-session-test
	/tmp/beer-steam-session-test
	$(CC) -O2 -Wall -Wextra -D_GNU_SOURCE -Iplatform platform/beer_steam_mms_test.c platform/beer_steam_mms.c -lpthread -o /tmp/beer-steam-mms-test
	/tmp/beer-steam-mms-test

# Hot: every BEER_* switch lookup goes through it.
platform/beer_env.o: platform/beer_env.c platform/beer_env.h
	$(CC) $(RENDER_CFLAGS) -c $< -o $@

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

# BD11 (D3D11 on the BD12 core): same Win64-ABI/SSE needs as BD12.
graphics/d3d11v/%.o: graphics/d3d11v/%.c graphics/d3d11v/bd11.h
	$(CC) $(D3D12_ABI_CFLAGS) -fno-strict-aliasing -Igraphics/d3d12v -Igraphics/d3d11v -idirafter $(VULKAN_HEADERS) -c $< -o $@

# Memory accessor handles floats (strtod/strtof), which the SSE-less loader flags cannot compile.
platform/beer_memory.o: platform/beer_memory.c platform/beer_memory.h platform/beer_control.h
	$(CC) $(D3D12_ABI_CFLAGS) -Iplatform -c $< -o $@

# The object scanner does float math on candidate transforms.
platform/beer_objects.o: platform/beer_objects.c platform/beer_objects.h platform/beer_control.h
	$(CC) $(D3D12_ABI_CFLAGS) -O2 -Iplatform -c $< -o $@

# This CRT entry point calls host libc and real Win64 guest callbacks. Compile
# with full ABI support so the host transition also preserves XMM6-XMM15.
platform/win32_onexit.o: platform/win32_onexit.c platform/win32_onexit.h
	$(CC) $(D3D12_ABI_CFLAGS) -Iplatform -c $< -o $@

platform/win32_crt_init.o: platform/win32_crt_init.c platform/win32_crt_init.h
	$(CC) $(D3D12_ABI_CFLAGS) -Iplatform -c $< -o $@

platform/win32_shell_strings.o: platform/win32_shell_strings.c platform/win32_shell_strings.h
	$(CC) $(D3D12_ABI_CFLAGS) -Iplatform -c $< -o $@

# Schannel calls the host OpenSSL (loaded at run time) and is called by guest code.
platform/win32_schannel.o: platform/win32_schannel.c platform/win32_schannel.h
	$(CC) $(D3D12_ABI_CFLAGS) -Iplatform -c $< -o $@

# GDI+ takes REAL (float) parameters in XMM registers and calls the guest's IStream through its vtable.
platform/win32_gdiplus.o: platform/win32_gdiplus.c platform/win32_gdiplus.h platform/win32_gdi.h
	$(CC) $(D3D12_ABI_CFLAGS) -O2 -Iplatform -c $< -o $@

# VARIANT conversions use floating point.
platform/win32_variant.o: platform/win32_variant.c platform/win32_variant.h
	$(CC) $(D3D12_ABI_CFLAGS) -Iplatform -c $< -o $@

# COM objects the guest calls directly through their vtables (no dispatch trampoline): WASAPI, Media Foundation and
# Text Services. With -mno-sse GCC cannot keep the Win64 nonvolatile XMM6-XMM15 across the libc calls these methods
# make, so callers saw them clobbered (the same fault that stopped RE9's frame limiter through DirectInput).
# With SSE, GCC saves and restores XMM6-XMM15 in every ms_abi function.
audio/wasapi_compat.o: audio/wasapi_compat.c audio/wasapi_compat.h
	$(CC) $(D3D12_ABI_CFLAGS) -Iaudio -c $< -o $@

media/media_foundation.o: media/media_foundation.c media/media_foundation.h
	$(CC) $(D3D12_ABI_CFLAGS) -Imedia -c $< -o $@

platform/win32_tsf.o: platform/win32_tsf.c platform/win32_tsf.h
	$(CC) $(D3D12_ABI_CFLAGS) -Iplatform -c $< -o $@

# The on-disk shader translation cache is keyed by the translator's own sources (not the loader binary), so a rebuild
# that does not touch translation keeps the cache. Any change to these files rebuilds this object with a new key.
TRANSLATOR_KEY_SRCS = graphics/d3d12/d3d12_shader_translator.c graphics/d3d12/d3d12_shader_translator.h \
                      graphics/d3d12/d3d12_dxil_translator.c graphics/d3d12/d3d12_dxil_translator.h
graphics/d3d12/d3d12_shader_translator.o: $(TRANSLATOR_KEY_SRCS)
	$(CC) $(RENDER_CFLAGS) -DBEER_TRANSLATOR_SOURCE_KEY=0x$(shell cat $(TRANSLATOR_KEY_SRCS) | sha1sum | cut -c1-16)ull \
	      -c $< -o $@

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

.PHONY: test-bd12-rt dxil-spirv clean smoke-cli test-audio test-d3d11 test-d3d12 test-d3d12v test-media test-renderer test-input test-steam test-ki-dispatcher

test-d3d11: $(filter-out loader.o,$(OBJS))
	$(CC) -O2 -Wall -Wextra -pthread -Igraphics/d3d11 \
		graphics/d3d11/d3d11_lifetime_test.c $(filter-out loader.o,$(OBJS)) \
		$(LDFLAGS) -o /tmp/beer-d3d11-lifetime-test
	/tmp/beer-d3d11-lifetime-test
	rm -f /tmp/beer-d3d11-lifetime-test

test-native-loader: $(filter-out loader.o,$(OBJS))
	$(CC) -O2 -Wall -Wextra -pthread -Iplatform platform/win32_native_heap_test.c platform/win32_native_heap.c -o /tmp/beer-win32-native-heap-test
	/tmp/beer-win32-native-heap-test
	$(CC) -O2 -Wall -Wextra -Iplatform platform/win32_static_tls_test.c -o /tmp/beer-win32-static-tls-test
	/tmp/beer-win32-static-tls-test
	$(CC) $(CFLAGS) -ffunction-sections -fdata-sections -Wl,--gc-sections tests/native_loader_contract_test.c $(filter-out loader.o,$(OBJS)) $(LDFLAGS) -o /tmp/beer-native-loader-contract-test
	/tmp/beer-native-loader-contract-test
	rm -f /tmp/beer-native-loader-contract-test
	rm -f /tmp/beer-win32-native-heap-test /tmp/beer-win32-static-tls-test

.PHONY: test-native-loader

test-native-sync: $(filter-out loader.o,$(OBJS))
	$(CC) $(CFLAGS) -ffunction-sections -fdata-sections -Wl,--gc-sections tests/native_sync_contract_test.c $(filter-out loader.o,$(OBJS)) $(LDFLAGS) -o /tmp/beer-native-sync-contract-test
	/tmp/beer-native-sync-contract-test
	rm -f /tmp/beer-native-sync-contract-test

.PHONY: test-native-sync

test-adapter-info:
	$(CC) -O2 -Wall -Wextra -Iplatform platform/win32_adapter_info_test.c -o /tmp/beer-adapter-info-test
	/tmp/beer-adapter-info-test

.PHONY: test-adapter-info

test-sddl:
	$(CC) -O2 -Wall -Wextra -Iplatform platform/win32_sddl_test.c platform/win32_sddl.c -o /tmp/beer-sddl-test
	/tmp/beer-sddl-test

.PHONY: test-sddl

test-shell-strings:
	$(CC) -O2 -Wall -Wextra -Iplatform platform/win32_shell_strings_test.c platform/win32_shell_strings.c -o /tmp/beer-shell-strings-test
	/tmp/beer-shell-strings-test
	$(CC) -O2 -Wall -Wextra -Iplatform platform/win32_command_line_test.c -o /tmp/beer-command-line-test
	/tmp/beer-command-line-test

.PHONY: test-shell-strings

test-input: test-native-loader test-shell-strings test-adapter-info
	$(CC) -O2 -Wall -Wextra -Iplatform platform/win32_version_test.c platform/win32_version.c -o /tmp/beer-win32-version-test
	/tmp/beer-win32-version-test
	rm -f /tmp/beer-win32-version-test
	$(CC) -O2 -Wall -Wextra -Iplatform platform/beer_overlay_test.c platform/beer_overlay.c -lpthread -o /tmp/beer-overlay-test
	/tmp/beer-overlay-test
	rm -f /tmp/beer-overlay-test
	$(CC) -O2 -Wall -Wextra -DBD12_PIPELIB_STANDALONE graphics/d3d12v/bd12_pipelib_test.c graphics/d3d12v/bd12_pipelib.c -o /tmp/beer-pipelib-test
	/tmp/beer-pipelib-test
	rm -f /tmp/beer-pipelib-test
	$(CC) -O2 -Wall -Wextra -Iplatform platform/win32_full_path_test.c platform/win32_full_path.c -o /tmp/beer-full-path-test
	/tmp/beer-full-path-test
	rm -f /tmp/beer-full-path-test
	$(CC) -O2 -Wall -Wextra -Iplatform platform/win32_winhttp_test.c platform/win32_winhttp.c -lpthread -o /tmp/beer-winhttp-test
	/tmp/beer-winhttp-test > /dev/null
	rm -f /tmp/beer-winhttp-test
	$(CC) -O2 -Wall -Wextra -Iplatform platform/win32_winhttp_url_test.c platform/win32_winhttp_url.c -o /tmp/beer-winhttp-url-test
	/tmp/beer-winhttp-url-test
	rm -f /tmp/beer-winhttp-url-test
	$(CC) -O2 -Wall -Wextra -Iplatform platform/win32_syscall_test.c platform/win32_syscall.c -o /tmp/beer-win32-syscall-test
	/tmp/beer-win32-syscall-test
	rm -f /tmp/beer-win32-syscall-test
	$(CC) -O2 -Wall -Wextra -Iplatform platform/win32_process_parameters_test.c -o /tmp/beer-win32-process-parameters-test
	/tmp/beer-win32-process-parameters-test
	rm -f /tmp/beer-win32-process-parameters-test
	$(CC) -O2 -Wall -Wextra -Iplatform platform/win32_crt_init_test.c platform/win32_crt_init.c -o /tmp/beer-win32-crt-init-test
	/tmp/beer-win32-crt-init-test
	rm -f /tmp/beer-win32-crt-init-test
	$(CC) -O2 -Wall -Wextra -pthread -Iplatform platform/win32_onexit_test.c platform/win32_onexit.c -o /tmp/beer-win32-onexit-test
	/tmp/beer-win32-onexit-test
	rm -f /tmp/beer-win32-onexit-test
	$(CC) -O2 -Wall -Wextra -Iplatform platform/win32_ui_language_test.c -o /tmp/beer-win32-ui-language-test
	/tmp/beer-win32-ui-language-test
	rm -f /tmp/beer-win32-ui-language-test
	$(CC) -O2 -Wall -Wextra -Iplatform platform/win32_entry_test.c platform/win32_entry.c -o /tmp/beer-win32-entry-test
	/tmp/beer-win32-entry-test
	rm -f /tmp/beer-win32-entry-test
	$(CC) -O2 -Wall -Wextra -Iplatform platform/directinput_format_test.c -o /tmp/beer-directinput-format-test
	/tmp/beer-directinput-format-test
	$(CC) -O2 -Wall -Wextra -Iplatform platform/window_style_policy_test.c -o /tmp/beer-window-style-policy-test
	/tmp/beer-window-style-policy-test
	$(CC) -O2 -Wall -Wextra -Iplatform platform/window_icon_policy_test.c -o /tmp/beer-window-icon-policy-test
	$(CC) -O2 -Wall -Wextra -Iplatform platform/beer_icon_png_test.c platform/beer_icon_png.c -lz -o /tmp/beer-icon-png-test
	/tmp/beer-icon-png-test
	/tmp/beer-window-icon-policy-test
	$(CC) -O2 -Wall -Wextra -Iplatform platform/guid_compat_test.c -o /tmp/beer-guid-compat-test
	/tmp/beer-guid-compat-test
	$(CC) -O2 -Wall -Wextra -Iplatform platform/win32_filesystem_policy_test.c -o /tmp/beer-win32-filesystem-policy-test
	/tmp/beer-win32-filesystem-policy-test
	$(CC) -O2 -Wall -Wextra -Iplatform platform/steam_offline_policy_test.c -o /tmp/beer-steam-offline-policy-test
	/tmp/beer-steam-offline-policy-test
	$(CC) -O2 -Wall -Wextra -Iplatform platform/win32_environment_test.c platform/win32_environment.c -o /tmp/beer-win32-environment-test
	/tmp/beer-win32-environment-test
	$(CC) -O2 -Wall -Wextra -Iplatform platform/win32_apiset_test.c platform/win32_apiset.c -o /tmp/beer-win32-apiset-test
	/tmp/beer-win32-apiset-test
	$(CC) -O2 -Wall -Wextra -Iplatform platform/win32_tsf_test.c platform/win32_tsf.c -o /tmp/beer-win32-tsf-test
	/tmp/beer-win32-tsf-test
	$(CC) -O2 -Wall -Wextra -pthread -Iplatform platform/win32_registered_message_test.c platform/win32_registered_message.c -o /tmp/beer-win32-registered-message-test
	/tmp/beer-win32-registered-message-test
	$(CC) -O2 -Wall -Wextra -Iplatform platform/win32_kernel32_contract_test.c -o /tmp/beer-win32-kernel32-contract-test
	/tmp/beer-win32-kernel32-contract-test
	$(CC) -O2 -Wall -Wextra -pthread -Iplatform platform/win32_address_wait_test.c platform/win32_address_wait.c -o /tmp/beer-win32-address-wait-test
	/tmp/beer-win32-address-wait-test
	$(CC) -O2 -Wall -Wextra -Iplatform platform/win32_ip_adapter_test.c -o /tmp/beer-win32-ip-adapter-test
	/tmp/beer-win32-ip-adapter-test
	$(CC) -O2 -Wall -Wextra -pthread -Iplatform platform/win32_shared_data_test.c platform/win32_shared_data.c platform/beer_time.c -lm -o /tmp/beer-win32-shared-data-test
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
	rm -f /tmp/beer-directinput-format-test /tmp/beer-window-style-policy-test /tmp/beer-window-icon-policy-test /tmp/beer-guid-compat-test /tmp/beer-win32-filesystem-policy-test /tmp/beer-steam-offline-policy-test /tmp/beer-win32-environment-test /tmp/beer-win32-apiset-test /tmp/beer-win32-kernel32-contract-test /tmp/beer-win32-address-wait-test /tmp/beer-win32-ip-adapter-test /tmp/beer-win32-shared-data-test /tmp/beer-win32-nt-section-test /tmp/beer-win32-virtual-memory-test /tmp/beer-x64-fault-emulation-test /tmp/beer-win32-vectored-exception-test /tmp/beer-win32-unwind-test /tmp/beer-win32-seh-test

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
              graphics/d3d12v/bd12_extra.c graphics/d3d12v/bd12_mirror.c \
              graphics/d3d12v/bd12_raytracing.c graphics/d3d12v/bd12_capture.c \
              graphics/d3d12v/bd12_heapdb.c graphics/d3d12v/bd12_rootpush.c
D3D12V_CFLAGS = -O2 -g -Wall -Wextra -pthread -Igraphics/d3d12v -Igraphics/d3d12 -Irenderer/vulkan -idirafter $(VULKAN_HEADERS)

# Standalone tests for the GPU-resident D3D12-on-Vulkan core (no game needed).
test-bd12-rt:
	$(VULKAN_GLSLC) --target-env=vulkan1.3 graphics/d3d12v/tests/rt_trace.comp -o /tmp/bd12-rt.spv
	$(CC) $(D3D12V_CFLAGS) graphics/d3d12v/bd12_rt_test.c $(D3D12V_SRCS) graphics/d3d12v/bd12_formats.c renderer/vulkan/vulkan_context.c $(LDFLAGS) -o /tmp/bd12-rt-test
	/tmp/bd12-rt-test /tmp/bd12-rt.spv

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
	$(CC) $(D3D12V_CFLAGS) -Igraphics/d3d12 graphics/d3d12v/bd12_com_test.c $(D3D12V_SRCS) graphics/d3d12v/bd12_formats.c graphics/d3d12v/bd12_pso.c graphics/d3d12v/bd12_translate.c graphics/d3d12v/bd12_com.c graphics/d3d12v/bd12_com_list.c graphics/d3d12v/bd12_pipelib.c graphics/d3d12/d3d12_shader_translator.c renderer/vulkan/vulkan_context.c $(LDFLAGS) -o /tmp/bd12-com-test
	/tmp/bd12-m1-test /tmp/bd12-m1.spv
	/tmp/bd12-m2-test /tmp/bd12-m2.spv
	/tmp/bd12-m3-test /tmp/bd12-m3.spv
	/tmp/bd12-m4-test /tmp/bd12-m4.vert.spv /tmp/bd12-m4.frag.spv
	/tmp/bd12-com-test
	rm -f /tmp/bd12-com-test /tmp/bd12-m1-test /tmp/bd12-m2-test /tmp/bd12-m3-test /tmp/bd12-m4-test /tmp/bd12-m4.vert.spv /tmp/bd12-m4.frag.spv /tmp/bd12-m1.spv /tmp/bd12-m2.spv /tmp/bd12-m3.spv

test-d3d12:
	$(CC) -O2 -Wall -Wextra -pthread -Igraphics/d3d12 -Ithird_party/dxil-spirv graphics/d3d12/d3d12_shader_translator_test.c graphics/d3d12/d3d12_shader_translator.c graphics/d3d12/d3d12_dxil_translator.c -ldl -o /tmp/beer-d3d12-shader-translator-test
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

# DXIL -> SPIR-V translator used for DXR shader libraries (loaded with dlopen).
DXIL_SPIRV_DIR := third_party/dxil-spirv
DXIL_SPIRV_LIB := $(DXIL_SPIRV_DIR)/build/libdxil-spirv-c-shared.so

dxil-spirv: $(DXIL_SPIRV_LIB)

$(DXIL_SPIRV_LIB):
	cmake -S $(DXIL_SPIRV_DIR) -B $(DXIL_SPIRV_DIR)/build -G Ninja -DCMAKE_BUILD_TYPE=Release
	ninja -C $(DXIL_SPIRV_DIR)/build dxil-spirv-c-shared

clean:
	rm -f $(TARGET) $(OBJS) $(DEPS) $(VULKAN_SPV)
