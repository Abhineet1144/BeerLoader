#ifndef BEER_WIN32_NATIVE_HEAP_H
#define BEER_WIN32_NATIVE_HEAP_H

#include <stddef.h>
#include <stdint.h>

#define BEER_HEAP_NO_SERIALIZE          UINT32_C(0x00000001)
#define BEER_HEAP_GROWABLE              UINT32_C(0x00000002)
#define BEER_HEAP_GENERATE_EXCEPTIONS   UINT32_C(0x00000004)
#define BEER_HEAP_ZERO_MEMORY          UINT32_C(0x00000008)
#define BEER_HEAP_REALLOC_IN_PLACE_ONLY UINT32_C(0x00000010)
#define BEER_NATIVE_HEAP_HEADER_SIZE    0x400u

/* Host/System V ABI, not guest Win64 ABI. No loader or guest dependencies.
 * Create accepts NO_SERIALIZE, GROWABLE and ZERO_MEMORY only. All other bits,
 * including GENERATE_EXCEPTIONS, and nonnull base/lock/parameters fail.
 * reserve == 0 implies GROWABLE; otherwise GROWABLE must be explicit.
 * Growable reserve/commit are hints. Fixed heaps require commit <= reserve
 * and cap the sum of LIVE REQUESTED sizes (not metadata/alignment overhead).
 * NO_SERIALIZE is accepted but synchronization is never disabled.
 * Alloc accepts NO_SERIALIZE|ZERO_MEMORY; realloc also accepts IN_PLACE_ONLY;
 * free/size accept NO_SERIALIZE only. Creation ZERO_MEMORY is inherited.
 * Every successful allocation, even size zero, has 16-byte alignment.
 * Realloc requires an owned nonnull pointer, preserves min(old,new) bytes,
 * zeros only the grown tail when requested, and leaves the old block intact
 * on failure. Shrinks keep the pointer; IN_PLACE_ONLY rejects every growth.
 * Free(NULL), unknown/interior/cross-heap pointers and unknown handles fail.
 * Handle validation never dereferences the supplied handle. Live handles are
 * distinct readable headers: native uint32_t signature +0x10 = 0xffeeffee,
 * flags +0x70, ForceFlags +0x74 = 0; remaining public bytes are zero.
 * Header reads require caller coordination with destroy; do not modify it.
 * Destroy releases all live blocks and the header. Like malloc pointers,
 * retired handle/block addresses can be reused; they are not generation IDs.
 * API calls are serialized, including calls racing with destroy; callers
 * must coordinate direct block accesses themselves.
 */
uintptr_t beer_native_heap_create(uint32_t flags, void *base, size_t reserve,
                                  size_t commit, void *lock, void *parameters);
uint8_t beer_native_heap_contains(uintptr_t heap);
void *beer_native_heap_alloc(uintptr_t heap, uint32_t flags, size_t size);
void *beer_native_heap_reallocate(uintptr_t heap, uint32_t flags, void *ptr,
                                  size_t size);
uint8_t beer_native_heap_free(uintptr_t heap, uint32_t flags, void *ptr);
/* NULL/zero on success, original handle on failure (including zero input). */
uintptr_t beer_native_heap_destroy(uintptr_t heap);
/* Requested size, or SIZE_MAX on failure. */
size_t beer_native_heap_size(uintptr_t heap, uint32_t flags, const void *ptr);

#endif