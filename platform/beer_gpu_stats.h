#ifndef BEER_GPU_STATS_H
#define BEER_GPU_STATS_H

#include <stdint.h>

/* GPU load and video memory for the overlay. NVIDIA through NVML (loaded at run time), AMD through sysfs;
 * a background thread refreshes the numbers about once a second. Returns 0 when nothing is available. */
int beer_gpu_stats_get(int *util_percent, uint64_t *vram_used_mb, uint64_t *vram_total_mb, uint64_t *process_vram_mb);

#endif
