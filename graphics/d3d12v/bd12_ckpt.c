#define _GNU_SOURCE
#include "bd12.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* BEER_VK_CHECKPOINTS=1 (VK_NV_device_diagnostic_checkpoints): every recorded draw, copy, clear and dispatch drops a
 * numbered checkpoint into its command buffer. After a GPU fault or hang the driver reports the last checkpoint each
 * pipeline stage reached; bd12_ckpt_dump matches those numbers against a ring of what each one was. */

#define RING 16384u

typedef struct { uint64_t seq; uint32_t kind; uint32_t w, h, d; uint64_t a, b; } Entry;

static Entry g_ring[RING];
static uint64_t g_seq;
static PFN_vkCmdSetCheckpointNV g_set;
typedef void (*GetCheckpointFn)(VkQueue, uint32_t *, VkCheckpointDataNV *);
static GetCheckpointFn g_get;
static BD12Device *g_device;
static int g_state;   /* 0 unknown, 1 enabled, -1 disabled */

static int enabled(BD12Device *device)
{
    if (g_state) return g_state > 0;
    g_state = -1;
    if (!getenv("BEER_VK_CHECKPOINTS")) return 0;
    PFN_vkGetDeviceProcAddr get_device = beer_vulkan_get_device_proc_addr();
    g_set = (PFN_vkCmdSetCheckpointNV)get_device(device->device, "vkCmdSetCheckpointNV");
    g_get = (GetCheckpointFn)get_device(device->device, "vkGetQueueCheckpointDataNV");
    if (!g_set || !g_get) {
        fprintf(stderr, "[BD12] VK_NV_device_diagnostic_checkpoints is not available\n");
        return 0;
    }
    g_device = device;
    g_state = 1;
    fprintf(stderr, "[BD12] GPU checkpoints enabled\n");
    return 1;
}

void bd12_ckpt_mark(BD12CmdList *list, uint32_t kind, uint64_t a, uint64_t b, uint32_t w, uint32_t h, uint32_t d)
{
    if (!enabled(list->device)) return;
    const uint64_t seq = __atomic_add_fetch(&g_seq, 1, __ATOMIC_RELAXED);
    g_ring[seq % RING] = (Entry){ seq, kind, w, h, d, a, b };
    g_set(list->command_buffer, (const void *)(uintptr_t)seq);
}

void bd12_ckpt_dump(void)
{
    if (g_state <= 0 || !g_device) return;
    static int busy;
    if (__atomic_exchange_n(&busy, 1, __ATOMIC_ACQUIRE)) return;
    VkCheckpointDataNV data[16];
    uint32_t count = 16;
    for (uint32_t i = 0; i < 16; ++i) data[i] = (VkCheckpointDataNV){ .sType = VK_STRUCTURE_TYPE_CHECKPOINT_DATA_NV };
    g_get(g_device->queue, &count, data);
    fprintf(stderr, "[CKPT] %u checkpoint record(s); last recorded sequence %llu\n", count, (unsigned long long)g_seq);
    for (uint32_t i = 0; i < count && i < 16; ++i) {
        const uint64_t seq = (uint64_t)(uintptr_t)data[i].pCheckpointMarker;
        const Entry *e = &g_ring[seq % RING];
        fprintf(stderr, "[CKPT]   stage=0x%x seq=%llu %s", (unsigned)data[i].stage, (unsigned long long)seq,
                e->seq == seq ? "" : "(overwritten) ");
        if (e->seq == seq)
            fprintf(stderr, "kind=%u a=%llx b=%llx dims=%ux%ux%u", e->kind, (unsigned long long)e->a, (unsigned long long)e->b, e->w, e->h, e->d);
        fprintf(stderr, "\n");
    }
    __atomic_store_n(&busy, 0, __ATOMIC_RELEASE);
}

/* Memory-corruption canary: the device limits never change after creation. A mismatch names the step after which
 * something overwrote the device structure. */
static uint64_t g_canary[2];
static BD12Device *g_canary_device;
static int g_canary_reported;

void bd12_canary_arm(BD12Device *device)
{
    g_canary_device = device;
    g_canary[0] = device->min_uniform_texel_alignment;
    g_canary[1] = device->min_storage_texel_alignment;
}

void bd12_canary_check(const char *where)
{
    BD12Device *device = g_canary_device;
    if (!device || g_canary_reported) return;
    if (device->min_uniform_texel_alignment == g_canary[0] && device->min_storage_texel_alignment == g_canary[1]) return;
    g_canary_reported = 1;
    fprintf(stderr, "[CANARY] device structure was overwritten; first noticed at: %s (uniform %llu->%llu storage %llu->%llu)\n",
            where, (unsigned long long)g_canary[0], (unsigned long long)device->min_uniform_texel_alignment,
            (unsigned long long)g_canary[1], (unsigned long long)device->min_storage_texel_alignment);
}
