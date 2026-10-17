#ifndef BEER_D3D12_SHADER_TRANSLATOR_H
#define BEER_D3D12_SHADER_TRANSLATOR_H

#include <stddef.h>
#include <stdint.h>

#define BEER_D3D12_MAX_SHADER_DESCRIPTORS 64

typedef enum BeerD3D12ShaderDescriptorType {
    BEER_D3D12_SHADER_DESCRIPTOR_SRV = 0,
    BEER_D3D12_SHADER_DESCRIPTOR_UAV = 1,
    BEER_D3D12_SHADER_DESCRIPTOR_CBV = 2,
    BEER_D3D12_SHADER_DESCRIPTOR_SAMPLER = 3
} BeerD3D12ShaderDescriptorType;

typedef enum BeerD3D12ShaderResourceType {
    BEER_D3D12_SHADER_RESOURCE_NONE = 0,
    BEER_D3D12_SHADER_RESOURCE_BUFFER = 1,
    BEER_D3D12_SHADER_RESOURCE_TEXTURE_1D = 2,
    BEER_D3D12_SHADER_RESOURCE_TEXTURE_2D = 3,
    BEER_D3D12_SHADER_RESOURCE_TEXTURE_2D_MS = 4,
    BEER_D3D12_SHADER_RESOURCE_TEXTURE_3D = 5,
    BEER_D3D12_SHADER_RESOURCE_TEXTURE_CUBE = 6,
    BEER_D3D12_SHADER_RESOURCE_TEXTURE_1D_ARRAY = 7,
    BEER_D3D12_SHADER_RESOURCE_TEXTURE_2D_ARRAY = 8,
    BEER_D3D12_SHADER_RESOURCE_TEXTURE_2D_MS_ARRAY = 9,
    BEER_D3D12_SHADER_RESOURCE_TEXTURE_CUBE_ARRAY = 10
} BeerD3D12ShaderResourceType;

typedef struct BeerD3D12ShaderDescriptor {
    uint32_t type;
    uint32_t register_space;
    uint32_t register_index;
    uint32_t resource_type;
    uint32_t resource_data_type;
    uint32_t flags;
    uint32_t count;
    uint32_t set;
    uint32_t binding;
} BeerD3D12ShaderDescriptor;

typedef struct BeerD3D12TranslatedShader {
    uint8_t *spirv;
    size_t spirv_size;
    BeerD3D12ShaderDescriptor descriptors[BEER_D3D12_MAX_SHADER_DESCRIPTORS];
    uint32_t descriptor_count;
    uint64_t shader_hash;
} BeerD3D12TranslatedShader;

/* Automatically scans DXBC resource declarations and translates the shader to
 * Vulkan SPIR-V. Each D3D register receives a deterministic descriptor binding
 * in set zero. Results are cached by shader hash and bytecode identity. */
const BeerD3D12TranslatedShader *beer_d3d12_translate_shader(
    const uint8_t *dxbc, size_t dxbc_size, uint64_t shader_hash);

/* Returns a human-readable reason for the most recent translation failure on
 * the calling thread. The returned pointer remains valid until the next call. */
const char *beer_d3d12_shader_translation_error(void);

/* Releases the process-wide translation cache during final teardown/tests. */
void beer_d3d12_shader_translator_destroy(void);

#endif
