/* Deterministic tests for the Vulkan render-target mirror cache policy.
 *
 * These cover the exact conditions that produced Sekiro's hover-dependent UI
 * corruption: recycled mirror slots, freshly created targets whose content
 * serial is still zero, invalidated slots whose device memory still holds a
 * previous surface, and size changes on a reused allocation. */

#include "target_mirror.h"

#include <stdio.h>

static int failures;

static void check(int condition, const char *name)
{
    if (condition) return;
    fprintf(stderr, "FAIL: %s\n", name);
    ++failures;
}

int main(void)
{
    int target_a = 0;
    int target_b = 0;
    const void *resource_a = &target_a;
    const void *resource_b = &target_b;
    const size_t bytes = 1920u * 1080u * 4u;

    /* A zeroed slot has never been uploaded. Serial 0 must not look valid. */
    BeerTargetMirrorState empty = {0};
    check(!beer_target_mirror_matches(&empty, resource_a, 0, bytes),
          "empty mirror must not match serial 0");
    check(beer_target_mirror_needs_upload(&empty, resource_a, 0, bytes),
          "empty mirror must require upload at serial 0");

    /* A slot that genuinely produced this content is usable. */
    BeerTargetMirrorState valid = {
        .resource = resource_a, .serial = 7, .bytes = bytes, .content_valid = 1
    };
    check(beer_target_mirror_matches(&valid, resource_a, 7, bytes),
          "valid mirror must match its own resource/serial/size");
    check(!beer_target_mirror_needs_upload(&valid, resource_a, 7, bytes),
          "valid mirror must accumulate at its own output serial");

    /* A different draw's serial means the CPU copy is authoritative. */
    check(!beer_target_mirror_matches(&valid, resource_a, 8, bytes),
          "mirror must not match a newer serial");
    check(beer_target_mirror_needs_upload(&valid, resource_a, 8, bytes),
          "serial gap must force re-upload");

    /* A recycled slot holding another surface must never be reused, even
     * when the requested serial happens to coincide. */
    check(!beer_target_mirror_matches(&valid, resource_b, 7, bytes),
          "mirror must not match a different resource");
    check(beer_target_mirror_needs_upload(&valid, resource_b, 7, bytes),
          "different resource must force re-upload");

    /* Invalidated content: identity and serial still match, but the device
     * memory is stale. This is the hover-corruption case. */
    BeerTargetMirrorState invalidated = valid;
    invalidated.content_valid = 0;
    check(!beer_target_mirror_matches(&invalidated, resource_a, 7, bytes),
          "invalidated mirror must not match");
    check(beer_target_mirror_needs_upload(&invalidated, resource_a, 7, bytes),
          "invalidated mirror must force re-upload");

    /* A resized surface reuses the allocation but not the content. */
    check(!beer_target_mirror_matches(&valid, resource_a, 7, bytes / 2u),
          "mirror must not match a different surface size");
    check(beer_target_mirror_needs_upload(&valid, resource_a, 7, bytes / 2u),
          "size change must force re-upload");

    /* Degenerate arguments are rejected rather than silently accepted. */
    check(!beer_target_mirror_matches(&valid, NULL, 7, bytes),
          "null resource must not match");
    check(!beer_target_mirror_matches(&valid, resource_a, 7, 0),
          "zero size must not match");
    check(beer_target_mirror_needs_upload(NULL, resource_a, 7, bytes),
          "null mirror must force upload");

    /* A legitimately uploaded serial-0 target is usable; this guards against
     * over-correcting by treating serial 0 as permanently invalid. */
    BeerTargetMirrorState fresh = {
        .resource = resource_a, .serial = 0, .bytes = bytes, .content_valid = 1
    };
    check(beer_target_mirror_matches(&fresh, resource_a, 0, bytes),
          "uploaded serial-0 mirror must match");
    check(!beer_target_mirror_needs_upload(&fresh, resource_a, 0, bytes),
          "uploaded serial-0 mirror must not re-upload");

    /* Claim policy: acquiring a slot as the destination of a dispatch that
     * has not run yet must not advertise the pending serial as produced. */
    check(!beer_target_mirror_claim_valid(&valid, resource_a, 8, bytes),
          "claiming a new serial must drop validity");
    check(beer_target_mirror_claim_valid(&valid, resource_a, 7, bytes),
          "re-claiming the already produced serial stays valid");
    check(!beer_target_mirror_claim_valid(&valid, resource_b, 7, bytes),
          "claiming for another resource must drop validity");
    check(!beer_target_mirror_claim_valid(&valid, resource_a, 7, bytes / 2u),
          "claiming at another size must drop validity");
    check(!beer_target_mirror_claim_valid(&invalidated, resource_a, 7, bytes),
          "claiming an invalidated slot stays invalid");
    check(!beer_target_mirror_claim_valid(&empty, resource_a, 0, bytes),
          "claiming a never-uploaded slot stays invalid");
    check(!beer_target_mirror_claim_valid(NULL, resource_a, 7, bytes),
          "null mirror is never claim-valid");

    /* A claim followed by a lookup from a concurrent reader must miss: this
     * is the ordering that leaked a previous frame into an unrelated layer. */
    BeerTargetMirrorState claimed = valid;
    claimed.content_valid =
        beer_target_mirror_claim_valid(&valid, resource_a, 8, bytes);
    check(!beer_target_mirror_matches(&claimed, resource_a, 8, bytes),
          "a pending claim must not satisfy a reader at the pending serial");
    check(beer_target_mirror_needs_upload(&claimed, resource_a, 8, bytes),
          "a pending claim must still require upload");

    if (failures) {
        fprintf(stderr, "target mirror tests failed: %d\n", failures);
        return 1;
    }
    printf("target mirror cache policy tests passed\n");
    return 0;
}
