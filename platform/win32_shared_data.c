#define _GNU_SOURCE
#include "win32_shared_data.h"
#include "beer_arch.h"
#include "beer_time.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static void put_u16(uint8_t *data, size_t offset, uint16_t value)
{
    memcpy(data + offset, &value, sizeof(value));
}

static void put_u32(uint8_t *data, size_t offset, uint32_t value)
{
    memcpy(data + offset, &value, sizeof(value));
}

static void put_u64(uint8_t *data, size_t offset, uint64_t value)
{
    memcpy(data + offset, &value, sizeof(value));
}

static void put_system_time(uint8_t *data, size_t offset, uint64_t value)
{
    put_u32(data, offset, (uint32_t)value);
    put_u32(data, offset + 4, (uint32_t)(value >> 32));
    put_u32(data, offset + 8, (uint32_t)(value >> 32));
}

void beer_win32_shared_data_refresh(void *memory, size_t size,
                                    uint64_t realtime_100ns,
                                    uint64_t monotonic_100ns)
{
    if (!memory || size < BEER_WIN32_SHARED_DATA_SIZE)
        return;

    uint8_t *data = memory;
    put_system_time(data, 0x008, monotonic_100ns); /* InterruptTime */
    put_system_time(data, 0x014, realtime_100ns);  /* SystemTime */
    uint64_t tick_ms = monotonic_100ns / 10000u;
    put_u32(data, 0x2f4, (uint32_t)tick_ms); /* LastSystemRITEventTickCount */
    put_system_time(data, 0x320, tick_ms); /* TickCount */
}

void beer_win32_shared_data_initialize(void *memory, size_t size)
{
    if (!memory || size < BEER_WIN32_SHARED_DATA_SIZE)
        return;

    uint8_t *data = memory;
    memset(data, 0, BEER_WIN32_SHARED_DATA_SIZE);

    /* KUSER_SHARED_DATA is a stable user ABI at 0x7ffe0000. Protected and
     * ordinary Windows binaries both read it directly rather than importing an
     * API, so leaving the page unmapped causes faults before window creation. */
    put_u32(data, 0x004, 1u << 24); /* TickCountMultiplier */

    struct timespec realtime = {0};
    struct timespec monotonic = {0};
    clock_gettime(CLOCK_REALTIME, &realtime);
    clock_gettime(CLOCK_MONOTONIC, &monotonic);
    uint64_t unix_100ns = (uint64_t)realtime.tv_sec * 10000000u +
                          (uint64_t)realtime.tv_nsec / 100u;
    uint64_t windows_100ns = unix_100ns + 116444736000000000ull;
    uint64_t interrupt_100ns = (uint64_t)monotonic.tv_sec * 10000000u +
                               (uint64_t)monotonic.tv_nsec / 100u;
    beer_win32_shared_data_refresh(data, BEER_WIN32_SHARED_DATA_SIZE,
                                   windows_100ns, interrupt_100ns);

    static const char system_root[] = "C:\\Windows";
    for (size_t i = 0; i < sizeof(system_root); ++i)
        put_u16(data, 0x030 + i * 2, (uint8_t)system_root[i]);

    put_u32(data, 0x244, 0x200000u); /* LargePageMinimum */
    put_u32(data, 0x260, 19045); /* NtBuildNumber */
    put_u32(data, 0x264, 1);     /* NtProductWinNt */
    data[0x268] = 1;             /* ProductTypeIsValid */
    put_u16(data, 0x26a, 9);     /* PROCESSOR_ARCHITECTURE_AMD64 */
    put_u32(data, 0x26c, 10);    /* NtMajorVersion */
    put_u32(data, 0x270, 0);     /* NtMinorVersion */

    /* ProcessorFeatures[] (0x274) uses winnt.h PF_* numbering and must agree with IsProcessorFeaturePresent: runtime
     * libraries read the array directly. Both read beer_guest_processor_features(). */
    uint8_t features[BEER_PF_COUNT];
    beer_guest_processor_features(features);
    memcpy(data + 0x274, features, sizeof(features));

    put_u16(data, 0x02c, 0x014c); /* ImageNumberLow: IMAGE_FILE_MACHINE_I386 (WOW64 range) */
    put_u16(data, 0x02e, 0x8664); /* ImageNumberHigh: IMAGE_FILE_MACHINE_AMD64 */

    /* XSTATE_CONFIGURATION (0x3d8): what GetEnabledXStateFeatures / RtlGetEnabledExtendedFeatures report. x87 + SSE,
     * plus AVX when the host has it; area offsets follow the standard XSAVE layout. */
    {
        const int avx = features[39] != 0;
        const uint64_t enabled = 0x3u | (avx ? 0x4u : 0u);
        put_u64(data, 0x3d8, enabled);                       /* EnabledFeatures */
        put_u64(data, 0x3e0, enabled);                       /* EnabledVolatileFeatures */
        put_u32(data, 0x3e8, avx ? 832u : 576u);             /* Size of the XSAVE area */
        put_u32(data, 0x3ec, 0);                             /* ControlFlags */
        put_u32(data, 0x3f0, 0);   put_u32(data, 0x3f4, 160);  /* x87: offset 0, size 160 */
        put_u32(data, 0x3f8, 160); put_u32(data, 0x3fc, 256);  /* SSE: offset 160, size 256 */
        if (avx) { put_u32(data, 0x400, 576); put_u32(data, 0x404, 256); }   /* AVX: offset 576, size 256 */
    }

    /* These offsets are part of the stable x64 KUSER_SHARED_DATA user ABI.
     * Code may read them without an imported API, so an internally plausible
     * value at the wrong offset is still an ABI failure. */
    put_u16(data, 0x2e6, 64); /* CyclesPerYield */
    long physical_pages = sysconf(_SC_PHYS_PAGES);
    if (physical_pages < 1) physical_pages = 1;
    if ((uint64_t)physical_pages > UINT32_MAX) physical_pages = UINT32_MAX;
    put_u32(data, 0x2f8, (uint32_t)physical_pages); /* NumberOfPhysicalPages */
    put_u64(data, 0x310, 10000000u); /* QpcFrequency */
}

static _Atomic int shared_data_updater_running;
static void *shared_data_mapping;

static void *shared_data_update_thread(void *opaque)
{
    (void)opaque;
    while (atomic_load_explicit(&shared_data_updater_running,
                                memory_order_relaxed)) {
        /* Guest-visible clocks follow the manager's game-speed multiplier. */
        uint64_t unix_100ns = beer_time_realtime_ns_for(BEER_CLOCK_KUSER) / 100u;
        uint64_t windows_100ns = unix_100ns + 116444736000000000ull;
        uint64_t interrupt_100ns = beer_time_monotonic_ns_for(BEER_CLOCK_KUSER) / 100u;
        beer_win32_shared_data_refresh(shared_data_mapping,
                                       BEER_WIN32_SHARED_DATA_SIZE,
                                       windows_100ns, interrupt_100ns);
        struct timespec interval = {0, 1000000L};
        nanosleep(&interval, NULL);
    }
    return NULL;
}

static void shared_data_stop_updater(void)
{
    atomic_store_explicit(&shared_data_updater_running, 0, memory_order_relaxed);
}

int beer_win32_shared_data_map(void)
{
    void *wanted = (void *)BEER_WIN32_SHARED_DATA_ADDRESS;
    char backing_name[64];
    snprintf(backing_name, sizeof(backing_name), "/beer-kuser-%ld",
             (long)getpid());
#if defined(__ANDROID__)
    /* bionic has no shm_open/shm_unlink: an anonymous memfd gives the same sealed-off backing. */
    (void)backing_name;
    int fd = (int)syscall(SYS_memfd_create, "beer-kuser", 0u);
#else
    int fd = shm_open(backing_name, O_CREAT | O_EXCL | O_RDWR, 0600);
#endif
    if (fd < 0) {
        fprintf(stderr, "[KUSER] unable to create shared-data backing: %s\n",
                strerror(errno));
        return 0;
    }
#if !defined(__ANDROID__)
    shm_unlink(backing_name);
#endif
    if (ftruncate(fd, BEER_WIN32_SHARED_DATA_SIZE) != 0) {
        fprintf(stderr, "[KUSER] unable to size shared-data backing: %s\n",
                strerror(errno));
        close(fd);
        return 0;
    }

    /* Map the same pages twice. Guest code sees a read-only Windows ABI page,
     * while the host updater retains a writable alias. This matches the live
     * KUSER_SHARED_DATA clock semantics without allowing guest writes. */
    void *mapped = mmap(wanted, BEER_WIN32_SHARED_DATA_SIZE, PROT_READ,
                        MAP_SHARED | MAP_FIXED_NOREPLACE, fd, 0);
    if (mapped == MAP_FAILED) {
        if (errno == EEXIST) {
            fprintf(stderr, "[KUSER] shared-data address is already mapped\n");
            close(fd);
            return 1;
        }
        fprintf(stderr, "[KUSER] unable to map Windows shared data at %p: %s\n",
                wanted, strerror(errno));
        close(fd);
        return 0;
    }
    void *writable = mmap(NULL, BEER_WIN32_SHARED_DATA_SIZE,
                          PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (writable == MAP_FAILED) {
        fprintf(stderr, "[KUSER] unable to map writable shared-data alias: %s\n",
                strerror(errno));
        munmap(mapped, BEER_WIN32_SHARED_DATA_SIZE);
        close(fd);
        return 0;
    }

    beer_win32_shared_data_initialize(writable, BEER_WIN32_SHARED_DATA_SIZE);
    shared_data_mapping = writable;
    atomic_store_explicit(&shared_data_updater_running, 1, memory_order_relaxed);
    pthread_t updater;
    if (pthread_create(&updater, NULL, shared_data_update_thread, NULL) != 0) {
        fprintf(stderr, "[KUSER] unable to start shared-data clock updater\n");
        atomic_store_explicit(&shared_data_updater_running, 0,
                              memory_order_relaxed);
        munmap(writable, BEER_WIN32_SHARED_DATA_SIZE);
        munmap(mapped, BEER_WIN32_SHARED_DATA_SIZE);
        close(fd);
        shared_data_mapping = NULL;
        return 0;
    }
    pthread_detach(updater);
    atexit(shared_data_stop_updater);
    fprintf(stderr, "[KUSER] mapped live Windows shared data at %p\n", mapped);
    return 1;
}
