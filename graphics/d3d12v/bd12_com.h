#ifndef BD12_COM_H
#define BD12_COM_H

/* COM shim over BD12: guest-visible D3D12 objects (Win64 ms_abi vtables) that
 * forward 1:1 to the GPU-resident core. The only D3D12 implementation (D3D12CreateDevice always lands here). */

#include "bd12.h"
#include "bd12_pso.h"

#include <pthread.h>
#include <stdint.h>

#define MS __attribute__((ms_abi))

typedef uint64_t HR;
#define HR_OK UINT64_C(0)
#define HR_FALSE UINT64_C(1)
#define HR_E_NOTIMPL UINT64_C(0x80004001)
#define HR_E_NOINTERFACE UINT64_C(0x80004002)
#define HR_E_FAIL UINT64_C(0x80004005)
#define HR_E_INVALIDARG UINT64_C(0x80070057)
#define HR_E_OUTOFMEMORY UINT64_C(0x8007000e)
#define HR_NOT_FOUND UINT64_C(0x887a0002)
#define HR_DEVICE_REMOVED UINT64_C(0x887a0005)

enum {
    COM_DEVICE, COM_QUEUE, COM_ALLOCATOR, COM_LIST, COM_FENCE, COM_HEAP,
    COM_RESOURCE, COM_DESC_HEAP, COM_ROOT_SIG, COM_PSO, COM_SIGNATURE,
    COM_QUERY_HEAP, COM_KIND_COUNT
};

typedef struct ComDevice ComDevice;

#define COM_FLIGHT_SUBMISSIONS 16

typedef struct {
    uint64_t serial;
    uint64_t total_dispatch_count;
    uint64_t total_op_count;
    uint32_t dispatch_count;
    uint32_t op_count;
    uint32_t wait_count, signal_count;
    BD12FenceOp waits[9], signals[8];
    BD12DispatchFlight dispatches[BD12_FLIGHT_DISPATCHES];
    BD12FlightOp ops[BD12_FLIGHT_OPS];
} ComFlightSubmission;

/* Every guest-visible object starts with the vtable pointer. */
typedef struct ComObj {
    void **vtable;
    uint32_t refs;
    uint32_t kind;
    ComDevice *dev;
    void (*destroy)(struct ComObj *);
} ComObj;

/* Deferred destruction: objects the GPU may still read are freed once the
 * internal retire timeline passes the serial current at release time. */
typedef struct ComGrave {
    struct ComGrave *next;
    uint64_t serial;
    void (*fn)(ComDevice *, void *);
    void *ptr;
} ComGrave;

typedef struct ComWaiter {
    struct ComWaiter *next;
    ComObj *fence;
    uint64_t value;
    uint64_t event;
} ComWaiter;

struct ComDevice {
    ComObj obj;
    BD12Device *bd;
    BD12Queue *queue;             /* shared by every D3D12 queue */
    pthread_mutex_t submit_lock;  /* orders retire serials with submissions */
    BD12Fence *retire;
    uint64_t serial;              /* last submitted retire value */
    ComFlightSubmission flight[COM_FLIGHT_SUBMISSIONS];
    pthread_mutex_t grave_lock;
    ComGrave *grave_head, *grave_tail;
    pthread_mutex_t wait_lock;
    pthread_cond_t wait_cond;
    ComWaiter *waiters;
    int waiter_started;
};

extern ComDevice *g_com_device;
extern const uint8_t com_iid_iunknown[16];

/* Shared vtable slots for ID3D12Object-derived objects. */
HR MS com_obj_get_private(ComObj *self, const uint8_t *guid, uint32_t *size,
                          void *data);
HR MS com_obj_set_private(ComObj *self, const uint8_t *guid, uint32_t size,
                          const void *data);
HR MS com_obj_set_private_iface(ComObj *self, const uint8_t *guid, void *iface);
HR MS com_obj_set_name(ComObj *self, const uint16_t *name);
HR MS com_obj_get_device(ComObj *self, const uint8_t *riid, void **out);
HR com_obj_query(ComObj *self, const uint8_t *riid, void **out,
                 const uint8_t *own_iid, const uint8_t *extra_iid);
void com_obj_init(ComObj *obj, uint32_t kind, void **vtable, ComDevice *dev,
                  void (*destroy)(ComObj *));
uint32_t MS com_add_ref(ComObj *obj);
uint32_t MS com_release(ComObj *obj);
int com_iid_equal(const uint8_t *a, const uint8_t *b);
/* Frees ptr through fn once all GPU work submitted so far has completed. */
void com_defer(ComDevice *dev, void (*fn)(ComDevice *, void *), void *ptr);
void com_reap(ComDevice *dev);
/* One vkQueueSubmit2 that also advances the retire timeline. */
VkResult com_submit(ComDevice *dev, BD12CmdList *const *lists, uint32_t count,
                    const BD12FenceOp *waits, uint32_t wait_count,
                    const BD12FenceOp *signals, uint32_t signal_count);

typedef struct ComHeap {
    ComObj obj;
    BD12Heap *heap;
    uint8_t desc[48];
} ComHeap;

/* ID3D12QueryHeap: a Vulkan query pool. D3D12 heap types: 0 occlusion,
 * 1 timestamp, 2 pipeline statistics, 3 stream-output statistics. `written`
 * records (at record time) which queries were ended, so resolving a query that
 * never ran writes zero instead of waiting forever for its availability. */
typedef struct ComQueryHeap {
    ComObj obj;
    VkQueryPool pool;
    uint32_t heap_type;
    uint32_t count;
    uint8_t *written;
} ComQueryHeap;

typedef struct ComResource {
    ComObj obj;
    BD12Buffer *buffer;
    BD12Image *image;
    ComHeap *heap;                /* placed resources keep the heap alive */
    uint8_t desc[56];
    uint8_t props[20];
    uint32_t heap_flags;
    uint32_t dimension;           /* 1 buffer, 2 1D, 3 2D, 4 3D */
    uint64_t width;
    uint32_t height, depth_or_layers, mips, dxgi_format, flags;
    uint32_t planes;              /* 2 for combined depth/stencil */
} ComResource;

typedef struct ComDescHeap {
    ComObj obj;
    BD12DescriptorHeap *heap;
    uint32_t desc[4];
} ComDescHeap;

typedef struct ComRootSig {
    ComObj obj;
    BD12RootSignature *rs;
} ComRootSig;

typedef struct ComFence {
    ComObj obj;
    BD12Fence *fence;
} ComFence;

/* ID3D12StateObject (+ ID3D12StateObjectProperties as an embedded second interface). */
typedef struct ComStateObject {
    ComObj obj;
    void **props_vtable;
    BD12RtPipeline *rt;
    ComObj *global_rs;
} ComStateObject;

typedef struct ComSignature {
    ComObj obj;
    uint32_t stride;
    uint32_t arg_count;
    uint32_t arg_types[8];        /* D3D12_INDIRECT_ARGUMENT_TYPE */
} ComSignature;

/* Exported by bd12_com_list.c ------------------------------------------- */
HR com_create_allocator(ComDevice *dev, uint32_t type, const uint8_t *riid,
                        void **out);
HR com_create_list(ComDevice *dev, uint32_t type, ComObj *allocator,
                   ComObj *pso, const uint8_t *riid, void **out);
/* kind: 0 legacy graphics desc, 1 legacy compute desc, 2 stream desc. */
HR com_create_pso(ComDevice *dev, const void *desc, int kind,
                  const uint8_t *riid, void **out);
/* Closes the list if it is still open and returns its recorded commands. */
BD12CmdList *com_list_finish(ComObj *list);

/* Exported by bd12_com.c ------------------------------------------------- */
/* Presentation of a back buffer image (set by the loader). Returns nonzero on
 * success. layout is the layout the image is in. The callback must consume all
 * GPU references to image before returning; the guest may immediately release
 * the corresponding D3D12 resource during a swapchain/title transition. */
typedef int (*Bd12ImagePresenter)(VkImage image, VkImageLayout layout,
                                  uint32_t width, uint32_t height,
                                  uint32_t dxgi_format);
void bd12_com_set_image_presenter(Bd12ImagePresenter presenter);
Bd12ImagePresenter bd12_com_get_image_presenter(void);

uint64_t MS beer_bd12_create_device(const uint8_t *riid, void **device);

#endif
