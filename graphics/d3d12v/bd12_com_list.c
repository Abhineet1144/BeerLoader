#define _GNU_SOURCE
/* BD12 COM shim, part 2: command allocators, command lists and pipeline
 * states. Every command list method records straight into a VkCommandBuffer
 * through the BD12 core. */

#include "bd12_com.h"
#include "bd12_formats.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint8_t iid_allocator[16] = { 0xe4, 0xde, 0x02, 0x61, 0x59, 0xaf,
    0x09, 0x4b, 0xb9, 0x99, 0xb4, 0x4d, 0x73, 0xf0, 0x9b, 0x24 };
static const uint8_t iid_list[16] = { 0x0f, 0x0d, 0x16, 0x5b, 0x1b, 0xac, 0x85,
    0x41, 0x8b, 0xa8, 0xb3, 0xae, 0x42, 0xa5, 0xa4, 0x55 };
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
        p->cmp = bd12_compute_pipeline_create_dxbc(bd, p->desc.cs,
                                                   p->desc.cs_size, p->hash);
    else
        p->gfx = bd12_pso_create_graphics(bd, &p->desc);
    bd12_pso_release(&p->desc);
    pthread_mutex_lock(&p->lock);
    p->state = (p->gfx || p->cmp) ? 2 : 3;
    pthread_cond_broadcast(&p->cond);
    pthread_mutex_unlock(&p->lock);
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
    if (blob) *blob = NULL;
    return HR_E_NOTIMPL;
}
static void *pso_vtable[9] = {
    [0] = pso_query, [1] = com_add_ref, [2] = com_release,
    [3] = com_obj_get_private, [4] = com_obj_set_private,
    [5] = com_obj_set_private_iface, [6] = com_obj_set_name,
    [7] = com_obj_get_device, [8] = pso_cached_blob,
};

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
    if (!p->compute && (!tmp.vs_size)) {
        free(p);
        return HR_E_INVALIDARG;
    }
    if (!bd12_pso_clone(&tmp, &p->desc)) {
        free(p);
        return HR_E_OUTOFMEMORY;
    }
    p->hash = p->compute ? fnv(p->desc.cs, p->desc.cs_size) : 0;
    pthread_mutex_init(&p->lock, NULL);
    pthread_cond_init(&p->cond, NULL);
    com_obj_init(&p->obj, COM_PSO, pso_vtable, dev, pso_destroy);
    /* D3D12 permits deferred PSO compilation. RE8 creates a large library at
     * startup but binds only a small working set before the title. Compiling
     * the entire library eagerly made the NVIDIA driver retain multiple GiB.
     * Keep asynchronous eager compilation available only for profiling. */
    if (getenv("BEER_BD12_ASYNC_PSO") && !pso_enqueue(p)) pso_compile(p);
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

static void list_set_pso(ComList *l, ComPso *p)
{
    BD12CmdList *c = l->cur;
    if (!p) return;
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
    l->open = 1;
    if (pso) list_set_pso(l, pso);
    return 1;
}

HR com_create_list(ComDevice *dev, uint32_t type, ComObj *allocator,
                   ComObj *pso, const uint8_t *riid, void **out)
{
    if (!allocator || !riid || !out) return HR_E_INVALIDARG;
    *out = NULL;
    if (!com_iid_equal(riid, iid_list) && !com_iid_equal(riid, iid_command_list) &&
        !com_iid_equal(riid, com_iid_iunknown))
        return HR_E_NOINTERFACE;
    ComList *l = calloc(1, sizeof(*l));
    if (!l) return HR_E_OUTOFMEMORY;
    l->type = type;
    com_obj_init(&l->obj, COM_LIST, NULL, dev, list_destroy);
    extern void *com_list_vtable_[60];
    l->obj.vtable = com_list_vtable_;
    if (!list_start(l, (ComAllocator *)allocator, (ComPso *)pso)) {
        com_release(&l->obj);
        return HR_E_FAIL;
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

#define LIST_CUR(l) BD12CmdList *c = (l)->open ? (l)->cur : NULL; if (!c) return
#define LIST_CUR_V(l, v) BD12CmdList *c = (l)->open ? (l)->cur : NULL; if (!c) return (v)

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
    bd12_cmd_draw_indexed(c, ic, inst, si, bv, sinst);
}

static void MS cl_dispatch(ComList *l, uint32_t x, uint32_t y, uint32_t z)
{
    LIST_CUR(l);
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

static void MS cl_set_topology(ComList *l, uint32_t topology)
{
    LIST_CUR(l);
    uint32_t patch = 0;
    bd12_cmd_set_topology(c, bd12_topology_to_vk(topology, &patch));
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

static void MS cl_set_pso(ComList *l, ComPso *p)
{
    LIST_CUR(l);
    list_set_pso(l, p);
}

static void MS cl_barrier(ComList *l, uint32_t n, const uint8_t *barriers)
{
    LIST_CUR(l);
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
            else bd12_cmd_global_barrier(c);
        }
    }
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
    uint32_t count = rects ? convert_rects(rects, n, r) : 0;
    bd12_cmd_clear_dsv(c, (const BD12Descriptor *)(uintptr_t)handle,
                       flags & 3, depth, stencil, count, count ? r : NULL);
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
    if (!d) return;
    if (d->kind == BD12_DESC_UAV_BUFFER) {
        if (r && r->buffer)
            bd12_cmd_clear_uav_buffer(c, r->buffer, d->offset, d->range,
                                      values[0]);
    } else if (d->kind == BD12_DESC_UAV_IMAGE) {
        VkClearColorValue color;
        memcpy(&color, values, sizeof(color));
        (void)is_float;
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

static void MS cl_resolve_query(ComList *l, ComObj *heap, uint32_t type,
                                uint32_t start, uint32_t count,
                                ComResource *dst, uint64_t offset)
{
    (void)heap; (void)type; (void)start;
    LIST_CUR(l);
    if (dst && dst->buffer && count)
        bd12_cmd_clear_uav_buffer(c, dst->buffer, offset, (uint64_t)count * 8, 0);
}

static void MS cl_marker(ComList *l, uint32_t a, const void *b, uint32_t c)
{ (void)l; (void)a; (void)b; (void)c; }
static void MS cl_end_event(ComList *l) { (void)l; }

static void MS cl_execute_indirect(ComList *l, ComSignature *sig,
                                   uint32_t max_count, ComResource *args,
                                   uint64_t arg_off, ComResource *count_res,
                                   uint64_t count_off)
{
    LIST_CUR(l);
    if (!sig || !args || !args->buffer) return;
    if (sig->arg_count != 1) {
        static int warned;
        if (warned++ < 4)
            fprintf(stderr, "[BD12] ExecuteIndirect with %u arguments (first=%u) "
                    "is not supported\n", sig->arg_count, sig->arg_types[0]);
        return;
    }
    uint32_t stride = sig->stride;
    BD12Buffer *cb = count_res ? count_res->buffer : NULL;
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
    case 2:
        for (uint32_t i = 0; i < max_count; ++i)
            bd12_cmd_dispatch_indirect_bound(c, args->buffer,
                                             arg_off + (uint64_t)i * stride);
        break;
    default:
        break;
    }
}

void *com_list_vtable_[60] = {
    [0] = list_query, [1] = com_add_ref, [2] = com_release,
    [3] = com_obj_get_private, [4] = com_obj_set_private,
    [5] = com_obj_set_private_iface, [6] = com_obj_set_name,
    [7] = com_obj_get_device, [8] = cl_get_type, [9] = cl_close,
    [10] = cl_reset, [11] = cl_clear_state, [12] = cl_draw,
    [13] = cl_draw_indexed, [14] = cl_dispatch, [15] = cl_copy_buffer_region,
    [16] = cl_copy_texture_region, [17] = cl_copy_resource, [18] = cl_noop,
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
    [51] = cl_noop, [52] = cl_noop, [53] = cl_noop, [54] = cl_resolve_query,
    [55] = cl_noop, [56] = cl_marker, [57] = cl_marker, [58] = cl_end_event,
    [59] = cl_execute_indirect,
};
