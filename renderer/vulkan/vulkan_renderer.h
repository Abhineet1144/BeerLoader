#ifndef BEER_VULKAN_RENDERER_H
#define BEER_VULKAN_RENDERER_H

#include <stddef.h>
#include <stdint.h>

/* Executes Beer's two verified fullscreen D3D11 compositor paths on Vulkan.
 * mode 1: R10G10B10A2 scene * RGBA8 UI alpha + RGBA8 UI -> R10G10B10A2
 * mode 2: R10G10B10A2 intermediate -> RGBA8 target
 * Returns 1 only when the GPU operation completed and target was populated. */
int vulkan_renderer_composite_rgba8(const void *source_resource,
                                    uint64_t source_serial,
                                    const uint8_t *source,
                                    const void *overlay_resource,
                                    uint64_t overlay_serial,
                                    const uint8_t *overlay,
                                    const void *target_resource,
                                    uint64_t target_serial,
                                    uint8_t *target,
                                    uint32_t width,
                                    uint32_t height,
                                    uint32_t mode);
int vulkan_renderer_sync_target(const void *resource, uint64_t serial,
                                uint8_t *pixels, size_t bytes);
/* Copies a completed compositor target into host-visible storage for genuine
 * CPU consumers. Presentation should prefer the shared Vulkan buffer below. */
int vulkan_renderer_copy_target(const void *resource, uint64_t serial,
                                uint8_t *destination, size_t bytes);
/* Returns the shared-device Vulkan buffer containing a compositor result. */
int vulkan_renderer_get_target_buffer(const void *resource, uint64_t serial,
                                      size_t bytes, uint64_t *buffer_handle);
void vulkan_renderer_forget_resource(const void *resource);
void vulkan_renderer_destroy(void);

#endif
