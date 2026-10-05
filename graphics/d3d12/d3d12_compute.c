#include "d3d12_compute.h"

#include <stdint.h>
#include <string.h>

#define RE8_FAST_COPY_SHADER_HASH UINT64_C(0x31b662b443572be7)
#define RE8_FAST_COPY_DWORDS_PER_GROUP 512u

int beer_d3d12_execute_known_compute(
    const BeerD3D12ComputeDispatch *dispatch)
{
    if (!dispatch || dispatch->shader_hash != RE8_FAST_COPY_SHADER_HASH ||
        dispatch->group_count_y != 1 || dispatch->group_count_z != 1 ||
        dispatch->bindings[0].kind != 1 ||
        dispatch->bindings[1].kind != 1)
        return 0;

    const BeerD3D12ComputeBinding *source = &dispatch->bindings[0];
    const BeerD3D12ComputeBinding *destination = &dispatch->bindings[1];
    if (!source->resource_storage || !destination->resource_storage)
        return 0;
    size_t byte_count = (size_t)dispatch->group_count_x *
        RE8_FAST_COPY_DWORDS_PER_GROUP * sizeof(uint32_t);
    if (byte_count > source->resource_storage_size ||
        byte_count > destination->resource_storage_size)
        return 0;

    /* The captured SM5 shader has local size (256,1,1). Each invocation copies
     * one uint at group*512+lane and one at group*512+256+lane from t0 to u0.
     * This is exactly one contiguous 512-dword copy per workgroup. memmove also
     * preserves deterministic behavior if a guest aliases the two resources. */
    memmove(destination->resource_storage, source->resource_storage, byte_count);
    return 1;
}
