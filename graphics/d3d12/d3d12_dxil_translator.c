#define _GNU_SOURCE
#include "d3d12_dxil_translator.h"

#include <dlfcn.h>
#include <libgen.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "dxil_spirv_c.h"

/* DXIL (shader model 6) -> SPIR-V through dxil-spirv, loaded at run time from
 * third_party/dxil-spirv/build (built by `make dxil-spirv`). Resources are
 * reported through the converter's remapping callbacks and mapped onto the
 * same set-0 binding model the DXBC path uses, so the BD12 pipeline code does
 * not care which translator produced a shader: raw and structured buffers
 * become texel buffers, CBVs uniform buffers, and each D3D register gets its
 * own binding inside the stage's namespace (stage * 0x10000 + n). */

#define LOAD(name) do { \
        g.name = (void *)dlsym(g.library, "dxil_spv_" #name); \
        if (!g.name) return 0; } while (0)

static struct {
    void *library;
    int attempted;
    dxil_spv_result (*parse_dxil_blob)(const void *, size_t, dxil_spv_parsed_blob *);
    void (*parsed_blob_free)(dxil_spv_parsed_blob);
    dxil_spv_shader_stage (*parsed_blob_get_shader_stage)(dxil_spv_parsed_blob);
    dxil_spv_result (*create_converter)(dxil_spv_parsed_blob, dxil_spv_converter *);
    void (*converter_free)(dxil_spv_converter);
    dxil_spv_result (*parsed_blob_get_num_entry_points)(dxil_spv_parsed_blob, unsigned *);
    dxil_spv_result (*parsed_blob_get_entry_point_name)(dxil_spv_parsed_blob, unsigned, const char **);
    dxil_spv_result (*parsed_blob_get_entry_point_demangled_name)(dxil_spv_parsed_blob, unsigned, const char **);
    dxil_spv_shader_stage (*parsed_blob_get_shader_stage_for_entry)(dxil_spv_parsed_blob, const char *);
    void (*converter_set_entry_point)(dxil_spv_converter, const char *);
    void (*converter_set_root_constant_word_count)(dxil_spv_converter, unsigned);
    void (*converter_set_srv_remapper)(dxil_spv_converter, dxil_spv_srv_remapper_cb, void *);
    void (*converter_set_sampler_remapper)(dxil_spv_converter, dxil_spv_sampler_remapper_cb, void *);
    void (*converter_set_uav_remapper)(dxil_spv_converter, dxil_spv_uav_remapper_cb, void *);
    void (*converter_set_cbv_remapper)(dxil_spv_converter, dxil_spv_cbv_remapper_cb, void *);
    dxil_spv_result (*converter_add_option)(dxil_spv_converter, const dxil_spv_option_base *);
    dxil_spv_result (*converter_run)(dxil_spv_converter);
    dxil_spv_result (*converter_get_compiled_spirv)(dxil_spv_converter, dxil_spv_compiled_spirv *);
    void (*begin_thread_allocator_context)(void);
    void (*end_thread_allocator_context)(void);
    void (*set_thread_log_callback)(dxil_spv_log_cb, void *);
} g;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static int load_symbols(void)
{
    LOAD(parse_dxil_blob);
    LOAD(parsed_blob_free);
    LOAD(parsed_blob_get_shader_stage);
    LOAD(create_converter);
    LOAD(converter_free);
    LOAD(parsed_blob_get_num_entry_points);
    LOAD(parsed_blob_get_entry_point_name);
    LOAD(parsed_blob_get_entry_point_demangled_name);
    LOAD(parsed_blob_get_shader_stage_for_entry);
    LOAD(converter_set_entry_point);
    LOAD(converter_set_root_constant_word_count);
    LOAD(converter_set_srv_remapper);
    LOAD(converter_set_sampler_remapper);
    LOAD(converter_set_uav_remapper);
    LOAD(converter_set_cbv_remapper);
    LOAD(converter_add_option);
    LOAD(converter_run);
    LOAD(converter_get_compiled_spirv);
    LOAD(begin_thread_allocator_context);
    LOAD(end_thread_allocator_context);
    LOAD(set_thread_log_callback);
    return 1;
}

static int load_library(void)
{
    if (g.library) return 1;
    if (g.attempted) return 0;
    g.attempted = 1;
    char exe[PATH_MAX], relative[PATH_MAX + 64] = "";
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0) {
        exe[n] = '\0';
        snprintf(relative, sizeof(relative),
                 "%s/third_party/dxil-spirv/build/libdxil-spirv-c-shared.so",
                 dirname(exe));
    }
    const char *candidates[] = {
        getenv("BEER_DXIL_SPIRV_LIBRARY"),
        relative,
        "third_party/dxil-spirv/build/libdxil-spirv-c-shared.so",
        "libdxil-spirv-c-shared.so.2",
        "libdxil-spirv-c-shared.so",
        NULL
    };
    for (size_t i = 0; candidates[i] || i < 5; ++i) {
        if (!candidates[i] || !*candidates[i]) continue;
        g.library = dlopen(candidates[i], RTLD_NOW | RTLD_LOCAL);
        if (g.library) break;
    }
    if (!g.library || !load_symbols()) {
        fprintf(stderr, "[DXIL] dxil-spirv unavailable (run `make dxil-spirv` or set "
                        "BEER_DXIL_SPIRV_LIBRARY)\n");
        if (g.library) dlclose(g.library);
        g.library = NULL;
        return 0;
    }
    fprintf(stderr, "[DXIL] dxil-spirv loaded\n");
    return 1;
}

int beer_d3d12_is_dxil(const uint8_t *dxbc, size_t size)
{
    if (!dxbc || size < 36 || memcmp(dxbc, "DXBC", 4) != 0) return 0;
    uint32_t chunks;
    memcpy(&chunks, dxbc + 28, 4);
    for (uint32_t c = 0; c < chunks && c < 64; ++c) {
        uint32_t off;
        memcpy(&off, dxbc + 32 + (size_t)c * 4, 4);
        if (off + 4 <= size && memcmp(dxbc + off, "DXIL", 4) == 0) return 1;
    }
    return 0;
}

/* Collects descriptors while the converter queries each resource. */
typedef struct {
    BeerD3D12TranslatedShader *shader;
    uint32_t stage;              /* BD12 binding namespace */
    uint32_t next;
    uint32_t bindless;           /* bindless arrays so far */
    int failed;
    uint32_t words_per_stage;    /* push words available to this collection (RT pipelines share a wider block) */
    int accel_ok;                /* ray tracing acceleration structures are allowed */
} Collect;

#define PUSH_WORDS_PER_STAGE 4u

static uint32_t resource_type(dxil_spv_resource_kind kind)
{
    switch (kind) {
    case DXIL_SPV_RESOURCE_KIND_TEXTURE_1D: return BEER_D3D12_SHADER_RESOURCE_TEXTURE_1D;
    case DXIL_SPV_RESOURCE_KIND_TEXTURE_2D: return BEER_D3D12_SHADER_RESOURCE_TEXTURE_2D;
    case DXIL_SPV_RESOURCE_KIND_TEXTURE_2DMS: return BEER_D3D12_SHADER_RESOURCE_TEXTURE_2D_MS;
    case DXIL_SPV_RESOURCE_KIND_TEXTURE_3D: return BEER_D3D12_SHADER_RESOURCE_TEXTURE_3D;
    case DXIL_SPV_RESOURCE_KIND_TEXTURE_CUBE: return BEER_D3D12_SHADER_RESOURCE_TEXTURE_CUBE;
    case DXIL_SPV_RESOURCE_KIND_TEXTURE_1D_ARRAY: return BEER_D3D12_SHADER_RESOURCE_TEXTURE_1D_ARRAY;
    case DXIL_SPV_RESOURCE_KIND_TEXTURE_2D_ARRAY: return BEER_D3D12_SHADER_RESOURCE_TEXTURE_2D_ARRAY;
    case DXIL_SPV_RESOURCE_KIND_TEXTURE_2D_MS_ARRAY: return BEER_D3D12_SHADER_RESOURCE_TEXTURE_2D_MS_ARRAY;
    case DXIL_SPV_RESOURCE_KIND_TEXTURE_CUBE_ARRAY: return BEER_D3D12_SHADER_RESOURCE_TEXTURE_CUBE_ARRAY;
    case DXIL_SPV_RESOURCE_KIND_RT_ACCELERATION_STRUCTURE: return BEER_D3D12_SHADER_RESOURCE_ACCEL;
    case DXIL_SPV_RESOURCE_KIND_TYPED_BUFFER:
    case DXIL_SPV_RESOURCE_KIND_RAW_BUFFER:
    case DXIL_SPV_RESOURCE_KIND_STRUCTURED_BUFFER: return BEER_D3D12_SHADER_RESOURCE_BUFFER;
    default: return BEER_D3D12_SHADER_RESOURCE_NONE;
    }
}

/* Returns the binding assigned to (type, space, register); allocates one the
 * first time a register is seen. */
static uint32_t assign(Collect *c, uint32_t type, const dxil_spv_d3d_binding *d)
{
    BeerD3D12TranslatedShader *s = c->shader;
    for (uint32_t i = 0; i < s->descriptor_count; ++i) {
        const BeerD3D12ShaderDescriptor *e = &s->descriptors[i];
        if (e->type == type && e->register_space == d->register_space &&
            e->register_index == d->register_index)
            return e->binding;
    }
    if (s->descriptor_count == BEER_D3D12_MAX_SHADER_DESCRIPTORS) {
        c->failed = 1;
        return 0;
    }
    if (getenv("BEER_DXIL_VERBOSE") && d->range_size != 1)
        fprintf(stderr, "[DXIL] array resource class=%u kind=%u space=%u reg=%u range=%u\n",
                type, (unsigned)d->kind, d->register_space, d->register_index, d->range_size);
    BeerD3D12ShaderDescriptor *e = &s->descriptors[s->descriptor_count++];
    *e = (BeerD3D12ShaderDescriptor){
        .type = type,
        .register_space = d->register_space,
        .register_index = d->register_index,
        .resource_type = resource_type(d->kind),
        /* Unbounded ranges (range_size 0 or ~0) are exposed as a bounded array. */
        .count = d->range_size == 0 || d->range_size > 256 ? 256 : d->range_size,
        .set = 0,
        /* Offset past the heap mirror's own bindings (0..3 of set 1) so a
         * source binding can never be confused with them during renumbering. */
        .binding = c->stage * UINT32_C(0x10000) + 0x100u + c->next++
    };
    if (d->range_size == 1) e->count = 1;
    return e->binding;
}

/* Unbounded (or huge) SRV/UAV arrays cannot be resolved per draw: index the
 * whole heap through the mirror (set 1) instead. */
static int wants_bindless(const dxil_spv_d3d_binding *d, uint32_t type)
{
    if (type != BEER_D3D12_SHADER_DESCRIPTOR_SRV &&
        type != BEER_D3D12_SHADER_DESCRIPTOR_UAV) return 0;
    if (d->kind == DXIL_SPV_RESOURCE_KIND_SAMPLER ||
        d->kind == DXIL_SPV_RESOURCE_KIND_CONSTANT_BUFFER ||
        d->kind == DXIL_SPV_RESOURCE_KIND_RT_ACCELERATION_STRUCTURE) return 0;
    return d->range_size == 0 || d->range_size > 64;
}

static int bindless_class(const dxil_spv_d3d_binding *d, uint32_t type)
{
    const int buffer = d->kind == DXIL_SPV_RESOURCE_KIND_TYPED_BUFFER ||
                       d->kind == DXIL_SPV_RESOURCE_KIND_RAW_BUFFER ||
                       d->kind == DXIL_SPV_RESOURCE_KIND_STRUCTURED_BUFFER;
    return (buffer ? 2 : 0) + (type == BEER_D3D12_SHADER_DESCRIPTOR_UAV ? 1 : 0);
}

/* Registers a bindless array and fills its Vulkan binding. */
/* Heap-mirror classes beyond the SRV/UAV arrays: uniform buffers (set 1 binding 4) and the sampler heap
 * (set 1 binding 5). SM 6.6 ResourceDescriptorHeap / SamplerDescriptorHeap access arrives with all-ones
 * register fields; the index then comes straight from the shader. */
#define HEAP_CLASS_CBV 4u
#define HEAP_CLASS_SAMPLER 5u

static int is_heap_access(const dxil_spv_d3d_binding *d)
{
    return d->register_space == UINT32_MAX && d->register_index == UINT32_MAX;
}

static int assign_bindless_class(Collect *c, uint32_t type, const dxil_spv_d3d_binding *d,
                                 dxil_spv_vulkan_binding *out, uint32_t heap_class);
static int assign_bindless(Collect *c, uint32_t type, const dxil_spv_d3d_binding *d,
                           dxil_spv_vulkan_binding *out)
{
    return assign_bindless_class(c, type, d, out, (uint32_t)bindless_class(d, type));
}

static int assign_bindless_class(Collect *c, uint32_t type, const dxil_spv_d3d_binding *d,
                                 dxil_spv_vulkan_binding *out, uint32_t heap_class)
{
    BeerD3D12TranslatedShader *s = c->shader;
    /* The same bindless array seen again (another entry point of a shared collection) keeps its word. */
    for (uint32_t i = 0; i < s->descriptor_count; ++i) {
        const BeerD3D12ShaderDescriptor *e = &s->descriptors[i];
        if (e->bindless && e->type == type && e->register_space == d->register_space &&
            e->register_index == d->register_index && e->heap_class == heap_class) {
            memset(out, 0, sizeof(*out));
            out->set = 1; out->binding = e->heap_class; out->root_constant_index = e->push_word;
            out->bindless.use_heap = DXIL_SPV_TRUE;
            const int untyped_again = d->kind == DXIL_SPV_RESOURCE_KIND_RAW_BUFFER || d->kind == DXIL_SPV_RESOURCE_KIND_STRUCTURED_BUFFER;
            out->descriptor_type = heap_class == HEAP_CLASS_CBV ? DXIL_SPV_VULKAN_DESCRIPTOR_TYPE_UBO :
                                   untyped_again ? DXIL_SPV_VULKAN_DESCRIPTOR_TYPE_TEXEL_BUFFER : DXIL_SPV_VULKAN_DESCRIPTOR_TYPE_IDENTITY;
            return 1;
        }
    }
    if (c->bindless == c->words_per_stage ||
        s->descriptor_count == BEER_D3D12_MAX_SHADER_DESCRIPTORS) {
        c->failed = 1;
        return 0;
    }
    BeerD3D12ShaderDescriptor *e = &s->descriptors[s->descriptor_count++];
    const uint32_t word = c->stage * c->words_per_stage + c->bindless++;
    *e = (BeerD3D12ShaderDescriptor){
        .type = type, .register_space = d->register_space,
        .register_index = d->register_index,
        .resource_type = resource_type(d->kind), .count = 1,
        .bindless = 1, .push_word = word,
        .heap_class = heap_class
    };
    memset(out, 0, sizeof(*out));
    out->set = 1;
    out->binding = e->heap_class;
    out->root_constant_index = word;
    out->bindless.use_heap = DXIL_SPV_TRUE;
    out->bindless.heap_root_offset = 0;   /* static offset; the index comes from the push word */
    const int untyped = d->kind == DXIL_SPV_RESOURCE_KIND_RAW_BUFFER ||
                        d->kind == DXIL_SPV_RESOURCE_KIND_STRUCTURED_BUFFER;
    out->descriptor_type = heap_class == HEAP_CLASS_CBV ? DXIL_SPV_VULKAN_DESCRIPTOR_TYPE_UBO :
                           untyped ? DXIL_SPV_VULKAN_DESCRIPTOR_TYPE_TEXEL_BUFFER :
                                     DXIL_SPV_VULKAN_DESCRIPTOR_TYPE_IDENTITY;
    return 1;
}

static void fill(dxil_spv_vulkan_binding *out, uint32_t binding,
                 const dxil_spv_d3d_binding *d)
{
    memset(out, 0, sizeof(*out));
    out->set = 0;
    out->binding = binding;
    const int untyped = d->kind == DXIL_SPV_RESOURCE_KIND_RAW_BUFFER ||
                        d->kind == DXIL_SPV_RESOURCE_KIND_STRUCTURED_BUFFER;
    out->descriptor_type = untyped ? DXIL_SPV_VULKAN_DESCRIPTOR_TYPE_TEXEL_BUFFER :
                                     DXIL_SPV_VULKAN_DESCRIPTOR_TYPE_IDENTITY;
}

static dxil_spv_bool srv_cb(void *user, const dxil_spv_d3d_binding *d,
                            dxil_spv_srv_vulkan_binding *out)
{
    Collect *c = user;
    memset(out, 0, sizeof(*out));
    if (d->kind == DXIL_SPV_RESOURCE_KIND_RT_ACCELERATION_STRUCTURE) {
        if (!c->accel_ok) { c->failed = 1; return DXIL_SPV_FALSE; }
        fill(&out->buffer_binding, assign(c, BEER_D3D12_SHADER_DESCRIPTOR_SRV, d), d);
        return c->failed ? DXIL_SPV_FALSE : DXIL_SPV_TRUE;
    }
    if (wants_bindless(d, BEER_D3D12_SHADER_DESCRIPTOR_SRV)) {
        assign_bindless(c, BEER_D3D12_SHADER_DESCRIPTOR_SRV, d, &out->buffer_binding);
        return c->failed ? DXIL_SPV_FALSE : DXIL_SPV_TRUE;
    }
    fill(&out->buffer_binding, assign(c, BEER_D3D12_SHADER_DESCRIPTOR_SRV, d), d);
    return c->failed ? DXIL_SPV_FALSE : DXIL_SPV_TRUE;
}

static dxil_spv_bool sampler_cb(void *user, const dxil_spv_d3d_binding *d,
                                dxil_spv_vulkan_binding *out)
{
    Collect *c = user;
    if (is_heap_access(d)) {
        assign_bindless_class(c, BEER_D3D12_SHADER_DESCRIPTOR_SAMPLER, d, out, HEAP_CLASS_SAMPLER);
        return c->failed ? DXIL_SPV_FALSE : DXIL_SPV_TRUE;
    }
    fill(out, assign(c, BEER_D3D12_SHADER_DESCRIPTOR_SAMPLER, d), d);
    return c->failed ? DXIL_SPV_FALSE : DXIL_SPV_TRUE;
}

static dxil_spv_bool uav_cb(void *user, const dxil_spv_uav_d3d_binding *d,
                            dxil_spv_uav_vulkan_binding *out)
{
    Collect *c = user;
    memset(out, 0, sizeof(*out));
    if (wants_bindless(&d->d3d_binding, BEER_D3D12_SHADER_DESCRIPTOR_UAV)) {
        assign_bindless(c, BEER_D3D12_SHADER_DESCRIPTOR_UAV, &d->d3d_binding,
                        &out->buffer_binding);
        return c->failed ? DXIL_SPV_FALSE : DXIL_SPV_TRUE;
    }
    fill(&out->buffer_binding, assign(c, BEER_D3D12_SHADER_DESCRIPTOR_UAV, &d->d3d_binding),
         &d->d3d_binding);
    if (d->has_counter) {
        /* Hidden counter buffers are not backed by a D3D12 resource BD12 knows
         * about; give them a binding of their own so the shader stays valid. */
        dxil_spv_d3d_binding counter = d->d3d_binding;
        counter.register_space = 0xfffe;
        counter.kind = DXIL_SPV_RESOURCE_KIND_RAW_BUFFER;
        fill(&out->counter_binding, assign(c, BEER_D3D12_SHADER_DESCRIPTOR_UAV, &counter),
             &counter);
    }
    return c->failed ? DXIL_SPV_FALSE : DXIL_SPV_TRUE;
}

static dxil_spv_bool cbv_cb(void *user, const dxil_spv_d3d_binding *d,
                            dxil_spv_cbv_vulkan_binding *out)
{
    Collect *c = user;
    memset(out, 0, sizeof(*out));
    out->push_constant = DXIL_SPV_FALSE;
    if (is_heap_access(d)) {
        assign_bindless_class(c, BEER_D3D12_SHADER_DESCRIPTOR_CBV, d, &out->vulkan.uniform_binding, HEAP_CLASS_CBV);
        return c->failed ? DXIL_SPV_FALSE : DXIL_SPV_TRUE;
    }
    out->vulkan.uniform_binding.set = 0;
    out->vulkan.uniform_binding.binding =
        assign(c, BEER_D3D12_SHADER_DESCRIPTOR_CBV, d);
    out->vulkan.uniform_binding.descriptor_type = DXIL_SPV_VULKAN_DESCRIPTOR_TYPE_UBO;
    return c->failed ? DXIL_SPV_FALSE : DXIL_SPV_TRUE;
}

static void log_cb(void *user, dxil_spv_log_level level, const char *msg)
{
    (void)user;
    if (level >= DXIL_SPV_LOG_LEVEL_WARN || getenv("BEER_DXIL_VERBOSE"))
        fprintf(stderr, "[DXIL] %s", msg);
}

static void add_bool_option(dxil_spv_converter c, dxil_spv_option type, int enabled)
{
    /* Every boolean option begins with {base, bool}. */
    struct { dxil_spv_option_base base; dxil_spv_bool value; } opt = {
        { type }, enabled ? DXIL_SPV_TRUE : DXIL_SPV_FALSE
    };
    g.converter_add_option(c, &opt.base);
}

int beer_d3d12_dxil_translate(const uint8_t *container, size_t size,
                              uint32_t stage_namespace,
                              BeerD3D12TranslatedShader *out)
{
    pthread_mutex_lock(&g_lock);
    if (!load_library()) {
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    int ok = 0;
    g.begin_thread_allocator_context();
    g.set_thread_log_callback(log_cb, NULL);
    dxil_spv_parsed_blob blob = NULL;
    dxil_spv_converter converter = NULL;
    /* accel_ok: inline ray queries (DXR 1.1) in ordinary compute/pixel shaders bind an acceleration structure SRV too;
     * refusing it dropped RE8's ray-traced shadow/AO passes, which left the lighting black. */
    Collect collect = { out, stage_namespace, 0, 0, 0, PUSH_WORDS_PER_STAGE, 1 };
    if (g.parse_dxil_blob(container, size, &blob) != DXIL_SPV_SUCCESS) {
        fprintf(stderr, "[DXIL] failed to parse DXIL blob\n");
        goto done;
    }
    if (stage_namespace == ~0u) {           /* derive it from the program */
        switch (g.parsed_blob_get_shader_stage(blob)) {
        case DXIL_SPV_STAGE_VERTEX: collect.stage = 1; break;
        case DXIL_SPV_STAGE_PIXEL: collect.stage = 2; break;
        case DXIL_SPV_STAGE_GEOMETRY: collect.stage = 5; break;
        case DXIL_SPV_STAGE_HULL: collect.stage = 6; break;
        case DXIL_SPV_STAGE_DOMAIN: collect.stage = 7; break;
        default: collect.stage = 0; break;
        }
    }
    if (g.create_converter(blob, &converter) != DXIL_SPV_SUCCESS) goto done;
    /* Push block covering this stage's word window. */
    g.converter_set_root_constant_word_count(converter,
        (collect.stage + 1) * collect.words_per_stage);
    g.converter_set_srv_remapper(converter, srv_cb, &collect);
    g.converter_set_sampler_remapper(converter, sampler_cb, &collect);
    g.converter_set_uav_remapper(converter, uav_cb, &collect);
    g.converter_set_cbv_remapper(converter, cbv_cb, &collect);
    add_bool_option(converter, DXIL_SPV_OPTION_SHADER_DEMOTE_TO_HELPER, 1);
    add_bool_option(converter, DXIL_SPV_OPTION_TYPED_UAV_READ_WITHOUT_FORMAT, 1);
    if (g.converter_run(converter) != DXIL_SPV_SUCCESS || collect.failed) {
        fprintf(stderr, "[DXIL] conversion failed%s\n",
                collect.failed ? " (unsupported resource)" : "");
        goto done;
    }
    dxil_spv_compiled_spirv spirv = { 0 };
    if (g.converter_get_compiled_spirv(converter, &spirv) != DXIL_SPV_SUCCESS ||
        !spirv.data || !spirv.size)
        goto done;
    out->spirv = malloc(spirv.size);
    if (!out->spirv) goto done;
    memcpy(out->spirv, spirv.data, spirv.size);
    out->spirv_size = spirv.size;
    ok = 1;
done:
    if (converter) g.converter_free(converter);
    if (blob) g.parsed_blob_free(blob);
    g.end_thread_allocator_context();
    pthread_mutex_unlock(&g_lock);
    if (!ok) out->descriptor_count = 0;
    return ok;
}

/* ---- ray tracing libraries ---- */

#define RT_PUSH_WORDS 16u

int beer_d3d12_dxil_translate_library(const uint8_t *container, size_t size, BeerD3D12TranslatedShader *shared,
                                      BeerD3D12RtShader *out, uint32_t max, uint32_t *count)
{
    *count = 0;
    pthread_mutex_lock(&g_lock);
    if (!load_library()) { pthread_mutex_unlock(&g_lock); return 0; }
    int ok = 0;
    g.begin_thread_allocator_context();
    g.set_thread_log_callback(log_cb, NULL);
    dxil_spv_parsed_blob blob = NULL;
    Collect collect = { shared, 0, 0, 0, 0, RT_PUSH_WORDS, 1 };
    unsigned entries = 0;
    if (g.parse_dxil_blob(container, size, &blob) != DXIL_SPV_SUCCESS ||
        g.parsed_blob_get_num_entry_points(blob, &entries) != DXIL_SPV_SUCCESS) {
        fprintf(stderr, "[DXIL] failed to parse DXIL library\n");
        goto done;
    }
    /* Continue numbering where earlier libraries of the same pipeline stopped. */
    for (uint32_t i = 0; i < shared->descriptor_count; ++i)
        if (!shared->descriptors[i].bindless) ++collect.next;
        else ++collect.bindless;
    for (unsigned i = 0; i < entries && *count < max; ++i) {
        const char *mangled = NULL, *demangled = NULL;
        if (g.parsed_blob_get_entry_point_name(blob, i, &mangled) != DXIL_SPV_SUCCESS || !mangled) continue;
        if (g.parsed_blob_get_entry_point_demangled_name(blob, i, &demangled) != DXIL_SPV_SUCCESS || !demangled) demangled = mangled;
        dxil_spv_converter converter = NULL;
        if (g.create_converter(blob, &converter) != DXIL_SPV_SUCCESS) { fprintf(stderr, "[DXIL] cannot create converter for %s\n", demangled); goto done; }
        g.converter_set_entry_point(converter, mangled);
        g.converter_set_root_constant_word_count(converter, RT_PUSH_WORDS);
        g.converter_set_srv_remapper(converter, srv_cb, &collect);
        g.converter_set_sampler_remapper(converter, sampler_cb, &collect);
        g.converter_set_uav_remapper(converter, uav_cb, &collect);
        g.converter_set_cbv_remapper(converter, cbv_cb, &collect);
        add_bool_option(converter, DXIL_SPV_OPTION_SHADER_DEMOTE_TO_HELPER, 1);
        add_bool_option(converter, DXIL_SPV_OPTION_TYPED_UAV_READ_WITHOUT_FORMAT, 1);
        {
            dxil_spv_option_sbt_descriptor_size_log2 sbt = { { DXIL_SPV_OPTION_SBT_DESCRIPTOR_SIZE_LOG2 }, 7, 7 };
            g.converter_add_option(converter, &sbt.base);
        }
        if (g.converter_run(converter) != DXIL_SPV_SUCCESS || collect.failed) {
            fprintf(stderr, "[DXIL] conversion of %s failed%s\n", demangled, collect.failed ? " (unsupported resource)" : "");
            g.converter_free(converter);
            goto done;
        }
        dxil_spv_compiled_spirv spirv = { 0 };
        if (g.converter_get_compiled_spirv(converter, &spirv) != DXIL_SPV_SUCCESS || !spirv.data || !spirv.size) { g.converter_free(converter); goto done; }
        BeerD3D12RtShader *shader = &out[*count];
        memset(shader, 0, sizeof(*shader));
        snprintf(shader->name, sizeof(shader->name), "%s", demangled);
        shader->stage = (uint32_t)g.parsed_blob_get_shader_stage_for_entry(blob, mangled);
        shader->spirv = malloc(spirv.size);
        if (!shader->spirv) { g.converter_free(converter); goto done; }
        memcpy(shader->spirv, spirv.data, spirv.size);
        shader->spirv_size = spirv.size;
        ++*count;
        g.converter_free(converter);
    }
    ok = 1;
done:
    if (blob) g.parsed_blob_free(blob);
    g.end_thread_allocator_context();
    pthread_mutex_unlock(&g_lock);
    if (!ok) {
        for (uint32_t i = 0; i < *count; ++i) free(out[i].spirv);
        *count = 0;
    }
    return ok;
}
