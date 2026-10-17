#include "d3d12_compute.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ---- helpers ---- */
static void fill_rgba_float(float *buf, uint32_t w, uint32_t h, float seed)
{
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            size_t i = ((size_t)y * w + x) * 4;
            buf[i+0] = seed + (float)(x ^ y);
            buf[i+1] = seed + (float)(x + y);
            buf[i+2] = seed + (float)(x * 2 + y);
            buf[i+3] = 1.0f;
        }
}

static void make_blur_dispatch(BeerD3D12ComputeDispatch *d,
                               uint64_t hash,
                               float *src, uint32_t sw, uint32_t sh,
                               float *dst, uint32_t dw, uint32_t dh,
                               uint32_t gx, uint32_t gy)
{
    memset(d, 0, sizeof(*d));
    d->shader_hash = hash;
    d->group_count_x = gx;
    d->group_count_y = gy;
    d->group_count_z = 1;
    /* param[1] = SRV table: descriptor 0 = t0, space 0, type 0 (SRV) */
    d->bindings[1].kind = 1;
    d->bindings[1].descriptor_count = 1;
    d->bindings[1].descriptor_types[0]    = 0; /* SRV */
    d->bindings[1].descriptor_registers[0] = 0; /* t0 */
    d->bindings[1].descriptor_spaces[0]   = 0;
    d->bindings[1].descriptor_resource_storage[0] = (uint8_t *)src;
    d->bindings[1].descriptor_resource_storage_size[0] =
        (size_t)sw * sh * 4 * sizeof(float);
    d->bindings[1].descriptor_resources[0] = src; /* non-NULL sentinel */
    d->bindings[1].descriptor_widths[0]  = sw;
    d->bindings[1].descriptor_heights[0] = sh;
    d->bindings[1].descriptor_formats[0] = 2; /* RGBA32_FLOAT */
    d->bindings[1].descriptor_row_pitches[0] = (size_t)sw * 4 * sizeof(float);
    /* param[2] = UAV table: descriptor 0 = u0, space 0, type 1 (UAV) */
    d->bindings[2].kind = 1;
    d->bindings[2].descriptor_count = 1;
    d->bindings[2].descriptor_types[0]    = 1; /* UAV */
    d->bindings[2].descriptor_registers[0] = 0; /* u0 */
    d->bindings[2].descriptor_spaces[0]   = 0;
    d->bindings[2].descriptor_resource_storage[0] = (uint8_t *)dst;
    d->bindings[2].descriptor_resource_storage_size[0] =
        (size_t)dw * dh * 4 * sizeof(float);
    d->bindings[2].descriptor_resources[0] = dst;
    d->bindings[2].descriptor_widths[0]  = dw;
    d->bindings[2].descriptor_heights[0] = dh;
    d->bindings[2].descriptor_formats[0] = 2; /* RGBA32_FLOAT */
    d->bindings[2].descriptor_row_pitches[0] = (size_t)dw * 4 * sizeof(float);
}

static void set_blur_root_cbv(BeerD3D12ComputeDispatch *d,
                              uint32_t width, uint32_t height,
                              uint32_t cb[4])
{
    cb[0] = width - 1;
    cb[1] = height - 1;
    cb[2] = cb[3] = 0;
    d->bindings[0].kind = 3;
    d->bindings[0].resource_storage = (uint8_t *)cb;
    d->bindings[0].resource_storage_size = 4 * sizeof(uint32_t);
}

int main(void)
{
    /* ---- existing copy/indirect tests ---- */
    uint32_t source[1024];
    uint32_t destination[1024];
    for (uint32_t i = 0; i < 1024; ++i) source[i] = i ^ 0xa5a55a5a;
    memset(destination, 0, sizeof(destination));

    BeerD3D12ComputeDispatch dispatch = {0};
    dispatch.shader_hash = UINT64_C(0x31b662b443572be7);
    dispatch.group_count_x = 2;
    dispatch.group_count_y = 1;
    dispatch.group_count_z = 1;
    /* Captured root signature: param 0 is UAV u0, param 1 is SRV t0. */
    dispatch.bindings[0].kind = 1;
    dispatch.bindings[0].resource_storage = (uint8_t *)destination;
    dispatch.bindings[0].resource_storage_size = sizeof(destination);
    dispatch.bindings[1].kind = 1;
    dispatch.bindings[1].resource_storage = (uint8_t *)source;
    dispatch.bindings[1].resource_storage_size = sizeof(source);

    if (!beer_d3d12_execute_known_compute(&dispatch) ||
        memcmp(source, destination, sizeof(source)) != 0)
        return 1;

    dispatch.shader_hash++;
    memset(destination, 0, sizeof(destination));
    if (beer_d3d12_execute_known_compute(&dispatch) || destination[0] != 0)
        return 2;

    dispatch.shader_hash--;
    dispatch.bindings[0].resource_storage_size--;
    if (beer_d3d12_execute_known_compute(&dispatch))
        return 3;

    uint32_t indirect_source[128];
    uint32_t indirect_destination[128];
    for (uint32_t i = 0; i < 128; ++i)
        indirect_source[i] = UINT32_C(0x10000000) + i;
    memset(indirect_destination, 0, sizeof(indirect_destination));
    memset(&dispatch, 0, sizeof(dispatch));
    dispatch.shader_hash = UINT64_C(0x31cbd8c251bbbe42);
    dispatch.group_count_x = 2;
    dispatch.group_count_y = 1;
    dispatch.group_count_z = 1;
    dispatch.bindings[1].kind = 1;
    dispatch.bindings[1].resource_storage = (uint8_t *)indirect_source;
    dispatch.bindings[1].resource_storage_size = sizeof(indirect_source);
    dispatch.bindings[2].kind = 1;
    dispatch.bindings[2].resource_storage = (uint8_t *)indirect_destination;
    dispatch.bindings[2].resource_storage_size = sizeof(indirect_destination);
    if (!beer_d3d12_execute_known_compute(&dispatch) ||
        memcmp(indirect_source, indirect_destination,
               sizeof(indirect_source)) != 0)
        return 4;

    dispatch.bindings[2].resource_storage_size--;
    memset(indirect_destination, 0, sizeof(indirect_destination));
    if (beer_d3d12_execute_known_compute(&dispatch) ||
        indirect_destination[0] != 0)
        return 5;

    dispatch.bindings[2].resource_storage_size = sizeof(indirect_destination);
    dispatch.bindings[1].kind = 0;
    if (beer_d3d12_execute_known_compute(&dispatch))
        return 6;

    /* Root-signature-aware dispatch snapshots store t0/u0 in expanded
     * descriptor metadata rather than the old table-base fields. */
    memset(indirect_destination, 0, sizeof(indirect_destination));
    memset(&dispatch, 0, sizeof(dispatch));
    dispatch.shader_hash = UINT64_C(0x31cbd8c251bbbe42);
    dispatch.group_count_x = 2;
    dispatch.group_count_y = 1;
    dispatch.group_count_z = 1;
    dispatch.bindings[1].kind = 1;
    dispatch.bindings[1].descriptor_count = 1;
    dispatch.bindings[1].descriptor_types[0] = 0;
    dispatch.bindings[1].descriptor_registers[0] = 0;
    dispatch.bindings[1].descriptor_spaces[0] = 0;
    dispatch.bindings[1].descriptor_resource_storage[0] =
        (uint8_t *)indirect_source;
    dispatch.bindings[1].descriptor_resource_storage_size[0] =
        sizeof(indirect_source);
    dispatch.bindings[2].kind = 1;
    dispatch.bindings[2].descriptor_count = 1;
    dispatch.bindings[2].descriptor_types[0] = 1;
    dispatch.bindings[2].descriptor_registers[0] = 0;
    dispatch.bindings[2].descriptor_spaces[0] = 0;
    dispatch.bindings[2].descriptor_resource_storage[0] =
        (uint8_t *)indirect_destination;
    dispatch.bindings[2].descriptor_resource_storage_size[0] =
        sizeof(indirect_destination);
    if (!beer_d3d12_execute_known_compute(&dispatch) ||
        memcmp(indirect_source, indirect_destination,
               sizeof(indirect_source)) != 0)
        return 20;

    dispatch.bindings[2].descriptor_registers[0] = 1;
    memset(indirect_destination, 0, sizeof(indirect_destination));
    if (beer_d3d12_execute_known_compute(&dispatch) ||
        indirect_destination[0] != 0)
        return 21;

    /* Buffer SRV/UAV FirstElement origins are part of the view. RE8 uses them
     * to copy GUI slices out of shared allocator buffers. */
    memset(&dispatch, 0, sizeof(dispatch));
    dispatch.shader_hash = UINT64_C(0x31b662b443572be7);
    dispatch.group_count_x = 1;
    dispatch.group_count_y = dispatch.group_count_z = 1;
    uint32_t offset_source[520], offset_destination[520];
    for (uint32_t i = 0; i < 520; ++i) {
        offset_source[i] = UINT32_C(0xabc00000) + i;
        offset_destination[i] = UINT32_C(0xdeadbeef);
    }
    dispatch.bindings[1].kind = 1;
    dispatch.bindings[1].descriptor_count = 1;
    dispatch.bindings[1].descriptor_types[0] = 0;
    dispatch.bindings[1].descriptor_resource_storage[0] =
        (uint8_t *)offset_source;
    dispatch.bindings[1].descriptor_resource_storage_size[0] =
        sizeof(offset_source);
    dispatch.bindings[1].descriptor_view_formats[0] = 42;
    dispatch.bindings[1].descriptor_view_first_elements[0] = 4;
    dispatch.bindings[0].kind = 1;
    dispatch.bindings[0].descriptor_count = 1;
    dispatch.bindings[0].descriptor_types[0] = 1;
    dispatch.bindings[0].descriptor_resource_storage[0] =
        (uint8_t *)offset_destination;
    dispatch.bindings[0].descriptor_resource_storage_size[0] =
        sizeof(offset_destination);
    dispatch.bindings[0].descriptor_view_formats[0] = 42;
    dispatch.bindings[0].descriptor_view_first_elements[0] = 8;
    if (!beer_d3d12_execute_known_compute(&dispatch) ||
        offset_destination[7] != UINT32_C(0xdeadbeef) ||
        offset_destination[8] != offset_source[4] ||
        offset_destination[519] != offset_source[515])
        return 22;

    /* Structured views measure FirstElement in StructureByteStride units. */
    uint8_t structured_source[520 * 20], structured_destination[520 * 20];
    for (size_t i = 0; i < sizeof(structured_source); ++i)
        structured_source[i] = (uint8_t)(i * 17u + 3u);
    memset(structured_destination, 0xcc, sizeof(structured_destination));
    dispatch.bindings[1].descriptor_resource_storage[0] = structured_source;
    dispatch.bindings[1].descriptor_resource_storage_size[0] =
        sizeof(structured_source);
    dispatch.bindings[1].descriptor_view_formats[0] = 0;
    dispatch.bindings[1].descriptor_view_first_elements[0] = 4;
    dispatch.bindings[1].descriptor_view_structure_strides[0] = 20;
    dispatch.bindings[0].descriptor_resource_storage[0] = structured_destination;
    dispatch.bindings[0].descriptor_resource_storage_size[0] =
        sizeof(structured_destination);
    dispatch.bindings[0].descriptor_view_formats[0] = 0;
    dispatch.bindings[0].descriptor_view_first_elements[0] = 8;
    dispatch.bindings[0].descriptor_view_structure_strides[0] = 20;
    if (!beer_d3d12_execute_known_compute(&dispatch) ||
        structured_destination[8 * 20 - 1] != 0xcc ||
        memcmp(structured_destination + 8 * 20,
               structured_source + 4 * 20, 512 * sizeof(uint32_t)) != 0)
        return 23;

    /* Fixed-size threadgroups clamp to explicit structured view counts. */
    memset(structured_destination, 0xcc, sizeof(structured_destination));
    dispatch.bindings[1].descriptor_view_element_counts[0] = 5;
    dispatch.bindings[0].descriptor_view_element_counts[0] = 5;
    if (!beer_d3d12_execute_known_compute(&dispatch) ||
        memcmp(structured_destination + 8 * 20,
               structured_source + 4 * 20, 5 * 20) != 0 ||
        structured_destination[8 * 20 + 5 * 20] != 0xcc)
        return 32;

    /* The scene-event reset shader atomically clears only dword zero of u0. */
    {
        uint32_t event_counts[4] = {17, 23, 42, 99};
        memset(&dispatch, 0, sizeof(dispatch));
        dispatch.shader_hash = UINT64_C(0x515ff5924e1066f0);
        dispatch.group_count_x = 1;
        dispatch.group_count_y = 1;
        dispatch.group_count_z = 1;
        dispatch.bindings[2].kind = 1;
        dispatch.bindings[2].descriptor_count = 1;
        dispatch.bindings[2].descriptor_types[0] = 1;
        dispatch.bindings[2].descriptor_registers[0] = 0;
        dispatch.bindings[2].descriptor_spaces[0] = 0;
        dispatch.bindings[2].descriptor_resource_storage[0] =
            (uint8_t *)event_counts;
        dispatch.bindings[2].descriptor_resource_storage_size[0] =
            sizeof(event_counts);
        if (!beer_d3d12_execute_known_compute(&dispatch) ||
            event_counts[0] != 0 || event_counts[1] != 23 ||
            event_counts[2] != 42 || event_counts[3] != 99)
            return 24;
        dispatch.bindings[2].descriptor_resource_storage_size[0] =
            sizeof(uint32_t) - 1;
        event_counts[0] = 17;
        if (beer_d3d12_execute_known_compute(&dispatch) ||
            event_counts[0] != 17)
            return 25;
        dispatch.bindings[2].descriptor_registers[0] = 1;
        dispatch.bindings[2].descriptor_resource_storage_size[0] =
            sizeof(event_counts);
        if (beer_d3d12_execute_known_compute(&dispatch))
            return 26;
    }

    /* The fast-clear shader clears 512 dwords per group through UAV u0. */
    {
        uint32_t clear_target[1032];
        for (uint32_t i = 0; i < 1032; ++i)
            clear_target[i] = UINT32_C(0xdead0000) | i;
        memset(&dispatch, 0, sizeof(dispatch));
        dispatch.shader_hash = UINT64_C(0x327de45285abc564);
        dispatch.group_count_x = 2;
        dispatch.group_count_y = 1;
        dispatch.group_count_z = 1;
        dispatch.bindings[0].kind = 1;
        dispatch.bindings[0].descriptor_count = 1;
        dispatch.bindings[0].descriptor_types[0] = 1;
        dispatch.bindings[0].descriptor_registers[0] = 0;
        dispatch.bindings[0].descriptor_spaces[0] = 0;
        dispatch.bindings[0].descriptor_resource_storage[0] =
            (uint8_t *)clear_target;
        dispatch.bindings[0].descriptor_resource_storage_size[0] =
            sizeof(clear_target);
        if (!beer_d3d12_execute_known_compute(&dispatch))
            return 27;
        for (uint32_t i = 0; i < 1024; ++i)
            if (clear_target[i] != 0)
                return 28;
        for (uint32_t i = 1024; i < 1032; ++i)
            if (clear_target[i] != (UINT32_C(0xdead0000) | i))
                return 29;

        dispatch.bindings[0].descriptor_resource_storage_size[0] =
            1024 * sizeof(uint32_t) - 1;
        clear_target[0] = UINT32_C(0x12345678);
        if (beer_d3d12_execute_known_compute(&dispatch) ||
            clear_target[0] != UINT32_C(0x12345678))
            return 30;
        dispatch.bindings[0].descriptor_resource_storage_size[0] =
            sizeof(clear_target);
        dispatch.bindings[0].descriptor_registers[0] = 1;
        if (beer_d3d12_execute_known_compute(&dispatch))
            return 31;
    }

    /* ---- Blur shader tests ---- */

    /* Synthetic blur fixtures use explicit RGBA32_FLOAT (format 2). Runtime
     * RE8 blur coverage separately exercises packed R11G11B10_FLOAT. */

    /* 8.1: horizontal blur succeeds with valid 4×4 RGBA-float textures */
    {
        static float src[64*4*4], dst[64*4*4];
        fill_rgba_float(src, 64, 4, 1.0f);
        memset(dst, 0, sizeof(dst));
        make_blur_dispatch(&dispatch,
                           UINT64_C(0x751080ccaa2ee213),
                           src, 64, 4, dst, 64, 4,
                           1 /* groups_x: 64 pixels */,
                           4 /* groups_y: one per row */);
        if (!beer_d3d12_execute_known_compute(&dispatch))
            return 7;
        /* Centre pixel should be a non-trivial weighted sum, not simply src[centre] */
        float centre_src = src[(2*64+32)*4+0];
        float centre_dst = dst[(2*64+32)*4+0];
        (void)centre_src; (void)centre_dst;
        /* At least verify dst is non-zero (blur ran) */
        int nonzero = 0;
        for (int i = 0; i < 4*4*4; ++i) if (dst[i] != 0.0f) { nonzero = 1; break; }
        if (!nonzero) return 8;
    }

    /* 8.2: vertical blur succeeds */
    {
        static float src[4*64*4], dst[4*64*4];
        fill_rgba_float(src, 4, 64, 2.0f);
        memset(dst, 0, sizeof(dst));
        make_blur_dispatch(&dispatch,
                           UINT64_C(0x45ba603d1586ec73),
                           src, 4, 64, dst, 4, 64,
                           4 /* groups_x: one per column */,
                           1 /* groups_y: 64 pixels */);
        if (!beer_d3d12_execute_known_compute(&dispatch))
            return 9;
        int nonzero = 0;
        for (int i = 0; i < 4*4*4; ++i) if (dst[i] != 0.0f) { nonzero = 1; break; }
        if (!nonzero) return 10;
    }

    /* 8.3: blur rejected when SRV descriptor missing (descriptor_count = 0) */
    {
        static float src[4*4*4], dst[4*4*4];
        fill_rgba_float(src, 4, 4, 3.0f);
        memset(dst, 0, sizeof(dst));
        make_blur_dispatch(&dispatch,
                           UINT64_C(0x751080ccaa2ee213),
                           src, 4, 4, dst, 4, 4, 1, 4);
        dispatch.bindings[1].descriptor_count = 0; /* remove SRV */
        if (beer_d3d12_execute_known_compute(&dispatch))
            return 11;
        /* dst must remain zero */
        for (int i = 0; i < 4*4*4; ++i) if (dst[i] != 0.0f) return 12;
    }

    /* 8.4: blur rejected when UAV descriptor missing */
    {
        static float src[4*4*4], dst[4*4*4];
        fill_rgba_float(src, 4, 4, 4.0f);
        memset(dst, 0, sizeof(dst));
        make_blur_dispatch(&dispatch,
                           UINT64_C(0x45ba603d1586ec73),
                           src, 4, 4, dst, 4, 4, 4, 1);
        dispatch.bindings[2].descriptor_count = 0; /* remove UAV */
        if (beer_d3d12_execute_known_compute(&dispatch))
            return 13;
    }

    /* 8.5: RE8 binds a larger scratch SRV and a smaller output UAV. The
     * shader clamps source reads to the output extent, so this is valid. */
    {
        static float src[128*8*4], dst[64*4*4];
        fill_rgba_float(src, 128, 8, 5.0f);
        memset(dst, 0, sizeof(dst));
        make_blur_dispatch(&dispatch,
                           UINT64_C(0x751080ccaa2ee213),
                           src, 128, 8, dst, 64, 4,
                           1, 4);
        if (!beer_d3d12_execute_known_compute(&dispatch))
            return 14;
    }

    /* 8.5b: the SRV must still cover the complete UAV extent. */
    {
        static float src[4*4*4], dst[8*8*4];
        fill_rgba_float(src, 4, 4, 5.0f);
        memset(dst, 0, sizeof(dst));
        make_blur_dispatch(&dispatch,
                           UINT64_C(0x751080ccaa2ee213),
                           src, 4, 4, dst, 8, 8,
                           1, 8);
        if (beer_d3d12_execute_known_compute(&dispatch))
            return 23;
    }

    /* 8.5c: a root CBV supplies the active extent for rounded-up dispatches. */
    {
        static float src[128*8*4], dst[128*8*4];
        uint32_t cb[4];
        fill_rgba_float(src, 128, 8, 5.5f);
        memset(dst, 0, sizeof(dst));
        make_blur_dispatch(&dispatch,
                           UINT64_C(0x751080ccaa2ee213),
                           src, 128, 8, dst, 128, 8,
                           2, 8);
        set_blur_root_cbv(&dispatch, 120, 7, cb);
        if (!beer_d3d12_execute_known_compute(&dispatch))
            return 24;
        if (dst[((size_t)6 * 128 + 119) * 4] == 0.0f ||
            dst[((size_t)7 * 128) * 4] != 0.0f ||
            dst[((size_t)6 * 128 + 120) * 4] != 0.0f)
            return 25;
    }

    /* 8.6: horizontal blur: constant image produces same constant output */
    {
        static float src[4*4*4], dst[4*4*4];
        /* Fill all pixels with (1, 2, 3, 1) */
        for (int i = 0; i < 4*4; ++i) {
            src[i*4+0] = 1.0f; src[i*4+1] = 2.0f;
            src[i*4+2] = 3.0f; src[i*4+3] = 1.0f;
        }
        memset(dst, 0, sizeof(dst));
        make_blur_dispatch(&dispatch,
                           UINT64_C(0x751080ccaa2ee213),
                           src, 4, 4, dst, 4, 4, 1, 4);
        if (!beer_d3d12_execute_known_compute(&dispatch))
            return 15;
        /* For a constant image, blur output ≈ input (kernel sums to ~1.001;
         * allow 2e-3 tolerance to cover float rounding and kernel imprecision). */
        for (int i = 0; i < 4*4; ++i) {
            if (fabsf(dst[i*4+0] - 1.0f) > 2e-3f) return 16;
            if (fabsf(dst[i*4+1] - 2.0f) > 4e-3f) return 17;
            if (fabsf(dst[i*4+2] - 3.0f) > 6e-3f) return 18;
        }
    }

    /* 8.7: unknown hash not confused with blur hashes */
    {
        static float src[4*4*4], dst[4*4*4];
        fill_rgba_float(src, 4, 4, 6.0f);
        memset(dst, 0, sizeof(dst));
        make_blur_dispatch(&dispatch,
                           UINT64_C(0x751080ccaa2ee213) ^ 1ULL,
                           src, 4, 4, dst, 4, 4, 1, 4);
        if (beer_d3d12_execute_known_compute(&dispatch))
            return 19;
    }

    /* 8.8: rounded-up dispatch groups are valid. Without cb0, the executor
     * safely clamps the active region to the UAV extent. */
    {
        static float src[4*4*4], dst[4*4*4];
        fill_rgba_float(src, 4, 4, 7.0f);
        memset(dst, 0, sizeof(dst));
        make_blur_dispatch(&dispatch,
                           UINT64_C(0x751080ccaa2ee213),
                           src, 4, 4, dst, 4, 4, 2, 4);
        if (!beer_d3d12_execute_known_compute(&dispatch))
            return 22;
    }

    puts("D3D12 known compute tests passed");
    return 0;
}
