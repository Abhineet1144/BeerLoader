#ifndef BEER_WIN32_RESOURCE_H
#define BEER_WIN32_RESOURCE_H

#include <stddef.h>
#include <stdint.h>

/* PE resource access on images that are mapped at their load address (the guest executable and its
 * PE DLLs). A resource handle (HRSRC) is the address of the IMAGE_RESOURCE_DATA_ENTRY. */

/* Module handle used when the guest passes NULL (the executable's image base). */
void beer_resource_set_default_module(uint64_t image_base);

/* Looks a resource up by type and name (each either a MAKEINTRESOURCE value or a UTF-16 string).
 * language 0 / 0xFFFF picks the first language. Returns the data entry or 0. */
uint64_t beer_resource_find(uint64_t module, uint64_t name, uint64_t type, uint16_t language);
/* Pointer to the resource bytes (valid for the life of the mapped image) and their size. */
const uint8_t *beer_resource_data(uint64_t module, uint64_t resource, uint32_t *size);

uint64_t __attribute__((ms_abi)) beer_FindResourceW(uint64_t module, uint64_t name, uint64_t type);
uint64_t __attribute__((ms_abi)) beer_FindResourceExW(uint64_t module, uint64_t type, uint64_t name,
                                                       uint16_t language);
uint64_t __attribute__((ms_abi)) beer_FindResourceA(uint64_t module, uint64_t name, uint64_t type);
uint64_t __attribute__((ms_abi)) beer_FindResourceExA(uint64_t module, uint64_t type, uint64_t name,
                                                       uint16_t language);
uint64_t __attribute__((ms_abi)) beer_LoadResource(uint64_t module, uint64_t resource);
uint64_t __attribute__((ms_abi)) beer_LockResource(uint64_t data);
uint64_t __attribute__((ms_abi)) beer_SizeofResource(uint64_t module, uint64_t resource);
uint64_t __attribute__((ms_abi)) beer_FreeResource(uint64_t data);

#endif
