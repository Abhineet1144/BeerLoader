#define _GNU_SOURCE
/* BD12 COM shim, part 1: device, queue, fence, heaps, resources, descriptors,
 * root signatures and the other creation-time objects. Command lists, command
 * allocators and pipeline states are in bd12_com_list.c. */

#include "bd12_com.h"
#include <sys/syscall.h>
#include <unistd.h>
#include "bd12_formats.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef void (*SignalEventFn)(uint64_t);
SignalEventFn beer_d3d12_get_event_signaler(void);  /* d3d12_api.c */

#define IID(...) { __VA_ARGS__ }
const uint8_t com_iid_iunknown[16] = IID(0, 0, 0, 0, 0, 0, 0, 0xc0,
                                         0, 0, 0, 0, 0, 0, 0, 0x46);
static const uint8_t iid_device[16] = IID(0xf1, 0x19, 0x98, 0x18, 0xb6, 0x1d,
    0x57, 0x4b, 0xbe, 0x54, 0x18, 0x21, 0x33, 0x9b, 0x85, 0xf7);
static const uint8_t iid_device1[16] = IID(0x80, 0xce, 0xac, 0x77, 0x8e, 0x63,
    0x65, 0x4e, 0x88, 0x95, 0xc1, 0xf2, 0x33, 0x86, 0x86, 0x3e);
static const uint8_t iid_device2[16] = IID(0x1e, 0xa4, 0xba, 0x30, 0x5b, 0xb1,
    0x5c, 0x47, 0xa0, 0xbb, 0x1a, 0xf5, 0xc5, 0xb6, 0x43, 0x28);
/* ID3D12Device3..5: exposed only when acceleration structures work. */
static const uint8_t iid_device3[16] = IID(0x15, 0xdc, 0xda, 0x81, 0xad, 0x2b,
    0x92, 0x43, 0x93, 0xc5, 0x10, 0x13, 0x45, 0xc4, 0xaa, 0x98);
static const uint8_t iid_device4[16] = IID(0x17, 0xdf, 0x65, 0xe8, 0xee, 0xa9,
    0xf6, 0x46, 0xa4, 0x63, 0x30, 0x98, 0x31, 0x5a, 0xa2, 0xe5);
static const uint8_t iid_device5[16] = IID(0x3b, 0x17, 0x4f, 0x8b, 0xea, 0x2f,
    0x80, 0x4b, 0x8f, 0x58, 0x43, 0x07, 0x19, 0x1a, 0xb9, 0x5d);
static const uint8_t iid_device_downlevel[16] = IID(0x3f, 0xee, 0xea, 0x74,
    0x4b, 0x2f, 0x6d, 0x47, 0x82, 0xba, 0x2b, 0x85, 0xcb, 0x49, 0xe3, 0x10);
static const uint8_t iid_queue[16] = IID(0xa6, 0x70, 0xc8, 0x0e, 0x7e, 0x5d,
    0x22, 0x4c, 0x8c, 0xfc, 0x5b, 0xaa, 0xe0, 0x76, 0x16, 0xed);
static const uint8_t iid_queue_downlevel[16] = IID(0xef, 0xc5, 0xa8, 0x38,
    0xcb, 0x7c, 0x81, 0x4e, 0x91, 0x4f, 0xa6, 0xe9, 0xd0, 0x72, 0xc4, 0x94);
static const uint8_t iid_fence[16] = IID(0xcf, 0x3d, 0x75, 0x0a, 0xd8, 0xc4,
    0x91, 0x4b, 0xad, 0xf6, 0xbe, 0x5a, 0x60, 0xd9, 0x5a, 0x76);
static const uint8_t iid_descriptor_heap[16] = IID(0x1d, 0x47, 0xfb, 0x8e,
    0x6c, 0x61, 0x49, 0x4f, 0x90, 0xf7, 0x12, 0x7b, 0xb7, 0x63, 0xfa, 0x51);
static const uint8_t iid_query_heap[16] = IID(0xae, 0x58, 0x96, 0x0d, 0x45,
    0xed, 0x9e, 0x46, 0xa6, 0x1d, 0x97, 0x0e, 0xc5, 0x83, 0xca, 0xb4);
static const uint8_t iid_resource[16] = IID(0xbe, 0x42, 0x64, 0x69, 0x2e, 0xa7,
    0x59, 0x40, 0xbc, 0x79, 0x5b, 0x5c, 0x98, 0x04, 0x0f, 0xad);
static const uint8_t iid_heap[16] = IID(0x02, 0x25, 0x3b, 0x6b, 0x51, 0x6e,
    0xb3, 0x45, 0x90, 0xee, 0x98, 0x84, 0x26, 0x5e, 0x8d, 0xf3);
static const uint8_t iid_command_signature[16] = IID(0x7c, 0x79, 0x6a, 0xc3,
    0x80, 0xec, 0x0a, 0x4f, 0x89, 0x85, 0xa7, 0xb2, 0x47, 0x50, 0x82, 0xd1);
static const uint8_t iid_root_signature[16] = IID(0x66, 0x6b, 0x4a, 0xc5, 0xdf,
    0x72, 0xe8, 0x4e, 0x8b, 0xe5, 0xa9, 0x46, 0xa1, 0x42, 0x92, 0x14);

int com_iid_equal(const uint8_t *a, const uint8_t *b)
{
    return a && memcmp(a, b, 16) == 0;
}

/* Objects ---------------------------------------------------------------- */

void com_obj_init(ComObj *obj, uint32_t kind, void **vtable, ComDevice *dev,
                  void (*destroy)(ComObj *))
{
    obj->vtable = vtable;
    obj->refs = 1;
    obj->kind = kind;
    obj->dev = dev;
    obj->destroy = destroy;
}

uint32_t MS com_add_ref(ComObj *obj)
{
    return obj ? __atomic_add_fetch(&obj->refs, 1, __ATOMIC_RELAXED) : 0;
}

uint32_t MS com_release(ComObj *obj)
{
    if (!obj) return 0;
    uint32_t refs = __atomic_sub_fetch(&obj->refs, 1, __ATOMIC_ACQ_REL);
    if (!refs) obj->destroy(obj);
    return refs;
}

/* Returns the object itself for an IUnknown or kind-specific IID. */
HR com_obj_query(ComObj *self, const uint8_t *riid, void **out,
                    const uint8_t *own_iid, const uint8_t *extra_iid)
{
    if (!out) return HR_E_INVALIDARG;
    *out = NULL;
    if (!riid || !self) return HR_E_NOINTERFACE;
    if (!com_iid_equal(riid, com_iid_iunknown) &&
        !com_iid_equal(riid, own_iid) &&
        !(extra_iid && com_iid_equal(riid, extra_iid))) {
        if (getenv("BEER_COM_DIAGNOSTICS")) {
            fprintf(stderr, "[BD12 QI] refused %02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x\n",
                    riid[3], riid[2], riid[1], riid[0], riid[5], riid[4], riid[7], riid[6],
                    riid[8], riid[9], riid[10], riid[11], riid[12], riid[13], riid[14], riid[15]);
        }
        return HR_E_NOINTERFACE;
    }
    com_add_ref(self);
    *out = self;
    return HR_OK;
}

/* Generic ID3D12Object slots ----------------------------------------------- */

HR MS com_obj_get_private(ComObj *self, const uint8_t *guid, uint32_t *size,
                             void *data)
{
    (void)self; (void)data;
    if (!guid || !size) return HR_E_INVALIDARG;
    *size = 0;
    return HR_NOT_FOUND;
}
HR MS com_obj_set_private(ComObj *self, const uint8_t *guid, uint32_t size,
                             const void *data)
{
    (void)self; (void)guid; (void)size; (void)data;
    return HR_OK;
}
HR MS com_obj_set_private_iface(ComObj *self, const uint8_t *guid,
                                   void *iface)
{
    (void)self; (void)guid; (void)iface;
    return HR_OK;
}
HR MS com_obj_set_name(ComObj *self, const uint16_t *name)
{
    (void)self; (void)name;
    return HR_OK;
}
HR MS com_obj_get_device(ComObj *self, const uint8_t *riid, void **out)
{
    if (!out) return HR_E_INVALIDARG;
    *out = NULL;
    if (!self || !self->dev) return HR_E_NOINTERFACE;
    ComObj *device = &self->dev->obj;
    (void)riid;
    com_add_ref(device);
    *out = device;
    return HR_OK;
}
static HR MS obj_notimpl(void)
{
    /* Names the guest caller of an unimplemented COM method (COM calls reach here without a trampoline). */
    static unsigned logged;
    if (logged < 32) {
        ++logged;
        fprintf(stderr, "[BD12] unimplemented COM method called from guest code at %p\n", __builtin_return_address(0));
    }
    return HR_E_NOTIMPL;
}
static HR MS obj_ok(void) { return HR_OK; }

/* Device state: deferred destruction, submission --------------------------- */

ComDevice *g_com_device;

/* Weak so standalone test builds link without the presenter. The presenter no
 * longer waits for its blit to finish, so a destroyed resource may still be
 * read by the previous frame's present blit. */
extern int __attribute__((weak)) vulkan_presenter_wait_source(void);
static void presenter_sync(void)
{
    if (vulkan_presenter_wait_source) vulkan_presenter_wait_source();
}

void com_reap(ComDevice *dev)
{
    uint64_t done = bd12_fence_completed(dev->retire);
    for (;;) {
        pthread_mutex_lock(&dev->grave_lock);
        ComGrave *grave = dev->grave_head;
        if (!grave || grave->serial > done) {
            pthread_mutex_unlock(&dev->grave_lock);
            return;
        }
        dev->grave_head = grave->next;
        if (!dev->grave_head) dev->grave_tail = NULL;
        pthread_mutex_unlock(&dev->grave_lock);
        presenter_sync();
        grave->fn(dev, grave->ptr);
        free(grave);
    }
}

void com_defer(ComDevice *dev, void (*fn)(ComDevice *, void *), void *ptr)
{
    pthread_mutex_lock(&dev->submit_lock);
    uint64_t serial = dev->serial;
    pthread_mutex_unlock(&dev->submit_lock);
    if (serial <= bd12_fence_completed(dev->retire)) {
        presenter_sync();
        fn(dev, ptr);
        return;
    }
    ComGrave *grave = malloc(sizeof(*grave));
    if (!grave) return;  /* leak rather than risk a use-after-free */
    *grave = (ComGrave){ NULL, serial, fn, ptr };
    pthread_mutex_lock(&dev->grave_lock);
    if (dev->grave_tail) dev->grave_tail->next = grave; else dev->grave_head = grave;
    dev->grave_tail = grave;
    pthread_mutex_unlock(&dev->grave_lock);
}

static void dump_dispatch_flight(ComDevice *dev, uint64_t serial,
                                 BD12CmdList *const *lists, uint32_t count,
                                 const BD12FenceOp *waits, uint32_t wait_count,
                                 const BD12FenceOp *signals, uint32_t signal_count)
{
    const char *directory = getenv("BEER_BD12_FLIGHT_RECORDER");
    if (!directory || !*directory) return;
    if (strcmp(directory, "1") == 0) directory = "/tmp";

    ComFlightSubmission *current =
        &dev->flight[serial % COM_FLIGHT_SUBMISSIONS];
    memset(current, 0, sizeof(*current));
    current->serial = serial;
    current->wait_count = wait_count;
    current->signal_count = signal_count;
    memcpy(current->waits, waits, wait_count * sizeof(*waits));
    memcpy(current->signals, signals, signal_count * sizeof(*signals));
    for (uint32_t l = 0; l < count; ++l) {
        const BD12CmdList *list = lists[l];
        if (!list) continue;
        current->total_dispatch_count += list->dispatch_count;
        current->total_op_count += list->op_count;
        const uint64_t first_op = list->op_count > BD12_FLIGHT_OPS ?
            list->op_count - BD12_FLIGHT_OPS : 0;
        for (uint64_t o = first_op; o < list->op_count; ++o) {
            if (current->op_count == BD12_FLIGHT_OPS) {
                memmove(current->ops, current->ops + 1,
                        (BD12_FLIGHT_OPS - 1) * sizeof(current->ops[0]));
                --current->op_count;
            }
            current->ops[current->op_count++] =
                list->op_flight[o % BD12_FLIGHT_OPS];
        }
        const uint64_t first = list->dispatch_count > BD12_FLIGHT_DISPATCHES ?
            list->dispatch_count - BD12_FLIGHT_DISPATCHES : 0;
        for (uint64_t d = first; d < list->dispatch_count; ++d) {
            if (current->dispatch_count == BD12_FLIGHT_DISPATCHES) {
                memmove(current->dispatches, current->dispatches + 1,
                        (BD12_FLIGHT_DISPATCHES - 1) *
                            sizeof(current->dispatches[0]));
                --current->dispatch_count;
            }
            current->dispatches[current->dispatch_count++] =
                list->dispatch_flight[d % BD12_FLIGHT_DISPATCHES];
        }
    }

    char path[512], temporary[544];
    snprintf(path, sizeof(path), "%s/beer-bd12-flight-%u.log", directory,
             (unsigned)(serial & 1));
    snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    FILE *out = fopen(temporary, "w");
    if (!out) return;
    const uint64_t completed = bd12_fence_completed(dev->retire);
    fprintf(out, "latest=%llu completed=%llu history=%u\n",
            (unsigned long long)serial, (unsigned long long)completed,
            COM_FLIGHT_SUBMISSIONS);
    const uint64_t first_serial = serial >= COM_FLIGHT_SUBMISSIONS ?
        serial - COM_FLIGHT_SUBMISSIONS + 1 : 1;
    for (uint64_t s = first_serial; s <= serial; ++s) {
        const ComFlightSubmission *submission =
            &dev->flight[s % COM_FLIGHT_SUBMISSIONS];
        if (submission->serial != s) continue;
        fprintf(out, "submit=%llu state=%s dispatches=%u total-dispatches=%llu "
                     "ops=%u total-ops=%llu\n",
                (unsigned long long)s, s <= completed ? "completed" : "pending",
                submission->dispatch_count,
                (unsigned long long)submission->total_dispatch_count,
                submission->op_count,
                (unsigned long long)submission->total_op_count);
        for (uint32_t w = 0; w < submission->wait_count; ++w)
            fprintf(out, " submit=%llu wait fence=%p value=%llu%s\n", (unsigned long long)s, (void *)submission->waits[w].fence,
                    (unsigned long long)submission->waits[w].value, submission->waits[w].fence == dev->retire ? " (retire)" : "");
        for (uint32_t w = 0; w < submission->signal_count; ++w)
            fprintf(out, " submit=%llu signal fence=%p value=%llu%s\n", (unsigned long long)s, (void *)submission->signals[w].fence,
                    (unsigned long long)submission->signals[w].value, submission->signals[w].fence == dev->retire ? " (retire)" : "");
        for (uint32_t d = 0; d < submission->dispatch_count; ++d) {
            const BD12DispatchFlight *entry = &submission->dispatches[d];
            fprintf(out,
                    " submit=%llu dispatch=%u shader=%016llx mode=%s "
                    "groups=%u,%u,%u loops=%u barriers=%u uav3d=%u watchdog=%u\n",
                    (unsigned long long)s, d,
                    (unsigned long long)entry->shader_hash,
                    entry->indirect ? "indirect" : "direct", entry->x,
                    entry->y, entry->z, entry->loop_count,
                    entry->control_barrier_count,
                    entry->writes_3d_storage_image, entry->watchdog);
        }
        static const char *const op_names[] = {
            "draw", "draw-indexed", "draw-indirect", "draw-indexed-indirect",
            "copy-buffer", "copy-buffer-image", "copy-image-buffer",
            "copy-image", "clear", "resolve"
        };
        for (uint32_t o = 0; o < submission->op_count; ++o) {
            const BD12FlightOp *entry = &submission->ops[o];
            const char *name = entry->type < sizeof(op_names) / sizeof(op_names[0]) ?
                               op_names[entry->type] : "unknown";
            fprintf(out, " submit=%llu op=%u type=%s pipeline=%016llx "
                         "amount=%llu dims=%u,%u,%u\n",
                    (unsigned long long)s, o, name,
                    (unsigned long long)entry->pipeline_hash,
                    (unsigned long long)entry->amount, entry->width,
                    entry->height, entry->depth);
        }
    }
    if (fclose(out) == 0) rename(temporary, path);
    else remove(temporary);
}

VkResult com_submit(ComDevice *dev, BD12CmdList *const *lists, uint32_t count,
                    const BD12FenceOp *waits, uint32_t wait_count,
                    const BD12FenceOp *signals, uint32_t signal_count)
{
    BD12FenceOp all_waits[9], all_signals[8];
    uint32_t total_waits = wait_count;
    uint32_t total_signals = signal_count;
    if (wait_count > 8 || signal_count > 7)
        return VK_ERROR_INITIALIZATION_FAILED;
    memcpy(all_waits, waits, wait_count * sizeof(*waits));
    BD12_PROF_BEGIN();
    pthread_mutex_lock(&dev->submit_lock);
    total_signals = 0;
    for (uint32_t i = 0; i < signal_count; ++i)
        if (bd12_fence_claim(signals[i].fence, signals[i].value))
            all_signals[total_signals++] = signals[i];
    const uint64_t previous = dev->serial;
    const uint64_t serial = previous + 1;
    if (previous)
        all_waits[total_waits++] = (BD12FenceOp){ dev->retire, previous };
    all_signals[total_signals++] = (BD12FenceOp){ dev->retire, serial };
    for (uint32_t i = 0; i < total_waits; ++i) __atomic_store_n(&all_waits[i].fence->last_use, serial, __ATOMIC_RELEASE);
    for (uint32_t i = 0; i < total_signals; ++i) __atomic_store_n(&all_signals[i].fence->last_use, serial, __ATOMIC_RELEASE);
    dump_dispatch_flight(dev, serial, lists, count, all_waits, total_waits, all_signals, total_signals);
    VkResult result = bd12_queue_submit(dev->queue, lists, count, all_waits,
                                        total_waits, all_signals,
                                        total_signals);
    if (result == VK_SUCCESS) ++dev->serial;
    pthread_mutex_unlock(&dev->submit_lock);
    { /* Diagnostic: BEER_BD12_SYNC_SUBMIT=1 drains the GPU after every submit. */
        static int drain = -1;
        if (drain < 0) drain = getenv("BEER_BD12_SYNC_SUBMIT") != NULL;
        if (drain && result == VK_SUCCESS) bd12_fence_wait(dev->retire, serial, UINT64_MAX);
    }
    com_reap(dev);
    BD12_PROF_END(BD12_PROF_SUBMIT);
    return result;
}

/* Fence waiter: signals guest events when timeline values are reached. */

static void *waiter_main(void *arg)
{
    ComDevice *dev = arg;
    pthread_mutex_lock(&dev->wait_lock);
    for (;;) {
        while (!dev->waiters) pthread_cond_wait(&dev->wait_cond, &dev->wait_lock);
        ComWaiter *ready = NULL, **link = &dev->waiters;
        while (*link) {
            ComWaiter *w = *link;
            ComFence *fence = (ComFence *)w->fence;
            if (bd12_fence_completed(fence->fence) >= w->value ||
                beer_vulkan_device_lost()) {
                *link = w->next;
                w->next = ready;
                ready = w;
            } else {
                link = &w->next;
            }
        }
        if (ready) {
            pthread_mutex_unlock(&dev->wait_lock);
            SignalEventFn signal = beer_d3d12_get_event_signaler();
            while (ready) {
                ComWaiter *w = ready;
                ready = w->next;
                if (signal && w->event) signal(w->event);
                com_release(w->fence);
                free(w);
            }
            pthread_mutex_lock(&dev->wait_lock);
            continue;
        }
        ComFence *first = (ComFence *)dev->waiters->fence;
        uint64_t value = dev->waiters->value;
        com_add_ref(&first->obj);
        pthread_mutex_unlock(&dev->wait_lock);
        bd12_fence_wait(first->fence, value, 1000000);
        com_release(&first->obj);
        pthread_mutex_lock(&dev->wait_lock);
    }
    return NULL;
}

static void add_waiter(ComDevice *dev, ComFence *fence, uint64_t value,
                       uint64_t event)
{
    ComWaiter *w = calloc(1, sizeof(*w));
    if (!w) return;
    w->fence = &fence->obj;
    w->value = value;
    w->event = event;
    com_add_ref(&fence->obj);
    pthread_mutex_lock(&dev->wait_lock);
    if (!dev->waiter_started) {
        pthread_t thread;
        if (pthread_create(&thread, NULL, waiter_main, dev) == 0) {
            pthread_detach(thread);
            dev->waiter_started = 1;
        }
    }
    w->next = dev->waiters;
    dev->waiters = w;
    pthread_cond_signal(&dev->wait_cond);
    pthread_mutex_unlock(&dev->wait_lock);
}

/* Fence ------------------------------------------------------------------- */

static void fence_free(ComDevice *dev, void *ptr)
{
    ComFence *f = ptr;
    /* The semaphore must outlive every submission that signals or waits on it; a GPU signal into a destroyed
     * semaphore faults (NVIDIA Xid 31, write to address 0) and hangs the game. */
    const uint64_t use = __atomic_load_n(&f->fence->last_use, __ATOMIC_ACQUIRE);
    if (use > bd12_fence_completed(dev->retire)) {
        fprintf(stderr, "[BD12] fence %p released while submission %llu still uses it (completed %llu); waiting\n",
                (void *)f->fence, (unsigned long long)use, (unsigned long long)bd12_fence_completed(dev->retire));
        bd12_fence_wait(dev->retire, use, UINT64_MAX);
    }
    bd12_fence_destroy(f->fence);
    free(f);
}
static void fence_destroy(ComObj *o) { com_defer(o->dev, fence_free, o); }

static HR MS fence_query(ComObj *s, const uint8_t *r, void **o)
{ return com_obj_query(s, r, o, iid_fence, NULL); }
static uint64_t MS fence_completed(ComFence *f)
{
    return bd12_fence_completed(f->fence);
}
static int fence_diag(void)
{
    static int on = -1;
    if (on < 0) on = getenv("BEER_FENCE_DIAGNOSTICS") != NULL;
    return on;
}
static HR MS fence_set_event(ComFence *f, uint64_t value, uint64_t event)
{
    if (fence_diag())
        fprintf(stderr, "[FENCE] SetEventOnCompletion fence=%p value=%llu completed=%llu event=0x%llx\n", (void *)f,
                (unsigned long long)value, (unsigned long long)bd12_fence_completed(f->fence), (unsigned long long)event);
    if (bd12_fence_completed(f->fence) >= value) {
        SignalEventFn signal = beer_d3d12_get_event_signaler();
        if (signal && event) signal(event);
        return HR_OK;
    }
    if (!event) {
        bd12_fence_wait(f->fence, value, UINT64_MAX);
        return HR_OK;
    }
    add_waiter(f->obj.dev, f, value, event);
    return HR_OK;
}
static HR MS fence_signal(ComFence *f, uint64_t value)
{
    if (fence_diag())
        fprintf(stderr, "[FENCE] CPU Signal fence=%p value=%llu\n", (void *)f, (unsigned long long)value);
    return bd12_fence_signal_cpu(f->fence, value) ? HR_OK : HR_E_FAIL;
}
static void *fence_vtable[11] = {
    [0] = fence_query, [1] = com_add_ref, [2] = com_release,
    [3] = com_obj_get_private, [4] = com_obj_set_private, [5] = com_obj_set_private_iface,
    [6] = com_obj_set_name, [7] = com_obj_get_device, [8] = fence_completed,
    [9] = fence_set_event, [10] = fence_signal,
};

/* Queue -------------------------------------------------------------------- */

typedef struct ComQueue {
    ComObj obj;
    struct { void **vtable; } downlevel;
    uint32_t desc[4];
} ComQueue;

static Bd12ImagePresenter g_presenter;
void bd12_com_set_image_presenter(Bd12ImagePresenter p) { g_presenter = p; }
Bd12ImagePresenter bd12_com_get_image_presenter(void) { return g_presenter; }

static void queue_destroy(ComObj *o) { free(o); }

static HR MS queue_query(ComQueue *q, const uint8_t *riid, void **out)
{
    if (!out) return HR_E_INVALIDARG;
    *out = NULL;
    if (!riid) return HR_E_NOINTERFACE;
    if (com_iid_equal(riid, iid_queue_downlevel)) {
        com_add_ref(&q->obj);
        *out = &q->downlevel;
        return HR_OK;
    }
    return com_obj_query(&q->obj, riid, out, iid_queue, NULL);
}

static void MS queue_execute(ComQueue *q, uint32_t count, ComObj *const *lists)
{
    BD12_PROF_BEGIN();
    { extern uint64_t g_bd12_render_tid; g_bd12_render_tid = (uint64_t)syscall(SYS_gettid); }
    bd12_prof_report();
    BD12CmdList *batch[64];
    uint32_t n = 0;
    ComDevice *dev = q->obj.dev;
    for (uint32_t i = 0; i < count; ++i) {
        BD12CmdList *recorded = lists[i] ? com_list_finish(lists[i]) : NULL;
        if (!recorded) continue;
        batch[n++] = recorded;
        if (n == 64) {
            com_submit(dev, batch, n, NULL, 0, NULL, 0);
            n = 0;
        }
    }
    if (n) com_submit(dev, batch, n, NULL, 0, NULL, 0);
    BD12_PROF_END(BD12_PROF_EXECUTE);
}

static HR MS queue_signal(ComQueue *q, ComFence *f, uint64_t value)
{
    if (q && q->obj.kind != COM_QUEUE)
        fprintf(stderr, "[BD12] ID3D12CommandQueue::Signal on a non-queue object %p (kind=%d)\n", (void *)q, (int)q->obj.kind);
    if (!q || q->obj.kind != COM_QUEUE || !f || f->obj.kind != COM_FENCE || !f->fence) {
        fprintf(stderr, "[BD12] ID3D12CommandQueue::Signal(queue=%p fence=%p kind=%d inner=%p, %llu): invalid fence\n",
                (void *)q, (void *)f, f ? (int)f->obj.kind : -1, f ? (void *)f->fence : NULL,
                (unsigned long long)value);
        return HR_E_INVALIDARG;
    }
    BD12FenceOp s = { f->fence, value };
    if (fence_diag())
        fprintf(stderr, "[FENCE] queue %p Signal fence=%p value=%llu\n", (void *)q, (void *)f, (unsigned long long)value);
    return com_submit(q->obj.dev, NULL, 0, NULL, 0, &s, 1) == VK_SUCCESS ?
           HR_OK : HR_E_FAIL;
}
static HR MS queue_wait(ComQueue *q, ComFence *f, uint64_t value)
{
    if (!q || !f) return HR_E_INVALIDARG;
    BD12FenceOp w = { f->fence, value };
    if (fence_diag())
        fprintf(stderr, "[FENCE] queue %p Wait fence=%p value=%llu\n", (void *)q, (void *)f, (unsigned long long)value);
    return com_submit(q->obj.dev, NULL, 0, &w, 1, NULL, 0) == VK_SUCCESS ?
           HR_OK : HR_E_FAIL;
}
static HR MS queue_frequency(ComQueue *q, uint64_t *freq)
{
    (void)q;
    if (freq) *freq = 1000000000ull;
    return HR_OK;
}
static HR MS queue_clock(ComQueue *q, uint64_t *gpu, uint64_t *cpu)
{
    (void)q;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    if (gpu) *gpu = now;
    if (cpu) *cpu = now;
    return HR_OK;
}
static void *MS queue_get_desc(ComQueue *q, uint32_t *result)
{
    memcpy(result, q->desc, sizeof(q->desc));
    return result;
}
static void MS queue_marker(ComQueue *q, uint32_t a, const void *b, uint32_t c)
{ (void)q; (void)a; (void)b; (void)c; }
static void MS queue_end_event(ComQueue *q) { (void)q; }
static void MS queue_tile_noop(void) {}

static void *queue_vtable[19] = {
    [0] = queue_query, [1] = com_add_ref, [2] = com_release,
    [3] = com_obj_get_private, [4] = com_obj_set_private, [5] = com_obj_set_private_iface,
    [6] = com_obj_set_name, [7] = com_obj_get_device, [8] = queue_tile_noop,
    [9] = queue_tile_noop, [10] = queue_execute, [11] = queue_marker,
    [12] = queue_marker, [13] = queue_end_event, [14] = queue_signal,
    [15] = queue_wait, [16] = queue_frequency, [17] = queue_clock,
    [18] = queue_get_desc,
};

static ComQueue *queue_from_downlevel(void *iface)
{
    return (ComQueue *)((uint8_t *)iface - offsetof(ComQueue, downlevel));
}
static HR MS dl_query(void *iface, const uint8_t *riid, void **out)
{
    return queue_query(queue_from_downlevel(iface), riid, out);
}
static uint32_t MS dl_add_ref(void *iface)
{
    return com_add_ref(&queue_from_downlevel(iface)->obj);
}
static uint32_t MS dl_release(void *iface)
{
    return com_release(&queue_from_downlevel(iface)->obj);
}

static HR present_epilogue(ComQueue *q, ComResource *texture);
static HR MS dl_present(void *iface, ComObj *list, ComResource *texture,
                        uint64_t window, uint32_t flags)
{
    (void)window; (void)flags;
    ComQueue *q = queue_from_downlevel(iface);
    ComDevice *dev = q->obj.dev;
    if (!list || !texture || !texture->image) {
        bd12_trace_frame_advance();
        return HR_E_INVALIDARG;
    }
    BD12CmdList *recorded = com_list_finish(list);
    if (recorded) com_submit(dev, &recorded, 1, NULL, 0, NULL, 0);
    return present_epilogue(q, texture);
}

/* Everything after the recorded work is submitted: epoch advance, capture hooks, then hand the image to the
 * presenter. Shared by the downlevel Present and the DXGI swapchain. */
static HR present_epilogue(ComQueue *q, ComResource *texture)
{
    ComDevice *dev = q->obj.dev;
    if (getenv("BEER_BD12_TS_LOG")) {
        extern BD12Buffer *g_ts_dst; extern VkDeviceSize g_ts_offset; extern uint32_t g_ts_count;
        static unsigned frame;
        if (g_ts_dst && g_ts_dst->cpu && (++frame % 30) == 0) {
            bd12_fence_wait(dev->retire, dev->serial, UINT64_MAX);
            const uint64_t *t = (const uint64_t *)((char *)g_ts_dst->cpu + g_ts_offset);
            fprintf(stderr, "[TS] count=%u t0=%llu t1=%llu delta_ns=%lld\n", g_ts_count,
                    (unsigned long long)t[0], (unsigned long long)(g_ts_count > 1 ? t[1] : 0),
                    (long long)(g_ts_count > 1 ? (long long)(t[1] - t[0]) : 0));
        }
    }
    bd12_device_advance_epoch(dev->bd);
    if (bd12_capture_rb_pending()) {
        bd12_fence_wait(dev->retire, dev->serial, UINT64_MAX);
        bd12_capture_rb_flush();
    }
    if (bd12_capture_pending()) {      /* traced frame: drain, then write files */
        bd12_fence_wait(dev->retire, dev->serial, UINT64_MAX);
        bd12_capture_flush();
    }
    bd12_trace_frame_advance();
    if (!g_presenter) return HR_OK;
    BD12_PROF_BEGIN();
    const int presented = g_presenter(texture->image->image,
                     bd12_layout_for_state(BD12_STATE_PRESENT, 0),
                     (uint32_t)texture->width, texture->height,
                     texture->dxgi_format);
    BD12_PROF_END(BD12_PROF_PRESENT);
    if (!presented) return HR_E_NOTIMPL;
    return HR_OK;
}
static void *downlevel_vtable[4] = {
    [0] = dl_query, [1] = dl_add_ref, [2] = dl_release, [3] = dl_present,
};

/* Heaps -------------------------------------------------------------------- */

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static float rdf(const uint8_t *p) { float v; memcpy(&v, p, 4); return v; }

static BD12HeapType heap_type_from_props(const uint8_t *props)
{
    uint32_t type = rd32(props), page = rd32(props + 4);
    switch (type) {
    case 2: return BD12_HEAP_UPLOAD;
    case 3: return BD12_HEAP_READBACK;
    case 4: return page == 2 ? BD12_HEAP_UPLOAD :
                   page == 3 ? BD12_HEAP_READBACK : BD12_HEAP_DEFAULT;
    default: return BD12_HEAP_DEFAULT;
    }
}

static void heap_free(ComDevice *dev, void *ptr)
{
    (void)dev;
    ComHeap *h = ptr;
    bd12_heap_destroy(h->heap);
    free(h);
}
static void heap_destroy(ComObj *o) { com_defer(o->dev, heap_free, o); }
static HR MS heap_query(ComObj *s, const uint8_t *r, void **o)
{ return com_obj_query(s, r, o, iid_heap, NULL); }
static void *MS heap_get_desc(ComHeap *h, void *result)
{
    memcpy(result, h->desc, sizeof(h->desc));
    return result;
}
static void *heap_vtable[9] = {
    [0] = heap_query, [1] = com_add_ref, [2] = com_release,
    [3] = com_obj_get_private, [4] = com_obj_set_private, [5] = com_obj_set_private_iface,
    [6] = com_obj_set_name, [7] = com_obj_get_device, [8] = heap_get_desc,
};

static HR create_heap(ComDevice *dev, const uint8_t *desc, const uint8_t *riid,
                      void **out)
{
    if (!desc || !riid || !out) return HR_E_INVALIDARG;
    *out = NULL;
    if (!com_iid_equal(riid, iid_heap) && !com_iid_equal(riid, com_iid_iunknown))
        return HR_E_NOINTERFACE;
    uint64_t size = rd64(desc);
    if (!size) return HR_E_INVALIDARG;
    size = (size + 65535) & ~(uint64_t)65535;
    ComHeap *h = calloc(1, sizeof(*h));
    if (!h) return HR_E_OUTOFMEMORY;
    h->heap = bd12_heap_create(dev->bd, heap_type_from_props(desc + 8), size);
    if (!h->heap) {
        free(h);
        return HR_E_OUTOFMEMORY;
    }
    memcpy(h->desc, desc, sizeof(h->desc));
    com_obj_init(&h->obj, COM_HEAP, heap_vtable, dev, heap_destroy);
    *out = h;
    return HR_OK;
}

/* Resources ------------------------------------------------------------------ */

static void resource_free(ComDevice *dev, void *ptr)
{
    (void)dev;
    ComResource *r = ptr;
    if (r->buffer) bd12_buffer_destroy(r->buffer);
    if (r->image) bd12_image_destroy(r->image);
    if (r->heap) com_release(&r->heap->obj);
    free(r);
}
static void resource_destroy(ComObj *o) { com_defer(o->dev, resource_free, o); }

static HR MS resource_query(ComObj *s, const uint8_t *r, void **o)
{ return com_obj_query(s, r, o, iid_resource, NULL); }

static HR MS resource_map(ComResource *r, uint32_t sub, const void *range,
                          void **data)
{
    (void)range;
    if (data) *data = NULL;
    if (!r->buffer || !r->buffer->cpu) {
        static int logs;
        if (logs++ < 16)
            fprintf(stderr, "[BD12] Map(subresource %u) failed: %s resource fmt=%u %llux%u\n",
                    sub, r->image ? "texture" : "no-cpu", r->dxgi_format,
                    (unsigned long long)r->width, r->height);
        return HR_E_FAIL;
    }
    if (data) *data = r->buffer->cpu;
    return HR_OK;
}
static void MS resource_unmap(ComResource *r, uint32_t sub, const void *range)
{ (void)r; (void)sub; (void)range; }
static void *MS resource_get_desc(ComResource *r, void *result)
{
    memcpy(result, r->desc, sizeof(r->desc));
    return result;
}
static uint64_t MS resource_get_va(ComResource *r)
{
    return r->buffer ? r->buffer->va : 0;
}
static HR MS resource_get_heap_props(ComResource *r, void *props,
                                     uint32_t *flags)
{
    if (props) memcpy(props, r->props, sizeof(r->props));
    if (flags) *flags = r->heap_flags;
    return HR_OK;
}
static HR MS resource_write_sub(ComResource *r, uint32_t sub, const void *box,
                                const void *src, uint32_t row_pitch,
                                uint32_t depth_pitch)
{
    static int logs;
    if (logs++ < 16)
        fprintf(stderr, "[BD12] WriteToSubresource(%u) fmt=%u %llux%u pitch=%u box=%p NOT IMPLEMENTED\n",
                sub, r->dxgi_format, (unsigned long long)r->width, r->height,
                row_pitch, box);
    (void)src; (void)depth_pitch;
    return HR_E_NOTIMPL;
}
static HR MS resource_read_sub(ComResource *r, void *dst, uint32_t row_pitch,
                               uint32_t depth_pitch, uint32_t sub, const void *box)
{
    static int logs;
    if (logs++ < 16)
        fprintf(stderr, "[BD12] ReadFromSubresource(%u) fmt=%u %llux%u NOT IMPLEMENTED\n",
                sub, r->dxgi_format, (unsigned long long)r->width, r->height);
    (void)dst; (void)row_pitch; (void)depth_pitch; (void)box;
    return HR_E_NOTIMPL;
}

static void *resource_vtable[15] = {
    [0] = resource_query, [1] = com_add_ref, [2] = com_release,
    [3] = com_obj_get_private, [4] = com_obj_set_private, [5] = com_obj_set_private_iface,
    [6] = com_obj_set_name, [7] = com_obj_get_device, [8] = resource_map,
    [9] = resource_unmap, [10] = resource_get_desc, [11] = resource_get_va,
    [12] = resource_write_sub, [13] = resource_read_sub, [14] = resource_get_heap_props,
};

static uint32_t full_mip_count(uint64_t w, uint32_t h, uint32_t d)
{
    uint64_t m = w > h ? w : h;
    if (d > m) m = d;
    uint32_t n = 1;
    while (m > 1) { m >>= 1; ++n; }
    return n;
}

static void resource_parse(ComResource *r, const uint8_t *desc)
{
    memcpy(r->desc, desc, sizeof(r->desc));
    r->dimension = rd32(desc);
    r->width = rd64(desc + 16);
    r->height = rd32(desc + 24);
    uint16_t depth, mips;
    memcpy(&depth, desc + 28, 2);
    memcpy(&mips, desc + 30, 2);
    r->depth_or_layers = depth ? depth : 1;
    r->mips = mips;
    r->dxgi_format = rd32(desc + 32);
    r->flags = rd32(desc + 48);
    if (r->dimension != 1 && !r->mips)
        r->mips = full_mip_count(r->width, r->height,
                                 r->dimension == 4 ? r->depth_or_layers : 1);
    if (!r->height) r->height = 1;
}

static int image_desc_from(const ComResource *r, BD12ImageDesc *d)
{
    memset(d, 0, sizeof(*d));
    d->type = r->dimension == 2 ? VK_IMAGE_TYPE_1D :
              r->dimension == 4 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    d->allow_depth_stencil = (r->flags & 2) != 0;
    d->format = bd12_dxgi_resource_to_vk(r->dxgi_format, d->allow_depth_stencil);
    if (d->format == VK_FORMAT_UNDEFINED) return 0;
    d->width = (uint32_t)r->width;
    d->height = r->height;
    d->depth_or_layers = r->depth_or_layers;
    d->mip_levels = r->mips;
    uint32_t samples = rd32(r->desc + 36);
    d->samples = samples ? samples : 1;
    d->allow_render_target = (r->flags & 1) != 0;
    d->allow_uav = (r->flags & 4) != 0;
    d->cube_compatible = r->dimension == 3 && !(d->samples > 1) &&
                         r->depth_or_layers >= 6 && r->width == r->height;
    return 1;
}

static void res_finish_planes(ComResource *r)
{
    r->planes = 1;
    if (r->image && (r->image->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) &&
        (r->image->aspect & VK_IMAGE_ASPECT_STENCIL_BIT))
        r->planes = 2;
}

static HR create_resource(ComDevice *dev, ComHeap *heap, uint64_t heap_offset,
                          const uint8_t *props, uint32_t heap_flags,
                          const uint8_t *desc, uint32_t state,
                          const uint8_t *riid, void **out)
{
    if (!desc || !riid) return HR_E_INVALIDARG;
    if (out) *out = NULL;
    if (!com_iid_equal(riid, iid_resource) &&
        !com_iid_equal(riid, com_iid_iunknown))
        return HR_E_NOINTERFACE;
    ComResource *r = calloc(1, sizeof(*r));
    if (!r) return HR_E_OUTOFMEMORY;
    resource_parse(r, desc);
    if (r->dimension == 0 || !r->width) { free(r); return HR_E_INVALIDARG; }
    if (props) memcpy(r->props, props, sizeof(r->props));
    r->heap_flags = heap_flags;
    BD12HeapType type = heap ? heap->heap->type :
                        props ? heap_type_from_props(props) : BD12_HEAP_DEFAULT;
    if (r->dimension == 1) {
        if (heap) {
            r->buffer = bd12_buffer_create_placed(heap->heap, heap_offset,
                                                  r->width);
        } else {
            r->buffer = bd12_buffer_create_committed(dev->bd, type, r->width);
        }
        if (!r->buffer) goto fail;
    } else {
        BD12ImageDesc image_desc;
        if (getenv("BEER_BD12_RES_LOG"))
            fprintf(stderr, "[BD12 RES] image dim=%u fmt=%u %llux%u mips=%u layers=%u\n",
                    r->dimension, r->dxgi_format, (unsigned long long)r->width,
                    r->height, r->mips, r->depth_or_layers);
        if (!image_desc_from(r, &image_desc)) {
            fprintf(stderr, "[BD12] unsupported resource format %u\n",
                    r->dxgi_format);
            goto fail;
        }
        if (heap) {
            r->image = bd12_image_create_placed(heap->heap, heap_offset,
                                                &image_desc, state);
        } else {
            r->image = bd12_image_create_committed(dev->bd, &image_desc, state);
        }
        if (!r->image) goto fail;
        res_finish_planes(r);
    }
    if (heap) {
        r->heap = heap;
        com_add_ref(&heap->obj);
    }
    com_obj_init(&r->obj, COM_RESOURCE, resource_vtable, dev, resource_destroy);
    if (!out) {
        com_release(&r->obj);
        return HR_FALSE;
    }
    *out = r;
    return HR_OK;
fail:
    if (r->buffer) bd12_buffer_destroy(r->buffer);
    free(r);
    return HR_E_OUTOFMEMORY;
}

/* Descriptor heaps ----------------------------------------------------------- */

/* Recorded work reads the heap's mirror descriptor sets (and the pool they come from) until the GPU is done. */
static void desc_heap_free(ComDevice *dev, void *ptr)
{
    (void)dev;
    ComDescHeap *h = ptr;
    bd12_descriptor_heap_destroy(h->heap);
    free(h);
}
static void desc_heap_destroy(ComObj *o) { com_defer(o->dev, desc_heap_free, o); }
static HR MS desc_heap_query(ComObj *s, const uint8_t *r, void **o)
{ return com_obj_query(s, r, o, iid_descriptor_heap, NULL); }
static void *MS desc_heap_get_desc(ComDescHeap *h, void *result)
{
    memcpy(result, h->desc, sizeof(h->desc));
    return result;
}
static uint64_t *MS desc_heap_get_cpu(ComDescHeap *h, uint64_t *result)
{
    *result = (uint64_t)(uintptr_t)h->heap->descriptors;
    return result;
}
static uint64_t *MS desc_heap_get_gpu(ComDescHeap *h, uint64_t *result)
{
    *result = h->heap->gpu_base;
    return result;
}
static void *desc_heap_vtable[11] = {
    [0] = desc_heap_query, [1] = com_add_ref, [2] = com_release,
    [3] = com_obj_get_private, [4] = com_obj_set_private, [5] = com_obj_set_private_iface,
    [6] = com_obj_set_name, [7] = com_obj_get_device, [8] = desc_heap_get_desc,
    [9] = desc_heap_get_cpu, [10] = desc_heap_get_gpu,
};

/* Root signatures -------------------------------------------------------------- */

/* Lists that were recorded but not yet submitted (or still executing) refer to the signature's parameters. */
static void root_sig_free(ComDevice *dev, void *ptr)
{
    (void)dev;
    ComRootSig *r = ptr;
    bd12_root_signature_destroy(r->rs);
    free(r);
}
static void root_sig_destroy(ComObj *o) { com_defer(o->dev, root_sig_free, o); }
static HR MS root_sig_query(ComObj *s, const uint8_t *r, void **o)
{ return com_obj_query(s, r, o, iid_root_signature, NULL); }
static void *root_sig_vtable[8] = {
    [0] = root_sig_query, [1] = com_add_ref, [2] = com_release,
    [3] = com_obj_get_private, [4] = com_obj_set_private, [5] = com_obj_set_private_iface,
    [6] = com_obj_set_name, [7] = com_obj_get_device,
};

static VkSamplerCreateInfo sampler_from_desc(const uint8_t *d, int is_static);

/* Parses the BEERRSIG blob written by the compat serializer. */
static HR create_root_signature(ComDevice *dev, const uint8_t *blob,
                                size_t size, const uint8_t *riid, void **out)
{
    if (!blob || size < 24 || !riid || !out) return HR_E_INVALIDARG;
    *out = NULL;
    if (!com_iid_equal(riid, iid_root_signature) &&
        !com_iid_equal(riid, com_iid_iunknown))
        return HR_E_NOINTERFACE;
    if (memcmp(blob, "BEERRSIG", 8) != 0) return HR_E_INVALIDARG;
    uint32_t version = rd32(blob + 8), param_count = rd32(blob + 12);
    uint32_t sampler_count = rd32(blob + 16);
    if (param_count > BD12_MAX_ROOT_PARAMETERS) return HR_E_INVALIDARG;
    size_t range_size = version == 1 ? 20 : 24;
    BD12RootParameter params[BD12_MAX_ROOT_PARAMETERS];
    memset(params, 0, sizeof(params));
    BD12StaticSampler *statics = NULL;
    size_t pos = 24;
    HR hr = HR_E_INVALIDARG;
    for (uint32_t i = 0; i < param_count; ++i) {
        if (pos + 8 > size) goto done;
        uint32_t type = rd32(blob + pos), vis = rd32(blob + pos + 4);
        pos += 8;
        BD12RootParameter *p = &params[i];
        p->visibility = vis;
        if (type == 0) {
            if (pos + 4 > size) goto done;
            uint32_t n = rd32(blob + pos);
            pos += 4;
            if (n > 65536 || pos + (size_t)n * range_size > size) goto done;
            p->type = BD12_ROOT_TABLE;
            p->ranges = calloc(n ? n : 1, sizeof(BD12DescriptorRange));
            if (!p->ranges) { hr = HR_E_OUTOFMEMORY; goto done; }
            p->range_count = n;
            for (uint32_t k = 0; k < n; ++k) {
                const uint8_t *rg = blob + pos + (size_t)k * range_size;
                p->ranges[k] = (BD12DescriptorRange){
                    .type = rd32(rg), .count = rd32(rg + 4),
                    .base_register = rd32(rg + 8), .space = rd32(rg + 12),
                    .offset = version == 1 ? rd32(rg + 16) : rd32(rg + 20) };
            }
            pos += (size_t)n * range_size;
        } else if (type == 1) {
            if (pos + 12 > size) goto done;
            p->type = BD12_ROOT_CONSTANTS;
            p->shader_register = rd32(blob + pos);
            p->space = rd32(blob + pos + 4);
            p->dword_count = rd32(blob + pos + 8);
            pos += 12;
        } else if (type >= 2 && type <= 4) {
            size_t n = version == 1 ? 8 : 12;
            if (pos + n > size) goto done;
            p->type = type;  /* CBV=2, SRV=3, UAV=4 match BD12 */
            p->shader_register = rd32(blob + pos);
            p->space = rd32(blob + pos + 4);
            pos += n;
        } else {
            goto done;
        }
    }
    size_t sampler_size = version >= 3 ? 56 : 52;
    if (pos + (size_t)sampler_count * sampler_size > size) goto done;
    ComRootSig *r = calloc(1, sizeof(*r));
    if (!r) { hr = HR_E_OUTOFMEMORY; goto done; }
    r->rs = bd12_root_signature_create(dev->bd, params, param_count);
    if (!r->rs) { free(r); hr = HR_E_FAIL; goto done; }
    if (sampler_count) {
        statics = calloc(sampler_count, sizeof(*statics));
        if (!statics) { bd12_root_signature_destroy(r->rs); free(r);
                        hr = HR_E_OUTOFMEMORY; goto done; }
        for (uint32_t i = 0; i < sampler_count; ++i) {
            const uint8_t *s = blob + pos + (size_t)i * sampler_size;
            statics[i].reg = rd32(s + 40);
            statics[i].space = rd32(s + 44);
            statics[i].info = sampler_from_desc(s, 1);
        }
        bd12_root_signature_set_static_samplers(r->rs, statics, sampler_count);
    }
    com_obj_init(&r->obj, COM_ROOT_SIG, root_sig_vtable, dev, root_sig_destroy);
    *out = r;
    hr = HR_OK;
done:
    for (uint32_t i = 0; i < param_count; ++i) free(params[i].ranges);
    free(statics);
    return hr;
}

/* Command signatures ----------------------------------------------------------- */

static void signature_free(ComDevice *dev, void *ptr) { (void)dev; free(ptr); }
static void signature_destroy(ComObj *o) { com_defer(o->dev, signature_free, o); }
static HR MS signature_query(ComObj *s, const uint8_t *r, void **o)
{ return com_obj_query(s, r, o, iid_command_signature, NULL); }
static void *signature_vtable[8] = {
    [0] = signature_query, [1] = com_add_ref, [2] = com_release,
    [3] = com_obj_get_private, [4] = com_obj_set_private, [5] = com_obj_set_private_iface,
    [6] = com_obj_set_name, [7] = com_obj_get_device,
};

/* Query heaps (timestamps and occlusion are not modelled yet) -------------------- */

static void query_heap_free(ComDevice *dev, void *ptr)
{
    ComQueryHeap *h = ptr;
    if (h->pool)
        dev->bd->vk.DestroyQueryPool(dev->bd->device, h->pool, NULL);
    free(h->written);
    free(h);
}
/* The GPU may still be writing the pool: release it on the retire timeline. */
static void query_heap_destroy(ComObj *o) { com_defer(o->dev, query_heap_free, o); }
static HR MS query_heap_query(ComObj *s, const uint8_t *r, void **o)
{ return com_obj_query(s, r, o, iid_query_heap, NULL); }
static void *query_heap_vtable[8] = {
    [0] = query_heap_query, [1] = com_add_ref, [2] = com_release,
    [3] = com_obj_get_private, [4] = com_obj_set_private, [5] = com_obj_set_private_iface,
    [6] = com_obj_set_name, [7] = com_obj_get_device,
};

/* Samplers ------------------------------------------------------------------------ */

static VkSamplerAddressMode address_mode(uint32_t m)
{
    switch (m) {
    case 2: case 5: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case 3: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case 4: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    default: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
}

static const VkSamplerReductionModeCreateInfo k_reduction_min = {
    .sType = VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO,
    .reductionMode = VK_SAMPLER_REDUCTION_MODE_MIN
};
static const VkSamplerReductionModeCreateInfo k_reduction_max = {
    .sType = VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO,
    .reductionMode = VK_SAMPLER_REDUCTION_MODE_MAX
};

static VkSamplerCreateInfo sampler_from_desc(const uint8_t *d, int is_static)
{
    uint32_t filter = rd32(d);
    /* D3D12_FILTER bits 7-8 are the reduction type: 1 comparison, 2 minimum,
     * 3 maximum. Only the comparison type enables depth comparison. */
    const uint32_t reduction = (filter >> 7) & 3u;
    VkSamplerCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = (filter & 4) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        .minFilter = (filter & 16) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        .mipmapMode = (filter & 1) ? VK_SAMPLER_MIPMAP_MODE_LINEAR :
                                     VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = address_mode(rd32(d + 4)),
        .addressModeV = address_mode(rd32(d + 8)),
        .addressModeW = address_mode(rd32(d + 12)),
        .mipLodBias = rdf(d + 16),
        .anisotropyEnable = (filter & 0x40) != 0,
        .maxAnisotropy = (float)rd32(d + 20),
        .compareEnable = reduction == 1,
        .pNext = reduction == 2 ? &k_reduction_min :
                 reduction == 3 ? &k_reduction_max : NULL,
        .compareOp = (VkCompareOp)(rd32(d + 24) ? rd32(d + 24) - 1 : 0),
    };
    if (info.maxAnisotropy < 1.0f) info.maxAnisotropy = 1.0f;
    if (info.maxAnisotropy > 16.0f) info.maxAnisotropy = 16.0f;
    if (is_static) {
        uint32_t color = rd32(d + 28);
        info.minLod = rdf(d + 32);
        info.maxLod = rdf(d + 36);
        info.borderColor = color == 2 ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE :
                           color == 1 ? VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK :
                                        VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    } else {
        float c[4] = { rdf(d + 28), rdf(d + 32), rdf(d + 36), rdf(d + 40) };
        info.minLod = rdf(d + 44);
        info.maxLod = rdf(d + 48);
        info.borderColor = (c[0] > 0.5f) ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE :
                           (c[3] > 0.5f) ? VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK :
                                           VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    }
    if (info.maxLod > 1000.0f) info.maxLod = VK_LOD_CLAMP_NONE;
    return info;
}

/* Device --------------------------------------------------------------------------- */

#define DEVICE_FROM(self) ((ComDevice *)(self))

static HR MS dev_query(ComDevice *self, const uint8_t *riid, void **out)
{
    if (!out) return HR_E_INVALIDARG;
    *out = NULL;
    if (!riid || (!com_iid_equal(riid, com_iid_iunknown) &&
                  !com_iid_equal(riid, iid_device) &&
                  !com_iid_equal(riid, iid_device1) &&
                  !com_iid_equal(riid, iid_device2) &&
                  !com_iid_equal(riid, iid_device_downlevel) &&
                  !(self->bd->rt_enabled &&
                    (com_iid_equal(riid, iid_device3) ||
                     com_iid_equal(riid, iid_device4) ||
                     com_iid_equal(riid, iid_device5)))))
        return HR_E_NOINTERFACE;
    com_add_ref(&self->obj);
    *out = self;
    return HR_OK;
}
static uint32_t MS dev_add_ref(ComDevice *self)
{
    return __atomic_add_fetch(&self->obj.refs, 1, __ATOMIC_RELAXED);
}
static uint32_t MS dev_release(ComDevice *self)
{
    /* The device is a process-wide singleton and is never destroyed. */
    uint32_t cur = __atomic_load_n(&self->obj.refs, __ATOMIC_RELAXED);
    while (cur > 1 && !__atomic_compare_exchange_n(&self->obj.refs, &cur,
                                                   cur - 1, 0, __ATOMIC_RELAXED,
                                                   __ATOMIC_RELAXED)) {}
    return cur > 1 ? cur - 1 : 1;
}
static uint32_t MS dev_node_count(ComDevice *self) { (void)self; return 1; }

static HR MS dev_create_queue(ComDevice *self, const uint32_t *desc,
                              const uint8_t *riid, void **out)
{
    (void)riid;
    if (!out) return HR_E_INVALIDARG;
    *out = NULL;
    ComQueue *q = calloc(1, sizeof(*q));
    if (!q) return HR_E_OUTOFMEMORY;
    com_obj_init(&q->obj, COM_QUEUE, queue_vtable, self, queue_destroy);
    q->downlevel.vtable = downlevel_vtable;
    if (desc) memcpy(q->desc, desc, sizeof(q->desc));
    *out = q;
    return HR_OK;
}

static HR MS dev_create_allocator(ComDevice *self, uint32_t type,
                                  const uint8_t *riid, void **out)
{
    return com_create_allocator(self, type, riid, out);
}
static HR MS dev_create_graphics_pso(ComDevice *self, const void *desc,
                                     const uint8_t *riid, void **out)
{
    return com_create_pso(self, desc, 0, riid, out);
}
static HR MS dev_create_compute_pso(ComDevice *self, const void *desc,
                                    const uint8_t *riid, void **out)
{
    return com_create_pso(self, desc, 1, riid, out);
}
static HR MS dev_create_pso(ComDevice *self, const void *stream_desc,
                            const uint8_t *riid, void **out)
{
    return com_create_pso(self, stream_desc, 2, riid, out);
}
static HR MS dev_create_list(ComDevice *self, uint32_t node, uint32_t type,
                             ComObj *allocator, ComObj *pso,
                             const uint8_t *riid, void **out)
{
    (void)node;
    return com_create_list(self, type, allocator, pso, riid, out);
}

static HR MS dev_check_feature(ComDevice *self, uint32_t feature, void *data,
                               uint32_t size)
{
    (void)self;
    if (!data || !size) return HR_E_INVALIDARG;
    { static unsigned seen_mask; if (feature < 32 && !(seen_mask & (1u << feature))) { seen_mask |= 1u << feature; fprintf(stderr, "[BD12] CheckFeatureSupport feature=%u size=%u\n", feature, size); } }
    if (feature == 0 && size >= 60 && !getenv("BEER_DXR_FEATURES")) {
        memset(data, 0, size);
        return HR_OK;
    }
    if (feature == 1 && size >= 16) {
        uint32_t *fields = data;
        fields[3] = 0xb000;
        if (size >= 24) fields[4] = 0xb000;
        return HR_OK;
    }
    /* BEER_DXR_PROBE=1: advertise DXR 1.1 and shader model 6.6 and log what the
     * application then asks for. Experimental; the DXR pipeline is not built yet. */
    if (self->bd->rt_enabled && (getenv("BEER_DXR_PROBE") || getenv("BEER_DXR"))) {
        if (feature == 27 && size >= 12) {          /* OPTIONS5 */
            memset(data, 0, size);
            ((uint32_t *)data)[2] = 11;             /* RAYTRACING_TIER_1_1 */
            return HR_OK;
        }
        if (getenv("BEER_DXR_FEATURES")) {
            /* Report what the GPU really supports, so a game picks its full ray tracing paths. */
            uint32_t *f = data;
            if (feature == 0 && size >= 60) {              /* OPTIONS */
                memset(data, 0, size);
                f[0] = 1; f[2] = 0; f[3] = 3; f[4] = 3; f[5] = 1; f[6] = 1; f[7] = 1; f[8] = 3; f[9] = 40; f[14] = 2;
                return HR_OK;
            }
            if (feature == 8 && size >= 24) {              /* OPTIONS1: wave ops, 64-bit integers */
                memset(data, 0, size);
                f[0] = 1; f[1] = 32; f[2] = 32; f[3] = 0x2800; f[4] = 1; f[5] = 1;
                return HR_OK;
            }
            if (feature == 12 && size >= 4) { f[0] = 2; return HR_OK; }   /* ROOT_SIGNATURE: 1.1 */
            if (feature == 18 && size >= 8) { memset(data, 0, size); f[0] = 1; return HR_OK; }   /* OPTIONS2 */
            if (feature == 21 && size >= 20) {             /* OPTIONS3 */
                memset(data, 0, size);
                f[0] = 1; f[1] = 1; f[2] = 0xf; f[3] = 1; f[4] = 1;
                return HR_OK;
            }
        }
        if (feature == 7 && size >= 4 && getenv("BEER_DXR_PROBE_SM")) {
            *(uint32_t *)data = (uint32_t)strtoul(getenv("BEER_DXR_PROBE_SM"), NULL, 16);
            return HR_OK;                           /* SHADER_MODEL, e.g. 66 */
        }
    }
    if (feature == 8 && size >= 24) {
        /* OPTIONS1. Wave intrinsics are not translated, so WaveOps stays FALSE, but every device has a
         * native wave width and callers divide by it. Vulkan does not expose the processor-wide lane
         * total, so TotalLaneCount reports the one certain value, a single wave's maximum width. */
        uint32_t *f = data;
        memset(data, 0, size);
        f[1] = self->bd->wave_lane_min;
        f[2] = self->bd->wave_lane_max;
        f[3] = self->bd->wave_lane_max;
        return HR_OK;
    }
    switch (feature) {
    case 7: case 8: case 12: case 18: case 19:
    case 21: case 23: case 27: case 30: case 32:
        memset(data, 0, size);
        return HR_OK;
    default:
        /* Every other defined D3D12_FEATURE answers "not supported" (all-zero data) with success, as a driver
         * does; E_NOTIMPL is fatal to callers such as DirectStorage that probe EXISTING_HEAPS, SERIALIZATION,
         * CROSS_NODE, ... Values outside the enumeration stay invalid. */
        {
            static uint64_t logged;
            if (feature < 64 && !(logged & (1ull << feature))) {
                logged |= 1ull << feature;
                fprintf(stderr, "[BD12] CheckFeatureSupport feature=%u size=%u -> zeroed (unsupported)\n", feature, size);
            }
        }
        if (feature < 64) { memset(data, 0, size); return HR_OK; }
        return HR_E_NOTIMPL;
    }
}

static HR MS dev_create_descriptor_heap(ComDevice *self, const uint32_t *desc,
                                        const uint8_t *riid, void **out)
{
    if (!desc || !riid || !out) return HR_E_INVALIDARG;
    *out = NULL;
    if (!com_iid_equal(riid, iid_descriptor_heap) &&
        !com_iid_equal(riid, com_iid_iunknown))
        return HR_E_NOINTERFACE;
    uint32_t type = desc[0], count = desc[1];
    if (getenv("BEER_BD12_HEAP_LOG"))
        fprintf(stderr, "[BD12] descriptor heap type=%u count=%u flags=%u\n", type, count, desc[2]);
    if (type > 1 && type != 2 && type != 3) return HR_E_INVALIDARG;
    ComDescHeap *h = calloc(1, sizeof(*h));
    if (!h) return HR_E_OUTOFMEMORY;
    /* CBV_SRV_UAV=0, SAMPLER=1 share one descriptor layout; RTV/DSV heaps
     * reuse the same host array (type 0). */
    h->heap = bd12_descriptor_heap_create(self->bd, type == 1 ? 1 : 0,
                                          count ? count : 1);
    if (!h->heap) { free(h); return HR_E_OUTOFMEMORY; }
    /* Shader-visible CBV/SRV/UAV heaps get a bindless mirror up front when
     * shader model 6 is exposed (BEER_DXR_PROBE_SM), so every later write is
     * mirrored. */
    if (type == 0 && (desc[2] & 1) && count >= 1024 && getenv("BEER_DXR_PROBE_SM"))
        bd12_mirror_enable(h->heap);
    memcpy(h->desc, desc, sizeof(h->desc));
    com_obj_init(&h->obj, COM_DESC_HEAP, desc_heap_vtable, self, desc_heap_destroy);
    *out = h;
    return HR_OK;
}

static uint32_t MS dev_increment_size(ComDevice *self, uint32_t type)
{
    (void)self; (void)type;
    return BD12_DESCRIPTOR_INCREMENT;
}

static HR MS dev_create_root_signature(ComDevice *self, uint32_t node,
                                       const void *blob, size_t size,
                                       const uint8_t *riid, void **out)
{
    (void)node;
    return create_root_signature(self, blob, size, riid, out);
}

/* One-descriptor heap view so the descriptor writers can target a raw CPU
 * handle (which is a pointer into a heap's descriptor array). */
static BD12DescriptorHeap temp_heap(ComDevice *dev, uint64_t handle)
{
    BD12DescriptorHeap heap = { 0 };
    heap.device = dev->bd;
    heap.count = 1;
    heap.descriptors = (BD12Descriptor *)(uintptr_t)handle;
    return heap;
}

static void clear_descriptor(uint64_t handle)
{
    memset((void *)(uintptr_t)handle, 0, sizeof(BD12Descriptor));
}

static void MS dev_create_cbv(ComDevice *self, const uint8_t *desc,
                              uint64_t dest)
{
    if (!dest) return;
    clear_descriptor(dest);
    if (!desc) return;
    uint64_t va = rd64(desc);
    uint32_t size = rd32(desc + 8);
    VkDeviceSize offset = 0;
    BD12Buffer *buffer = va ? bd12_va_resolve(self->bd, va, &offset) : NULL;
    if (!buffer) return;
    BD12DescriptorHeap heap = temp_heap(self, dest);
    bd12_descriptor_write_cbv(&heap, 0, buffer, offset, size);
    bd12_mirror_note((const void *)(uintptr_t)dest, 1);
}

static VkFormat view_format(const ComResource *r, uint32_t dxgi)
{
    if (!dxgi) return r->image ? r->image->desc.format : VK_FORMAT_UNDEFINED;
    VkFormat f = bd12_dxgi_to_vk(dxgi);
    if (f == VK_FORMAT_UNDEFINED && r->image) return r->image->desc.format;
    return f;
}

static uint32_t clamp_mips(uint32_t count, uint32_t base, uint32_t total)
{
    if (base >= total) base = total - 1;
    return (count == UINT32_MAX || count > total - base) ? total - base : count;
}

static void write_buffer_view(ComDevice *self, ComResource *r,
                              uint64_t first, uint32_t count, uint32_t stride,
                              uint32_t dxgi, int raw, int uav, uint64_t dest)
{
    if (!r->buffer) return;
    BD12DescriptorHeap heap = temp_heap(self, dest);
    VkFormat format = VK_FORMAT_R32_UINT;
    uint32_t element = 4;
    if (dxgi && !raw) {
        VkFormat typed = bd12_dxgi_to_vk(dxgi);
        if (typed != VK_FORMAT_UNDEFINED) {
            format = typed;
            element = bd12_dxgi_bytes(dxgi);
        }
    } else if (stride) {
        element = stride;
    }
    if (!element) element = 4;
    VkDeviceSize offset = first * element;
    VkDeviceSize size = (VkDeviceSize)count * element;
    int ok = uav ?
        bd12_descriptor_write_buffer_uav(&heap, 0, r->buffer, offset, size, format) :
        bd12_descriptor_write_buffer_srv(&heap, 0, r->buffer, offset, size, format);
    if (!ok) {
        static int warned;
        if (warned++ < 8)
            fprintf(stderr, "[BD12] buffer %s view failed offset=%llu size=%llu "
                    "fmt=%d\n", uav ? "UAV" : "SRV", (unsigned long long)offset,
                    (unsigned long long)size, (int)format);
        clear_descriptor(dest);
        return;
    }
    heap.descriptors[0].offset = offset;
    heap.descriptors[0].range = size;
}

static void MS dev_create_srv_impl(ComDevice *self, ComResource *r,
                              const uint8_t *desc, uint64_t dest)
{
    if (!dest) return;
    clear_descriptor(dest);
    if (desc && !r && rd32(desc + 4) == 11) {          /* D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE */
        BD12Descriptor *d = (BD12Descriptor *)(uintptr_t)dest;
        d->kind = BD12_DESC_SRV_AS;
        d->offset = rd64(desc + 16);
        return;
    }
    if (!r) return;
    uint32_t dim = desc ? rd32(desc + 4) : 0;
    if (r->buffer) {
        if (!desc || dim != 1) return;
        write_buffer_view(self, r, rd64(desc + 16), rd32(desc + 24),
                          rd32(desc + 28), rd32(desc), (rd32(desc + 32) & 1) != 0,
                          0, dest);
        return;
    }
    if (!r->image) return;
    BD12DescriptorHeap heap = temp_heap(self, dest);
    VkFormat format = view_format(r, desc ? rd32(desc) : 0);
    uint32_t total_mips = r->mips, layers = r->depth_or_layers;
    VkImageViewType type;
    uint32_t mip = 0, mips = total_mips, first = 0, count = 1;
    if (!desc) {
        type = r->dimension == 4 ? VK_IMAGE_VIEW_TYPE_3D :
               r->dimension == 2 ? (layers > 1 ? VK_IMAGE_VIEW_TYPE_1D_ARRAY :
                                    VK_IMAGE_VIEW_TYPE_1D) :
               (layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D);
        count = r->dimension == 4 ? 1 : layers;
    } else {
        switch (dim) {
        case 2: type = VK_IMAGE_VIEW_TYPE_1D;
                mip = rd32(desc + 16); mips = rd32(desc + 20); break;
        case 3: type = VK_IMAGE_VIEW_TYPE_1D_ARRAY;
                mip = rd32(desc + 16); mips = rd32(desc + 20);
                first = rd32(desc + 24); count = rd32(desc + 28); break;
        case 4: type = VK_IMAGE_VIEW_TYPE_2D;
                mip = rd32(desc + 16); mips = rd32(desc + 20); break;
        case 5: type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
                mip = rd32(desc + 16); mips = rd32(desc + 20);
                first = rd32(desc + 24); count = rd32(desc + 28); break;
        case 6: type = VK_IMAGE_VIEW_TYPE_2D; mips = 1; break;
        case 7: type = VK_IMAGE_VIEW_TYPE_2D_ARRAY; mips = 1;
                first = rd32(desc + 16); count = rd32(desc + 20); break;
        case 8: type = VK_IMAGE_VIEW_TYPE_3D;
                mip = rd32(desc + 16); mips = rd32(desc + 20); break;
        case 9: type = VK_IMAGE_VIEW_TYPE_CUBE;
                mip = rd32(desc + 16); mips = rd32(desc + 20); count = 6; break;
        case 10: type = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
                mip = rd32(desc + 16); mips = rd32(desc + 20);
                /* First2DArrayFace is already a face index, NumCubes counts cubes. */
                first = rd32(desc + 24); count = rd32(desc + 28) * 6; break;
        default: return;
        }
    }
    mips = clamp_mips(mips, mip, total_mips);
    if (count == UINT32_MAX) count = layers - first;
    bd12_descriptor_write_image_srv(&heap, 0, r->image, type, format, mip, mips,
                                    first, count);
}

static void MS dev_create_uav_impl(ComDevice *self, ComResource *r, ComResource *counter,
                              const uint8_t *desc, uint64_t dest)
{
    (void)counter;
    if (!dest) return;
    clear_descriptor(dest);
    if (!r) return;
    uint32_t dim = desc ? rd32(desc + 4) : 0;
    if (r->buffer) {
        if (!desc || dim != 1) return;
        write_buffer_view(self, r, rd64(desc + 8), rd32(desc + 16),
                          rd32(desc + 20), rd32(desc), (rd32(desc + 32) & 1) != 0,
                          1, dest);
        return;
    }
    if (!r->image) return;
    BD12DescriptorHeap heap = temp_heap(self, dest);
    VkFormat format = view_format(r, desc ? rd32(desc) : 0);
    VkImageViewType type;
    uint32_t mip = 0, first = 0, count = 1;
    switch (dim) {
    case 2: type = VK_IMAGE_VIEW_TYPE_1D; mip = rd32(desc + 8); break;
    case 3: type = VK_IMAGE_VIEW_TYPE_1D_ARRAY; mip = rd32(desc + 8);
            first = rd32(desc + 12); count = rd32(desc + 16); break;
    case 4: type = VK_IMAGE_VIEW_TYPE_2D; mip = rd32(desc + 8); break;
    case 5: type = VK_IMAGE_VIEW_TYPE_2D_ARRAY; mip = rd32(desc + 8);
            first = rd32(desc + 12); count = rd32(desc + 16); break;
    case 8: type = VK_IMAGE_VIEW_TYPE_3D; mip = rd32(desc + 8); break;
    default:
        if (desc) return;
        type = r->dimension == 4 ? VK_IMAGE_VIEW_TYPE_3D :
               r->depth_or_layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY :
                                        VK_IMAGE_VIEW_TYPE_2D;
        count = r->dimension == 4 ? 1 : r->depth_or_layers;
        break;
    }
    if (count == UINT32_MAX) count = r->depth_or_layers - first;
    bd12_descriptor_write_image_uav(&heap, 0, r->image, type, format, mip, first,
                                    count);
}

/* Descriptor writes also refresh the bindless heap mirror, if one exists. */
static void MS dev_create_srv(ComDevice *self, ComResource *r,
                              const uint8_t *desc, uint64_t dest)
{
    dev_create_srv_impl(self, r, desc, dest);
    bd12_mirror_note((const void *)(uintptr_t)dest, 1);
}
static void MS dev_create_uav(ComDevice *self, ComResource *r, ComResource *counter,
                              const uint8_t *desc, uint64_t dest)
{
    dev_create_uav_impl(self, r, counter, desc, dest);
    bd12_mirror_note((const void *)(uintptr_t)dest, 1);
}

static void MS dev_create_rtv(ComDevice *self, ComResource *r,
                              const uint8_t *desc, uint64_t dest)
{
    if (!dest) return;
    clear_descriptor(dest);
    if (!r || !r->image) return;
    BD12DescriptorHeap heap = temp_heap(self, dest);
    VkFormat format = view_format(r, desc ? rd32(desc) : 0);
    uint32_t dim = desc ? rd32(desc + 4) : 0, mip = 0, first = 0, count = 1;
    switch (dim) {
    case 2: case 4: mip = rd32(desc + 8); break;
    case 3: case 5: mip = rd32(desc + 8); first = rd32(desc + 12);
            count = rd32(desc + 16); break;
    case 7: first = rd32(desc + 8); count = rd32(desc + 12); break;
    case 8: mip = rd32(desc + 8); first = rd32(desc + 12);
            count = rd32(desc + 16); break;
    case 6: break;
    default:
        count = r->dimension == 4 ? 1 : r->depth_or_layers;
        break;
    }
    if (count == UINT32_MAX)
        count = (r->dimension == 4 ? r->depth_or_layers : r->depth_or_layers) - first;
    bd12_descriptor_write_rtv(&heap, 0, r->image, format, mip, first, count);
}

static void MS dev_create_dsv(ComDevice *self, ComResource *r,
                              const uint8_t *desc, uint64_t dest)
{
    if (!dest) return;
    clear_descriptor(dest);
    if (!r || !r->image) return;
    BD12DescriptorHeap heap = temp_heap(self, dest);
    VkFormat format = view_format(r, desc ? rd32(desc) : 0);
    if (format != r->image->desc.format) format = r->image->desc.format;
    uint32_t dim = desc ? rd32(desc + 4) : 0, flags = desc ? rd32(desc + 8) : 0;
    uint32_t mip = 0, first = 0, count = r->depth_or_layers;
    switch (dim) {
    case 1: case 3: mip = rd32(desc + 12); count = 1; break;
    case 2: case 4: mip = rd32(desc + 12); first = rd32(desc + 16);
            count = rd32(desc + 20); break;
    case 5: count = 1; break;
    case 6: first = rd32(desc + 12); count = rd32(desc + 16); break;
    default: break;
    }
    if (count == UINT32_MAX) count = r->depth_or_layers - first;
    bd12_descriptor_write_dsv(&heap, 0, r->image, format, mip, first, count,
                              (flags & 1) != 0, (flags & 2) != 0);
}

static void MS dev_create_sampler(ComDevice *self, const uint8_t *desc,
                                  uint64_t dest)
{
    if (!dest) return;
    clear_descriptor(dest);
    if (!desc) return;
    VkSamplerCreateInfo info = sampler_from_desc(desc, 0);
    BD12DescriptorHeap heap = temp_heap(self, dest);
    bd12_descriptor_write_sampler(&heap, 0, &info);
}

static void MS dev_copy_descriptors_simple(ComDevice *self, uint32_t count,
                                           uint64_t dest, uint64_t src,
                                           uint32_t type)
{
    (void)self; (void)type;
    if (!count || !dest || !src) return;
    memmove((void *)(uintptr_t)dest, (const void *)(uintptr_t)src,
            (size_t)count * sizeof(BD12Descriptor));
    bd12_mirror_note((const void *)(uintptr_t)dest, count);
}

static void MS dev_copy_descriptors(ComDevice *self, uint32_t dest_ranges,
                                    const uint64_t *dest_starts,
                                    const uint32_t *dest_sizes,
                                    uint32_t src_ranges,
                                    const uint64_t *src_starts,
                                    const uint32_t *src_sizes, uint32_t type)
{
    (void)self; (void)type;
    if (!dest_ranges || !src_ranges || !dest_starts || !src_starts) return;
    uint32_t dr = 0, sr = 0, doff = 0, soff = 0;
    while (dr < dest_ranges && sr < src_ranges) {
        uint32_t dcount = dest_sizes ? dest_sizes[dr] : 1;
        uint32_t scount = src_sizes ? src_sizes[sr] : 1;
        if (doff >= dcount) { ++dr; doff = 0; continue; }
        if (soff >= scount) { ++sr; soff = 0; continue; }
        uint32_t n = dcount - doff;
        if (n > scount - soff) n = scount - soff;
        memmove((BD12Descriptor *)(uintptr_t)dest_starts[dr] + doff,
                (const BD12Descriptor *)(uintptr_t)src_starts[sr] + soff,
                (size_t)n * sizeof(BD12Descriptor));
        bd12_mirror_note((BD12Descriptor *)(uintptr_t)dest_starts[dr] + doff, n);
        doff += n;
        soff += n;
    }
}

/* Allocation info and footprints ---------------------------------------------------- */

static uint64_t align_up(uint64_t v, uint64_t a)
{
    return a ? (v + a - 1) / a * a : v;
}

static uint64_t texture_size_estimate(ComDevice *dev, const uint8_t *desc,
                                      uint64_t *alignment)
{
    ComResource tmp;
    memset(&tmp, 0, sizeof(tmp));
    resource_parse(&tmp, desc);
    *alignment = 65536;
    BD12ImageDesc image_desc;
    VkDeviceSize size = 0, vk_align = 0;
    if (!image_desc_from(&tmp, &image_desc) ||
        !bd12_image_requirements(dev->bd, &image_desc, &size, &vk_align))
        return 0;
    if (vk_align > *alignment) *alignment = vk_align;
    return size;
}

static void *MS dev_get_allocation_info(ComDevice *self, uint64_t *result,
                                        uint32_t mask, uint32_t count,
                                        const uint8_t *descs)
{
    (void)mask;
    uint64_t total = 0, max_align = 65536;
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t *d = descs + (size_t)i * 56;
        uint64_t align = 65536, size;
        if (rd32(d) == 1) {
            size = rd64(d + 16);
        } else {
            size = texture_size_estimate(self, d, &align);
            if (!size) size = 65536;
        }
        uint64_t req = rd64(d + 8);
        if (req > align) align = req;
        if (align > max_align) max_align = align;
        total = align_up(total, align) + align_up(size, align);
    }
    result[0] = align_up(total, max_align);
    result[1] = max_align;
    return result;
}

static void *MS dev_get_custom_heap_props(ComDevice *self, uint32_t *result,
                                          uint32_t node, uint32_t type)
{
    (void)self; (void)node;
    result[0] = 4;
    result[1] = type == 2 ? 2 : type == 3 ? 3 : 1;
    result[2] = type == 1 ? 2 : 1;
    result[3] = 1;
    result[4] = 1;
    return result;
}

static HR MS dev_create_committed(ComDevice *self, const uint8_t *props,
                                  uint32_t flags, const uint8_t *desc,
                                  uint32_t state, const void *clear,
                                  const uint8_t *riid, void **out)
{
    (void)clear;
    return create_resource(self, NULL, 0, props, flags, desc, state, riid, out);
}
static HR MS dev_create_heap(ComDevice *self, const uint8_t *desc,
                             const uint8_t *riid, void **out)
{
    return create_heap(self, desc, riid, out);
}
static HR MS dev_create_placed(ComDevice *self, ComHeap *heap, uint64_t offset,
                               const uint8_t *desc, uint32_t state,
                               const void *clear, const uint8_t *riid,
                               void **out)
{
    (void)clear;
    if (!heap) return HR_E_INVALIDARG;
    return create_resource(self, heap, offset, heap->desc + 8, 0, desc, state,
                           riid, out);
}

static HR MS dev_create_fence(ComDevice *self, uint64_t initial, uint32_t flags,
                              const uint8_t *riid, void **out)
{
    (void)flags;
    if (!out) return HR_E_INVALIDARG;
    *out = NULL;
    if (!self || self->obj.kind != COM_DEVICE || !self->bd) {
        fprintf(stderr, "[BD12] ID3D12Device::CreateFence on a non-BD12 object %p (kind=%d vtable=%p)\n",
                (void *)self, self ? (int)self->obj.kind : -1, self ? (void *)self->obj.vtable : NULL);
        return HR_E_INVALIDARG;
    }
    if (!com_iid_equal(riid, iid_fence) && !com_iid_equal(riid, com_iid_iunknown)) {
        fprintf(stderr, "[BD12] CreateFence: unsupported IID %08x-%04x-%04x\n",
                *(const uint32_t *)riid, *(const uint16_t *)(riid + 4), *(const uint16_t *)(riid + 6));
        return HR_E_NOINTERFACE;
    }
    ComFence *f = calloc(1, sizeof(*f));
    if (!f) return HR_E_OUTOFMEMORY;
    f->fence = bd12_fence_create(self->bd, initial);
    if (!f->fence) { free(f); return HR_E_FAIL; }
    com_obj_init(&f->obj, COM_FENCE, fence_vtable, self, fence_destroy);
    *out = f;
    if (fence_diag())
        fprintf(stderr, "[FENCE] create fence=%p initial=%llu\n", (void *)f, (unsigned long long)initial);
    return HR_OK;
}

static HR MS dev_removed_reason(ComDevice *self)
{
    (void)self;
    return beer_vulkan_device_lost() ? HR_DEVICE_REMOVED : HR_OK;
}

/* Subresource footprints (placed copies between buffers and textures). */
static void MS dev_get_footprints(ComDevice *self, const uint8_t *desc,
                                  uint32_t first, uint32_t count,
                                  uint64_t base, uint8_t *layouts,
                                  uint32_t *rows, uint64_t *row_sizes,
                                  uint64_t *total)
{
    (void)self;
    ComResource r;
    memset(&r, 0, sizeof(r));
    resource_parse(&r, desc);
    uint64_t cursor = base, sum = 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t sub = first + i;
        uint32_t fmt = r.dimension == 1 ? 0 : r.dxgi_format;
        uint32_t width, height = 1, depth = 1, nrows = 1;
        uint64_t row_size, pitch;
        if (r.dimension == 1) {
            width = (uint32_t)r.width;
            row_size = r.width;
            pitch = r.width;
        } else {
            uint32_t mip = r.mips ? sub % r.mips : 0;
            uint32_t bytes = bd12_dxgi_bytes(fmt);
            int block = bd12_dxgi_is_compressed(fmt);
            uint64_t w = r.width >> mip;
            if (!w) w = 1;
            height = r.height >> mip;
            if (!height) height = 1;
            depth = r.dimension == 4 ? r.depth_or_layers >> mip : 1;
            if (!depth) depth = 1;
            width = (uint32_t)w;
            nrows = block ? (height + 3) / 4 : height;
            row_size = block ? (w + 3) / 4 * bytes : w * bytes;
            pitch = align_up(row_size, 256);
        }
        cursor = align_up(cursor, 512);
        if (layouts) {
            uint8_t *l = layouts + (size_t)i * 32;
            memset(l, 0, 32);
            memcpy(l, &cursor, 8);
            memcpy(l + 8, &fmt, 4);
            memcpy(l + 12, &width, 4);
            memcpy(l + 16, &height, 4);
            memcpy(l + 20, &depth, 4);
            uint32_t p32 = (uint32_t)pitch;
            memcpy(l + 24, &p32, 4);
        }
        if (rows) rows[i] = nrows;
        if (row_sizes) row_sizes[i] = row_size;
        uint64_t bytes = pitch * nrows * depth;
        if (r.dimension != 1) {
            /* The last row of the last slice needs no padding. */
            bytes = pitch * ((uint64_t)nrows * depth - 1) + row_size;
        }
        sum = cursor + bytes - base;
        cursor += bytes;
    }
    if (total) *total = sum;
}

static HR MS dev_create_query_heap(ComDevice *self, const uint32_t *desc,
                                   const uint8_t *riid, void **out)
{
    if (!out || !desc) return HR_E_INVALIDARG;
    *out = NULL;
    if (!com_iid_equal(riid, iid_query_heap) &&
        !com_iid_equal(riid, com_iid_iunknown))
        return HR_E_NOINTERFACE;
    ComQueryHeap *h = calloc(1, sizeof(*h));
    if (!h) return HR_E_OUTOFMEMORY;
    h->heap_type = desc[0];
    h->count = desc[1];
    h->written = calloc(h->count ? h->count : 1, 1);
    /* Occlusion and timestamp heaps are real Vulkan pools; pipeline and
     * stream-output statistics keep the zero-filled behaviour. */
    if (h->written && h->count && (h->heap_type == 0 || h->heap_type == 1)) {
        VkQueryPoolCreateInfo info = {
            .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
            .queryType = h->heap_type == 0 ? VK_QUERY_TYPE_OCCLUSION
                                           : VK_QUERY_TYPE_TIMESTAMP,
            .queryCount = h->count
        };
        if (self->bd->vk.CreateQueryPool(self->bd->device, &info, NULL,
                                         &h->pool) == VK_SUCCESS)
            self->bd->vk.ResetQueryPool(self->bd->device, h->pool, 0, h->count);
        else
            h->pool = VK_NULL_HANDLE;
    }
    if (!h->written) { free(h); return HR_E_OUTOFMEMORY; }
    com_obj_init(&h->obj, COM_QUERY_HEAP, query_heap_vtable, self, query_heap_destroy);
    *out = h;
    return HR_OK;
}

static HR MS dev_create_command_signature(ComDevice *self, const uint8_t *desc,
                                          ComObj *root_sig, const uint8_t *riid,
                                          void **out)
{
    (void)root_sig;
    if (!desc || !out) return HR_E_INVALIDARG;
    *out = NULL;
    if (!com_iid_equal(riid, iid_command_signature) &&
        !com_iid_equal(riid, com_iid_iunknown))
        return HR_E_NOINTERFACE;
    uint32_t stride = rd32(desc), n = rd32(desc + 4);
    const uint8_t *args = NULL;
    memcpy(&args, desc + 8, sizeof(args));
    if (!n || !args || n > 1024) return HR_E_INVALIDARG;
    ComSignature *s = calloc(1, sizeof(*s));
    if (!s) return HR_E_OUTOFMEMORY;
    s->stride = stride;
    s->arg_count = n;
    for (uint32_t i = 0; i < n && i < 8; ++i) s->arg_types[i] = rd32(args + i * 16);
    fprintf(stderr, "[BD12] CommandSignature stride=%u args=%u first=%u\n",
            stride, n, s->arg_types[0]);
    com_obj_init(&s->obj, COM_SIGNATURE, signature_vtable, self, signature_destroy);
    *out = s;
    return HR_OK;
}

static void MS dev_get_resource_tiling(void) {}
static uint64_t *MS dev_get_luid(ComDevice *self, uint64_t *result)
{
    (void)self;
    *result = 0;
    return result;
}

/* BEER_DXR_DUMP=<dir>: writes every state object as text (subobjects, exports, hit groups, associations) and each
 * distinct DXIL library as lib_<hash>.dxil, so translation can be studied and tested offline. */
static void dxr_ascii(const uint8_t *wide, char *out, size_t size)
{
    size_t i = 0;
    for (; wide && i + 1 < size; ++i) {
        uint16_t c;
        memcpy(&c, wide + i * 2, 2);
        if (!c) break;
        out[i] = c < 128 ? (char)c : '?';
    }
    out[i] = 0;
}

static void dxr_dump_state_object(const uint8_t *desc)
{
    const char *dir = getenv("BEER_DXR_DUMP");
    static unsigned counter;
    if (!dir || !*dir || counter > 400) return;
    const uint32_t count = rd32(desc + 4);
    const uint8_t *subs = (const uint8_t *)(uintptr_t)rd64(desc + 8);
    char path[600];
    snprintf(path, sizeof(path), "%s/so%04u.txt", dir, counter++);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "state object type=%u subobjects=%u\n", rd32(desc), count);
    for (uint32_t i = 0; subs && i < count && i < 64; ++i) {
        const uint32_t st = rd32(subs + i * 16);
        const uint8_t *sd = (const uint8_t *)(uintptr_t)rd64(subs + i * 16 + 8);
        fprintf(f, "[%u] type=%u at %p", i, st, (const void *)(subs + i * 16));
        char a[160], b[160], c[160], d[160];
        if (st == 5 && sd) {
            const uint8_t *code = (const uint8_t *)(uintptr_t)rd64(sd);
            const uint64_t bytes = rd64(sd + 8);
            uint64_t h = 1469598103934665603ull;
            for (uint64_t k = 0; code && k < bytes; ++k) { h ^= code[k]; h *= 1099511628211ull; }
            fprintf(f, " DXIL_LIBRARY bytes=%llu hash=%016llx exports=%u\n", (unsigned long long)bytes, (unsigned long long)h, rd32(sd + 16));
            const uint8_t *exports = (const uint8_t *)(uintptr_t)rd64(sd + 24);
            for (uint32_t k = 0; exports && k < rd32(sd + 16) && k < 64; ++k) {
                dxr_ascii((const uint8_t *)(uintptr_t)rd64(exports + k * 24), a, sizeof(a));
                dxr_ascii((const uint8_t *)(uintptr_t)rd64(exports + k * 24 + 8), b, sizeof(b));
                fprintf(f, "      export \"%s\" rename-from \"%s\"\n", a, b);
            }
            snprintf(path, sizeof(path), "%s/lib_%016llx.dxil", dir, (unsigned long long)h);
            if (code && access(path, F_OK) != 0) {
                FILE *lib = fopen(path, "wb");
                if (lib) { fwrite(code, 1, (size_t)bytes, lib); fclose(lib); }
            }
        } else if (st == 11 && sd) {
            dxr_ascii((const uint8_t *)(uintptr_t)rd64(sd), a, sizeof(a));
            dxr_ascii((const uint8_t *)(uintptr_t)rd64(sd + 16), b, sizeof(b));
            dxr_ascii((const uint8_t *)(uintptr_t)rd64(sd + 24), c, sizeof(c));
            dxr_ascii((const uint8_t *)(uintptr_t)rd64(sd + 32), d, sizeof(d));
            fprintf(f, " HIT_GROUP \"%s\" type=%u any=\"%s\" closest=\"%s\" intersection=\"%s\"\n", a, rd32(sd + 8), b, c, d);
        } else if (st == 9 && sd) {
            fprintf(f, " SHADER_CONFIG payload=%u attributes=%u\n", rd32(sd), rd32(sd + 4));
        } else if (st == 10 && sd) {
            fprintf(f, " PIPELINE_CONFIG max recursion=%u\n", rd32(sd));
        } else if ((st == 1 || st == 2) && sd) {
            ComRootSig *crs = (ComRootSig *)(uintptr_t)rd64(sd);
            fprintf(f, " %s_ROOT_SIGNATURE object=%p\n", st == 1 ? "GLOBAL" : "LOCAL", (void *)crs);
            const BD12RootSignature *rs = crs ? crs->rs : NULL;
            for (uint32_t k = 0; rs && k < rs->parameter_count; ++k) {
                const BD12RootParameter *rp = &rs->parameters[k];
                static const char *const type_names[] = { "TABLE", "CONSTANTS", "CBV", "SRV", "UAV" };
                fprintf(f, "      param %u %s vis=%u reg=%u space=%u dwords=%u ranges=%u", k, rp->type < 5 ? type_names[rp->type] : "?",
                        rp->visibility, rp->shader_register, rp->space, rp->dword_count, rp->range_count);
                for (uint32_t r = 0; r < rp->range_count; ++r)
                    fprintf(f, " [type%u n=%d base=%u space=%u off=%d]", rp->ranges[r].type, (int)rp->ranges[r].count,
                            rp->ranges[r].base_register, rp->ranges[r].space, (int)rp->ranges[r].offset);
                fprintf(f, "\n");
            }
            for (uint32_t k = 0; rs && k < rs->static_count; ++k)
                fprintf(f, "      static sampler reg=%u space=%u\n", rs->statics[k].reg, rs->statics[k].space);
        } else if (st == 7 && sd) {
            const uint8_t *target = (const uint8_t *)(uintptr_t)rd64(sd);
            fprintf(f, " ASSOCIATION to subobject at %p (type %u) exports:", (const void *)target, target ? rd32(target) : 0);
            const uint8_t *names = (const uint8_t *)(uintptr_t)rd64(sd + 16);
            for (uint32_t k = 0; names && k < rd32(sd + 8) && k < 64; ++k) {
                dxr_ascii((const uint8_t *)(uintptr_t)rd64(names + k * 8), a, sizeof(a));
                fprintf(f, " \"%s\"", a);
            }
            fprintf(f, "\n");
        } else fprintf(f, "\n");
    }
    fclose(f);
}

/* ID3D12StateObject / ID3D12StateObjectProperties ---------------------------------- */
static const uint8_t iid_state_object[16] = IID(0x43, 0x69, 0x01, 0x47, 0xa8, 0xfc, 0x94, 0x45,
    0x93, 0xea, 0xaf, 0x25, 0x8b, 0x55, 0x34, 0x6d);
static const uint8_t iid_state_props[16] = IID(0x27, 0xa8, 0x5f, 0xde, 0xf9, 0x9b, 0x26, 0x4f,
    0x89, 0xff, 0xd7, 0xf5, 0x6f, 0xde, 0x38, 0x60);

static void so_free(ComDevice *dev, void *ptr)
{
    (void)dev;
    ComStateObject *so = ptr;
    bd12_rt_pipeline_destroy(so->rt);
    if (so->global_rs) com_release(so->global_rs);
    free(so);
}
static void so_destroy(ComObj *o) { com_defer(o->dev, so_free, o); }

static HR MS so_query(ComObj *s, const uint8_t *r, void **o)
{
    ComStateObject *so = (ComStateObject *)s;
    if (r && com_iid_equal(r, iid_state_props)) {
        if (!o) return HR_E_INVALIDARG;
        com_add_ref(s);
        *o = &so->props_vtable;
        return HR_OK;
    }
    return com_obj_query(s, r, o, iid_state_object, NULL);
}
#define SO_FROM_PROPS(p) ((ComStateObject *)((uint8_t *)(p) - offsetof(ComStateObject, props_vtable)))
static HR MS sop_query(void **p, const uint8_t *r, void **o)
{
    ComStateObject *so = SO_FROM_PROPS(p);
    if (r && com_iid_equal(r, iid_state_props)) {
        if (!o) return HR_E_INVALIDARG;
        com_add_ref(&so->obj);
        *o = p;
        return HR_OK;
    }
    return so_query(&so->obj, r, o);
}
static uint32_t MS sop_add_ref(void **p) { return com_add_ref(&SO_FROM_PROPS(p)->obj); }
static uint32_t MS sop_release(void **p) { return com_release(&SO_FROM_PROPS(p)->obj); }
static const void *MS sop_identifier(void **p, const uint16_t *name)
{
    char ascii[128];
    dxr_ascii((const uint8_t *)name, ascii, sizeof(ascii));
    return bd12_rt_pipeline_identifier(SO_FROM_PROPS(p)->rt, ascii);
}
static uint64_t MS sop_shader_stack(void **p, const uint16_t *name) { (void)p; (void)name; return 2048; }
static uint64_t MS sop_pipeline_stack(void **p) { (void)p; return 8192; }
static void MS sop_set_pipeline_stack(void **p, uint64_t size) { (void)p; (void)size; }
static void *so_props_vtable[7] = {
    [0] = sop_query, [1] = sop_add_ref, [2] = sop_release, [3] = sop_identifier,
    [4] = sop_shader_stack, [5] = sop_pipeline_stack, [6] = sop_set_pipeline_stack,
};
static void *so_vtable[7] = {
    [0] = so_query, [1] = com_add_ref, [2] = com_release,
    [3] = com_obj_get_private, [4] = com_obj_set_private, [5] = com_obj_set_private_iface,
    [6] = com_obj_set_name,
};
/* GetDevice is slot 7; the state object has no methods of its own beyond ID3D12Object. */
static void *so_vtable8[8];

static HR MS dev_create_state_object(ComDevice *self, const uint8_t *desc,
                                     const uint8_t *riid, void **out)
{
    if (out) *out = NULL;
    if (!desc || !out) return HR_E_INVALIDARG;
    if (!self->bd->rt_enabled) return HR_E_NOTIMPL;
    dxr_dump_state_object(desc);
    const uint32_t count = rd32(desc + 4);
    const uint8_t *subs = (const uint8_t *)(uintptr_t)rd64(desc + 8);
    BD12RtDesc d;
    memset(&d, 0, sizeof(d));
    d.max_recursion = 1;
    ComObj *global_rs = NULL;
    d.libraries = calloc(count ? count : 1, sizeof(*d.libraries));
    d.hit_groups = calloc(count ? count : 1, sizeof(*d.hit_groups));
    if (!d.libraries || !d.hit_groups) goto fail;
    for (uint32_t i = 0; subs && i < count; ++i) {
        const uint32_t st = rd32(subs + i * 16);
        const uint8_t *sd = (const uint8_t *)(uintptr_t)rd64(subs + i * 16 + 8);
        if (!sd) continue;
        if (st == 5) {
            const uint8_t *code = (const uint8_t *)(uintptr_t)rd64(sd);
            const uint64_t bytes = rd64(sd + 8);
            BD12RtLibrary *lib = &d.libraries[d.library_count];
            lib->code = code ? bd12_rt_intern_blob(code, (size_t)bytes) : NULL;
            lib->size = (size_t)bytes;
            if (!lib->code) goto fail;
            const uint32_t n = rd32(sd + 16);
            const uint8_t *exports = (const uint8_t *)(uintptr_t)rd64(sd + 24);
            if (n && exports) {
                lib->exports = calloc(n, sizeof(*lib->exports));
                if (!lib->exports) goto fail;
                lib->export_count = n;
                for (uint32_t k = 0; k < n; ++k) {
                    dxr_ascii((const uint8_t *)(uintptr_t)rd64(exports + k * 24), lib->exports[k].name, 128);
                    dxr_ascii((const uint8_t *)(uintptr_t)rd64(exports + k * 24 + 8), lib->exports[k].rename_from, 128);
                }
            }
            d.library_count++;
        } else if (st == 11) {
            BD12RtHitGroup *hg = &d.hit_groups[d.hit_group_count++];
            dxr_ascii((const uint8_t *)(uintptr_t)rd64(sd), hg->name, 128);
            hg->type = rd32(sd + 8);
            dxr_ascii((const uint8_t *)(uintptr_t)rd64(sd + 16), hg->any_hit, 128);
            dxr_ascii((const uint8_t *)(uintptr_t)rd64(sd + 24), hg->closest_hit, 128);
            dxr_ascii((const uint8_t *)(uintptr_t)rd64(sd + 32), hg->intersection, 128);
        } else if (st == 9) {
            d.max_payload = rd32(sd);
            d.max_attributes = rd32(sd + 4);
        } else if (st == 10) {
            d.max_recursion = rd32(sd);
        } else if (st == 1) {
            ComRootSig *crs = (ComRootSig *)(uintptr_t)rd64(sd);
            if (crs) { global_rs = &crs->obj; d.global_root_signature = crs->rs; }
        } else if (st == 2) {
            ComRootSig *crs = (ComRootSig *)(uintptr_t)rd64(sd);
            if (crs && crs->rs && crs->rs->parameter_count)
                fprintf(stderr, "[BD12 RT] local root signature with %u parameters is not supported yet\n", crs->rs->parameter_count);
        }
    }
    if (!d.library_count) goto fail;
    ComStateObject *so = calloc(1, sizeof(*so));
    if (!so) goto fail;
    so->rt = bd12_rt_pipeline_create(self->bd, &d);
    if (!so->rt) { free(so); goto fail; }
    if (global_rs) { com_add_ref(global_rs); so->global_rs = global_rs; }
    if (!so_vtable8[0]) {
        memcpy(so_vtable8, so_vtable, sizeof(so_vtable));
        so_vtable8[7] = (void *)com_obj_get_device;
    }
    so->props_vtable = so_props_vtable;
    com_obj_init(&so->obj, COM_PSO, so_vtable8, self, so_destroy);
    if (riid && !com_iid_equal(riid, iid_state_object) && !com_iid_equal(riid, iid_state_props) &&
        !com_iid_equal(riid, com_iid_iunknown)) {
        com_release(&so->obj);
        return HR_E_NOINTERFACE;
    }
    HR hr = so_query(&so->obj, riid ? riid : iid_state_object, out);
    com_release(&so->obj);
    {   static unsigned created;
        if (++created % 500 == 0 || created == 1) fprintf(stderr, "[DXR] CreateStateObject: %u created so far\n", created);
    }
    return hr;
fail:
    {   static unsigned failed;
        if (failed++ < 10) fprintf(stderr, "[DXR] CreateStateObject FAILED (#%u): libraries=%u hit_groups=%u subobjects=%u\n", failed, d.library_count, d.hit_group_count, count);
    }
    for (uint32_t i = 0; i < d.library_count; ++i) free(d.libraries[i].exports);
    free(d.libraries);
    free(d.hit_groups);
    return HR_E_FAIL;
}
static HR MS dev_create_committed1(ComDevice *self, const uint8_t *props,
                                   uint32_t flags, const uint8_t *desc,
                                   uint32_t state, const void *clear,
                                   const void *session, const uint8_t *riid,
                                   void **out)
{
    (void)session;
    return dev_create_committed(self, props, flags, desc, state, clear, riid, out);
}
static HR MS dev_create_heap1(ComDevice *self, const uint8_t *desc,
                              const void *session, const uint8_t *riid,
                              void **out)
{
    (void)session;
    return dev_create_heap(self, desc, riid, out);
}
/* ID3D12Device5::GetRaytracingAccelerationStructurePrebuildInfo */
static void MS dev_rt_prebuild(ComDevice *self, const void *inputs, uint64_t *info)
{
    uint64_t sizes[3] = { 0, 0, 0 };
    bd12_rt_prebuild(self->bd, inputs, sizes);
    memcpy(info, sizes, sizeof(sizes));
}
static uint32_t MS dev_check_matching_identifier(void) { return 0; /* compatible */ }
static void MS dev_remove_device(void) {}

/* ID3D12Device4::CreateCommandList1(NodeMask, Type, Flags, riid, ppCommandList): a list that starts CLOSED. It is
 * built over a private allocator (the list keeps it alive) and closed straight away; the caller Reset()s it with
 * its own allocator before recording. */
static HR MS dev_create_list1(ComDevice *self, uint32_t node, uint32_t type, uint32_t flags,
                              const uint8_t *riid, void **out)
{
    (void)node; (void)flags;
    if (!out) return HR_E_INVALIDARG;
    *out = NULL;
    void *allocator = NULL;
    HR hr = com_create_allocator(self, type, com_iid_iunknown, &allocator);
    if (hr != HR_OK || !allocator) return hr ? hr : HR_E_FAIL;
    void *list = NULL;
    hr = com_create_list(self, type, allocator, NULL, riid, &list);
    com_release(allocator);                        /* the list holds its own reference */
    if (hr != HR_OK || !list) return hr ? hr : HR_E_FAIL;
    void **vtable = *(void ***)list;
    ((HR (MS *)(void *))vtable[9])(list);          /* ID3D12GraphicsCommandList::Close */
    *out = list;
    return HR_OK;
}

/* ID3D12Device5 meta commands: none are exposed (vendor GPU decompression etc.). */
static HR MS dev_enumerate_meta_commands(ComDevice *self, uint32_t *count, void *descs)
{
    (void)self; (void)descs;
    if (count) *count = 0;
    return HR_OK;
}
static HR MS dev_meta_command_invalid(void) { return HR_E_INVALIDARG; }

static void *device_vtable[65] = {
    [0] = dev_query, [1] = dev_add_ref, [2] = dev_release,
    [3] = com_obj_get_private, [4] = com_obj_set_private, [5] = com_obj_set_private_iface,
    [6] = com_obj_set_name, [7] = dev_node_count, [8] = dev_create_queue,
    [9] = dev_create_allocator, [10] = dev_create_graphics_pso,
    [11] = dev_create_compute_pso, [12] = dev_create_list,
    [13] = dev_check_feature, [14] = dev_create_descriptor_heap,
    [15] = dev_increment_size, [16] = dev_create_root_signature,
    [17] = dev_create_cbv, [18] = dev_create_srv, [19] = dev_create_uav,
    [20] = dev_create_rtv, [21] = dev_create_dsv, [22] = dev_create_sampler,
    [23] = dev_copy_descriptors, [24] = dev_copy_descriptors_simple,
    [25] = dev_get_allocation_info, [26] = dev_get_custom_heap_props,
    [27] = dev_create_committed, [28] = dev_create_heap,
    [29] = dev_create_placed, [30] = obj_notimpl, [31] = obj_notimpl,
    [32] = obj_notimpl, [33] = obj_notimpl, [34] = obj_ok, [35] = obj_ok,
    [36] = dev_create_fence, [37] = dev_removed_reason,
    [38] = dev_get_footprints, [39] = dev_create_query_heap, [40] = obj_ok,
    [41] = dev_create_command_signature, [42] = dev_get_resource_tiling,
    [43] = dev_get_luid, [44] = obj_notimpl, [45] = obj_notimpl,
    [46] = obj_ok, [47] = dev_create_pso,
    /* Device3 */ [48] = obj_notimpl, [49] = obj_notimpl, [50] = obj_ok,
    /* Device4 */ [51] = dev_create_list1, [52] = obj_notimpl,
    [53] = dev_create_committed1, [54] = dev_create_heap1,
    [55] = obj_notimpl, [56] = obj_notimpl,
    /* Device5 */ [57] = obj_notimpl, [58] = dev_remove_device,
    [59] = dev_enumerate_meta_commands, [60] = dev_meta_command_invalid, [61] = dev_meta_command_invalid,
    [62] = dev_create_state_object, [63] = dev_rt_prebuild,
    [64] = dev_check_matching_identifier,
};


/* DXGI swap chain (IDXGISwapChain1-4) over BD12 textures ---------------------------------------------------- */
static const uint8_t iid_dxgi_object[16] = IID(0xb8, 0x2f, 0xc2, 0xae, 0xf3, 0x76, 0x39, 0x46, 0x9b, 0xe0, 0x28, 0xeb, 0x43, 0xa6, 0x7a, 0x2e);
static const uint8_t iid_dxgi_subobject[16] = IID(0x79, 0x03, 0x3e, 0x3d, 0xde, 0xf9, 0x58, 0x4d, 0xbb, 0x6c, 0x18, 0xd6, 0x29, 0x92, 0xf1, 0xa6);
static const uint8_t iid_swapchain[16] = IID(0xa0, 0x36, 0x0d, 0x31, 0xe7, 0xd2, 0x0a, 0x4c, 0xaa, 0x04, 0x6a, 0x9d, 0x23, 0xb8, 0x88, 0x6a);
static const uint8_t iid_swapchain1[16] = IID(0xf7, 0x45, 0x0a, 0x79, 0x42, 0x0d, 0x76, 0x48, 0x98, 0x3a, 0x0a, 0x55, 0xcf, 0xe6, 0xf4, 0xaa);
static const uint8_t iid_swapchain2[16] = IID(0xc4, 0x2a, 0xbe, 0xa8, 0x9f, 0x19, 0x46, 0x49, 0xb3, 0x31, 0x79, 0x59, 0x9f, 0xb9, 0x8d, 0xe7);
static const uint8_t iid_swapchain3[16] = IID(0xdb, 0x9b, 0xd9, 0x94, 0xf8, 0xf1, 0xb0, 0x4a, 0xb2, 0x36, 0x7d, 0xa0, 0x17, 0x0e, 0xda, 0xb1);
static const uint8_t iid_swapchain4[16] = IID(0x5a, 0x5d, 0x58, 0x3d, 0x4a, 0xbd, 0x9e, 0x48, 0xb1, 0xf4, 0x3d, 0xbc, 0xb6, 0x45, 0x2f, 0xfb);

#define COM_SWAPCHAIN_KIND (COM_KIND_COUNT + 1)
#define SC_MAX_BUFFERS 16
#define HR_DXGI_INVALID_CALL UINT64_C(0x887a0001)

typedef struct ComSwapChain {
    ComObj obj;
    ComQueue *queue;
    uint64_t hwnd;
    uint32_t width, height, format, buffer_count, flags, usage, swap_effect, windowed;
    uint32_t sample_count, sample_quality;
    ComResource *buffers[SC_MAX_BUFFERS];
    uint32_t index, present_count, max_latency, fullscreen, source_w, source_h;
    uint64_t waitable;
} ComSwapChain;

static uint64_t (*g_event_factory)(void);
static void *g_dxgi_factory_object;
void bd12_com_set_event_factory(uint64_t (*fn)(void)) { g_event_factory = fn; }
void bd12_com_set_dxgi_factory(void *factory) { g_dxgi_factory_object = factory; }

static void swapchain_release_buffers(ComSwapChain *sc)
{
    for (uint32_t i = 0; i < SC_MAX_BUFFERS; ++i)
        if (sc->buffers[i]) { com_release(&sc->buffers[i]->obj); sc->buffers[i] = NULL; }
}

static HR swapchain_create_buffers(ComSwapChain *sc)
{
    ComDevice *dev = sc->queue->obj.dev;
    for (uint32_t i = 0; i < sc->buffer_count; ++i) {
        uint8_t props[20] = {0}, desc[56] = {0};
        uint32_t heap_default = 1, one = 1, flags = 1;                    /* ALLOW_RENDER_TARGET */
        if (sc->usage & 0x80) flags |= 4;                                 /* DXGI_USAGE_UNORDERED_ACCESS */
        uint32_t dimension = 3;                                           /* TEXTURE2D */
        uint64_t width = sc->width;
        uint16_t depth = 1, mips = 1;
        memcpy(props, &heap_default, 4);
        memcpy(desc, &dimension, 4);
        memcpy(desc + 16, &width, 8);
        memcpy(desc + 24, &sc->height, 4);
        memcpy(desc + 28, &depth, 2);
        memcpy(desc + 30, &mips, 2);
        memcpy(desc + 32, &sc->format, 4);
        memcpy(desc + 36, &one, 4);
        memcpy(desc + 48, &flags, 4);
        void *out = NULL;
        HR hr = create_resource(dev, NULL, 0, props, 0, desc, 0 /* COMMON == PRESENT */, iid_resource, &out);
        if (hr != HR_OK || !out) {
            swapchain_release_buffers(sc);
            return hr ? hr : HR_E_FAIL;
        }
        sc->buffers[i] = out;
    }
    return HR_OK;
}

static void swapchain_destroy(ComObj *o)
{
    ComSwapChain *sc = (ComSwapChain *)o;
    swapchain_release_buffers(sc);
    if (sc->queue) com_release(&sc->queue->obj);
    free(sc);
}

static HR MS sc_query(ComSwapChain *sc, const uint8_t *riid, void **out)
{
    if (!out) return HR_E_INVALIDARG;
    *out = NULL;
    if (!riid) return HR_E_NOINTERFACE;
    if (com_iid_equal(riid, com_iid_iunknown) || com_iid_equal(riid, iid_dxgi_object) ||
        com_iid_equal(riid, iid_dxgi_subobject) || com_iid_equal(riid, iid_swapchain) ||
        com_iid_equal(riid, iid_swapchain1) || com_iid_equal(riid, iid_swapchain2) ||
        com_iid_equal(riid, iid_swapchain3) || com_iid_equal(riid, iid_swapchain4)) {
        com_add_ref(&sc->obj);
        *out = sc;
        return HR_OK;
    }
    if (getenv("BEER_COM_DIAGNOSTICS"))
        fprintf(stderr, "[BD12 QI] swapchain refused %02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x\n",
                riid[3], riid[2], riid[1], riid[0], riid[5], riid[4], riid[7], riid[6], riid[8], riid[9],
                riid[10], riid[11], riid[12], riid[13], riid[14], riid[15]);
    return HR_E_NOINTERFACE;
}
static uint32_t MS sc_add_ref(ComSwapChain *sc) { return com_add_ref(&sc->obj); }
static uint32_t MS sc_release(ComSwapChain *sc) { return com_release(&sc->obj); }
static HR MS sc_set_private(ComSwapChain *sc, const uint8_t *g, uint32_t n, const void *d) { (void)sc; (void)g; (void)n; (void)d; return HR_OK; }
static HR MS sc_set_private_iface(ComSwapChain *sc, const uint8_t *g, void *i) { (void)sc; (void)g; (void)i; return HR_OK; }
static HR MS sc_get_private(ComSwapChain *sc, const uint8_t *g, uint32_t *n, void *d) { (void)sc; (void)g; (void)d; if (n) *n = 0; return HR_NOT_FOUND; }
static HR MS sc_get_parent(ComSwapChain *sc, const uint8_t *riid, void **out)
{
    (void)sc; (void)riid;
    if (!out) return HR_E_INVALIDARG;
    *out = NULL;
    if (!g_dxgi_factory_object) return HR_E_NOINTERFACE;
    *out = g_dxgi_factory_object;           /* the loader's factory is process-lifetime */
    return HR_OK;
}
static HR MS sc_get_device(ComSwapChain *sc, const uint8_t *riid, void **out)
{
    if (!out) return HR_E_INVALIDARG;
    if (com_iid_equal(riid, iid_queue)) return com_obj_query(&sc->queue->obj, riid, out, iid_queue, NULL);
    return com_obj_query(&sc->queue->obj.dev->obj, riid, out, iid_device, iid_device1);
}

static HR MS sc_present(ComSwapChain *sc, uint32_t sync, uint32_t flags)
{
    (void)sync; (void)flags;
    if (fence_diag() && (sc->present_count < 5 || sc->present_count % 300 == 0))
        fprintf(stderr, "[FENCE] swapchain %p Present #%u sync=%u flags=0x%x presenter=%p\n", (void *)sc,
                sc->present_count, sync, flags, (void *)g_presenter);
    /* DXGI_PRESENT_TEST only asks whether presenting would succeed (occlusion probe): it neither shows the
     * back buffer nor advances the buffer index. Treating it as a real present doubles every frame. */
    if (flags & 0x1u) return HR_OK;
    ComResource *texture = sc->buffers[sc->index % (sc->buffer_count ? sc->buffer_count : 1)];
    HR hr = texture ? present_epilogue(sc->queue, texture) : HR_E_FAIL;
    sc->index = (sc->index + 1) % (sc->buffer_count ? sc->buffer_count : 1);
    ++sc->present_count;
    return hr == HR_E_NOTIMPL ? HR_OK : hr;     /* no presenter attached: nothing to show, not an error */
}
static HR MS sc_present1(ComSwapChain *sc, uint32_t sync, uint32_t flags, const void *params)
{
    (void)params;
    return sc_present(sc, sync, flags);
}
static HR MS sc_get_buffer(ComSwapChain *sc, uint32_t index, const uint8_t *riid, void **out)
{
    if (!out) return HR_E_INVALIDARG;
    *out = NULL;
    if (index >= sc->buffer_count || !sc->buffers[index]) return HR_DXGI_INVALID_CALL;
    return com_obj_query(&sc->buffers[index]->obj, riid, out, iid_resource, NULL);
}
static HR MS sc_set_fullscreen(ComSwapChain *sc, int32_t fullscreen, void *target)
{
    (void)target;
    sc->fullscreen = fullscreen != 0;
    return HR_OK;
}
static HR MS sc_get_fullscreen(ComSwapChain *sc, int32_t *fullscreen, void **target)
{
    if (fullscreen) *fullscreen = (int32_t)sc->fullscreen;
    if (target) *target = NULL;
    return HR_OK;
}
static HR MS sc_get_desc(ComSwapChain *sc, uint8_t *d)
{
    if (!d) return HR_E_INVALIDARG;
    memset(d, 0, 72);
    uint32_t one = 1, zero = 0, sixty = 60;
    memcpy(d + 0, &sc->width, 4); memcpy(d + 4, &sc->height, 4);
    memcpy(d + 8, &sixty, 4); memcpy(d + 12, &one, 4);
    memcpy(d + 16, &sc->format, 4); memcpy(d + 20, &zero, 4); memcpy(d + 24, &zero, 4);
    memcpy(d + 28, &sc->sample_count, 4); memcpy(d + 32, &sc->sample_quality, 4);
    memcpy(d + 36, &sc->usage, 4); memcpy(d + 40, &sc->buffer_count, 4);
    memcpy(d + 48, &sc->hwnd, 8);
    uint32_t windowed = sc->fullscreen ? 0 : 1;
    memcpy(d + 56, &windowed, 4); memcpy(d + 60, &sc->swap_effect, 4); memcpy(d + 64, &sc->flags, 4);
    return HR_OK;
}
static HR swapchain_resize(ComSwapChain *sc, uint32_t count, uint32_t w, uint32_t h, uint32_t format,
                           uint32_t flags)
{
    uint32_t old_count = sc->buffer_count, old_w = sc->width, old_h = sc->height, old_fmt = sc->format;
    if (count) sc->buffer_count = count > SC_MAX_BUFFERS ? SC_MAX_BUFFERS : count;
    if (w) sc->width = w;
    if (h) sc->height = h;
    if (format) sc->format = format;
    sc->flags = flags;
    swapchain_release_buffers(sc);
    HR hr = swapchain_create_buffers(sc);
    if (hr != HR_OK) {
        sc->buffer_count = old_count; sc->width = old_w; sc->height = old_h; sc->format = old_fmt;
        swapchain_create_buffers(sc);
    }
    sc->index = 0;
    return hr;
}
static HR MS sc_resize_buffers(ComSwapChain *sc, uint32_t count, uint32_t w, uint32_t h, uint32_t format,
                               uint32_t flags)
{
    return swapchain_resize(sc, count, w, h, format, flags);
}
static HR MS sc_resize_target(ComSwapChain *sc, const void *mode) { (void)sc; (void)mode; return HR_OK; }
static HR MS sc_get_containing_output(ComSwapChain *sc, void **out)
{
    (void)sc;
    if (out) *out = NULL;
    return HR_NOT_FOUND;
}
static HR MS sc_get_frame_statistics(ComSwapChain *sc, void *stats) { (void)sc; (void)stats; return HR_DXGI_INVALID_CALL; }
static HR MS sc_get_last_present_count(ComSwapChain *sc, uint32_t *count)
{
    if (!count) return HR_E_INVALIDARG;
    *count = sc->present_count;
    return HR_OK;
}
static HR MS sc_get_desc1(ComSwapChain *sc, uint8_t *d)
{
    if (!d) return HR_E_INVALIDARG;
    memset(d, 0, 48);
    memcpy(d + 0, &sc->width, 4); memcpy(d + 4, &sc->height, 4); memcpy(d + 8, &sc->format, 4);
    memcpy(d + 16, &sc->sample_count, 4); memcpy(d + 20, &sc->sample_quality, 4);
    memcpy(d + 24, &sc->usage, 4); memcpy(d + 28, &sc->buffer_count, 4);
    memcpy(d + 36, &sc->swap_effect, 4);   /* AlphaMode (40) stays DXGI_ALPHA_MODE_UNSPECIFIED */
    memcpy(d + 44, &sc->flags, 4);
    return HR_OK;
}
static HR MS sc_get_fullscreen_desc(ComSwapChain *sc, uint8_t *d)
{
    if (!d) return HR_E_INVALIDARG;
    memset(d, 0, 20);
    uint32_t sixty = 60, one = 1, windowed = sc->fullscreen ? 0 : 1;
    memcpy(d + 0, &sixty, 4); memcpy(d + 4, &one, 4); memcpy(d + 16, &windowed, 4);
    return HR_OK;
}
static HR MS sc_get_hwnd(ComSwapChain *sc, uint64_t *hwnd)
{
    if (!hwnd) return HR_E_INVALIDARG;
    *hwnd = sc->hwnd;
    return HR_OK;
}
static HR MS sc_get_core_window(ComSwapChain *sc, const uint8_t *riid, void **out)
{
    (void)sc; (void)riid;
    if (out) *out = NULL;
    return HR_DXGI_INVALID_CALL;
}
static uint32_t MS sc_is_temp_mono(ComSwapChain *sc) { (void)sc; return 0; }
static HR MS sc_get_restrict(ComSwapChain *sc, void **out) { (void)sc; if (out) *out = NULL; return HR_OK; }
static HR MS sc_set_bg(ComSwapChain *sc, const void *color) { (void)sc; (void)color; return HR_OK; }
static HR MS sc_get_bg(ComSwapChain *sc, float *color)
{
    (void)sc;
    if (color) color[0] = color[1] = color[2] = color[3] = 0.0f;
    return HR_OK;
}
static HR MS sc_set_rotation(ComSwapChain *sc, uint32_t r) { (void)sc; (void)r; return HR_OK; }
static HR MS sc_get_rotation(ComSwapChain *sc, uint32_t *r) { (void)sc; if (r) *r = 1; return HR_OK; }
static HR MS sc_set_source_size(ComSwapChain *sc, uint32_t w, uint32_t h) { sc->source_w = w; sc->source_h = h; return HR_OK; }
static HR MS sc_get_source_size(ComSwapChain *sc, uint32_t *w, uint32_t *h)
{
    if (w) *w = sc->source_w ? sc->source_w : sc->width;
    if (h) *h = sc->source_h ? sc->source_h : sc->height;
    return HR_OK;
}
static HR MS sc_set_max_latency(ComSwapChain *sc, uint32_t n)
{
    if (!n || n > 16) return HR_E_INVALIDARG;
    sc->max_latency = n;
    return HR_OK;
}
static HR MS sc_get_max_latency(ComSwapChain *sc, uint32_t *n)
{
    if (!n) return HR_E_INVALIDARG;
    *n = sc->max_latency;
    return HR_OK;
}
static uint64_t MS sc_get_waitable(ComSwapChain *sc) { return sc->waitable; }
static HR MS sc_set_matrix(ComSwapChain *sc, const void *m) { (void)sc; (void)m; return HR_DXGI_INVALID_CALL; }
static HR MS sc_get_matrix(ComSwapChain *sc, void *m) { (void)sc; (void)m; return HR_DXGI_INVALID_CALL; }
static uint32_t MS sc_get_current_index(ComSwapChain *sc) { return sc->index; }
static HR MS sc_check_color_space(ComSwapChain *sc, uint32_t space, uint32_t *support)
{
    (void)sc;
    if (!support) return HR_E_INVALIDARG;
    *support = (space == 0 || space == 1) ? 1u : 0u;   /* sRGB / scRGB: DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT */
    return HR_OK;
}
static HR MS sc_set_color_space(ComSwapChain *sc, uint32_t space) { (void)sc; return space <= 1 ? HR_OK : HR_E_INVALIDARG; }
static HR MS sc_resize_buffers1(ComSwapChain *sc, uint32_t count, uint32_t w, uint32_t h, uint32_t format,
                                uint32_t flags, const uint32_t *masks, void *const *queues)
{
    (void)masks; (void)queues;
    return swapchain_resize(sc, count, w, h, format, flags);
}
static HR MS sc_set_hdr(ComSwapChain *sc, uint32_t type, uint32_t size, const void *data)
{
    (void)sc; (void)type; (void)size; (void)data;
    return HR_OK;
}

static void *swapchain_vtable[41] = {
    [0] = sc_query, [1] = sc_add_ref, [2] = sc_release,
    [3] = sc_set_private, [4] = sc_set_private_iface, [5] = sc_get_private, [6] = sc_get_parent,
    [7] = sc_get_device, [8] = sc_present, [9] = sc_get_buffer, [10] = sc_set_fullscreen,
    [11] = sc_get_fullscreen, [12] = sc_get_desc, [13] = sc_resize_buffers, [14] = sc_resize_target,
    [15] = sc_get_containing_output, [16] = sc_get_frame_statistics, [17] = sc_get_last_present_count,
    [18] = sc_get_desc1, [19] = sc_get_fullscreen_desc, [20] = sc_get_hwnd, [21] = sc_get_core_window,
    [22] = sc_present1, [23] = sc_is_temp_mono, [24] = sc_get_restrict, [25] = sc_set_bg,
    [26] = sc_get_bg, [27] = sc_set_rotation, [28] = sc_get_rotation,
    [29] = sc_set_source_size, [30] = sc_get_source_size, [31] = sc_set_max_latency,
    [32] = sc_get_max_latency, [33] = sc_get_waitable, [34] = sc_set_matrix, [35] = sc_get_matrix,
    [36] = sc_get_current_index, [37] = sc_check_color_space, [38] = sc_set_color_space,
    [39] = sc_resize_buffers1, [40] = sc_set_hdr,
};

int bd12_com_is_queue(const void *object)
{
    return object && *(void *const *)object == (void *)queue_vtable;
}

/* desc: DXGI_SWAP_CHAIN_DESC (72 bytes). */
uint64_t bd12_com_create_swapchain(void *queue_object, const uint8_t *desc, void **out)
{
    if (!out || !desc || !bd12_com_is_queue(queue_object)) return HR_E_INVALIDARG;
    *out = NULL;
    ComQueue *queue = queue_object;
    ComSwapChain *sc = calloc(1, sizeof(*sc));
    if (!sc) return HR_E_OUTOFMEMORY;
    memcpy(&sc->width, desc + 0, 4);
    memcpy(&sc->height, desc + 4, 4);
    memcpy(&sc->format, desc + 16, 4);
    memcpy(&sc->sample_count, desc + 28, 4);
    memcpy(&sc->sample_quality, desc + 32, 4);
    memcpy(&sc->usage, desc + 36, 4);
    memcpy(&sc->buffer_count, desc + 40, 4);
    memcpy(&sc->hwnd, desc + 48, 8);
    memcpy(&sc->windowed, desc + 56, 4);
    memcpy(&sc->swap_effect, desc + 60, 4);
    memcpy(&sc->flags, desc + 64, 4);
    if (!sc->buffer_count) sc->buffer_count = 2;
    if (sc->buffer_count > SC_MAX_BUFFERS) sc->buffer_count = SC_MAX_BUFFERS;
    if (!sc->width || !sc->height || !sc->format) { free(sc); return HR_E_INVALIDARG; }
    sc->max_latency = 1;
    sc->fullscreen = !sc->windowed;
    com_add_ref(&queue->obj);
    sc->queue = queue;
    com_obj_init(&sc->obj, COM_SWAPCHAIN_KIND, swapchain_vtable, queue->obj.dev, swapchain_destroy);
    HR hr = swapchain_create_buffers(sc);
    if (hr != HR_OK) { swapchain_destroy(&sc->obj); return hr; }
    if ((sc->flags & 0x40 /* FRAME_LATENCY_WAITABLE_OBJECT */) && g_event_factory) sc->waitable = g_event_factory();
    fprintf(stderr, "[BD12] swapchain %ux%u format=%u buffers=%u hwnd=%llx flags=0x%x waitable=%llx\n", sc->width,
            sc->height, sc->format, sc->buffer_count, (unsigned long long)sc->hwnd, sc->flags,
            (unsigned long long)sc->waitable);
    *out = sc;
    return HR_OK;
}

uint64_t MS beer_bd12_create_device(const uint8_t *riid, void **out)
{
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    if (!out) return HR_E_INVALIDARG;
    *out = NULL;
    if (!com_iid_equal(riid, iid_device) && !com_iid_equal(riid, com_iid_iunknown))
        return HR_E_NOINTERFACE;
    pthread_mutex_lock(&lock);
    if (!g_com_device) {
        ComDevice *dev = calloc(1, sizeof(*dev));
        if (dev) {
            dev->bd = bd12_device_create();
            if (dev->bd) {
                dev->queue = bd12_queue_create(dev->bd);
                dev->retire = bd12_fence_create(dev->bd, 0);
            }
            if (!dev->bd || !dev->queue || !dev->retire) {
                fprintf(stderr, "[BD12] device creation failed\n");
                free(dev);
                dev = NULL;
            } else {
                pthread_mutex_init(&dev->submit_lock, NULL);
                pthread_mutex_init(&dev->grave_lock, NULL);
                pthread_mutex_init(&dev->wait_lock, NULL);
                pthread_cond_init(&dev->wait_cond, NULL);
                com_obj_init(&dev->obj, COM_DEVICE, device_vtable, dev, NULL);
                g_com_device = dev;
                fprintf(stderr, "[BD12] device ready (%s)\n", dev->bd->device_name);
            }
        }
    }
    pthread_mutex_unlock(&lock);
    if (!g_com_device) return HR_E_FAIL;
    com_add_ref(&g_com_device->obj);
    *out = g_com_device;
    return HR_OK;
}
