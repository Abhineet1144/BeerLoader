#define _GNU_SOURCE
#include "d3d12_shader_translator.h"
#include "d3d12_dxil_translator.h"

#include <dlfcn.h>
#include <glob.h>
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
    const char *candidates[] = {
        override,
        "libvkd3d-shader.so.1",
        "libvkd3d-shader.so",
        NULL
    };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        if (!candidates[i] || !*candidates[i]) continue;
        g_api.library = dlopen(candidates[i], RTLD_NOW | RTLD_LOCAL);
        if (g_api.library) break;
    }
    /* Steam runtimes are a useful fallback, but their versioned platform
     * directories change on every runtime update, so search them instead of
     * baking a version into the loader. Newest match (last in glob order)
     * first. */
    const char *home = getenv("HOME");
    static const char *const runtime_patterns[] = {
        "%s/.local/share/Steam/steamapps/common/SteamLinuxRuntime_*/"
        "*_platform_*/files/lib/x86_64-linux-gnu/libvkd3d-shader.so.1*",
        "%s/.local/share/umu/steamrt*/steamrt*_platform_*/files/lib/"
        "x86_64-linux-gnu/libvkd3d-shader.so.1*",
    };
    for (size_t p = 0; !g_api.library && home && *home &&
         p < sizeof(runtime_patterns) / sizeof(runtime_patterns[0]); ++p) {
        char pattern[4096];
        glob_t found;
        snprintf(pattern, sizeof(pattern), runtime_patterns[p], home);
        if (glob(pattern, 0, NULL, &found) != 0) continue;
        for (size_t i = found.gl_pathc; i-- > 0 && !g_api.library;)
            g_api.library = dlopen(found.gl_pathv[i], RTLD_NOW | RTLD_LOCAL);
        globfree(&found);
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


/* Small-group compute shaders written for lockstep hardware: a group no wider
 * than a subgroup commonly exchanges data through threadgroup memory with
 * atomics and plain loads and no barrier, because on D3D hardware the lanes
 * cannot diverge in time. The translated SPIR-V carries only relaxed atomics
 * and no barriers, so the Vulkan driver is free to reorder or desynchronise
 * those accesses. Bracket every workgroup-memory access of such shaders with a
 * barrier to restore the ordering the source assumed.
 * mode 1: OpMemoryBarrier only; mode 2: subgroup-scope OpControlBarrier. */
#define SPV_MAX_SET 1024
static uint32_t *spirv_sync_workgroup(const uint32_t *in, size_t size,
                                      int mode, size_t *out_size)
{
    const size_t n = size / 4;
    if (n < 6 || in[0] != 0x07230203u) return NULL;
    uint32_t set[SPV_MAX_SET];
    uint32_t set_count = 0;
    uint32_t uint_type = 0, local_size = 0;
    size_t first_function = 0;
    int compute = 0;
    size_t i = 5;
    while (i < n) {
        const uint32_t count = in[i] >> 16, op = in[i] & 0xffff;
        if (!count || i + count > n) return NULL;
        if (op == 15 && in[i + 1] == 5) compute = 1;
        if (op == 16 && in[i + 2] == 17 && count >= 6)
            local_size = in[i + 3] * in[i + 4] * in[i + 5];
        if (op == 21 && in[i + 2] == 32 && in[i + 3] == 0) uint_type = in[i + 1];
        if (op == 54 && !first_function) first_function = i;
        if (op == 59 && in[i + 3] == 4 && set_count < SPV_MAX_SET)
            set[set_count++] = in[i + 2];
        i += count;
    }
    if (!compute || !uint_type || !set_count || !first_function ||
        !local_size || local_size > 32) return NULL;
    uint32_t bound = in[3];
    const uint32_t c_scope = bound++, c_sem = bound++;
    uint32_t *out = malloc(size + 4096 + n * 16);
    if (!out) return NULL;
    memcpy(out, in, 5 * 4);
    size_t o = 5;
    i = 5;
    while (i < n) {
        const uint32_t count = in[i] >> 16, op = in[i] & 0xffff;
        if (i == first_function) {
            out[o++] = (4u << 16) | 43; out[o++] = uint_type;
            out[o++] = c_scope; out[o++] = 3;            /* Subgroup */
            out[o++] = (4u << 16) | 43; out[o++] = uint_type;
            out[o++] = c_sem; out[o++] = 0x108;  /* AcquireRelease|WorkgroupMemory */
        }
        uint32_t pointer = 0;
        int before = 0, after = 0;
        if ((op == 65 || op == 66 || op == 67) && count >= 4) {
            for (uint32_t k = 0; k < set_count; ++k)
                if (set[k] == in[i + 3] && set_count < SPV_MAX_SET) {
                    set[set_count++] = in[i + 2];
                    break;
                }
        }
        if (op == 61 || op == 227 || (op >= 229 && op <= 242)) pointer = in[i + 3];
        else if (op == 62 || op == 228) pointer = in[i + 1];
        int hit = 0;
        for (uint32_t k = 0; pointer && k < set_count; ++k)
            if (set[k] == pointer) { hit = 1; break; }
        if (hit) {
            before = op != 62 && op != 228;
            after = op != 61 && op != 227;
        }
        for (int pass = 0; pass < 2; ++pass) {
            if (pass == 0 && !before) continue;
            if (pass == 1) {
                memcpy(out + o, in + i, count * 4);
                o += count;
                if (!after) continue;
            }
            if (mode == 2) {
                out[o++] = (4u << 16) | 224; out[o++] = c_scope;
                out[o++] = c_scope; out[o++] = c_sem;
            } else {
                out[o++] = (3u << 16) | 225; out[o++] = c_scope;
                out[o++] = c_sem;
            }
        }
        i += count;
    }
    out[3] = bound;
    *out_size = o * 4;
    return out;
}

/* D3D12 shaders rely on IEEE Inf/NaN/signed-zero behaviour (culling math in
 * particular divides by zero and compares against infinities). Vulkan only
 * guarantees that when the module asks for it, otherwise a driver may compile
 * assuming finite values. Request SignedZeroInfNanPreserve for 32-bit floats
 * (BEER_SPV_FLOAT_CONTROLS=0 turns this off). */
static uint32_t *spirv_preserve_inf_nan(const uint32_t *in, size_t size,
                                        size_t *out_size)
{
    const size_t n = size / 4;
    if (n < 6 || in[0] != 0x07230203u) return NULL;
    size_t cap_end = 5, entry_end = 0;
    uint32_t entry_id = 0;
    for (size_t i = 5; i < n; ) {
        const uint32_t count = in[i] >> 16, op = in[i] & 0xffff;
        if (!count || i + count > n) return NULL;
        if (op == 17) {
            if (in[i + 1] == 4466) return NULL;       /* already requested */
            cap_end = i + count;
        } else if (op == 15 && !entry_id) {
            entry_id = in[i + 2];
            entry_end = i + count;
        } else if (op == 54) break;
        i += count;
    }
    if (!entry_id || !entry_end) return NULL;
    /* "SPV_KHR_float_controls" padded to words: 22 chars + NUL = 6 words. */
    static const char ext[] = "SPV_KHR_float_controls";
    const size_t ext_words = (sizeof(ext) + 3) / 4;
    uint32_t *out = malloc(size + 64);
    if (!out) return NULL;
    size_t o = 0;
    memcpy(out, in, cap_end * 4);
    o = cap_end;
    out[o++] = (2u << 16) | 17; out[o++] = 4466;          /* OpCapability */
    out[o++] = (uint32_t)((1 + ext_words) << 16) | 10;    /* OpExtension */
    memset(&out[o], 0, ext_words * 4);
    memcpy(&out[o], ext, sizeof(ext));
    o += ext_words;
    memcpy(out + o, in + cap_end, (entry_end - cap_end) * 4);
    o += entry_end - cap_end;
    out[o++] = (4u << 16) | 16; out[o++] = entry_id;      /* OpExecutionMode */
    out[o++] = 4461; out[o++] = 32;
    memcpy(out + o, in + entry_end, (n - entry_end) * 4);
    o += n - entry_end;
    *out_size = o * 4;
    return out;
}

/* D3D guarantees that identical position math gives bit-identical depth across
 * different vertex shaders (depth-equal passes, occlusion proxies drawn over
 * the geometry they test). Vulkan only does for outputs decorated Invariant;
 * without it the driver may contract mul+add into FMA differently per shader
 * and coplanar depth tests flicker. Decorate the pre-rasterisation Position
 * output (as DXVK does by default). BEER_SPV_INVARIANT_POSITION=0 disables. */
static uint32_t *spirv_invariant_position(const uint32_t *in, size_t size,
                                          size_t *out_size)
{
    const size_t n = size / 4;
    if (n < 6 || in[0] != 0x07230203u) return NULL;
    int pre_raster = 0;
    /* Positions of OpDecorate/OpMemberDecorate BuiltIn Position, and output variable ids. */
    size_t sites[8]; int site_count = 0;
    uint32_t *outputs = calloc(in[3] ? in[3] : 1, 1);   /* id bound: one byte per id */
    if (!outputs) return NULL;
    for (size_t i = 5; i < n; ) {
        const uint32_t count = in[i] >> 16, op = in[i] & 0xffff;
        if (!count || i + count > n) { free(outputs); return NULL; }
        if (op == 15 && count >= 3 && (in[i + 1] == 0 || in[i + 1] == 2 || in[i + 1] == 3))
            pre_raster = 1;                               /* Vertex, TessellationEvaluation, Geometry */
        else if (op == 71 && count >= 4 && in[i + 2] == 18) { free(outputs); return NULL; }   /* already Invariant */
        else if (op == 72 && count >= 5 && in[i + 3] == 18) { free(outputs); return NULL; }
        else if (((op == 71 && count >= 4 && in[i + 2] == 11 && in[i + 3] == 0) ||
                  (op == 72 && count >= 5 && in[i + 3] == 11 && in[i + 4] == 0)) && site_count < 8)
            sites[site_count++] = i;
        else if (op == 59 && count >= 4 && in[i + 3] == 3 && in[i + 2] < in[3])
            ((uint8_t *)outputs)[in[i + 2]] = 1;          /* OpVariable ... Output */
        i += count;
    }
    if (!pre_raster || !site_count) { free(outputs); return NULL; }
    uint32_t *out = malloc(size + (size_t)site_count * 16);
    if (!out) { free(outputs); return NULL; }
    size_t o = 0, from = 0, added = 0;
    for (int k = 0; k < site_count; ++k) {
        const size_t i = sites[k], count = in[i] >> 16;
        memcpy(out + o, in + from, (i + count - from) * 4);
        o += i + count - from;
        from = i + count;
        if ((in[i] & 0xffff) == 71) {
            if (!((uint8_t *)outputs)[in[i + 1]]) continue;   /* gl_in[].Position etc. are inputs */
            out[o++] = (3u << 16) | 71; out[o++] = in[i + 1]; out[o++] = 18;
        } else {
            /* Block member: only vertex shaders, whose per-vertex block is output-only. */
            int vertex = 0;
            for (size_t j = 5; j < n; j += in[j] >> 16)
                if ((in[j] & 0xffff) == 15) { vertex = in[j + 1] == 0; break; }
            if (!vertex) continue;
            out[o++] = (4u << 16) | 72; out[o++] = in[i + 1]; out[o++] = in[i + 2]; out[o++] = 18;
        }
        ++added;
    }
    memcpy(out + o, in + from, (n - from) * 4);
    o += n - from;
    free(outputs);
    if (!added) { free(out); return NULL; }
    *out_size = o * 4;
    return out;
}

static int spirv_invariant_position_enabled(void)
{
    const char *e = getenv("BEER_SPV_INVARIANT_POSITION");
    return !e || atoi(e);
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
    if (beer_d3d12_is_dxil(dxbc, dxbc_size)) {
        uint32_t stage = ~0u;
        TranslationCacheEntry *entry = calloc(1, sizeof(*entry));
        if (!entry || !(entry->dxbc = malloc(dxbc_size))) {
            free(entry);
            set_error("out of memory while caching translated shader");
            return NULL;
        }
        if (!beer_d3d12_dxil_translate(dxbc, dxbc_size, stage, &entry->shader)) {
            free(entry->dxbc);
            free(entry);
            set_error("DXIL to SPIR-V translation failed");
            return NULL;
        }
        memcpy(entry->dxbc, dxbc, dxbc_size);
        entry->dxbc_size = dxbc_size;
        entry->shader.shader_hash = shader_hash;
        if (!getenv("BEER_SPV_FLOAT_CONTROLS") || atoi(getenv("BEER_SPV_FLOAT_CONTROLS"))) {
            size_t fs = 0;
            uint32_t *fc = spirv_preserve_inf_nan((const uint32_t *)entry->shader.spirv,
                                                  entry->shader.spirv_size, &fs);
            if (fc) { free(entry->shader.spirv); entry->shader.spirv = (void *)fc;
                      entry->shader.spirv_size = fs; }
        }
        if (spirv_invariant_position_enabled()) {
            size_t is = 0;
            uint32_t *inv = spirv_invariant_position((const uint32_t *)entry->shader.spirv,
                                                     entry->shader.spirv_size, &is);
            if (inv) { free(entry->shader.spirv); entry->shader.spirv = (void *)inv;
                       entry->shader.spirv_size = is; }
        }
        if (getenv("BEER_DUMP_SHADERS")) {
            char path[512];
            snprintf(path, sizeof(path), "%s/%016llx.spv", getenv("BEER_DUMP_SHADERS"),
                     (unsigned long long)shader_hash);
            FILE *f = fopen(path, "wb");
            if (f) { fwrite(entry->shader.spirv, 1, entry->shader.spirv_size, f); fclose(f); }
        }
        unsigned accel = 0;
        for (uint32_t d = 0; d < entry->shader.descriptor_count; ++d)
            accel += entry->shader.descriptors[d].resource_type == BEER_D3D12_SHADER_RESOURCE_ACCEL;
        fprintf(stderr, "[DXIL] translated shader %016llx descriptors=%u spirv=%zu%s\n",
                (unsigned long long)shader_hash, entry->shader.descriptor_count,
                entry->shader.spirv_size, accel ? " RAY-QUERY" : "");
        entry->next = g_cache;
        g_cache = entry;
        g_error[0] = '\0';
        return &entry->shader;
    }
    if (!load_api()) return NULL;
    {   /* Diagnostic: BEER_DUMP_SHADERS=<dir> writes every distinct DXBC blob. */
        const char *dir = getenv("BEER_DUMP_SHADERS");
        if (dir) {
            char path[512];
            snprintf(path, sizeof(path), "%s/%016llx.dxbc", dir,
                     (unsigned long long)shader_hash);
            FILE *f = fopen(path, "wb");
            if (f) { fwrite(dxbc, 1, dxbc_size, f); fclose(f); }
        }
    }

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
    if (getenv("BEER_DXR_PROBE") && dxbc_size > 36 &&
        !memcmp(dxbc, "DXBC", 4)) {
        uint32_t chunks;
        memcpy(&chunks, (const uint8_t *)dxbc + 28, 4);
        for (uint32_t c = 0; c < chunks && c < 32; ++c) {
            uint32_t off;
            memcpy(&off, (const uint8_t *)dxbc + 32 + c * 4, 4);
            if (off + 4 <= dxbc_size && !memcmp((const uint8_t *)dxbc + off, "DXIL", 4)) {
                fprintf(stderr, "[DXR PROBE] DXIL (SM6) shader hash=%016llx bytes=%zu\n",
                        (unsigned long long)shader_hash, dxbc_size);
                break;
            }
        }
    }
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
    size_t final_size = spirv.size;
    if (!getenv("BEER_SPV_FLOAT_CONTROLS") || atoi(getenv("BEER_SPV_FLOAT_CONTROLS"))) {
        size_t fs = 0;
        uint32_t *fc = spirv_preserve_inf_nan((const uint32_t *)entry->shader.spirv,
                                              spirv.size, &fs);
        if (fc) { free(entry->shader.spirv); entry->shader.spirv = (void *)fc; final_size = fs; }
    }
    if (spirv_invariant_position_enabled()) {
        size_t is = 0;
        uint32_t *inv = spirv_invariant_position((const uint32_t *)entry->shader.spirv,
                                                 final_size, &is);
        if (inv) { free(entry->shader.spirv); entry->shader.spirv = (void *)inv; final_size = is; }
    }
    {
        const char *sync = getenv("BEER_SPV_SYNC");
        size_t patched_size = 0;
        uint32_t *patched = sync ? spirv_sync_workgroup(spirv.code, spirv.size,
                                    atoi(sync), &patched_size) : NULL;
        if (patched) {
            free(entry->shader.spirv);
            entry->shader.spirv = (void *)patched;
            final_size = patched_size;
        }
    }
    {
        const char *dir = getenv("BEER_DUMP_SHADERS");
        if (dir) {
            char path[512];
            snprintf(path, sizeof(path), "%s/%016llx.spv", dir,
                     (unsigned long long)shader_hash);
            FILE *f = fopen(path, "wb");
            if (f) { fwrite(entry->shader.spirv, 1, final_size, f); fclose(f); }
        }
    }
    entry->dxbc_size = dxbc_size;
    entry->shader.spirv_size = final_size;
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
