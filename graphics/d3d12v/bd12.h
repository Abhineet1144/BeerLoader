#ifndef BD12_H
#define BD12_H

/* BD12: GPU-resident D3D12-on-Vulkan core (vkd3d-style 1:1 mapping).
 *
 *   ID3D12Device          -> BD12Device       (shared Vulkan 1.3 device)
 *   ID3D12Heap            -> BD12Heap         (VkDeviceMemory)
 *   ID3D12Resource        -> BD12Buffer       (VkBuffer bound into a heap)
 *   GPU virtual address   -> VkBuffer device address + sorted VA table
 *   ID3D12Fence           -> BD12Fence        (timeline VkSemaphore)
 *   ID3D12CommandQueue    -> BD12Queue        (vkQueueSubmit2, one per Execute)
 *   ID3D12CommandAllocator-> BD12CmdAllocator (VkCommandPool)
 *   ID3D12GraphicsCommandList -> BD12CmdList  (VkCommandBuffer recorded at
 *                                              call time, no snapshots)
 *
 * This layer is host-ABI C with no COM; the COM vtables are a thin shim added
 * on top in a later milestone. */

#include "vulkan_context.h"
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

/* D3D12_RESOURCE_STATES values (identical numeric encoding). */
enum {
    BD12_STATE_COMMON = 0,
    BD12_STATE_VERTEX_AND_CONSTANT_BUFFER = 0x1,
    BD12_STATE_INDEX_BUFFER = 0x2,
    BD12_STATE_RENDER_TARGET = 0x4,
    BD12_STATE_UNORDERED_ACCESS = 0x8,
    BD12_STATE_DEPTH_WRITE = 0x10,
    BD12_STATE_DEPTH_READ = 0x20,
    BD12_STATE_NON_PIXEL_SHADER_RESOURCE = 0x40,
    BD12_STATE_PIXEL_SHADER_RESOURCE = 0x80,
    BD12_STATE_STREAM_OUT = 0x100,
    BD12_STATE_INDIRECT_ARGUMENT = 0x200,
    BD12_STATE_COPY_DEST = 0x400,
    BD12_STATE_COPY_SOURCE = 0x800,
    BD12_STATE_RESOLVE_DEST = 0x1000,
    BD12_STATE_RESOLVE_SOURCE = 0x2000,
    BD12_STATE_DEPTH_READ_STENCIL_WRITE = 0x4000,   /* BD11: read-only depth (sampleable) with a writable stencil */
    BD12_STATE_GENERIC_READ = 0xac3,
    BD12_STATE_PRESENT = 0
};

typedef enum {
    BD12_HEAP_DEFAULT = 1,
    BD12_HEAP_UPLOAD = 2,
    BD12_HEAP_READBACK = 3
} BD12HeapType;

typedef struct BD12Device BD12Device;
typedef struct BD12RtPipeline BD12RtPipeline;

typedef struct {
    uint64_t va;
    uint64_t size;
    struct BD12Buffer *buffer;
} BD12VaEntry;

struct BD12Device {
    VkInstance instance;
    VkPhysicalDevice physical_device;
    PFN_vkGetPhysicalDeviceFormatProperties get_format_properties;   /* CheckFeatureSupport(FORMAT_SUPPORT) */
    VkDevice device;
    VkQueue queue;
    uint32_t queue_family;
    VkPipelineCache pipeline_cache;      /* persisted under ~/.cache/beer between runs */
    char pipeline_cache_path[512];
    uint32_t pipeline_cache_new;         /* pipelines built since the last save */
    uint64_t pipeline_cache_saved_ms;
    const BeerVulkanModern *modern;
    VkPhysicalDeviceMemoryProperties memory;
    char device_name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
    uint32_t buffer_memory_bits;
    int32_t memory_type[4];       /* indexed by BD12HeapType */
    int readback_coherent;
    VkDeviceSize min_uniform_alignment;
    VkDeviceSize max_uniform_range;
    uint32_t wave_lane_min;       /* Vulkan subgroup size range, reported as D3D12 wave widths */
    uint32_t wave_lane_max;
    VkDeviceSize min_uniform_texel_alignment;
    VkDeviceSize min_storage_texel_alignment;
    int uniform_texel_single_alignment;
    int storage_texel_single_alignment;

    pthread_mutex_t view_lock;    /* guards every BD12Buffer view cache */
    pthread_mutex_t va_lock;
    BD12VaEntry *va_table;        /* sorted by va */
    size_t va_count, va_capacity;

    struct BD12SamplerEntry *samplers; /* deduplicated, live until destroy */
    size_t sampler_count, sampler_capacity;

    struct {
#define BD12_VK_FN(name) PFN_vk##name name;
#include "bd12_vk_functions.h"
#undef BD12_VK_FN
    } vk;
    uint64_t epoch;               /* advances once per presented frame */
    /* Batched heap zero fills: every default heap is cleared by a transfer fill. Fills are recorded into one command
     * buffer and submitted together ahead of the next queue submit instead of one submission per heap. */
    pthread_mutex_t zero_lock;
    VkCommandPool zero_pool;
    VkCommandBuffer zero_cb;       /* batch being recorded */
    VkSemaphore zero_sem;          /* timeline: value of the newest submitted batch */
    uint64_t zero_recorded, zero_submitted;
    VkCommandBuffer zero_inflight[16];
    uint64_t zero_inflight_value[16];
    uint32_t zero_inflight_count;
    int rt_enabled;               /* acceleration structures usable */
    uint32_t rt_handle_size, rt_base_alignment, rt_handle_alignment, rt_max_recursion, rt_max_stride;
    struct {
#define BD12_VK_RT_FN(name) PFN_vk##name name;
#include "bd12_vk_rt_functions.h"
#undef BD12_VK_RT_FN
    } vk_rt;
};

typedef struct {
    BD12Device *device;
    VkDeviceMemory memory;
    VkDeviceSize size;
    BD12HeapType type;
    void *mapped;                 /* persistent mapping for UPLOAD/READBACK */
    VkBuffer zero_buffer;         /* DEFAULT heap zero-fill, freed with heap */
    uint64_t zero_value;          /* device zero-fill timeline value that covers this heap (0 = none) */
} BD12Heap;

/* Cached typed views over a buffer range (texel-buffer SRV/UAV descriptors).
 * Views live as long as the buffer, so descriptor sets written at record time
 * can never reference a destroyed view. */
typedef struct {
    VkFormat format;
    VkDeviceSize offset, range;
    VkBufferView view;
    uint64_t last_use;            /* device epoch of the last lookup */
    int pinned;                   /* referenced by a long-lived descriptor: never freed */
} BD12BufferView;

typedef struct BD12Buffer {
    BD12Device *device;
    BD12BufferView *views;
    uint32_t view_count, view_capacity;
    uint32_t *view_hash;          /* open-addressed index into views (0 = empty, else index+1) */
    uint32_t view_hash_mask;
    uint32_t view_gc_threshold;   /* view count that triggers the next age-out pass */
    VkBuffer buffer;
    BD12Heap *heap;
    int owns_heap;
    VkDeviceSize heap_offset;
    VkDeviceSize size;
    uint64_t va;                  /* D3D12 GPU virtual address */
    void *cpu;                    /* heap->mapped + offset, or NULL */
    uint32_t serial;              /* unique per buffer: descriptors remember it to detect pointer reuse */
    struct BD12AccelStruct *accel;/* acceleration structures placed in it */
    uint32_t accel_count, accel_capacity;
} BD12Buffer;

typedef struct BD12AccelStruct {
    VkDeviceSize offset, size;
    VkAccelerationStructureTypeKHR type;
    VkAccelerationStructureKHR handle;
} BD12AccelStruct;

typedef struct BD12SamplerEntry {
    VkSamplerCreateInfo info;
    VkSampler sampler;
} BD12SamplerEntry;

typedef struct {
    VkImageViewType type;
    VkFormat format;
    VkImageAspectFlags aspect;
    uint32_t base_mip, mip_count, base_layer, layer_count;
    VkImageView view;
} BD12ImageViewEntry;

typedef struct {
    VkImageType type;
    VkFormat format;
    uint32_t width, height;
    uint32_t depth_or_layers;     /* depth for 3D, array size otherwise */
    uint32_t mip_levels;
    uint32_t samples;
    int allow_render_target, allow_depth_stencil, allow_uav, cube_compatible;
} BD12ImageDesc;

/* ID3D12Resource (texture). Layouts are derived from D3D12 resource states
 * (BD12 never tracks layouts): COMMON/UAV -> GENERAL, RENDER_TARGET ->
 * COLOR_ATTACHMENT, SRV -> SHADER_READ_ONLY, COPY_* -> TRANSFER_*, etc. The
 * first command-list use transitions the whole image from UNDEFINED into the
 * layout of its creation state, like vkd3d. */
typedef struct BD12Image {
    BD12Device *device;
    VkImage image;
    BD12Heap *heap;
    int owns_heap;
    VkDeviceSize heap_offset;
    BD12ImageDesc desc;
    VkImageUsageFlags usage;
    VkImageAspectFlags aspect;    /* every aspect of the format */
    uint32_t initial_state;
    int initialized;              /* guarded by device->view_lock */
    BD12ImageViewEntry *views;
    uint32_t view_count, view_capacity;
    uint32_t serial;              /* unique per image (see BD12Descriptor.image_serial) */
} BD12Image;

typedef struct {
    BD12Device *device;
    VkSemaphore semaphore;
    uint64_t last_use;            /* retire serial of the newest submission that waits on or signals it */
    uint64_t signaled;            /* highest value signaled or queued; Vulkan timelines only move forward */
} BD12Fence;

typedef struct {
    BD12Fence *fence;
    uint64_t value;
} BD12FenceOp;

typedef struct {
    BD12Device *device;
    VkQueue queue;
    uint64_t submit_count;        /* vkQueueSubmit2 calls, for diagnostics */
} BD12Queue;

/* D3D12_DESCRIPTOR_RANGE_TYPE / shader descriptor classes. */
enum {
    BD12_RANGE_SRV = 0,
    BD12_RANGE_UAV = 1,
    BD12_RANGE_CBV = 2,
    BD12_RANGE_SAMPLER = 3,
    BD12_RANGE_APPEND = 0xffffffffu
};

typedef enum {
    BD12_DESC_NONE = 0,
    BD12_DESC_CBV,
    BD12_DESC_SRV_BUFFER,         /* uniform texel buffer */
    BD12_DESC_UAV_BUFFER,         /* storage texel buffer */
    BD12_DESC_SRV_IMAGE,
    BD12_DESC_UAV_IMAGE,
    BD12_DESC_SAMPLER,
    BD12_DESC_RTV,
    BD12_DESC_DSV,
    BD12_DESC_SRV_AS              /* ray tracing acceleration structure; `offset` holds its GPU VA */
} BD12DescriptorKind;

/* One D3D12 descriptor. The heap is plain host memory: descriptors are
 * resolved into Vulkan descriptor sets at record time. */
typedef struct {
    uint32_t kind;
    VkBuffer buffer;              /* CBV */
    VkDeviceSize offset, range;   /* CBV or source buffer byte range */
    struct BD12Buffer *source_buffer; /* diagnostic source for buffer views */
    VkBufferView buffer_view;     /* texel buffers */
    VkImageView image_view;
    VkImageLayout image_layout;
    VkSampler sampler;
    struct BD12Image *image;      /* first-use layout initialization */
    VkFormat format;              /* RTV/DSV */
    VkImageAspectFlags aspect;    /* DSV */
    uint32_t width, height, layer_count; /* RTV/DSV */
    uint32_t view_mip, view_base_layer, view_layers; /* image views, for clears */
    uint32_t buffer_serial, image_serial;  /* identity of source_buffer / image when written (pointers get reused) */
} __attribute__((aligned(128))) BD12Descriptor;   /* power-of-two stride: ray tracing shader records index the heap by shifting */
_Static_assert(sizeof(BD12Descriptor) == 128, "descriptor stride must stay 128 bytes (DXIL SBT shift)");

/* The guest sees descriptors as opaque handles advanced by this increment, so
 * the CPU handle of descriptor i is simply &descriptors[i]. */
#define BD12_DESCRIPTOR_INCREMENT ((uint32_t)sizeof(BD12Descriptor))

typedef struct {
    BD12Device *device;
    uint32_t type;                /* 0 = CBV/SRV/UAV, 1 = sampler */
    uint32_t count;
    BD12Descriptor *descriptors;
    uint64_t gpu_base;            /* fake GPU handle range, unique per heap */
    struct BD12HeapMirror *mirror;/* Vulkan mirror for bindless access, lazily */
    uint32_t sampler_gen;         /* sampler heaps: bumped on every descriptor write/copy */
} BD12DescriptorHeap;

typedef enum {
    BD12_ROOT_TABLE = 0,
    BD12_ROOT_CONSTANTS = 1,
    BD12_ROOT_CBV = 2,
    BD12_ROOT_SRV = 3,
    BD12_ROOT_UAV = 4
} BD12RootParameterType;

typedef struct {
    uint32_t type;                /* BD12_RANGE_* */
    uint32_t count;               /* UINT32_MAX = unbounded */
    uint32_t base_register;
    uint32_t space;
    uint32_t offset;              /* BD12_RANGE_APPEND = after previous range */
} BD12DescriptorRange;

typedef struct {
    uint32_t type;                /* BD12RootParameterType */
    uint32_t visibility;
    uint32_t shader_register;     /* CONSTANTS/CBV/SRV/UAV */
    uint32_t space;
    uint32_t dword_count;         /* CONSTANTS */
    BD12DescriptorRange *ranges;  /* TABLE (owned copy) */
    uint32_t range_count;
} BD12RootParameter;

#define BD12_MAX_ROOT_PARAMETERS 64
#define BD12_MAX_ROOT_CONSTANTS 64

typedef struct {
    uint32_t reg, space;
    VkSamplerCreateInfo info;
} BD12StaticSampler;

typedef struct {
    BD12Device *device;
    uint32_t parameter_count;
    BD12RootParameter parameters[BD12_MAX_ROOT_PARAMETERS];
    BD12StaticSampler *statics;
    uint32_t static_count;
    uint32_t serial;              /* unique per root signature (binding candidate caches key on it) */
} BD12RootSignature;

/* A shader descriptor declaration with its Vulkan binding in set 0; this is
 * the reflection the DXBC translator reports for every D3D register. */
typedef struct {
    uint32_t type;                /* BD12_RANGE_* */
    uint32_t space;
    uint32_t reg;
    uint32_t is_buffer;           /* resource is a buffer (else an image) */
    uint32_t count;
    uint32_t binding;             /* Vulkan binding in set 0 (dense once the pipeline is built) */
    uint32_t stage;               /* translator stage namespace: 0 compute, 1 vertex, 2 pixel, ... */
    uint32_t source_binding;      /* binding number the translator put in the SPIR-V */
    uint32_t bindless;            /* heap array (set 1), not a set-0 binding */
    uint32_t push_word;           /* bindless: push-constant word with the heap index */
    uint32_t heap_class;          /* bindless: heap-mirror binding 0..3 */
    uint32_t is_accel;            /* SRV is a ray tracing acceleration structure */
    /* Root parameters that can satisfy this binding, found once per root signature (resolve_one). */
    uint32_t cand_serial;         /* root signature serial the list below was built for (0 = none) */
    uint32_t cand_count;          /* 0xff = too many, scan every parameter */
    uint8_t cand[12];
} BD12ShaderBinding;

/* The translator numbers bindings stage*0x10000 + class*0x2000 + register, up
 * to several hundred thousand. NVIDIA sizes per-layout host tables by the
 * highest binding index (~8.6 MB per descriptor set layout, thousands of
 * pipelines), so a pipeline renumbers its bindings densely from *next and gets
 * a rewritten copy of the SPIR-V (free() it). `bindings` is updated in place
 * (source_binding/stage keep the translator's identity). Returns NULL on
 * allocation failure. */
uint32_t *bd12_densify_bindings(const uint32_t *spirv, size_t spirv_bytes,
                                BD12ShaderBinding *bindings, uint32_t count,
                                uint32_t *next);

typedef struct {
    BD12Device *device;
    VkPipelineLayout layout;
    VkPipeline pipeline;
    uint32_t push_bytes;
    VkDescriptorSetLayout set_layout; /* bound pipelines only */
    BD12ShaderBinding *bindless;      /* heap-array bindings (set 1) */
    uint32_t bindless_count, bindless_words;
    BD12ShaderBinding *bindings;
    VkDescriptorType *binding_types;  /* parallel to bindings */
    uint32_t binding_count;
    uint64_t shader_hash;             /* DXBC hash, zero for raw SPIR-V */
    uint32_t loop_count;
    uint32_t control_barrier_count;
    int writes_3d_storage_image;
} BD12ComputePipeline;

typedef struct {
    uint64_t value;               /* root CBV/SRV/UAV VA, or table GPU handle */
    uint32_t constants[BD12_MAX_ROOT_CONSTANTS];
    VkBuffer cbv_buffer;          /* scratch copy of constants, valid until */
    VkDeviceSize cbv_offset;      /* the constants change again */
    int cbv_valid;
} BD12RootArgument;

typedef struct {
    BD12RootSignature *root_signature;
    BD12RootArgument arguments[BD12_MAX_ROOT_PARAMETERS];
} BD12BindState;

/* Per-allocator record-time storage: descriptor pools and constant scratch.
 * Both are recycled when the allocator is reset (after the GPU is done). */
typedef struct {
    BD12Device *device;
    VkCommandPool pool;
    VkDescriptorPool *descriptor_pools;
    uint32_t descriptor_pool_count, descriptor_pool_capacity;
    uint32_t descriptor_pool_current;
    BD12Buffer **scratch;
    uint32_t scratch_count, scratch_capacity;
    uint32_t scratch_current;
    VkDeviceSize scratch_offset;
    struct BD12SetCache *set_cache;   /* descriptor sets written so far, reused for identical contents */
} BD12CmdAllocator;

/* Graphics pipelines (M4) -------------------------------------------------- */
typedef struct {
    const uint32_t *spirv;
    size_t spirv_bytes;
    const BD12ShaderBinding *bindings;  /* binding = stage*0x10000 + ... */
    uint32_t binding_count;
} BD12ShaderCode;

typedef struct {
    uint32_t location, binding, offset;
    VkFormat format;
    int per_instance;
} BD12VertexAttribute;

typedef struct {
    int blend_enable;
    VkBlendFactor src_color, dst_color, src_alpha, dst_alpha;
    VkBlendOp color_op, alpha_op;
    uint32_t write_mask;          /* VkColorComponentFlags */
    int logic_enable;
    VkLogicOp logic_op;
} BD12BlendTarget;

typedef struct {
    BD12ShaderCode vs, ps;        /* ps.spirv may be NULL (depth-only) */
    BD12ShaderCode hs, ds, gs;    /* optional; hs and ds come together */
    uint32_t patch_control_points; /* tessellation input patch size */
    const BD12VertexAttribute *attributes;
    uint32_t attribute_count;
    VkPrimitiveTopology topology;
    VkPolygonMode fill_mode;
    VkCullModeFlags cull_mode;
    int front_ccw;
    int depth_clip_enable;        /* off -> depth clamp */
    int depth_bias_enable;
    float depth_bias, depth_bias_clamp, slope_scaled_bias;
    int depth_test, depth_write;
    VkCompareOp depth_func;
    int stencil_test;
    VkStencilOpState stencil_front, stencil_back;
    uint32_t rt_count;
    VkFormat rt_formats[8];
    BD12BlendTarget blend[8];
    VkFormat ds_format;
    uint32_t samples;
    int alpha_to_coverage;
    int primitive_restart;        /* strip topologies only */
} BD12GraphicsPipelineDesc;

typedef struct {
    BD12Device *device;
    VkPipelineLayout layout;
    VkPipeline pipeline;
    VkDescriptorSetLayout set_layout;
    BD12ShaderBinding *bindless;      /* heap-array bindings (set 1) */
    uint32_t bindless_count, bindless_words;
    BD12ShaderBinding *bindings;
    VkDescriptorType *binding_types;
    uint32_t binding_count;
    uint32_t vertex_slot_count;   /* highest used vertex buffer slot + 1 */
    uint64_t shader_hash;         /* combined SPIR-V hash for diagnostics */
    VkShaderStageFlags stages;    /* every stage the pipeline carries */
    int tessellated;              /* draws must use a patch list */
} BD12GraphicsPipeline;

#define BD12_MAX_VERTEX_BUFFERS 16
#define BD12_MAX_RENDER_TARGETS 8
#define BD12_MAX_VIEWPORTS 16

typedef struct {
    VkBuffer buffer;
    VkDeviceSize offset, size;
    uint32_t stride;
    uint64_t va;                  /* GPU VA it was bound from, re-checked at draw time */
} BD12VertexBuffer;

enum {
    BD12_DIRTY_VIEWPORT = 1u << 0,
    BD12_DIRTY_SCISSOR = 1u << 1,
    BD12_DIRTY_VERTEX = 1u << 2,
    BD12_DIRTY_INDEX = 1u << 3,
    BD12_DIRTY_TOPOLOGY = 1u << 4,
    BD12_DIRTY_BLEND = 1u << 5,
    BD12_DIRTY_STENCIL = 1u << 6,
    BD12_DIRTY_PIPELINE = 1u << 7,
    BD12_DIRTY_ALL = 0xffu
};

typedef struct {
    BD12GraphicsPipeline *pipeline;
    BD12Descriptor rtv[BD12_MAX_RENDER_TARGETS];
    uint32_t rtv_count;
    BD12Descriptor dsv;
    int has_dsv;
    int rendering;                /* vkCmdBeginRendering is open */
    VkViewport viewports[BD12_MAX_VIEWPORTS];
    VkRect2D scissors[BD12_MAX_VIEWPORTS];
    uint32_t viewport_count, scissor_count;
    BD12VertexBuffer vertex_buffers[BD12_MAX_VERTEX_BUFFERS];
    VkBuffer index_buffer;
    uint64_t index_va;            /* GPU VA it was bound from, re-checked at draw time */
    VkDeviceSize index_offset;
    VkIndexType index_type;
    VkPrimitiveTopology topology;
    uint32_t patch_points;        /* control points when topology is a patch list */
    VkPrimitiveTopology bound_topology; /* last value given to the command buffer */
    VkDescriptorSet bound_set;    /* set 0 last bound; valid while the pipeline is not dirty */
    float blend_constants[4];
    uint32_t stencil_ref;
    uint32_t dirty;
    BD12Descriptor cap_rtv[BD12_MAX_RENDER_TARGETS]; /* render-target dump (diagnostic) */
    uint32_t cap_rtv_count;
    uint64_t cap_hash;
    BD12Descriptor cap_dsv;
    int cap_has_dsv;
    int cap_clear_depth;
    uint32_t cap_draws;
} BD12GfxState;

#define BD12_FLIGHT_DISPATCHES 64
#define BD12_FLIGHT_OPS 64

typedef struct {
    uint64_t shader_hash;
    uint32_t x, y, z;
    uint32_t loop_count, control_barrier_count;
    uint32_t segment;
    uint8_t indirect;
    uint8_t writes_3d_storage_image;
    uint8_t watchdog;
} BD12DispatchFlight;

typedef enum {
    BD12_FLIGHT_DRAW,
    BD12_FLIGHT_DRAW_INDEXED,
    BD12_FLIGHT_DRAW_INDIRECT,
    BD12_FLIGHT_DRAW_INDEXED_INDIRECT,
    BD12_FLIGHT_COPY_BUFFER,
    BD12_FLIGHT_COPY_BUFFER_IMAGE,
    BD12_FLIGHT_COPY_IMAGE_BUFFER,
    BD12_FLIGHT_COPY_IMAGE,
    BD12_FLIGHT_CLEAR,
    BD12_FLIGHT_RESOLVE
} BD12FlightOpType;

typedef struct {
    uint64_t pipeline_hash;
    uint64_t amount;
    uint32_t type;
    uint32_t width, height, depth;
    uint32_t segment;
} BD12FlightOp;

typedef struct BD12CmdList {
    BD12Device *device;
    BD12CmdAllocator *allocator;
    VkCommandBuffer command_buffer; /* current recording segment */
    VkCommandBuffer *command_buffers;
    uint32_t command_buffer_count, command_buffer_capacity;
    /* Segments allocated from the pool so far. Re-recording a list reuses them
     * instead of allocating new ones (the old handles could still be in flight
     * and cannot be freed here). */
    uint32_t command_buffer_allocated;

    int recording;
    /* Optional descriptor-set resolver (BD11): when set, draws and dispatches ask it for the
     * set-0 descriptor set instead of resolving against a root signature. */
    BD12RtPipeline *rt_pipeline;  /* SetPipelineState1 */
    VkDescriptorSet (*resolve_hook)(struct BD12CmdList *list, void *user,
                                    const BD12ShaderBinding *bindings,
                                    const VkDescriptorType *types, uint32_t binding_count,
                                    VkDescriptorSetLayout set_layout, int compute);
    void *resolve_user;
    BD12DescriptorHeap *heaps[2]; /* current CBV/SRV/UAV and sampler heaps */
    BD12ComputePipeline *compute_pipeline;
    BD12BindState compute;
    BD12BindState graphics;
    BD12GfxState gfx;
    /* Fixed-size rolling metadata only; no resource references are retained. */
    BD12DispatchFlight dispatch_flight[BD12_FLIGHT_DISPATCHES];
    uint64_t dispatch_count;
    BD12FlightOp op_flight[BD12_FLIGHT_OPS];
    uint64_t op_count;
} BD12CmdList;

/* Device ------------------------------------------------------------- */
BD12Device *bd12_device_create(void);
void bd12_pipeline_cache_note_new(BD12Device *device);
void bd12_pipeline_cache_save(BD12Device *device);
void bd12_device_destroy(BD12Device *device);

/* Heaps and buffers ---------------------------------------------------- */
BD12Heap *bd12_heap_create(BD12Device *device, BD12HeapType type,
                           VkDeviceSize size);
BD12Heap *bd12_heap_create_unzeroed(BD12Device *device, BD12HeapType type,
                                    VkDeviceSize size);
void bd12_heap_destroy(BD12Heap *heap);
/* Placed buffer. offset must satisfy the buffer's alignment (64 KiB-aligned
 * placement always does). */
BD12Buffer *bd12_buffer_create_placed(BD12Heap *heap, VkDeviceSize offset,
                                      VkDeviceSize size);
/* Committed buffer: dedicated heap of the requested type. */
BD12Buffer *bd12_buffer_create_committed(BD12Device *device, BD12HeapType type,
                                         VkDeviceSize size);
void bd12_buffer_destroy(BD12Buffer *buffer);
/* Maps a GPU virtual address back to its buffer; returns NULL if unmapped. */
BD12Buffer *bd12_va_resolve(BD12Device *device, uint64_t va,
                            VkDeviceSize *offset);

/* Fences and queues ----------------------------------------------------- */
BD12Fence *bd12_fence_create(BD12Device *device, uint64_t initial);
void bd12_fence_destroy(BD12Fence *fence);
uint64_t bd12_fence_completed(BD12Fence *fence);
int bd12_fence_signal_cpu(BD12Fence *fence, uint64_t value);
int bd12_fence_claim(BD12Fence *fence, uint64_t value);
/* Nonzero when the fence reached value within timeout_ns. */
int bd12_fence_wait(BD12Fence *fence, uint64_t value, uint64_t timeout_ns);

BD12Queue *bd12_queue_create(BD12Device *device);
void bd12_queue_destroy(BD12Queue *queue);
/* One vkQueueSubmit2 carrying every command buffer, wait and signal. */
VkResult bd12_queue_submit(BD12Queue *queue, BD12CmdList *const *lists,
                           uint32_t list_count, const BD12FenceOp *waits,
                           uint32_t wait_count, const BD12FenceOp *signals,
                           uint32_t signal_count);

/* Command recording ----------------------------------------------------- */
BD12CmdAllocator *bd12_cmd_allocator_create(BD12Device *device);
void bd12_cmd_allocator_destroy(BD12CmdAllocator *allocator);
int bd12_cmd_allocator_reset(BD12CmdAllocator *allocator);

BD12CmdList *bd12_cmd_list_create(BD12CmdAllocator *allocator);
void bd12_cmd_list_destroy(BD12CmdList *list, BD12CmdAllocator *allocator);
int bd12_cmd_list_begin(BD12CmdList *list);
int bd12_cmd_list_end(BD12CmdList *list);
/* Starts another primary command-buffer segment in the same D3D12 list. */
int bd12_cmd_append_segment(BD12CmdList *list);
/* Bounded metadata recorder; retains no Vulkan or resource references. */
void bd12_cmd_record_flight_op(BD12CmdList *list, BD12FlightOpType type,
                               uint64_t pipeline_hash, uint64_t amount,
                               uint32_t width, uint32_t height, uint32_t depth);

void bd12_cmd_copy_buffer(BD12CmdList *list, BD12Buffer *dst,
                          VkDeviceSize dst_offset, BD12Buffer *src,
                          VkDeviceSize src_offset, VkDeviceSize size);
/* D3D12 ResourceBarrier TRANSITION / UAV for buffers. */
void bd12_cmd_transition_buffer(BD12CmdList *list, BD12Buffer *buffer,
                                uint32_t before, uint32_t after);
void bd12_cmd_uav_barrier_buffer(BD12CmdList *list, BD12Buffer *buffer);
/* Makes prior GPU writes to a READBACK buffer visible to the host. */
void bd12_cmd_host_visibility(BD12CmdList *list, BD12Buffer *buffer);

/* Descriptor heaps ------------------------------------------------------- */
BD12DescriptorHeap *bd12_descriptor_heap_create(BD12Device *device,
                                                uint32_t type, uint32_t count);
void bd12_descriptor_heap_destroy(BD12DescriptorHeap *heap);
static inline uint64_t bd12_descriptor_gpu_handle(const BD12DescriptorHeap *heap,
                                                  uint32_t index)
{
    return heap->gpu_base + (uint64_t)index * BD12_DESCRIPTOR_INCREMENT;
}
/* Buffer SRV/UAV: typed texel-buffer view (R32_UINT for raw/structured). */
int bd12_descriptor_write_buffer_srv(BD12DescriptorHeap *heap, uint32_t index,
                                     BD12Buffer *buffer, VkDeviceSize offset,
                                     VkDeviceSize size, VkFormat format);
int bd12_descriptor_write_buffer_uav(BD12DescriptorHeap *heap, uint32_t index,
                                     BD12Buffer *buffer, VkDeviceSize offset,
                                     VkDeviceSize size, VkFormat format);
int bd12_descriptor_write_cbv(BD12DescriptorHeap *heap, uint32_t index,
                              BD12Buffer *buffer, VkDeviceSize offset,
                              VkDeviceSize size);
void bd12_descriptor_copy(BD12DescriptorHeap *dst, uint32_t dst_index,
                          const BD12DescriptorHeap *src, uint32_t src_index,
                          uint32_t count);
/* Cached typed view over a buffer range; NULL on failure. */
VkBufferView bd12_buffer_view(BD12Buffer *buffer, VkFormat format,
                              VkDeviceSize offset, VkDeviceSize range,
                              int storage);
/* Same, for a view that a long-lived descriptor keeps referencing: never aged out. */
VkBufferView bd12_buffer_view_pinned(BD12Buffer *buffer, VkFormat format,
                                     VkDeviceSize offset, VkDeviceSize range,
                                     int storage);
/* Called once per presented frame; unpinned views unused for a while are freed. */
void bd12_device_advance_epoch(BD12Device *device);

/* Root signatures --------------------------------------------------------- */
BD12RootSignature *bd12_root_signature_create(
    BD12Device *device, const BD12RootParameter *parameters, uint32_t count);
void bd12_root_signature_destroy(BD12RootSignature *signature);
/* Static samplers resolve sampler bindings by register/space. */
int bd12_root_signature_set_static_samplers(BD12RootSignature *signature,
                                            const BD12StaticSampler *samplers,
                                            uint32_t count);

/* Pipelines ------------------------------------------------------------------ */
/* Push-constant-only compute pipeline (no descriptors). */
BD12ComputePipeline *bd12_compute_pipeline_create(BD12Device *device,
                                                  const uint32_t *spirv,
                                                  size_t spirv_bytes,
                                                  uint32_t push_bytes);
/* Internal: shared pipeline builder (set_layout may be VK_NULL_HANDLE; the
 * pipeline takes ownership of a non-null set layout). */
BD12ComputePipeline *bd12_compute_pipeline_build(
    BD12Device *device, const uint32_t *spirv, size_t spirv_bytes,
    uint32_t push_bytes, VkDescriptorSetLayout set_layout,
    uint32_t bindless_words);
/* Root-signature-bound compute pipeline: set 0 is built from the shader's
 * reflected bindings and resolved against the root signature at dispatch. */
BD12ComputePipeline *bd12_compute_pipeline_create_bound(
    BD12Device *device, const uint32_t *spirv, size_t spirv_bytes,
    const BD12ShaderBinding *bindings, uint32_t binding_count);
/* Same, from DXBC through the shared vkd3d-shader translator (bd12_translate.c). */
BD12ComputePipeline *bd12_compute_pipeline_create_dxbc(
    BD12Device *device, const uint8_t *dxbc, size_t dxbc_size, uint64_t hash);
void bd12_compute_pipeline_destroy(BD12ComputePipeline *pipeline);
void bd12_cmd_dispatch(BD12CmdList *list, BD12ComputePipeline *pipeline,
                       const void *push, uint32_t x, uint32_t y, uint32_t z);

/* D3D12 state setters and bound dispatch (descriptor resolution at record
 * time into per-allocator descriptor sets). */
void bd12_cmd_set_descriptor_heaps(BD12CmdList *list,
                                   BD12DescriptorHeap *cbv_srv_uav,
                                   BD12DescriptorHeap *sampler);
void bd12_cmd_set_compute_root_signature(BD12CmdList *list,
                                         BD12RootSignature *signature);
void bd12_cmd_set_pipeline(BD12CmdList *list, BD12ComputePipeline *pipeline);
void bd12_cmd_set_compute_root_constants(BD12CmdList *list, uint32_t parameter,
                                         const uint32_t *data, uint32_t count,
                                         uint32_t dest_offset);
void bd12_cmd_set_compute_root_va(BD12CmdList *list, uint32_t parameter,
                                  uint64_t gpu_va);
void bd12_cmd_set_compute_root_table(BD12CmdList *list, uint32_t parameter,
                                     uint64_t gpu_handle);
/* Returns 0 (and records nothing) when a required binding cannot resolve. */
int bd12_cmd_dispatch_bound(BD12CmdList *list, uint32_t x, uint32_t y,
                            uint32_t z);

int bd12_cmd_dispatch_indirect_bound(BD12CmdList *list, BD12Buffer *args,
                                     VkDeviceSize offset);

/* Graphics (M4) ------------------------------------------------------------ */
BD12GraphicsPipeline *bd12_graphics_pipeline_create(
    BD12Device *device, const BD12GraphicsPipelineDesc *desc);
void bd12_graphics_pipeline_destroy(BD12GraphicsPipeline *pipeline);
/* Closes an open vkCmdBeginRendering; every non-draw command calls this. */
void bd12_cmd_end_rendering(BD12CmdList *list);
int bd12_binding_descriptor_type(const BD12ShaderBinding *binding,
                                 VkDescriptorType *type);
/* Allocates a descriptor set from the allocator's record-time pools (valid until it is reset). */
VkDescriptorSet bd12_allocate_descriptor_set(BD12CmdAllocator *allocator,
                                             VkDescriptorSetLayout layout);
/* A set holding exactly `writes` (their dstSet is ignored and overwritten): an identical set written earlier
 * from this allocator in the same frame, or a fresh one. Most draws bind the same resources as a recent draw,
 * and vkAllocateDescriptorSets + vkUpdateDescriptorSets per draw dominated driver time. */
VkDescriptorSet bd12_descriptor_set_for_writes(BD12CmdAllocator *allocator,
                                               VkDescriptorSetLayout layout,
                                               VkWriteDescriptorSet *writes,
                                               uint32_t count);
void bd12_set_cache_clear(BD12CmdAllocator *allocator);
void bd12_set_cache_free(BD12CmdAllocator *allocator);
VkDescriptorSet bd12_resolve_descriptor_set(
    BD12CmdList *list, BD12BindState *state, const BD12ShaderBinding *bindings,
    const VkDescriptorType *types, uint32_t binding_count,
    VkDescriptorSetLayout set_layout);

int bd12_descriptor_write_rtv(BD12DescriptorHeap *heap, uint32_t index,
                              BD12Image *image, VkFormat format, uint32_t mip,
                              uint32_t base_layer, uint32_t layer_count);
int bd12_descriptor_write_dsv(BD12DescriptorHeap *heap, uint32_t index,
                              BD12Image *image, VkFormat format, uint32_t mip,
                              uint32_t base_layer, uint32_t layer_count,
                              int read_only_depth, int read_only_stencil);

void bd12_cmd_set_graphics_root_signature(BD12CmdList *list,
                                          BD12RootSignature *signature);
void bd12_cmd_set_graphics_root_constants(BD12CmdList *list, uint32_t parameter,
                                          const uint32_t *data, uint32_t count,
                                          uint32_t dest_offset);
void bd12_cmd_set_graphics_root_va(BD12CmdList *list, uint32_t parameter,
                                   uint64_t gpu_va);
void bd12_cmd_set_graphics_root_table(BD12CmdList *list, uint32_t parameter,
                                      uint64_t gpu_handle);
void bd12_cmd_set_graphics_pipeline(BD12CmdList *list,
                                    BD12GraphicsPipeline *pipeline);
/* Descriptors are copied by value, like D3D12 OMSetRenderTargets. */
void bd12_cmd_set_render_targets(BD12CmdList *list,
                                 const BD12Descriptor *const *rtvs,
                                 uint32_t rtv_count, const BD12Descriptor *dsv);
void bd12_cmd_set_viewports(BD12CmdList *list, const VkViewport *viewports,
                            uint32_t count);   /* D3D convention (y down) */
void bd12_cmd_set_scissors(BD12CmdList *list, const VkRect2D *rects,
                           uint32_t count);
void bd12_cmd_set_topology(BD12CmdList *list, VkPrimitiveTopology topology);
/* D3D_PRIMITIVE_TOPOLOGY value; patch lists are mapped or dropped (no tessellation stages). */
void bd12_cmd_set_d3d_topology(BD12CmdList *list, uint32_t d3d_topology);
void bd12_cmd_set_vertex_buffers(BD12CmdList *list, uint32_t start,
                                 const uint64_t *gpu_va, const uint32_t *sizes,
                                 const uint32_t *strides, uint32_t count);
void bd12_cmd_set_index_buffer(BD12CmdList *list, uint64_t gpu_va,
                               uint32_t size, VkIndexType type);
void bd12_cmd_set_blend_constants(BD12CmdList *list, const float rgba[4]);
void bd12_cmd_set_stencil_ref(BD12CmdList *list, uint32_t ref);

int bd12_cmd_draw(BD12CmdList *list, uint32_t vertex_count,
                  uint32_t instance_count, uint32_t first_vertex,
                  uint32_t first_instance);
int bd12_cmd_draw_indexed(BD12CmdList *list, uint32_t index_count,
                          uint32_t instance_count, uint32_t first_index,
                          int32_t base_vertex, uint32_t first_instance);
/* Argument layouts match D3D12_DRAW(_INDEXED)_ARGUMENTS. */
int bd12_cmd_draw_indirect(BD12CmdList *list, BD12Buffer *args,
                           VkDeviceSize offset, uint32_t count,
                           uint32_t stride);
int bd12_cmd_draw_indexed_indirect(BD12CmdList *list, BD12Buffer *args,
                                   VkDeviceSize offset, uint32_t count,
                                   uint32_t stride);

int bd12_cmd_draw_indirect_count(BD12CmdList *list, BD12Buffer *args,
                                 VkDeviceSize offset, BD12Buffer *count_buffer,
                                 VkDeviceSize count_offset, uint32_t max_count,
                                 uint32_t stride);
int bd12_cmd_draw_indexed_indirect_count(
    BD12CmdList *list, BD12Buffer *args, VkDeviceSize offset,
    BD12Buffer *count_buffer, VkDeviceSize count_offset, uint32_t max_count,
    uint32_t stride);

void bd12_cmd_clear_rtv(BD12CmdList *list, const BD12Descriptor *rtv,
                        const float color[4], uint32_t rect_count,
                        const VkRect2D *rects);
void bd12_cmd_clear_dsv(BD12CmdList *list, const BD12Descriptor *dsv,
                        VkImageAspectFlags aspects, float depth,
                        uint32_t stencil, uint32_t rect_count,
                        const VkRect2D *rects);

/* D3D12_RESOURCE_STATES -> Vulkan synchronization2 stage/access. */
void bd12_state_sync(uint32_t state, int is_source,
                     VkPipelineStageFlags2 *stage, VkAccessFlags2 *access);

/* Images (M3) ------------------------------------------------------------ */
/* Placed texture in a DEFAULT heap; initial_state is the D3D12 creation state. */
BD12Image *bd12_image_create_placed(BD12Heap *heap, VkDeviceSize offset,
                                    const BD12ImageDesc *desc,
                                    uint32_t initial_state);
BD12Image *bd12_image_create_committed(BD12Device *device,
                                       const BD12ImageDesc *desc,
                                       uint32_t initial_state);
void bd12_image_destroy(BD12Image *image);
VkImageLayout bd12_layout_for_state(uint32_t state, int is_depth_stencil);
/* Cached view; NULL on failure. aspect 0 = sampled default (depth if the
 * format has depth, else color). */
VkImageView bd12_image_view(BD12Image *image, VkImageViewType type,
                            VkFormat format, VkImageAspectFlags aspect,
                            uint32_t base_mip,
                            uint32_t mip_count, uint32_t base_layer,
                            uint32_t layer_count);
/* Texel/block geometry of a format aspect. */
void bd12_format_block(VkFormat format, VkImageAspectFlags aspect,
                       uint32_t *block_width, uint32_t *block_height,
                       uint32_t *block_bytes);

#define BD12_ALL_SUBRESOURCES 0xffffffffu
/* D3D12 ResourceBarrier TRANSITION for a texture. subresource is the D3D12
 * index (mip + layer * mips + plane * mips * layers) or ALL_SUBRESOURCES. */
void bd12_cmd_transition_image(BD12CmdList *list, BD12Image *image,
                               uint32_t subresource, uint32_t before,
                               uint32_t after);
void bd12_cmd_uav_barrier_image(BD12CmdList *list, BD12Image *image);
/* Internal: called by every command that touches a texture. */
void bd12_image_ensure_initialized(BD12CmdList *list, BD12Image *image);

typedef struct {
    uint32_t subresource;
    int32_t x, y, z;
} BD12ImagePoint;

/* D3D12 CopyTextureRegion with a placed footprint (buffer <-> texture). The
 * buffer row pitch is in bytes. */
void bd12_cmd_copy_buffer_to_image(BD12CmdList *list, BD12Image *dst,
                                   BD12ImagePoint at, BD12Buffer *src,
                                   VkDeviceSize src_offset, uint32_t row_pitch,
                                   uint32_t width, uint32_t height,
                                   uint32_t depth);
void bd12_cmd_copy_image_to_buffer(BD12CmdList *list, BD12Buffer *dst,
                                   VkDeviceSize dst_offset, uint32_t row_pitch,
                                   BD12Image *src, BD12ImagePoint from,
                                   uint32_t width, uint32_t height,
                                   uint32_t depth);
void bd12_cmd_copy_image(BD12CmdList *list, BD12Image *dst, BD12ImagePoint at,
                         BD12Image *src, BD12ImagePoint from, uint32_t width,
                         uint32_t height, uint32_t depth);

/* Image and sampler descriptors. */
int bd12_descriptor_write_image_srv_aspect(BD12DescriptorHeap *heap, uint32_t index,
                                           BD12Image *image, VkImageViewType type,
                                           VkFormat format, uint32_t base_mip,
                                           uint32_t mip_count, uint32_t base_layer,
                                           uint32_t layer_count,
                                           VkImageAspectFlags aspect);
int bd12_descriptor_write_image_srv(BD12DescriptorHeap *heap, uint32_t index,
                                    BD12Image *image, VkImageViewType type,
                                    VkFormat format, uint32_t base_mip,
                                    uint32_t mip_count, uint32_t base_layer,
                                    uint32_t layer_count);
int bd12_descriptor_write_image_uav(BD12DescriptorHeap *heap, uint32_t index,
                                    BD12Image *image, VkImageViewType type,
                                    VkFormat format, uint32_t mip,
                                    uint32_t base_layer, uint32_t layer_count);
VkSampler bd12_sampler_get(BD12Device *device, const VkSamplerCreateInfo *info);
/* info is deduplicated device-wide; samplers live until the device dies. */
int bd12_descriptor_write_sampler(BD12DescriptorHeap *heap, uint32_t index,
                                  const VkSamplerCreateInfo *info);

/* Extras (bd12_extra.c) -------------------------------------------------- */
/* Size/alignment a placed image needs in a DEFAULT heap. */
int bd12_image_requirements(BD12Device *device, const BD12ImageDesc *desc,
                            VkDeviceSize *size, VkDeviceSize *alignment);
/* Full memory dependency (UAV barrier on all resources, aliasing barriers). */
void bd12_cmd_global_barrier(BD12CmdList *list);
int bd12_debug_sync_all(void);
void bd12_trace_frame_advance(void);
int bd12_trace_active(void);
/* D3D12 ClearUnorderedAccessView* on buffers (all four values equal) and
 * images. Colors are raw 32-bit patterns (uint) or floats. */
void bd12_cmd_clear_uav_buffer(BD12CmdList *list, BD12Buffer *buffer,
                               VkDeviceSize offset, VkDeviceSize size,
                               uint32_t value);
void bd12_cmd_clear_uav_image(BD12CmdList *list, const BD12Descriptor *uav,
                              const VkClearColorValue *color);
void bd12_cmd_resolve_image(BD12CmdList *list, BD12Image *dst,
                            uint32_t dst_subresource, BD12Image *src,
                            uint32_t src_subresource);
/* Copies a buffer range written by the CPU into a command stream (used for
 * small constant updates). */
void bd12_cmd_update_buffer(BD12CmdList *list, BD12Buffer *dst,
                            VkDeviceSize offset, VkDeviceSize size,
                            const void *data);


/* Opt-in host profiler (BEER_BD12_PROF=1): accumulated wall time per section,
 * printed about every two seconds by queue_execute. Racy by design. */
enum { BD12_PROF_EXECUTE, BD12_PROF_SUBMIT, BD12_PROF_PSO, BD12_PROF_RESOLVE,
       BD12_PROF_FENCE_WAIT, BD12_PROF_FINISH, BD12_PROF_PRESENT, BD12_PROF_HEAP_CREATE,
       BD12_PROF_HEAP_DESTROY,
       BD12_PROF_ALLOC_SET, BD12_PROF_UPDATE_SETS, BD12_PROF_COUNT };
extern uint64_t g_bd12_prof_ns[BD12_PROF_COUNT];
extern uint64_t g_bd12_prof_calls[BD12_PROF_COUNT];
static inline uint64_t bd12_prof_now(void)
{
    /* Per-draw timing is only read with BEER_BD12_PROF; skip the clock read on every draw otherwise. */
    if (!getenv("BEER_BD12_PROF")) return 0;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
#define BD12_PROF_BEGIN() const uint64_t bd12_prof_t0_ = bd12_prof_now()
#define BD12_PROF_END(id) do { g_bd12_prof_ns[id] += bd12_prof_now() - bd12_prof_t0_; \
                               ++g_bd12_prof_calls[id]; } while (0)
void bd12_prof_report(void);
/* Recorded-command counters for the per-frame stats lines (BEER_BD11_STATS): barriers, render passes. */
extern uint64_t g_bd12_cmd_counts[4];
enum { BD12_COUNT_BARRIER, BD12_COUNT_RENDER_PASS, BD12_COUNT_SUBMIT };


/* Work BD12 silently could not execute. Every site that skips a draw or
 * dispatch increments a counter; the totals are reported on stderr every few
 * seconds while nonzero so an unsupported path is visible instead of showing up
 * as missing geometry. */
enum {
    BD12_DROP_DRAW_NO_STATE, BD12_DROP_DRAW_NO_DESCRIPTORS,
    BD12_DROP_DISPATCH_NO_STATE, BD12_DROP_DISPATCH_NO_DESCRIPTORS,
    BD12_DROP_INDIRECT_MULTI_ARG, BD12_DROP_INDIRECT_DISPATCH_COUNT,
    BD12_DROP_PSO_FAILED, BD12_DROP_DRAW_PATCH_LIST, BD12_DROP_COUNT
};
extern uint64_t g_bd12_drops[BD12_DROP_COUNT];
#define BD12_DROP(reason) __atomic_add_fetch(&g_bd12_drops[reason], 1, __ATOMIC_RELAXED)


/* Queries (occlusion, timestamp). `index` addresses one query in `pool`. */
void bd12_cmd_begin_query(BD12CmdList *list, VkQueryPool pool, uint32_t index);
void bd12_cmd_end_query(BD12CmdList *list, VkQueryPool pool, uint32_t index);
void bd12_cmd_write_timestamp(BD12CmdList *list, VkQueryPool pool, uint32_t index);
/* Copies query results as 64-bit values into `dst` at `offset`. */
void bd12_cmd_copy_query_results(BD12CmdList *list, VkQueryPool pool,
                                 uint32_t first, uint32_t count,
                                 BD12Buffer *dst, VkDeviceSize offset);

/* Ray tracing acceleration structures (bd12_raytracing.c). `inputs` is a
 * D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS; sizes[] receives
 * result, scratch and update-scratch bytes. */
int bd12_rt_prebuild(BD12Device *device, const void *inputs, uint64_t sizes[3]);
/* `desc` is a D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC. */
void bd12_cmd_build_as(BD12CmdList *list, const void *desc);
void bd12_cmd_copy_as(BD12CmdList *list, uint64_t dst, uint64_t src, uint32_t mode);
void bd12_cmd_emit_as_info(BD12CmdList *list, uint64_t dest, uint32_t type,
                           uint32_t source_count, const uint64_t *sources);
void bd12_rt_buffer_release(BD12Buffer *buffer);
VkAccelerationStructureKHR bd12_rt_as_from_va(BD12Device *device, uint64_t va);
void bd12_rt_load(BD12Device *device);

/* Bindless descriptor heap mirror (bd12_mirror.c): a CBV/SRV/UAV heap's
 * descriptors mirrored into one update-after-bind Vulkan set (set 1) so shaders
 * can index the whole heap. */
#define BD12_MIRROR_COUNT 1000000u
VkDescriptorSetLayout bd12_mirror_layout(BD12Device *device);
void bd12_mirror_heap_register(BD12DescriptorHeap *heap);
void bd12_mirror_forget_image(struct BD12Image *image);
/* Creates the mirror now (call right after creating a shader-visible heap). */
void bd12_mirror_enable(BD12DescriptorHeap *heap);
void bd12_mirror_heap_unregister(BD12DescriptorHeap *heap);
/* A descriptor array starting at `first` was (re)written. */
void bd12_mirror_note(const void *first, uint32_t count);
/* Before a draw/dispatch outside rendering: creates the bound heap's mirror
 * and initializes images it exposes. Returns 0 if bindless is unavailable. */
int bd12_mirror_prepare(BD12CmdList *list);
int bd12_bindless_heap_index(BD12CmdList *list, BD12BindState *state,
                             const BD12ShaderBinding *b, uint32_t *index);
/* Binds set 1 and pushes the per-binding heap indices. */
int bd12_cmd_bind_bindless(BD12CmdList *list, BD12BindState *state,
                           VkPipelineBindPoint point, VkPipelineLayout layout,
                           VkShaderStageFlags stages,
                           const BD12ShaderBinding *bindless, uint32_t count,
                           uint32_t words);

int bd12_scratch_alloc(BD12CmdAllocator *a, VkDeviceSize size,
                       BD12Buffer **buffer, VkDeviceSize *offset, void **ptr);

/* Compute buffer capture of a traced frame (bd12_capture.c, diagnostic). */
void bd12_capture_srv_depth(BD12CmdList *list, struct BD12Image *image, uint32_t mip,
                            uint32_t base_layer, uint32_t layers, uint64_t shader, uint32_t reg);
int bd12_capture_rt_enabled(void);
void bd12_capture_rt_pass(BD12CmdList *list);
int bd12_capture_enabled(void);
int bd12_capture_wants(uint64_t hash);
void bd12_capture_begin(void);
void bd12_capture_note(uint32_t cls, uint32_t space, uint32_t reg, VkBuffer buffer,
                       VkDeviceSize offset, VkDeviceSize range);
void bd12_capture_record(BD12CmdList *list, int post, uint64_t hash,
                         uint32_t x, uint32_t y, uint32_t z);
void bd12_capture_note_image(uint32_t reg, uint32_t space, struct BD12Image *image,
                             uint32_t mip);
void bd12_capture_indirect(BD12CmdList *list, uint64_t pso, uint32_t type,
                           BD12Buffer *args, VkDeviceSize arg_offset,
                           BD12Buffer *count, VkDeviceSize count_offset,
                           uint32_t max_count, uint32_t stride);
void bd12_capture_readback(BD12Buffer *dst, VkDeviceSize offset, VkDeviceSize size, uint64_t src_va);
int bd12_capture_rb_pending(void);
void bd12_capture_rb_flush(void);
void bd12_capture_end(void);
void bd12_capture_flush(void);
int bd12_capture_pending(void);

/* DXR state objects on VK_KHR_ray_tracing_pipeline (bd12_rtpipe.c). A state object keeps its description (library
 * blobs are interned) and is translated and compiled lazily, on the first shader identifier request or dispatch. */
typedef struct BD12RtExport { char name[128]; char rename_from[128]; } BD12RtExport;
typedef struct BD12RtLibrary {
    const uint8_t *code;          /* interned, never freed while the process lives */
    size_t size;
    BD12RtExport *exports;        /* empty = every entry point of the library */
    uint32_t export_count;
} BD12RtLibrary;
typedef struct BD12RtHitGroup {
    char name[128];
    uint32_t type;                /* 0 triangles, 1 procedural */
    char any_hit[128], closest_hit[128], intersection[128];
} BD12RtHitGroup;
typedef struct BD12RtDesc {
    BD12RtLibrary *libraries;
    uint32_t library_count;
    BD12RtHitGroup *hit_groups;
    uint32_t hit_group_count;
    uint32_t max_payload, max_attributes, max_recursion;
    BD12RootSignature *global_root_signature;
} BD12RtDesc;
typedef struct BD12RtPipeline BD12RtPipeline;

const uint8_t *bd12_rt_intern_blob(const uint8_t *code, size_t size);
BD12RtPipeline *bd12_rt_pipeline_create(BD12Device *device, const BD12RtDesc *desc);   /* takes ownership of desc arrays */
void bd12_rt_pipeline_destroy(BD12RtPipeline *pipeline);
/* 32-byte shader identifier of an export or hit group (builds the pipeline on first use), or NULL. */
const uint8_t *bd12_rt_pipeline_identifier(BD12RtPipeline *pipeline, const char *name);
/* GPU checkpoints (BEER_VK_CHECKPOINTS=1): mark the next command, dump the last reached ones after a fault. */
void bd12_ckpt_mark(BD12CmdList *list, uint32_t kind, uint64_t a, uint64_t b, uint32_t w, uint32_t h, uint32_t d);
void bd12_ckpt_dump(void);
void bd12_zero_flush(BD12Device *device);
/* live-object registry for descriptor-referenced buffers and images (bd12_live.c) */
void bd12_live_add(const void *ptr);
void bd12_live_remove(const void *ptr);
int bd12_live_has(const void *ptr);
uint32_t bd12_live_serial(void);
/* 0 when the buffer/image a descriptor was written for has since been destroyed. */
int bd12_descriptor_alive(const BD12Descriptor *d);
void bd12_canary_arm(BD12Device *device);
void bd12_canary_check(const char *where);
int bd12_cmd_dispatch_rays(BD12CmdList *list, BD12RtPipeline *pipeline, const uint8_t *desc);

/* Shader-constant finder and tweaks (bd12_cbtweak.c): the manager's Graphics window watches the constant buffers
 * every draw uses, scans them for values that react to what you do in the game, and overrides chosen floats. */
extern int g_bd12_cbt_active;   /* nonzero while capture is on or any tweak is enabled */
void bd12_cbt_observe(uint64_t pso_hash, uint32_t stage, uint32_t space, uint32_t reg,
                      BD12Buffer *buffer, VkDeviceSize offset, VkDeviceSize range);
void bd12_cbt_frame_advance(void);
void bd12_cbt_handle(const char *cmd, const char *request, char *out, size_t size);

#endif
