#include "win32_native_heap.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

static uint32_t header_word(uintptr_t heap, size_t offset)
{
    uint32_t value;
    memcpy(&value, (const unsigned char *)heap + offset, sizeof(value));
    return value;
}

static void bytes_equal(const unsigned char *ptr, size_t size, unsigned char byte)
{
    size_t i;
    for (i = 0; i < size; ++i)
        assert(ptr[i] == byte);
}

static void test_creation(void)
{
    uintptr_t heaps[32];
    size_t i, j;
    int foreign = 0;
    assert(!beer_native_heap_create(BEER_HEAP_GENERATE_EXCEPTIONS, NULL, 0, 0, NULL, NULL));
    assert(!beer_native_heap_create(UINT32_C(0x80000000), NULL, 0, 0, NULL, NULL));
    assert(!beer_native_heap_create(0, &foreign, 0, 0, NULL, NULL));
    assert(!beer_native_heap_create(0, NULL, 0, 0, &foreign, NULL));
    assert(!beer_native_heap_create(0, NULL, 0, 0, NULL, &foreign));
    assert(!beer_native_heap_create(0, NULL, 32, 33, NULL, NULL));
    for (i = 0; i < 32; ++i) {
        uint32_t flags = i % 2 ? BEER_HEAP_NO_SERIALIZE | BEER_HEAP_ZERO_MEMORY : 0;
        heaps[i] = beer_native_heap_create(flags, NULL, i % 2 ? 64 : 0, 0, NULL, NULL);
        assert(heaps[i] && !(heaps[i] & 15));
        assert(beer_native_heap_contains(heaps[i]));
        for (j = 0; j < i; ++j)
            assert(heaps[i] != heaps[j]);
        assert(header_word(heaps[i], 0x10) == UINT32_C(0xffeeffee));
        assert(header_word(heaps[i], 0x70) == (flags | (i % 2 ? 0 : BEER_HEAP_GROWABLE)));
        assert(header_word(heaps[i], 0x74) == 0);
        for (j = 0; j < BEER_NATIVE_HEAP_HEADER_SIZE; ++j)
            if (!(j >= 0x10 && j < 0x14) && !(j >= 0x70 && j < 0x74))
                assert(((const unsigned char *)heaps[i])[j] == 0);
    }
    for (i = 0; i < 32; ++i) {
        assert(!beer_native_heap_destroy(heaps[i]));
        assert(!beer_native_heap_contains(heaps[i]));
        assert(beer_native_heap_destroy(heaps[i]) == heaps[i]);
    }
}

static void test_invalid(void)
{
    int foreign = 0;
    uintptr_t bad[] = {0, 1, UINTPTR_MAX, (uintptr_t)&foreign};
    uintptr_t heap = beer_native_heap_create(0, NULL, 0, 0, NULL, NULL);
    unsigned char *ptr = beer_native_heap_alloc(heap, 0, 32);
    size_t i;
    assert(heap && ptr);
    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        assert(!beer_native_heap_contains(bad[i]));
        assert(!beer_native_heap_alloc(bad[i], 0, 1));
        assert(!beer_native_heap_reallocate(bad[i], 0, ptr, 1));
        assert(!beer_native_heap_free(bad[i], 0, ptr));
        assert(beer_native_heap_size(bad[i], 0, ptr) == SIZE_MAX);
        assert(beer_native_heap_destroy(bad[i]) == bad[i]);
    }
    assert(!beer_native_heap_contains(heap + 1));
    assert(!beer_native_heap_free(heap, 0, &foreign));
    assert(!beer_native_heap_free(heap, 0, ptr + 1));
    assert(!beer_native_heap_reallocate(heap, 0, &foreign, 64));
    assert(!beer_native_heap_reallocate(heap, 0, ptr + 1, 64));
    assert(beer_native_heap_size(heap, 0, ptr + 1) == SIZE_MAX);
    assert(!beer_native_heap_free(heap, 0, NULL));
    assert(!beer_native_heap_reallocate(heap, 0, NULL, 32));
    assert(beer_native_heap_size(heap, 0, NULL) == SIZE_MAX);
    assert(!beer_native_heap_destroy(heap)); /* Live allocation deliberately left. */
    assert(!beer_native_heap_free(heap, 0, ptr));
    assert(beer_native_heap_size(heap, 0, ptr) == SIZE_MAX);
}

static void test_allocations(void)
{
    uintptr_t heap = beer_native_heap_create(0, NULL, 0, SIZE_MAX, NULL, NULL);
    uintptr_t other = beer_native_heap_create(0, NULL, 0, 0, NULL, NULL);
    unsigned char *zero, *ptr, *grown;
    uint32_t rejected[] = {BEER_HEAP_GENERATE_EXCEPTIONS, UINT32_C(0x80000000)};
    size_t i;
    assert(heap && other);
    zero = beer_native_heap_alloc(heap, BEER_HEAP_ZERO_MEMORY, 0);
    ptr = beer_native_heap_alloc(heap, BEER_HEAP_ZERO_MEMORY, 31);
    assert(zero && ptr && zero != ptr && !((uintptr_t)zero & 15) && !((uintptr_t)ptr & 15));
    assert(zero[0] == 0 && beer_native_heap_size(heap, 0, zero) == 0);
    bytes_equal(ptr, 31, 0);
    memset(ptr, 0xa5, 31);
    for (i = 0; i < sizeof(rejected) / sizeof(rejected[0]); ++i) {
        assert(!beer_native_heap_alloc(heap, rejected[i], 32));
        assert(!beer_native_heap_reallocate(heap, rejected[i], ptr, 32));
        assert(!beer_native_heap_free(heap, rejected[i], ptr));
        assert(beer_native_heap_size(heap, rejected[i], ptr) == SIZE_MAX);
    }
    assert(!beer_native_heap_alloc(heap, BEER_HEAP_REALLOC_IN_PLACE_ONLY, 1));
    assert(!beer_native_heap_free(heap, BEER_HEAP_ZERO_MEMORY, ptr));
    assert(beer_native_heap_size(heap, BEER_HEAP_ZERO_MEMORY, ptr) == SIZE_MAX);
    assert(!beer_native_heap_free(other, 0, ptr));
    assert(!beer_native_heap_reallocate(other, 0, ptr, 64));
    assert(beer_native_heap_size(other, 0, ptr) == SIZE_MAX);
    assert(!beer_native_heap_alloc(heap, 0, SIZE_MAX));
    assert(!beer_native_heap_alloc(heap, 0, SIZE_MAX - 14));
    assert(!beer_native_heap_reallocate(heap, 0, ptr, SIZE_MAX));
    assert(!beer_native_heap_reallocate(heap, BEER_HEAP_REALLOC_IN_PLACE_ONLY, ptr, 32));
    assert(beer_native_heap_size(heap, 0, ptr) == 31);
    bytes_equal(ptr, 31, 0xa5);
    grown = beer_native_heap_reallocate(heap, BEER_HEAP_ZERO_MEMORY, ptr, 97);
    assert(grown && !((uintptr_t)grown & 15));
    bytes_equal(grown, 31, 0xa5);
    bytes_equal(grown + 31, 66, 0);
    assert(beer_native_heap_size(heap, 0, grown) == 97);
    memset(grown, 0x5a, 97);
    assert(beer_native_heap_reallocate(heap, BEER_HEAP_REALLOC_IN_PLACE_ONLY |
                                      BEER_HEAP_ZERO_MEMORY, grown, 12) == grown);
    bytes_equal(grown, 12, 0x5a);
    ptr = beer_native_heap_reallocate(heap, BEER_HEAP_ZERO_MEMORY, grown, 24);
    assert(ptr);
    bytes_equal(ptr, 12, 0x5a);
    bytes_equal(ptr + 12, 12, 0);
    assert(beer_native_heap_reallocate(heap, BEER_HEAP_REALLOC_IN_PLACE_ONLY, ptr, 24) == ptr);
    assert(beer_native_heap_reallocate(heap, 0, ptr, 0) == ptr);
    assert(beer_native_heap_size(heap, 0, ptr) == 0);
    assert(!beer_native_heap_reallocate(heap, BEER_HEAP_REALLOC_IN_PLACE_ONLY, ptr, 1));
    grown = beer_native_heap_reallocate(heap, BEER_HEAP_ZERO_MEMORY, ptr, 16);
    assert(grown);
    bytes_equal(grown, 16, 0);
    assert(beer_native_heap_free(heap, BEER_HEAP_NO_SERIALIZE, grown));
    assert(!beer_native_heap_free(heap, 0, grown));
    assert(!beer_native_heap_reallocate(heap, 0, grown, 16));
    assert(beer_native_heap_size(heap, 0, grown) == SIZE_MAX);
    assert(beer_native_heap_free(heap, 0, zero));
    assert(!beer_native_heap_destroy(heap));
    assert(!beer_native_heap_destroy(other));
}

static void test_capacity(void)
{
    uintptr_t heap = beer_native_heap_create(BEER_HEAP_ZERO_MEMORY, NULL, 64, 64, NULL, NULL);
    uintptr_t growable = beer_native_heap_create(BEER_HEAP_GROWABLE, NULL, 1, SIZE_MAX, NULL, NULL);
    unsigned char *a, *b, *c;
    assert(heap && growable);
    a = beer_native_heap_alloc(heap, 0, 48);
    b = beer_native_heap_alloc(heap, 0, 16);
    assert(a && b);
    bytes_equal(a, 48, 0); /* Inherited creation ZERO_MEMORY. */
    memset(a, 0x33, 48);
    assert(!beer_native_heap_alloc(heap, 0, 1));
    c = beer_native_heap_alloc(heap, 0, 0);
    assert(c && beer_native_heap_free(heap, 0, c));
    assert(!beer_native_heap_reallocate(heap, 0, a, 49));
    bytes_equal(a, 48, 0x33);
    assert(beer_native_heap_size(heap, 0, a) == 48);
    assert(beer_native_heap_reallocate(heap, 0, a, 32) == a);
    c = beer_native_heap_alloc(heap, 0, 16);
    assert(c && beer_native_heap_free(heap, 0, c));
    assert(beer_native_heap_free(heap, 0, b));
    b = beer_native_heap_reallocate(heap, 0, a, 64);
    assert(b);
    bytes_equal(b, 32, 0x33);
    bytes_equal(b + 32, 32, 0);
    assert(!beer_native_heap_alloc(heap, 0, 1));
    assert(beer_native_heap_alloc(growable, 0, 4096));
    assert(header_word(growable, 0x70) & BEER_HEAP_GROWABLE);
    assert(!beer_native_heap_destroy(heap)); /* Frees b. */
    assert(!beer_native_heap_destroy(growable)); /* Frees 4096-byte block. */
}

typedef struct Worker {
    uintptr_t heap;
    unsigned char byte;
} Worker;

static void *worker(void *argument)
{
    Worker *context = argument;
    size_t iteration, i;
    for (iteration = 0; iteration < 2000; ++iteration) {
        unsigned char *blocks[8];
        for (i = 0; i < 8; ++i) {
            size_t size = (iteration + i) % 127;
            blocks[i] = beer_native_heap_alloc(context->heap, BEER_HEAP_ZERO_MEMORY |
                                               BEER_HEAP_NO_SERIALIZE, size);
            assert(blocks[i] && !((uintptr_t)blocks[i] & 15));
            bytes_equal(blocks[i], size, 0);
            memset(blocks[i], context->byte, size);
            assert(beer_native_heap_size(context->heap, 0, blocks[i]) == size);
            blocks[i] = beer_native_heap_reallocate(context->heap, BEER_HEAP_ZERO_MEMORY,
                                                    blocks[i], size + 16);
            assert(blocks[i]);
            bytes_equal(blocks[i], size, context->byte);
            bytes_equal(blocks[i] + size, 16, 0);
        }
        for (i = 0; i < 8; ++i)
            assert(beer_native_heap_free(context->heap, 0, blocks[i]));
    }
    return NULL;
}

static void test_threads(void)
{
    uintptr_t heap = beer_native_heap_create(BEER_HEAP_NO_SERIALIZE, NULL, 0, 0, NULL, NULL);
    pthread_t threads[4];
    Worker contexts[4];
    size_t i;
    assert(heap);
    for (i = 0; i < 4; ++i) {
        contexts[i].heap = heap;
        contexts[i].byte = (unsigned char)(i + 1);
        assert(pthread_create(&threads[i], NULL, worker, &contexts[i]) == 0);
    }
    for (i = 0; i < 4; ++i)
        assert(pthread_join(threads[i], NULL) == 0);
    assert(!beer_native_heap_destroy(heap));
}

int main(void)
{
    test_creation();
    test_invalid();
    test_allocations();
    test_capacity();
    test_threads();
    puts("win32_native_heap: all tests passed (4 threads, 64000 allocation/free pairs)");
    return 0;
}