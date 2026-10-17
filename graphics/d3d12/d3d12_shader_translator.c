#define _GNU_SOURCE
#include "d3d12_shader_translator.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Runtime-compatible subset of vkd3d_shader.h. Beer loads the translator
 * dynamically so it does not acquire a build-time or loader-path dependency on
 * a particular Steam Runtime installation. These layouts are part of the
 * public vkd3d-shader 1.x ABI and are validated by the focused test. */
typedef struct Vkd3dShaderCode {
    const void *code;
    size_t size;
} Vkd3dShaderCode;

typedef struct Vkd3dShaderCompileInfo {
    uint32_t type;
    uint32_t padding;
    const void *next;
    Vkd3dShaderCode source;
    uint32_t source_type;
    uint32_t target_type;
    const void *options;
    uint32_t option_count;
    uint32_t log_level;
    const char *source_name;
} Vkd3dShaderCompileInfo;

typedef struct Vkd3dShaderDescriptorInfo {
    uint32_t type;
    uint32_t register_space;
    uint32_t register_index;
    uint32_t resource_type;
    uint32_t resource_data_type;
    uint32_t flags;
    uint32_t count;
} Vkd3dShaderDescriptorInfo;

typedef struct Vkd3dShaderScanDescriptorInfo {
    uint32_t type;
    uint32_t padding;
    const void *next;
    Vkd3dShaderDescriptorInfo *descriptors;
    uint32_t descriptor_count;
} Vkd3dShaderScanDescriptorInfo;

typedef struct Vkd3dShaderDescriptorBinding {
    uint32_t set;
    uint32_t binding;
    uint32_t count;
} Vkd3dShaderDescriptorBinding;

typedef struct Vkd3dShaderResourceBinding {
    uint32_t type;
    uint32_t register_space;
    uint32_t register_index;
    uint32_t shader_visibility;
    uint32_t flags;
    Vkd3dShaderDescriptorBinding binding;
} Vkd3dShaderResourceBinding;

typedef struct Vkd3dShaderInterfaceInfo {
    uint32_t type;
    uint32_t padding;
    const void *next;
    const Vkd3dShaderResourceBinding *bindings;
    uint32_t binding_count;
    const void *push_constant_buffers;
    uint32_t push_constant_buffer_count;
    const void *combined_samplers;
    uint32_t combined_sampler_count;
    const void *uav_counters;
    uint32_t uav_counter_count;
} Vkd3dShaderInterfaceInfo;

enum {
    VKD3D_STRUCTURE_COMPILE_INFO = 0,
    VKD3D_STRUCTURE_INTERFACE_INFO = 1,
    VKD3D_STRUCTURE_SCAN_DESCRIPTOR_INFO = 2,
    VKD3D_SOURCE_DXBC_TPF = 1,
    VKD3D_TARGET_SPIRV_BINARY = 1,
    VKD3D_LOG_WARNING = 2,
    VKD3D_VISIBILITY_ALL = 0,
    VKD3D_BINDING_BUFFER = 1,
    VKD3D_BINDING_IMAGE = 2
};

typedef int (*Vkd3dCompileFn)(const Vkd3dShaderCompileInfo *,
                              Vkd3dShaderCode *, char **);
typedef int (*Vkd3dScanFn)(const Vkd3dShaderCompileInfo *, char **);
typedef void (*Vkd3dFreeCodeFn)(Vkd3dShaderCode *);
typedef void (*Vkd3dFreeMessagesFn)(char *);
typedef void (*Vkd3dFreeScanFn)(Vkd3dShaderScanDescriptorInfo *);

typedef struct TranslatorApi {
    void *library;
    Vkd3dCompileFn compile;
    Vkd3dScanFn scan;
    Vkd3dFreeCodeFn free_code;
    Vkd3dFreeMessagesFn free_messages;
    Vkd3dFreeScanFn free_scan;
    int attempted;
} TranslatorApi;

typedef struct TranslationCacheEntry {
    BeerD3D12TranslatedShader shader;
    uint8_t *dxbc;
    size_t dxbc_size;
    struct TranslationCacheEntry *next;
} TranslationCacheEntry;

static TranslatorApi g_api;
static TranslationCacheEntry *g_cache;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local char g_error[512];

static void set_error(const char *message)
{
    snprintf(g_error, sizeof(g_error), "%s", message ? message : "unknown error");
}

static void set_compile_error(const char *stage, int result, const char *messages)
{
    snprintf(g_error, sizeof(g_error), "%s failed (%d)%s%s", stage, result,
             messages && *messages ? ": " : "", messages && *messages ? messages : "");
}

static int load_symbol(void **target, const char *name)
{
    *target = dlsym(g_api.library, name);
    return *target != NULL;
}

static int load_api(void)
{
    if (g_api.library) return 1;
    if (g_api.attempted) {
        /* Log once on each re-entry after a failed first attempt. */
        static int logged_fail;
        if (!logged_fail) {
            logged_fail = 1;
            fprintf(stderr,
                    "[SHADER-TRANSLATOR] load_api: already attempted and failed\n");
        }
        return 0;
    }
    g_api.attempted = 1;

    const char *override = getenv("BEER_VKD3D_SHADER_LIBRARY");
    /* Prefer normal system resolution. Steam runtimes are a useful fallback,
     * but their versioned directory changes on every runtime update, so never
     * bake a transient var/tmp path into the loader. */
    const char *home = getenv("HOME");
    char steamrt4[4096] = {0};
    char sniper[4096] = {0};
    if (home && *home) {
        snprintf(steamrt4, sizeof(steamrt4),
                 "%s/.local/share/Steam/steamapps/common/SteamLinuxRuntime_4/"
                 "steamrt4_platform_4.0.20260928.262390/files/lib/"
                 "x86_64-linux-gnu/libvkd3d-shader.so.1.19.0", home);
        snprintf(sniper, sizeof(sniper),
                 "%s/.local/share/Steam/steamapps/common/"
                 "SteamLinuxRuntime_sniper/sniper_platform_3.0.20260805.254768/"
                 "files/lib/x86_64-linux-gnu/libvkd3d-shader.so.1.18.0", home);
    }
    const char *candidates[] = {
        override,
        "libvkd3d-shader.so.1",
        "libvkd3d-shader.so",
        steamrt4,
        sniper,
        NULL
    };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        if (!candidates[i] || !*candidates[i]) continue;
        g_api.library = dlopen(candidates[i], RTLD_NOW | RTLD_LOCAL);
        if (g_api.library) break;
    }
    if (!g_api.library) {
        set_error("libvkd3d-shader.so.1 was not found; set BEER_VKD3D_SHADER_LIBRARY");
        fprintf(stderr,
                "[SHADER-TRANSLATOR] libvkd3d-shader.so.1 not found "
                "(override=%s)\n",
                override ? override : "none");
        return 0;
    }
    fprintf(stderr, "[SHADER-TRANSLATOR] libvkd3d-shader loaded\n");

    if (!load_symbol((void **)&g_api.compile, "vkd3d_shader_compile") ||
        !load_symbol((void **)&g_api.scan, "vkd3d_shader_scan") ||
        !load_symbol((void **)&g_api.free_code, "vkd3d_shader_free_shader_code") ||
        !load_symbol((void **)&g_api.free_messages, "vkd3d_shader_free_messages") ||
        !load_symbol((void **)&g_api.free_scan,
                     "vkd3d_shader_free_scan_descriptor_info")) {
        set_error("libvkd3d-shader lacks the required public ABI");
        dlclose(g_api.library);
        memset(&g_api, 0, sizeof(g_api));
        g_api.attempted = 1;
        return 0;
    }
    return 1;
}

static uint32_t binding_flags(const Vkd3dShaderDescriptorInfo *descriptor)
{
    if (!descriptor) return 0;
    return descriptor->resource_type == BEER_D3D12_SHADER_RESOURCE_BUFFER ?
        VKD3D_BINDING_BUFFER : VKD3D_BINDING_IMAGE;
}

/* Graphics stages are translated independently, but Vulkan requires one
 * descriptor type for a binding across every stage. D3D register namespaces
 * are stage-local and permit, for example, a structured VS t0 and a sampled
 * PS t0 in one pipeline. Descriptor type alone therefore is not a complete
 * semantic key: SRV/UAV buffers and images need distinct Vulkan bindings.
 *
 * Keep six collision-free classes in set zero, each with 8192 registers:
 *
 *   0 SRV buffer, 1 SRV image, 2 UAV buffer, 3 UAV image,
 *   4 CBV,        5 sampler
 *
 * The current Vulkan executors expose one descriptor set, so nonzero D3D
 * register spaces are rejected explicitly instead of being aliased. */
static int dxbc_binding_stage(const uint8_t *dxbc, size_t dxbc_size,
                              uint32_t *stage)
{
    if (!dxbc || dxbc_size < 36 || memcmp(dxbc, "DXBC", 4) != 0 || !stage)
        return 0;
    uint32_t chunk_count = 0;
    memcpy(&chunk_count, dxbc + 28, sizeof(chunk_count));
    if (chunk_count > (dxbc_size - 32) / 4u) return 0;
    for (uint32_t index = 0; index < chunk_count; ++index) {
        uint32_t offset = 0;
        memcpy(&offset, dxbc + 32 + (size_t)index * 4u, sizeof(offset));
        if (offset > dxbc_size || dxbc_size - offset < 12) continue;
        const uint8_t *chunk = dxbc + offset;
        if (memcmp(chunk, "SHDR", 4) != 0 && memcmp(chunk, "SHEX", 4) != 0)
            continue;
        uint32_t token = 0;
        memcpy(&token, chunk + 8, sizeof(token));
        uint32_t program_type = token >> 16;
        /* Keep compute at the historical base. Vertex and pixel resources use
         * independent namespaces because D3D register visibility is stage
         * local and b0/t0 may legally name different resources in each stage. */
        if (program_type == 5) *stage = 0;      /* compute */
        else if (program_type == 1) *stage = 1; /* vertex */
        else if (program_type == 0) *stage = 2; /* pixel */
        else *stage = program_type + 3u;
        return 1;
    }
    return 0;
}

static int semantic_binding(const Vkd3dShaderDescriptorInfo *descriptor,
                            uint32_t stage, uint32_t *binding)
{
    if (!descriptor || !binding || descriptor->type > 3 ||
        descriptor->register_index > UINT32_C(0x1fff) ||
        descriptor->register_space != 0 || stage > UINT32_C(0x3fff))
        return 0;
    uint32_t descriptor_class;
    if (descriptor->type == BEER_D3D12_SHADER_DESCRIPTOR_SRV)
        descriptor_class = descriptor->resource_type ==
            BEER_D3D12_SHADER_RESOURCE_BUFFER ? 0u : 1u;
    else if (descriptor->type == BEER_D3D12_SHADER_DESCRIPTOR_UAV)
        descriptor_class = descriptor->resource_type ==
            BEER_D3D12_SHADER_RESOURCE_BUFFER ? 2u : 3u;
    else if (descriptor->type == BEER_D3D12_SHADER_DESCRIPTOR_CBV)
        descriptor_class = 4u;
    else
        descriptor_class = 5u;
    *binding = stage * UINT32_C(0x10000) +
               descriptor_class * UINT32_C(0x2000) +
               descriptor->register_index;
    return 1;
}

static const BeerD3D12TranslatedShader *translate_locked(
    const uint8_t *dxbc, size_t dxbc_size, uint64_t shader_hash)
{
    for (TranslationCacheEntry *entry = g_cache; entry; entry = entry->next) {
        if (entry->shader.shader_hash == shader_hash &&
            entry->dxbc_size == dxbc_size &&
            memcmp(entry->dxbc, dxbc, dxbc_size) == 0)
            return &entry->shader;
    }
    if (!load_api()) return NULL;

    Vkd3dShaderScanDescriptorInfo scan = {
        .type = VKD3D_STRUCTURE_SCAN_DESCRIPTOR_INFO
    };
    Vkd3dShaderCompileInfo scan_info = {
        .type = VKD3D_STRUCTURE_COMPILE_INFO,
        .next = &scan,
        .source = {dxbc, dxbc_size},
        .source_type = VKD3D_SOURCE_DXBC_TPF,
        .target_type = VKD3D_TARGET_SPIRV_BINARY,
        .log_level = VKD3D_LOG_WARNING,
        .source_name = "Beer D3D12 shader"
    };
    char *messages = NULL;
    int result = g_api.scan(&scan_info, &messages);
    if (result < 0) {
        set_compile_error("vkd3d shader scan", result, messages);
        if (messages) g_api.free_messages(messages);
        g_api.free_scan(&scan);
        return NULL;
    }
    if (messages) g_api.free_messages(messages);
    messages = NULL;
    if (scan.descriptor_count > BEER_D3D12_MAX_SHADER_DESCRIPTORS) {
        set_error("shader declares more descriptors than Beer can map");
        g_api.free_scan(&scan);
        return NULL;
    }

    uint32_t binding_stage = 0;
    if (!dxbc_binding_stage(dxbc, dxbc_size, &binding_stage)) {
        set_error("DXBC shader stage is unavailable");
        g_api.free_scan(&scan);
        return NULL;
    }
    Vkd3dShaderResourceBinding mappings[BEER_D3D12_MAX_SHADER_DESCRIPTORS];
    memset(mappings, 0, sizeof(mappings));
    for (uint32_t i = 0; i < scan.descriptor_count; ++i) {
        const Vkd3dShaderDescriptorInfo *source = &scan.descriptors[i];
        mappings[i].type = source->type;
        mappings[i].register_space = source->register_space;
        mappings[i].register_index = source->register_index;
        mappings[i].shader_visibility = VKD3D_VISIBILITY_ALL;
        mappings[i].flags = binding_flags(source);
        mappings[i].binding.set = 0;
        if (!semantic_binding(source, binding_stage,
                              &mappings[i].binding.binding)) {
            set_error("shader descriptor register is outside Beer's Vulkan binding namespace");
            g_api.free_scan(&scan);
            return NULL;
        }
        mappings[i].binding.count = source->count ? source->count : 1;
    }
    Vkd3dShaderInterfaceInfo interface_info = {
        .type = VKD3D_STRUCTURE_INTERFACE_INFO,
        .bindings = mappings,
        .binding_count = scan.descriptor_count
    };
    Vkd3dShaderCompileInfo compile_info = scan_info;
    compile_info.next = &interface_info;
    Vkd3dShaderCode spirv = {0};
    result = g_api.compile(&compile_info, &spirv, &messages);
    if (result < 0 || !spirv.code || !spirv.size || (spirv.size & 3u)) {
        set_compile_error("DXBC to SPIR-V translation", result, messages);
        if (messages) g_api.free_messages(messages);
        if (spirv.code) g_api.free_code(&spirv);
        g_api.free_scan(&scan);
        return NULL;
    }
    if (messages) g_api.free_messages(messages);

    TranslationCacheEntry *entry = calloc(1, sizeof(*entry));
    if (!entry || !(entry->dxbc = malloc(dxbc_size)) ||
        !(entry->shader.spirv = malloc(spirv.size))) {
        set_error("out of memory while caching translated shader");
        if (entry) {
            free(entry->shader.spirv);
            free(entry->dxbc);
        }
        free(entry);
        g_api.free_code(&spirv);
        g_api.free_scan(&scan);
        return NULL;
    }
    memcpy(entry->dxbc, dxbc, dxbc_size);
    memcpy(entry->shader.spirv, spirv.code, spirv.size);
    entry->dxbc_size = dxbc_size;
    entry->shader.spirv_size = spirv.size;
    entry->shader.shader_hash = shader_hash;
    entry->shader.descriptor_count = scan.descriptor_count;
    for (uint32_t i = 0; i < scan.descriptor_count; ++i) {
        BeerD3D12ShaderDescriptor *destination = &entry->shader.descriptors[i];
        const Vkd3dShaderDescriptorInfo *source = &scan.descriptors[i];
        destination->type = source->type;
        destination->register_space = source->register_space;
        destination->register_index = source->register_index;
        destination->resource_type = source->resource_type;
        destination->resource_data_type = source->resource_data_type;
        destination->flags = source->flags;
        destination->count = source->count ? source->count : 1;
        destination->set = 0;
        /* Already validated while constructing the compiler interface. */
        (void)semantic_binding(source, binding_stage,
                               &destination->binding);
    }
    entry->next = g_cache;
    g_cache = entry;
    g_api.free_code(&spirv);
    g_api.free_scan(&scan);
    g_error[0] = '\0';
    if (getenv("BEER_D3D12_DIAGNOSTICS"))
        fprintf(stderr,
                "[SHADER-TRANSLATOR] translated hash=%016llx stage=%u "
                "descriptors=%u spirv=%zu bytes\n",
                (unsigned long long)shader_hash, binding_stage,
                entry->shader.descriptor_count, entry->shader.spirv_size);
    return &entry->shader;
}

const BeerD3D12TranslatedShader *beer_d3d12_translate_shader(
    const uint8_t *dxbc, size_t dxbc_size, uint64_t shader_hash)
{
    if (!dxbc || dxbc_size < 4 || memcmp(dxbc, "DXBC", 4) != 0) {
        set_error("shader is not a valid DXBC container");
        return NULL;
    }
    pthread_mutex_lock(&g_lock);
    const BeerD3D12TranslatedShader *result =
        translate_locked(dxbc, dxbc_size, shader_hash);
    pthread_mutex_unlock(&g_lock);
    return result;
}

const char *beer_d3d12_shader_translation_error(void)
{
    return g_error[0] ? g_error : "no translation error";
}

void beer_d3d12_shader_translator_destroy(void)
{
    pthread_mutex_lock(&g_lock);
    while (g_cache) {
        TranslationCacheEntry *next = g_cache->next;
        free(g_cache->shader.spirv);
        free(g_cache->dxbc);
        free(g_cache);
        g_cache = next;
    }
    if (g_api.library) dlclose(g_api.library);
    memset(&g_api, 0, sizeof(g_api));
    pthread_mutex_unlock(&g_lock);
}
