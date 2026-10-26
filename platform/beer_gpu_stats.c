#include "beer_gpu_stats.h"

#include <dirent.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static atomic_int g_ok, g_util;
static _Atomic uint64_t g_used, g_total, g_process;
static pthread_once_t once = PTHREAD_ONCE_INIT;

typedef struct { unsigned gpu, memory; } NvmlUtil;
typedef struct { unsigned long long total, free, used; } NvmlMemory;
typedef struct { unsigned pid; unsigned pad; unsigned long long used; unsigned gpu_instance, compute_instance; } NvmlProcessV3;

static long read_long(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    long v = -1;
    if (fscanf(f, "%ld", &v) != 1) v = -1;
    fclose(f);
    return v;
}

static void *poll_thread(void *opaque)
{
    (void)opaque;
    void *nvml = dlopen("libnvidia-ml.so.1", RTLD_NOW | RTLD_LOCAL);
    int (*init)(void) = nvml ? dlsym(nvml, "nvmlInit_v2") : NULL;
    int (*handle)(unsigned, void **) = nvml ? dlsym(nvml, "nvmlDeviceGetHandleByIndex_v2") : NULL;
    int (*util)(void *, NvmlUtil *) = nvml ? dlsym(nvml, "nvmlDeviceGetUtilizationRates") : NULL;
    int (*memory)(void *, NvmlMemory *) = nvml ? dlsym(nvml, "nvmlDeviceGetMemoryInfo") : NULL;
    int (*procs)(void *, unsigned *, NvmlProcessV3 *) = nvml ? dlsym(nvml, "nvmlDeviceGetGraphicsRunningProcesses_v3") : NULL;
    void *device = NULL;
    const int have_nvml = init && handle && util && memory && init() == 0 && handle(0, &device) == 0;

    /* AMD: the first card exposing gpu_busy_percent. */
    char amd_busy[200] = "", amd_used[200] = "", amd_total[200] = "";
    if (!have_nvml) {
        DIR *d = opendir("/sys/class/drm");
        struct dirent *e;
        while (d && (e = readdir(d))) {
            if (strncmp(e->d_name, "card", 4) || strchr(e->d_name + 4, '-')) continue;
            snprintf(amd_busy, sizeof(amd_busy), "/sys/class/drm/%s/device/gpu_busy_percent", e->d_name);
            if (read_long(amd_busy) >= 0) {
                snprintf(amd_used, sizeof(amd_used), "/sys/class/drm/%s/device/mem_info_vram_used", e->d_name);
                snprintf(amd_total, sizeof(amd_total), "/sys/class/drm/%s/device/mem_info_vram_total", e->d_name);
                break;
            }
            amd_busy[0] = 0;
        }
        if (d) closedir(d);
    }
    for (;;) {
        if (have_nvml) {
            NvmlUtil u = {0};
            NvmlMemory m = {0};
            if (util(device, &u) == 0 && memory(device, &m) == 0) {
                atomic_store(&g_util, (int)u.gpu);
                atomic_store(&g_used, m.used >> 20);
                atomic_store(&g_total, m.total >> 20);
                atomic_store(&g_ok, 1);
            }
            if (procs) {
                NvmlProcessV3 list[64];
                unsigned count = 64;
                memset(list, 0, sizeof(list));
                uint64_t mine = 0;
                if (procs(device, &count, list) == 0)
                    for (unsigned i = 0; i < count && i < 64; ++i)
                        if (list[i].pid == (unsigned)getpid()) mine += list[i].used >> 20;
                atomic_store(&g_process, mine);
            }
        } else if (amd_busy[0]) {
            const long busy = read_long(amd_busy), used = read_long(amd_used), total = read_long(amd_total);
            if (busy >= 0) {
                atomic_store(&g_util, (int)busy);
                atomic_store(&g_used, used > 0 ? (uint64_t)used >> 20 : 0);
                atomic_store(&g_total, total > 0 ? (uint64_t)total >> 20 : 0);
                atomic_store(&g_ok, 1);
            }
        }
        struct timespec ts = { 1, 0 };
        nanosleep(&ts, NULL);
    }
    return NULL;
}

static void start(void)
{
    pthread_t thread;
    if (pthread_create(&thread, NULL, poll_thread, NULL) == 0) pthread_detach(thread);
}

int beer_gpu_stats_get(int *util_percent, uint64_t *vram_used_mb, uint64_t *vram_total_mb, uint64_t *process_vram_mb)
{
    pthread_once(&once, start);
    if (!atomic_load(&g_ok)) return 0;
    *util_percent = atomic_load(&g_util);
    *vram_used_mb = atomic_load(&g_used);
    *vram_total_mb = atomic_load(&g_total);
    *process_vram_mb = atomic_load(&g_process);
    return 1;
}
