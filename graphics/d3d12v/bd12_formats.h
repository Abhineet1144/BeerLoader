#ifndef BD12_FORMATS_H
#define BD12_FORMATS_H

#include "bd12.h"

/* DXGI_FORMAT -> VkFormat for typed views, vertex attributes and render
 * targets. Typeless formats map to a representative member of their family.
 * Returns VK_FORMAT_UNDEFINED for formats with no Vulkan equivalent. */
VkFormat bd12_dxgi_to_vk(uint32_t dxgi);
/* Format of the VkImage backing a resource: typeless families pick a member
 * and depth-stencil-capable typeless formats pick the matching depth format. */
VkFormat bd12_dxgi_resource_to_vk(uint32_t dxgi, int allow_depth_stencil);
/* Bytes per texel (or per block for compressed formats); 0 if unknown. */
uint32_t bd12_dxgi_bytes(uint32_t dxgi);
int bd12_dxgi_is_compressed(uint32_t dxgi);
/* 1 when the SRV format reads the stencil plane of a depth-stencil image. */
int bd12_dxgi_reads_stencil(uint32_t dxgi);

#endif
