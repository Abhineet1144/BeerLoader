#define _GNU_SOURCE
#include "d3d12_dxil_translator.h"

#include <dlfcn.h>
#include <libgen.h>
#include <limits.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "dxil_spirv_c.h"

/* dxil-spirv calls abort() on some internal failures (the CFG structurizer: "Fatal, detected infinite loop"), which
 * ended the whole game. Around a conversion the translating thread arms t_abort_jump; the chained SIGABRT handler
 * jumps back to it, the conversion counts as failed (the pipeline uses its fallback or is not created) and the game
 * keeps running. What the aborted conversion allocated is leaked. Any other SIGABRT goes to the previous handler. */
static __thread sigjmp_buf *t_abort_jump;
static struct sigaction g_previous_abort;
static BeerD3D12DxilCaps g_caps;

void beer_d3d12_dxil_set_caps(const BeerD3D12DxilCaps *caps)
{
    if (caps) g_caps = *caps;
    fprintf(stderr, "[DXIL] caps: scalar block layout %d, raw access chains %d, vendor 0x%04x\n",
            g_caps.scalar_block_layout, g_caps.raw_access_chains, g_caps.vendor_id);
}

uint64_t beer_d3d12_dxil_caps_key(void)
{
    return ((uint64_t)g_caps.vendor_id << 32) | ((uint64_t)!!g_caps.barycentrics << 2) |
           ((uint64_t)!!g_caps.raw_access_chains << 1) | (uint64_t)!!g_caps.scalar_block_layout;
}

/* Options that follow the device (vkd3d-proton adds the same ones from its device's shader target extensions). */
static void add_device_options(dxil_spv_converter converter,
                               dxil_spv_result (*add)(dxil_spv_converter, const dxil_spv_option_base *))
{
    {   /* BEER_DXIL_LEGACY_STRUCTURIZER=1: dxil-spirv's older CFG structurizer (diagnostic comparison). */
        static int legacy = -1;
        if (legacy < 0) { const char *v = getenv("BEER_DXIL_LEGACY_STRUCTURIZER"); legacy = v && *v && strcmp(v, "0"); }
        if (legacy) {
            const dxil_spv_option_legacy_structurizer opt = { { DXIL_SPV_OPTION_LEGACY_STRUCTURIZER }, DXIL_SPV_TRUE };
            add(converter, &opt.base);
        }
    }
    if (g_caps.scalar_block_layout) {
        /* Vectorized SSBO loads/stores with scalar alignment. Only AMD bounds-checks such access per component; on
         * the others a vec3 byte-address access stays split so robustness keeps working per element. */
        const dxil_spv_option_scalar_block_layout scalar = { { DXIL_SPV_OPTION_SCALAR_BLOCK_LAYOUT }, DXIL_SPV_TRUE,
                                                             g_caps.vendor_id == 0x1002 ? DXIL_SPV_TRUE : DXIL_SPV_FALSE };
        add(converter, &scalar.base);
        /* NVIDIA assumes the shader-implied SSBO alignment (the heap binds 16-byte-aligned bases below it), so do
         * not vectorize past what the shader itself guarantees. */
        if (g_caps.vendor_id == 0x10de) {
            const dxil_spv_option_conservative_ssbo_vectorization conservative = {
                { DXIL_SPV_OPTION_CONSERVATIVE_SSBO_VECTORIZATION }, DXIL_SPV_TRUE };
            add(converter, &conservative.base);
        }
    }
    if (g_caps.barycentrics) {
        const dxil_spv_option_barycentric_khr bary = { { DXIL_SPV_OPTION_BARYCENTRIC_KHR }, DXIL_SPV_TRUE };
        add(converter, &bary.base);
    }
    if (g_caps.raw_access_chains) {
        const dxil_spv_option_raw_access_chains_nv chains = { { DXIL_SPV_OPTION_RAW_ACCESS_CHAINS_NV }, DXIL_SPV_TRUE };
        add(converter, &chains.base);
    }
}
static void translator_abort_handler(int sig, siginfo_t *info, void *context)
{
    sigjmp_buf *jump = t_abort_jump;
    if (jump) { t_abort_jump = NULL; siglongjmp(*jump, 1); }
    if (g_previous_abort.sa_flags & SA_SIGINFO) {
        if (g_previous_abort.sa_sigaction) { g_previous_abort.sa_sigaction(sig, info, context); return; }
    } else if (g_previous_abort.sa_handler != SIG_IGN && g_previous_abort.sa_handler != SIG_DFL &&
               g_previous_abort.sa_handler) {
        g_previous_abort.sa_handler(sig);
        return;
    }
    signal(SIGABRT, SIG_DFL);
    raise(SIGABRT);
}
static void translator_abort_guard_setup(void)
{
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = translator_abort_handler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    sigaction(SIGABRT, &action, &g_previous_abort);
}
static void translator_abort_guard_install(void)
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, translator_abort_guard_setup);
}
static dxil_spv_result guarded_converter_run(dxil_spv_result (*run)(dxil_spv_converter), dxil_spv_converter converter)
{
    translator_abort_guard_install();
    sigjmp_buf jump;
    if (sigsetjmp(jump, 1)) {
        fprintf(stderr, "[DXIL] dxil-spirv aborted during conversion: shader treated as untranslatable\n");
        return DXIL_SPV_ERROR_GENERIC;
    }
    t_abort_jump = &jump;
    const dxil_spv_result result = run(converter);
    t_abort_jump = NULL;
    return result;
}

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
    void (*converter_set_root_descriptor_count)(dxil_spv_converter, unsigned);
    void (*converter_set_srv_remapper)(dxil_spv_converter, dxil_spv_srv_remapper_cb, void *);
    void (*converter_set_sampler_remapper)(dxil_spv_converter, dxil_spv_sampler_remapper_cb, void *);
    void (*converter_set_uav_remapper)(dxil_spv_converter, dxil_spv_uav_remapper_cb, void *);
    void (*converter_set_cbv_remapper)(dxil_spv_converter, dxil_spv_cbv_remapper_cb, void *);
    void (*converter_set_stage_input_remapper)(dxil_spv_converter, dxil_spv_shader_stage_io_remapper_cb, void *);
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
    LOAD(converter_set_root_descriptor_count);
    LOAD(converter_set_srv_remapper);
    LOAD(converter_set_sampler_remapper);
    LOAD(converter_set_uav_remapper);
    LOAD(converter_set_cbv_remapper);
    LOAD(converter_set_stage_input_remapper);
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
    int ssbo;                    /* raw/structured buffers become storage buffers (second attempt) */
    const BeerD3D12RootLayout *rl;   /* root-signature translation (beer_d3d12_dxil_translate_rs) */
} Collect;

#define PUSH_WORDS_PER_STAGE 4u
#define COMPUTE_PUSH_WORDS 32u

static uint32_t resource_type(dxil_spv_resource_kind kind)
{
    switch (kind) {
    case DXIL_SPV_RESOURCE_KIND_TEXTURE_1D: return BEER_D3D12_SHADER_RESOURCE_TEXTURE_1D;
    case DXIL_SPV_RESOURCE_KIND_TEXTURE_2D: return BEER_D3D12_SHADER_RESOURCE_TEXTURE_2D;
    case DXIL_SPV_RESOURCE_KIND_FEEDBACK_TEXTURE_2D: return BEER_D3D12_SHADER_RESOURCE_TEXTURE_2D;   /* R64 storage image */
    case DXIL_SPV_RESOURCE_KIND_FEEDBACK_TEXTURE_2D_ARRAY: return BEER_D3D12_SHADER_RESOURCE_TEXTURE_2D_ARRAY;
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

/* A set-0 binding for (type, d). Raw and structured buffers are texel buffers, or storage buffers in ssbo mode. */
static void fill(Collect *c, uint32_t type, dxil_spv_vulkan_binding *out, const dxil_spv_d3d_binding *d)
{
    const uint32_t binding = assign(c, type, d);
    memset(out, 0, sizeof(*out));
    out->set = 0;
    out->binding = binding;
    const int untyped = d->kind == DXIL_SPV_RESOURCE_KIND_RAW_BUFFER ||
                        d->kind == DXIL_SPV_RESOURCE_KIND_STRUCTURED_BUFFER;
    out->descriptor_type = !untyped ? DXIL_SPV_VULKAN_DESCRIPTOR_TYPE_IDENTITY :
                           c->ssbo ? DXIL_SPV_VULKAN_DESCRIPTOR_TYPE_SSBO :
                                     DXIL_SPV_VULKAN_DESCRIPTOR_TYPE_TEXEL_BUFFER;
    if (untyped && c->ssbo && !c->failed)
        for (uint32_t i = 0; i < c->shader->descriptor_count; ++i)
            if (c->shader->descriptors[i].binding == binding && !c->shader->descriptors[i].bindless)
                c->shader->descriptors[i].ssbo = 1;
}

/* ---- root-signature translation ----
 * Each resource is looked up in the root signature: a table range becomes a heap-mirror access through the table's
 * push word (set 1, mirror binding by class, static offset inside the table), a root CBV/SRV/UAV a buffer device
 * address in the push block, root constants push-constant words, a static sampler a set-0 binding. Anything that
 * does not fit fails the translation (the pipeline then uses the per-draw descriptor path). */
static uint32_t stage_visibility(uint32_t stage)
{
    switch (stage) {
    case 1: return 1;   /* vertex */
    case 2: return 5;   /* pixel */
    case 5: return 4;   /* geometry */
    case 6: return 2;   /* hull */
    case 7: return 3;   /* domain */
    case 3: return 6;   /* amplification */
    case 4: return 7;   /* mesh */
    default: return 0;  /* compute: ALL only */
    }
}

/* The root parameter supplying register `d` of descriptor class `type`; *range set for tables. */
static const BeerD3D12RootParam *rs_find(const Collect *c, uint32_t type, const dxil_spv_d3d_binding *d,
                                         const BeerD3D12RootRange **range)
{
    const BeerD3D12RootLayout *rl = c->rl;
    const uint32_t vis = stage_visibility(c->stage);
    *range = NULL;
    for (uint32_t i = 0; i < rl->param_count; ++i) {
        const BeerD3D12RootParam *p = &rl->params[i];
        if (p->visibility != 0 && p->visibility != vis) continue;
        if (p->type == BEER_ROOT_TABLE) {
            for (uint32_t r = 0; r < p->range_count; ++r) {
                const BeerD3D12RootRange *rr = &p->ranges[r];
                if (rr->type != type || rr->space != d->register_space || d->register_index < rr->base_register)
                    continue;
                if (rr->count != UINT32_MAX && d->register_index - rr->base_register >= rr->count) continue;
                *range = rr;
                return p;
            }
            continue;
        }
        const uint32_t want = p->type == BEER_ROOT_CBV || p->type == BEER_ROOT_CONSTANTS ? BEER_D3D12_SHADER_DESCRIPTOR_CBV :
                              p->type == BEER_ROOT_SRV ? BEER_D3D12_SHADER_DESCRIPTOR_SRV : BEER_D3D12_SHADER_DESCRIPTOR_UAV;
        if (want == type && p->shader_register == d->register_index && p->space == d->register_space) return p;
    }
    return NULL;
}

/* A heap-mirror access: binding `heap_class` of set 1, index = push word `word` + `offset` (+ the shader's index). */
static void rs_heap(const Collect *c, dxil_spv_vulkan_binding *out, uint32_t heap_class, uint32_t word,
                    uint32_t offset, dxil_spv_vulkan_descriptor_type type)
{
    memset(out, 0, sizeof(*out));
    if (c->rl->heap_model == BEER_HEAP_MODEL_MUTABLE) {
        /* Every resource class aliases the one mutable array; samplers are their own heap. */
        out->set = heap_class == HEAP_CLASS_SAMPLER ? 1 : 0;
        out->binding = 0;
    } else {
        out->set = 1;
        out->binding = heap_class;
    }
    out->root_constant_index = word;
    out->bindless.use_heap = DXIL_SPV_TRUE;
    out->bindless.heap_root_offset = offset;
    out->descriptor_type = type;
}

static int untyped_buffer(const dxil_spv_d3d_binding *d)
{
    return d->kind == DXIL_SPV_RESOURCE_KIND_RAW_BUFFER || d->kind == DXIL_SPV_RESOURCE_KIND_STRUCTURED_BUFFER;
}

static int is_buffer_kind(const dxil_spv_d3d_binding *d)
{
    return untyped_buffer(d) || d->kind == DXIL_SPV_RESOURCE_KIND_TYPED_BUFFER;
}

/* SRV (uav = 0) or UAV (uav = 1) in root-signature mode. */
static dxil_spv_bool rs_resource(Collect *c, const dxil_spv_d3d_binding *d, int uav, dxil_spv_vulkan_binding *out)
{
    if (d->kind == DXIL_SPV_RESOURCE_KIND_RT_ACCELERATION_STRUCTURE) {
        /* Inline ray queries: a TLAS passed as a root SRV is its GPU address, and BD12 places every acceleration
         * structure at exactly that address (bd12_raytracing.c), so the shader converts the pushed address
         * (OpConvertUToAccelerationStructureKHR). A TLAS in a descriptor table still takes the per-draw path. */
        const BeerD3D12RootRange *range;
        const BeerD3D12RootParam *p = rs_find(c, BEER_D3D12_SHADER_DESCRIPTOR_SRV, d, &range);
        if (p && p->type == BEER_ROOT_TABLE && !uav && c->rl->heap_model == BEER_HEAP_MODEL_MUTABLE &&
            c->rl->accel_in_heap) {   /* a TLAS SRV in a table: the mutable heap holds its AS descriptor */
            rs_heap(c, out, 0, p->push_index, range->offset + (d->register_index - range->base_register),
                    DXIL_SPV_VULKAN_DESCRIPTOR_TYPE_IDENTITY);
            return DXIL_SPV_TRUE;
        }
        if (!p || p->type == BEER_ROOT_TABLE || uav) {
            static int logged;
            if (logged++ < 3) fprintf(stderr, "[DXIL] ray-query TLAS in a %s: per-draw descriptors\n",
                                      p ? "descriptor table" : "root signature without it");
            c->failed = 1;
            return DXIL_SPV_FALSE;
        }
        memset(out, 0, sizeof(*out));
        out->root_constant_index = p->push_index;
        out->descriptor_type = DXIL_SPV_VULKAN_DESCRIPTOR_TYPE_BUFFER_DEVICE_ADDRESS;
        return DXIL_SPV_TRUE;
    }
    const uint32_t heap_class = (is_buffer_kind(d) ? 2u : 0u) + (uav ? 1u : 0u);
    /* Storage-buffer mode: raw/structured heap views are SSBOs (bd12_heapdb.c writes them that way). */
    /* Every raw/structured heap access is an SSBO (as vkd3d-proton's shaders see them outside heap lowering, where a
     * slot also holds an SSBO for odd strides): misaligned bases are corrected by the set-3 offsets, and 64-bit raw
     * accesses, which dxil-spirv cannot do on texel buffers, work. */
    const int ssbo = c->rl->ssbo_heap && untyped_buffer(d);
    const dxil_spv_vulkan_descriptor_type type =
        !untyped_buffer(d) ? DXIL_SPV_VULKAN_DESCRIPTOR_TYPE_IDENTITY :
        ssbo ? DXIL_SPV_VULKAN_DESCRIPTOR_TYPE_SSBO : DXIL_SPV_VULKAN_DESCRIPTOR_TYPE_TEXEL_BUFFER;
    if (is_heap_access(d)) { rs_heap(c, out, heap_class, c->rl->zero_word, 0, type); return DXIL_SPV_TRUE; }
    const BeerD3D12RootRange *range;
    const BeerD3D12RootParam *p = rs_find(c, uav ? BEER_D3D12_SHADER_DESCRIPTOR_UAV : BEER_D3D12_SHADER_DESCRIPTOR_SRV,
                                          d, &range);
    if (!p) { c->failed = 1; return DXIL_SPV_FALSE; }
    if (p->type == BEER_ROOT_TABLE) {
        rs_heap(c, out, heap_class, p->push_index, range->offset + (d->register_index - range->base_register), type);
        return DXIL_SPV_TRUE;
    }
    if (!untyped_buffer(d)) { c->failed = 1; return DXIL_SPV_FALSE; }   /* root SRV/UAV: raw or structured only */
    memset(out, 0, sizeof(*out));
    out->root_constant_index = p->push_index;
    out->descriptor_type = DXIL_SPV_VULKAN_DESCRIPTOR_TYPE_BUFFER_DEVICE_ADDRESS;
    return DXIL_SPV_TRUE;
}

/* Storage-buffer mode: an SSBO heap access reads its {offset, size} from the heap metadata (set 3 binding 0, a plain
 * binding: dxil-spirv indexes it by the same heap slot). */
static void rs_ssbo_offsets(const Collect *c, const dxil_spv_vulkan_binding *buffer, dxil_spv_vulkan_binding *offset)
{
    memset(offset, 0, sizeof(*offset));
    if (!c->rl->ssbo_heap || buffer->descriptor_type != DXIL_SPV_VULKAN_DESCRIPTOR_TYPE_SSBO ||
        !buffer->bindless.use_heap)
        return;
    offset->set = 3;
    offset->binding = 0;
    offset->descriptor_type = DXIL_SPV_VULKAN_DESCRIPTOR_TYPE_SSBO;
}

static dxil_spv_bool srv_cb(void *user, const dxil_spv_d3d_binding *d,
                            dxil_spv_srv_vulkan_binding *out)
{
    if (((Collect *)user)->rl) {
        memset(out, 0, sizeof(*out));
        const dxil_spv_bool ok = rs_resource(user, d, 0, &out->buffer_binding);
        if (ok) rs_ssbo_offsets(user, &out->buffer_binding, &out->offset_binding);
        return ok;
    }
    Collect *c = user;
    memset(out, 0, sizeof(*out));
    if (d->kind == DXIL_SPV_RESOURCE_KIND_RT_ACCELERATION_STRUCTURE) {
        if (!c->accel_ok) { c->failed = 1; return DXIL_SPV_FALSE; }
        fill(c, BEER_D3D12_SHADER_DESCRIPTOR_SRV, &out->buffer_binding, d);
        return c->failed ? DXIL_SPV_FALSE : DXIL_SPV_TRUE;
    }
    if (wants_bindless(d, BEER_D3D12_SHADER_DESCRIPTOR_SRV)) {
        assign_bindless(c, BEER_D3D12_SHADER_DESCRIPTOR_SRV, d, &out->buffer_binding);
        return c->failed ? DXIL_SPV_FALSE : DXIL_SPV_TRUE;
    }
    fill(c, BEER_D3D12_SHADER_DESCRIPTOR_SRV, &out->buffer_binding, d);
    return c->failed ? DXIL_SPV_FALSE : DXIL_SPV_TRUE;
}

static dxil_spv_bool sampler_cb(void *user, const dxil_spv_d3d_binding *d,
                                dxil_spv_vulkan_binding *out)
{
    Collect *c = user;
    if (c->rl) {
        if (is_heap_access(d)) {
            rs_heap(c, out, HEAP_CLASS_SAMPLER, c->rl->zero_word, 0, DXIL_SPV_VULKAN_DESCRIPTOR_TYPE_IDENTITY);
            return DXIL_SPV_TRUE;
        }
        for (uint32_t i = 0; i < c->rl->static_count; ++i)
            if (c->rl->static_register[i] == d->register_index && c->rl->static_space[i] == d->register_space) {
                if (c->rl->heap_model == BEER_HEAP_MODEL_MUTABLE) {   /* immutable sampler i of set 2 */
                    memset(out, 0, sizeof(*out));
                    out->set = 2;
                    out->binding = i;
                    return DXIL_SPV_TRUE;
                }
                fill(c, BEER_D3D12_SHADER_DESCRIPTOR_SAMPLER, out, d);   /* static sampler: set 0 */
                return c->failed ? DXIL_SPV_FALSE : DXIL_SPV_TRUE;
            }
        const BeerD3D12RootRange *range;
        const BeerD3D12RootParam *p = rs_find(c, BEER_D3D12_SHADER_DESCRIPTOR_SAMPLER, d, &range);
        if (!p || p->type != BEER_ROOT_TABLE) { c->failed = 1; return DXIL_SPV_FALSE; }
        rs_heap(c, out, HEAP_CLASS_SAMPLER, p->push_index, range->offset + (d->register_index - range->base_register),
                DXIL_SPV_VULKAN_DESCRIPTOR_TYPE_IDENTITY);
        return DXIL_SPV_TRUE;
    }
    if (is_heap_access(d)) {
        assign_bindless_class(c, BEER_D3D12_SHADER_DESCRIPTOR_SAMPLER, d, out, HEAP_CLASS_SAMPLER);
        return c->failed ? DXIL_SPV_FALSE : DXIL_SPV_TRUE;
    }
    fill(c, BEER_D3D12_SHADER_DESCRIPTOR_SAMPLER, out, d);
    return c->failed ? DXIL_SPV_FALSE : DXIL_SPV_TRUE;
}

static dxil_spv_bool uav_cb_impl(void *user, const dxil_spv_uav_d3d_binding *d, dxil_spv_uav_vulkan_binding *out);
/* BEER_DXIL_VERBOSE: why a UAV was refused (the converter only says "Failed to remap UAV"). */
static dxil_spv_bool uav_cb(void *user, const dxil_spv_uav_d3d_binding *d, dxil_spv_uav_vulkan_binding *out)
{
    const dxil_spv_bool ok = uav_cb_impl(user, d, out);
    if (!ok && getenv("BEER_DXIL_VERBOSE")) {
        const Collect *c = user;
        fprintf(stderr, "[DXIL] UAV u%u space %u refused: kind=%d range=%u counter=%d root-layout=%d descriptors=%u bindless=%u/%u\n",
                d->d3d_binding.register_index, d->d3d_binding.register_space, (int)d->d3d_binding.kind,
                d->d3d_binding.range_size, (int)d->has_counter, c->rl != NULL, c->shader->descriptor_count,
                c->bindless, c->words_per_stage);
    }
    return ok;
}
static dxil_spv_bool uav_cb_impl(void *user, const dxil_spv_uav_d3d_binding *d,
                                 dxil_spv_uav_vulkan_binding *out)
{
    Collect *c = user;
    memset(out, 0, sizeof(*out));
    if (c->rl) {
        /* Hidden counters need the storage-buffer heap: the counter's address is read from the slot's metadata and
         * the atomic goes through it (physical storage buffer). Without it: per-draw path. */
        if (d->has_counter && !c->rl->ssbo_heap) { c->failed = 1; return DXIL_SPV_FALSE; }
        if (!rs_resource(c, &d->d3d_binding, 1, &out->buffer_binding)) return DXIL_SPV_FALSE;
        rs_ssbo_offsets(c, &out->buffer_binding, &out->offset_binding);
        if (d->has_counter) {
            if (!out->buffer_binding.bindless.use_heap) { c->failed = 1; return DXIL_SPV_FALSE; }
            out->counter_binding = out->buffer_binding;   /* same push word and heap offset: the same slot */
            out->counter_binding.set = 3;
            out->counter_binding.binding = 0;
            out->counter_binding.descriptor_type = DXIL_SPV_VULKAN_DESCRIPTOR_TYPE_BUFFER_DEVICE_ADDRESS;
        }
        return DXIL_SPV_TRUE;
    }
    if (wants_bindless(&d->d3d_binding, BEER_D3D12_SHADER_DESCRIPTOR_UAV)) {
        assign_bindless(c, BEER_D3D12_SHADER_DESCRIPTOR_UAV, &d->d3d_binding,
                        &out->buffer_binding);
        return c->failed ? DXIL_SPV_FALSE : DXIL_SPV_TRUE;
    }
    fill(c, BEER_D3D12_SHADER_DESCRIPTOR_UAV, &out->buffer_binding, &d->d3d_binding);
    if (d->has_counter) {
        /* Hidden counter buffers are not backed by a D3D12 resource BD12 knows
         * about; give them a binding of their own so the shader stays valid. */
        dxil_spv_d3d_binding counter = d->d3d_binding;
        counter.register_space = 0xfffe;
        counter.kind = DXIL_SPV_RESOURCE_KIND_RAW_BUFFER;
        fill(c, BEER_D3D12_SHADER_DESCRIPTOR_UAV, &out->counter_binding, &counter);
    }
    return c->failed ? DXIL_SPV_FALSE : DXIL_SPV_TRUE;
}

static dxil_spv_bool cbv_cb(void *user, const dxil_spv_d3d_binding *d,
                            dxil_spv_cbv_vulkan_binding *out)
{
    Collect *c = user;
    memset(out, 0, sizeof(*out));
    out->push_constant = DXIL_SPV_FALSE;
    if (c->rl) {
        if (is_heap_access(d)) {
            rs_heap(c, &out->vulkan.uniform_binding, HEAP_CLASS_CBV, c->rl->zero_word, 0,
                    DXIL_SPV_VULKAN_DESCRIPTOR_TYPE_UBO);
            return DXIL_SPV_TRUE;
        }
        const BeerD3D12RootRange *range;
        const BeerD3D12RootParam *p = rs_find(c, BEER_D3D12_SHADER_DESCRIPTOR_CBV, d, &range);
        if (!p) { c->failed = 1; return DXIL_SPV_FALSE; }
        if (p->type == BEER_ROOT_CONSTANTS) {
            out->push_constant = DXIL_SPV_TRUE;
            out->vulkan.push_constant.offset_in_words = p->push_index;
        } else if (p->type == BEER_ROOT_CBV) {
            out->vulkan.uniform_binding.root_constant_index = p->push_index;
            out->vulkan.uniform_binding.descriptor_type = DXIL_SPV_VULKAN_DESCRIPTOR_TYPE_BUFFER_DEVICE_ADDRESS;
        } else {
            rs_heap(c, &out->vulkan.uniform_binding, HEAP_CLASS_CBV, p->push_index,
                    range->offset + (d->register_index - range->base_register), DXIL_SPV_VULKAN_DESCRIPTOR_TYPE_UBO);
        }
        return DXIL_SPV_TRUE;
    }
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

const char *beer_d3d12_dxil_library_path(void)
{
    static char path[PATH_MAX];
    pthread_mutex_lock(&g_lock);
    const int ok = load_library();
    pthread_mutex_unlock(&g_lock);
    Dl_info info;
    if (!ok || !dladdr((void *)g.converter_run, &info) || !info.dli_fname) return NULL;
    snprintf(path, sizeof(path), "%s", info.dli_fname);
    return path;
}

int beer_d3d12_dxil_available(void)
{
    pthread_mutex_lock(&g_lock);
    const int ok = load_library();
    pthread_mutex_unlock(&g_lock);
    return ok;
}

/* Mesh-shader -> pixel-shader linkage. D3D12 links the stages by semantic; Vulkan by location, and a per-primitive
 * attribute must be PerPrimitiveEXT on both sides. dxil-spirv places a mesh shader's per-vertex outputs at their
 * signature row and its per-primitive outputs at row + (end of the per-vertex rows), decorated per-primitive. A pixel
 * shader translated on its own takes its own rows and no per-primitive flag, so materials reading per-primitive
 * attributes got undefined inputs (RE9 floors and walls on the mesh-shader path vanished). The map below is built
 * from the mesh shader's OSG1 and PSG1 signature chunks and applied through the stage-input remapper. */
#define LINK_MAX 64
typedef struct {
    uint32_t count;
    struct { char name[48]; uint32_t index, location, component, flags; } e[LINK_MAX];
} StageLink;

static const uint8_t *dxbc_chunk(const uint8_t *c, size_t size, const char fourcc[4], uint32_t *chunk_size)
{
    if (size < 32 || memcmp(c, "DXBC", 4)) return NULL;
    uint32_t count; memcpy(&count, c + 28, 4);
    for (uint32_t i = 0; i < count && 32 + 4ull * (i + 1) <= size; ++i) {
        uint32_t off; memcpy(&off, c + 32 + 4 * i, 4);
        if ((size_t)off + 8 > size) continue;
        if (memcmp(c + off, fourcc, 4)) continue;
        uint32_t n; memcpy(&n, c + off + 4, 4);
        if ((size_t)off + 8 + n > size) return NULL;
        *chunk_size = n;
        return c + off + 8;
    }
    return NULL;
}
/* Signature v1 elements (32 bytes): stream, name offset, semantic index, system value, component type, register,
 * mask, rw mask, pad, min precision. `extent` gets the end row of every element; user elements are added to `link`
 * at register + `row_offset`. */
static void link_add_signature(StageLink *link, const uint8_t *chunk, uint32_t chunk_size, uint32_t row_offset,
                               uint32_t flags, uint32_t *extent)
{
    if (!chunk || chunk_size < 8) return;
    uint32_t count, first; memcpy(&count, chunk, 4); memcpy(&first, chunk + 4, 4);
    for (uint32_t i = 0; i < count; ++i) {
        const size_t at = (size_t)first + 32u * i;
        if (at + 32 > chunk_size) break;
        uint32_t name_off, index, sysval, reg; uint8_t mask = chunk[at + 24];
        memcpy(&name_off, chunk + at + 4, 4); memcpy(&index, chunk + at + 8, 4);
        memcpy(&sysval, chunk + at + 12, 4); memcpy(&reg, chunk + at + 20, 4);
        if (reg == ~0u) continue;
        if (extent && reg + 1 > *extent) *extent = reg + 1;
        if (sysval || name_off >= chunk_size || link->count == LINK_MAX) continue;
        const char *name = (const char *)chunk + name_off;
        const size_t len = strnlen(name, chunk_size - name_off);
        if (len == chunk_size - name_off || len >= sizeof(link->e[0].name)) continue;
        uint32_t component = 0;
        while (component < 4 && mask && !(mask & (1u << component))) ++component;
        memcpy(link->e[link->count].name, name, len + 1);
        link->e[link->count].index = index;
        link->e[link->count].location = reg + row_offset;
        link->e[link->count].component = component & 3;
        link->e[link->count].flags = flags;
        ++link->count;
    }
}
static int link_from_mesh_shader(const uint8_t *ms, size_t ms_size, StageLink *link)
{
    memset(link, 0, sizeof(*link));
    uint32_t osg_size = 0, psg_size = 0, extent = 0;
    const uint8_t *osg = dxbc_chunk(ms, ms_size, "OSG1", &osg_size);
    const uint8_t *psg = dxbc_chunk(ms, ms_size, "PSG1", &psg_size);
    if (!osg) return 0;
    link_add_signature(link, osg, osg_size, 0, DXIL_SPV_SHADER_STAGE_IO_NONE, &extent);
    link_add_signature(link, psg, psg_size, extent, DXIL_SPV_SHADER_STAGE_IO_PER_PRIMITIVE, NULL);
    return 1;
}
uint64_t beer_d3d12_dxil_stage_link_hash(const uint8_t *ms, size_t ms_size)
{
    StageLink link;
    if (!ms || !link_from_mesh_shader(ms, ms_size, &link)) return 0;
    uint64_t h = 1469598103934665603ull;
    const uint8_t *p = (const uint8_t *)&link;
    for (size_t i = 0; i < sizeof(link); ++i) h = (h ^ p[i]) * 1099511628211ull;
    return h ? h : 1;
}
static dxil_spv_bool link_remap(void *user, const dxil_spv_d3d_shader_stage_io *d3d, dxil_spv_vulkan_shader_stage_io *vk)
{
    const StageLink *link = user;
    for (uint32_t i = 0; i < link->count; ++i)
        if (link->e[i].index == d3d->semantic_index && !strcasecmp(link->e[i].name, d3d->semantic)) {
            vk->location = link->e[i].location;
            vk->component = link->e[i].component;
            vk->flags = link->e[i].flags;
            return DXIL_SPV_TRUE;
        }
    return DXIL_SPV_TRUE;   /* not written by the mesh shader: keep the pixel shader's own row (reads are undefined) */
}

static int dxil_translate(const uint8_t *container, size_t size, uint32_t stage_namespace,
                          const BeerD3D12RootLayout *layout, BeerD3D12TranslatedShader *out,
                          const StageLink *link);

int beer_d3d12_dxil_translate(const uint8_t *container, size_t size,
                              uint32_t stage_namespace,
                              BeerD3D12TranslatedShader *out)
{
    return dxil_translate(container, size, stage_namespace, NULL, out, NULL);
}

int beer_d3d12_dxil_translate_rs(const uint8_t *container, size_t size, uint32_t stage_namespace,
                                 const BeerD3D12RootLayout *layout, BeerD3D12TranslatedShader *out)
{
    return layout ? dxil_translate(container, size, stage_namespace, layout, out, NULL) : 0;
}

int beer_d3d12_dxil_translate_rs_linked(const uint8_t *container, size_t size, uint32_t stage_namespace,
                                        const BeerD3D12RootLayout *layout, const uint8_t *mesh_shader,
                                        size_t mesh_shader_size, BeerD3D12TranslatedShader *out)
{
    StageLink link;
    if (!layout) return 0;
    if (!mesh_shader || !link_from_mesh_shader(mesh_shader, mesh_shader_size, &link))
        return dxil_translate(container, size, stage_namespace, layout, out, NULL);
    return dxil_translate(container, size, stage_namespace, layout, out, &link);
}

static int dxil_translate(const uint8_t *container, size_t size, uint32_t stage_namespace,
                          const BeerD3D12RootLayout *layout, BeerD3D12TranslatedShader *out,
                          const StageLink *link)
{
    /* The lock covers loading the library only. dxil-spirv converts concurrently (per-thread allocator contexts and
     * log callbacks; parsed blobs and converters are per call), and pipeline warm-ups translate on many threads. */
    pthread_mutex_lock(&g_lock);
    const int loaded = load_library();
    pthread_mutex_unlock(&g_lock);
    if (!loaded) return 0;
    int ok = 0;
    g.begin_thread_allocator_context();
    g.set_thread_log_callback(log_cb, NULL);
    dxil_spv_parsed_blob blob = NULL;
    dxil_spv_converter converter = NULL;
    /* accel_ok: inline ray queries (DXR 1.1) in ordinary compute/pixel shaders bind an acceleration structure SRV too;
     * refusing it dropped RE8's ray-traced shadow/AO passes, which left the lighting black. */
    Collect collect = { out, stage_namespace, 0, 0, 0, PUSH_WORDS_PER_STAGE, 1, 0, layout };
    /* Diagnostic BEER_DXIL_FORCE_SSBO=1: raw/structured buffers as storage buffers from the start (the retry mode
     * below), as vkd3d-proton translates them on NVIDIA; isolates texel-buffer view problems. */
    if (!layout && getenv("BEER_DXIL_FORCE_SSBO")) collect.ssbo = 1;
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
        case DXIL_SPV_STAGE_AMPLIFICATION: collect.stage = 3; break;
        case DXIL_SPV_STAGE_MESH: collect.stage = 4; break;
        default: collect.stage = 0; break;
        }
    }
    /* A compute pipeline has a single stage, so it owns the whole push block: 32 words (128 bytes, the guaranteed
     * maxPushConstantsSize) of bindless arrays instead of the 4-word per-stage share graphics stages split. Ghost of
     * Tsushima's lighting shaders declare more than four large UAV arrays; they failed to translate ("Failed to remap
     * UAV") and every dispatch was dropped, leaving the lit image black. */
    if (!layout && collect.stage == 0 && collect.words_per_stage == PUSH_WORDS_PER_STAGE)
        collect.words_per_stage = COMPUTE_PUSH_WORDS;
retry:
    if (g.create_converter(blob, &converter) != DXIL_SPV_SUCCESS) goto done;
    add_device_options(converter, g.converter_add_option);
    if (layout) {
        /* Root-signature translation: the push block holds the root arguments (BeerD3D12RootLayout). */
        g.converter_set_root_descriptor_count(converter, layout->descriptor_count);
        g.converter_set_root_constant_word_count(converter, layout->word_count);
        dxil_spv_option_physical_storage_buffer bda = { { DXIL_SPV_OPTION_PHYSICAL_STORAGE_BUFFER }, DXIL_SPV_TRUE };
        g.converter_add_option(converter, &bda.base);
        if (layout->ssbo_heap) {
            /* SSBO heap views: bases aligned down to ssbo_alignment, the rest in the set-3 metadata, which holds per
             * slot uvec2 {offset, size} at uvec2 index 2*slot and the UAV counter address at u64 index 2*slot+1. */
            dxil_spv_option_ssbo_alignment align = { { DXIL_SPV_OPTION_SSBO_ALIGNMENT },
                                                     layout->ssbo_alignment ? layout->ssbo_alignment : 16 };
            g.converter_add_option(converter, &align.base);
            dxil_spv_option_bindless_offset_buffer_layout offsets = { { DXIL_SPV_OPTION_BINDLESS_OFFSET_BUFFER_LAYOUT },
                                                                      0, 0, 2 };
            g.converter_add_option(converter, &offsets.base);
            dxil_spv_option_physical_address_descriptor_indexing counters = {
                { DXIL_SPV_OPTION_PHYSICAL_ADDRESS_DESCRIPTOR_INDEXING }, 2, 1 };
            g.converter_add_option(converter, &counters.base);
        }
    } else {
        /* Push block covering this stage's word window. */
        g.converter_set_root_constant_word_count(converter,
            (collect.stage + 1) * collect.words_per_stage);
    }
    g.converter_set_srv_remapper(converter, srv_cb, &collect);
    g.converter_set_sampler_remapper(converter, sampler_cb, &collect);
    g.converter_set_uav_remapper(converter, uav_cb, &collect);
    g.converter_set_cbv_remapper(converter, cbv_cb, &collect);
    if (link && g.parsed_blob_get_shader_stage(blob) == DXIL_SPV_STAGE_PIXEL)
        g.converter_set_stage_input_remapper(converter, link_remap, (void *)link);
    add_bool_option(converter, DXIL_SPV_OPTION_SHADER_DEMOTE_TO_HELPER, 1);
    add_bool_option(converter, DXIL_SPV_OPTION_TYPED_UAV_READ_WITHOUT_FORMAT, 1);
    if (collect.ssbo) {
        /* Storage-buffer descriptors point at the view's first byte (BD12 binds the exact offset), so no offset
         * emulation: that needs a bindless offset buffer. */
        dxil_spv_option_ssbo_alignment align = { { DXIL_SPV_OPTION_SSBO_ALIGNMENT }, 1 };
        g.converter_add_option(converter, &align.base);
    }
    if (guarded_converter_run(g.converter_run, converter) != DXIL_SPV_SUCCESS || collect.failed) {
        if (!collect.failed && !collect.ssbo && !layout) {
            /* Texel buffers cannot express everything raw/structured buffers allow (64-bit raw loads and stores):
             * translate again with those buffers as storage buffers. */
            g.converter_free(converter);
            converter = NULL;
            out->descriptor_count = 0;
            collect.next = collect.bindless = 0;
            collect.ssbo = 1;
            fprintf(stderr, "[DXIL] retrying with raw/structured buffers as storage buffers\n");
            goto retry;
        }
        if (!layout || getenv("BEER_DXIL_VERBOSE"))
            fprintf(stderr, "[DXIL] conversion failed%s%s\n", collect.failed ? " (unsupported resource)" : "",
                    layout ? " for root-signature translation (per-draw descriptors instead)" : "");
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
    if (getenv("BEER_DXR_PROBE")) {          /* diagnostic: shaders using inline ray queries (OpCapability RayQueryKHR) */
        const uint32_t *w = (const uint32_t *)out->spirv;
        for (size_t i = 5; i + 1 < spirv.size / 4; ) {
            const uint32_t count = w[i] >> 16, op = w[i] & 0xffff;
            if (!count) break;
            if (op == 17 && w[i + 1] == 4472) {
                static unsigned n;
                if (++n <= 5 || !(n % 50)) fprintf(stderr, "[BD12 RT] ray-query shader #%u (stage %u)\n", n, collect.stage);
                break;
            }
            if (op == 14) break;              /* OpMemoryModel: capabilities are over */
            i += count;
        }
    }
done:
    if (converter) g.converter_free(converter);
    if (blob) g.parsed_blob_free(blob);
    g.end_thread_allocator_context();
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
    Collect collect = { shared, 0, 0, 0, 0, RT_PUSH_WORDS, 1, 0, NULL };
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
        add_device_options(converter, g.converter_add_option);
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
        if (guarded_converter_run(g.converter_run, converter) != DXIL_SPV_SUCCESS || collect.failed) {
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
