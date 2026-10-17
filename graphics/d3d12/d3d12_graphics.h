#ifndef BEER_D3D12_GRAPHICS_H
#define BEER_D3D12_GRAPHICS_H

#include "d3d12_compat.h"

/* Returns whether a resolved D3D12 view has the exact dimensionality required
 * by a reflected shader resource. This prevents same-register texture tables
 * from resolving a Texture3D declaration to an unrelated Texture2D view. */
uint32_t beer_d3d12_graphics_resolve_layer_count(
    uint32_t first_layer, uint32_t declared_count, uint32_t resource_layers);

int beer_d3d12_graphics_descriptor_shape_matches(
    uint32_t shader_resource_type, uint32_t view_dimension,
    uint32_t resource_dimension, uint32_t resource_layers,
    uint32_t resource_samples);

/* Maps the non-patch D3D primitive topology values to VkPrimitiveTopology.
 * Returns UINT32_MAX for undefined and patch-list topologies, which require a
 * tessellation pipeline rather than an input-assembly-only mapping. */
uint32_t beer_d3d12_graphics_map_primitive_topology(uint32_t topology);

#endif
