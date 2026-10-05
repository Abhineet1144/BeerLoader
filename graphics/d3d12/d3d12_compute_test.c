#include "d3d12_compute.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    uint32_t source[1024];
    uint32_t destination[1024];
    for (uint32_t i = 0; i < 1024; ++i) source[i] = i ^ 0xa5a55a5a;
    memset(destination, 0, sizeof(destination));

    BeerD3D12ComputeDispatch dispatch = {0};
    dispatch.shader_hash = UINT64_C(0x31b662b443572be7);
    dispatch.group_count_x = 2;
    dispatch.group_count_y = 1;
    dispatch.group_count_z = 1;
    dispatch.bindings[0].kind = 1;
    dispatch.bindings[0].resource_storage = (uint8_t *)source;
    dispatch.bindings[0].resource_storage_size = sizeof(source);
    dispatch.bindings[1].kind = 1;
    dispatch.bindings[1].resource_storage = (uint8_t *)destination;
    dispatch.bindings[1].resource_storage_size = sizeof(destination);

    if (!beer_d3d12_execute_known_compute(&dispatch) ||
        memcmp(source, destination, sizeof(source)) != 0)
        return 1;

    dispatch.shader_hash++;
    memset(destination, 0, sizeof(destination));
    if (beer_d3d12_execute_known_compute(&dispatch) || destination[0] != 0)
        return 2;

    dispatch.shader_hash--;
    dispatch.bindings[1].resource_storage_size--;
    if (beer_d3d12_execute_known_compute(&dispatch))
        return 3;

    puts("D3D12 known compute tests passed");
    return 0;
}
