#ifndef BEER_WIN32_NT_SECTION_H
#define BEER_WIN32_NT_SECTION_H

#include <stddef.h>
#include <stdint.h>

#define BEER_STATUS_SUCCESS              UINT32_C(0x00000000)
#define BEER_STATUS_INVALID_HANDLE       UINT32_C(0xc0000008)
#define BEER_STATUS_INVALID_PARAMETER    UINT32_C(0xc000000d)
#define BEER_STATUS_NO_MEMORY            UINT32_C(0xc0000017)
#define BEER_STATUS_CONFLICTING_ADDRESSES UINT32_C(0xc0000018)
#define BEER_STATUS_SECTION_TOO_BIG      UINT32_C(0xc0000040)
#define BEER_STATUS_MAPPED_FILE_SIZE_ZERO UINT32_C(0xc000011e)

/* Minimal NT section-object implementation. file_fd is borrowed and duplicated;
 * pass -1 for a page-file-backed section. Handles occupy their own namespace. */
uint32_t beer_nt_section_create(uint64_t *handle, int file_fd,
                                int64_t maximum_size, uint32_t protection,
                                uint32_t allocation_attributes);
uint32_t beer_nt_section_map(uint64_t handle, void **base_address,
                             uint64_t zero_bits, int64_t *section_offset,
                             size_t *view_size, uint32_t inherit_disposition,
                             uint32_t allocation_type, uint32_t protection);
uint32_t beer_nt_section_unmap(void *base_address);
int beer_nt_section_close(uint64_t handle);
void beer_nt_section_reset(void);

#endif
