#include "d3d12_compute.h"

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RE8_FAST_COPY_SHADER_HASH UINT64_C(0x31b662b443572be7)
#define RE8_FAST_COPY_DWORDS_PER_GROUP 512u
#define RE8_INDIRECT_COPY_SHADER_HASH UINT64_C(0x31cbd8c251bbbe42)
#define RE8_INDIRECT_COPY_DWORDS_PER_GROUP 64u
#define RE8_SCENE_EVENT_RESET_SHADER_HASH UINT64_C(0x515ff5924e1066f0)
#define RE8_FAST_CLEAR_SHADER_HASH UINT64_C(0x327de45285abc564)
#define RE8_FAST_CLEAR_DWORDS_PER_GROUP 512u

/* Horizontal Gaussian blur: threadGroup(64,1,1).
 * dispatch(groups_x, height_px, 1) — groups_x * 64 == image width in pixels.
 * Reads DownsampledTexture t0 (SRV, root param 1, space 0, register 0).
 * Writes RWBlurredTexture  u0 (UAV, root param 2, space 0, register 0).
 * Kernel: [0.006, 0.061, 0.242, 0.383, 0.242, 0.061, 0.006] (sum ≈ 1.0).
 * The shader uses TGSM and clamped addressing; its CPU equivalent is a
 * simple 7-tap horizontal separable pass with border-clamp sampling. */
#define RE8_BLUR_H_SHADER_HASH UINT64_C(0x751080ccaa2ee213)

/* Vertical Gaussian blur: threadGroup(1,64,1).
 * dispatch(width_px, groups_y, 1) — groups_y * 64 == image height in pixels.
 * Same resources and kernel as the horizontal pass, transposed along Y. */
#define RE8_BLUR_V_SHADER_HASH UINT64_C(0x45ba603d1586ec73)

/* 7-tap Gaussian kernel embedded in the SHEX immediate constant buffer. */
static const float k_blur_kernel[7] = {
    0.006f, 0.061f, 0.242f, 0.383f, 0.242f, 0.061f, 0.006f
};

static uint8_t *copy_binding_storage(const BeerD3D12ComputeBinding *binding,
                                     uint32_t range_type, size_t *out_size)
{
    if (!binding || binding->kind != 1) return NULL;

    /* Retain compatibility with the original simple descriptor snapshot. */
    if (binding->resource_storage) {
        if (out_size) *out_size = binding->resource_storage_size;
        return binding->resource_storage;
    }

    /* Root-signature-aware snapshots expose the resource through the exact
     * t0/u0 descriptor instead of the legacy table-base fields. */
    for (uint32_t descriptor = 0;
         descriptor < binding->descriptor_count &&
         descriptor < BEER_D3D12_MAX_RESOLVED_DESCRIPTORS;
         ++descriptor) {
        if (binding->descriptor_types[descriptor] != range_type ||
            binding->descriptor_registers[descriptor] != 0 ||
            binding->descriptor_spaces[descriptor] != 0 ||
            !binding->descriptor_resource_storage[descriptor])
            continue;
        size_t offset = 0;
        uint32_t format = binding->descriptor_view_formats[descriptor];
        uint32_t stride =
            binding->descriptor_view_structure_strides[descriptor];
        uint64_t first =
            binding->descriptor_view_first_elements[descriptor];
        /* Buffer FirstElement is measured in structured elements when a
         * StructureByteStride is present, and in 32-bit units for RE8's
         * raw/typed allocator views. Applying only the latter selected allocator
         * headers instead of the GUI vertex/index slice. */
        size_t element_size = stride;
        if (!element_size &&
            (format == 39 || format == 41 || format == 42 || format == 43))
            element_size = sizeof(uint32_t);
        if (element_size) {
            if (first > SIZE_MAX / element_size) continue;
            offset = (size_t)first * element_size;
        }
        size_t size = binding->descriptor_resource_storage_size[descriptor];
        if (offset >= size) continue;
        size -= offset;
        uint32_t elements =
            binding->descriptor_view_element_counts[descriptor];
        if (elements && element_size &&
            elements <= SIZE_MAX / element_size &&
            (size_t)elements * element_size < size)
            size = (size_t)elements * element_size;
        if (out_size) *out_size = size;
        return binding->descriptor_resource_storage[descriptor] + offset;
    }
    return NULL;
}

static int execute_copy(const BeerD3D12ComputeDispatch *dispatch,
                        uint32_t source_parameter,
                        uint32_t destination_parameter,
                        uint32_t dwords_per_group)
{
    const BeerD3D12ComputeBinding *source =
        &dispatch->bindings[source_parameter];
    const BeerD3D12ComputeBinding *destination =
        &dispatch->bindings[destination_parameter];
    size_t source_size = 0, destination_size = 0;
    uint8_t *source_storage = copy_binding_storage(source, 0, &source_size);
    uint8_t *destination_storage =
        copy_binding_storage(destination, 1, &destination_size);
    int provenance = getenv("BEER_D3D12_GUI_COPY_PROVENANCE") != NULL;
    if (!source_storage || !destination_storage ||
        dispatch->group_count_x > SIZE_MAX / dwords_per_group /
            sizeof(uint32_t)) {
        if (provenance)
            fprintf(stderr,
                    "[D3D12 BUFFER COPY REJECT] shader=%016llx groups=%u "
                    "src-parameter=%u kind=%u descriptors=%u storage=%p/%zu "
                    "dst-parameter=%u kind=%u descriptors=%u storage=%p/%zu\n",
                    (unsigned long long)dispatch->shader_hash,
                    dispatch->group_count_x, source_parameter, source->kind,
                    source->descriptor_count, (void *)source_storage, source_size,
                    destination_parameter, destination->kind,
                    destination->descriptor_count, (void *)destination_storage,
                    destination_size);
        return 0;
    }

    size_t byte_count = (size_t)dispatch->group_count_x * dwords_per_group *
        sizeof(uint32_t);
    if (byte_count > source_size || byte_count > destination_size) {
        /* The fast-copy shader launches fixed 512-dword groups but guards each
         * load/store with the structured view's element count. Small GUI
         * slices therefore legitimately occupy only a few elements. Mirror
         * that bound when both descriptors carry explicit NumElements; do not
         * hide genuinely truncated legacy/raw storage. */
        uint32_t source_elements =
            source->descriptor_view_element_counts[0];
        uint32_t destination_elements =
            destination->descriptor_view_element_counts[0];
        if (source_elements && destination_elements) {
            if (source_size < byte_count) byte_count = source_size;
            if (destination_size < byte_count) byte_count = destination_size;
            byte_count &= ~(sizeof(uint32_t) - 1u);
        }
        if (!byte_count || byte_count > source_size ||
            byte_count > destination_size) {
            if (provenance)
                fprintf(stderr,
                        "[D3D12 BUFFER COPY REJECT] shader=%016llx groups=%u "
                        "bytes=%zu src-bytes=%zu dst-bytes=%zu\n",
                        (unsigned long long)dispatch->shader_hash,
                        dispatch->group_count_x, byte_count, source_size,
                        destination_size);
            return 0;
        }
    }

    static unsigned diagnostic_count;
    if (getenv("BEER_D3D12_GUI_COPY_PROVENANCE") &&
        diagnostic_count++ < 64) {
        fprintf(stderr,
                "[D3D12 BUFFER COPY] shader=%016llx groups=%u bytes=%zu "
                "src-resource=%p src-first=%llu src-stride=%u src-format=%u "
                "dst-resource=%p dst-first=%llu dst-stride=%u dst-format=%u\n",
                (unsigned long long)dispatch->shader_hash,
                dispatch->group_count_x, byte_count,
                source->descriptor_resources[0],
                (unsigned long long)source->descriptor_view_first_elements[0],
                source->descriptor_view_structure_strides[0],
                source->descriptor_view_formats[0],
                destination->descriptor_resources[0],
                (unsigned long long)destination->descriptor_view_first_elements[0],
                destination->descriptor_view_structure_strides[0],
                destination->descriptor_view_formats[0]);
    }
    memmove(destination_storage, source_storage, byte_count);
    return 1;
}

static int execute_fast_clear(const BeerD3D12ComputeDispatch *dispatch)
{
    /* The exact 748-byte SM5 shader declares one structured UAV named
     * fastCopyTarget at u0 and uses a (256,1,1) group. Each invocation clears
     * dwords group*512+lane and group*512+256+lane. */
    if (dispatch->group_count_y != 1 ||
        dispatch->group_count_x > SIZE_MAX /
            RE8_FAST_CLEAR_DWORDS_PER_GROUP / sizeof(uint32_t))
        return 0;

    const BeerD3D12ComputeBinding *binding = &dispatch->bindings[0];
    size_t storage_size = 0;
    uint8_t *storage = copy_binding_storage(binding, 1, &storage_size);
    size_t byte_count = (size_t)dispatch->group_count_x *
        RE8_FAST_CLEAR_DWORDS_PER_GROUP * sizeof(uint32_t);
    if (!storage || byte_count > storage_size)
        return 0;

    memset(storage, 0, byte_count);
    return 1;
}

static int execute_scene_event_reset(
    const BeerD3D12ComputeDispatch *dispatch)
{
    /* The exact 536-byte SM5 shader declares one raw UAV,
     * RWSceneEventCount at u0. Its only executable instruction performs an
     * atomic exchange of zero at byte offset zero. The shader uses one
     * (1,1,1) thread group; reject any other shape rather than extending its
     * semantics by assumption. */
    if (dispatch->group_count_x != 1 || dispatch->group_count_y != 1 ||
        dispatch->group_count_z != 1)
        return 0;

    const BeerD3D12ComputeBinding *binding = &dispatch->bindings[2];
    for (uint32_t descriptor = 0;
         descriptor < binding->descriptor_count &&
         descriptor < BEER_D3D12_MAX_RESOLVED_DESCRIPTORS;
         ++descriptor) {
        if (binding->descriptor_types[descriptor] != 1 ||
            binding->descriptor_registers[descriptor] != 0 ||
            binding->descriptor_spaces[descriptor] != 0 ||
            !binding->descriptor_resource_storage[descriptor] ||
            binding->descriptor_resource_storage_size[descriptor] <
                sizeof(uint32_t))
            continue;
        uint32_t zero = 0;
        memcpy(binding->descriptor_resource_storage[descriptor], &zero,
               sizeof(zero));
        return 1;
    }
    return 0;
}

/* Return the resource storage for the descriptor at (param, range_type,
 * register, space) — or NULL/0 if unresolved. */
static const uint8_t *
descriptor_storage(const BeerD3D12ComputeDispatch *dispatch,
                   uint32_t parameter, uint32_t range_type,
                   uint32_t reg, uint32_t space,
                   size_t *out_size,
                   uint32_t *out_width, uint32_t *out_height,
                   uint32_t *out_format, size_t *out_row_pitch)
{
    if (parameter >= BEER_D3D12_MAX_ROOT_PARAMETERS) return NULL;
    const BeerD3D12ComputeBinding *b = &dispatch->bindings[parameter];
    for (uint32_t d = 0; d < b->descriptor_count &&
         d < BEER_D3D12_MAX_RESOLVED_DESCRIPTORS; ++d) {
        if (b->descriptor_types[d] == range_type &&
            b->descriptor_registers[d] == reg &&
            b->descriptor_spaces[d] == space) {
            if (out_size) *out_size = b->descriptor_resource_storage_size[d];
            if (out_width) *out_width = b->descriptor_widths[d];
            if (out_height) *out_height = b->descriptor_heights[d];
            if (out_format) *out_format = b->descriptor_formats[d];
            if (out_row_pitch) *out_row_pitch = b->descriptor_row_pitches[d];
            return b->descriptor_resource_storage[d];
        }
    }
    return NULL;
}

/* Clamp integer coordinate to [0, limit-1]. */
static int32_t clamp_coord(int32_t v, uint32_t limit)
{
    if (v < 0) return 0;
    if (limit == 0) return 0;
    if ((uint32_t)v >= limit) return (int32_t)(limit - 1);
    return v;
}

static float unpack_unsigned_float(uint32_t value, uint32_t mantissa_bits)
{
    uint32_t mantissa_mask = (UINT32_C(1) << mantissa_bits) - 1u;
    uint32_t mantissa = value & mantissa_mask;
    uint32_t exponent = value >> mantissa_bits;
    uint32_t bits;
    if (!exponent) {
        if (!mantissa) return 0.0f;
        uint32_t leading = 31u - (uint32_t)__builtin_clz(mantissa);
        uint32_t ieee_exponent = 127u - 14u - mantissa_bits + leading;
        bits = (ieee_exponent << 23) |
            ((mantissa - (UINT32_C(1) << leading)) << (23u - leading));
    } else if (exponent == 31u) {
        bits = UINT32_C(0x7f800000) | (mantissa << (23u - mantissa_bits));
    } else {
        bits = ((exponent + 112u) << 23) |
            (mantissa << (23u - mantissa_bits));
    }
    float result;
    memcpy(&result, &bits, sizeof(result));
    return result;
}

static uint32_t pack_unsigned_float(float value, uint32_t mantissa_bits)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    if ((bits & UINT32_C(0x80000000)) || !(value > 0.0f)) return 0;
    uint32_t exponent = (bits >> 23) & 0xffu;
    uint32_t mantissa = bits & UINT32_C(0x7fffff);
    if (exponent == 0xffu)
        return (31u << mantissa_bits) |
            (mantissa ? (UINT32_C(1) << (mantissa_bits - 1u)) : 0u);
    int32_t mini_exponent = (int32_t)exponent - 127 + 15;
    if (mini_exponent >= 31) return 31u << mantissa_bits;
    if (mini_exponent <= 0) {
        uint32_t significand = UINT32_C(0x800000) | mantissa;
        uint32_t shift = (uint32_t)(1 - mini_exponent) + 23u - mantissa_bits;
        return shift < 32u ? significand >> shift : 0u;
    }
    return ((uint32_t)mini_exponent << mantissa_bits) |
        (mantissa >> (23u - mantissa_bits));
}

static void load_blur_pixel(const uint8_t *storage, size_t row_pitch,
                            uint32_t format, uint32_t x, uint32_t y,
                            float rgba[4])
{
    const uint8_t *pixel = storage + (size_t)y * row_pitch;
    if (format == 2) {
        memcpy(rgba, pixel + (size_t)x * 4u * sizeof(float),
               4u * sizeof(float));
        return;
    }
    uint32_t packed;
    memcpy(&packed, pixel + (size_t)x * sizeof(packed), sizeof(packed));
    rgba[0] = unpack_unsigned_float(packed & 0x7ffu, 6);
    rgba[1] = unpack_unsigned_float((packed >> 11) & 0x7ffu, 6);
    rgba[2] = unpack_unsigned_float((packed >> 22) & 0x3ffu, 5);
    rgba[3] = 1.0f;
}

static void store_blur_pixel(uint8_t *storage, size_t row_pitch,
                             uint32_t format, uint32_t x, uint32_t y,
                             const float rgba[4])
{
    uint8_t *pixel = storage + (size_t)y * row_pitch;
    if (format == 2) {
        memcpy(pixel + (size_t)x * 4u * sizeof(float), rgba,
               4u * sizeof(float));
        return;
    }
    uint32_t packed = pack_unsigned_float(rgba[0], 6) |
        (pack_unsigned_float(rgba[1], 6) << 11) |
        (pack_unsigned_float(rgba[2], 5) << 22);
    memcpy(pixel + (size_t)x * sizeof(packed), &packed, sizeof(packed));
}

/* Execute a separable Gaussian blur on CPU-backed RGBA-float 2D textures.
 *
 * Parameters
 *   dispatch    — snapshot with root-signature-resolved descriptors
 *   horizontal  — 1 for the horizontal pass (blur along X), 0 for vertical
 *
 * Root signature layout (12-parameter shared table, space 0):
 *   param[1] range SRV  t0..t127  — DownsampledTexture at t0, space 0
 *   param[2] range UAV  u0..u7    — RWBlurredTexture   at u0, space 0
 *
 * dispatch(groups_x, groups_y, 1):
 *   horizontal pass: groups_x * 64 == image_width;  groups_y == image_height
 *   vertical   pass: groups_x == image_width;        groups_y * 64 == image_height
 *
 * The CPU implementation mirrors the exact TGSM clamped-border sampling:
 *   for each output pixel (x,y), accumulate 7 kernel taps from the SRV
 *   with coordinates clamped to [0, width-1] / [0, height-1] respectively.
 *   Result written to the UAV.
 *
 * Synthetic format 2 uses four float32 channels. RE8 format 26 is packed
 * DXGI_FORMAT_R11G11B10_FLOAT and uses four bytes per texel. */
static int execute_blur(const BeerD3D12ComputeDispatch *dispatch,
                        int horizontal)
{
    static unsigned rejection_traces;
#define BLUR_REJECT(reason) do { \
    if (rejection_traces++ < 24) \
        fprintf(stderr, "[D3D12 BLUR] hash=%016llx groups=%u,%u reject=%s\n", \
                (unsigned long long)dispatch->shader_hash, \
                dispatch->group_count_x, dispatch->group_count_y, reason); \
    return 0; \
} while (0)
    /* Root signature: param[1]=SRV table (t0, space 0), param[2]=UAV table (u0, space 0).
     * range_type: 0=SRV, 1=UAV */
    size_t src_size = 0, dst_size = 0;
    size_t src_row_pitch = 0, dst_row_pitch = 0;
    uint32_t src_w = 0, src_h = 0, dst_w = 0, dst_h = 0;
    uint32_t src_format = 0, dst_format = 0;
    const uint8_t *src = descriptor_storage(dispatch, 1, 0, 0, 0,
                                            &src_size, &src_w, &src_h,
                                            &src_format, &src_row_pitch);
    const uint8_t *dst_const = descriptor_storage(dispatch, 2, 1, 0, 0,
                                                   &dst_size, &dst_w, &dst_h,
                                                   &dst_format, &dst_row_pitch);
    uint8_t *dst = (uint8_t *)dst_const; /* UAV is writable */

    if (!src || !dst || src_w == 0 || src_h == 0 || dst_w == 0 || dst_h == 0)
        BLUR_REJECT("binding");

    /* RE8 binds a full-size scratch SRV and smaller UAVs for this pyramid.
     * The shader's cb0 limits sampling to the output extent, and Dispatch covers
     * exactly the UAV dimensions. The source therefore needs to cover the UAV;
     * it does not need to have identical resource dimensions. */
    uint32_t width = dst_w, height = dst_h;
    if (src_w < width || src_h < height || src_format != dst_format)
        do {
            if (rejection_traces++ < 24)
                fprintf(stderr, "[D3D12 BLUR] hash=%016llx groups=%u,%u "
                        "reject=metadata src=%ux%u/f%u dst=%ux%u/f%u\n",
                        (unsigned long long)dispatch->shader_hash,
                        dispatch->group_count_x, dispatch->group_count_y,
                        src_w, src_h, src_format, dst_w, dst_h, dst_format);
            return 0;
        } while (0);

    /* Synthetic tests use RGBA32_FLOAT (format 2). RE8's live mip chain uses
     * packed R11G11B10_FLOAT (format 26), four bytes per texel. */
    size_t texel_bytes = src_format == 2 ? 4 * sizeof(float) :
                         src_format == 26 ? sizeof(uint32_t) : 0;
    if (!texel_bytes || width > SIZE_MAX / texel_bytes)
        BLUR_REJECT("format");
    size_t minimum_row_pitch = (size_t)width * texel_bytes;
    if (!src_row_pitch) src_row_pitch = minimum_row_pitch;
    if (!dst_row_pitch) dst_row_pitch = minimum_row_pitch;
    if (src_row_pitch < minimum_row_pitch || dst_row_pitch < minimum_row_pitch ||
        height > SIZE_MAX / src_row_pitch ||
        height > SIZE_MAX / dst_row_pitch ||
        src_size < src_row_pitch * height || dst_size < dst_row_pitch * height)
        BLUR_REJECT("storage");

    /* cb0 stores inclusive maximum X/Y coordinates. Use it to distinguish the
     * real odd-sized image extent from the rounded-up dispatch coverage. */
    uint64_t dispatch_width = horizontal ?
        (uint64_t)dispatch->group_count_x * 64u : dispatch->group_count_x;
    uint64_t dispatch_height = horizontal ?
        dispatch->group_count_y : (uint64_t)dispatch->group_count_y * 64u;
    uint32_t cb_width = 0, cb_height = 0;
    size_t cb_size = 0;
    const uint8_t *cb = descriptor_storage(dispatch, 0, 2, 0, 0,
                                            &cb_size, NULL, NULL, NULL, NULL);
    if (!cb && dispatch->bindings[0].kind == 3) {
        cb = dispatch->bindings[0].resource_storage;
        cb_size = dispatch->bindings[0].resource_storage_size;
    }
    if (cb && cb_size >= 2u * sizeof(uint32_t)) {
        memcpy(&cb_width, cb, sizeof(cb_width));
        memcpy(&cb_height, cb + sizeof(cb_width), sizeof(cb_height));
        if (cb_width != UINT32_MAX) ++cb_width;
        if (cb_height != UINT32_MAX) ++cb_height;
    }
    if (!cb_width || !cb_height) {
        /* Dispatch dimensions are rounded up to the shader's 64-thread group
         * size. Without a resolved cb0, the UAV view is the strongest safe
         * bound for the active region; never reject a valid partial final
         * group or let it write beyond the destination. */
        uint64_t bounded_width = dispatch_width < width ? dispatch_width : width;
        uint64_t bounded_height = dispatch_height < height ?
            dispatch_height : height;
        cb_width = bounded_width > UINT32_MAX ? UINT32_MAX :
            (uint32_t)bounded_width;
        cb_height = bounded_height > UINT32_MAX ? UINT32_MAX :
            (uint32_t)bounded_height;
    }
    if (!dispatch_width || !dispatch_height ||
        cb_width > dispatch_width || cb_height > dispatch_height ||
        cb_width > width || cb_height > height) {
        if (rejection_traces++ < 24)
            fprintf(stderr, "[D3D12 BLUR] hash=%016llx groups=%u,%u "
                    "reject=dimensions cb=%ux%u cb-resolved=%u "
                    "dispatch=%llux%llu resource=%ux%u\n",
                    (unsigned long long)dispatch->shader_hash,
                    dispatch->group_count_x, dispatch->group_count_y,
                    cb_width, cb_height, cb != NULL,
                    (unsigned long long)dispatch_width,
                    (unsigned long long)dispatch_height, width, height);
        return 0;
    }
    width = cb_width;
    height = cb_height;

    /* 7-tap separable kernel, offset range [-3, +3] from the centre. */
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            float output[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            for (int tap = 0; tap < 7; ++tap) {
                int delta = tap - 3; /* -3..+3 */
                int32_t sx = (int32_t)x + (horizontal ? delta : 0);
                int32_t sy = (int32_t)y + (horizontal ? 0 : delta);
                sx = clamp_coord(sx, width);
                sy = clamp_coord(sy, height);
                float sample[4];
                load_blur_pixel(src, src_row_pitch, src_format,
                                (uint32_t)sx, (uint32_t)sy, sample);
                for (uint32_t channel = 0; channel < 4; ++channel)
                    output[channel] += k_blur_kernel[tap] * sample[channel];
            }
            store_blur_pixel(dst, dst_row_pitch, dst_format, x, y, output);
        }
    }
#undef BLUR_REJECT
    return 1;
}

int beer_d3d12_execute_known_compute(
    const BeerD3D12ComputeDispatch *dispatch)
{
    if (!dispatch || dispatch->group_count_z != 1)
        return 0;

    if (dispatch->shader_hash == RE8_FAST_COPY_SHADER_HASH) {
        /* The captured SM5 shader has local size (256,1,1). Each invocation
         * copies one uint at group*512+lane and one at
         * group*512+256+lane from t0 to u0. */
        if (dispatch->group_count_y != 1) return 0;
        return execute_copy(dispatch, 1, 0,
                            RE8_FAST_COPY_DWORDS_PER_GROUP);
    }

    if (dispatch->shader_hash == RE8_INDIRECT_COPY_SHADER_HASH) {
        /* SPIR-V translated from the exact captured 648-byte SM5 shader proves
         * local size (64,1,1) and one uint load/store at DispatchThreadID.x.
         * RE8 binds its SRV table at root parameter 1 and UAV table at 2. */
        if (dispatch->group_count_y != 1) return 0;
        return execute_copy(dispatch, 1, 2,
                            RE8_INDIRECT_COPY_DWORDS_PER_GROUP);
    }

    if (dispatch->shader_hash == RE8_SCENE_EVENT_RESET_SHADER_HASH)
        return execute_scene_event_reset(dispatch);

    if (dispatch->shader_hash == RE8_FAST_CLEAR_SHADER_HASH)
        return execute_fast_clear(dispatch);

    if (dispatch->shader_hash == RE8_BLUR_H_SHADER_HASH) {
        /* Horizontal 7-tap Gaussian blur: threadGroup(64,1,1).
         * dispatch(groups_x, height_rows, 1): groups_x * 64 = image width.
         * Reads SRV t0 (param 1, space 0) → writes UAV u0 (param 2, space 0).
         * Descriptor tables expanded by root_signature_resolve_compute_descriptors
         * before this executor is reached. */
        return execute_blur(dispatch, 1 /* horizontal */);
    }

    if (dispatch->shader_hash == RE8_BLUR_V_SHADER_HASH) {
        /* Vertical 7-tap Gaussian blur: threadGroup(1,64,1).
         * dispatch(width_cols, groups_y, 1): groups_y * 64 = image height.
         * Same resources and kernel as the horizontal pass, transposed along Y. */
        return execute_blur(dispatch, 0 /* vertical */);
    }

    return 0;
}
