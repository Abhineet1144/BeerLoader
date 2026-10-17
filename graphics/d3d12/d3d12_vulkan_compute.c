#include "d3d12_vulkan_compute.h"
#include "vulkan_compute.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void sampled_fingerprint(const uint8_t *data, size_t size,
                                size_t *nonzero, uint64_t *hash)
{
    *nonzero = 0;
    *hash = UINT64_C(1469598103934665603);
    if (!data || !size) return;
    size_t samples = size < 4096u ? size : 4096u;
    for (size_t sample = 0; sample < samples; ++sample) {
        size_t offset = sample * size / samples;
        uint8_t value = data[offset];
        *nonzero += value != 0;
        *hash = (*hash ^ value) * UINT64_C(1099511628211);
    }
}

/* Keep this bridge independent of Vulkan SDK headers; the renderer owns the
 * Vulkan ABI and exports only this process-wide status query. */
int beer_vulkan_device_lost(void);

enum {
    ROOT_TABLE = 0,
    ROOT_CONSTANTS = 1,
    ROOT_CBV = 2,
    ROOT_SRV = 3,
    ROOT_UAV = 4
};

typedef struct RootParameterInfo {
    uint32_t valid;
    uint32_t type;
    uint32_t shader_register;
    uint32_t register_space;
} RootParameterInfo;

/* Walks the private BEERRSIG blob to learn the register of each non-table
 * root parameter, which the command-list snapshot does not retain. */
static int parse_root_parameters(const uint8_t *blob, size_t blob_size,
                                 RootParameterInfo *out)
{
    static const uint8_t magic[8] = {'B','E','E','R','R','S','I','G'};
    if (!blob || blob_size < 24 || memcmp(blob, magic, sizeof(magic)) != 0)
        return 0;
    uint32_t version = 0, parameter_count = 0;
    memcpy(&version, blob + 8, sizeof(version));
    memcpy(&parameter_count, blob + 12, sizeof(parameter_count));
    if (version < 1 || version > 3 || parameter_count > 4096) return 0;
    size_t range_size = version == 1 ? 20u : 24u;
    size_t cursor = 24;
    for (uint32_t parameter = 0; parameter < parameter_count; ++parameter) {
        if (blob_size - cursor < 8) return 0;
        uint32_t type = 0;
        memcpy(&type, blob + cursor, sizeof(type));
        cursor += 8;
        if (type == ROOT_TABLE) {
            if (blob_size - cursor < 4) return 0;
            uint32_t range_count = 0;
            memcpy(&range_count, blob + cursor, sizeof(range_count));
            cursor += 4;
            if (range_count > (blob_size - cursor) / range_size) return 0;
            cursor += (size_t)range_count * range_size;
            if (parameter < BEER_D3D12_MAX_ROOT_PARAMETERS) {
                out[parameter].valid = 1;
                out[parameter].type = type;
            }
        } else {
            size_t payload = type == ROOT_CONSTANTS ? 12u :
                (type >= ROOT_CBV && type <= ROOT_UAV ?
                     (version == 1 ? 8u : 12u) : 0u);
            if (!payload || payload > blob_size - cursor) return 0;
            if (parameter < BEER_D3D12_MAX_ROOT_PARAMETERS) {
                out[parameter].valid = 1;
                out[parameter].type = type;
                memcpy(&out[parameter].shader_register, blob + cursor, 4);
                memcpy(&out[parameter].register_space, blob + cursor + 4, 4);
            }
            cursor += payload;
        }
    }
    return 1;
}

/* Maps a typed Buffer<T> SRV DXGI format to a VkFormat (numeric values from
 * the Vulkan registry) and its element size. Unmapped formats (including
 * UNKNOWN and the typeless raw-buffer R32_TYPELESS) leave the output zero so
 * the binding stays a plain buffer. */
static void dxgi_to_texel_format(uint32_t dxgi, uint32_t *vk_format,
                                 uint32_t *bytes)
{
    static const struct { uint32_t dxgi, vk, bytes; } table[] = {
        {2, 109, 16}, {3, 107, 16}, {4, 108, 16},
        {10, 97, 8}, {11, 91, 8}, {12, 95, 8},
        {16, 103, 8}, {17, 101, 8}, {18, 102, 8},
        {28, 37, 4}, {30, 41, 4},
        {34, 83, 4}, {36, 81, 4},
        {41, 100, 4}, {42, 98, 4}, {43, 99, 4},
        {49, 16, 2}, {54, 76, 2}, {57, 74, 2},
        {61, 9, 1}, {62, 13, 1}
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); ++i) {
        if (table[i].dxgi == dxgi) {
            *vk_format = table[i].vk;
            *bytes = table[i].bytes;
            return;
        }
    }
}

/* Typed 2D texture formats the executor can mirror (VkFormat numeric values
 * from the Vulkan registry) with their texel size. */
static int dxgi_to_compressed_format(uint32_t dxgi, uint32_t *vk_format,
                                     uint32_t *bytes_per_block)
{
    static const struct { uint32_t dxgi, vk, bytes; } table[] = {
        {71, 131, 8}, {72, 132, 8},
        {74, 133, 16}, {75, 134, 16},
        {77, 135, 16}, {78, 136, 16},
        {80, 137, 8}, {81, 138, 8},
        {83, 139, 16}, {84, 140, 16},
        {95, 143, 16}, {96, 144, 16},
        {98, 145, 16}, {99, 146, 16}
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); ++i) {
        if (table[i].dxgi == dxgi) {
            *vk_format = table[i].vk;
            *bytes_per_block = table[i].bytes;
            return 1;
        }
    }
    return 0;
}

static int dxgi_to_texture_format(uint32_t dxgi, uint32_t *vk_format,
                                  uint32_t *bytes)
{
    static const struct { uint32_t dxgi, vk, bytes; } table[] = {
        {2, 109, 16}, {3, 107, 16}, {4, 108, 16},
        {10, 97, 8}, {11, 91, 8}, {12, 95, 8}, {13, 92, 8}, {14, 96, 8},
        {16, 103, 8}, {17, 101, 8}, {18, 102, 8},
        {24, 64, 4}, {25, 68, 4}, {26, 122, 4},
        {28, 37, 4}, {29, 43, 4}, {30, 41, 4}, {31, 38, 4}, {32, 42, 4},
        {34, 83, 4}, {35, 77, 4}, {36, 81, 4}, {37, 78, 4}, {38, 82, 4},
        {41, 100, 4}, {42, 98, 4}, {43, 99, 4},
        {49, 16, 2}, {50, 20, 2}, {51, 17, 2}, {52, 21, 2},
        {54, 76, 2}, {56, 70, 2}, {57, 74, 2}, {58, 71, 2}, {59, 75, 2},
        {61, 9, 1}, {62, 13, 1}, {63, 10, 1}, {64, 14, 1},
        {87, 44, 4}, {91, 50, 4}
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); ++i) {
        if (table[i].dxgi == dxgi) {
            *vk_format = table[i].vk;
            *bytes = table[i].bytes;
            return 1;
        }
    }
    return 0;
}

/* Texel size of a resource format, typed or typeless; zero if unknown. */
static uint32_t dxgi_resource_bytes(uint32_t dxgi)
{
    uint32_t vk = 0, bytes = 0;
    if (dxgi_to_texture_format(dxgi, &vk, &bytes)) return bytes;
    switch (dxgi) {
    case 1: return 16;
    case 9: case 15: return 8;
    case 23: case 27: case 33: case 39: case 90: case 92: return 4;
    case 48: case 53: return 2;
    case 60: return 1;
    default: return 0;
    }
}

/* Maps a D3D12 sampler onto Vulkan. D3D's comparison, minimum and maximum
 * reduction modes map directly to Vulkan comparison sampling and
 * VK_EXT_sampler_filter_minmax. */
static const char *map_sampler(const uint32_t desc[8],
                               BeerVulkanComputeBinding *binding)
{
    uint32_t filter = desc[0];
    uint32_t reduction = (filter >> 7) & 3u;
    binding->min_filter = (filter >> 4) & 1u;
    binding->mag_filter = (filter >> 2) & 1u;
    binding->mipmap_mode = filter & 1u;
    binding->anisotropy_enable = (filter & 0x40u) != 0;
    binding->max_anisotropy = binding->anisotropy_enable ?
        (float)(desc[5] ? desc[5] : 1u) : 1.0f;
    if (reduction == 1u) {
        if (desc[6] < 1u || desc[6] > 8u)
            return "comparison sampler function invalid";
        binding->compare_enable = 1;
        binding->compare_op = desc[6] - 1u;
    } else if (reduction == 2u) {
        binding->reduction_mode = 1;
    } else if (reduction == 3u) {
        binding->reduction_mode = 2;
    }
    uint32_t *addresses[3] = { &binding->address_u, &binding->address_v,
                               &binding->address_w };
    for (int a = 0; a < 3; ++a) {
        switch (desc[1 + a]) {
        case 1: *addresses[a] = 0; break; /* WRAP -> REPEAT */
        case 2: *addresses[a] = 1; break; /* MIRROR -> MIRRORED_REPEAT */
        case 3: *addresses[a] = 2; break; /* CLAMP -> CLAMP_TO_EDGE */
        default: return "sampler border or mirror-once addressing";
        }
    }
    return NULL;
}

/* Fills an image binding from a resolved descriptor-table entry. Compute
 * mirrors one selected mip of 1D/2D arrays or a 3D volume; multisampling and
 * multi-mip views remain explicit rejections. */
static const char *map_texture_descriptor(
    const BeerD3D12ComputeBinding *source, uint32_t i, int is_srv,
    uint32_t resource_type, BeerVulkanComputeBinding *binding)
{
    const int texture_1d = resource_type ==
        BEER_D3D12_SHADER_RESOURCE_TEXTURE_1D;
    const int texture_1d_array = resource_type ==
        BEER_D3D12_SHADER_RESOURCE_TEXTURE_1D_ARRAY;
    const int texture_2d = resource_type ==
        BEER_D3D12_SHADER_RESOURCE_TEXTURE_2D;
    const int texture_2d_array = resource_type ==
        BEER_D3D12_SHADER_RESOURCE_TEXTURE_2D_ARRAY;
    const int texture_3d = resource_type ==
        BEER_D3D12_SHADER_RESOURCE_TEXTURE_3D;
    const int texture_cube = resource_type ==
        BEER_D3D12_SHADER_RESOURCE_TEXTURE_CUBE;
    const int texture_cube_array = resource_type ==
        BEER_D3D12_SHADER_RESOURCE_TEXTURE_CUBE_ARRAY;
    const uint32_t expected_view = texture_1d ? 2u :
        texture_1d_array ? 3u : texture_2d ? 4u :
        texture_2d_array ? 5u : texture_3d ? 8u :
        texture_cube ? 9u : texture_cube_array ? 10u : 0u;
    const uint32_t expected_resource = (texture_1d || texture_1d_array) ? 2u :
        (texture_2d || texture_2d_array || texture_cube ||
         texture_cube_array) ? 3u : texture_3d ? 4u : 0u;
    const uint32_t actual_view = source->descriptor_view_dimensions[i];
    const uint32_t resource_layers = source->descriptor_resource_layers[i] ?
        source->descriptor_resource_layers[i] : 1u;
    /* D3D permits an array declaration to consume a non-array view when the
     * resource contains exactly one layer. Preserve the shader's array type in
     * Vulkan by promoting that view to a one-layer array image. */
    const int single_layer_array_view =
        ((texture_1d_array && actual_view == 2u) ||
         (texture_2d_array && actual_view == 4u)) && resource_layers == 1u;
    if (!expected_view ||
        (actual_view != expected_view && !single_layer_array_view) ||
        source->descriptor_resource_dimensions[i] != expected_resource ||
        source->descriptor_resource_samples[i] != 1) {
        if (getenv("BEER_D3D12_DIAGNOSTICS")) {
            static unsigned shape_traces;
            if (shape_traces++ < 32)
                fprintf(stderr,
                    "[D3D12 VULKAN COMPUTE] image shape mismatch resource=%u "
                    "expected-view=%u actual-view=%u expected-resource=%u "
                    "actual-resource=%u layers=%u first=%u count=%u depth=%u "
                    "samples=%u srv=%d\n",
                    resource_type, expected_view,
                    source->descriptor_view_dimensions[i], expected_resource,
                    source->descriptor_resource_dimensions[i],
                    source->descriptor_resource_layers[i],
                    source->descriptor_view_first_layers[i],
                    source->descriptor_view_layer_counts[i],
                    source->descriptor_depths[i],
                    source->descriptor_resource_samples[i], is_srv);
        }
        return "texture image shape metadata invalid";
    }
    uint32_t mip_levels = is_srv ? source->descriptor_view_mip_levels[i] : 1u;
    uint32_t resource_mips = source->descriptor_resource_mips[i] ?
        source->descriptor_resource_mips[i] : 1u;
    uint32_t selected_mip = source->descriptor_subresources[i] % resource_mips;
    if (!mip_levels || mip_levels == UINT32_MAX)
        mip_levels = resource_mips - selected_mip;
    if (!mip_levels || selected_mip >= resource_mips ||
        mip_levels > resource_mips - selected_mip)
        return "texture mip view range invalid";
    uint32_t resource_format = source->descriptor_formats[i];
    uint32_t view_format = source->descriptor_view_formats[i];
    if (!view_format) view_format = resource_format;
    uint32_t vk_format = 0, bytes = 0, block_bytes = 0;
    const int compressed = dxgi_to_compressed_format(
        view_format, &vk_format, &block_bytes);
    const int depth_aspect = resource_format == 19 && view_format == 21;
    if (!compressed && !depth_aspect &&
        !dxgi_to_texture_format(view_format, &vk_format, &bytes)) {
        if (getenv("BEER_D3D12_DIAGNOSTICS"))
            fprintf(stderr,
                    "[D3D12 VULKAN COMPUTE] unmapped texture format: "
                    "resource=%u view=%u size=%ux%u\n", resource_format,
                    source->descriptor_view_formats[i],
                    source->descriptor_widths[i],
                    source->descriptor_heights[i]);
        return "texture format not mapped";
    }
    if (depth_aspect) {
        vk_format = 126; /* VK_FORMAT_D32_SFLOAT */
        bytes = 4;
    } else if (!compressed && dxgi_resource_bytes(resource_format) != bytes) {
        return "texture view and resource texel sizes differ";
    }
    uint32_t width = source->descriptor_widths[i];
    uint32_t height = source->descriptor_heights[i];
    size_t pitch = source->descriptor_row_pitches[i];
    uint32_t layers = 1;
    uint32_t depth = 1;
    if (texture_1d || texture_1d_array) height = 1;
    if (texture_1d_array || texture_2d_array) {
        uint32_t first_layer = single_layer_array_view ? 0u :
            source->descriptor_view_first_layers[i];
        layers = single_layer_array_view ? 1u :
            source->descriptor_view_layer_counts[i];
        if (!layers || first_layer >= resource_layers ||
            layers > resource_layers - first_layer)
            return "texture array view range invalid";
    } else if (texture_cube) {
        layers = 6;
        if (resource_layers < layers)
            return "texture cube requires six faces";
    } else if (texture_cube_array) {
        uint32_t first_face = source->descriptor_view_first_layers[i];
        uint32_t cube_count = source->descriptor_view_layer_counts[i];
        if (!cube_count || cube_count > UINT32_MAX / 6u)
            return "texture cube-array view range invalid";
        layers = cube_count * 6u;
        if (first_face >= resource_layers ||
            layers > resource_layers - first_face)
            return "texture cube-array view range invalid";
    } else if (texture_3d) {
        depth = source->descriptor_depths[i];
        if (!depth) return "texture 3D depth unavailable";
    }
    size_t extent = 0;
    if (compressed) {
        uint32_t blocks_x = (width + 3u) / 4u;
        uint32_t blocks_y = (height + 3u) / 4u;
        size_t minimum_pitch = (size_t)blocks_x * block_bytes;
        if (!width || !height || !pitch || pitch > UINT32_MAX ||
            pitch < minimum_pitch || pitch > SIZE_MAX / blocks_y)
            return "compressed texture row layout unavailable";
        extent = pitch * blocks_y;
    } else {
        if (!width || !height || !pitch || pitch > UINT32_MAX ||
            pitch % bytes || pitch < (size_t)width * bytes)
            return "texture row layout unavailable";
        extent = pitch * (height - 1) + (size_t)width * bytes;
    }
    uint32_t image_count = texture_3d ? depth : layers;
    if (!extent || image_count > SIZE_MAX / extent)
        return "texture extent overflow";
    extent *= image_count;
    if (mip_levels > 1) {
        uint32_t level_width = width;
        uint32_t level_height = height;
        uint32_t level_depth = depth;
        extent = 0;
        for (uint32_t level = 0; level < mip_levels; ++level) {
            size_t level_pitch = compressed ?
                (size_t)((level_width + 3u) / 4u) * block_bytes :
                (size_t)level_width * bytes;
            uint32_t level_rows = compressed ?
                (level_height + 3u) / 4u : level_height;
            uint32_t level_images = texture_3d ? level_depth : layers;
            if (!level_pitch || !level_rows ||
                level_pitch > SIZE_MAX / level_rows ||
                level_images > SIZE_MAX / (level_pitch * level_rows) ||
                extent > SIZE_MAX - level_pitch * level_rows * level_images)
                return "texture mip extent overflow";
            extent += level_pitch * level_rows * level_images;
            if (level_width > 1) level_width >>= 1;
            if (level_height > 1) level_height >>= 1;
            if (level_depth > 1) level_depth >>= 1;
        }
    }
    if (!source->descriptor_resource_storage[i] ||
        source->descriptor_resource_storage_size[i] < extent)
        return "texture storage smaller than its layout";
    binding->kind = BEER_VULKAN_COMPUTE_BINDING_IMAGE;
    binding->data = source->descriptor_resource_storage[i];
    binding->size = extent;
    binding->resource = source->descriptor_resources[i];
    binding->serial_location = (uint64_t *)
        source->descriptor_resource_serial_locations[i];
    binding->serial = binding->serial_location ?
        *binding->serial_location : UINT64_MAX;
    binding->image_format = vk_format;
    binding->image_bytes = bytes;
    binding->image_block_width = compressed ? 4u : 0u;
    binding->image_block_height = compressed ? 4u : 0u;
    binding->image_block_bytes = compressed ? block_bytes : 0u;
    binding->image_width = width;
    binding->image_height = height;
    binding->image_row_pitch = (uint32_t)pitch;
    binding->image_dimension = (texture_1d || texture_1d_array) ? 1u :
        texture_3d ? 3u : 2u;
    binding->image_arrayed = texture_1d_array || texture_2d_array ||
        texture_cube_array;
    binding->image_cube = texture_cube || texture_cube_array;
    binding->image_layers = layers;
    binding->image_depth = depth;
    binding->image_mip_levels = mip_levels;
    if (mip_levels > 1 && (texture_1d_array || texture_2d_array ||
                           texture_cube || texture_cube_array)) {
        /* Host D3D storage is layer-major: all resource mips of layer zero,
         * then all mips of layer one. Record that stride so the Vulkan upload
         * can issue one copy region per mip/layer without repacking. */
        size_t layer_stride = 0;
        uint32_t full_width = (uint32_t)(source->descriptor_widths[i] <<
            selected_mip);
        uint32_t full_height = (uint32_t)(source->descriptor_heights[i] <<
            selected_mip);
        for (uint32_t mip = 0; mip < resource_mips; ++mip) {
            uint32_t mip_width = full_width >> mip;
            uint32_t mip_height = full_height >> mip;
            if (!mip_width) mip_width = 1;
            if (!mip_height) mip_height = 1;
            size_t mip_pitch = compressed ?
                (size_t)((mip_width + 3u) / 4u) * block_bytes :
                (size_t)mip_width * bytes;
            uint32_t mip_rows = compressed ?
                (mip_height + 3u) / 4u : mip_height;
            if (mip_pitch > SIZE_MAX / mip_rows ||
                layer_stride > SIZE_MAX - mip_pitch * mip_rows)
                return "texture array layer stride overflow";
            layer_stride += mip_pitch * mip_rows;
        }
        binding->image_layer_stride = layer_stride;
    }
    return NULL;
}

typedef struct PreparedComputeBindings {
    BeerVulkanComputeBinding bindings[BEER_VULKAN_COMPUTE_MAX_BINDINGS];
    uint32_t constant_blocks[BEER_D3D12_MAX_ROOT_PARAMETERS]
                            [BEER_D3D12_MAX_ROOT_CONSTANTS];
} PreparedComputeBindings;

/* Resolves immutable root-signature and descriptor metadata without copying
 * resource payloads into Vulkan staging. The resulting pointers refer to the
 * command-list snapshot and remain valid for the containing IR batch; root
 * constants are owned by this record so no stack address escapes preparation. */
static int prepare_compute_bindings(
    const BeerD3D12ComputeDispatch *dispatch,
    const BeerD3D12TranslatedShader *shader,
    PreparedComputeBindings *prepared, const char **reason)
{
    if (!dispatch || !shader || !prepared || !shader->spirv ||
        !shader->spirv_size) {
        *reason = "invalid arguments";
        return 0;
    }

    memset(prepared, 0, sizeof(*prepared));
    RootParameterInfo parameters[BEER_D3D12_MAX_ROOT_PARAMETERS];
    memset(parameters, 0, sizeof(parameters));
    if (!parse_root_parameters(dispatch->root_signature_blob,
                               dispatch->root_signature_blob_size,
                               parameters)) {
        *reason = "root signature unavailable";
        return 0;
    }

    BeerVulkanComputeBinding *bindings = prepared->bindings;
    uint32_t (*constant_blocks)[BEER_D3D12_MAX_ROOT_CONSTANTS] =
        prepared->constant_blocks;
    uint32_t constant_used[BEER_D3D12_MAX_ROOT_PARAMETERS];
    memset(constant_used, 0, sizeof(constant_used));
    if (shader->descriptor_count > BEER_VULKAN_COMPUTE_MAX_BINDINGS) {
        *reason = "too many descriptors";
        return 0;
    }

    for (uint32_t d = 0; d < shader->descriptor_count; ++d) {
        const BeerD3D12ShaderDescriptor *descriptor = &shader->descriptors[d];
        BeerVulkanComputeBinding *binding = &bindings[d];
        memset(binding, 0, sizeof(*binding));
        binding->binding = descriptor->binding;
        binding->uniform = descriptor->type == BEER_D3D12_SHADER_DESCRIPTOR_CBV;
        binding->writable = descriptor->type == BEER_D3D12_SHADER_DESCRIPTOR_UAV;

        const int is_sampler =
            descriptor->type == BEER_D3D12_SHADER_DESCRIPTOR_SAMPLER;
        const int is_texture =
            (descriptor->type == BEER_D3D12_SHADER_DESCRIPTOR_SRV ||
             descriptor->type == BEER_D3D12_SHADER_DESCRIPTOR_UAV) &&
            (descriptor->resource_type == BEER_D3D12_SHADER_RESOURCE_TEXTURE_1D ||
             descriptor->resource_type == BEER_D3D12_SHADER_RESOURCE_TEXTURE_2D ||
             descriptor->resource_type == BEER_D3D12_SHADER_RESOURCE_TEXTURE_3D ||
             descriptor->resource_type == BEER_D3D12_SHADER_RESOURCE_TEXTURE_CUBE ||
             descriptor->resource_type == BEER_D3D12_SHADER_RESOURCE_TEXTURE_1D_ARRAY ||
             descriptor->resource_type == BEER_D3D12_SHADER_RESOURCE_TEXTURE_2D_ARRAY ||
             descriptor->resource_type == BEER_D3D12_SHADER_RESOURCE_TEXTURE_CUBE_ARRAY);
        int resolved = 0;
        if (is_sampler)
            binding->kind = BEER_VULKAN_COMPUTE_BINDING_SAMPLER;

        if (descriptor->count != 1 ||
            (!is_sampler && !is_texture &&
             descriptor->resource_type != BEER_D3D12_SHADER_RESOURCE_BUFFER &&
             descriptor->type != BEER_D3D12_SHADER_DESCRIPTOR_CBV)) {
            *reason = "texture shape or descriptor-array resource not mapped";
            if (getenv("BEER_D3D12_DIAGNOSTICS")) {
                static uint64_t listed[64];
                static unsigned listed_count;
                int seen = 0;
                for (unsigned s = 0; s < listed_count; ++s)
                    if (listed[s] == shader->shader_hash) seen = 1;
                if (!seen && listed_count < 64) {
                    listed[listed_count++] = shader->shader_hash;
                    for (uint32_t k = 0; k < shader->descriptor_count; ++k) {
                        const BeerD3D12ShaderDescriptor *item =
                            &shader->descriptors[k];
                        fprintf(stderr,
                                "[D3D12 VULKAN COMPUTE] shader=%016llx "
                                "descriptor b%u type=%u resource=%u data=%u "
                                "flags=%x count=%u space=%u reg=%u\n",
                                (unsigned long long)shader->shader_hash,
                                item->binding, item->type,
                                item->resource_type, item->resource_data_type,
                                item->flags, item->count, item->register_space,
                                item->register_index);
                    }
                }
            }
            return 0;
        }

        for (uint32_t p = 0; p < BEER_D3D12_MAX_ROOT_PARAMETERS &&
                             !resolved; ++p) {
            if (!parameters[p].valid) continue;
            const BeerD3D12ComputeBinding *source = &dispatch->bindings[p];
            if (parameters[p].type == ROOT_TABLE && source->kind == 1) {
                for (uint32_t i = 0; i < source->descriptor_count &&
                     i < BEER_D3D12_MAX_RESOLVED_DESCRIPTORS; ++i) {
                    if (source->descriptor_types[i] != descriptor->type ||
                        source->descriptor_registers[i] !=
                            descriptor->register_index ||
                        source->descriptor_spaces[i] !=
                            descriptor->register_space)
                        continue;
                    binding->source_root_parameter = p;
                    binding->source_descriptor_index = i;
                    binding->source_descriptor_type =
                        source->descriptor_types[i];
                    binding->source_register =
                        source->descriptor_registers[i];
                    binding->source_register_space =
                        source->descriptor_spaces[i];
                    binding->source_descriptor_table = source->descriptor_table;
                    binding->source_descriptor_handle =
                        source->descriptor_handles[i];
                    binding->source_buffer_location =
                        source->descriptor_buffer_locations[i];
                    binding->source_resource_gpu_base =
                        source->descriptor_resource_gpu_bases[i];
                    binding->source_resource_total_size =
                        source->descriptor_resource_total_sizes[i];
                    binding->source_resource_heap_type =
                        source->descriptor_resource_heap_types[i];
                    binding->source_descriptor_size =
                        source->descriptor_raw_sizes[i];
                    memcpy(binding->source_descriptor_bytes,
                           source->descriptor_raw_bytes[i],
                           binding->source_descriptor_size);
                    if (is_sampler) {
                        const char *sampler_reason = map_sampler(
                            source->descriptor_samplers[i], binding);
                        if (sampler_reason) {
                            *reason = sampler_reason;
                            return 0;
                        }
                        resolved = 1;
                        break;
                    }
                    if (!source->descriptor_resource_storage[i] ||
                        !source->descriptor_resource_storage_size[i])
                        continue;
                    if (is_texture) {
                        const char *texture_reason = map_texture_descriptor(
                            source, i,
                            descriptor->type == BEER_D3D12_SHADER_DESCRIPTOR_SRV,
                            descriptor->resource_type, binding);
                        if (texture_reason) {
                            *reason = texture_reason;
                            return 0;
                        }
                        resolved = 1;
                        break;
                    }
                    binding->data = source->descriptor_resource_storage[i];
                    binding->size = source->descriptor_resource_storage_size[i];
                    binding->resource = source->descriptor_resources[i];
                    binding->serial_location = (uint64_t *)
                        source->descriptor_resource_serial_locations[i];
                    binding->serial = binding->serial_location ?
                        *binding->serial_location : UINT64_MAX;
                    binding->resource_heap_type =
                        source->descriptor_resource_heap_types[i];
                    resolved = 1;
                    /* vkd3d-shader declares raw and structured buffers as
                     * R32_UINT texel buffers; typed views keep their DXGI
                     * format. Apply the D3D12 view origin and extent before
                     * exposing the range to Vulkan. FirstElement is measured
                     * in structures for structured views and in typed/raw
                     * elements otherwise. */
                    if (descriptor->type != BEER_D3D12_SHADER_DESCRIPTOR_CBV &&
                        source->descriptor_formats[i] == 0 &&
                        source->descriptor_row_pitches[i] == 0) {
                        uint32_t view_format = source->descriptor_view_formats[i];
                        uint32_t stride =
                            source->descriptor_view_structure_strides[i];
                        uint64_t first =
                            source->descriptor_view_first_elements[i];
                        uint32_t count =
                            source->descriptor_view_element_counts[i];
                        size_t element_bytes = stride;
                        if (!element_bytes &&
                            (view_format == 0 || view_format == 39))
                            element_bytes = sizeof(uint32_t);
                        if (element_bytes && first <= SIZE_MAX / element_bytes) {
                            size_t offset = (size_t)first * element_bytes;
                            if (offset < binding->size) {
                                binding->data += offset;
                                binding->size -= offset;
                                if (count && count <= SIZE_MAX / element_bytes &&
                                    (size_t)count * element_bytes < binding->size)
                                    binding->size = (size_t)count * element_bytes;
                                dxgi_to_texel_format(42, &binding->texel_format,
                                                     &binding->texel_bytes);
                            }
                        } else if (!element_bytes && !first && !view_format) {
                            dxgi_to_texel_format(42, &binding->texel_format,
                                                 &binding->texel_bytes);
                        } else if (!first) {
                            dxgi_to_texel_format(view_format,
                                                 &binding->texel_format,
                                                 &binding->texel_bytes);
                        }
                    }
                    if (getenv("BEER_D3D12_DIAGNOSTICS") &&
                        descriptor->type != BEER_D3D12_SHADER_DESCRIPTOR_CBV)
                        fprintf(stderr,
                                "[D3D12 VULKAN COMPUTE] descriptor b%u type=%u "
                                "resource-format=%u view-format=%u "
                                "first-element=%llu -> vk=%u\n",
                                descriptor->binding, descriptor->type,
                                source->descriptor_formats[i],
                                source->descriptor_view_formats[i],
                                (unsigned long long)
                                    source->descriptor_view_first_elements[i],
                                binding->texel_format);
                    break;
                }
                continue;
            }
            if (parameters[p].shader_register != descriptor->register_index ||
                parameters[p].register_space != descriptor->register_space ||
                is_sampler || is_texture)
                continue;
            binding->source_root_parameter = p;
            binding->source_descriptor_type = descriptor->type;
            binding->source_register = parameters[p].shader_register;
            binding->source_register_space = parameters[p].register_space;
            binding->source_buffer_location = source->buffer_location;
            if (parameters[p].type == ROOT_CONSTANTS && source->kind == 2 &&
                descriptor->type == BEER_D3D12_SHADER_DESCRIPTOR_CBV &&
                source->constant_count) {
                if (!constant_used[p]) {
                    memset(constant_blocks[p], 0, sizeof(constant_blocks[p]));
                    memcpy(constant_blocks[p], source->constants,
                           (size_t)source->constant_count * sizeof(uint32_t));
                    constant_used[p] = 1;
                }
                binding->data = (uint8_t *)constant_blocks[p];
                binding->size = sizeof(constant_blocks[p]);
                resolved = 1;
            } else if (source->resource_storage &&
                       source->resource_storage_size &&
                       ((parameters[p].type == ROOT_CBV && source->kind == 3 &&
                         descriptor->type == BEER_D3D12_SHADER_DESCRIPTOR_CBV) ||
                        (parameters[p].type == ROOT_SRV && source->kind == 4 &&
                         descriptor->type == BEER_D3D12_SHADER_DESCRIPTOR_SRV) ||
                        (parameters[p].type == ROOT_UAV && source->kind == 5 &&
                         descriptor->type == BEER_D3D12_SHADER_DESCRIPTOR_UAV))) {
                binding->data = source->resource_storage;
                binding->size = source->resource_storage_size;
                binding->resource = source->resource;
                binding->serial_location = (uint64_t *)
                    source->resource_serial_location;
                binding->serial = binding->serial_location ?
                    *binding->serial_location : UINT64_MAX;
                binding->resource_heap_type = source->resource_heap_type;
                resolved = 1;
            }
        }
        if (!resolved) {
            *reason = "descriptor not resolved from root signature";
            return 0;
        }
    }
    return 1;
}

static int execute_prepared_compute(
    const BeerD3D12ComputeDispatch *dispatch,
    const BeerD3D12TranslatedShader *shader, uint32_t hazards_before,
    PreparedComputeBindings *prepared, const char **reason)
{
    BeerVulkanComputeBinding *bindings = prepared->bindings;

    /* Optional rolling capture taken before vkCmdDispatch. Device-loss
     * diagnostics run only after the NVIDIA watchdog has destroyed the context;
     * this bounded snapshot preserves the exact root-signature and descriptor
     * provenance that selected each binding without retaining frame history. */
    const char *capture_directory = getenv("BEER_VULKAN_PRE_DISPATCH_CAPTURE_DIR");
    /* Freeze the rolling artifacts at the causal dispatch. Queue replay may
     * continue visiting later recorded dispatches after the Vulkan context is
     * lost; allowing those rejected operations to write here replaced the
     * useful pre-loss provenance with unrelated state. */
    if (capture_directory && *capture_directory &&
        !beer_vulkan_device_lost()) {
        char path[512];
        FILE *capture = NULL;
        snprintf(path, sizeof(path), "%s/beer-compute-last-root.bin",
                 capture_directory);
        capture = fopen(path, "wb");
        if (capture) {
            fwrite(dispatch->root_signature_blob, 1,
                   dispatch->root_signature_blob_size, capture);
            fclose(capture);
        }
        snprintf(path, sizeof(path), "%s/beer-compute-last.spv",
                 capture_directory);
        capture = fopen(path, "wb");
        if (capture) {
            fwrite(shader->spirv, 1, shader->spirv_size, capture);
            fclose(capture);
        }
        snprintf(path, sizeof(path), "%s/beer-compute-last.dxbc",
                 capture_directory);
        capture = fopen(path, "wb");
        if (capture) {
            fwrite(dispatch->shader, 1, dispatch->shader_size, capture);
            fclose(capture);
        }
        snprintf(path, sizeof(path), "%s/beer-compute-last.txt",
                 capture_directory);
        capture = fopen(path, "w");
        if (capture) {
            fprintf(capture,
                    "submission=%llu list=%u sequence=%llu shader=%016llx "
                    "groups=%ux%ux%u descriptors=%u root-bytes=%zu\n",
                    (unsigned long long)dispatch->debug_submission,
                    dispatch->debug_list_index,
                    (unsigned long long)dispatch->debug_sequence,
                    (unsigned long long)shader->shader_hash,
                    dispatch->group_count_x, dispatch->group_count_y,
                    dispatch->group_count_z, shader->descriptor_count,
                    dispatch->root_signature_blob_size);
            for (uint32_t p = 0; p < BEER_D3D12_MAX_ROOT_PARAMETERS; ++p) {
                const BeerD3D12ComputeBinding *source = &dispatch->bindings[p];
                if (!source->kind && !source->descriptor_count) continue;
                fprintf(capture,
                        "root[%u] kind=%u table=%016llx gpuva=%016llx "
                        "bytes=%zu constants=%u descriptors=%u\n",
                        p, source->kind,
                        (unsigned long long)source->descriptor_table,
                        (unsigned long long)source->buffer_location,
                        source->resource_storage_size, source->constant_count,
                        source->descriptor_count);
                for (uint32_t i = 0;
                     i < source->descriptor_count &&
                     i < BEER_D3D12_MAX_RESOLVED_DESCRIPTORS; ++i) {
                    fprintf(capture,
                            "  descriptor[%u] type=%u reg=%u space=%u "
                            "resource=%p bytes=%zu storage=%p view-format=%u "
                            "dim=%u first=%llu count=%u stride=%u\n",
                            i, source->descriptor_types[i],
                            source->descriptor_registers[i],
                            source->descriptor_spaces[i],
                            source->descriptor_resources[i],
                            source->descriptor_resource_storage_size[i],
                            source->descriptor_resource_storage[i],
                            source->descriptor_view_formats[i],
                            source->descriptor_view_dimensions[i],
                            (unsigned long long)
                                source->descriptor_view_first_elements[i],
                            source->descriptor_view_element_counts[i],
                            source->descriptor_view_structure_strides[i]);
                }
                if (source->kind == 1 && source->descriptor_table) {
                    for (uint32_t heap = 0;
                         heap < dispatch->descriptor_heap_count; ++heap) {
                        uint64_t heap_start =
                            dispatch->descriptor_heap_gpu_start[heap];
                        size_t heap_size = dispatch->descriptor_heap_size[heap];
                        const uint8_t *heap_storage =
                            dispatch->descriptor_heap_storage[heap];
                        if (!heap_storage || source->descriptor_table < heap_start)
                            continue;
                        uint64_t offset64 = source->descriptor_table - heap_start;
                        if (offset64 >= heap_size) continue;
                        size_t offset = (size_t)offset64;
                        size_t available = heap_size - offset;
                        if (available > 4096) available = 4096;
                        char table_path[512];
                        snprintf(table_path, sizeof(table_path),
                                 "%s/beer-compute-last-root-%u-table.bin",
                                 capture_directory, p);
                        FILE *table_file = fopen(table_path, "wb");
                        if (table_file) {
                            fwrite(heap_storage + offset, 1, available,
                                   table_file);
                            fclose(table_file);
                        }
                        fprintf(capture,
                                "  table heap=%u type=%u start=%016llx "
                                "offset=%zu captured=%zu\n",
                                heap, dispatch->descriptor_heap_type[heap],
                                (unsigned long long)heap_start, offset,
                                available);
                        break;
                    }
                }
            }
            for (uint32_t d = 0; d < shader->descriptor_count; ++d) {
                const BeerD3D12ShaderDescriptor *descriptor =
                    &shader->descriptors[d];
                const BeerVulkanComputeBinding *binding = &bindings[d];
                uint64_t relative_offset = binding->source_buffer_location >=
                        binding->source_resource_gpu_base ?
                    binding->source_buffer_location -
                        binding->source_resource_gpu_base : UINT64_MAX;
                fprintf(capture,
                        "binding[%u] vk=%u type=%u resource-type=%u reg=%u "
                        "space=%u kind=%u bytes=%zu resource=%p root=%u "
                        "descriptor=%u table=%016llx handle=%016llx "
                        "gpuva=%016llx resource-base=%016llx relative=%016llx "
                        "resource-bytes=%zu heap-type=%u raw=",
                        d, descriptor->binding, descriptor->type,
                        descriptor->resource_type, descriptor->register_index,
                        descriptor->register_space, binding->kind,
                        binding->size, binding->resource,
                        binding->source_root_parameter,
                        binding->source_descriptor_index,
                        (unsigned long long)binding->source_descriptor_table,
                        (unsigned long long)binding->source_descriptor_handle,
                        (unsigned long long)binding->source_buffer_location,
                        (unsigned long long)binding->source_resource_gpu_base,
                        (unsigned long long)relative_offset,
                        binding->source_resource_total_size,
                        binding->source_resource_heap_type);
                for (uint32_t byte = 0;
                     byte < binding->source_descriptor_size; ++byte)
                    fprintf(capture, "%02x",
                            binding->source_descriptor_bytes[byte]);
                fputc('\n', capture);
                if (binding->kind == BEER_VULKAN_COMPUTE_BINDING_BUFFER &&
                    binding->data && binding->size &&
                    binding->size <= 64u * 1024u) {
                    char binding_path[512];
                    snprintf(binding_path, sizeof(binding_path),
                             "%s/beer-compute-last-binding-%u.bin",
                             capture_directory, descriptor->binding);
                    FILE *binding_file = fopen(binding_path, "wb");
                    if (binding_file) {
                        fwrite(binding->data, 1, binding->size, binding_file);
                        fclose(binding_file);
                    }
                }
            }
            fclose(capture);
        }
    }

    uint32_t vulkan_hazards = 0;
    if (hazards_before & BEER_D3D12_COMPUTE_HAZARD_RAW)
        vulkan_hazards |= BEER_VULKAN_COMPUTE_HAZARD_RAW;
    if (hazards_before & BEER_D3D12_COMPUTE_HAZARD_WAR)
        vulkan_hazards |= BEER_VULKAN_COMPUTE_HAZARD_WAR;
    if (hazards_before & BEER_D3D12_COMPUTE_HAZARD_WAW)
        vulkan_hazards |= BEER_VULKAN_COMPUTE_HAZARD_WAW;
    int executed = beer_vulkan_compute_dispatch_with_hazards(
        shader, (const uint32_t *)shader->spirv, shader->spirv_size,
        bindings, shader->descriptor_count, dispatch->group_count_x,
        dispatch->group_count_y, dispatch->group_count_z, vulkan_hazards,
        reason);
    if (!executed && beer_vulkan_device_lost()) {
        static int captured_device_loss;
        if (!captured_device_loss) {
            captured_device_loss = 1;
            const char *directory = getenv("BEER_VULKAN_DEVICE_LOSS_DIR");
            if (!directory || !*directory) directory = "/tmp";
            char path[512];
            snprintf(path, sizeof(path), "%s/beer-device-loss-%016llx.spv",
                     directory, (unsigned long long)shader->shader_hash);
            FILE *file = fopen(path, "wb");
            if (file) {
                fwrite(shader->spirv, 1, shader->spirv_size, file);
                fclose(file);
            }
            snprintf(path, sizeof(path), "%s/beer-device-loss-%016llx.dxbc",
                     directory, (unsigned long long)shader->shader_hash);
            file = fopen(path, "wb");
            if (file) {
                fwrite(dispatch->shader, 1, dispatch->shader_size, file);
                fclose(file);
            }
            fprintf(stderr,
                    "[D3D12 DEVICE LOSS] submission=%llu list=%u sequence=%llu "
                    "shader=%016llx groups=%ux%ux%u descriptors=%u\n",
                    (unsigned long long)dispatch->debug_submission,
                    dispatch->debug_list_index,
                    (unsigned long long)dispatch->debug_sequence,
                    (unsigned long long)shader->shader_hash,
                    dispatch->group_count_x, dispatch->group_count_y,
                    dispatch->group_count_z, shader->descriptor_count);
            for (uint32_t d = 0; d < shader->descriptor_count; ++d) {
                const BeerD3D12ShaderDescriptor *descriptor =
                    &shader->descriptors[d];
                const BeerVulkanComputeBinding *binding = &bindings[d];
                fprintf(stderr,
                        "[D3D12 DEVICE LOSS BINDING] b=%u type=%u resource-type=%u "
                        "reg=%u space=%u kind=%u writable=%u resource=%p "
                        "bytes=%zu format=%u image=%ux%ux%u layers=%u mips=%u "
                        "row=%u serial=%llu\n",
                        descriptor->binding, descriptor->type,
                        descriptor->resource_type, descriptor->register_index,
                        descriptor->register_space, binding->kind,
                        binding->writable, binding->resource, binding->size,
                        binding->image_format, binding->image_width,
                        binding->image_height, binding->image_depth,
                        binding->image_layers, binding->image_mip_levels,
                        binding->image_row_pitch,
                        (unsigned long long)(binding->serial_location ?
                            *binding->serial_location : binding->serial));
                /* Preserve small non-image inputs, especially constant buffers
                 * that control translated loop bounds. Avoid dumping texture
                 * payloads or unbounded guest resources. */
                if (binding->kind == BEER_VULKAN_COMPUTE_BINDING_BUFFER &&
                    binding->data && binding->size &&
                    binding->size <= 64u * 1024u) {
                    snprintf(path, sizeof(path),
                             "%s/beer-device-loss-%016llx-binding-%u.bin",
                             directory, (unsigned long long)shader->shader_hash,
                             descriptor->binding);
                    file = fopen(path, "wb");
                    if (file) {
                        fwrite(binding->data, 1, binding->size, file);
                        fclose(file);
                    }
                }
            }
        }
    }
    if (executed && getenv("BEER_D3D12_TRACE_TEMPORAL_INPUTS")) {
        const char *minimum_text =
            getenv("BEER_D3D12_TITLE_TRACE_MIN_SUBMISSION");
        uint64_t minimum_submission = minimum_text ?
            strtoull(minimum_text, NULL, 10) : 0;
        int title_sized_output = 0;
        for (uint32_t d = 0; d < shader->descriptor_count; ++d) {
            const BeerVulkanComputeBinding *binding = &bindings[d];
            if (binding->writable &&
                binding->kind == BEER_VULKAN_COMPUTE_BINDING_IMAGE &&
                binding->image_width >= 900 && binding->image_height >= 1000) {
                title_sized_output = 1;
                break;
            }
        }
        static unsigned title_dispatch_traces;
        if (title_sized_output &&
            dispatch->debug_submission >= minimum_submission &&
            title_dispatch_traces++ < 512) {
            fprintf(stderr,
                    "[D3D12 TITLE COMPUTE] submission=%llu list=%u "
                    "sequence=%llu shader=%016llx groups=%ux%ux%u\n",
                    (unsigned long long)dispatch->debug_submission,
                    dispatch->debug_list_index,
                    (unsigned long long)dispatch->debug_sequence,
                    (unsigned long long)shader->shader_hash,
                    dispatch->group_count_x, dispatch->group_count_y,
                    dispatch->group_count_z);
            for (uint32_t d = 0; d < shader->descriptor_count; ++d) {
                const BeerVulkanComputeBinding *binding = &bindings[d];
                if (!binding->data || !binding->size) continue;
                size_t nonzero = 0;
                uint64_t hash = 0;
                sampled_fingerprint(binding->data, binding->size,
                                    &nonzero, &hash);
                fprintf(stderr,
                        "[D3D12 TITLE COMPUTE BINDING] binding=%u kind=%u "
                        "writable=%u resource=%p serial=%llu format=%u "
                        "bytes=%zu nonzero=%zu hash=%016llx size=%ux%u\n",
                        binding->binding, binding->kind, binding->writable,
                        binding->resource,
                        (unsigned long long)(binding->serial_location ?
                            *binding->serial_location : binding->serial),
                        binding->image_format, binding->size, nonzero,
                        (unsigned long long)hash, binding->image_width,
                        binding->image_height);
            }
        }
    }
    if (executed && getenv("BEER_D3D12_TARGET_DIAGNOSTICS")) {
        static unsigned output_traces;
        for (uint32_t d = 0;
             d < shader->descriptor_count && output_traces < 256; ++d) {
            const BeerVulkanComputeBinding *binding = &bindings[d];
            if (!binding->writable || !binding->data || !binding->size)
                continue;
            uint64_t hash = UINT64_C(1469598103934665603);
            size_t nonzero = 0;
            for (size_t i = 0; i < binding->size; ++i) {
                nonzero += binding->data[i] != 0;
                hash = (hash ^ binding->data[i]) * UINT64_C(1099511628211);
            }
            fprintf(stderr,
                "[D3D12 COMPUTE TARGET] shader=%016llx binding=%u "
                "kind=%u bytes=%zu nonzero=%zu hash=%016llx size=%ux%u "
                "layers=%u depth=%u\n",
                (unsigned long long)shader->shader_hash, binding->binding,
                binding->kind, binding->size, nonzero,
                (unsigned long long)hash, binding->image_width,
                binding->image_height, binding->image_layers,
                binding->image_depth);
            ++output_traces;
        }
    }
    return executed;
}

int beer_d3d12_execute_vulkan_compute(
    const BeerD3D12ComputeDispatch *dispatch,
    const BeerD3D12TranslatedShader *shader, uint32_t hazards_before,
    const char **reason)
{
    static const char *unused_reason;
    if (!reason) reason = &unused_reason;
    *reason = "invalid arguments";

    PreparedComputeBindings prepared;
    if (!prepare_compute_bindings(dispatch, shader, &prepared, reason))
        return 0;
    return execute_prepared_compute(dispatch, shader, hazards_before,
                                    &prepared, reason);
}

typedef struct PreparedComputeOperation {
    const BeerD3D12ComputeDispatch *dispatch;
    const BeerD3D12TranslatedShader *shader;
    uint32_t hazards_before;
    const char *reason;
    PreparedComputeBindings bindings;
    uint8_t bindings_prepared;
} PreparedComputeOperation;

void beer_d3d12_execute_vulkan_compute_batch(
    const BeerD3D12ComputeCommandBatch *batch, uint8_t *results)
{
    if (!batch || !batch->commands || !results) return;

    memset(results, 0, batch->command_count);

    /* A title transition can submit thousands of small compute batches. The
     * old bridge performed five independent calloc/free pairs for each batch,
     * adding allocator contention without contributing to Vulkan execution.
     * Keep all temporary planning/preparation arrays in one zeroed arena. The
     * arena remains batch-local, so peak memory and lifetime are unchanged. */
    size_t command_storage = 0;
    size_t access_storage = 0;
    const size_t per_command_storage = sizeof(PreparedComputeOperation) +
        sizeof(BeerVulkanComputeOperation) + sizeof(size_t) + sizeof(uint32_t);
    if ((batch->command_count &&
         batch->command_count > SIZE_MAX / per_command_storage) ||
        (batch->access_count &&
         batch->access_count > SIZE_MAX / sizeof(size_t))) {
        if (getenv("BEER_D3D12_DIAGNOSTICS"))
            fprintf(stderr, "[D3D12 COMPUTE IR] batch scratch size overflow\n");
        return;
    }
    command_storage = (size_t)batch->command_count * per_command_storage;
    access_storage = (size_t)batch->access_count * sizeof(size_t);
    if (command_storage > SIZE_MAX - access_storage) {
        if (getenv("BEER_D3D12_DIAGNOSTICS"))
            fprintf(stderr, "[D3D12 COMPUTE IR] batch scratch size overflow\n");
        return;
    }
    uint8_t *scratch = calloc(1, command_storage + access_storage + 1u);
    if (!scratch) {
        if (getenv("BEER_D3D12_DIAGNOSTICS"))
            fprintf(stderr, "[D3D12 COMPUTE IR] batch scratch allocation failed\n");
        return;
    }
    uint8_t *cursor = scratch;
    PreparedComputeOperation *prepared =
        (PreparedComputeOperation *)cursor;
    cursor += (size_t)batch->command_count * sizeof(*prepared);
    BeerVulkanComputeOperation *epoch_operations =
        (BeerVulkanComputeOperation *)cursor;
    cursor += (size_t)batch->command_count * sizeof(*epoch_operations);
    size_t *epoch_staging = (size_t *)cursor;
    cursor += (size_t)batch->command_count * sizeof(*epoch_staging);
    size_t *access_offsets = (size_t *)cursor;
    cursor += (size_t)batch->access_count * sizeof(*access_offsets);
    uint32_t *command_epochs = (uint32_t *)cursor;

    uint32_t executed_count = 0;
    uint32_t rejected_count = 0;
    uint32_t hazard_count = 0;
    uint32_t epoch_count = 0;
    if (!beer_d3d12_compute_plan_epochs(
            batch->commands, batch->command_count, command_epochs,
            &epoch_count)) {
        free(scratch);
        if (getenv("BEER_D3D12_DIAGNOSTICS"))
            fprintf(stderr, "[D3D12 COMPUTE IR] epoch plan failed\n");
        return;
    }

    /* Build one canonical staging layout for every dependency epoch. The
     * offsets remain planning metadata until shared recording is activated;
     * scalar execution continues using its proven per-dispatch layout. */
    const size_t root_constant_allowance =
        (size_t)BEER_D3D12_MAX_ROOT_PARAMETERS *
        BEER_D3D12_MAX_ROOT_CONSTANTS * sizeof(uint32_t);
    if (!beer_d3d12_compute_plan_epoch_staging(
            batch, command_epochs, epoch_count, 256u,
            root_constant_allowance, access_offsets, epoch_staging)) {
        free(scratch);
        if (getenv("BEER_D3D12_DIAGNOSTICS"))
            fprintf(stderr, "[D3D12 COMPUTE IR] epoch staging plan failed\n");
        return;
    }

    /* Prepare stable operation records before entering Vulkan execution.
     * These records are the backend-owned seam consumed by both today's
     * scalar adapter and the upcoming shared epoch recorder. No frontend array
     * needs to be revisited to recover dispatch identity, translated shader,
     * hazard state, or a persistent rejection reason. Resource bytes are
     * deliberately not snapshotted here; dependent epochs still publish GPU
     * writes before the next epoch resolves its bindings. */
    for (uint32_t index = 0; index < batch->command_count; ++index) {
        const BeerD3D12ComputeCommand *command = &batch->commands[index];
        PreparedComputeOperation *operation = &prepared[index];
        operation->dispatch = command->dispatch;
        operation->hazards_before = command->hazards_before;
        hazard_count += operation->hazards_before != 0;
        if (!operation->dispatch || !operation->dispatch->shader ||
            !operation->dispatch->shader_size) {
            operation->reason = "compute dispatch unavailable";
            continue;
        }
        operation->shader = beer_d3d12_translate_shader(
            operation->dispatch->shader, operation->dispatch->shader_size,
            operation->dispatch->shader_hash);
        if (!operation->shader) {
            operation->reason = beer_d3d12_shader_translation_error();
            continue;
        }
        operation->bindings_prepared = (uint8_t)prepare_compute_bindings(
            operation->dispatch, operation->shader, &operation->bindings,
            &operation->reason);
    }

    /* Execute one dependency epoch at a time. This remains the conservative
     * scalar Vulkan adapter, but it now consumes the operation array from the
     * batch scratch arena. Descriptor/transient lifetime, staging reservation,
     * and host publication end at the shared-recorder boundary. */

    uint32_t next_command = 0;
    for (uint32_t epoch = 0;
         epoch < epoch_count && next_command < batch->command_count; ++epoch) {
        if (!beer_vulkan_compute_begin_batch_with_staging(
                epoch_staging[epoch])) {
            if (getenv("BEER_D3D12_DIAGNOSTICS"))
                fprintf(stderr,
                        "[D3D12 COMPUTE IR] epoch=%u staging reservation "
                        "failed bytes=%zu\n", epoch, epoch_staging[epoch]);
            rejected_count += batch->command_count - next_command;
            break;
        }

        uint32_t epoch_first = next_command;
        while (next_command < batch->command_count &&
               command_epochs[next_command] == epoch)
            ++next_command;
        uint32_t epoch_operation_count = next_command - epoch_first;
        memset(epoch_operations, 0,
               (size_t)epoch_operation_count * sizeof(*epoch_operations));
        {
            uint32_t executable_count = 0;
            for (uint32_t index = epoch_first; index < next_command; ++index) {
                PreparedComputeOperation *operation = &prepared[index];
                if (!operation->shader || !operation->bindings_prepared)
                    continue;
                BeerVulkanComputeOperation *vulkan_operation =
                    &epoch_operations[executable_count++];
                vulkan_operation->shader_key = operation->shader;
                vulkan_operation->spirv =
                    (const uint32_t *)operation->shader->spirv;
                vulkan_operation->spirv_size = operation->shader->spirv_size;
                vulkan_operation->bindings = operation->bindings.bindings;
                vulkan_operation->binding_count =
                    operation->shader->descriptor_count;
                vulkan_operation->group_count_x =
                    operation->dispatch->group_count_x;
                vulkan_operation->group_count_y =
                    operation->dispatch->group_count_y;
                vulkan_operation->group_count_z =
                    operation->dispatch->group_count_z;
                if (operation->hazards_before & BEER_D3D12_COMPUTE_HAZARD_RAW)
                    vulkan_operation->hazards_before |=
                        BEER_VULKAN_COMPUTE_HAZARD_RAW;
                if (operation->hazards_before & BEER_D3D12_COMPUTE_HAZARD_WAR)
                    vulkan_operation->hazards_before |=
                        BEER_VULKAN_COMPUTE_HAZARD_WAR;
                if (operation->hazards_before & BEER_D3D12_COMPUTE_HAZARD_WAW)
                    vulkan_operation->hazards_before |=
                        BEER_VULKAN_COMPUTE_HAZARD_WAW;
                vulkan_operation->result = &results[index];
                vulkan_operation->reason = &operation->reason;
                vulkan_operation->debug_submission =
                    operation->dispatch->debug_submission;
                vulkan_operation->debug_sequence =
                    operation->dispatch->debug_sequence;
                vulkan_operation->debug_list_index =
                    operation->dispatch->debug_list_index;
                vulkan_operation->shader_hash = operation->shader->shader_hash;
                vulkan_operation->dxbc = operation->dispatch->shader;
                vulkan_operation->dxbc_size = operation->dispatch->shader_size;
                vulkan_operation->root_signature =
                    operation->dispatch->root_signature_blob;
                vulkan_operation->root_signature_size =
                    operation->dispatch->root_signature_blob_size;
            }
            (void)beer_vulkan_compute_execute_epoch(epoch_operations,
                                                     executable_count);

            for (uint32_t index = epoch_first; index < next_command; ++index) {
                PreparedComputeOperation *operation = &prepared[index];
                if (results[index]) {
                    ++executed_count;
                } else {
                    ++rejected_count;
                    if (getenv("BEER_D3D12_DIAGNOSTICS") && operation->dispatch)
                        fprintf(stderr,
                                "[D3D12 COMPUTE IR] submission=%llu list=%u "
                                "sequence=%llu shader=%016llx rejected: %s\n",
                                (unsigned long long)
                                    operation->dispatch->debug_submission,
                                operation->dispatch->debug_list_index,
                                (unsigned long long)
                                    operation->dispatch->debug_sequence,
                                (unsigned long long)
                                    operation->dispatch->shader_hash,
                                operation->reason ? operation->reason :
                                    "unknown");
                }
            }
        }
        beer_vulkan_compute_end_batch();
        if (beer_vulkan_device_lost()) break;
    }

    free(scratch);
    if (getenv("BEER_D3D12_DIAGNOSTICS"))
        fprintf(stderr,
                "[D3D12 COMPUTE IR] commands=%u accesses=%u hazards=%u "
                "epochs=%u executed=%u rejected=%u\n",
                batch->command_count, batch->access_count, hazard_count,
                epoch_count, executed_count, rejected_count);
}
