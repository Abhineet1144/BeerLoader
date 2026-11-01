#include "win32_native_heap.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

typedef struct NativeAllocation {
    void *ptr;
    void *raw;
    size_t size;
    struct NativeAllocation *next;
} NativeAllocation;

typedef struct NativeHeap {
    unsigned char header[BEER_NATIVE_HEAP_HEADER_SIZE];
    void *raw;
    uint32_t flags;
    size_t reserve;
    size_t used;
    NativeAllocation *allocations;
    struct NativeHeap *next;
} NativeHeap;

_Static_assert(offsetof(NativeHeap, header) == 0, "handle must address header");

static pthread_mutex_t registry_mutex = PTHREAD_MUTEX_INITIALIZER;
static NativeHeap *registry;

/* Own the original malloc pointer separately; no assumptions about malloc's
 * alignment and no rounded-down fallback on overflow/allocation failure. */
static void *allocate_aligned(size_t size, void **raw)
{
    size_t actual = size ? size : 1;
    unsigned char *memory;
    size_t offset;

    if (actual > SIZE_MAX - 15)
        return NULL;
    memory = malloc(actual + 15);
    if (!memory)
        return NULL;
    offset = (16 - ((uintptr_t)memory & 15)) & 15;
    *raw = memory;
    return memory + offset;
}

/* Must hold registry_mutex. Comparisons only: never inspect a foreign handle. */
static NativeHeap *find_heap(uintptr_t handle)
{
    NativeHeap *heap;
    for (heap = registry; heap; heap = heap->next)
        if ((uintptr_t)heap == handle)
            return heap;
    return NULL;
}

static NativeAllocation **find_allocation(NativeHeap *heap, const void *ptr)
{
    NativeAllocation **entry = &heap->allocations;
    while (*entry && (*entry)->ptr != ptr)
        entry = &(*entry)->next;
    return entry;
}

static int fits(NativeHeap *heap, size_t old_size, size_t new_size)
{
    size_t retained = heap->used - old_size;
    if (new_size > SIZE_MAX - retained)
        return 0;
    return (heap->flags & BEER_HEAP_GROWABLE) ||
           (retained <= heap->reserve && new_size <= heap->reserve - retained);
}

uintptr_t beer_native_heap_create(uint32_t flags, void *base, size_t reserve,
                                  size_t commit, void *lock, void *parameters)
{
    const uint32_t allowed = BEER_HEAP_NO_SERIALIZE | BEER_HEAP_GROWABLE |
                             BEER_HEAP_ZERO_MEMORY;
    NativeHeap *heap;
    void *raw;
    uint32_t signature = UINT32_C(0xffeeffee);

    if ((flags & ~allowed) || base || lock || parameters)
        return 0;
    if (!reserve)
        flags |= BEER_HEAP_GROWABLE;
    if (!(flags & BEER_HEAP_GROWABLE) && commit > reserve)
        return 0;
    heap = allocate_aligned(sizeof(*heap), &raw);
    if (!heap)
        return 0;
    memset(heap, 0, sizeof(*heap));
    heap->raw = raw;
    heap->flags = flags;
    heap->reserve = reserve;
    memcpy(heap->header + 0x10, &signature, sizeof(signature));
    memcpy(heap->header + 0x70, &flags, sizeof(flags));
    pthread_mutex_lock(&registry_mutex);
    heap->next = registry;
    registry = heap;
    pthread_mutex_unlock(&registry_mutex);
    return (uintptr_t)heap;
}

uint8_t beer_native_heap_contains(uintptr_t handle)
{
    uint8_t result;
    pthread_mutex_lock(&registry_mutex);
    result = find_heap(handle) != NULL;
    pthread_mutex_unlock(&registry_mutex);
    return result;
}

void *beer_native_heap_alloc(uintptr_t handle, uint32_t flags, size_t size)
{
    NativeHeap *heap;
    NativeAllocation *allocation;
    void *result = NULL;

    if (flags & ~(BEER_HEAP_NO_SERIALIZE | BEER_HEAP_ZERO_MEMORY))
        return NULL;
    pthread_mutex_lock(&registry_mutex);
    heap = find_heap(handle);
    if (!heap || !fits(heap, 0, size))
        goto done;
    allocation = malloc(sizeof(*allocation));
    if (!allocation)
        goto done;
    allocation->ptr = allocate_aligned(size, &allocation->raw);
    if (!allocation->ptr) {
        free(allocation);
        goto done;
    }
    allocation->size = size;
    if ((flags | heap->flags) & BEER_HEAP_ZERO_MEMORY)
        memset(allocation->ptr, 0, size ? size : 1);
    allocation->next = heap->allocations;
    heap->allocations = allocation;
    heap->used += size;
    result = allocation->ptr;
done:
    pthread_mutex_unlock(&registry_mutex);
    return result;
}

void *beer_native_heap_reallocate(uintptr_t handle, uint32_t flags, void *ptr,
                                  size_t size)
{
    NativeHeap *heap;
    NativeAllocation *allocation;
    void *result = NULL, *raw;
    size_t old_size;

    if (!ptr || (flags & ~(BEER_HEAP_NO_SERIALIZE | BEER_HEAP_ZERO_MEMORY |
                          BEER_HEAP_REALLOC_IN_PLACE_ONLY)))
        return NULL;
    pthread_mutex_lock(&registry_mutex);
    heap = find_heap(handle);
    if (!heap)
        goto done;
    allocation = *find_allocation(heap, ptr);
    if (!allocation || !fits(heap, allocation->size, size))
        goto done;
    old_size = allocation->size;
    if (size <= old_size) {
        result = ptr;
    } else {
        if (flags & BEER_HEAP_REALLOC_IN_PLACE_ONLY)
            goto done;
        result = allocate_aligned(size, &raw);
        if (!result)
            goto done;
        memcpy(result, ptr, old_size);
        if ((flags | heap->flags) & BEER_HEAP_ZERO_MEMORY)
            memset((unsigned char *)result + old_size, 0, size - old_size);
        free(allocation->raw);
        allocation->raw = raw;
        allocation->ptr = result;
    }
    allocation->size = size;
    heap->used = heap->used - old_size + size;
done:
    pthread_mutex_unlock(&registry_mutex);
    return result;
}

uint8_t beer_native_heap_free(uintptr_t handle, uint32_t flags, void *ptr)
{
    NativeHeap *heap;
    NativeAllocation **entry, *allocation;
    uint8_t result = 0;

    if (!ptr || (flags & ~BEER_HEAP_NO_SERIALIZE))
        return 0;
    pthread_mutex_lock(&registry_mutex);
    heap = find_heap(handle);
    if (!heap)
        goto done;
    entry = find_allocation(heap, ptr);
    allocation = *entry;
    if (!allocation)
        goto done;
    *entry = allocation->next;
    heap->used -= allocation->size;
    free(allocation->raw);
    free(allocation);
    result = 1;
done:
    pthread_mutex_unlock(&registry_mutex);
    return result;
}

uintptr_t beer_native_heap_destroy(uintptr_t handle)
{
    NativeHeap **entry, *heap;
    pthread_mutex_lock(&registry_mutex);
    entry = &registry;
    while (*entry && (uintptr_t)*entry != handle)
        entry = &(*entry)->next;
    heap = *entry;
    if (!heap) {
        pthread_mutex_unlock(&registry_mutex);
        return handle;
    }
    *entry = heap->next;
    while (heap->allocations) {
        NativeAllocation *allocation = heap->allocations;
        heap->allocations = allocation->next;
        free(allocation->raw);
        free(allocation);
    }
    free(heap->raw);
    pthread_mutex_unlock(&registry_mutex);
    return 0;
}

size_t beer_native_heap_size(uintptr_t handle, uint32_t flags, const void *ptr)
{
    NativeHeap *heap;
    NativeAllocation *allocation;
    size_t result = SIZE_MAX;
    if (!ptr || (flags & ~BEER_HEAP_NO_SERIALIZE))
        return result;
    pthread_mutex_lock(&registry_mutex);
    heap = find_heap(handle);
    if (heap) {
        allocation = *find_allocation(heap, ptr);
        if (allocation)
            result = allocation->size;
    }
    pthread_mutex_unlock(&registry_mutex);
    return result;
}