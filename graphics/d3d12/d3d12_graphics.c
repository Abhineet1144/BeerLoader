#include "d3d12_graphics.h"

#include <stdint.h>

uint32_t beer_d3d12_graphics_resolve_layer_count(
    uint32_t first_layer, uint32_t declared_count, uint32_t resource_layers)
{
    if (first_layer >= resource_layers) return 0;
    if (declared_count == UINT32_MAX)
        return resource_layers - first_layer;
    if (!declared_count || declared_count > resource_layers - first_layer)
        return 0;
    return declared_count;
}

uint32_t beer_d3d12_graphics_map_primitive_topology(uint32_t topology)
{
    /* D3D_PRIMITIVE_TOPOLOGY_POINTLIST through TRIANGLESTRIP are contiguous
     * and have the same ordering as VkPrimitiveTopology. Adjacency and patch
     * lists need geometry/tessellation stages that Beer does not expose yet. */
    return topology >= 1 && topology <= 5 ? topology - 1u : UINT32_MAX;
}

int beer_d3d12_graphics_descriptor_shape_matches(
    uint32_t shader_resource_type, uint32_t view_dimension,
    uint32_t resource_dimension, uint32_t resource_layers,
    uint32_t resource_samples)
{
    if (!resource_layers) resource_layers = 1;
    if (!resource_samples) resource_samples = 1;
    switch (shader_resource_type) {
    case 1: /* BUFFER */
        return resource_dimension == 1 && view_dimension == 1;
    case 2: /* TEXTURE_1D */
        return resource_dimension == 2 && view_dimension == 2 &&
            resource_samples == 1;
    case 3: /* TEXTURE_2D */
        return resource_dimension == 3 && view_dimension == 4 &&
            resource_samples == 1;
    case 4: /* TEXTURE_2D_MS */
        return resource_dimension == 3 && view_dimension == 6 &&
            resource_samples > 1;
    case 5: /* TEXTURE_3D */
        return resource_dimension == 4 && view_dimension == 8 &&
            resource_samples == 1;
    case 6: /* TEXTURE_CUBE */
        return resource_dimension == 3 && view_dimension == 9 &&
            resource_layers >= 6 && resource_samples == 1;
    case 7: /* TEXTURE_1D_ARRAY */
        return resource_dimension == 2 && resource_samples == 1 &&
            (view_dimension == 3 ||
             (view_dimension == 2 && resource_layers == 1));
    case 8: /* TEXTURE_2D_ARRAY */
        return resource_dimension == 3 && resource_samples == 1 &&
            (view_dimension == 5 ||
             (view_dimension == 4 && resource_layers == 1));
    case 9: /* TEXTURE_2D_MS_ARRAY */
        return resource_dimension == 3 && view_dimension == 7 &&
            resource_samples > 1;
    case 10: /* TEXTURE_CUBE_ARRAY */
        return resource_dimension == 3 && view_dimension == 10 &&
            resource_layers >= 6 && resource_samples == 1;
    default:
        return 0;
    }
}

