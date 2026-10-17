#include "d3d12_graphics.h"
#include "d3d12_shader_translator.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    /* A translated shader may expose 64 descriptors. Root-table snapshots must
     * never silently retain fewer entries, or high material SRVs disappear at
     * replay even though shader translation succeeds. */
    if (BEER_D3D12_MAX_RESOLVED_DESCRIPTORS <
        BEER_D3D12_MAX_SHADER_DESCRIPTORS)
        return 40;

    /* Same-register descriptor tables must resolve by exact shader/view shape,
     * not merely by the fact that both resources are textures. */
    if (beer_d3d12_graphics_map_primitive_topology(0) != UINT32_MAX ||
        beer_d3d12_graphics_map_primitive_topology(1) != 0 ||
        beer_d3d12_graphics_map_primitive_topology(2) != 1 ||
        beer_d3d12_graphics_map_primitive_topology(3) != 2 ||
        beer_d3d12_graphics_map_primitive_topology(4) != 3 ||
        beer_d3d12_graphics_map_primitive_topology(5) != 4 ||
        beer_d3d12_graphics_map_primitive_topology(6) != UINT32_MAX ||
        beer_d3d12_graphics_map_primitive_topology(33) != UINT32_MAX)
        return 38;

    if (!beer_d3d12_graphics_descriptor_shape_matches(3, 4, 3, 1, 1) ||
        beer_d3d12_graphics_descriptor_shape_matches(5, 4, 3, 1, 1) ||
        !beer_d3d12_graphics_descriptor_shape_matches(5, 8, 4, 32, 1) ||
        beer_d3d12_graphics_descriptor_shape_matches(3, 8, 4, 32, 1) ||
        !beer_d3d12_graphics_descriptor_shape_matches(7, 2, 2, 1, 1) ||
        beer_d3d12_graphics_descriptor_shape_matches(7, 2, 2, 2, 1) ||
        !beer_d3d12_graphics_descriptor_shape_matches(8, 5, 3, 4, 1) ||
        beer_d3d12_graphics_resolve_layer_count(2, UINT32_MAX, 6) != 4 ||
        beer_d3d12_graphics_resolve_layer_count(2, 3, 6) != 3 ||
        beer_d3d12_graphics_resolve_layer_count(2, 5, 6) != 0 ||
        beer_d3d12_graphics_resolve_layer_count(6, UINT32_MAX, 6) != 0 ||
        !beer_d3d12_graphics_descriptor_shape_matches(6, 9, 3, 6, 1) ||
        beer_d3d12_graphics_descriptor_shape_matches(6, 4, 3, 6, 1) ||
        !beer_d3d12_graphics_descriptor_shape_matches(4, 6, 3, 1, 4) ||
        beer_d3d12_graphics_descriptor_shape_matches(4, 6, 3, 1, 1))
        return 39;

    puts("D3D12 graphics descriptor-shape tests passed");
    return 0;
}
