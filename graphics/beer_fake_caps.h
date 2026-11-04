/* BEER_FAKE_CAPS=1: testing switch. D3D12, D3D11 and DXGI capability queries report every feature Beer can
 * route to a no-op as supported; using an unbacked feature then does nothing (dropped draws, inert pipelines
 * and state objects, ignored commands). The answers are deliberately untrue: for finding which capability
 * check stops a title, never for normal play. Features whose entry points Beer does not expose at all
 * (ID3D12Device10+/List7+: enhanced barriers, relaxed casting, GPU upload heaps, work graphs) stay false,
 * because a "supported" answer would send the title to a missing method instead of a no-op. */
#ifndef BEER_FAKE_CAPS_H
#define BEER_FAKE_CAPS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* One process-wide state (0 = not read yet, 1 = off, 2 = on), defined in bd12_com.c, so the notice prints once. */
extern int beer_fake_caps_state;

static inline int beer_fake_caps(void)
{
    if (!beer_fake_caps_state) {
        const char *value = getenv("BEER_FAKE_CAPS");
        int enabled = value && *value && strcmp(value, "0") != 0;
        beer_fake_caps_state = enabled ? 2 : 1;
        if (enabled)
            fprintf(stderr, "[CAPS] BEER_FAKE_CAPS: D3D12/D3D11/DXGI capability checks report supported; "
                            "unbacked features do nothing (testing only)\n");
    }
    return beer_fake_caps_state == 2;
}

#endif
