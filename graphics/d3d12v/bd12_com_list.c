#define _GNU_SOURCE
#include <time.h>
/* BD12 COM shim, part 2: command allocators, command lists and pipeline
 * states. Every command list method records straight into a VkCommandBuffer
 * through the BD12 core. */

#include "bd12_com.h"
#include "../beer_fake_caps.h"
#include "bd12_formats.h"

#include <stdatomic.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const uint8_t iid_allocator[16] = { 0xe4, 0xde, 0x02, 0x61, 0x59, 0xaf,
    0x09, 0x4b, 0xb9, 0x99, 0xb4, 0x4d, 0x73, 0xf0, 0x9b, 0x24 };
static const uint8_t iid_list[16] = { 0x0f, 0x0d, 0x16, 0x5b, 0x1b, 0xac, 0x85,
    0x41, 0x8b, 0xa8, 0xb3, 0xae, 0x42, 0xa5, 0xa4, 0x55 };
/* ID3D12GraphicsCommandList1..4 */
static const uint8_t iid_list1[16] = { 0xfb, 0x03, 0x31, 0x55, 0xe7, 0x1f, 0x57,
    0x45, 0xbb, 0x38, 0x94, 0x6d, 0x7d, 0x0e, 0x7c, 0xa7 };
static const uint8_t iid_list2[16] = { 0x85, 0xe5, 0xc3, 0x38, 0x17, 0xff, 0x2c,
    0x41, 0x91, 0x50, 0x4f, 0xc6, 0xf9, 0xd7, 0x2a, 0x28 };
static const uint8_t iid_list3[16] = { 0xa7, 0x83, 0xda, 0x6f, 0x4c, 0xb8, 0x38,
    0x4e, 0x9a, 0xc8, 0xc7, 0xbd, 0x22, 0x01, 0x6b, 0x3d };
static const uint8_t iid_list4[16] = { 0x8e, 0x31, 0x54, 0x87, 0xa9, 0xd3, 0x41,
    0x45, 0x98, 0xcf, 0x64, 0x5b, 0x50, 0xdc, 0x48, 0x74 };
/* ID3D12GraphicsCommandList5 (RSSetShadingRate[Image]): exposed when variable-rate shading is enabled
 * (BEER_BD12_VRS=1) or with BEER_FAKE_CAPS. List6 (DispatchMesh): BEER_FAKE_CAPS only, where it does nothing. */
static const uint8_t iid_list5[16] = { 0x59, 0x08, 0x05, 0x55, 0x24, 0x40, 0x4c,
    0x47, 0x87, 0xf5, 0x64, 0x72, 0xea, 0xee, 0x44, 0xea };
static const uint8_t iid_list6[16] = { 0x90, 0x78, 0x82, 0xc3, 0x48, 0xe5, 0xfa,
    0x4c, 0x96, 0xcf, 0x56, 0x89, 0xa9, 0x37, 0x0f, 0x80 };
static const uint8_t iid_command_list[16] = { 0x1c, 0xd9, 0x16, 0x71, 0xe4,
    0xe7, 0xce, 0x47, 0xb8, 0xc6, 0xec, 0x81, 0x68, 0xf4, 0x37, 0xe5 };
static const uint8_t iid_pso[16] = { 0xf3, 0x30, 0x5a, 0x76, 0x24, 0xf6, 0x6f,
    0x4c, 0xa8, 0x28, 0xac, 0xe9, 0x48, 0x62, 0x24, 0x45 };

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static void *rdptr(const uint8_t *p) { void *v; memcpy(&v, p, sizeof(v)); return v; }

typedef struct ComList ComList;

/* Pipeline states --------------------------------------------------------------- */

typedef struct ComPso {
    ComObj obj;
    int compute;
    BD12PsoDesc desc;             /* owned clone until compiled */
    uint64_t hash;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int state;                    /* 0 pending, 2 ready, 3 failed */
    BD12GraphicsPipeline *gfx;
    BD12ComputePipeline *cmp;
    const void *last_alloc;
    uint64_t last_gen;
    struct ComPso *job_next;
    ComObj *root_signature;       /* held while the pipeline lives: compilation may come later (first use) */
} ComPso;

static pthread_mutex_t g_job_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_job_cond = PTHREAD_COND_INITIALIZER;
static pthread_cond_t g_job_space = PTHREAD_COND_INITIALIZER;
static ComPso *g_job_head, *g_job_tail;
static uint32_t g_job_count;
static int g_workers_started, g_worker_count;

/* Pipeline descriptions own cloned shader blobs until compilation. RE8 creates
 * PSOs far faster than a Vulkan driver can compile them during startup, so an
 * unbounded queue grows by hundreds of MiB per second. Bound both queued blobs
 * and driver compiler residency. A PSO needed immediately can still remove
 * itself from the queue and compile on the recording thread. */
#define BD12_PSO_WORKERS 1
#define BD12_PSO_QUEUE_LIMIT 16u

static uint64_t fnv(const uint8_t *p, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

static void pso_compile(ComPso *p)
{
    BD12_PROF_BEGIN();
    BD12Device *bd = p->obj.dev->bd;
    if (p->compute)
        p->cmp = bd12_compute_pipeline_create_dxbc_rs(bd, p->desc.cs, p->desc.cs_size, p->hash,
            p->desc.root_signature ? ((ComRootSig *)p->desc.root_signature)->rs : NULL);
    else
        p->gfx = bd12_pso_create_graphics(bd, &p->desc);
    bd12_pso_release(&p->desc);
    pthread_mutex_lock(&p->lock);
    p->state = (p->gfx || p->cmp) ? 2 : 3;
    pthread_cond_broadcast(&p->cond);
    pthread_mutex_unlock(&p->lock);
    BD12_PROF_END(BD12_PROF_PSO);
}

/* Compile-at-create warms the pipeline cache without keeping the pipeline: build it (the driver stores the result
 * in the VkPipelineCache, which is saved to disk), destroy the VkPipeline at once, and keep the description (its
 * shaders are interned and shared) so the first bind rebuilds it as a cache hit. A live VkPipeline costs ~110 KB of
 * driver memory; engines create tens of thousands at their "compiling shaders" step and bind a fraction of them
 * (Where Winds Meet: 19.9k pipelines = 2.1 GB, killed at 9 GB; Stellar Blade 7.8 GB; RE9 ~1.6 GB more than Proton).
 * By default every compiled pipeline stays alive, as on Windows (layouts are shared, so each costs only its
 * VkPipeline). BEER_BD12_RELEASE_PIPELINES=1 opts into warm-and-release when memory is tight. */
/* The memory budget for keeping pipelines: BEER_BD12_PIPELINE_BUDGET_MB, else 60% of the smaller of this process's
 * cgroup limit (memory.max, e.g. a 9 GB launcher cap) and the machine's RAM. 0 = unknown (keep everything). */
static uint64_t pipeline_budget_bytes(void)
{
    const char *set = getenv("BEER_BD12_PIPELINE_BUDGET_MB");
    if (set && *set) return (uint64_t)strtoull(set, NULL, 10) << 20;
    uint64_t limit = 0;
    {   /* MemTotal */
        FILE *f = fopen("/proc/meminfo", "r");
        char line[256];
        while (f && fgets(line, sizeof(line), f))
            if (!strncmp(line, "MemTotal:", 9)) { limit = strtoull(line + 9, NULL, 10) << 10; break; }
        if (f) fclose(f);
    }
    {   /* cgroup v2: "0::/path" in /proc/self/cgroup, the limit in /sys/fs/cgroup/<path>/memory.max */
        FILE *f = fopen("/proc/self/cgroup", "r");
        char line[512];
        if (f && fgets(line, sizeof(line), f) && !strncmp(line, "0::", 3)) {
            line[strcspn(line, "\n")] = 0;
            char path[700];
            snprintf(path, sizeof(path), "/sys/fs/cgroup%s/memory.max", line + 3);
            FILE *m = fopen(path, "r");
            char value[64];
            if (m && fgets(value, sizeof(value), m) && strncmp(value, "max", 3)) {
                const uint64_t cg = strtoull(value, NULL, 10);
                if (cg && (!limit || cg < limit)) limit = cg;
            }
            if (m) fclose(m);
        }
        if (f) fclose(f);
    }
    return limit / 10 * 6;
}
static uint64_t process_resident_bytes(void)
{
    FILE *f = fopen("/proc/self/statm", "r");
    unsigned long long total = 0, resident = 0;
    if (f) { if (fscanf(f, "%llu %llu", &total, &resident) != 2) resident = 0; fclose(f); }
    return (uint64_t)resident * (uint64_t)sysconf(_SC_PAGESIZE);
}
/* Keep compiled pipelines (Windows behaviour) unless memory is tight: every 64 creations the process's resident size
 * is compared with the budget, and once over it, later compile-at-create pipelines are warmed into the cache and
 * released (pso_warm); the choice is sticky. BEER_BD12_RELEASE_PIPELINES=1 releases from the start, =0 never. */
static int keep_pipelines(void)
{
    static int forced = -2;                 /* -2 unread, -1 automatic, 0 release, 1 keep */
    static _Atomic int tight;
    static _Atomic uint32_t checks;
    static uint64_t budget;
    if (forced == -2) {
        const char *release = getenv("BEER_BD12_RELEASE_PIPELINES");
        forced = release && *release ? (strcmp(release, "0") ? 0 : 1) : -1;
        if (forced < 0) budget = pipeline_budget_bytes();
    }
    if (forced >= 0) return forced;
    if (atomic_load(&tight)) return 0;
    if (!budget || (atomic_fetch_add(&checks, 1) & 63)) return 1;
    const uint64_t resident = process_resident_bytes();
    if (resident > budget) {
        if (!atomic_exchange(&tight, 1))
            fprintf(stderr, "[BD12] memory tight (%llu MB resident, budget %llu MB): new pipelines are now warmed "
                    "into the cache and released\n", (unsigned long long)(resident >> 20),
                    (unsigned long long)(budget >> 20));
        return 0;
    }
    return 1;
}
static void pso_warm(ComPso *p)
{
    BD12_PROF_BEGIN();
    BD12Device *bd = p->obj.dev->bd;
    int ok;
    if (p->compute) {
        BD12ComputePipeline *c = bd12_compute_pipeline_create_dxbc_rs(bd, p->desc.cs, p->desc.cs_size, p->hash,
            p->desc.root_signature ? ((ComRootSig *)p->desc.root_signature)->rs : NULL);
        ok = c != NULL;
        if (c) bd12_compute_pipeline_destroy(c);
    } else {
        BD12GraphicsPipeline *g = bd12_pso_create_graphics(bd, &p->desc);
        ok = g != NULL;
        if (g) bd12_graphics_pipeline_destroy(g);
    }
    pthread_mutex_lock(&p->lock);
    p->state = ok ? 0 : 3;            /* pending again: the first bind rebuilds it from the cache */
    pthread_cond_broadcast(&p->cond);
    pthread_mutex_unlock(&p->lock);
    if (!ok) bd12_pso_release(&p->desc);
    {   static uint64_t warmed;
        const uint64_t n = __atomic_add_fetch(&warmed, 1, __ATOMIC_RELAXED);
        if (getenv("BEER_BD12_PIPELIB_LOG") && (n == 1 || n % 2000 == 0))
            fprintf(stderr, "[PIPELIB] pipelines warmed into the cache and released=%llu\n", (unsigned long long)n);
    }
    BD12_PROF_END(BD12_PROF_PSO);
}

static void *pso_worker(void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&g_job_lock);
        while (!g_job_head) pthread_cond_wait(&g_job_cond, &g_job_lock);
        ComPso *p = g_job_head;
        g_job_head = p->job_next;
        if (!g_job_head) g_job_tail = NULL;
        if (g_job_count) --g_job_count;
        pthread_cond_broadcast(&g_job_space);
        pthread_mutex_lock(&p->lock);
        p->state = 1;             /* compiling */
        pthread_mutex_unlock(&p->lock);
        pthread_mutex_unlock(&g_job_lock);
        pso_compile(p);
        com_release(&p->obj);   /* reference held by the queued job */
    }
    return NULL;
}

static int pso_enqueue(ComPso *p)
{
    com_add_ref(&p->obj);
    pthread_mutex_lock(&g_job_lock);
    if (!g_workers_started) {
        g_workers_started = 1;
        for (int i = 0; i < BD12_PSO_WORKERS; ++i) {
            pthread_t t;
            if (pthread_create(&t, NULL, pso_worker, NULL) == 0) {
                pthread_detach(t);
                ++g_worker_count;
            }
        }
    }
    if (!g_worker_count) {
        pthread_mutex_unlock(&g_job_lock);
        com_release(&p->obj);
        return 0;
    }
    while (g_job_count >= BD12_PSO_QUEUE_LIMIT)
        pthread_cond_wait(&g_job_space, &g_job_lock);
    p->job_next = NULL;
    if (g_job_tail) g_job_tail->job_next = p; else g_job_head = p;
    g_job_tail = p;
    ++g_job_count;
    pthread_cond_signal(&g_job_cond);
    pthread_mutex_unlock(&g_job_lock);
    return 1;
}

static void pso_wait(ComPso *p)
{
    pthread_mutex_lock(&p->lock);
    if (p->state == 0) {
        /* A PSO needed by command recording must not sit behind the entire
         * startup queue. Remove that still-pending job and compile it on this
         * thread. This preserves bounded worker concurrency without turning a
         * deep asynchronous queue into minutes of startup latency. */
        pthread_mutex_unlock(&p->lock);
        pthread_mutex_lock(&g_job_lock);
        pthread_mutex_lock(&p->lock);
        if (p->state == 0) {
            ComPso **link = &g_job_head;
            while (*link && *link != p) link = &(*link)->job_next;
            if (*link == p) {
                *link = p->job_next;
                if (g_job_count) --g_job_count;
                pthread_cond_broadcast(&g_job_space);
                if (g_job_tail == p) {
                    g_job_tail = g_job_head;
                    while (g_job_tail && g_job_tail->job_next)
                        g_job_tail = g_job_tail->job_next;
                }
                p->job_next = NULL;
                p->state = 1;
                pthread_mutex_unlock(&p->lock);
                pthread_mutex_unlock(&g_job_lock);
                pso_compile(p);
                com_release(&p->obj); /* consume queued-job reference */
                return;
            }
        }
        pthread_mutex_unlock(&g_job_lock);
        if (p->state == 0) {
            /* Lazy PSOs are not queued. The first command list that actually
             * binds one owns its compilation; other binders wait below. */
            p->state = 1;
            pthread_mutex_unlock(&p->lock);
            pso_compile(p);
            return;
        }
    }
    while (p->state < 2) pthread_cond_wait(&p->cond, &p->lock);
    pthread_mutex_unlock(&p->lock);
}

static void pso_free(ComDevice *dev, void *ptr)
{
    (void)dev;
    ComPso *p = ptr;
    if (p->root_signature) com_release(p->root_signature);
    {   static uint64_t freed;
        const uint64_t n = __atomic_add_fetch(&freed, 1, __ATOMIC_RELAXED);
        if (getenv("BEER_BD12_PIPELIB_LOG") && (n == 1 || n % 2000 == 0))
            fprintf(stderr, "[PIPELIB] pipelines destroyed=%llu\n", (unsigned long long)n);
    }
    if (p->gfx) bd12_graphics_pipeline_destroy(p->gfx);
    if (p->cmp) bd12_compute_pipeline_destroy(p->cmp);
    pthread_mutex_destroy(&p->lock);
    pthread_cond_destroy(&p->cond);
    free(p);
}
static void pso_destroy(ComObj *o) { com_defer(o->dev, pso_free, o); }
static HR MS pso_query(ComObj *s, const uint8_t *r, void **o)
{ return com_obj_query(s, r, o, iid_pso, NULL); }
static HR MS pso_cached_blob(ComObj *s, void **blob)
{
    (void)s;
    {   static uint64_t calls;
        const uint64_t n = __atomic_add_fetch(&calls, 1, __ATOMIC_RELAXED);
        if (getenv("BEER_BD12_PIPELIB_LOG") && (n == 1 || n % 500 == 0))
            fprintf(stderr, "[PIPELIB] GetCachedBlob calls=%llu\n", (unsigned long long)n);
    }
    /* Windows always returns a blob here, and games use it without checking the result (Where Winds Meet read
     * through the NULL blob). BD12 cannot serialise a pipeline, so the blob only identifies its producer; given back
     * as CachedPSO it is ignored and the pipeline is built from its description (the persistent VkPipelineCache
     * keeps that cheap). */
    extern void *beer_d3d_blob_create(const void *data, size_t size);
    static const char header[16] = "BEER-BD12-PSO-1";
    if (!blob) return HR_E_INVALIDARG;
    *blob = beer_d3d_blob_create(header, sizeof(header));
    return *blob ? HR_OK : HR_E_OUTOFMEMORY;
}
/* ID3D12PipelineLibrary records whether a stored pipeline is a compute pipeline. */
int com_pso_is_compute(ComObj *pso)
{
    return pso && pso->kind == COM_PSO && ((ComPso *)pso)->compute;
}
static void *pso_vtable[9] = {
    [0] = pso_query, [1] = com_add_ref, [2] = com_release,
    [3] = com_obj_get_private, [4] = com_obj_set_private,
    [5] = com_obj_set_private_iface, [6] = com_obj_set_name,
    [7] = com_obj_get_device, [8] = pso_cached_blob,
};

/* Set while ID3D12PipelineLibrary::Load*Pipeline creates a pipeline. Such pipelines are built at load, on the
 * game's calling thread, like any other with compile-at-create: compiling them at first use put driver compiles
 * (or at best pipeline-cache lookups) on the render thread mid-frame. BEER_BD12_LAZY_LIBRARY_LOADS=1 restores
 * first-use compilation for library loads (for comparison). */
__thread int g_bd12_pso_from_library;

static int compile_at_create(void)
{
    if (g_bd12_pso_from_library) {
        static int lazy = -1;
        if (lazy < 0) lazy = getenv("BEER_BD12_LAZY_LIBRARY_LOADS") != NULL;
        if (lazy) return 0;
    }
    static int value = -1;
    if (value < 0) {
        const char *set = getenv("BEER_BD12_COMPILE_AT_CREATE");
        value = set && *set ? strcmp(set, "0") != 0 : bd12_pipeline_library_enabled();
    }
    return value;
}

HR com_create_pso(ComDevice *dev, const void *desc, int kind,
                  const uint8_t *riid, void **out)
{
    if (!desc || !riid || !out) return HR_E_INVALIDARG;
    *out = NULL;
    if (!com_iid_equal(riid, iid_pso) && !com_iid_equal(riid, com_iid_iunknown))
        return HR_E_NOINTERFACE;
    BD12PsoDesc tmp;
    bd12_pso_defaults(&tmp);
    if (kind == 0) {
        bd12_pso_parse_graphics(desc, &tmp);
    } else if (kind == 1) {
        bd12_pso_parse_compute(desc, &tmp);
    } else {
        const uint8_t *d = desc;
        bd12_pso_parse_stream(rdptr(d + 8), (size_t)rd64(d), &tmp);
    }
    ComPso *p = calloc(1, sizeof(*p));
    if (!p) return HR_E_OUTOFMEMORY;
    p->compute = tmp.cs_size != 0;
    if (!p->compute && !tmp.vs_size && !(tmp.ms_size && dev->bd->CmdDrawMeshTasksEXT)) {
        static unsigned logged, total;
        ++total;
        if (logged++ < 2 || !(total % 500)) {
            fprintf(stderr, "[BD12] graphics PSO without VS rejected #%u (kind %d, ms %zu, as %zu, ps %zu)\n",
                    total, kind, tmp.ms_size, tmp.as_size, tmp.ps_size);
            if (kind == 2 && logged <= 2) {         /* the raw stream, 8 bytes per word */
                const uint8_t *d = desc, *stream = rdptr(d + 8);
                const size_t size = (size_t)rd64(d);
                fprintf(stderr, "[BD12]   stream %p size %zu:", (const void *)stream, size);
                for (size_t off = 0; off + 8 <= size && off < 1024; off += 8)
                    fprintf(stderr, "%s%zu:%016llx", (off % 64) ? " " : "\n[BD12]    ", off,
                            (unsigned long long)rd64(stream + off));
                fprintf(stderr, "\n");
            }
        }
        if (!beer_fake_caps()) {
            free(p);
            return HR_E_INVALIDARG;
        }
        /* BEER_FAKE_CAPS: a mesh/amplification pipeline (no VS or CS) becomes an inert pipeline: it binds as no
         * pipeline and its draws are dropped. */
        p->state = 3;
        pthread_mutex_init(&p->lock, NULL);
        pthread_cond_init(&p->cond, NULL);
        com_obj_init(&p->obj, COM_PSO, pso_vtable, dev, pso_destroy);
        *out = p;
        return HR_OK;
    }
    {   static uint64_t creates, cached;   /* CachedPSO blobs offered with the description */
        const uint64_t n = __atomic_add_fetch(&creates, 1, __ATOMIC_RELAXED);
        if (tmp.cached_pso_size) __atomic_add_fetch(&cached, 1, __ATOMIC_RELAXED);
        if (getenv("BEER_BD12_PIPELIB_LOG") && (n == 1 || n % 500 == 0))
            fprintf(stderr, "[PIPELIB] CreatePipelineState=%llu (with CachedPSO %llu)\n",
                    (unsigned long long)n, (unsigned long long)cached);
    }
    if (!bd12_pso_clone(&tmp, &p->desc)) {
        free(p);
        return HR_E_OUTOFMEMORY;
    }
    p->hash = p->compute ? fnv(p->desc.cs, p->desc.cs_size) : 0;
    if (p->desc.root_signature && ((ComObj *)p->desc.root_signature)->kind == COM_ROOT_SIG) {
        p->root_signature = p->desc.root_signature;
        com_add_ref(p->root_signature);
    }
    pthread_mutex_init(&p->lock, NULL);
    pthread_cond_init(&p->cond, NULL);
    com_obj_init(&p->obj, COM_PSO, pso_vtable, dev, pso_destroy);
    /* D3D12 permits deferred PSO compilation. RE8 creates a large library at
     * startup but binds only a small working set before the title. Compiling
     * the entire library eagerly made the NVIDIA driver retain multiple GiB.
     * Keep asynchronous eager compilation available only for profiling. */
    if (getenv("BEER_BD12_ASYNC_PSO") && !pso_enqueue(p)) pso_compile(p);
    /* Compile at creation, on the creating thread, as a Windows driver does: BEER_BD12_COMPILE_AT_CREATE=1, and by
     * default whenever pipeline libraries are offered (unless BEER_D3D12_PIPELINE_LIBRARY=0). An engine's "compiling
     * shaders" step then really builds every pipeline into the on-disk Vulkan pipeline cache, so gameplay builds
     * come from the cache instead of stalling frames. BEER_BD12_COMPILE_AT_CREATE=0 keeps first-use compilation. */
    else if (compile_at_create()) {
        p->state = 1;
        if (keep_pipelines()) pso_compile(p);
        else pso_warm(p);
    }
    *out = p;
    return HR_OK;
}

/* Allocators ---------------------------------------------------------------------- */

typedef struct {
    BD12CmdList *list;
    ComList *owner;
} AllocEntry;

typedef struct ComAllocator {
    ComObj obj;
    BD12CmdAllocator *a;
    uint32_t type;
    pthread_mutex_t lock;
    AllocEntry *entries;
    uint32_t entry_count, entry_capacity;
    ComPso **retained;
    uint32_t retained_count, retained_capacity;
    uint64_t gen;
} ComAllocator;

struct ComList {
    ComObj obj;
    uint32_t type;
    ComAllocator *alloc;
    BD12CmdList *cur;
    int open;
};

static void alloc_release_retained(ComAllocator *a)
{
    for (uint32_t i = 0; i < a->retained_count; ++i)
        com_release(&a->retained[i]->obj);
    a->retained_count = 0;
}

static void alloc_free(ComDevice *dev, void *ptr)
{
    (void)dev;
    ComAllocator *a = ptr;
    for (uint32_t i = 0; i < a->entry_count; ++i)
        bd12_cmd_list_destroy(a->entries[i].list, a->a);
    free(a->entries);
    alloc_release_retained(a);
    free(a->retained);
    bd12_cmd_allocator_destroy(a->a);
    pthread_mutex_destroy(&a->lock);
    free(a);
}
static void alloc_destroy(ComObj *o) { com_defer(o->dev, alloc_free, o); }
static HR MS alloc_query(ComObj *s, const uint8_t *r, void **o)
{ return com_obj_query(s, r, o, iid_allocator, NULL); }

static HR MS alloc_reset(ComAllocator *a)
{
    pthread_mutex_lock(&a->lock);
    for (uint32_t i = 0; i < a->entry_count; ++i) {
        ComList *owner = a->entries[i].owner;
        if (owner && owner->cur == a->entries[i].list) {
            owner->cur = NULL;
            owner->open = 0;
        }
    }
    int ok = bd12_cmd_allocator_reset(a->a);
    for (uint32_t i = 0; i < a->entry_count; ++i)
        bd12_cmd_list_destroy(a->entries[i].list, a->a);
    a->entry_count = 0;
    alloc_release_retained(a);
    ++a->gen;
    pthread_mutex_unlock(&a->lock);
    return ok ? HR_OK : HR_E_FAIL;
}

static void *alloc_vtable[9] = {
    [0] = alloc_query, [1] = com_add_ref, [2] = com_release,
    [3] = com_obj_get_private, [4] = com_obj_set_private,
    [5] = com_obj_set_private_iface, [6] = com_obj_set_name,
    [7] = com_obj_get_device, [8] = alloc_reset,
};

HR com_create_allocator(ComDevice *dev, uint32_t type, const uint8_t *riid,
                        void **out)
{
    if (!riid || !out) return HR_E_INVALIDARG;
    *out = NULL;
    if (!com_iid_equal(riid, iid_allocator) &&
        !com_iid_equal(riid, com_iid_iunknown))
        return HR_E_NOINTERFACE;
    ComAllocator *a = calloc(1, sizeof(*a));
    if (!a) return HR_E_OUTOFMEMORY;
    a->a = bd12_cmd_allocator_create(dev->bd);
    if (!a->a) { free(a); return HR_E_FAIL; }
    a->type = type;
    pthread_mutex_init(&a->lock, NULL);
    com_obj_init(&a->obj, COM_ALLOCATOR, alloc_vtable, dev, alloc_destroy);
    *out = a;
    return HR_OK;
}

/* Command lists --------------------------------------------------------------------- */

static void list_destroy(ComObj *o)
{
    ComList *l = (ComList *)o;
    ComAllocator *a = l->alloc;
    if (a) {
        pthread_mutex_lock(&a->lock);
        for (uint32_t i = 0; i < a->entry_count; ++i)
            if (a->entries[i].owner == l) a->entries[i].owner = NULL;
        pthread_mutex_unlock(&a->lock);
        com_release(&a->obj);
    }
    free(l);
}

static HR MS list_query(ComObj *s, const uint8_t *r, void **o)
{
    if (r && com_iid_equal(r, iid_command_list))
        return com_obj_query(s, r, o, iid_command_list, NULL);
    if (r && (s->dev->bd->rt_enabled || beer_fake_caps()) &&
        (com_iid_equal(r, iid_list1) || com_iid_equal(r, iid_list2) ||
         com_iid_equal(r, iid_list3) || com_iid_equal(r, iid_list4)))
        return com_obj_query(s, r, o, r, NULL);
    if (r && beer_fake_caps() && (com_iid_equal(r, iid_list5) || com_iid_equal(r, iid_list6)))
        return com_obj_query(s, r, o, r, NULL);
    /* List6 (DispatchMesh, and everything before it) whenever mesh shaders are real. */
    if (r && s->dev->bd->CmdDrawMeshTasksEXT &&
        (com_iid_equal(r, iid_list1) || com_iid_equal(r, iid_list2) || com_iid_equal(r, iid_list3) ||
         com_iid_equal(r, iid_list4) || com_iid_equal(r, iid_list5) || com_iid_equal(r, iid_list6)))
        return com_obj_query(s, r, o, r, NULL);
    /* List5 (and the List1..4 it derives from) whenever variable-rate shading is real. */
    if (r && s->dev->bd->CmdSetFragmentShadingRateKHR &&
        (com_iid_equal(r, iid_list1) || com_iid_equal(r, iid_list2) || com_iid_equal(r, iid_list3) ||
         com_iid_equal(r, iid_list4) || com_iid_equal(r, iid_list5)))
        return com_obj_query(s, r, o, r, NULL);
    return com_obj_query(s, r, o, iid_list, NULL);
}

/* ID3D12GraphicsCommandList::ClearState resets *binding* state only. It does
 * not discard commands already recorded into this list, so execution-affecting
 * bookkeeping (segment count, watchdog classification, flight history) must be
 * preserved. Clearing has_watchdog_dispatch here previously sent an already
 * segmented list down the ordinary path. */
static void list_reset_state(BD12CmdList *c)
{
    bd12_cmd_end_rendering(c);
    memset(c->heaps, 0, sizeof(c->heaps));
    c->compute_pipeline = NULL;
    memset(&c->compute, 0, sizeof(c->compute));
    memset(&c->graphics, 0, sizeof(c->graphics));
    memset(&c->gfx, 0, sizeof(c->gfx));
    c->gfx.dirty = BD12_DIRTY_ALL;

    c->gfx.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    c->gfx.index_type = VK_INDEX_TYPE_UINT16;
}

static int call_log_on(void);
static void call_note(const void *list, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void list_set_pso(ComList *l, ComPso *p)
{
    BD12CmdList *c = l->cur;
    if (!p) return;
    if (c && call_log_on()) call_note(c, "pso %016llx%s", (unsigned long long)p->hash, p->compute ? " (compute)" : "");
    ComAllocator *a = l->alloc;
    if (p->last_alloc != a || p->last_gen != a->gen) {
        pthread_mutex_lock(&a->lock);
        if (a->retained_count == a->retained_capacity) {
            uint32_t cap = a->retained_capacity ? a->retained_capacity * 2 : 64;
            ComPso **grown = realloc(a->retained, cap * sizeof(*grown));
            if (!grown) { pthread_mutex_unlock(&a->lock); return; }
            a->retained = grown;
            a->retained_capacity = cap;
        }
        com_add_ref(&p->obj);
        a->retained[a->retained_count++] = p;
        pthread_mutex_unlock(&a->lock);
        p->last_alloc = a;
        p->last_gen = a->gen;
    }
    pso_wait(p);
    if (!(p->compute ? (void *)p->cmp : (void *)p->gfx)) {
        BD12_DROP(BD12_DROP_PSO_FAILED);
        static unsigned failures;
        if (failures++ < 8)
            fprintf(stderr, "[BD12] %s pipeline %016llx failed to compile; its draws/dispatches are dropped\n",
                    p->compute ? "compute" : "graphics", (unsigned long long)p->hash);
    }
    if (p->compute) bd12_cmd_set_pipeline(c, p->cmp);
    else bd12_cmd_set_graphics_pipeline(c, p->gfx);
}

static int list_start(ComList *l, ComAllocator *a, ComPso *pso)
{
    BD12CmdList *c = bd12_cmd_list_create(a->a);
    if (!c) return 0;
    pthread_mutex_lock(&a->lock);
    if (a->entry_count == a->entry_capacity) {
        uint32_t cap = a->entry_capacity ? a->entry_capacity * 2 : 16;
        AllocEntry *grown = realloc(a->entries, cap * sizeof(*grown));
        if (!grown) {
            pthread_mutex_unlock(&a->lock);
            bd12_cmd_list_destroy(c, a->a);
            return 0;
        }
        a->entries = grown;
        a->entry_capacity = cap;
    }
    a->entries[a->entry_count++] = (AllocEntry){ c, l };
    pthread_mutex_unlock(&a->lock);
    if (l->alloc != a) {
        com_add_ref(&a->obj);
        if (l->alloc) {
            ComAllocator *old = l->alloc;
            pthread_mutex_lock(&old->lock);
            for (uint32_t i = 0; i < old->entry_count; ++i)
                if (old->entries[i].owner == l) old->entries[i].owner = NULL;
            pthread_mutex_unlock(&old->lock);
            com_release(&old->obj);
        }
        l->alloc = a;
    }
    l->cur = c;
    if (!bd12_cmd_list_begin(c)) { l->cur = NULL; return 0; }
    if (call_log_on()) call_note(c, "reset");
    l->open = 1;
    if (pso) list_set_pso(l, pso);
    return 1;
}

HR com_create_list(ComDevice *dev, uint32_t type, ComObj *allocator,
                   ComObj *pso, const uint8_t *riid, void **out)
{
    if (!allocator || !riid || !out) return HR_E_INVALIDARG;
    *out = NULL;
    /* CreateCommandList may name any interface the list implements (games ask for ID3D12GraphicsCommandList4/6
     * directly); the list answers it exactly as QueryInterface would. Refusing every newer interface here left
     * those lists uncreated. */
    const int base = com_iid_equal(riid, iid_list) || com_iid_equal(riid, iid_command_list) ||
                     com_iid_equal(riid, com_iid_iunknown);
    if (!base) {
        static const uint8_t *const newer[] = { iid_list1, iid_list2, iid_list3, iid_list4, iid_list5, iid_list6 };
        int known = 0;
        for (size_t i = 0; i < sizeof(newer) / sizeof(newer[0]); ++i)
            if (com_iid_equal(riid, newer[i])) {
                static unsigned logged_mask;
                if (!(__atomic_fetch_or(&logged_mask, 1u << i, __ATOMIC_RELAXED) & (1u << i)))
                    fprintf(stderr, "[BD12] CreateCommandList asks for ID3D12GraphicsCommandList%u\n", (unsigned)i + 1);
                known = 1;
            }
        if (!known) return HR_E_NOINTERFACE;
    }
    ComList *l = calloc(1, sizeof(*l));
    if (!l) return HR_E_OUTOFMEMORY;
    l->type = type;
    com_obj_init(&l->obj, COM_LIST, NULL, dev, list_destroy);
    extern void *com_list_vtable_[80];
    l->obj.vtable = com_list_vtable_;
    if (!list_start(l, (ComAllocator *)allocator, (ComPso *)pso)) {
        com_release(&l->obj);
        return HR_E_FAIL;
    }
    if (!base) {
        void *iface = NULL;
        const HR hr = list_query(&l->obj, riid, &iface);
        com_release(&l->obj);       /* the query holds the reference now, or the list is gone */
        if (hr != HR_OK) {
            static int logged;
            if (logged++ < 3) fprintf(stderr, "[BD12] CreateCommandList for a newer list interface refused (not enabled)\n");
            return hr;
        }
        *out = iface;
        return HR_OK;
    }
    *out = l;
    return HR_OK;
}

BD12CmdList *com_list_finish(ComObj *obj)
{
    ComList *l = (ComList *)obj;
    if (!l->cur) return NULL;
    if (l->open) {
        l->open = 0;
        if (!bd12_cmd_list_end(l->cur)) return NULL;
    } else if (l->cur->recording) {
        return NULL;
    }
    return l->cur;
}

#define LIST_CUR(l) bd12_canary_check(__func__); BD12CmdList *c = (l)->open ? (l)->cur : NULL; if (!c) return
#define LIST_CUR_V(l, v) bd12_canary_check(__func__); BD12CmdList *c = (l)->open ? (l)->cur : NULL; if (!c) return (v)

/* The commands being recorded by an open ID3D12GraphicsCommandList, for NVAPI's CUDA launches; NULL when closed. */
BD12CmdList *com_list_open_commands(ComObj *list)
{
    if (!list || list->kind != COM_LIST) return NULL;
    ComList *l = (ComList *)list;
    return l->open ? l->cur : NULL;
}

static uint32_t MS cl_get_type(ComList *l) { return l->type; }

static HR MS cl_close(ComList *l)
{
    if (!l->open || !l->cur) return HR_E_FAIL;
    l->open = 0;
    return bd12_cmd_list_end(l->cur) ? HR_OK : HR_E_FAIL;
}

static HR MS cl_reset(ComList *l, ComAllocator *a, ComPso *pso)
{
    if (!a) return HR_E_INVALIDARG;
    if (l->open && l->cur) {
        l->open = 0;
        bd12_cmd_list_end(l->cur);
    }
    return list_start(l, a, pso) ? HR_OK : HR_E_FAIL;
}

static void MS cl_clear_state(ComList *l, ComPso *pso)
{
    LIST_CUR(l);
    list_reset_state(c);
    if (pso) list_set_pso(l, pso);
}

static void MS cl_draw(ComList *l, uint32_t vc, uint32_t ic, uint32_t sv, uint32_t si)
{
    LIST_CUR(l);
    bd12_cmd_draw(c, vc, ic, sv, si);
}

static void MS cl_draw_indexed(ComList *l, uint32_t ic, uint32_t inst,
                               uint32_t si, int32_t bv, uint32_t sinst)
{
    LIST_CUR(l);
    if (bd12_trace_active() && c->gfx.pipeline)
        fprintf(stderr, "[DRAW] indexed list=%p pso=%016llx indices=%u instances=%u start=%u base=%d\n",
                (void *)c, (unsigned long long)c->gfx.pipeline->shader_hash, ic, inst, si, bv);
    bd12_cmd_draw_indexed(c, ic, inst, si, bv, sinst);
}


/* BEER_BD12_CALL_LOG=1 with BEER_BD12_TRACE_CS=<hash>: every list keeps a ring of its last 48 compute binding calls
 * (root signature, tables, pipeline) and prints it when it dispatches that compute shader, to see which arguments
 * the dispatch was given. Diagnostic only; rings are keyed by list pointer (collisions overwrite). */
#define CALL_RING 48
typedef struct { const void *list; uint32_t pos; char line[CALL_RING][112]; } CallRing;
static CallRing g_call_rings[256];
static int call_log_on(void)
{
    static int on = -1;
    if (on < 0) on = getenv("BEER_BD12_CALL_LOG") != NULL;
    return on;
}
static uint64_t call_log_hash(void)
{
    static uint64_t h = 1;
    if (h == 1) { const char *v = getenv("BEER_BD12_TRACE_CS"); h = v ? strtoull(v, NULL, 16) : 0; }
    return h;
}
static CallRing *call_ring(const void *list)
{
    CallRing *r = &g_call_rings[((uintptr_t)list >> 6) & 255];
    if (r->list != list) { r->list = list; r->pos = 0; memset(r->line, 0, sizeof(r->line)); }
    return r;
}
static void call_note(const void *list, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void call_note(const void *list, const char *fmt, ...)
{
    CallRing *r = call_ring(list);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r->line[r->pos++ % CALL_RING], sizeof(r->line[0]), fmt, ap);
    va_end(ap);
}
static void call_dump(const void *list, uint64_t cs)
{
    static _Atomic unsigned dumps;
    if (atomic_fetch_add(&dumps, 1) >= 6) return;
    CallRing *r = call_ring(list);
    fprintf(stderr, "[CALL] list=%p dispatch of %016llx; its last binding calls:\n", list, (unsigned long long)cs);
    for (uint32_t i = 0; i < CALL_RING; ++i) {
        const char *line = r->line[(r->pos + i) % CALL_RING];
        if (line[0]) fprintf(stderr, "[CALL]   %s\n", line);
    }
}
static void MS cl_dispatch(ComList *l, uint32_t x, uint32_t y, uint32_t z)
{
    LIST_CUR(l);
    if (call_log_on()) {
        const uint64_t cs = c->compute_pipeline ? c->compute_pipeline->shader_hash : 0;
        if (cs && cs == call_log_hash()) call_dump(c, cs);
        call_note(c, "dispatch %u,%u,%u cs=%016llx rs=%p", x, y, z, (unsigned long long)cs,
                  (void *)c->compute.root_signature);
    }
    bd12_cmd_dispatch_bound(c, x, y, z);
}

static void MS cl_copy_buffer_region(ComList *l, ComResource *dst,
                                     uint64_t dst_off, ComResource *src,
                                     uint64_t src_off, uint64_t bytes)
{
    LIST_CUR(l);
    if (!dst || !src || !dst->buffer || !src->buffer) return;
    bd12_cmd_copy_buffer(c, dst->buffer, dst_off, src->buffer, src_off, bytes);
}

static void mip_extent(const ComResource *r, uint32_t sub, uint32_t *w,
                       uint32_t *h, uint32_t *d)
{
    uint32_t mip = r->mips ? sub % r->mips : 0;
    uint64_t ww = r->width >> mip;
    uint32_t hh = r->height >> mip;
    uint32_t dd = r->dimension == 4 ? r->depth_or_layers >> mip : 1;
    *w = ww ? (uint32_t)ww : 1;
    *h = hh ? hh : 1;
    *d = dd ? dd : 1;
}

static uint32_t clamp_extent(uint32_t want, uint32_t offset, uint32_t limit)
{
    return offset >= limit ? 0 : (want > limit - offset ? limit - offset : want);
}

/* ID3D12GraphicsCommandList::CopyTiles(tiled, D3D12_TILED_RESOURCE_COORDINATE {x, y, z, subresource},
 * D3D12_TILE_REGION_SIZE {u32 tiles, BOOL use_box, u32 width, u16 height, u16 depth}, buffer, offset, flags).
 * Streaming engines upload texture tiles this way into reserved resources. Reserved resources are fully resident
 * here, so each 64 KiB tile is an ordinary copy between the buffer (one tile after another, texels row-major within
 * the tile's standard shape) and the tile's texel box. Without a box the tiles run X, then Y, then Z, then on into
 * the next subresource. Flag 0x4 copies the resource into the buffer; otherwise the buffer goes into the resource.
 * Tiles in a packed mip tail have an opaque layout and are skipped (logged once). */
static void MS cl_copy_tiles(ComList *l, ComResource *tiled, const uint32_t *coord, const uint8_t *region,
                             ComResource *buf, uint64_t offset, uint32_t flags)
{
    LIST_CUR(l);
    if (!tiled || !coord || !region || !buf || !buf->buffer) return;
    const int to_buffer = (flags & 0x4) != 0;
    const uint32_t count = rd32(region), use_box = rd32(region + 4);
    const uint32_t box_w = rd32(region + 8);
    const uint16_t box_h = *(const uint16_t *)(region + 12), box_d = *(const uint16_t *)(region + 14);
    static _Atomic int logged;
    if (atomic_load(&logged) < 4) {
        atomic_fetch_add(&logged, 1);
        fprintf(stderr, "[BD12] CopyTiles %s tiles=%u box=%u(%ux%ux%u) at %u,%u,%u sub=%u fmt=%u %llux%u flags=0x%x\n",
                to_buffer ? "resource->buffer" : "buffer->resource", count, use_box, box_w, box_h, box_d,
                coord[0], coord[1], coord[2], coord[3], tiled->dxgi_format, (unsigned long long)tiled->width,
                tiled->height, flags);
    }
    if (tiled->dimension == 1) {                         /* buffer: 64 KiB tiles in a row */
        if (!tiled->buffer) return;
        const uint64_t bytes = (uint64_t)(use_box ? box_w : count) * 65536u;
        const uint64_t at = (uint64_t)coord[0] * 65536u;
        if (to_buffer) bd12_cmd_copy_buffer(c, buf->buffer, offset, tiled->buffer, at, bytes);
        else bd12_cmd_copy_buffer(c, tiled->buffer, at, buf->buffer, offset, bytes);
        return;
    }
    if (!tiled->image) return;
    uint32_t shape[3];
    bd12_standard_tile_shape(tiled, shape);
    uint32_t bw = 1, bh = 1, bytes = 4;
    const VkFormat vk = bd12_dxgi_to_vk(tiled->dxgi_format);
    if (vk != VK_FORMAT_UNDEFINED) bd12_format_block(vk, VK_IMAGE_ASPECT_COLOR_BIT, &bw, &bh, &bytes);
    if (!bytes) bytes = 4;
    const uint32_t pitch = (shape[0] / bw) * bytes;
    const uint32_t mips = tiled->mips ? tiled->mips : 1;
    const uint32_t subresources = mips * (tiled->dimension == 4 ? 1 : (tiled->depth_or_layers ? tiled->depth_or_layers : 1));
    uint32_t sub = coord[3], tx = coord[0], ty = coord[1], tz = coord[2];
    const uint32_t total = use_box ? box_w * box_h * box_d : count;
    for (uint32_t i = 0; i < total; ++i) {
        if (use_box) {
            const uint32_t plane = box_w * box_h;
            tx = coord[0] + i % box_w; ty = coord[1] + (i % plane) / box_w; tz = coord[2] + i / plane;
        }
        if (sub >= subresources) break;
        uint32_t mw, mh, md;
        mip_extent(tiled, sub, &mw, &mh, &md);
        if (mw < shape[0] || mh < shape[1] || (tiled->dimension == 4 && md < shape[2])) {   /* packed tail */
            static _Atomic int packed_logged;
            if (!atomic_exchange(&packed_logged, 1))
                fprintf(stderr, "[BD12] CopyTiles into a packed mip tail (sub %u, %ux%u): skipped\n", sub, mw, mh);
            break;
        }
        const uint32_t wt = (mw + shape[0] - 1) / shape[0], ht = (mh + shape[1] - 1) / shape[1];
        const uint32_t dt = (md + shape[2] - 1) / shape[2];
        if (!use_box) {                                  /* linear walk: wrap into the next row/slice/subresource */
            if (tx >= wt) { tx = 0; ++ty; }
            if (ty >= ht) { ty = 0; ++tz; }
            if (tz >= dt) {
                tz = 0; ++sub; --i;                      /* retry this tile in the next subresource */
                continue;
            }
        }
        const uint32_t x = tx * shape[0], y = ty * shape[1], z = tz * shape[2];
        const uint32_t w = clamp_extent(shape[0], x, mw), h = clamp_extent(shape[1], y, mh);
        const uint32_t d = clamp_extent(shape[2], z, md);
        if (w && h && d) {
            const uint64_t at = offset + (uint64_t)i * 65536u;
            if (to_buffer)
                bd12_cmd_copy_image_to_buffer(c, buf->buffer, at, pitch, tiled->image,
                    (BD12ImagePoint){ sub, (int32_t)x, (int32_t)y, (int32_t)z }, w, h, d);
            else
                bd12_cmd_copy_buffer_to_image(c, tiled->image,
                    (BD12ImagePoint){ sub, (int32_t)x, (int32_t)y, (int32_t)z }, buf->buffer, at, pitch, w, h, d);
        }
        if (!use_box) ++tx;
    }
}

static void MS cl_copy_texture_region(ComList *l, const uint8_t *dst,
                                      uint32_t dx, uint32_t dy, uint32_t dz,
                                      const uint8_t *src, const uint8_t *box)
{
    LIST_CUR(l);
    if (!dst || !src) return;
    ComResource *dr = rdptr(dst), *sr = rdptr(src);
    uint32_t dtype = rd32(dst + 8), stype = rd32(src + 8);
    if (!dr || !sr) return;
    uint32_t bl = 0, bt = 0, bf = 0, br = 0, bb = 0, bk = 0;
    if (box) {
        bl = rd32(box); bt = rd32(box + 4); bf = rd32(box + 8);
        br = rd32(box + 12); bb = rd32(box + 16); bk = rd32(box + 20);
    }
    if (dtype == 0 && stype == 1 && dr->image && sr->buffer) {
        uint64_t off = rd64(src + 16);
        uint32_t fmt = rd32(src + 24), fw = rd32(src + 28), fh = rd32(src + 32);
        uint32_t fd = rd32(src + 36), pitch = rd32(src + 40);
        uint32_t w = box ? br - bl : fw, h = box ? bb - bt : fh;
        uint32_t d = box ? bk - bf : fd;
        if (box) {
            uint32_t bytes = bd12_dxgi_bytes(fmt);
            int block = bd12_dxgi_is_compressed(fmt);
            uint32_t rows = block ? (fh + 3) / 4 : fh;
            off += (uint64_t)bf * pitch * rows +
                   (uint64_t)(block ? bt / 4 : bt) * pitch +
                   (uint64_t)(block ? bl / 4 : bl) * bytes;
        }
        uint32_t sub = rd32(dst + 16), mw, mh, md;
        mip_extent(dr, sub, &mw, &mh, &md);
        w = clamp_extent(w, dx, mw);
        h = clamp_extent(h, dy, mh);
        d = clamp_extent(d, dz, md);
        if (!w || !h || !d) return;
        if (w >= 960 && getenv("BEER_BD12_UPLOAD_LOG")) {     /* large uploads: video planes, streamed textures */
            static uint64_t seen[128];
            static _Atomic uint32_t seen_count;
            const uint64_t key = ((uint64_t)fmt << 48) ^ ((uint64_t)w << 32) ^ ((uint64_t)h << 16) ^ dr->dxgi_format;
            int known = 0;
            const uint32_t count = atomic_load(&seen_count);
            for (uint32_t i = 0; i < count && i < 128 && !known; ++i) known = seen[i] == key;
            if (!known && count < 128) { seen[atomic_fetch_add(&seen_count, 1) % 128] = key; }
            if (!known && count < 128)
                fprintf(stderr, "[BD12] upload %ux%u footprint fmt=%u pitch=%u off=%llu -> texture fmt=%u %llux%u sub=%u at %u,%u\n",
                        w, h, fmt, pitch, (unsigned long long)off, dr->dxgi_format, (unsigned long long)dr->width,
                        dr->height, sub, dx, dy);
        }
        bd12_cmd_copy_buffer_to_image(c, dr->image,
            (BD12ImagePoint){ sub, (int32_t)dx, (int32_t)dy, (int32_t)dz },
            sr->buffer, off, pitch, w, h, d);
    } else if (dtype == 1 && stype == 0 && dr->buffer && sr->image) {
        uint64_t off = rd64(dst + 16);
        uint32_t fmt = rd32(dst + 24), fw = rd32(dst + 28), fh = rd32(dst + 32);
        uint32_t fd = rd32(dst + 36), pitch = rd32(dst + 40);
        uint32_t sub = rd32(src + 16), mw, mh, md;
        mip_extent(sr, sub, &mw, &mh, &md);
        uint32_t w = box ? br - bl : fw, h = box ? bb - bt : fh;
        uint32_t d = box ? bk - bf : fd;
        w = clamp_extent(w, bl, mw);
        h = clamp_extent(h, bt, mh);
        d = clamp_extent(d, bf, md);
        if (!w || !h || !d) return;
        if (dx || dy || dz) {
            uint32_t bytes = bd12_dxgi_bytes(fmt);
            int block = bd12_dxgi_is_compressed(fmt);
            uint32_t rows = block ? (fh + 3) / 4 : fh;
            off += (uint64_t)dz * pitch * rows +
                   (uint64_t)(block ? dy / 4 : dy) * pitch +
                   (uint64_t)(block ? dx / 4 : dx) * bytes;
        }
        bd12_cmd_copy_image_to_buffer(c, dr->buffer, off, pitch, sr->image,
            (BD12ImagePoint){ sub, (int32_t)bl, (int32_t)bt, (int32_t)bf },
            w, h, d);
    } else if (dtype == 0 && stype == 0 && dr->image && sr->image) {
        uint32_t dsub = rd32(dst + 16), ssub = rd32(src + 16);
        uint32_t sw, sh, sd, tw, th, td;
        mip_extent(sr, ssub, &sw, &sh, &sd);
        mip_extent(dr, dsub, &tw, &th, &td);
        uint32_t w = box ? br - bl : sw, h = box ? bb - bt : sh;
        uint32_t d = box ? bk - bf : sd;
        w = clamp_extent(w, dx, tw);
        h = clamp_extent(h, dy, th);
        d = clamp_extent(d, dz, td);
        if (!w || !h || !d) return;
        bd12_cmd_copy_image(c, dr->image,
            (BD12ImagePoint){ dsub, (int32_t)dx, (int32_t)dy, (int32_t)dz },
            sr->image,
            (BD12ImagePoint){ ssub, (int32_t)bl, (int32_t)bt, (int32_t)bf },
            w, h, d);
    }
}

static void MS cl_copy_resource(ComList *l, ComResource *dst, ComResource *src)
{
    LIST_CUR(l);
    if (!dst || !src) return;
    if (dst->buffer && src->buffer) {
        VkDeviceSize n = dst->buffer->size < src->buffer->size ?
                         dst->buffer->size : src->buffer->size;
        bd12_cmd_copy_buffer(c, dst->buffer, 0, src->buffer, 0, n);
        return;
    }
    if (!dst->image || !src->image) return;
    uint32_t subs = src->mips * src->depth_or_layers * src->planes;
    if (src->dimension == 4) subs = src->mips * src->planes;
    for (uint32_t s = 0; s < subs; ++s) {
        uint32_t w, h, d, tw, th, td;
        mip_extent(src, s, &w, &h, &d);
        mip_extent(dst, s, &tw, &th, &td);
        if (tw < w) w = tw;
        if (th < h) h = th;
        if (td < d) d = td;
        bd12_cmd_copy_image(c, dst->image, (BD12ImagePoint){ s, 0, 0, 0 },
                            src->image, (BD12ImagePoint){ s, 0, 0, 0 }, w, h, d);
    }
}

static void MS cl_noop(void) {}

static void MS cl_resolve(ComList *l, ComResource *dst, uint32_t dsub,
                          ComResource *src, uint32_t ssub, uint32_t format)
{
    (void)format;
    LIST_CUR(l);
    if (dst && src && dst->image && src->image)
        bd12_cmd_resolve_image(c, dst->image, dsub, src->image, ssub);
}

/* ResolveSubresourceRegion DECODE (5) / ENCODE (4) _SAMPLER_FEEDBACK: between a feedback map and R8_UINT data.
 * MIN_MIP: the R8 side is a single-mip texture (or, decoding, a buffer); its subresource picks the array layer
 * (UINT_MAX: all), the map's subresource is ignored. MIP_REGION_USED: the R8 texture has the map's mip count and the
 * subresource is mip + layer * mips (UINT_MAX: every mip and layer; a mismatched pair takes the concrete one). */
static void feedback_resolve(BD12CmdList *c, int encode, ComResource *map, uint32_t map_sub, ComResource *r8,
                             uint32_t r8_sub, uint32_t r8_x, uint32_t r8_y, const int32_t *rect)
{
    if (!map || !map->image || (map->dxgi_format != 189 && map->dxgi_format != 190) || !map->feedback_region[0] ||
        !r8 || (!r8->image && !r8->buffer)) {
        static unsigned logged;
        if (logged++ < 5) fprintf(stderr, "[BD12] sampler-feedback %s: invalid resources, skipped\n", encode ? "encode" : "decode");
        return;
    }
    const int min_mip = map->dxgi_format == 189;
    const uint32_t mips = map->mips ? map->mips : 1, layers = map->depth_or_layers ? map->depth_or_layers : 1;
    BD12FeedbackOp op = { 0 };
    op.map = map->image;
    bd12_feedback_padded_extent(map, &op.map_pitch, &op.map_rows);
    op.mips = mips;
    op.r8_image = r8->image;
    op.r8_buffer = r8->buffer;
    op.r8_mips = r8->image ? (r8->image->desc.mip_levels ? r8->image->desc.mip_levels : 1) : 1;
    op.r8_x = (int32_t)r8_x;
    op.r8_y = (int32_t)r8_y;
    if (min_mip) {
        op.variant = encode ? BD12_FEEDBACK_ENCODE_MIN_MIP : BD12_FEEDBACK_DECODE_MIN_MIP;
        const int all = r8_sub == UINT32_MAX || r8->buffer;
        op.map_layer = op.r8_layer = all ? 0 : r8_sub;
        op.layers = r8->buffer ? 1 : all ? layers : 1;
        bd12_feedback_active_extent(map, 0, &op.w, &op.h);
        if (rect && !encode) {   /* D3D12_RECT left, top, right, bottom in region units */
            op.x0 = (uint32_t)rect[0]; op.y0 = (uint32_t)rect[1];
            op.w = (uint32_t)(rect[2] - rect[0]); op.h = (uint32_t)(rect[3] - rect[1]);
        }
        bd12_cmd_feedback_transcode(c, &op);
        return;
    }
    op.variant = encode ? BD12_FEEDBACK_ENCODE_MIP_USED : BD12_FEEDBACK_DECODE_MIP_USED;
    uint32_t sub = map_sub != UINT32_MAX ? map_sub : r8_sub;
    for (uint32_t layer = 0; layer < layers; ++layer)
        for (uint32_t mip = 0; mip < mips; ++mip) {
            if (sub != UINT32_MAX && sub != mip + layer * mips) continue;
            op.mip = op.r8_mip = mip;
            op.map_layer = op.r8_layer = layer;
            op.layers = 1;
            op.x0 = op.y0 = 0;
            bd12_feedback_active_extent(map, mip, &op.w, &op.h);
            if (rect && sub != UINT32_MAX && !encode) {
                op.x0 = (uint32_t)rect[0]; op.y0 = (uint32_t)rect[1];
                op.w = (uint32_t)(rect[2] - rect[0]); op.h = (uint32_t)(rect[3] - rect[1]);
            }
            bd12_cmd_feedback_transcode(c, &op);
        }
}

/* ID3D12GraphicsCommandList1::ResolveSubresourceRegion. Modes 0-3 (decompress/min/max/average) resolve the whole
 * subresource; 4/5 transcode sampler feedback. */
static void MS cl_resolve_region(ComList *l, ComResource *dst, uint32_t dsub, uint32_t dx, uint32_t dy,
                                 ComResource *src, uint32_t ssub, const int32_t *rect, uint32_t format, uint32_t mode)
{
    (void)format;
    LIST_CUR(l);
    if (mode == 5) { feedback_resolve(c, 0, src, ssub, dst, dsub, dx, dy, rect); return; }
    if (mode == 4) { feedback_resolve(c, 1, dst, dsub, src, ssub, dx, dy, rect); return; }
    if (dst && src && dst->image && src->image)
        bd12_cmd_resolve_image(c, dst->image, dsub, src->image, ssub);
}

static void MS cl_set_topology(ComList *l, uint32_t topology)
{
    LIST_CUR(l);
    bd12_cmd_set_d3d_topology(c, topology);
}

static void MS cl_set_viewports(ComList *l, uint32_t n, const VkViewport *v)
{
    LIST_CUR(l);
    if (n > BD12_MAX_VIEWPORTS) n = BD12_MAX_VIEWPORTS;
    bd12_cmd_set_viewports(c, v, n);
}

static void MS cl_set_scissors(ComList *l, uint32_t n, const int32_t *rects)
{
    LIST_CUR(l);
    if (n > BD12_MAX_VIEWPORTS) n = BD12_MAX_VIEWPORTS;
    VkRect2D out[BD12_MAX_VIEWPORTS];
    for (uint32_t i = 0; i < n; ++i) {
        int32_t x0 = rects[i * 4], y0 = rects[i * 4 + 1];
        int32_t x1 = rects[i * 4 + 2], y1 = rects[i * 4 + 3];
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        out[i] = (VkRect2D){ { x0, y0 },
                             { x1 > x0 ? (uint32_t)(x1 - x0) : 0,
                               y1 > y0 ? (uint32_t)(y1 - y0) : 0 } };
    }
    bd12_cmd_set_scissors(c, out, n);
}

static void MS cl_set_blend_factor(ComList *l, const float *rgba)
{
    LIST_CUR(l);
    static const float ones[4] = { 1, 1, 1, 1 };
    bd12_cmd_set_blend_constants(c, rgba ? rgba : ones);
}

static void MS cl_set_stencil_ref(ComList *l, uint32_t ref)
{
    LIST_CUR(l);
    bd12_cmd_set_stencil_ref(c, ref);
}

/* ID3D12GraphicsCommandList5 */
static void MS cl_set_shading_rate(ComList *l, uint32_t rate, const uint32_t *combiners)
{
    LIST_CUR(l);
    bd12_cmd_set_shading_rate(c, rate, combiners);
}
static void MS cl_set_shading_rate_image(ComList *l, ComResource *r)
{
    LIST_CUR(l);
    bd12_cmd_set_shading_rate_image(c, r ? r->image : NULL);
}

/* ID3D12GraphicsCommandList6 */
static void MS cl_dispatch_mesh(ComList *l, uint32_t x, uint32_t y, uint32_t z)
{
    LIST_CUR(l);
    bd12_cmd_dispatch_mesh(c, x, y, z);
}

static void MS cl_set_pso(ComList *l, ComPso *p)
{
    LIST_CUR(l);
    list_set_pso(l, p);
}

static void MS cl_barrier(ComList *l, uint32_t n, const uint8_t *barriers)
{
    LIST_CUR(l);
    /* One D3D12 ResourceBarrier call: one Vulkan dependency (see bd12_cmd_barrier_batch_begin). */
    bd12_cmd_barrier_batch_begin(c);
    for (uint32_t i = 0; i < n; ++i) {
        const uint8_t *b = barriers + (size_t)i * 32;
        uint32_t type = rd32(b), flags = rd32(b + 4);
        if (type == 0) {
            if (flags & 1) continue;  /* BEGIN_ONLY: applied at END_ONLY */
            ComResource *r = rdptr(b + 8);
            uint32_t sub = rd32(b + 16), before = rd32(b + 20),
                     after = rd32(b + 24);
            if (!r) continue;
            if (r->buffer)
                bd12_cmd_transition_buffer(c, r->buffer, before, after);
            else if (r->image)
                bd12_cmd_transition_image(c, r->image, sub, before, after);
        } else if (type == 1) {
            ComResource *after = rdptr(b + 16);
            bd12_cmd_global_barrier(c);
            if (after && after->image) {
                pthread_mutex_lock(&c->device->view_lock);
                after->image->initialized = 0;
                pthread_mutex_unlock(&c->device->view_lock);
            }
        } else if (type == 2) {
            ComResource *r = rdptr(b + 8);
            if (r && r->buffer) bd12_cmd_uav_barrier_buffer(c, r->buffer);
            else if (r && r->image) bd12_cmd_uav_barrier_image(c, r->image);
            else bd12_cmd_uav_barrier_all(c);
        }
    }
    bd12_cmd_barrier_batch_end(c);
}

static void MS cl_set_descriptor_heaps(ComList *l, uint32_t n, ComDescHeap *const *heaps)
{
    LIST_CUR(l);
    BD12DescriptorHeap *cbv = NULL, *sampler = NULL;
    for (uint32_t i = 0; i < n; ++i) {
        if (!heaps[i]) continue;
        if (heaps[i]->heap->type == 1) sampler = heaps[i]->heap;
        else cbv = heaps[i]->heap;
    }
    bd12_cmd_set_descriptor_heaps(c, cbv, sampler);
}

static void MS cl_set_compute_rs(ComList *l, ComRootSig *rs)
{
    LIST_CUR(l);
    if (call_log_on()) call_note(c, "compute rs %p", (void *)(rs ? rs->rs : NULL));
    bd12_cmd_set_compute_root_signature(c, rs ? rs->rs : NULL);
}
static void MS cl_set_graphics_rs(ComList *l, ComRootSig *rs)
{
    LIST_CUR(l);
    bd12_cmd_set_graphics_root_signature(c, rs ? rs->rs : NULL);
}
static void MS cl_set_compute_table(ComList *l, uint32_t i, uint64_t h)
{
    LIST_CUR(l);
    if (call_log_on()) call_note(c, "compute table %u = 0x%llx", i, (unsigned long long)h);
    bd12_cmd_set_compute_root_table(c, i, h);
}
static void MS cl_set_graphics_table(ComList *l, uint32_t i, uint64_t h)
{
    LIST_CUR(l);
    bd12_cmd_set_graphics_root_table(c, i, h);
}
static void MS cl_set_compute_const(ComList *l, uint32_t i, uint32_t v, uint32_t off)
{
    LIST_CUR(l);
    if (call_log_on()) call_note(c, "compute const %u [%u] = 0x%x", i, off, v);
    bd12_cmd_set_compute_root_constants(c, i, &v, 1, off);
}
static void MS cl_set_graphics_const(ComList *l, uint32_t i, uint32_t v, uint32_t off)
{
    LIST_CUR(l);
    bd12_cmd_set_graphics_root_constants(c, i, &v, 1, off);
}
static void MS cl_set_compute_consts(ComList *l, uint32_t i, uint32_t n,
                                     const uint32_t *d, uint32_t off)
{
    LIST_CUR(l);
    if (call_log_on()) call_note(c, "compute consts %u [%u..+%u] = 0x%x 0x%x", i, off, n, n > 0 ? d[0] : 0, n > 1 ? d[1] : 0);
    bd12_cmd_set_compute_root_constants(c, i, d, n, off);
}
static void MS cl_set_graphics_consts(ComList *l, uint32_t i, uint32_t n,
                                      const uint32_t *d, uint32_t off)
{
    LIST_CUR(l);
    bd12_cmd_set_graphics_root_constants(c, i, d, n, off);
}
static void MS cl_set_compute_va(ComList *l, uint32_t i, uint64_t va)
{
    LIST_CUR(l);
    bd12_cmd_set_compute_root_va(c, i, va);
}
static void MS cl_set_graphics_va(ComList *l, uint32_t i, uint64_t va)
{
    LIST_CUR(l);
    bd12_cmd_set_graphics_root_va(c, i, va);
}

static void MS cl_set_index_buffer(ComList *l, const uint8_t *view)
{
    LIST_CUR(l);
    if (!view) {
        bd12_cmd_set_index_buffer(c, 0, 0, VK_INDEX_TYPE_UINT16);
        return;
    }
    bd12_cmd_set_index_buffer(c, rd64(view), rd32(view + 8),
                              rd32(view + 12) == 42 ? VK_INDEX_TYPE_UINT32 :
                                                      VK_INDEX_TYPE_UINT16);
}

static void MS cl_set_vertex_buffers(ComList *l, uint32_t start, uint32_t n,
                                     const uint8_t *views)
{
    LIST_CUR(l);
    if (start >= BD12_MAX_VERTEX_BUFFERS) return;
    if (n > BD12_MAX_VERTEX_BUFFERS - start) n = BD12_MAX_VERTEX_BUFFERS - start;
    uint64_t va[BD12_MAX_VERTEX_BUFFERS] = { 0 };
    uint32_t sizes[BD12_MAX_VERTEX_BUFFERS] = { 0 };
    uint32_t strides[BD12_MAX_VERTEX_BUFFERS] = { 0 };
    for (uint32_t i = 0; views && i < n; ++i) {
        va[i] = rd64(views + i * 16);
        sizes[i] = rd32(views + i * 16 + 8);
        strides[i] = rd32(views + i * 16 + 12);
    }
    bd12_cmd_set_vertex_buffers(c, start, va, sizes, strides, n);
}

static void MS cl_set_render_targets(ComList *l, uint32_t n,
                                     const uint64_t *handles, int32_t single,
                                     const uint64_t *dsv_handle)
{
    LIST_CUR(l);
    if (n > BD12_MAX_RENDER_TARGETS) n = BD12_MAX_RENDER_TARGETS;
    const BD12Descriptor *rtvs[BD12_MAX_RENDER_TARGETS];
    for (uint32_t i = 0; i < n; ++i) {
        uint64_t h = single ? handles[0] + (uint64_t)i * sizeof(BD12Descriptor)
                            : handles[i];
        rtvs[i] = (const BD12Descriptor *)(uintptr_t)h;
    }
    const BD12Descriptor *dsv = dsv_handle ?
        (const BD12Descriptor *)(uintptr_t)*dsv_handle : NULL;
    if (dsv && dsv->kind != BD12_DESC_DSV) dsv = NULL;
    bd12_cmd_set_render_targets(c, rtvs, n, dsv);
}

static uint32_t convert_rects(const int32_t *rects, uint32_t n, VkRect2D *out)
{
    if (n > 32) n = 32;
    for (uint32_t i = 0; i < n; ++i) {
        int32_t x0 = rects[i * 4], y0 = rects[i * 4 + 1];
        int32_t x1 = rects[i * 4 + 2], y1 = rects[i * 4 + 3];
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        out[i] = (VkRect2D){ { x0, y0 },
                             { x1 > x0 ? (uint32_t)(x1 - x0) : 0,
                               y1 > y0 ? (uint32_t)(y1 - y0) : 0 } };
    }
    return n;
}

static void MS cl_clear_dsv(ComList *l, uint64_t handle, uint32_t flags,
                            float depth, uint8_t stencil, uint32_t n,
                            const int32_t *rects)
{
    LIST_CUR(l);
    VkRect2D r[32];
    if (bd12_trace_active() && getenv("BEER_BD12_COPY_LOG")) {
        const BD12Descriptor *dd = (const BD12Descriptor *)(uintptr_t)handle;
        fprintf(stderr, "[CLEARDSV] %ux%u layers=%u base=%u flags=%u depth=%g rects=%u (kept %u)\n",
                dd ? dd->width : 0, dd ? dd->height : 0, dd ? dd->layer_count : 0,
                dd ? dd->view_base_layer : 0, flags, depth, n, n > 16 ? 16 : n);
        for (uint32_t i = 0; rects && i < n && i < 40; ++i)
            fprintf(stderr, "[CLEARDSV]   rect %d,%d..%d,%d\n", rects[i*4], rects[i*4+1], rects[i*4+2], rects[i*4+3]);
    }
    uint32_t count = rects ? convert_rects(rects, n, r) : 0;
    /* D3D12_CLEAR_FLAGS (DEPTH=1, STENCIL=2) are not Vulkan aspect bits (DEPTH=2,
     * STENCIL=4); passing them through cleared nothing for depth-only clears (shadow
     * maps stayed stale forever) and left stencil untouched. */
    const VkImageAspectFlags clear_aspects =
        ((flags & 1) ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) |
        ((flags & 2) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
    bd12_cmd_clear_dsv(c, (const BD12Descriptor *)(uintptr_t)handle,
                       clear_aspects, depth, stencil, count, count ? r : NULL);
}

static void MS cl_clear_rtv(ComList *l, uint64_t handle, const float *color,
                            uint32_t n, const int32_t *rects)
{
    LIST_CUR(l);
    VkRect2D r[32];
    uint32_t count = rects ? convert_rects(rects, n, r) : 0;
    bd12_cmd_clear_rtv(c, (const BD12Descriptor *)(uintptr_t)handle, color,
                       count, count ? r : NULL);
}

static void clear_uav(BD12CmdList *c, uint64_t cpu_handle, ComResource *r,
                      const uint32_t values[4], int is_float)
{
    const BD12Descriptor *d = (const BD12Descriptor *)(uintptr_t)cpu_handle;
    if (getenv("BEER_BD12_CLEAR_LOG")) {
        static unsigned logged;
        if (logged++ < 4000)
            fprintf(stderr, "[CLEAR] uav %s kind=%u res=%s %s %ux%ux%u fmt=%d%s\n",
                    is_float ? "float" : "uint", d ? d->kind : 99,
                    r ? (r->buffer ? "buffer" : r->image ? "image" : "none") : "null",
                    d && d->kind == BD12_DESC_UAV_IMAGE ? "image" : "",
                    d ? d->width : 0, d ? d->height : 0, d ? d->layer_count : 0,
                    d ? (int)d->format : 0,
                    (d && d->kind != BD12_DESC_UAV_BUFFER && d->kind != BD12_DESC_UAV_IMAGE) ?
                        "  <-- SKIPPED (not a UAV descriptor)" : "");
    }
    if (!d) return;
    if (d->kind == BD12_DESC_UAV_BUFFER) {
        if (r && r->buffer)
            bd12_cmd_clear_uav_buffer(c, r->buffer, d->offset, d->range,
                                      values[0]);
    } else if (d->kind == BD12_DESC_UAV_IMAGE) {
        VkClearColorValue color;
        memcpy(&color, values, sizeof(color));
        (void)is_float;
        /* A sampler-feedback map (R64_UINT) is cleared to "nothing sampled"; D3D12 only defines a zero clear. */
        if (r && (r->dxgi_format == 189 || r->dxgi_format == 190)) memset(&color, 0, sizeof(color));
        bd12_cmd_clear_uav_image(c, d, &color);
    }
}

static void MS cl_clear_uav_uint(ComList *l, uint64_t gpu, uint64_t cpu,
                                 ComResource *r, const uint32_t *v, uint32_t n,
                                 const void *rects)
{
    (void)gpu; (void)n; (void)rects;
    LIST_CUR(l);
    clear_uav(c, cpu, r, v, 0);
}

static void MS cl_clear_uav_float(ComList *l, uint64_t gpu, uint64_t cpu,
                                  ComResource *r, const uint32_t *v, uint32_t n,
                                  const void *rects)
{
    (void)gpu; (void)n; (void)rects;
    LIST_CUR(l);
    clear_uav(c, cpu, r, v, 1);
}

/* BeginQuery/EndQuery/ResolveQueryData. D3D12 query types: 0 occlusion,
 * 1 binary occlusion, 2 timestamp, 3 pipeline statistics, 4+ stream output. */
static void MS cl_begin_query(ComList *l, ComObj *heap, uint32_t type,
                              uint32_t index)
{
    LIST_CUR(l);
    ComQueryHeap *h = (ComQueryHeap *)heap;
    { static unsigned seen; if (seen++ < 3) fprintf(stderr, "[BD12] BeginQuery type=%u heap-type=%u index=%u\n", type, h ? h->heap_type : 99u, index); }
    if (!h || !h->pool || index >= h->count || type > 1) return;
    bd12_cmd_begin_query(c, h->pool, index);
}

static void MS cl_end_query(ComList *l, ComObj *heap, uint32_t type,
                            uint32_t index)
{
    LIST_CUR(l);
    ComQueryHeap *h = (ComQueryHeap *)heap;
    if (!h || !h->pool || index >= h->count) return;
    if (type <= 1) bd12_cmd_end_query(c, h->pool, index);
    else if (type == 2) bd12_cmd_write_timestamp(c, h->pool, index);
    else return;
    h->written[index] = 1;
}

static void MS cl_resolve_query(ComList *l, ComObj *heap, uint32_t type,
                                uint32_t start, uint32_t count,
                                ComResource *dst, uint64_t offset)
{
    LIST_CUR(l);
    if (!dst || !dst->buffer || !count) return;
    ComQueryHeap *h = (ComQueryHeap *)heap;
    { static unsigned seen; if (seen++ < 3) fprintf(stderr, "[BD12] ResolveQueryData type=%u heap-type=%u start=%u count=%u\n", type, h ? h->heap_type : 99u, start, count); }
    if (!h || !h->pool || start >= h->count) {
        bd12_cmd_clear_uav_buffer(c, dst->buffer, offset, (uint64_t)count * 8, 0);
        return;
    }
    if (count > h->count - start) count = h->count - start;
    /* Copy contiguous runs of queries that were ended; zero the rest. */
    uint32_t i = 0;
    while (i < count) {
        const int ended = h->written[start + i];
        uint32_t run = 1;
        while (i + run < count && h->written[start + i + run] == ended) ++run;
        if (ended)
            bd12_cmd_copy_query_results(c, h->pool, start + i, run, dst->buffer,
                                        offset + (uint64_t)i * 8);
        else
            bd12_cmd_clear_uav_buffer(c, dst->buffer, offset + (uint64_t)i * 8,
                                      (uint64_t)run * 8, 0);
        i += run;
    }
}

static void MS cl_marker(ComList *l, uint32_t a, const void *b, uint32_t c)
{ (void)l; (void)a; (void)b; (void)c; }
static void MS cl_end_event(ComList *l) { (void)l; }


/* Diagnostic: BEER_BD12_INDIRECT_LOG=1 copies each ExecuteIndirect count
 * value to a readback ring and reports, per max_count, how many draws the
 * GPU-side culling actually kept. */
#define IND_RING 4096u
static struct { uint32_t max_count, type; } g_ind_meta[IND_RING];
static BD12Buffer *g_ind_rb;
static uint32_t g_ind_slot;
static uint64_t g_ind_last_ns;

static void indirect_log(ComList *l, BD12CmdList *c, uint32_t type,
                         uint32_t max_count, BD12Buffer *cb, uint64_t count_off,
                         BD12Buffer *ab, uint64_t arg_off)
{
    static int enabled = -1;
    if (enabled < 0) enabled = getenv("BEER_BD12_INDIRECT_LOG") != NULL;
    if (!enabled || !cb) return;
    if (!g_ind_rb)
        g_ind_rb = bd12_buffer_create_committed(c->device, BD12_HEAP_READBACK,
                                                IND_RING * 32);
    if (!g_ind_rb || !g_ind_rb->cpu) return;
    uint32_t slot = g_ind_slot++ % IND_RING;
    g_ind_meta[slot].max_count = max_count;
    g_ind_meta[slot].type = type;
    bd12_cmd_global_barrier(c);
    bd12_cmd_copy_buffer(c, g_ind_rb, slot * 32u, cb, count_off, 4);
    if (ab && arg_off + 20 <= ab->size)
        bd12_cmd_copy_buffer(c, g_ind_rb, slot * 32u + 4, ab, arg_off, 20);
    bd12_cmd_global_barrier(c);
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now = (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
    if (now - g_ind_last_ns < 2000000000ull || g_ind_slot < 3000) return;
    g_ind_last_ns = now;
    struct { uint32_t max, type, n, zero; uint64_t sum; uint32_t peak; } b[16] = {0};
    uint32_t nb = 0;
    for (uint32_t k = 1000; k < 3000; ++k) {
        uint32_t sl = (g_ind_slot - k) % IND_RING;
        uint32_t m = g_ind_meta[sl].max_count, t = g_ind_meta[sl].type;
        uint32_t v = ((volatile uint32_t *)g_ind_rb->cpu)[sl * 8], i;
        if (k < 1040 && v) { volatile uint32_t *q = (volatile uint32_t *)g_ind_rb->cpu + sl * 8; fprintf(stderr, "[BD12 INDARG] max=%u count=%u idx=%u inst=%u first=%u base=%d fi=%u (off argbuf)\n", m, v, q[1], q[2], q[3], (int)q[4], q[5]); }
        for (i = 0; i < nb; ++i) if (b[i].max == m && b[i].type == t) break;
        if (i == nb) { if (nb == 16) continue; b[nb++] = (typeof(b[0])){ .max = m, .type = t }; }
        b[i].n++; b[i].sum += v; if (!v) b[i].zero++;
        if (v > b[i].peak) b[i].peak = v;
    }
    for (uint32_t i = 0; i < nb; ++i)
        fprintf(stderr, "[BD12 IND] type=%u max=%u calls=%u avg=%llu peak=%u zero=%u\n",
                b[i].type, b[i].max, b[i].n,
                (unsigned long long)(b[i].sum / b[i].n), b[i].peak, b[i].zero);
    (void)l;
}

static void MS cl_execute_indirect(ComList *l, ComSignature *sig,
                                   uint32_t max_count, ComResource *args,
                                   uint64_t arg_off, ComResource *count_res,
                                   uint64_t count_off)
{
    LIST_CUR(l);
    if (!sig || !args || !args->buffer) return;
    if (sig->arg_count != 1) {
        BD12_DROP(BD12_DROP_INDIRECT_MULTI_ARG);
        static uint32_t seen_masks[8];
        uint32_t key = sig->arg_count;
        for (uint32_t i = 0; i < sig->arg_count && i < 8; ++i)
            key = key * 31u + sig->arg_types[i];
        int known = 0;
        for (uint32_t i = 0; i < 8; ++i) known |= seen_masks[i] == key;
        if (!known) {
            for (uint32_t i = 0; i < 8; ++i)
                if (!seen_masks[i]) { seen_masks[i] = key; break; }
            fprintf(stderr, "[BD12] ExecuteIndirect signature with %u arguments (types:",
                    sig->arg_count);
            for (uint32_t i = 0; i < sig->arg_count && i < 8; ++i)
                fprintf(stderr, " %u", sig->arg_types[i]);
            fprintf(stderr, ", stride %u) is not supported; its work is dropped\n",
                    sig->stride);
        }
        return;
    }
    uint32_t stride = sig->stride;
    BD12Buffer *cb = count_res ? count_res->buffer : NULL;
    { static int force = -1; if (force < 0) force = getenv("BEER_BD12_INDIRECT_FORCE") != NULL; if (force) cb = NULL; }
    if (bd12_trace_active())
        fprintf(stderr, "[TRACE] execute_indirect list=%p pso=%016llx type=%u max=%u args=%llx+%llx count=%llx+%llx\n",
                (void *)c, (unsigned long long)(sig->arg_types[0] <= 2 && c->gfx.pipeline ? c->gfx.pipeline->shader_hash : 0),
                sig->arg_types[0], max_count,
                (unsigned long long)args->buffer->va, (unsigned long long)arg_off,
                (unsigned long long)(cb ? cb->va : 0), (unsigned long long)count_off);
    if (sig->arg_types[0] <= 1 && c->gfx.pipeline)
        bd12_capture_indirect(c, c->gfx.pipeline->shader_hash, sig->arg_types[0],
                              args->buffer, arg_off, cb, count_off, max_count, stride);
    if (sig->arg_types[0] <= 1)
        indirect_log(l, c, sig->arg_types[0], max_count, cb, count_off, args->buffer, arg_off);
    switch (sig->arg_types[0]) {
    case 0:
        if (cb) bd12_cmd_draw_indirect_count(c, args->buffer, arg_off, cb,
                                             count_off, max_count, stride);
        else bd12_cmd_draw_indirect(c, args->buffer, arg_off, max_count, stride);
        break;
    case 1:
        if (cb) bd12_cmd_draw_indexed_indirect_count(c, args->buffer, arg_off, cb,
                                                     count_off, max_count, stride);
        else bd12_cmd_draw_indexed_indirect(c, args->buffer, arg_off, max_count,
                                            stride);
        break;
    case 10:                                       /* D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH */
        if (bd12_trace_active() && !c->gfx.pipeline)
            fprintf(stderr, "[TRACE] mesh-indirect with no pipeline bound\n");
        if (c->gfx.pipeline)
            bd12_capture_indirect(c, c->gfx.pipeline->shader_hash, 10, args->buffer, arg_off, cb, count_off,
                                  max_count, stride);
        bd12_cmd_dispatch_mesh_indirect(c, args->buffer, arg_off, cb, count_off, max_count, stride);
        break;
    case 2:
        if (cb) BD12_DROP(BD12_DROP_INDIRECT_DISPATCH_COUNT);
        for (uint32_t i = 0; i < max_count; ++i)
            bd12_cmd_dispatch_indirect_bound(c, args->buffer,
                                             arg_off + (uint64_t)i * stride);
        break;
    default:
        break;
    }
}

/* ID3D12GraphicsCommandList4 raytracing methods. */
typedef struct { uint64_t dest; uint32_t type; } RtPostbuild;
static void MS cl_rt_build(ComList *l, const void *desc, uint32_t post_count,
                           const RtPostbuild *post)
{
    LIST_CUR(l);
    bd12_cmd_build_as(c, desc);
    const uint64_t dest_va = *(const uint64_t *)desc;
    for (uint32_t i = 0; i < post_count; ++i)
        bd12_cmd_emit_as_info(c, post[i].dest, post[i].type, 1, &dest_va);
}
static void MS cl_rt_emit(ComList *l, const RtPostbuild *post, uint32_t count,
                          const uint64_t *sources)
{
    LIST_CUR(l);
    bd12_cmd_emit_as_info(c, post->dest, post->type, count, sources);
}
unsigned g_dxr_set_calls;
static void MS cl_rt_set_pso1(ComList *l, ComStateObject *state)
{
    LIST_CUR(l);
    __atomic_add_fetch(&g_dxr_set_calls, 1, __ATOMIC_RELAXED);
    c->rt_pipeline = state ? state->rt : NULL;
    if (getenv("BEER_DXR_PROBE")) {
        static unsigned calls;
        static void *seen[64];
        static unsigned seen_count;
        ++calls;
        unsigned known = 0;
        for (unsigned i = 0; i < seen_count; ++i) known |= seen[i] == (void *)state;
        if (!known && seen_count < 64) {
            seen[seen_count++] = (void *)state;
            {
                extern __thread uint64_t g_dispatch_incoming_return;
                fprintf(stderr, "[DXR] SetPipelineState1 #%u: new state object %p (rt pipeline %p), %u distinct so far (guest caller 0x%llx)\n",
                        calls, (void *)state, state ? (void *)state->rt : NULL, seen_count,
                        (unsigned long long)g_dispatch_incoming_return);
                extern __thread uint64_t g_dispatch_incoming_rsp;
                const uint64_t *sp = (const uint64_t *)(uintptr_t)g_dispatch_incoming_rsp;
                fprintf(stderr, "[DXR]   guest stack:");
                for (int k = 0; sp && k < 160; ++k)
                    if (sp[k] >= 0x140001000ull && sp[k] < 0x14d000000ull) fprintf(stderr, " %llx", (unsigned long long)sp[k]);
                fprintf(stderr, "\n");
            }
        }
    }
}
static void MS cl_rt_dispatch_rays(ComList *l, const uint8_t *desc)
{
    LIST_CUR(l);
    static unsigned total, skipped;
    const int done = bd12_cmd_dispatch_rays(c, c->rt_pipeline, desc);
    ++total;
    if (!done) ++skipped;
    if (total <= 3 || !(total % 1000) || (!done && skipped <= 5))
        fprintf(stderr, "[DXR] DispatchRays #%u (SetPipelineState1 calls: %u) %ux%ux%u %s (skipped so far: %u)\n", total, g_dxr_set_calls,
                desc ? *(const uint32_t *)(desc + 88) : 0, desc ? *(const uint32_t *)(desc + 92) : 0,
                desc ? *(const uint32_t *)(desc + 96) : 0, done ? "recorded" : "SKIPPED", skipped);
}
static void MS cl_rt_copy(ComList *l, uint64_t dst, uint64_t src, uint32_t mode)
{
    LIST_CUR(l);
    bd12_cmd_copy_as(c, dst, src, mode);
}

void *com_list_vtable_[80] = {
    [0] = list_query, [1] = com_add_ref, [2] = com_release,
    [3] = com_obj_get_private, [4] = com_obj_set_private,
    [5] = com_obj_set_private_iface, [6] = com_obj_set_name,
    [7] = com_obj_get_device, [8] = cl_get_type, [9] = cl_close,
    [10] = cl_reset, [11] = cl_clear_state, [12] = cl_draw,
    [13] = cl_draw_indexed, [14] = cl_dispatch, [15] = cl_copy_buffer_region,
    [16] = cl_copy_texture_region, [17] = cl_copy_resource, [18] = cl_copy_tiles,
    [19] = cl_resolve, [20] = cl_set_topology, [21] = cl_set_viewports,
    [22] = cl_set_scissors, [23] = cl_set_blend_factor,
    [24] = cl_set_stencil_ref, [25] = cl_set_pso, [26] = cl_barrier,
    [27] = cl_noop, [28] = cl_set_descriptor_heaps,
    [29] = cl_set_compute_rs, [30] = cl_set_graphics_rs,
    [31] = cl_set_compute_table, [32] = cl_set_graphics_table,
    [33] = cl_set_compute_const, [34] = cl_set_graphics_const,
    [35] = cl_set_compute_consts, [36] = cl_set_graphics_consts,
    [37] = cl_set_compute_va, [38] = cl_set_graphics_va,
    [39] = cl_set_compute_va, [40] = cl_set_graphics_va,
    [41] = cl_set_compute_va, [42] = cl_set_graphics_va,
    [43] = cl_set_index_buffer, [44] = cl_set_vertex_buffers,
    [45] = cl_noop, [46] = cl_set_render_targets, [47] = cl_clear_dsv,
    [48] = cl_clear_rtv, [49] = cl_clear_uav_uint, [50] = cl_clear_uav_float,
    [51] = cl_noop, [52] = cl_begin_query, [53] = cl_end_query, [54] = cl_resolve_query,
    [55] = cl_noop, [56] = cl_marker, [57] = cl_marker, [58] = cl_end_event,
    [59] = cl_execute_indirect,
    /* List1 */ [60] = cl_noop, [61] = cl_noop, [62] = cl_noop, [63] = cl_noop,
    [64] = cl_resolve_region, [65] = cl_noop,
    /* List2, List3 */ [66] = cl_noop, [67] = cl_noop,
    /* List4 */ [68] = cl_noop, [69] = cl_noop, [70] = cl_noop, [71] = cl_noop,
    [72] = cl_rt_build, [73] = cl_rt_emit, [74] = cl_rt_copy,
    [75] = cl_rt_set_pso1, [76] = cl_rt_dispatch_rays,
    /* List5: RSSetShadingRate, RSSetShadingRateImage (VRS, BEER_BD12_VRS=1); List6: DispatchMesh (BEER_FAKE_CAPS
     * only: no-op) */
    [77] = cl_set_shading_rate, [78] = cl_set_shading_rate_image, [79] = cl_dispatch_mesh,
};
