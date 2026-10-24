#define _GNU_SOURCE
#include "win32_nt_section.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#define BEER_NT_SECTION_CAPACITY 256
#define BEER_NT_VIEW_CAPACITY 512
#define BEER_NT_SECTION_HANDLE_BASE UINT64_C(0xb3000000)
#define BEER_PAGE_NOACCESS          UINT32_C(0x01)
#define BEER_PAGE_READONLY          UINT32_C(0x02)
#define BEER_PAGE_READWRITE         UINT32_C(0x04)
#define BEER_PAGE_WRITECOPY         UINT32_C(0x08)
#define BEER_PAGE_EXECUTE           UINT32_C(0x10)
#define BEER_PAGE_EXECUTE_READ      UINT32_C(0x20)
#define BEER_PAGE_EXECUTE_READWRITE UINT32_C(0x40)
#define BEER_PAGE_EXECUTE_WRITECOPY UINT32_C(0x80)

typedef struct {
    int used;
    int fd;
    size_t size;
    uint32_t protection;
    uint32_t attributes;
} BeerNtSection;

typedef struct {
    int used;
    void *base;
    size_t size;
} BeerNtView;

static BeerNtSection g_sections[BEER_NT_SECTION_CAPACITY];
static BeerNtView g_views[BEER_NT_VIEW_CAPACITY];
static pthread_mutex_t g_section_mutex = PTHREAD_MUTEX_INITIALIZER;

static int protection_to_posix(uint32_t protection)
{
    switch (protection & UINT32_C(0xff)) {
    case BEER_PAGE_NOACCESS: return PROT_NONE;
    case BEER_PAGE_READONLY: return PROT_READ;
    case BEER_PAGE_READWRITE:
    case BEER_PAGE_WRITECOPY: return PROT_READ | PROT_WRITE;
    case BEER_PAGE_EXECUTE: return PROT_EXEC;
    case BEER_PAGE_EXECUTE_READ: return PROT_READ | PROT_EXEC;
    case BEER_PAGE_EXECUTE_READWRITE:
    case BEER_PAGE_EXECUTE_WRITECOPY: return PROT_READ | PROT_WRITE | PROT_EXEC;
    default: return -1;
    }
}

static int create_backing_fd(void)
{
    int fd;
#ifdef SYS_memfd_create
    fd = (int)syscall(SYS_memfd_create, "beer-nt-section", 1u /* MFD_CLOEXEC */);
    if (fd >= 0) return fd;
#endif
    char path[] = "/tmp/beer-nt-section-XXXXXX";
    fd = mkstemp(path);
    if (fd >= 0) {
        unlink(path);
        (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
    }
    return fd;
}

static int section_index(uint64_t handle)
{
    if (handle <= BEER_NT_SECTION_HANDLE_BASE ||
        handle >= BEER_NT_SECTION_HANDLE_BASE + BEER_NT_SECTION_CAPACITY)
        return -1;
    int index = (int)(handle - BEER_NT_SECTION_HANDLE_BASE);
    return g_sections[index].used ? index : -1;
}

uint32_t beer_nt_section_create(uint64_t *handle, int file_fd,
                                int64_t maximum_size, uint32_t protection,
                                uint32_t allocation_attributes)
{
    if (handle) *handle = 0;
    if (!handle || maximum_size < 0 || protection_to_posix(protection) < 0)
        return BEER_STATUS_INVALID_PARAMETER;

    int fd = -1;
    size_t size = 0;
    if (file_fd >= 0) {
        struct stat status;
        if (fstat(file_fd, &status) != 0 || status.st_size < 0)
            return BEER_STATUS_INVALID_HANDLE;
        uint64_t file_size = (uint64_t)status.st_size;
        uint64_t requested = maximum_size > 0 ? (uint64_t)maximum_size : file_size;
        if (!requested) return BEER_STATUS_MAPPED_FILE_SIZE_ZERO;
        if (requested > SIZE_MAX) return BEER_STATUS_SECTION_TOO_BIG;
        fd = dup(file_fd);
        if (fd < 0) return BEER_STATUS_NO_MEMORY;
        size = (size_t)requested;
        if (requested > file_size && ftruncate(fd, (off_t)requested) != 0) {
            close(fd);
            return BEER_STATUS_INVALID_PARAMETER;
        }
    } else {
        if (maximum_size <= 0 || (uint64_t)maximum_size > SIZE_MAX)
            return BEER_STATUS_INVALID_PARAMETER;
        fd = create_backing_fd();
        if (fd < 0) return BEER_STATUS_NO_MEMORY;
        size = (size_t)maximum_size;
        if (ftruncate(fd, (off_t)size) != 0) {
            close(fd);
            return BEER_STATUS_NO_MEMORY;
        }
    }

    pthread_mutex_lock(&g_section_mutex);
    int index = -1;
    for (int candidate = 1; candidate < BEER_NT_SECTION_CAPACITY; ++candidate) {
        if (!g_sections[candidate].used) { index = candidate; break; }
    }
    if (index >= 0) {
        g_sections[index] = (BeerNtSection){
            .used = 1, .fd = fd, .size = size,
            .protection = protection, .attributes = allocation_attributes
        };
        *handle = BEER_NT_SECTION_HANDLE_BASE + (uint64_t)index;
    }
    pthread_mutex_unlock(&g_section_mutex);
    if (index < 0) {
        close(fd);
        return BEER_STATUS_NO_MEMORY;
    }
    return BEER_STATUS_SUCCESS;
}

uint32_t beer_nt_section_map(uint64_t handle, void **base_address,
                             uint64_t zero_bits, int64_t *section_offset,
                             size_t *view_size, uint32_t inherit_disposition,
                             uint32_t allocation_type, uint32_t protection)
{
    (void)inherit_disposition;
    (void)allocation_type;
    if (!base_address || !view_size || zero_bits > 21)
        return BEER_STATUS_INVALID_PARAMETER;
    int prot = protection_to_posix(protection);
    if (prot < 0) return BEER_STATUS_INVALID_PARAMETER;

    long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) page = 4096;
    int64_t offset_value = section_offset ? *section_offset : 0;
    if (offset_value < 0 || (offset_value & (page - 1)) != 0)
        return BEER_STATUS_INVALID_PARAMETER;

    pthread_mutex_lock(&g_section_mutex);
    int index = section_index(handle);
    if (index < 0) {
        pthread_mutex_unlock(&g_section_mutex);
        return BEER_STATUS_INVALID_HANDLE;
    }
    BeerNtSection section = g_sections[index];
    if ((uint64_t)offset_value >= section.size) {
        pthread_mutex_unlock(&g_section_mutex);
        return BEER_STATUS_INVALID_PARAMETER;
    }
    size_t available = section.size - (size_t)offset_value;
    size_t length = *view_size ? *view_size : available;
    if (!length || length > available) {
        pthread_mutex_unlock(&g_section_mutex);
        return BEER_STATUS_INVALID_PARAMETER;
    }
    int view_index = -1;
    for (int candidate = 0; candidate < BEER_NT_VIEW_CAPACITY; ++candidate) {
        if (!g_views[candidate].used) { view_index = candidate; break; }
    }
    pthread_mutex_unlock(&g_section_mutex);
    if (view_index < 0) return BEER_STATUS_NO_MEMORY;

    int flags = MAP_SHARED;
    void *requested = *base_address;
#ifdef MAP_FIXED_NOREPLACE
    if (requested) flags |= MAP_FIXED_NOREPLACE;
#endif
    void *mapped = mmap(requested, length, prot, flags, section.fd, (off_t)offset_value);
    if (mapped == MAP_FAILED)
        return requested && (errno == EEXIST || errno == EINVAL)
            ? BEER_STATUS_CONFLICTING_ADDRESSES : BEER_STATUS_NO_MEMORY;
#ifndef MAP_FIXED_NOREPLACE
    if (requested && mapped != requested) {
        munmap(mapped, length);
        return BEER_STATUS_CONFLICTING_ADDRESSES;
    }
#endif

    pthread_mutex_lock(&g_section_mutex);
    g_views[view_index] = (BeerNtView){.used = 1, .base = mapped, .size = length};
    pthread_mutex_unlock(&g_section_mutex);
    *base_address = mapped;
    *view_size = length;
    return BEER_STATUS_SUCCESS;
}

uint32_t beer_nt_section_unmap(void *base_address)
{
    if (!base_address) return BEER_STATUS_INVALID_PARAMETER;
    pthread_mutex_lock(&g_section_mutex);
    int index = -1;
    size_t size = 0;
    for (int candidate = 0; candidate < BEER_NT_VIEW_CAPACITY; ++candidate) {
        if (g_views[candidate].used && g_views[candidate].base == base_address) {
            index = candidate;
            size = g_views[candidate].size;
            g_views[candidate].used = 0;
            break;
        }
    }
    pthread_mutex_unlock(&g_section_mutex);
    if (index < 0) return BEER_STATUS_INVALID_PARAMETER;
    return munmap(base_address, size) == 0 ? BEER_STATUS_SUCCESS
                                           : BEER_STATUS_INVALID_PARAMETER;
}

int beer_nt_section_close(uint64_t handle)
{
    pthread_mutex_lock(&g_section_mutex);
    int index = section_index(handle);
    int fd = index >= 0 ? g_sections[index].fd : -1;
    if (index >= 0) memset(&g_sections[index], 0, sizeof(g_sections[index]));
    pthread_mutex_unlock(&g_section_mutex);
    if (fd < 0) return 0;
    close(fd);
    return 1;
}

void beer_nt_section_reset(void)
{
    pthread_mutex_lock(&g_section_mutex);
    for (int index = 0; index < BEER_NT_VIEW_CAPACITY; ++index) {
        if (g_views[index].used) munmap(g_views[index].base, g_views[index].size);
        memset(&g_views[index], 0, sizeof(g_views[index]));
    }
    for (int index = 1; index < BEER_NT_SECTION_CAPACITY; ++index) {
        if (g_sections[index].used) close(g_sections[index].fd);
        memset(&g_sections[index], 0, sizeof(g_sections[index]));
    }
    pthread_mutex_unlock(&g_section_mutex);
}
