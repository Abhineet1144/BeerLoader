#ifndef BEER_VULKAN_PRESENTER_H
#define BEER_VULKAN_PRESENTER_H

#include <stdint.h>

/* Presents an RGBA8 frame through a Vulkan swapchain attached to the existing
 * XWayland Xlib window. Pixel upload is host-visible, while format conversion
 * and output scaling are performed by a Vulkan image blit. Returns 1 on
 * success and 0 when Vulkan is unavailable. */
int vulkan_presenter_present_rgba8(void *display, unsigned long window,
                                   const uint8_t *pixels,
                                   int width, int height, int row_pitch,
                                   int output_width, int output_height);
/* Presents a compositor-owned GPU mirror directly into Vulkan staging. The
 * guest CPU resource is used only when no matching mirror exists. */
int vulkan_presenter_present_resource_rgba8(void *display, unsigned long window,
                                            const void *resource,
                                            uint64_t serial,
                                            const uint8_t *pixels,
                                            int width, int height, int row_pitch,
                                            int output_width, int output_height);
void vulkan_presenter_destroy(void);
int vulkan_presenter_is_active(void);

#endif
