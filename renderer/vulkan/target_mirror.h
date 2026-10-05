#ifndef BEER_TARGET_MIRROR_H
#define BEER_TARGET_MIRROR_H

#include <stddef.h>
#include <stdint.h>

/* Render-target mirror cache policy.
 *
 * A Vulkan target mirror is only a legitimate substitute for a D3D11
 * render target when three facts hold at the same time:
 *
 *   1. the mirror currently owns that exact resource,
 *   2. its device-local bytes were actually produced for the requested
 *      content serial, and
 *   3. its allocation still describes the same surface size.
 *
 * Beer previously inferred all three from the serial value alone. Content
 * serials start at zero, mirror slots are recycled, and invalidation reset
 * the serial back to zero while leaving the device buffer untouched. A newly
 * created Scaleform target therefore matched a recycled slot that still held
 * another surface's pixels: the renderer skipped the upload and blended the
 * new frame on top of a previous control's leftover rasterization. That is
 * directly observable as UI layers that appear, disappear or bleed into each
 * other depending on which control was last built.
 *
 * These predicates make the validity explicit so it can be tested without a
 * Vulkan device. */

typedef struct {
    const void *resource;
    uint64_t serial;
    size_t bytes;
    int content_valid;
} BeerTargetMirrorState;

/* True when the mirror holds device-local content that was produced for the
 * exact (resource, serial, bytes) triple a reader is asking for. */
static inline int beer_target_mirror_matches(const BeerTargetMirrorState *mirror,
                                             const void *resource,
                                             uint64_t serial, size_t bytes)
{
    if (!mirror || !resource || !bytes) return 0;
    return mirror->content_valid && mirror->resource == resource &&
        mirror->serial == serial && mirror->bytes == bytes;
}

/* True when a draw must re-upload the CPU copy before rasterizing into the
 * mirror. Unknown content, a recycled slot and a serial gap are all genuine
 * re-upload conditions; only a mirror that already produced exactly this
 * draw's input serial may be accumulated into. */
static inline int beer_target_mirror_needs_upload(
    const BeerTargetMirrorState *mirror, const void *resource,
    uint64_t input_serial, size_t bytes)
{
    if (!mirror || !resource || !bytes) return 1;
    if (!mirror->content_valid) return 1;
    if (mirror->resource != resource) return 1;
    if (mirror->bytes != bytes) return 1;
    return mirror->serial != input_serial;
}

/* Validity a slot retains when it is *claimed* as the destination of a draw
 * or dispatch that has not executed yet.
 *
 * Claiming must never publish the requested serial as if it were already
 * produced. A concurrent reader looking up (resource, serial) would then
 * match the slot and consume the previous frame's device memory as this
 * frame's result, which is exactly the "hovering one control changes an
 * unrelated layer" symptom. Content stays valid only while the slot keeps
 * describing the same (resource, serial, size) triple it already produced. */
static inline int beer_target_mirror_claim_valid(
    const BeerTargetMirrorState *mirror, const void *resource,
    uint64_t pending_serial, size_t bytes)
{
    if (!mirror) return 0;
    if (!mirror->content_valid) return 0;
    if (mirror->resource != resource) return 0;
    if (mirror->bytes != bytes) return 0;
    return mirror->serial == pending_serial;
}

#endif /* BEER_TARGET_MIRROR_H */
