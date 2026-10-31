#ifndef BD11_H
#define BD11_H

/* BD11: D3D11 on Vulkan, built on the BD12 core (see beer-loader-progress.md, "BD11").
 *
 *   ID3D11Device                 -> B11Device   (owns a BD12Device, queue, retire timeline)
 *   ID3D11DeviceContext          -> B11Context  (immediate: records into a BD12CmdList;
 *                                                deferred: records a compact op stream)
 *   ID3D11CommandList            -> B11CmdList  (op stream replayed by ExecuteCommandList)
 *   ID3D11Buffer / Texture*      -> B11Buffer / B11Texture (BD12Buffer / BD12Image)
 *   views, shaders, states       -> small objects holding descriptors / DXBC / D3D state
 *
 * D3D11 has no resource barriers and no descriptor heaps: BD11 tracks one resource state
 * per texture (transitions are emitted when a use changes) and builds BD12 descriptor tables
 * per draw from the bound views.  Everything the guest sees is ms_abi COM. */

#include "../d3d12v/bd12.h"
#include "../d3d12v/bd12_formats.h"
#include "../d3d12v/bd12_pso.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>

#define B11_MS __attribute__((ms_abi))
typedef int32_t B11HR;
#define B11_S_OK ((B11HR)0)
#define B11_S_FALSE ((B11HR)1)
#define B11_E_NOTIMPL ((B11HR)0x80004001)
#define B11_E_NOINTERFACE ((B11HR)0x80004002)
#define B11_E_FAIL ((B11HR)0x80004005)
#define B11_E_INVALIDARG ((B11HR)0x80070057)
#define B11_E_OUTOFMEMORY ((B11HR)0x8007000e)
#define B11_DXGI_ERROR_WAS_STILL_DRAWING ((B11HR)0x887a000a)
#define B11_DXGI_ERROR_NOT_FOUND ((B11HR)0x887a0002)

/* D3D11_USAGE */
enum { B11_USAGE_DEFAULT = 0, B11_USAGE_IMMUTABLE = 1, B11_USAGE_DYNAMIC = 2, B11_USAGE_STAGING = 3 };
/* D3D11_BIND_FLAG */
enum {
    B11_BIND_VERTEX = 0x1, B11_BIND_INDEX = 0x2, B11_BIND_CONSTANT = 0x4, B11_BIND_SRV = 0x8,
    B11_BIND_STREAM_OUTPUT = 0x10, B11_BIND_RTV = 0x20, B11_BIND_DSV = 0x40, B11_BIND_UAV = 0x80
};
enum { B11_CPU_WRITE = 0x10000, B11_CPU_READ = 0x20000 };
enum { B11_MISC_GENERATE_MIPS = 0x1, B11_MISC_CUBE = 0x4, B11_MISC_DRAW_INDIRECT = 0x10,
       B11_MISC_RAW = 0x20, B11_MISC_STRUCTURED = 0x40 };
/* D3D11_MAP */
enum { B11_MAP_READ = 1, B11_MAP_WRITE = 2, B11_MAP_READ_WRITE = 3, B11_MAP_WRITE_DISCARD = 4,
       B11_MAP_WRITE_NO_OVERWRITE = 5 };

enum {
    B11_STAGE_VS, B11_STAGE_HS, B11_STAGE_DS, B11_STAGE_GS, B11_STAGE_PS, B11_STAGE_CS,
    B11_STAGE_COUNT
};
#define B11_MAX_CBV 14
#define B11_MAX_SRV 128
#define B11_MAX_SAMPLER 16
#define B11_MAX_UAV 8
#define B11_MAX_VB 16
#define B11_MAX_RT 8
#define B11_MAX_VIEWPORTS 16

enum {
    B11_DEVICE, B11_CONTEXT, B11_BUFFER, B11_TEXTURE, B11_SRV, B11_UAV, B11_RTV, B11_DSV,
    B11_SHADER, B11_LAYOUT, B11_BLEND, B11_DEPTH, B11_RASTER, B11_SAMPLER, B11_QUERY,
    B11_CMDLIST, B11_LINKAGE, B11_SWAPCHAIN
};

typedef struct B11Device B11Device;
typedef struct B11Context B11Context;

typedef struct B11Obj {
    void **vtable;
    _Atomic uint32_t refs;
    uint32_t kind;
    B11Device *dev;
    void (*destroy)(struct B11Obj *);
} B11Obj;

/* Deferred destruction: freed once the retire timeline passes `serial`. */
typedef struct B11Grave {
    struct B11Grave *next;
    uint64_t serial;
    void (*fn)(B11Device *, void *);
    void *ptr;
} B11Grave;

typedef struct {
    uint32_t byte_width, usage, bind, cpu, misc, stride;
} B11BufferDesc;

typedef struct {
    uint32_t dimension;          /* 1, 2, 3 */
    uint32_t width, height, depth, mips, array_size, format, samples, quality;
    uint32_t usage, bind, cpu, misc;
} B11TextureDesc;

/* Command lists in flight. Presents no longer wait for the GPU and games Flush() several times a frame,
 * so the ring must outlast a few frames of submissions or recording would stall on the oldest one. */
#define B11_ALLOCATORS 16

typedef struct B11Buffer {
    B11Obj obj;
    B11BufferDesc desc;
    /* GPU backing(s). DEFAULT/IMMUTABLE: one device-local buffer. DYNAMIC: a ring of host-visible
     * backings renamed on Map(WRITE_DISCARD). STAGING: one host-visible buffer. */
    BD12Buffer **backing;
    uint64_t *backing_serial;     /* last submission serial that used each backing */
    uint32_t backing_count, backing_capacity, current;
    int host_visible;
    int renamed;                  /* backing[0] is an upload slice replaced on every UpdateSubresource */
    int gpu_written;              /* a GPU copy wrote it: CPU-side renames would lose that data */
    uint8_t *shadow;              /* CPU copy for STAGING reads/writes via Map */
    /* Renamed buffers: cached CPU copy of the current version, so a partial UpdateSubresource builds the next
     * version without reading the old slice (upload memory is write-combined: reads are uncached and slow). */
    uint8_t *versions_copy;
    int versions_copy_valid;
} B11Buffer;

typedef struct B11Texture {
    B11Obj obj;
    B11TextureDesc desc;
    BD12Image *image;             /* NULL for STAGING (data lives in `staging`) */
    BD12Buffer *staging;          /* STAGING: host-visible buffer holding all subresources */
    uint64_t *sub_offset;         /* STAGING: per-subresource byte offset */
    uint32_t *sub_pitch, *sub_slice;
    uint32_t state;               /* current BD12_STATE_* of the whole image */
    uint32_t subresources;
    uint64_t last_use_serial;
    int is_backbuffer;
} B11Texture;

typedef struct B11View {
    B11Obj obj;
    B11Obj *resource;             /* retained */
    uint32_t dimension, format;
    uint32_t desc_bytes[8];       /* raw D3D11 *_VIEW_DESC copy */
    BD12DescriptorHeap *heap;     /* one-entry heap holding the descriptor */
    BD12Descriptor *desc;         /* &heap->descriptors[0] */
    uint32_t first, count;        /* buffer element range / mip range */
    uint32_t base_mip, mip_count, base_layer, layer_count;
    uint32_t flags;               /* DSV read-only flags / UAV flags */
    uint32_t uav_counter;
} B11View;

typedef struct B11Shader {
    B11Obj obj;
    uint32_t stage;
    uint8_t *bytecode;
    size_t size;
    uint64_t hash;
} B11Shader;

typedef struct B11Layout {
    B11Obj obj;
    uint32_t count;
    uint8_t *elements;            /* count * 32 bytes, D3D11_INPUT_ELEMENT_DESC with owned names */
    char (*names)[32];
    uint64_t hash;
} B11Layout;

typedef struct B11State {
    B11Obj obj;
    uint8_t desc[264];
    uint64_t hash;
    VkSamplerCreateInfo sampler;  /* B11_SAMPLER only */
} B11State;

typedef struct B11Query {
    B11Obj obj;
    uint32_t type;                /* D3D11_QUERY */
    uint32_t misc;
    uint32_t pool_index;          /* index in the device query pool, or ~0 */
    uint64_t submit_serial;       /* retire serial after which the result is readable */
    uint64_t value;
    int issued, ended;
} B11Query;

/* ---- recorded operations (deferred contexts) ------------------------------------- */
typedef enum {
    B11_OP_SET_IA_LAYOUT, B11_OP_SET_IA_VB, B11_OP_SET_IA_IB, B11_OP_SET_TOPOLOGY,
    B11_OP_SET_SHADER, B11_OP_SET_CB, B11_OP_SET_SRV, B11_OP_SET_SAMPLER, B11_OP_SET_UAV_CS,
    B11_OP_SET_RT, B11_OP_SET_BLEND, B11_OP_SET_DEPTH, B11_OP_SET_RASTER,
    B11_OP_SET_VIEWPORTS, B11_OP_SET_SCISSORS, B11_OP_DRAW, B11_OP_DRAW_INDEXED,
    B11_OP_DRAW_INSTANCED, B11_OP_DRAW_INDEXED_INSTANCED, B11_OP_DISPATCH,
    B11_OP_CLEAR_RTV, B11_OP_CLEAR_DSV, B11_OP_CLEAR_UAV_UINT, B11_OP_CLEAR_UAV_FLOAT,
    B11_OP_COPY_RESOURCE, B11_OP_COPY_REGION, B11_OP_UPDATE, B11_OP_BUFFER_WRITE,
    B11_OP_GENERATE_MIPS, B11_OP_RESOLVE, B11_OP_BEGIN_QUERY, B11_OP_END_QUERY,
    B11_OP_SET_PREDICATION, B11_OP_CLEAR_STATE,
    B11_OP_DRAW_INDEXED_INDIRECT, B11_OP_DRAW_INSTANCED_INDIRECT, B11_OP_DISPATCH_INDIRECT,
    B11_OP_BUFFER_RENAME
} B11OpType;

typedef struct {
    B11OpType type;
    uint32_t a, b, c, d, e, f;
    B11Obj *obj[3];               /* retained references, released with the list */
    void *data;                   /* owned payload (viewports, update data, buffer writes...) */
    size_t data_size;
} B11Op;

typedef struct B11CmdList {
    B11Obj obj;
    B11Op *ops;
    size_t count;
    uint32_t restore_state;
} B11CmdList;

/* ---- pipeline state shadow ---------------------------------------------------------- */
typedef struct {
    B11Obj *shader[B11_STAGE_COUNT];
    B11Obj *cb[B11_STAGE_COUNT][B11_MAX_CBV];
    uint32_t cb_first[B11_STAGE_COUNT][B11_MAX_CBV];
    uint32_t cb_count[B11_STAGE_COUNT][B11_MAX_CBV];
    B11Obj *srv[B11_STAGE_COUNT][B11_MAX_SRV];
    /* The resource of each bound SRV and one past the highest slot ever bound since the last clear, so the
     * per-draw and per-render-target scans touch only used slots and never chase view pointers. */
    B11Obj *srv_res[B11_STAGE_COUNT][B11_MAX_SRV];
    uint32_t srv_end[B11_STAGE_COUNT];
    B11Obj *sampler[B11_STAGE_COUNT][B11_MAX_SAMPLER];
    B11Obj *uav[B11_STAGE_COUNT][B11_MAX_UAV];       /* PS slots follow the RTV count; CS slots 0..7 */
    uint32_t uav_initial[B11_STAGE_COUNT][B11_MAX_UAV];
    B11Obj *vb[B11_MAX_VB];
    uint32_t vb_stride[B11_MAX_VB], vb_offset[B11_MAX_VB];
    B11Obj *ib;
    uint32_t ib_format, ib_offset;
    B11Obj *layout;
    uint32_t topology;
    B11Obj *rtv[B11_MAX_RT];
    uint32_t rtv_count;
    B11Obj *dsv;
    B11Obj *blend, *depth, *raster;
    float blend_factor[4];
    uint32_t sample_mask, stencil_ref;
    uint32_t viewport_count, scissor_count;
    float viewports[B11_MAX_VIEWPORTS][6];
    int32_t scissors[B11_MAX_VIEWPORTS][4];
    B11Obj *predicate;
    uint32_t predicate_value;
} B11PipelineState;

struct B11Context {
    B11Obj obj;
    uint32_t type;                /* 0 immediate, 1 deferred (D3D11_DEVICE_CONTEXT_TYPE) */
    uint32_t flags;
    B11PipelineState st;
    /* deferred recording */
    B11Op *ops;
    size_t op_count, op_capacity;
    /* mapped resources of this context */
    struct {
        B11Obj *resource;
        uint32_t subresource, map_type;
        void *data;
        size_t size;
        int lazy_slot;            /* lazy scratch registry slot + 1, 0 when data is plain heap memory */
        BD12Buffer *slice;        /* deferred DISCARD: the new version, renamed in when the list executes */
        int direct;               /* deferred NO_OVERWRITE into this context's discarded slice: nothing to emit */
    } mapped[64];
    /* deferred: the slice each buffer was last discarded into while recording the current list */
    struct { B11Obj *buffer; BD12Buffer *slice; } discards[64];
    uint32_t discard_count;
    /* immediate execution state */
    uint32_t dirty;
    uint64_t generation;          /* dev->cmd_generation the dirty bits were computed for */
    int rendering_valid;
    uint64_t pending_serial;
    uint64_t draw_count;
    uint64_t bound_vb_va[B11_MAX_VB];   /* what BD12 currently has bound (renamed dynamic buffers change it) */
    uint64_t bound_ib_va;
};

#define B11_DIRTY_PIPELINE 0x1u
#define B11_DIRTY_TARGETS 0x2u
#define B11_DIRTY_VIEWPORT 0x4u
#define B11_DIRTY_SCISSOR 0x8u
#define B11_DIRTY_VB 0x10u
#define B11_DIRTY_IB 0x20u
#define B11_DIRTY_TOPOLOGY 0x40u
#define B11_DIRTY_BLEND_CONST 0x80u
#define B11_DIRTY_STENCIL_REF 0x100u
#define B11_DIRTY_ALL 0x1ffu

typedef struct B11PsoEntry {
    uint64_t key;
    BD12GraphicsPipeline *pipeline;
    BD12ComputePipeline *compute;      /* set instead of pipeline for compute shaders */
    struct B11PsoEntry *next;
} B11PsoEntry;

typedef struct B11RootEntry {
    uint32_t sig[B11_STAGE_COUNT][4];   /* per stage: cbv, srv, uav, sampler counts */
    BD12RootSignature *root;
    uint32_t param_index[B11_STAGE_COUNT][4];
    struct B11RootEntry *next;
} B11RootEntry;

struct B11Device {
    B11Obj obj;
    BD12Device *bd;
    BD12Queue *queue;
    BD12Fence *retire;
    uint64_t serial;              /* last submitted retire value */
    pthread_mutex_t lock;
    pthread_mutex_t grave_lock;
    B11Grave *grave_head, *grave_tail;
    BD12CmdAllocator *allocators[B11_ALLOCATORS];
    uint64_t allocator_serial[B11_ALLOCATORS];
    uint32_t allocator_current;
    BD12CmdList *cmd;             /* the immediate context's open list */
    BD12CmdList *lists[B11_ALLOCATORS];  /* one list per allocator */
    uint64_t cmd_generation;      /* bumped whenever a fresh command list begins */
    int recording;
    B11Context *immediate;
    BD12DescriptorHeap *ring_heap[3];     /* CBV/SRV/UAV descriptor rings */
    BD12DescriptorHeap *ring_sampler[3];
    uint32_t ring_used, ring_sampler_used, ring_index;
    uint64_t ring_serial[3];
    B11PsoEntry *pso[4096];
    B11RootEntry *roots;
    VkQueryPool occlusion_pool;
    uint32_t occlusion_next;
    uint32_t feature_level;
    uint64_t frame;
    BD12Buffer *null_buffer;       /* bound for NULL constant buffers */
    uint32_t flags;
    struct B11SlicePool *slices;   /* recycled host-visible upload slices */
};

/* ---- API -------------------------------------------------------------------------------- */
void b11_init_vtables(void);
void *b11_obj_new(B11Device *dev, uint32_t kind, size_t size, void **vtable,
                  void (*destroy)(B11Obj *));
uint32_t B11_MS b11_add_ref(B11Obj *obj);
uint32_t B11_MS b11_release(B11Obj *obj);
B11HR b11_query_interface(B11Obj *obj, const uint8_t *riid, void **out);
void b11_retain(B11Obj *obj);
void b11_defer(B11Device *dev, void (*fn)(B11Device *, void *), void *ptr);
void b11_free_buffer_fn(B11Device *dev, void *p);
void b11_reap(B11Device *dev);

/* device (bd11_dev.c) */
B11Device *b11_device_create(void);
B11Context *b11_context_create(B11Device *dev, uint32_t type, uint32_t flags);
void b11_flush(B11Device *dev, int wait);
BD12CmdList *b11_cmd(B11Device *dev);        /* the open immediate list, creating it on demand */
int b11_wait_serial(B11Device *dev, uint64_t serial);
/* Host-visible upload slices of up to 32 MiB, carved from shared heaps and recycled once the GPU is done with
 * them: CPU-written buffer versions and staging copies without one vkAllocateMemory per update. */
#define B11_SLICE_MAX (32u << 20)
BD12Buffer *b11_slice_get(B11Device *dev, size_t size);
/* Returns a slice; it is reused after the work recorded so far has retired. */
void b11_slice_retire(B11Device *dev, BD12Buffer *slice);
/* Makes `slice` the buffer's contents (the previous version retires once the GPU is done with it). */
void b11_buffer_rename(B11Buffer *b, BD12Buffer *slice);
/* Waits until GPU work recorded up to `serial` (possibly still in the open list) has finished. */
void b11_wait_gpu_use(B11Device *dev, uint64_t serial);
VkDescriptorSet b11_resolve_hook(BD12CmdList *list, void *user, const BD12ShaderBinding *bindings,
                                 const VkDescriptorType *types, uint32_t binding_count,
                                 VkDescriptorSetLayout set_layout, int compute);
BD12Buffer *b11_buffer_current(B11Buffer *b);
BD12Buffer *b11_buffer_write_backing(B11Buffer *b, int discard);
extern void *g_b11_device_vtable[43];
B11HR b11_device_create_for_loader(void **device, void **context);

/* context (bd11_ctx.c) */
void b11_context_init_vtable(void);
void b11_context_replay(B11Context *immediate, B11CmdList *list);
void b11_state_clear(B11Context *ctx);
void b11_texture_use(B11Context *ctx, B11Texture *tex, uint32_t state);

/* swap chain (bd11_swap.c) */
B11HR b11_swapchain_create(B11Device *dev, const void *desc, void **out);
B11HR b11_texture_create(B11Device *dev, const B11TextureDesc *in, const uint8_t *init, void **out);
B11HR b11_dxgi_device_query(B11Device *dev, void **out);

/* objects (bd11_obj.c) */
void b11_device_install_creators(void **slots);
VkSamplerCreateInfo b11_sampler_info(const uint8_t *desc);
VkFormat b11_vk_format(uint32_t dxgi);
uint64_t b11_hash(const void *data, size_t size);
BD12Image *b11_texture_image(B11Texture *tex);
extern int g_b11_trace;
extern long g_b11_trace_frame;   /* frame being traced (BEER_BD11_TRACE=<n>, or the frame after BEER_BD11_TRACE_FILE appears); -1 = none */

#endif
