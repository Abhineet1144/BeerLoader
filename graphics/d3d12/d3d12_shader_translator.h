#ifndef BEER_D3D12_SHADER_TRANSLATOR_H
#define BEER_D3D12_SHADER_TRANSLATOR_H

#include <stddef.h>
#include <stdint.h>

#define BEER_D3D12_MAX_SHADER_DESCRIPTORS 128

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
    BEER_D3D12_SHADER_RESOURCE_TEXTURE_CUBE_ARRAY = 10,
    BEER_D3D12_SHADER_RESOURCE_ACCEL = 11          /* ray tracing acceleration structure */
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
    /* Bindless: the resource is an array indexed into the whole descriptor
     * heap (set 1 of the pipeline layout) instead of owning a set-0 binding.
     * push_word is the push-constant word that carries the heap index of the
     * array's first element; heap_class selects the heap-mirror binding
     * (0 sampled image, 1 storage image, 2 uniform texel, 3 storage texel). */
    uint32_t bindless;
    uint32_t push_word;
    uint32_t heap_class;
    /* A raw or structured buffer bound as a Vulkan storage buffer instead of a texel buffer: shaders that access
     * them in ways texel buffers cannot express (64-bit raw loads and stores) are translated this way. */
    uint32_t ssbo;
} BeerD3D12ShaderDescriptor;

typedef struct BeerD3D12TranslatedShader {
    uint8_t *spirv;
    size_t spirv_size;
    BeerD3D12ShaderDescriptor descriptors[BEER_D3D12_MAX_SHADER_DESCRIPTORS];
    uint32_t descriptor_count;
    uint64_t shader_hash;
    uint64_t root_layout_hash;   /* nonzero: translated for a root signature (beer_d3d12_translate_shader_rs) */
} BeerD3D12TranslatedShader;

/* Root-signature layout for root-signature translation: the root arguments are passed as push constants, laid out
 * as `descriptor_count` 64-bit GPU addresses (root CBV/SRV/UAV, in push_index order) followed by `word_count`
 * 32-bit words (each table's first heap index, root constants, and one word kept zero for direct heap access).
 * Descriptor tables are read straight from the bindless heap mirror (set 1); static samplers stay set-0 bindings. */
#define BEER_ROOT_TABLE 0u
#define BEER_ROOT_CONSTANTS 1u
#define BEER_ROOT_CBV 2u
#define BEER_ROOT_SRV 3u
#define BEER_ROOT_UAV 4u
typedef struct {
    uint32_t type;               /* BEER_D3D12_SHADER_DESCRIPTOR_* (SRV, UAV, CBV, SAMPLER) */
    uint32_t count;              /* UINT32_MAX = unbounded */
    uint32_t base_register, space;
    uint32_t offset;             /* descriptors from the table start (appended ranges resolved) */
} BeerD3D12RootRange;
typedef struct {
    uint32_t type;               /* BEER_ROOT_* */
    uint32_t visibility;         /* D3D12_SHADER_VISIBILITY */
    uint32_t shader_register, space, dword_count;
    const BeerD3D12RootRange *ranges;
    uint32_t range_count;
    uint32_t push_index;         /* descriptor slot (CBV/SRV/UAV) or first word (table, constants) */
} BeerD3D12RootParam;
typedef struct {
    uint32_t param_count;
    BeerD3D12RootParam params[64];
    uint32_t static_count;
    uint32_t static_register[64], static_space[64];
    uint32_t descriptor_count, word_count, zero_word;
    /* Where heap descriptors live. BEER_HEAP_MODEL_CLASSES: set 1, one array per class (bindings 0-5, samplers 5),
     * static samplers in set 0 at translator-assigned bindings. BEER_HEAP_MODEL_MUTABLE: set 0 binding 0 is one
     * mutable-type array for the CBV/SRV/UAV heap, set 1 binding 0 the sampler heap, set 2 binding i static
     * sampler i (immutable). */
    uint32_t heap_model;
    uint64_t hash;               /* of everything above that affects translation */
} BeerD3D12RootLayout;
#define BEER_HEAP_MODEL_CLASSES 0u
#define BEER_HEAP_MODEL_MUTABLE 1u

/* Like beer_d3d12_translate_shader for a DXIL shader, translated for `layout` (see BeerD3D12RootLayout). Returns
 * NULL when the shader cannot use root-signature translation (DXBC, UAV counters, acceleration structures, ...);
 * the caller then uses beer_d3d12_translate_shader. Cached per (shader, layout). */
const BeerD3D12TranslatedShader *beer_d3d12_translate_shader_rs(
    const uint8_t *dxbc, size_t dxbc_size, uint64_t shader_hash, const BeerD3D12RootLayout *layout);

/* Automatically scans DXBC resource declarations and translates the shader to
 * Vulkan SPIR-V. Each D3D register receives a deterministic descriptor binding
 * in set zero. Results are cached by shader hash and bytecode identity. */
void beer_d3d12_translator_stats(void);
const BeerD3D12TranslatedShader *beer_d3d12_translate_shader(
    const uint8_t *dxbc, size_t dxbc_size, uint64_t shader_hash);

/* Returns a human-readable reason for the most recent translation failure on
 * the calling thread. The returned pointer remains valid until the next call. */
const char *beer_d3d12_shader_translation_error(void);

/* Releases the process-wide translation cache during final teardown/tests. */
void beer_d3d12_shader_translator_destroy(void);

#endif
