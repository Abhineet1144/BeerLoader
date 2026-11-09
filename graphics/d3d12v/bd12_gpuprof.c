#define _GNU_SOURCE
#include "bd12.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* BEER_BD12_GPU_PROF=1: where GPU time goes, per shader.
 *
 * Every draw, dispatch, copy and clear is preceded by a timestamp (VK_PIPELINE_STAGE_2_ALL_COMMANDS: written once
 * all earlier work of the command buffer has finished), and the list ends with one. The time between a timestamp
 * and the next is charged to the operation recorded right after the first, so overlapping work is charged once and
 * barriers/waits land on the operation that waited. Closed recordings wait in a queue and are read at report time
 * once the GPU has finished them; their pools are then reused. The report (every 2 s with BEER_BD12_PROF's window) lists the most expensive shaders. Diagnostic only:
 * the timestamps themselves serialise the GPU slightly. */

#define SLOTS 8192
#define TABLE 4096
#define PENDING 512

/* A recording in flight: its timestamp pool and what each timestamp starts. */
typedef struct BD12GpuProfList {
    VkQueryPool pool;
    uint32_t count;
    uint64_t when_ns;            /* host time the recording was closed */
    uint64_t tag[SLOTS];         /* kind << 56 | shader hash (low 56 bits) */
} BD12GpuProfList;

typedef struct { uint64_t tag; uint64_t ns; uint64_t calls; } Entry;

float g_bd12_timestamp_period = 1.0f;
static Entry g_table[TABLE];
static uint64_t g_total_ns;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static BD12GpuProfList *g_free[PENDING];
static uint32_t g_free_count;
static BD12GpuProfList *g_pending[PENDING];   /* closed recordings, oldest first */
static uint32_t g_pending_count;
static BD12Device *g_device;

static int enabled(void)
{
    static int on = -1;
    if (on < 0) on = getenv("BEER_BD12_GPU_PROF") != NULL;
    return on;
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void charge(uint64_t tag, uint64_t ns)
{
    size_t s = (size_t)((tag * 0x9E3779B97F4A7C15ull) >> 52) & (TABLE - 1);
    for (size_t n = 0; n < TABLE; ++n, s = (s + 1) & (TABLE - 1)) {
        if (g_table[s].tag == tag || !g_table[s].calls) {
            g_table[s].tag = tag;
            g_table[s].ns += ns;
            g_table[s].calls++;
            return;
        }
    }
}

/* Caller holds g_lock. Reads finished recordings (oldest first) and returns their pools to the free list. A
 * recording still not readable after 3 s was never executed (or executed much later) and is dropped. */
static void harvest_locked(void)
{
    static uint64_t values[SLOTS];
    uint32_t kept = 0;
    const uint64_t now = now_ns();
    for (uint32_t i = 0; i < g_pending_count; ++i) {
        BD12GpuProfList *p = g_pending[i];
        int done = 0;
        if (p->count < 2) done = 1;
        else if (g_device->vk.GetQueryPoolResults(g_device->device, p->pool, 0, p->count, p->count * sizeof(uint64_t),
                                                  values, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            for (uint32_t q = 0; q + 1 < p->count; ++q) {
                if (values[q + 1] < values[q]) continue;
                const uint64_t ns = (uint64_t)((double)(values[q + 1] - values[q]) * g_bd12_timestamp_period);
                if (ns > 1000000000ull) continue;
                charge(p->tag[q], ns);
                g_total_ns += ns;
            }
            done = 1;
        } else if (now - p->when_ns > 3000000000ull) {
            done = 1;
        }
        if (done && g_free_count < PENDING) g_free[g_free_count++] = p;
        else if (done) { g_device->vk.DestroyQueryPool(g_device->device, p->pool, NULL); free(p); }
        else g_pending[kept++] = p;
    }
    g_pending_count = kept;
}

void bd12_gpuprof_list_begin(BD12CmdList *list)
{
    if (!enabled()) return;
    if (list->gpuprof) bd12_gpuprof_list_destroy(list);   /* an earlier recording never closed */
    pthread_mutex_lock(&g_lock);
    g_device = list->device;
    BD12GpuProfList *p = g_free_count ? g_free[--g_free_count] : NULL;
    pthread_mutex_unlock(&g_lock);
    if (!p) {
        p = calloc(1, sizeof(*p));
        if (!p) return;
        VkQueryPoolCreateInfo info = { .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                       .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = SLOTS };
        if (list->device->vk.CreateQueryPool(list->device->device, &info, NULL, &p->pool) != VK_SUCCESS) {
            free(p);
            return;
        }
    }
    list->device->vk.ResetQueryPool(list->device->device, p->pool, 0, SLOTS);   /* host reset (Vulkan 1.2) */
    p->count = 0;
    list->gpuprof = p;
}

void bd12_gpuprof_mark(BD12CmdList *list, uint32_t kind, uint64_t tag)
{
    BD12GpuProfList *p = list->gpuprof;
    if (!p || p->count >= SLOTS - 1) return;
    p->tag[p->count] = ((uint64_t)kind << 56) | (tag & 0x00ffffffffffffffull);
    list->device->vk.CmdWriteTimestamp2(list->command_buffer, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, p->pool,
                                        p->count++);
}

void bd12_gpuprof_list_end(BD12CmdList *list)
{
    BD12GpuProfList *p = list->gpuprof;
    if (!p) return;
    bd12_gpuprof_mark(list, 255, 0);                      /* closes the last operation's interval */
    p->when_ns = now_ns();
    list->gpuprof = NULL;
    pthread_mutex_lock(&g_lock);
    if (g_pending_count == PENDING) harvest_locked();
    if (g_pending_count < PENDING) { g_pending[g_pending_count++] = p; p = NULL; }
    pthread_mutex_unlock(&g_lock);
    if (p) { list->device->vk.DestroyQueryPool(list->device->device, p->pool, NULL); free(p); }
}

void bd12_gpuprof_list_destroy(BD12CmdList *list)
{
    BD12GpuProfList *p = list->gpuprof;
    if (!p) return;
    list->gpuprof = NULL;
    list->device->vk.DestroyQueryPool(list->device->device, p->pool, NULL);   /* never submitted */
    free(p);
}

static int by_time(const void *a, const void *b)
{
    const Entry *x = a, *y = b;
    return x->ns < y->ns ? 1 : x->ns > y->ns ? -1 : 0;
}

void bd12_gpuprof_report(void)
{
    if (!enabled()) return;
    static Entry snapshot[TABLE];
    pthread_mutex_lock(&g_lock);
    if (g_device) harvest_locked();
    memcpy(snapshot, g_table, sizeof(snapshot));
    const uint64_t total = g_total_ns;
    memset(g_table, 0, sizeof(g_table));
    g_total_ns = 0;
    pthread_mutex_unlock(&g_lock);
    qsort(snapshot, TABLE, sizeof(*snapshot), by_time);
    static const char *const names[] = { "draw", "draw-idx", "draw-ind", "draw-idx-ind", "copy-buf", "copy-buf-img",
                                         "copy-img-buf", "copy-img", "clear", "resolve" };
    fprintf(stderr, "[GPU PROF] measured %.1f ms of GPU time in this window\n", total / 1e6);
    for (int i = 0; i < 20 && snapshot[i].calls; ++i) {
        const uint32_t kind = (uint32_t)(snapshot[i].tag >> 56);
        const char *name = kind < 10 ? names[kind] : kind == 100 ? "dispatch" : kind == 101 ? "dispatch-ind" : "other";
        fprintf(stderr, "[GPU PROF]   %5.1f%% %8.2f ms %7llu x %-12s %014llx\n", total ? 100.0 * snapshot[i].ns / total : 0,
                snapshot[i].ns / 1e6, (unsigned long long)snapshot[i].calls, name,
                (unsigned long long)(snapshot[i].tag & 0x00ffffffffffffffull));
    }
}
