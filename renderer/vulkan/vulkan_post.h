#ifndef BEER_VULKAN_POST_H
#define BEER_VULKAN_POST_H

#include <stdint.h>
#include <vulkan/vulkan_core.h>

/* Final-image compute passes run between the guest-frame copy and the blit to the swapchain: effects
 * (platform/beer_post.c parameters), overlay blending with real transparency, and screenshots.
 *
 * Call right after the frame was copied into `upload` (a R8G8B8A8_UNORM image with STORAGE usage, layout
 * TRANSFER_DST_OPTIMAL). Returns the image to blit instead (left in TRANSFER_SRC_OPTIMAL) with `upload`
 * handed back in TRANSFER_DST_OPTIMAL, or VK_NULL_HANDLE when nothing needs doing (then nothing was touched).
 * *overlay_done is set when the overlay boxes were already blended into the returned image. */
VkImage vulkan_post_apply(VkCommandBuffer cmd, VkImage upload, uint32_t width, uint32_t height,
                          uint32_t output_width, uint32_t output_height, int *overlay_done);

/* Runs the passes on a device the guest created (the Vulkan mapper) instead of the shared one.  Call before the first
 * vulkan_post_apply; returns 0 when post-processing is already bound to another device. */
int vulkan_post_bind_device(VkInstance instance, VkPhysicalDevice physical, VkDevice device,
                            PFN_vkGetInstanceProcAddr get_instance, PFN_vkGetDeviceProcAddr get_device);

/* Nonzero while a recorded screenshot has not been collected yet. */
int vulkan_post_screenshot_waiting(void);

/* Finishes a screenshot recorded by an earlier frame once its GPU work is complete (call after the
 * presenter's fence wait). */
void vulkan_post_collect(void);

/* The presenter destroyed (or is about to recreate) its upload image: drop cached views of it. */
void vulkan_post_forget_upload(void);

#endif
